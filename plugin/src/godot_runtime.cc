/*
 * Copyright 2026 Joel Winarske
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "godot_runtime.h"

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <vector>

#include <ihs/platform_view.h>

#include "godot_view.h"
#include "log.h"
#include "view_params.h"

namespace ihs_godot {

namespace {

constexpr auto kIdlePeriod = std::chrono::milliseconds(100);
// At exit: how long the engine gets to finish its current iteration, and then
// to shut down, before the process leaves without it.
constexpr auto kExitBusyTimeout = std::chrono::seconds(2);
// How long a view's dispose waits for the engine to finish its iteration.
constexpr auto kDetachTimeout = std::chrono::seconds(5);
constexpr auto kExitTimeout = std::chrono::seconds(10);
constexpr auto kDefaultPeriod = std::chrono::microseconds(16667);

// A module built before loads_in_progress was appended reports no loads.
bool LoadsInProgress(const IhsGodotApiV1& api) {
  constexpr size_t kNeeded = offsetof(IhsGodotApiV1, loads_in_progress) +
                             sizeof(IhsGodotApiV1::loads_in_progress);
  return api.struct_size >= kNeeded && api.loads_in_progress != nullptr &&
         api.loads_in_progress() != 0;
}

// The part of Dart's dart_native_api.h that Emit uses: a Dart_CObject is a
// type tag and a union, and Dart_PostCObject copies it into a message. The
// layout has been stable since Dart 2; the union is padded to its full size.
struct DartCObject {
  int32_t type;
  union {
    int32_t as_int32;
    const char* as_string;
    struct {
      intptr_t length;
      DartCObject** values;
    } as_array;
    unsigned char size[40];
  } value;
};
constexpr int32_t kDartCObjectInt32 = 2;
constexpr int32_t kDartCObjectString = 5;
constexpr int32_t kDartCObjectArray = 6;
using DartPostCObject = bool (*)(int64_t port, DartCObject* message);

std::string DirOf(const std::string& path) {
  const size_t slash = path.rfind('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

}  // namespace

GodotRuntime& GodotRuntime::Get() {
  // Leaked on purpose: a joinable std::thread in a static destroyed at exit
  // terminates the process, and the engine must not be torn down from an
  // atexit handler on whatever thread runs it.
  static GodotRuntime* runtime = new GodotRuntime();
  return *runtime;
}

GodotRuntime::~GodotRuntime() = default;

bool GodotRuntime::Attach(const std::shared_ptr<GodotView>& view) {
  {
    std::lock_guard<std::mutex> lock(views_mutex_);
    const int32_t active = active_id_.load();
    if (active != -1) {
      auto it = views_.find(active);
      auto incumbent = it != views_.end() ? it->second.lock() : nullptr;
      if (incumbent && !incumbent->detached()) {
        LOG_E("view %d: view %d already shows the game; Godot has one window",
              view->id(), active);
        return false;
      }
    }
    views_[view->id()] = view;
    active_id_ = view->id();
  }

  Post([this, view] {
    if (active_ && active_ != view) {
      active_->Teardown();
    }
    active_ = view;
    if (instance_ == nullptr && !engine_dead_ && !StartEngine(view)) {
      engine_dead_ = true;
    }
    const IhsGodotApiV1* api = api_.load();
    if (engine_dead_ || api == nullptr) {
      Emit(view->id(), IHS_GODOT_VIEW_EVENT_ERROR, "Godot is not running");
      return;
    }
    api->notify(instance_, IHS_GODOT_FOCUS_IN);
    view->OnEngineReady(api);
  });

  std::lock_guard<std::mutex> lock(mutex_);
  if (!thread_.joinable()) {
    thread_ = std::thread([this] { ThreadMain(); });
  }
  return true;
}

void GodotRuntime::Detach(int32_t view_id) {
  std::shared_ptr<GodotView> view;
  {
    std::lock_guard<std::mutex> lock(views_mutex_);
    auto it = views_.find(view_id);
    if (it != views_.end()) {
      view = it->second.lock();
      views_.erase(it);
    }
    if (active_id_ == view_id) {
      active_id_ = -1;
    }
  }
  if (!view) {
    return;
  }
  auto done = std::make_shared<std::promise<void>>();
  std::future<void> detached = done->get_future();
  Post([this, view, done] {
    view->Teardown();
    if (active_ == view) {
      active_.reset();
      if (const IhsGodotApiV1* api = api_.load()) {
        // Keep the game; it pauses until a view attaches again.
        api->set_visible(0);
        api->notify(instance_, IHS_GODOT_FOCUS_OUT);
      }
    }
    done->set_value();
  });
  // Wait for the engine to finish the iteration in flight: with no view it
  // then pauses, so nothing it does lands after the exit handler registered
  // below (see StopAtExit). Bounded, for an iteration loading a scene.
  if (thread_.get_id() != std::this_thread::get_id() &&
      detached.wait_for(kDetachTimeout) != std::future_status::ready) {
    LOG_W("view %d: Godot is busy; detached without waiting for it",
          view_id);
  }
  StopAtExit();
}

void GodotRuntime::Post(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.push_back(std::move(task));
  }
  cv_.notify_one();
}

std::shared_ptr<GodotView> GodotRuntime::Find(int32_t view_id) {
  std::lock_guard<std::mutex> lock(views_mutex_);
  auto it = views_.find(view_id);
  return it == views_.end() ? nullptr : it->second.lock();
}

double GodotRuntime::ActiveDevicePixelRatio() {
  auto view = Find(active_id_.load());
  return view ? view->device_pixel_ratio() : 1.0;
}

void GodotRuntime::SetDartPort(int64_t port, void* post_cobject) {
  std::lock_guard<std::mutex> lock(dart_mutex_);
  dart_port_ = port;
  dart_post_ = post_cobject;
}

void GodotRuntime::Emit(int32_t view_id,
                        IhsGodotViewEvent event,
                        const std::string& text) {
  {
    std::lock_guard<std::mutex> lock(dart_mutex_);
    if (dart_post_ != nullptr) {
      DartCObject id{kDartCObjectInt32, {}};
      id.value.as_int32 = view_id;
      DartCObject ev{kDartCObjectInt32, {}};
      ev.value.as_int32 = static_cast<int32_t>(event);
      DartCObject payload{kDartCObjectString, {}};
      payload.value.as_string = text.c_str();
      DartCObject* items[] = {&id, &ev, &payload};
      DartCObject list{kDartCObjectArray, {}};
      list.value.as_array.length = 3;
      list.value.as_array.values = items;
      // False once the port's isolate has gone; the event is dropped.
      reinterpret_cast<DartPostCObject>(dart_post_)(dart_port_, &list);
      return;
    }
  }
  IhsGodotViewListener listener = listener_.load();
  if (listener == nullptr) {
    return;
  }
  // The receiver frees it (ihs_godot_view_free_string): a Dart listener runs
  // asynchronously, after this returns.
  char* payload = strdup(text.c_str());
  listener(view_id, static_cast<int32_t>(event), payload);
}

void GodotRuntime::SetPaused(bool paused) {
  Post([this, paused] {
    paused_ = paused;
    const IhsGodotApiV1* api = api_.load();
    if (api == nullptr || instance_ == nullptr) {
      return;
    }
    api->notify(instance_, paused ? IHS_GODOT_PAUSE : IHS_GODOT_RESUME);
    api->notify(instance_, paused ? IHS_GODOT_FOCUS_OUT : IHS_GODOT_FOCUS_IN);
  });
}

void GodotRuntime::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
    thread_.join();
  }
}

// The engine outlives every view (a hot restart re-attaches to it), so a view's
// dispose does not stop it, and nothing else does before the host process
// exits. Left running, its threads keep calling into libraries that exit()
// is tearing down under them: on Adreno the shader compiler's LLVM asserts
// mid-compile, and Godot's own statics are destroyed under a running frame.
//
// exit() runs its handlers newest first, and anything Godot registers after
// this handler (a library it loads, a function-local static it first
// touches) is destroyed before the handler gets to stop it. So the handler is
// registered once the engine has started, and again at every dispose after
// the engine has finished its iteration and paused for want of a view; a
// paused engine registers nothing further.
void GodotRuntime::StopAtExit() {
#if defined(__GLIBC__)
  on_exit(&GodotRuntime::OnExit, nullptr);
#else
  std::atexit([] { OnExit(EXIT_SUCCESS, nullptr); });
#endif
}

void GodotRuntime::OnExit(int status, void* /*arg*/) {
  GodotRuntime& self = Get();
  std::unique_lock<std::mutex> lock(self.mutex_);
  if (!self.thread_.joinable() ||
      self.thread_.get_id() == std::this_thread::get_id()) {
    return;  // never started, already joined, or exit() from the engine thread
  }
  self.stop_ = true;
  self.exiting_ = true;
  self.cv_.notify_all();
  // An iteration that loads a scene or compiles shaders can run for minutes
  // and cannot be interrupted. Leave without the destructors rather than wait
  // it out; _exit() takes the engine's threads down with the process.
  const char* why = nullptr;
  if (!self.cv_.wait_for(lock, kExitBusyTimeout,
                         [&self] { return self.stopping_; })) {
    why = "is busy (loading a scene?)";
  } else if (!self.cv_.wait_for(lock, kExitTimeout,
                                [&self] { return self.stopped_; })) {
    why = "did not finish shutting down";
  } else if (self.abandoned_) {
    why = "is loading in the background";
  }
  if (why != nullptr) {
    LOG_W("Godot %s; exiting without shutting it down", why);
    std::fflush(nullptr);
    ::_exit(status);
  }
  lock.unlock();
  self.thread_.join();
  LOG_I("Godot stopped at exit");
}

bool GodotRuntime::RunTasks(std::unique_lock<std::mutex>& lock) {
  if (tasks_.empty()) {
    return false;
  }
  std::deque<std::function<void()>> tasks;
  tasks.swap(tasks_);
  lock.unlock();
  for (auto& task : tasks) {
    task();
  }
  lock.lock();
  return true;
}

void GodotRuntime::ThreadMain() {
  pthread_setname_np(pthread_self(), "godot");
  std::unique_lock<std::mutex> lock(mutex_);
  auto pending = [this] { return stop_ || !tasks_.empty(); };
  while (!stop_) {
    RunTasks(lock);
    if (stop_) {
      break;
    }
    // No view: the game pauses until one attaches (or the process exits).
    if (instance_ == nullptr || engine_dead_ || !active_) {
      cv_.wait(lock, pending);
      continue;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < next_frame_) {
      cv_.wait_until(lock, next_frame_, pending);
      continue;
    }
    lock.unlock();
    Iterate();
    Pace();
    lock.lock();
  }
  stopping_ = true;
  const bool exiting = exiting_;
  lock.unlock();
  cv_.notify_all();

  // Shutdown, on the thread Godot belongs to.
  if (active_) {
    active_->Teardown();
    active_.reset();
  }
  bool abandoned = false;
  if (const IhsGodotApiV1* api = api_.exchange(nullptr);
      api != nullptr && instance_ != nullptr) {
    // Godot's cleanup frees the background load tasks under a worker that may
    // still be writing to one. A process that is exiting gives up only the
    // cleanup's saves (the pipeline cache) by leaving the engine as it is;
    // OnExit then _exit()s.
    if (exiting && LoadsInProgress(*api)) {
      abandoned = true;
    } else {
      api->destroy_instance(instance_);
    }
  }
  instance_ = nullptr;
  engine_dead_ = true;

  lock.lock();
  stopped_ = true;
  abandoned_ = abandoned;
  lock.unlock();
  cv_.notify_all();
}

void GodotRuntime::Pace() {
  auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      kDefaultPeriod);
  if (!active_ || active_->suspended() || paused_) {
    period = kIdlePeriod;
  } else if (active_->params().max_fps > 0) {
    period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::nanoseconds(1000000000LL / active_->params().max_fps));
  } else if (const uint32_t refresh = active_->refresh_ns(); refresh != 0) {
    period = std::chrono::nanoseconds(refresh);
  }
  const auto now = std::chrono::steady_clock::now();
  next_frame_ += period;
  // A long frame (a scene load) skips ahead instead of chasing the backlog.
  if (next_frame_ < now) {
    next_frame_ = now;
  }
}

void GodotRuntime::Iterate() {
  const IhsGodotApiV1* api = api_.load();
  std::shared_ptr<GodotView> view = active_;
  const uint32_t target = view ? view->BeginFrame() : IHS_GODOT_NO_TARGET;
  api->set_next_target(target);

  IhsGodotFrameInfo info{};
  info.struct_size = sizeof(info);
  info.target_id = IHS_GODOT_NO_TARGET;
  info.acquire_fence_fd = -1;
  const int quit = api->iteration(instance_, &info);

  if (view) {
    view->EndFrame(info);
  } else if (info.acquire_fence_fd >= 0) {
    ::close(info.acquire_fence_fd);
  }

  if (quit != 0) {
    LOG_I("the game quit; Godot cannot be started again in this process");
    engine_dead_ = true;
    const int32_t id = view ? view->id() : active_id_.load();
    if (view) {
      view->Teardown();
    }
    api_.store(nullptr);
    api->destroy_instance(instance_);
    instance_ = nullptr;
    Emit(id, IHS_GODOT_VIEW_EVENT_QUIT, "");
  }
}

void* GodotRuntime::LoadLibGodot(const std::string& requested) {
  std::vector<std::string> candidates;
  if (!requested.empty()) {
    candidates.push_back(requested);
  } else {
    // Beside this plugin, then the bundle's lib/, then the loader's path.
    Dl_info self{};
    if (dladdr(reinterpret_cast<void*>(&GodotRuntime::Get), &self) != 0 &&
        self.dli_fname != nullptr) {
      const std::string dir = DirOf(self.dli_fname);
      if (!dir.empty()) {
        candidates.push_back(dir + "/libgodot.so");
      }
    }
    if (ihs_pv_assets_path != nullptr) {
      if (const char* assets = ihs_pv_assets_path()) {
        candidates.push_back(DirOf(assets) + "/lib/libgodot.so");
        candidates.push_back(DirOf(DirOf(assets)) + "/lib/libgodot.so");
      }
    }
    candidates.emplace_back("libgodot.so");
  }
  for (const std::string& path : candidates) {
    if (void* lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL)) {
      LOG_I("loaded %s", path.c_str());
      return lib;
    }
    LOG_D("dlopen(%s): %s", path.c_str(), dlerror());
  }
  LOG_E("libgodot.so not found (set creation param 'libgodot' or "
        "IHS_GODOT_LIBRARY): %s",
        dlerror());
  return nullptr;
}

bool GodotRuntime::StartEngine(const std::shared_ptr<GodotView>& view) {
  libgodot_ = LoadLibGodot(view->params().libgodot);
  if (libgodot_ == nullptr) {
    Emit(view->id(), IHS_GODOT_VIEW_EVENT_ERROR, "libgodot.so not found");
    return false;
  }
  auto get_api = reinterpret_cast<IhsGodotBridgeGetApiFn>(
      dlsym(libgodot_, IHS_GODOT_BRIDGE_GET_API_SYMBOL));
  const IhsGodotApiV1* api =
      get_api != nullptr ? get_api(IHS_GODOT_BRIDGE_VERSION) : nullptr;
  if (api == nullptr || api->struct_size < sizeof(IhsGodotApiV1)) {
    LOG_E("libgodot has no compatible ihs bridge (built without the ihs "
          "module?)");
    Emit(view->id(), IHS_GODOT_VIEW_EVENT_ERROR,
         "libgodot was built without the ihs module");
    return false;
  }

  IhsGodotConfig config{};
  config.struct_size = sizeof(config);
  config.render_device = view->shell_render_device();
  config.has_device_uuid = view->shell_device_uuid(config.device_uuid) ? 1 : 0;
  IhsGodotHostCallbacks callbacks{};
  callbacks.struct_size = sizeof(callbacks);
  callbacks.user_data = this;
  callbacks.log = &GodotRuntime::OnLog;
  callbacks.message = &GodotRuntime::OnMessage;
  callbacks.mouse_mode = &GodotRuntime::OnMouseMode;
  if (api->configure(&config, &callbacks) != IHS_GODOT_OK) {
    return false;
  }

  uint32_t w = 0, h = 0;
  view->size(w, h);
  if (w != 0 && h != 0) {
    api->set_window_size(w, h, static_cast<float>(view->device_pixel_ratio()));
  }
  // Input queued from now on reaches the engine.
  api_.store(api);

  std::vector<std::string> args = BuildGodotArgs(view->params());
  std::string line;
  std::vector<char*> argv;
  for (std::string& a : args) {
    argv.push_back(a.data());
    line += " " + a;
  }
  argv.push_back(nullptr);
  LOG_I("starting Godot:%s", line.c_str());

  instance_ = api->create_instance(static_cast<int>(args.size()), argv.data());
  if (instance_ == nullptr) {
    LOG_E("libgodot_create_godot_instance failed (see Godot's output above)");
    api_.store(nullptr);
    Emit(view->id(), IHS_GODOT_VIEW_EVENT_ERROR, "Godot failed to set up");
    return false;
  }
  if (api->start(instance_) != IHS_GODOT_OK) {
    LOG_E("GodotInstance::start failed");
    api_.store(nullptr);
    // Godot cannot be set up again in this process; leave it be.
    instance_ = nullptr;
    Emit(view->id(), IHS_GODOT_VIEW_EVENT_ERROR, "Godot failed to start");
    return false;
  }
  next_frame_ = std::chrono::steady_clock::now();
  StopAtExit();
  return true;
}

void GodotRuntime::OnLog(void* /*user_data*/, int level, const char* text) {
  Log(static_cast<LogLevel>(level < 0 ? 0 : level > 3 ? 3 : level),
      "godot: %s", text);
}

void GodotRuntime::OnMessage(void* user_data, const char* text) {
  auto* self = static_cast<GodotRuntime*>(user_data);
  self->Emit(self->active_id_.load(), IHS_GODOT_VIEW_EVENT_MESSAGE,
             text ? text : "");
}

void GodotRuntime::OnMouseMode(void* user_data, uint32_t mode) {
  auto* self = static_cast<GodotRuntime*>(user_data);
  self->Emit(self->active_id_.load(), IHS_GODOT_VIEW_EVENT_MOUSE_MODE,
             std::to_string(mode));
}

}  // namespace ihs_godot

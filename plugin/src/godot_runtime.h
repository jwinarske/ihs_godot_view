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

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <ihs_godot/bridge.h>

#include "../include/ihs_godot_view/ihs_godot_view.h"

namespace ihs_godot {

class GodotView;

// The one Godot engine of the process, and the thread it lives on.
//
// LibGodot allows one instance per process and cannot set Godot up a second
// time, so the engine is created for the first view and outlives every view:
// a view that goes away (dispose, hot restart) detaches, the engine keeps its
// state and stops drawing, and the next view attaches to it. Only one view is
// attached at a time -- Godot has one root window to show.
//
// Godot binds its main thread to whoever sets it up, so everything Godot is
// done on the engine thread: loading libgodot, create/start/iteration, target
// imports. Other threads reach it with Post().
class GodotRuntime {
 public:
  static GodotRuntime& Get();

  // --- platform thread ---
  // Returns false when another view is attached (one game, one window).
  bool Attach(const std::shared_ptr<GodotView>& view);
  void Detach(int32_t view_id);

  // --- any thread ---
  void Post(std::function<void()> task);
  std::shared_ptr<GodotView> Find(int32_t view_id);
  // The bridge, once libgodot is loaded (input entry points are thread-safe).
  [[nodiscard]] const IhsGodotApiV1* api() const { return api_.load(); }
  uint32_t NewTargetId() { return next_target_id_.fetch_add(1); }
  void SetListener(IhsGodotViewListener listener) { listener_ = listener; }
  void Emit(int32_t view_id, IhsGodotViewEvent event, const std::string& text);
  [[nodiscard]] int32_t active_view_id() const { return active_id_.load(); }
  double ActiveDevicePixelRatio();
  void SetPaused(bool paused);
  void Shutdown();
  // Stop the engine before the process's exit handlers run (see .cc).
  void StopAtExit();

 private:
  GodotRuntime() = default;
  ~GodotRuntime();

  static void OnExit(int status, void* arg);
  void ThreadMain();
  bool RunTasks(std::unique_lock<std::mutex>& lock);
  bool StartEngine(const std::shared_ptr<GodotView>& view);
  void* LoadLibGodot(const std::string& requested);
  void Iterate();
  void Pace();

  static void OnLog(void* user_data, int level, const char* text);
  static void OnMessage(void* user_data, const char* text);
  static void OnMouseMode(void* user_data, uint32_t mode);

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  std::thread thread_;
  bool stop_ = false;
  bool stopping_ = false;  // the engine thread left its loop to shut down
  bool stopped_ = false;   // and finished

  std::mutex views_mutex_;
  std::map<int32_t, std::weak_ptr<GodotView>> views_;
  std::atomic<int32_t> active_id_{-1};

  std::atomic<const IhsGodotApiV1*> api_{nullptr};
  std::atomic<uint32_t> next_target_id_{1};
  std::atomic<IhsGodotViewListener> listener_{nullptr};

  // Engine thread only.
  void* libgodot_ = nullptr;
  void* instance_ = nullptr;
  bool engine_dead_ = false;  // quit, or failed to start
  bool paused_ = false;
  std::shared_ptr<GodotView> active_;
  std::chrono::steady_clock::time_point next_frame_{};
};

}  // namespace ihs_godot

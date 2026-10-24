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

// Unit tests for the parts of the producer that need neither a GPU nor a
// running shell: creation-param decoding, touch decoding, the surface
// selection policy, and frame-latency matching.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include <ihs/platform_view.h>
#include <ihs_godot/bridge.h>

#include "../src/codec.h"
#include "../src/frame_latency.h"
#include "../src/input_map.h"
#include "../src/surface_policy.h"
#include "../src/view_params.h"

using namespace ihs_godot;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    ++g_checks;                                                        \
    if (!(cond)) {                                                     \
      ++g_failures;                                                    \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__,      \
                   __LINE__, #cond);                                   \
    }                                                                  \
  } while (0)

std::vector<std::pair<const char*, std::function<void()>>>& Tests() {
  static std::vector<std::pair<const char*, std::function<void()>>> tests;
  return tests;
}

struct Register {
  Register(const char* name, std::function<void()> fn) {
    Tests().emplace_back(name, std::move(fn));
  }
};

#define TEST(name)                         \
  void name();                             \
  Register name##_reg(#name, name);        \
  void name()

CodecValue Str(const std::string& s) {
  CodecValue v;
  v.type = CodecValue::Type::kString;
  v.s = s;
  return v;
}

CodecValue Dbl(double d) {
  CodecValue v;
  v.type = CodecValue::Type::kDouble;
  v.d = d;
  return v;
}

CodecValue Int(int64_t i) {
  CodecValue v;
  v.type = CodecValue::Type::kInt;
  v.i = i;
  return v;
}

constexpr uint32_t Fourcc(char a, char b, char c, char d) {
  return uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) |
         (uint32_t(d) << 24);
}
constexpr uint32_t kAR24 = Fourcc('A', 'R', '2', '4');
constexpr uint32_t kXR24 = Fourcc('X', 'R', '2', '4');
constexpr uint32_t kAB24 = Fourcc('A', 'B', '2', '4');
constexpr uint32_t kXB24 = Fourcc('X', 'B', '2', '4');
constexpr uint64_t kTiled = 0x0100000000000002ULL;  // I915_X_TILED

// --- codec -----------------------------------------------------------------

TEST(DecodesBytesProducedByFlutter) {
  // StandardMessageCodec().encodeMessage({'a': 1, 'b': 'hi'}) in Dart.
  const uint8_t bytes[] = {13, 2, 7, 1, 'a', 3, 1, 0, 0, 0, 7, 1, 'b', 7, 2,
                           'h', 'i'};
  auto v = DecodeStandardMessage(bytes, sizeof(bytes));
  CHECK(v.has_value());
  CHECK(v->type == CodecValue::Type::kMap);
  CHECK(v->Find("a") && v->Find("a")->AsInt() == 1);
  CHECK(v->Find("b") && v->Find("b")->AsString() == std::string("hi"));
  CHECK(v->Find("c") == nullptr);
}

TEST(DoubleIsAlignedFromMessageStart) {
  // {'d': 1.5}: type 13, size 1, key "d" (3 bytes), type 6 at offset 5, then
  // padding to 8, then the double.
  CodecValue map;
  map.type = CodecValue::Type::kMap;
  map.map.emplace_back(Str("d"), Dbl(1.5));
  std::vector<uint8_t> out;
  EncodeStandardMessage(map, out);
  CHECK(out.size() == 16);
  CHECK(out[5] == 6);
  auto v = DecodeStandardMessage(out.data(), out.size());
  CHECK(v && v->Find("d") && v->Find("d")->AsDouble() == 1.5);
}

TEST(RejectsTruncatedAndUnknown) {
  const uint8_t truncated[] = {7, 5, 'a', 'b'};
  CHECK(!DecodeStandardMessage(truncated, sizeof(truncated)).has_value());
  const uint8_t unknown[] = {99};
  CHECK(!DecodeStandardMessage(unknown, sizeof(unknown)).has_value());
  CHECK(!DecodeStandardMessage(nullptr, 0).has_value());
  // A list claiming more entries than bytes left.
  const uint8_t list[] = {12, 200};
  CHECK(!DecodeStandardMessage(list, sizeof(list)).has_value());
}

TEST(LongStringSizePrefix) {
  CodecValue s = Str(std::string(300, 'x'));
  std::vector<uint8_t> out;
  EncodeStandardMessage(s, out);
  CHECK(out[1] == 254);
  auto v = DecodeStandardMessage(out.data(), out.size());
  CHECK(v && v->AsString() && v->AsString()->size() == 300);
}

// --- params ----------------------------------------------------------------

TEST(ParsesCreationParams) {
  CodecValue map;
  map.type = CodecValue::Type::kMap;
  map.map.emplace_back(Str("main_pack"), Str("games/tps.pck"));
  map.map.emplace_back(Str("surface"), Str("vk_image"));
  map.map.emplace_back(Str("device_pixel_ratio"), Dbl(2.0));
  map.map.emplace_back(Str("buffers"), Int(9));
  CodecValue args;
  args.type = CodecValue::Type::kList;
  args.list.push_back(Str("--verbose"));
  map.map.emplace_back(Str("args"), args);
  std::vector<uint8_t> out;
  EncodeStandardMessage(map, out);

  ViewParams p =
      ParseViewParams(out.data(), out.size(), "/opt/app/data/flutter_assets");
  CHECK(p.main_pack == "/opt/app/data/flutter_assets/games/tps.pck");
  CHECK(p.surface == SurfacePref::kVkImage);
  CHECK(p.device_pixel_ratio == 2.0);
  CHECK(p.buffers == 4);  // clamped
  CHECK(p.extra_args.size() == 1 && p.extra_args[0] == "--verbose");

  auto argv = BuildGodotArgs(p);
  CHECK(argv.size() == 4);
  CHECK(argv[1] == "--main-pack");
  CHECK(argv[3] == "--verbose");
}

TEST(AbsolutePathsAndGarbage) {
  CodecValue map;
  map.type = CodecValue::Type::kMap;
  map.map.emplace_back(Str("path"), Str("/srv/game"));
  std::vector<uint8_t> out;
  EncodeStandardMessage(map, out);
  ViewParams p = ParseViewParams(out.data(), out.size(), "/assets");
  CHECK(p.project_path == "/srv/game");

  const uint8_t junk[] = {1, 2, 3};
  ViewParams d = ParseViewParams(junk, sizeof(junk), nullptr);
  CHECK(d.surface == SurfacePref::kAuto && d.main_pack.empty());
}

// --- touch -----------------------------------------------------------------

std::vector<double> Contact(double id, double x, double y) {
  return {id, 1.0, 0, 0, 0, 0, 0, x, y};
}

TEST(SingleContactAsTheShellSendsIt) {
  auto c = Contact(0, 10, 20);
  auto ev = DecodeAndroidTouch(0 /*DOWN*/, 1, c.size(), c.data(), 2.0);
  CHECK(ev.size() == 1);
  CHECK(ev[0].phase == IHS_GODOT_TOUCH_DOWN);
  CHECK(ev[0].x == 20.0f && ev[0].y == 40.0f);
  CHECK(ev[0].index == 0);
  // point_count says 2 but only one contact's coordinates came across.
  ev = DecodeAndroidTouch(2 /*MOVE*/, 2, c.size(), c.data(), 1.0);
  CHECK(ev.size() == 1 && ev[0].phase == IHS_GODOT_TOUCH_MOVE);
}

TEST(MultiContactWithIds) {
  std::vector<double> data = Contact(3, 1, 1);
  auto second = Contact(7, 5, 5);
  data.insert(data.end(), second.begin(), second.end());
  // POINTER_DOWN for index 1.
  auto ev = DecodeAndroidTouch(5 | (1 << 8), 2, data.size(), data.data(), 1.0);
  CHECK(ev.size() == 1);
  CHECK(ev[0].index == 7 && ev[0].phase == IHS_GODOT_TOUCH_DOWN);
  ev = DecodeAndroidTouch(2, 2, data.size(), data.data(), 1.0);
  CHECK(ev.size() == 2 && ev[0].index == 3 && ev[1].index == 7);
  ev = DecodeAndroidTouch(3 /*CANCEL*/, 2, data.size(), data.data(), 1.0);
  CHECK(ev.size() == 2 && ev[1].phase == IHS_GODOT_TOUCH_CANCEL);
}

TEST(DuplicateIdsFallBackToPosition) {
  // Plain AndroidView packets carry orientation (0) in slot 0.
  std::vector<double> data = Contact(0, 1, 1);
  auto second = Contact(0, 5, 5);
  data.insert(data.end(), second.begin(), second.end());
  auto ev = DecodeAndroidTouch(2, 2, data.size(), data.data(), 1.0);
  CHECK(ev.size() == 2 && ev[0].index == 0 && ev[1].index == 1);
}

TEST(TouchSlotsReuseLowestIndex) {
  TouchSlots slots;
  CHECK(slots.Acquire(100) == 0);
  CHECK(slots.Acquire(101) == 1);
  CHECK(slots.Acquire(100) == 0);  // idempotent
  slots.Release(100);
  CHECK(slots.Find(100) == -1);
  CHECK(slots.Acquire(102) == 0);
}

TEST(ButtonMapping) {
  CHECK(FlutterButtonsToGodot(0x1) == IHS_GODOT_BUTTON_LEFT);
  CHECK(FlutterButtonsToGodot(0x2) == IHS_GODOT_BUTTON_RIGHT);
  CHECK(FlutterButtonsToGodot(0x18) ==
        (IHS_GODOT_BUTTON_BACK | IHS_GODOT_BUTTON_FORWARD));
  auto d = DiffButtons(IHS_GODOT_BUTTON_LEFT, IHS_GODOT_BUTTON_RIGHT);
  CHECK(d.size() == 2);
  CHECK(d[0].button == IHS_GODOT_BUTTON_LEFT && !d[0].pressed);
  CHECK(d[1].button == IHS_GODOT_BUTTON_RIGHT && d[1].pressed);
}

// --- surface policy --------------------------------------------------------

SurfaceFacts VulkanShell() {
  SurfaceFacts f;
  f.shell_kinds = IHS_PV_KIND_TEXTURE_DMABUF_IMPORT | IHS_PV_KIND_SOFTWARE_SHM |
                  IHS_PV_KIND_TEXTURE_VK_IMAGE;
  f.shell_formats = {{kAR24, kTiled}, {kXR24, kTiled}, {kAR24, 0}, {kXR24, 0}};
  f.godot_target_types = (1u << IHS_GODOT_TARGET_DMABUF) |
                         (1u << IHS_GODOT_TARGET_VK_OPAQUE_FD) |
                         (1u << IHS_GODOT_TARGET_CPU);
  f.godot_vulkan = true;
  f.godot_formats = {{kXR24, 0}, {kAR24, 0}, {kXR24, kTiled}, {kAR24, kTiled}};
  f.same_vk_device = true;
  f.shell_vk_export = true;
  return f;
}

TEST(AutoPrefersDmabufInShellOrderOpaqueFirst) {
  SurfacePlan p = PlanSurface(VulkanShell(), SurfacePref::kAuto);
  CHECK(p.path == SurfacePath::kDmabuf);
  CHECK(p.negotiate);
  CHECK((p.kinds & IHS_PV_KIND_SOFTWARE_SHM) != 0);
  CHECK(p.preferred_kind == 0);
  // Shell order (tiled before linear), X before A.
  CHECK(p.formats.size() == 4);
  CHECK(p.formats[0] == (FourccModifier{kXR24, kTiled}));
  CHECK(p.formats[1] == (FourccModifier{kXR24, 0}));
  CHECK(p.formats[2] == (FourccModifier{kAR24, kTiled}));
}

TEST(NoCommonModifierFallsToVkImage) {
  SurfaceFacts f = VulkanShell();
  f.godot_formats = {{kAB24, 0x123}};
  SurfacePlan p = PlanSurface(f, SurfacePref::kAuto);
  CHECK(p.path == SurfacePath::kVkImage);
  CHECK(!p.negotiate);
}

TEST(VkImageNeedsTheSameGpu) {
  SurfaceFacts f = VulkanShell();
  f.same_vk_device = false;
  SurfacePlan p = PlanSurface(f, SurfacePref::kVkImage);
  CHECK(p.path == SurfacePath::kDmabuf);  // preference falls back to auto
}

TEST(PlaneIsAPreferenceWithFallbacks) {
  SurfaceFacts f = VulkanShell();
  f.shell_kinds |= IHS_PV_KIND_DRM_PLANE;
  SurfacePlan p = PlanSurface(f, SurfacePref::kPlane);
  CHECK(p.path == SurfacePath::kDmabuf);
  CHECK(p.preferred_kind == IHS_PV_KIND_DRM_PLANE);
  CHECK((p.kinds & IHS_PV_KIND_TEXTURE_DMABUF_IMPORT) != 0);
  CHECK((p.kinds & IHS_PV_KIND_SOFTWARE_SHM) != 0);
}

TEST(SoftwareShellGetsTheFloor) {
  SurfaceFacts f;
  f.shell_kinds = IHS_PV_KIND_SOFTWARE_SHM;
  f.godot_target_types = (1u << IHS_GODOT_TARGET_DMABUF) |
                         (1u << IHS_GODOT_TARGET_CPU);
  f.godot_formats = {{kXR24, 0}};
  SurfacePlan p = PlanSurface(f, SurfacePref::kAuto);
  CHECK(p.path == SurfacePath::kShm);
  CHECK(p.kinds == IHS_PV_KIND_SOFTWARE_SHM);
  CHECK(p.preferred_kind == IHS_PV_KIND_SOFTWARE_SHM);
}

TEST(EglShellWithGlGodot) {
  SurfaceFacts f;
  f.shell_kinds = IHS_PV_KIND_TEXTURE_DMABUF_IMPORT | IHS_PV_KIND_DRM_PLANE |
                  IHS_PV_KIND_SOFTWARE_SHM | IHS_PV_KIND_TEXTURE_EGL_IMAGE;
  f.shell_formats = {{kXB24, 0}, {kAB24, 0}};
  f.godot_target_types =
      (1u << IHS_GODOT_TARGET_DMABUF) | (1u << IHS_GODOT_TARGET_CPU);
  f.godot_formats = {{kXB24, 0}, {kAB24, 0}};
  f.shell_egl_import = true;
  CHECK(PlanSurface(f, SurfacePref::kAuto).path == SurfacePath::kDmabuf);
  CHECK(PlanSurface(f, SurfacePref::kEglImage).path == SurfacePath::kEglImage);
  // A GL Godot cannot take the VkImage path.
  CHECK(PlanSurface(f, SurfacePref::kVkImage).path == SurfacePath::kDmabuf);
}

TEST(NothingInCommon) {
  SurfaceFacts f;
  f.shell_kinds = IHS_PV_KIND_TEXTURE_DMABUF_IMPORT;
  f.shell_formats = {{kXR24, kTiled}};
  f.godot_target_types = 1u << IHS_GODOT_TARGET_DMABUF;
  f.godot_formats = {{kXR24, 0}};
  CHECK(PlanSurface(f, SurfacePref::kAuto).path == SurfacePath::kNone);
}

TEST(GrantMapping) {
  CHECK(PathForGrant(IHS_PV_KIND_DRM_PLANE) == SurfacePath::kDmabuf);
  CHECK(PathForGrant(IHS_PV_KIND_TEXTURE_DMABUF_IMPORT) ==
        SurfacePath::kDmabuf);
  CHECK(PathForGrant(IHS_PV_KIND_SOFTWARE_SHM) == SurfacePath::kShm);
  CHECK(PathForGrant(0) == SurfacePath::kNone);
}

// --- frame latency ---------------------------------------------------------

constexpr uint64_t kMs = 1000000;

// Fences are fake: the "fd" is the signal time in ms, -1 for none. Closes are
// counted, so every fence taken is closed exactly once.
int g_closed = 0;
uint64_t FakeSignal(int fd) {
  return fd > 0 ? static_cast<uint64_t>(fd) * kMs : 0;
}
void FakeClose(int /*fd*/) {
  ++g_closed;
}
constexpr FrameLatency::FenceOps kFakeOps{&FakeSignal, &FakeClose};

TEST(LatencyMatchesSeqAndReportsAWindow) {
  g_closed = 0;
  FrameLatency l(kFakeOps, 2);
  FrameLatency::Report r;
  // begin 100, submit 110, gpu 115, shown 130 ms.
  l.Submitted(1, 100 * kMs, 110 * kMs, 115);
  CHECK(!l.Presented(1, 130 * kMs, 0x7, &r));
  l.Submitted(2, 116 * kMs, 126 * kMs, 131);
  CHECK(l.Presented(2, 150 * kMs, 0x7, &r));
  CHECK(r.shown == 2 && r.replaced == 0 && r.flags == 0x7);
  CHECK(r.frame.count == 2 && r.frame.p50_ns == 30 * kMs &&
        r.frame.max_ns == 34 * kMs);
  CHECK(r.submit.p50_ns == 20 * kMs && r.submit.max_ns == 24 * kMs);
  CHECK(r.gpu.p50_ns == 15 * kMs && r.gpu.max_ns == 19 * kMs);
  CHECK(g_closed == 2);
}

TEST(LatencySkippedSeqIsReplaced) {
  g_closed = 0;
  FrameLatency l(kFakeOps, 100);
  FrameLatency::Report r;
  l.Submitted(1, 10 * kMs, 11 * kMs, -1);
  l.Submitted(2, 20 * kMs, 21 * kMs, 22);
  l.Submitted(3, 30 * kMs, 31 * kMs, 32);
  l.Presented(3, 40 * kMs, 0, &r);  // 1 and 2 were never shown
  l.Presented(2, 41 * kMs, 0, &r);  // late or repeated: ignored
  l.Presented(9, 42 * kMs, 0, &r);  // not recorded here
  CHECK(l.Session(&r));
  CHECK(r.shown == 1 && r.replaced == 2);
  CHECK(r.frame.count == 1 && r.frame.p50_ns == 10 * kMs);
  CHECK(r.gpu.count == 1 && r.gpu.p50_ns == 8 * kMs);
  CHECK(g_closed == 2);  // the -1 fence was never closed
}

TEST(LatencyUnknownTimesAreLeftOut) {
  g_closed = 0;
  FrameLatency l(kFakeOps, 100);
  FrameLatency::Report r;
  l.Submitted(1, 0, 11 * kMs, -1);  // begin not measured, no fence
  l.Presented(1, 20 * kMs, 0, &r);
  l.Submitted(2, 30 * kMs, 31 * kMs, 50);  // fence "after" the photon
  l.Presented(2, 40 * kMs, 0, &r);
  CHECK(l.Session(&r));
  CHECK(r.shown == 2);
  CHECK(r.frame.count == 1 && r.submit.count == 2 && r.gpu.count == 0);
}

TEST(LatencyStubFenceTimeIsLeftOut) {
  g_closed = 0;
  FrameLatency l(kFakeOps, 100);
  FrameLatency::Report r;
  l.Submitted(1, 30 * kMs, 31 * kMs, 5);  // "signaled" before the frame began
  l.Presented(1, 40 * kMs, 0, &r);
  CHECK(l.Session(&r));
  CHECK(r.frame.count == 1 && r.gpu.count == 0);
  CHECK(g_closed == 1);
}

TEST(LatencyBoundsWhatIsPending) {
  g_closed = 0;
  FrameLatency::Report r;
  {
    FrameLatency l(kFakeOps, 100);
    for (uint64_t seq = 1; seq <= 40; seq++) {
      l.Submitted(seq, seq * kMs, seq * kMs, 1);
    }
    CHECK(!l.Session(&r));  // nothing shown
    CHECK(g_closed == 24);  // 16 kept
  }
  CHECK(g_closed == 40);  // the rest closed with it
}

TEST(LatencyEnvironmentSwitch) {
  ViewParams p;
  ::setenv("IHS_GODOT_VIEW_LATENCY", "1", 1);
  ApplyEnvironment(p);
  CHECK(p.latency_window == 300);
  ::setenv("IHS_GODOT_VIEW_LATENCY", "30", 1);
  ApplyEnvironment(p);
  CHECK(p.latency_window == 30);
  ::setenv("IHS_GODOT_VIEW_LATENCY", "yes", 1);
  ApplyEnvironment(p);
  CHECK(p.latency_window == 300);
  ::setenv("IHS_GODOT_VIEW_LATENCY", "99999999999999999999", 1);
  ApplyEnvironment(p);
  CHECK(p.latency_window == 300);
  ::setenv("IHS_GODOT_VIEW_LATENCY", "5000000", 1);
  ApplyEnvironment(p);
  CHECK(p.latency_window == 100000);
  ::setenv("IHS_GODOT_VIEW_LATENCY", "0", 1);
  ApplyEnvironment(p);
  CHECK(p.latency_window == 0);
  ::unsetenv("IHS_GODOT_VIEW_LATENCY");
}

}  // namespace

int main() {
  for (auto& [name, fn] : Tests()) {
    const int before = g_failures;
    fn();
    std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", name);
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

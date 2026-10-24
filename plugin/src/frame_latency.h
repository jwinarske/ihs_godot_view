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

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace ihs_godot {

// How long a view's frames take to reach the screen, from the shell's
// presented reports (IHS_GODOT_VIEW_LATENCY). Three spans end at the moment
// the frame started to be shown (presented's ust_ns):
//   frame   from the start of the Godot iteration that drew it, after the
//           buffer's release fence
//   submit  from the hand-off to the shell
//   gpu     from the GPU finishing it (the acquire fence's signal time), when
//           the driver records one and it is not before the frame began
// A seq the shell skips over was replaced before it was shown.
//
// Submitted() runs on the engine thread and Presented() on the display
// thread; the state is locked.
class FrameLatency {
 public:
  // How a fence's signal time is read and the fence closed. Signal time is
  // CLOCK_MONOTONIC nanoseconds, 0 when unknown.
  struct FenceOps {
    uint64_t (*signal_time_ns)(int fd);
    void (*close_fd)(int fd);
  };

  struct Span {
    size_t count = 0;
    uint64_t p50_ns = 0;
    uint64_t p99_ns = 0;
    uint64_t max_ns = 0;
  };

  struct Report {
    uint64_t shown = 0;
    uint64_t replaced = 0;
    uint32_t flags = 0;  // IHS_PV_PRESENTED_* of the last frame shown
    Span frame;
    Span submit;
    Span gpu;
  };

  // A report every @window frames shown (at most kSessionCap).
  FrameLatency(FenceOps ops, size_t window);
  ~FrameLatency();
  FrameLatency(const FrameLatency&) = delete;
  FrameLatency& operator=(const FrameLatency&) = delete;

  // Frame @seq (non-zero, increasing) was handed to the shell. Takes
  // @fence_fd (a dup of its acquire fence, or -1).
  void Submitted(uint64_t seq,
                 uint64_t begin_ns,
                 uint64_t submit_ns,
                 int fence_fd);

  // The shell showed frame @seq at @ust_ns. Returns true and fills @out when
  // that completed a window.
  bool Presented(uint64_t seq, uint64_t ust_ns, uint32_t flags, Report* out);

  // The whole session so far. Returns false when nothing was shown.
  bool Session(Report* out);

 private:
  struct Pending {
    uint64_t seq = 0;
    uint64_t begin_ns = 0;
    uint64_t submit_ns = 0;
    int fence_fd = -1;
  };

  struct Samples {
    std::vector<uint64_t> frame;
    std::vector<uint64_t> submit;
    std::vector<uint64_t> gpu;
    uint64_t shown = 0;
    uint64_t replaced = 0;
    void Clear();
  };

  void Drop(Pending& p);
  static Span Summarize(std::vector<uint64_t>& v);
  static void Fill(Samples& s, uint32_t flags, Report* out);

  // More frames than the ring has buffers are never in flight; a seq left
  // here past that was replaced.
  static constexpr size_t kPending = 16;
  // Session samples kept, per span: a long run reports on its first ones.
  static constexpr size_t kSessionCap = 1U << 18U;

  const FenceOps ops_;
  const size_t window_;
  std::mutex mutex_;
  std::array<Pending, kPending> pending_{};
  size_t head_ = 0;  // oldest
  size_t count_ = 0;
  uint32_t last_flags_ = 0;
  Samples window_samples_;
  Samples session_;
};

}  // namespace ihs_godot

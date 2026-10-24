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

#include "frame_latency.h"

#include <algorithm>

namespace ihs_godot {

namespace {

void Add(std::vector<uint64_t>& v,
         uint64_t later,
         uint64_t earlier,
         size_t cap) {
  if (earlier == 0 || later < earlier || v.size() >= cap) {
    return;
  }
  v.push_back(later - earlier);
}

}  // namespace

void FrameLatency::Samples::Clear() {
  frame.clear();
  submit.clear();
  gpu.clear();
  shown = 0;
  replaced = 0;
}

FrameLatency::FrameLatency(FenceOps ops, size_t window)
    : ops_(ops), window_(std::clamp<size_t>(window, 1, kSessionCap)) {
  window_samples_.frame.reserve(window_);
  window_samples_.submit.reserve(window_);
  window_samples_.gpu.reserve(window_);
}

FrameLatency::~FrameLatency() {
  for (size_t i = 0; i < count_; i++) {
    Drop(pending_[(head_ + i) % kPending]);
  }
}

void FrameLatency::Drop(Pending& p) {
  if (p.fence_fd >= 0 && ops_.close_fd != nullptr) {
    ops_.close_fd(p.fence_fd);
  }
  p = Pending{};
}

void FrameLatency::Submitted(uint64_t seq,
                             uint64_t begin_ns,
                             uint64_t submit_ns,
                             int fence_fd) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (count_ == kPending) {
    Drop(pending_[head_]);
    head_ = (head_ + 1) % kPending;
    count_--;
    window_samples_.replaced++;
    session_.replaced++;
  }
  Pending& p = pending_[(head_ + count_) % kPending];
  p.seq = seq;
  p.begin_ns = begin_ns;
  p.submit_ns = submit_ns;
  p.fence_fd = fence_fd;
  count_++;
}

bool FrameLatency::Presented(uint64_t seq,
                             uint64_t ust_ns,
                             uint32_t flags,
                             Report* out) {
  const std::lock_guard<std::mutex> lock(mutex_);
  // Older seqs ahead of this one were replaced before they were shown.
  while (count_ > 0 && pending_[head_].seq < seq) {
    Drop(pending_[head_]);
    head_ = (head_ + 1) % kPending;
    count_--;
    window_samples_.replaced++;
    session_.replaced++;
  }
  if (count_ == 0 || pending_[head_].seq != seq) {
    return false;  // not one recorded here
  }
  Pending& p = pending_[head_];
  uint64_t gpu_ns = p.fence_fd >= 0 && ops_.signal_time_ns != nullptr
                        ? ops_.signal_time_ns(p.fence_fd)
                        : 0;
  // A fence already signaled when it was exported can come back as the
  // kernel's stub fence, whose time is long past: before the frame began,
  // which no GPU work for it can be.
  if (gpu_ns != 0 && p.begin_ns != 0 && gpu_ns < p.begin_ns) {
    gpu_ns = 0;
  }
  for (Samples* s : {&window_samples_, &session_}) {
    const size_t cap = s == &session_ ? kSessionCap : window_;
    Add(s->frame, ust_ns, p.begin_ns, cap);
    Add(s->submit, ust_ns, p.submit_ns, cap);
    Add(s->gpu, ust_ns, gpu_ns, cap);
    s->shown++;
  }
  Drop(p);
  head_ = (head_ + 1) % kPending;
  count_--;
  last_flags_ = flags;

  if (window_samples_.shown < window_) {
    return false;
  }
  Fill(window_samples_, flags, out);
  window_samples_.Clear();
  return true;
}

bool FrameLatency::Session(Report* out) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (session_.shown == 0) {
    return false;
  }
  Samples copy = session_;
  Fill(copy, last_flags_, out);
  return true;
}

FrameLatency::Span FrameLatency::Summarize(std::vector<uint64_t>& v) {
  Span s;
  s.count = v.size();
  if (v.empty()) {
    return s;
  }
  std::sort(v.begin(), v.end());
  s.p50_ns = v[(v.size() - 1) / 2];
  s.p99_ns = v[(v.size() - 1) * 99 / 100];
  s.max_ns = v.back();
  return s;
}

void FrameLatency::Fill(Samples& s, uint32_t flags, Report* out) {
  if (out == nullptr) {
    return;
  }
  out->shown = s.shown;
  out->replaced = s.replaced;
  out->flags = flags;
  out->frame = Summarize(s.frame);
  out->submit = Summarize(s.submit);
  out->gpu = Summarize(s.gpu);
}

}  // namespace ihs_godot

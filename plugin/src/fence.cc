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

#include "fence.h"

#include <linux/dma-buf.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>

namespace ihs_godot {

bool WaitFence(UniqueFd& fence, int timeout_ms) {
  if (!fence.valid()) {
    return true;
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  pollfd pfd{fence.get(), POLLIN, 0};
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    const int timeout = left.count() > 0 ? static_cast<int>(left.count()) : 0;
    const int rc = ::poll(&pfd, 1, timeout);
    if (rc != -1 || errno != EINTR || timeout == 0) {
      break;
    }
  }
  fence.reset();
  return (pfd.revents & POLLIN) != 0;
}

uint64_t FenceSignalTimeNs(int fd) {
  if (fd < 0) {
    return 0;
  }
  sync_file_info info{};
  if (::ioctl(fd, SYNC_IOC_FILE_INFO, &info) != 0 || info.status != 1 ||
      info.num_fences == 0) {
    return 0;
  }
  // Called per presented frame: no allocation. A merged fence of more than a
  // few is not what a renderer hands over.
  std::array<sync_fence_info, 4> fences{};
  const uint32_t n = info.num_fences;
  if (n > fences.size()) {
    return 0;
  }
  info = sync_file_info{};
  info.num_fences = n;
  info.sync_fence_info = reinterpret_cast<uintptr_t>(fences.data());
  if (::ioctl(fd, SYNC_IOC_FILE_INFO, &info) != 0 || info.num_fences != n) {
    return 0;
  }
  uint64_t latest = 0;
  for (uint32_t i = 0; i < n; i++) {
    const sync_fence_info& f = fences[i];
    if (f.status != 1 || f.timestamp_ns == 0) {
      return 0;  // one without a time leaves the whole fence's time unknown
    }
    latest = std::max<uint64_t>(latest, f.timestamp_ns);
  }
  return latest;
}

namespace {

void Sync(int fd, uint64_t flags) {
  if (fd < 0) {
    return;
  }
  dma_buf_sync sync{};
  sync.flags = flags;
  while (::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) != 0 &&
         (errno == EINTR || errno == EAGAIN)) {
  }
}

}  // namespace

void DmabufSyncBegin(int fd, bool write) {
  Sync(fd, DMA_BUF_SYNC_START |
               (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ));
}

void DmabufSyncEnd(int fd, bool write) {
  Sync(fd, DMA_BUF_SYNC_END | (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ));
}

}  // namespace ihs_godot

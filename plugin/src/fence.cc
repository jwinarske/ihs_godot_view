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
#include <poll.h>
#include <sys/ioctl.h>

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

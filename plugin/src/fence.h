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

#include <cstdint>

#include "unique_fd.h"

namespace ihs_godot {

// Wait for a sync_file / eventfd release fence, then close it. Bounded:
// a registry that stops releasing must not park the render thread forever
// (fluorite and pv_bench both hit that), so after @timeout_ms the caller
// draws over the buffer anyway. EINTR does not restart the full timeout.
// Returns true when the fence signaled (an invalid fd counts as signaled).
bool WaitFence(UniqueFd& fence, int timeout_ms);

// When a signaled sync_file fence signaled, in CLOCK_MONOTONIC nanoseconds:
// the latest of its fences' timestamps. 0 when it has not signaled, is not a
// sync_file, or the driver records no time.
uint64_t FenceSignalTimeNs(int fd);

// DMA_BUF_IOCTL_SYNC around CPU access, retried on EINTR/EAGAIN.
void DmabufSyncBegin(int fd, bool write);
void DmabufSyncEnd(int fd, bool write);

}  // namespace ihs_godot

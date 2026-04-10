/* Copyright (C) 2025 Ahmed Gheith and contributors.
 *
 * Use restricted to classroom projects.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <cstdint>
// #include "syscall.h"

#include "print.h"
#include "thread.h"

namespace {

constexpr uint64_t userTopExclusive = UINT64_C(0x0000800000000000);

bool is_valid_user_buffer(uint64_t ptr, uint64_t len) {
  if (len == 0) {
    return true;
  }

  if (ptr >= userTopExclusive) {
    return false;
  }

  auto last = ptr + len - 1;
  if (last < ptr) {
    return false;
  }

  if (last >= userTopExclusive) {
    return false;
  }

  return true;
}

} // namespace

struct SyscallFrame {
  uint64_t rax;
  uint64_t rbx;
  uint64_t rcx;
  uint64_t rdx;
  uint64_t rsi;
  uint64_t rdi;
  uint64_t rbp;
  uint64_t r8;
  uint64_t r9;
  uint64_t r10;
  uint64_t r11;
  uint64_t r12;
  uint64_t r13;
  uint64_t r14;
  uint64_t r15;
};

extern "C" [[gnu::force_align_arg_pointer]] int
syscallHandler(SyscallFrame *frame) {
  // SAY("rax = ?\n", Dec(frame->rax));

  switch (frame->rax) {
  case 1: /* write */
  {
    if (frame->rdi != 1) {
      return -1;
    }

    if (!is_valid_user_buffer(frame->rsi, frame->rdx)) {
      return -1;
    }

    char *buffer = (char *)frame->rsi;
    uint64_t len = frame->rdx;
    for (uint64_t i = 0; i < len; i++) {
      putch(buffer[i]);
    }
    return (int)len;
  }
  case 60: /* exit */
  {
    (void)frame->rdi;
    Thread::stop();
  }
  default:
    SAY("syscall ?\n", Dec(frame->rax));
    KPANIC("Unknown syscall ?\n", Dec(frame->rax));
  }
}

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

#include "ext2.h"
#include "machine.h"
#include "per_core.h"
#include "print.h"
#include "thread.h"
#include "vmm.h"

extern "C" [[noreturn]] void fork_return_to_user(void *frame,
                                                   uint64_t user_rsp);


namespace impl {
extern Queue<TCB, SpinLock> readyQueue;
extern Atomic<uint64_t> n_active;
void reap();
} // namespace impl

extern Ext2 *g_sys_fs_ptr;

namespace {

constexpr uint64_t userTopExclusive = UINT64_C(0x0000800000000000);
constexpr int maxFds = 32;
constexpr int maxSemaphores = 64;

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

struct ProcessEntry {
  bool used = false;
  int parent = 0;
  bool exited = false;
  int status = 0;
};

struct FileDescriptor {
  bool used = false;
  uint32_t inode = 0;
  uint64_t offset = 0;
};

struct KernelSemaphore {
  bool used = false;
  int key = 0;
  int value = 0;
};

struct UserSembuf {
  uint16_t sem_num;
  int16_t sem_op;
  int16_t sem_flg;
};

constexpr int maxProcesses = 1024;

ProcessEntry processTable[maxProcesses]{};
SpinLock processLock{};
int nextPid = 2;
bool processTableInitialized = false;
KernelSemaphore semaphores[maxSemaphores]{};
SpinLock semLock{};
thread_local FileDescriptor fdTable[maxFds]{};
thread_local bool fdTableInitialized = false;

void init_fd_table() {
  if (fdTableInitialized) {
    return;
  }
  for (int i = 0; i < maxFds; i++) {
    fdTable[i].used = false;
    fdTable[i].inode = 0;
    fdTable[i].offset = 0;
  }
  fdTableInitialized = true;
}

bool copy_user_cstring(uint64_t ptr, char *out, uint64_t maxLen) {
  if (ptr == 0 || maxLen == 0) {
    return false;
  }

  for (uint64_t i = 0; i < maxLen; i++) {
    if (!VMM::is_user_range_mapped(ptr + i, 1)) {
      return false;
    }
    auto ch = ((char *)ptr)[i];
    out[i] = ch;
    if (ch == 0) {
      return true;
    }
  }
  out[maxLen - 1] = 0;
  return true;
}

int allocate_fd(uint32_t inode) {
  init_fd_table();
  for (int i = 3; i < maxFds; i++) {
    if (!fdTable[i].used) {
      fdTable[i].used = true;
      fdTable[i].inode = inode;
      fdTable[i].offset = 0;
      return i;
    }
  }
  return -1;
}

KernelSemaphore *find_or_create_sem_locked(int key) {
  for (int i = 0; i < maxSemaphores; i++) {
    if (semaphores[i].used && semaphores[i].key == key) {
      return &semaphores[i];
    }
  }

  for (int i = 0; i < maxSemaphores; i++) {
    if (!semaphores[i].used) {
      semaphores[i].used = true;
      semaphores[i].key = key;
      semaphores[i].value = 0;
      return &semaphores[i];
    }
  }

  return nullptr;
}

void init_process_table_locked() {
  if (processTableInitialized) {
    return;
  }
  for (int i = 0; i < maxProcesses; i++) {
    processTable[i].used = false;
    processTable[i].parent = 0;
    processTable[i].exited = false;
    processTable[i].status = 0;
  }
  nextPid = 2;
  processTableInitialized = true;
}

int allocate_pid_locked() {
  for (int i = 0; i < maxProcesses; i++) {
    int pid = nextPid;
    nextPid += 1;
    if (nextPid >= maxProcesses) {
      nextPid = 2;
    }
    if (pid > 0 && pid < maxProcesses && !processTable[pid].used) {
      return pid;
    }
  }
  return -1;
}

int ensure_current_pid() {
  auto me = impl::TCB::current();
  if (me == nullptr) {
    return -1;
  }

  if (me->pid > 0) {
    return me->pid;
  }

  processLock.lock();
  if (!processTableInitialized) {
    init_process_table_locked();
  }
  int pid = 1;
  me->pid = pid;
  processTable[pid].used = true;
  processTable[pid].parent = 0;
  processTable[pid].exited = false;
  processTable[pid].status = 0;
  processLock.unlock();
  return pid;
}

[[gnu::force_align_arg_pointer]] [[noreturn]] void
fork_child_entry(impl::TCB *prev) {
  prev->post_switch();

  auto me = impl::TCB::current();
  impl::reap();

  VMM::ForkState state{};
  state.cr3 = 0;
  state.private_vmes = me->fork_private_vmes;
  state.heap_start = me->fork_heap_start;
  state.heap_break = me->fork_heap_break;
  VMM::install_fork_state(state);

  disable();
  PerCore::get()->tss.rsp0 = (uint64_t)me->stack_bottom;

  fork_return_to_user((void *)me->fork_frame, me->fork_user_rsp);
}

void register_process_pid1() {
  auto me = impl::TCB::current();
  if (me == nullptr) {
    return;
  }

  processLock.lock();
  processTableInitialized = false;
  init_process_table_locked();
  me->pid = 1;
  processTable[1].used = true;
  processTable[1].parent = 0;
  processTable[1].exited = false;
  processTable[1].status = 0;
  processLock.unlock();
}

} // namespace

extern "C" void syscall_register_init_process() { register_process_pid1(); }

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

static inline long syscall_return(SyscallFrame *frame, long value) {
  frame->rax = (uint64_t)value;
  return value;
}

static inline uint64_t current_user_rsp() {
  uint64_t rsp;
  asm volatile("mov %%gs:28, %0" : "=r"(rsp));
  return rsp;
}

extern "C" [[gnu::force_align_arg_pointer]] long
syscallHandler(SyscallFrame *frame) {
  // SAY("rax = ?\n", Dec(frame->rax));

  switch (frame->rax) {

  case 0: /* read */
  {
    auto fd = (int)frame->rdi;
    auto buf = frame->rsi;
    auto len = frame->rdx;

    init_fd_table();
    if (fd < 0 || fd >= maxFds || !fdTable[fd].used) {
      return syscall_return(frame, -1);
    }
    if (!is_valid_user_buffer(buf, len) || !VMM::is_user_range_mapped(buf, len)) {
      return syscall_return(frame, -1);
    }

    if (g_sys_fs_ptr == nullptr || fdTable[fd].inode == 0) {
      return syscall_return(frame, -1);
    }

    auto node = g_sys_fs_ptr->get_node(fdTable[fd].inode);
    auto fileSize = node->size_in_bytes();
    auto off = fdTable[fd].offset;
    if (off >= fileSize) {
      return syscall_return(frame, 0);
    }

    auto remaining = fileSize - off;
    auto toRead = (len < remaining) ? len : remaining;
    auto got = node->read_all(off, toRead, (char *)buf);
    fdTable[fd].offset += got;
    return syscall_return(frame, got);
  }
  case 1: /* write */
  {
    if (frame->rdi != 1) {
      return syscall_return(frame, -1);
    }

    if (!is_valid_user_buffer(frame->rsi, frame->rdx)) {
      return syscall_return(frame, -1);
    }

    if (!VMM::is_user_range_mapped(frame->rsi, frame->rdx)) {
      return syscall_return(frame, -1);
    }

    char *buffer = (char *)frame->rsi;
    uint64_t len = frame->rdx;
    for (uint64_t i = 0; i < len; i++) {
      putch(buffer[i]);
    }
    return syscall_return(frame, (long)len);
  }
  case 2: /* open */
  {
    if (g_sys_fs_ptr == nullptr) {
      return syscall_return(frame, -1);
    }

    char path[256];
    if (!copy_user_cstring(frame->rdi, path, sizeof(path))) {
      return syscall_return(frame, -1);
    }

    auto node = g_sys_fs_ptr->find(g_sys_fs_ptr->root, path);
    if (node == StrongRef<Node>{} || !node->is_file()) {
      return syscall_return(frame, -1);
    }

    auto fd = allocate_fd(node->number);
    return syscall_return(frame, fd);
  }
  case 3: /* close */
  {
    auto fd = (int)frame->rdi;
    init_fd_table();
    if (fd < 0 || fd >= maxFds || !fdTable[fd].used) {
      return syscall_return(frame, -1);
    }
    fdTable[fd].used = false;
    fdTable[fd].inode = 0;
    fdTable[fd].offset = 0;
    return syscall_return(frame, 0);
  }
  case 8: /* lseek */
  {
    auto fd = (int)frame->rdi;
    auto offset = frame->rsi;
    auto whence = (int)frame->rdx;

    init_fd_table();
    if (fd < 0 || fd >= maxFds || !fdTable[fd].used) {
      return syscall_return(frame, -1);
    }
    if (whence != 0) {
      return syscall_return(frame, -1);
    }

    if (g_sys_fs_ptr == nullptr || fdTable[fd].inode == 0) {
      return syscall_return(frame, -1);
    }

    auto node = g_sys_fs_ptr->get_node(fdTable[fd].inode);
    auto fileSize = node->size_in_bytes();
    if (offset > fileSize) {
      return syscall_return(frame, -1);
    }

    fdTable[fd].offset = offset;
    return syscall_return(frame, (long)offset);
  }
  case 12: /* brk */
  {
    if (frame->rdi == 0) {
      return syscall_return(frame, (long)VMM::get_heap_break());
    }

    auto target = frame->rdi;
    if (VMM::set_heap_break(target) != 0) {
      return syscall_return(frame, (long)VMM::get_heap_break());
    }
    return syscall_return(frame, (long)target);
  }
  case 24: /* sched_yield */
  {
    restore(false);
    Thread::yield();
    return syscall_return(frame, 0);
  }
  case 57: /* fork */
  {
    auto myPid = ensure_current_pid();
    if (myPid <= 0) {
      myPid = 1;
    }
    VMM::ForkState childState{};
    if (!VMM::snapshot_for_fork(childState)) {
      KPRINT("fork: snapshot_for_fork failed\n");
      return syscall_return(frame, -1);
    }

    processLock.lock();
    init_process_table_locked();
    int childPid = allocate_pid_locked();
    if (childPid < 0) {
      KPRINT("fork: allocate_pid failed\n");
      processLock.unlock();
      return syscall_return(frame, -1);
    }

    processTable[childPid].used = true;
    processTable[childPid].parent = myPid;
    processTable[childPid].exited = false;
    processTable[childPid].status = 0;
    processLock.unlock();

    auto child = impl::TCB::alloc();
    child->pid = childPid;
    child->fork_user_rsp = current_user_rsp();
    for (uint64_t i = 0; i < 15; i++) {
      child->fork_frame[i] = ((uint64_t *)frame)[i];
    }
    child->fork_private_vmes = childState.private_vmes;
    child->fork_heap_start = childState.heap_start;
    child->fork_heap_break = childState.heap_break;

    static constexpr size_t STACK_QUADS = 2048;
    child->the_stack = new uintptr_t[STACK_QUADS];
    child->stack_bottom = child->the_stack + STACK_QUADS;
    child->the_stack[STACK_QUADS - 1] = (uintptr_t)fork_child_entry;
    child->the_stack[STACK_QUADS - 2] = 0;
    child->the_stack[STACK_QUADS - 3] = 0;
    child->the_stack[STACK_QUADS - 4] = 0;
    child->the_stack[STACK_QUADS - 5] = 0;
    child->the_stack[STACK_QUADS - 6] = 0;
    child->the_stack[STACK_QUADS - 7] = 0;
    child->the_stack[STACK_QUADS - 8] = 0x200;
    child->the_stack[STACK_QUADS - 9] = 0;
    child->the_stack[STACK_QUADS - 10] = childState.cr3;
    child->saved_rsp = (uintptr_t)&child->the_stack[STACK_QUADS - 10];

    impl::n_active.add_fetch(1);
    impl::readyQueue.add(child);
    return syscall_return(frame, childPid);
  }
  case 60: /* exit */
  {
    auto me = impl::TCB::current();
    auto myPid = ensure_current_pid();
    if (me != nullptr && myPid > 0) {
      processLock.lock();
      init_process_table_locked();
      auto pid = myPid;
      if (pid > 0 && pid < maxProcesses && processTable[pid].used) {
        processTable[pid].exited = true;
        processTable[pid].status = (int)frame->rdi;
      }
      processLock.unlock();
    }
    Thread::stop();
  }
  case 61: /* waitpid */
  {
    auto me = impl::TCB::current();
    auto myPid = ensure_current_pid();
    if (me == nullptr || myPid <= 0) {
      return syscall_return(frame, -1);
    }

    auto childPid = (int)frame->rdi;
    auto statusPtr = frame->rsi;
    if (childPid <= 0 || childPid >= maxProcesses) {
      return syscall_return(frame, -1);
    }
    if (frame->rdx != 0) {
      return syscall_return(frame, -1);
    }
    if (statusPtr != 0 &&
        (!is_valid_user_buffer(statusPtr, sizeof(int)) ||
         !VMM::is_user_range_mapped(statusPtr, sizeof(int)))) {
      return syscall_return(frame, -1);
    }

    while (true) {
      processLock.lock();
      init_process_table_locked();
      if (!processTable[childPid].used || processTable[childPid].parent != myPid) {
        processLock.unlock();
        return syscall_return(frame, -1);
      }

      if (processTable[childPid].exited) {
        auto status = processTable[childPid].status;
        processTable[childPid].used = false;
        processLock.unlock();

        if (statusPtr != 0) {
          *(int *)statusPtr = (status & 0xFF) << 8;
        }
        return syscall_return(frame, childPid);
      }
      processLock.unlock();
      restore(false);
      Thread::yield();
    }
  }
  case 64: /* semget */
  {
    auto key = (int)frame->rdi;
    auto nsems = (int)frame->rsi;
    (void)frame->rdx;

    if (nsems != 1) {
      return syscall_return(frame, -1);
    }

    semLock.lock();
    auto sem = find_or_create_sem_locked(key);
    if (sem == nullptr) {
      semLock.unlock();
      return syscall_return(frame, -1);
    }
    auto id = (int)(sem - &semaphores[0]);
    semLock.unlock();
    return syscall_return(frame, id);
  }
  case 65: /* semop */
  {
    auto semid = (int)frame->rdi;
    auto sops = frame->rsi;
    auto nsops = frame->rdx;

    if (semid < 0 || semid >= maxSemaphores || nsops != 1) {
      return syscall_return(frame, -1);
    }
    if (!is_valid_user_buffer(sops, sizeof(UserSembuf)) ||
        !VMM::is_user_range_mapped(sops, sizeof(UserSembuf))) {
      return syscall_return(frame, -1);
    }

    auto op = *(UserSembuf *)sops;
    if (op.sem_num != 0) {
      return syscall_return(frame, -1);
    }

    while (true) {
      semLock.lock();
      auto &sem = semaphores[semid];
      if (!sem.used) {
        semLock.unlock();
        return syscall_return(frame, -1);
      }

      if (op.sem_op < 0) {
        auto need = -int(op.sem_op);
        if (sem.value >= need) {
          sem.value -= need;
          semLock.unlock();
          return syscall_return(frame, 0);
        }
        semLock.unlock();
        restore(false);
        Thread::yield();
        continue;
      }

      if (op.sem_op > 0) {
        sem.value += int(op.sem_op);
      }
      semLock.unlock();
      return syscall_return(frame, 0);
    }
  }
  case 66: /* semctl */
  {
    auto semid = (int)frame->rdi;
    (void)frame->rsi;
    auto cmd = (int)frame->rdx;

    if (semid < 0 || semid >= maxSemaphores || cmd != 0) {
      return syscall_return(frame, -1);
    }

    semLock.lock();
    auto &sem = semaphores[semid];
    if (!sem.used) {
      semLock.unlock();
      return syscall_return(frame, -1);
    }
    sem.used = false;
    sem.key = 0;
    sem.value = 0;
    semLock.unlock();
    return syscall_return(frame, 0);
  }
  default:
    SAY("syscall ?\n", Dec(frame->rax));
    KPANIC("Unknown syscall ?\n", Dec(frame->rax));
  }
}

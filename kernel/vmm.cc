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

#include "vmm.h"

#include "atomic.h"
#include "ext2.h"
#include "idt.h"
#include "machine.h"
#include "physmem.h"
#include "print.h"
#include "shared.h"
#include "spin_lock.h"
#include "system_main.h"
#include <cstdint>

uint64_t impl::common_cr3 = 0;

uint64_t VA::check_canonical(uint64_t va) {
  int64_t sa = int64_t(va);
  ASSERT(((sa << 16) >> 16) == sa);
  return va;
}

namespace impl {

constexpr uint64_t presentBit = UINT64_C(1) << 0;
constexpr uint64_t writableBit = UINT64_C(1) << 1;
constexpr uint64_t userBit = UINT64_C(1) << 2;
constexpr uint64_t addressMask = UINT64_C(0x000FFFFFFFFFF000);

constexpr uint64_t sharedPaLimit = UINT64_C(128) * 1024 * 1024;
constexpr uint64_t sharedEnd = UINT64_C(0xFFFFFFFF80000000);

extern Atomic<uint64_t> n_active;

inline uint64_t roundUpPage(uint64_t n) {
  return ((n + FRAME_SIZE - 1) / FRAME_SIZE) * FRAME_SIZE;
}

inline uint64_t vpnIndex(VPN vpn, uint32_t level) {
  return (vpn.vpn() >> (9 * level)) & 0x1FF;
}

inline PPN entryPpn(uint64_t entry) {
  return PPN((entry & addressMask) >> LOG_FRAME_SIZE);
}

inline uint64_t sharedStart() { return Sys::hhdm_offset + sharedPaLimit; }

inline bool isSharedAddress(uint64_t va) {
  return (va >= sharedStart()) && (va < sharedEnd);
}

inline uint64_t *leafPte(uint64_t va) {
  auto vpn = VPN(VA(va));
  auto table = (uint64_t *)VA(PPN(get_cr3() >> LOG_FRAME_SIZE));

  for (uint32_t level = 3; level > 0; level--) {
    auto entry = table[vpnIndex(vpn, level)];
    if ((entry & presentBit) == 0) {
      return nullptr;
    }
    table = (uint64_t *)VA(entryPpn(entry));
  }

  return &table[vpnIndex(vpn, 0)];
}

PPN cloneTableSubtree(PPN srcTablePpn, uint32_t level) {
  auto dstTablePpn = physMem.alloc();
  auto src = (uint64_t *)VA(srcTablePpn);
  auto dst = (uint64_t *)VA(dstTablePpn);

  for (uint64_t i = 0; i < 512; i++) {
    dst[i] = 0;
  }

  for (uint64_t i = 0; i < 512; i++) {
    auto entry = src[i];
    if ((entry & presentBit) == 0) {
      continue;
    }

    if (level == 0) {
      auto srcFrame = entryPpn(entry);
      auto dstFrame = physMem.alloc();
      auto srcBytes = (char *)VA(srcFrame);
      auto dstBytes = (char *)VA(dstFrame);
      for (uint64_t j = 0; j < FRAME_SIZE; j++) {
        dstBytes[j] = srcBytes[j];
      }
      dst[i] = (dstFrame.ppn() << LOG_FRAME_SIZE) | (entry & ~addressMask);
      continue;
    }

    auto srcChild = entryPpn(entry);
    auto dstChild = cloneTableSubtree(srcChild, level - 1);
    dst[i] = (dstChild.ppn() << LOG_FRAME_SIZE) | (entry & ~addressMask);
  }

  return dstTablePpn;
}

inline void unmapPage(uint64_t va) {
  auto pte = leafPte(va);
  if (pte == nullptr) {
    return;
  }

  auto entry = *pte;
  if ((entry & presentBit) == 0) {
    return;
  }

  auto ppn = entryPpn(entry);
  *pte = 0;
  asm volatile("invlpg (%0)" : : "r"(va) : "memory");
  physMem.free(ppn);
}

inline void unmapRange(uint64_t start, uint64_t end) {
  for (uint64_t va = start; va < end; va += FRAME_SIZE) {
    unmapPage(va);
  }
}

void freeTableSubtree(PPN tablePpn, uint32_t level) {
  auto table = (uint64_t *)VA(tablePpn);

  if (level == 0) {
    for (uint64_t i = 0; i < 512; i++) {
      auto entry = table[i];
      if ((entry & presentBit) == 0) {
        continue;
      }
      physMem.free(entryPpn(entry));
      table[i] = 0;
    }
    return;
  }

  for (uint64_t i = 0; i < 512; i++) {
    auto entry = table[i];
    if ((entry & presentBit) == 0) {
      continue;
    }

    auto child = entryPpn(entry);
    freeTableSubtree(child, level - 1);
    physMem.free(child);
    table[i] = 0;
  }
}

// This helper function should insert the requested mapping into this thread's
// page table structure. It may be helpful to make it recursive, using the
// level variable (though it is not required!)
void map(const PPN tablePpn, uint32_t level, VPN vpn, PPN ppn, bool user,
         bool write) {
  ASSERT(level <= 3);

  const auto index = vpnIndex(vpn, level);
  auto table = (uint64_t *)VA(tablePpn);

  if (level == 0) {
    uint64_t flags = presentBit;
    if (write) {
      flags |= writableBit;
    }
    if (user) {
      flags |= userBit;
    }
    table[index] = (ppn.ppn() << LOG_FRAME_SIZE) | flags;
    return;
  }

  auto entry = table[index];
  if ((entry & presentBit) == 0) {
    auto child = physMem.alloc();
    table[index] =
        (child.ppn() << LOG_FRAME_SIZE) | presentBit | writableBit | userBit;
    entry = table[index];
  }

  auto childPpn = entryPpn(entry);
  map(childPpn, level - 1, vpn, ppn, user, write);
}

void map(VPN vpn, PPN ppn, bool user, bool write) {
  map(PA(get_cr3()), 3, vpn, ppn, user, write);
}

void map_range(VA start, uint64_t size, bool user, bool write) {
  if (size == 0) {
    return;
  }

  auto startVa = start.va() & ~(FRAME_SIZE - 1);
  auto lastByte = start.va() + size - 1;
  ASSERT(lastByte >= start.va());
  auto endVa = (lastByte | (FRAME_SIZE - 1)) + 1;
  ASSERT(endVa >= startVa);

  for (auto va = startVa; va < endVa; va += FRAME_SIZE) {
    auto pte = leafPte(va);
    if ((pte != nullptr) && ((*pte & presentBit) != 0)) {
      continue;
    }

    auto frame = physMem.alloc();
    map(VPN(VA(va)), frame, user, write);
  }
}

struct PageFaultTrapFrame {
  std::uint64_t rax;
  std::uint64_t rbx;
  std::uint64_t rcx;
  std::uint64_t rdx;
  std::uint64_t rdi;
  std::uint64_t rsi;
  std::uint64_t rbp;
  std::uint64_t r8;
  std::uint64_t r9;
  std::uint64_t r10;
  std::uint64_t r11;
  std::uint64_t r12;
  std::uint64_t r13;
  std::uint64_t r14;
  std::uint64_t r15;
  std::uint64_t error_code;
  std::uint64_t rip;
  std::uint64_t cs;
  std::uint64_t rflags;
  std::uint64_t rsp;
  std::uint64_t ss;
};

struct VME {
  uint64_t start; // page aligned
  uint64_t end;   // page alinged
  bool shared;
  StrongRef<Node> node;
  uint64_t offset; // page aligned
  VME *next = nullptr;
};

thread_local VME *privateVmes = nullptr;
VME *sharedVmes = nullptr;
SpinLock sharedLock{};

thread_local uint64_t heapStart = 0;
thread_local uint64_t heapBreak = 0;

VME *clonePrivateVmes() {
  VME *head = nullptr;
  VME **tail = &head;

  auto cur = privateVmes;
  while (cur != nullptr) {
    auto node = new VME();
    node->start = cur->start;
    node->end = cur->end;
    node->shared = cur->shared;
    node->node = cur->node;
    node->offset = cur->offset;
    node->next = nullptr;
    *tail = node;
    tail = &node->next;
    cur = cur->next;
  }

  return head;
}

bool mapPageFromVme(VME *vme, uint64_t pageVa) {
  auto pte = leafPte(pageVa);
  if ((pte != nullptr) && ((*pte & presentBit) != 0)) {
    return true;
  }

  auto frame = physMem.alloc();
  auto frameWords = (uint64_t *)VA(frame);
  for (uint64_t i = 0; i < FRAME_SIZE / sizeof(uint64_t); i++) {
    frameWords[i] = 0;
  }

  if (!(vme->node == StrongRef<Node>{})) {
    auto framePtr = (char *)VA(frame);
    auto fileOffset = uint32_t(vme->offset + (pageVa - vme->start));
    vme->node->read_all(fileOffset, FRAME_SIZE, framePtr);
  }

  pte = leafPte(pageVa);
  if ((pte != nullptr) && ((*pte & presentBit) != 0)) {
    physMem.free(frame);
    return true;
  }

  impl::map(VPN(VA(pageVa)), frame, true, true);
  return true;
}

bool ensureMappedPage(uint64_t pageVa) {
  auto pte = leafPte(pageVa);
  if ((pte != nullptr) && ((*pte & presentBit) != 0)) {
    return true;
  }

  auto vmeCursor = privateVmes;
  while (vmeCursor != nullptr) {
    if ((pageVa >= vmeCursor->start) && (pageVa < vmeCursor->end)) {
      return mapPageFromVme(vmeCursor, pageVa);
    }
    vmeCursor = vmeCursor->next;
  }

  VME *sharedMatch = nullptr;
  sharedLock.lock();
  vmeCursor = sharedVmes;
  while (vmeCursor != nullptr) {
    if ((pageVa >= vmeCursor->start) && (pageVa < vmeCursor->end)) {
      sharedMatch = vmeCursor;
      break;
    }
    vmeCursor = vmeCursor->next;
  }
  sharedLock.unlock();

  if (sharedMatch != nullptr) {
    return mapPageFromVme(sharedMatch, pageVa);
  }

  return false;
}

} // namespace impl

bool VMM::is_user_range_mapped(uint64_t start, uint64_t size) {
  using namespace impl;

  if (size == 0) {
    return true;
  }

  if (start < 0x1000 || start >= UINT64_C(0x0000800000000000)) {
    return false;
  }

  auto last = start + size - 1;
  if (last < start || last >= UINT64_C(0x0000800000000000)) {
    return false;
  }

  auto firstPage = start & ~(FRAME_SIZE - 1);
  auto lastPage = last & ~(FRAME_SIZE - 1);

  for (auto va = firstPage;; va += FRAME_SIZE) {
    if (!ensureMappedPage(va)) {
      return false;
    }
    if (va == lastPage) {
      break;
    }
  }

  return true;
}

void VMM::init_heap_break(uint64_t initial_break) {
  auto canonical = initial_break;
  if (canonical < 0x1000) {
    canonical = 0x1000;
  }
  impl::heapStart = canonical;
  impl::heapBreak = canonical;
}

uint64_t VMM::get_heap_break() { return impl::heapBreak; }

int VMM::set_heap_break(uint64_t new_break) {
  using namespace impl;

  if (heapStart == 0 || heapBreak == 0) {
    return -1;
  }

  if (new_break < heapStart || new_break >= UINT64_C(0x0000800000000000)) {
    return -1;
  }

  auto old_break = heapBreak;
  auto old_end = roundUpPage(old_break);
  auto new_end = roundUpPage(new_break);

  if (new_end > old_end) {
    map_range(VA(old_end), new_end - old_end, true, true);
  } else if (new_end < old_end) {
    unmapRange(new_end, old_end);
  }

  heapBreak = new_break;
  return 0;
}

bool VMM::snapshot_for_fork(ForkState &out) {
  using namespace impl;

  auto parentCr3Ppn = PPN(get_cr3() >> LOG_FRAME_SIZE);
  auto parentPml4 = (uint64_t *)VA(parentCr3Ppn);

  auto childCr3Ppn = physMem.alloc();
  auto childPml4 = (uint64_t *)VA(childCr3Ppn);

  for (uint64_t i = 0; i < 512; i++) {
    childPml4[i] = 0;
  }

  for (uint64_t i = 0; i < 256; i++) {
    auto entry = parentPml4[i];
    if ((entry & presentBit) == 0) {
      continue;
    }
    auto child = cloneTableSubtree(entryPpn(entry), 2);
    childPml4[i] = (child.ppn() << LOG_FRAME_SIZE) | (entry & ~addressMask);
  }

  for (uint64_t i = 256; i < 512; i++) {
    childPml4[i] = parentPml4[i];
  }

  out.cr3 = PA(childCr3Ppn).pa();
  out.private_vmes = clonePrivateVmes();
  out.heap_start = heapStart;
  out.heap_break = heapBreak;
  return true;
}

void VMM::install_fork_state(const ForkState &state) {
  impl::privateVmes = (impl::VME *)state.private_vmes;
  impl::heapStart = state.heap_start;
  impl::heapBreak = state.heap_break;
}

/*
 * A simplified mmap implementation
 */

void *VMM::mmap(void *addr, size_t length, int prot, int flags,
                StrongRef<Node> file, uint64_t offset) {
  (void)addr;
  (void)prot;

  const bool isShared = (flags & MAP_SHARED) != 0;
  const bool isAnonymous = (flags & MAP_ANONYMOUS) != 0;

  if (length == 0) {
    return (void *)-1;
  }

  if ((offset % FRAME_SIZE) != 0) {
    return (void *)-1;
  }

  if (isAnonymous && !(file == StrongRef<Node>{})) {
    return (void *)-1;
  }

  if (!isAnonymous && (file == StrongRef<Node>{})) {
    return (void *)-1;
  }

  length = impl::roundUpPage(length);

  if (isShared) {
    using namespace impl;

    uint64_t candidateVa = sharedStart();
    sharedLock.lock();

    auto nextVme = sharedVmes;
    VME **prevNextLink = &sharedVmes;
    while (nextVme != nullptr) {
      if (candidateVa + length <= nextVme->start) {
        break;
      }
      candidateVa = nextVme->end;
      prevNextLink = &nextVme->next;
      nextVme = nextVme->next;
    }

    if (candidateVa > sharedEnd || (sharedEnd - candidateVa) < length) {
      sharedLock.unlock();
      return (void *)-1;
    }

    auto vme = new VME();
    vme->start = candidateVa;
    vme->end = candidateVa + length;
    vme->shared = true;
    vme->node = file;
    vme->offset = offset;
    vme->next = nextVme;
    *prevNextLink = vme;

    sharedLock.unlock();
    return (void *)candidateVa;
  }

  uint64_t candidateVa = 0x1000;
  using namespace impl;
  auto nextVme = privateVmes;
  VME **prevNextLink = &privateVmes;
  while (nextVme != nullptr) {
    if (candidateVa + length <= nextVme->start) {
      break;
    }
    candidateVa = nextVme->end;
    prevNextLink = &nextVme->next;
    nextVme = nextVme->next;
  }

  auto vme = new VME();
  vme->start = candidateVa;
  vme->end = candidateVa + length;
  vme->shared = false;
  vme->node = file;
  vme->offset = offset;
  vme->next = nextVme;
  *prevNextLink = vme;
  return (void *)candidateVa;
}

int VMM::munmap(void *addr, size_t length) {
  using namespace impl;

  auto start = uint64_t(addr);
  if ((start % FRAME_SIZE) != 0) {
    return -1;
  }
  if (length == 0) {
    return -1;
  }
  if (isSharedAddress(start)) {
    return -1;
  }

  auto len = roundUpPage(length);
  auto end = start + len;
  if (end < start) {
    return -1;
  }

  bool touched = false;
  VME **vmeLink = &privateVmes;

  while (*vmeLink != nullptr) {
    auto vme = *vmeLink;

    if (end <= vme->start) {
      break;
    }

    if (start >= vme->end) {
      vmeLink = &vme->next;
      continue;
    }

    if (vme->shared) {
      return -1;
    }

    auto overlap_start = (start > vme->start) ? start : vme->start;
    auto overlap_end = (end < vme->end) ? end : vme->end;
    if (overlap_start >= overlap_end) {
      vmeLink = &vme->next;
      continue;
    }

    touched = true;
    unmapRange(overlap_start, overlap_end);

    if ((overlap_start == vme->start) && (overlap_end == vme->end)) {
      *vmeLink = vme->next;
      delete vme;
      continue;
    }
    if (overlap_start == vme->start) {
      vme->start = overlap_end;
      vme->offset += (overlap_end - overlap_start);
      vmeLink = &vme->next;
      continue;
    }

    if (overlap_end == vme->end) {
      vme->end = overlap_start;
      vmeLink = &vme->next;
      continue;
    }

    auto right = new VME();
    right->start = overlap_end;
    right->end = vme->end;
    
    right->shared = vme->shared;
    right->node = vme->node;
    right->offset = vme->offset + (overlap_end - vme->start);
    right->next = vme->next;

    vme->end = overlap_start;
    vme->next = right;
    vmeLink = &right->next;
  }

  return touched ? 0 : -1;
}

extern "C" [[gnu::force_align_arg_pointer]] void
pageFaultHandler(uintptr_t cr2, impl::PageFaultTrapFrame *trap_frame) {
  using namespace impl;

  ASSERT(!is_disabled());

  auto faultPage = cr2 & ~(FRAME_SIZE - 1);
  if (ensureMappedPage(faultPage)) {
    return;
  }

  ASSERT(false);
}

void VMM::init_system() {
  impl::common_cr3 = get_cr3();
  IDT::trap(14, uintptr_t(pageFaultHandler_), 0);
}

void VMM::init_core() {}

void VMM::init_thread() {
  PPN newCr3Ppn = physMem.alloc();
  PA newCr3Pa = newCr3Ppn;

  uint64_t *newTable = VA(newCr3Ppn);
  uint64_t *oldTable = VA(PPN(impl::common_cr3 >> 12));

  for (uint64_t i = 512 / 2; i < 512; i++) {
    newTable[i] = oldTable[i];
  }

  set_cr3(newCr3Pa.pa());
}

void VMM::fini_thread() {
  using namespace impl;

  auto oldCr3Ppn = PPN(get_cr3() >> LOG_FRAME_SIZE);

  auto vmeCursor = privateVmes;
  privateVmes = nullptr;
  while (vmeCursor != nullptr) {
    unmapRange(vmeCursor->start, vmeCursor->end);
    auto nextVme = vmeCursor->next;
    delete vmeCursor;
    vmeCursor = nextVme;
  }

  if (n_active.get() == 1) {
    sharedLock.lock();
    auto sharedCursor = sharedVmes;
    sharedVmes = nullptr;
    sharedLock.unlock();

    while (sharedCursor != nullptr) {
      auto nextShared = sharedCursor->next;
      delete sharedCursor;
      sharedCursor = nextShared;
    }
  }

  if (oldCr3Ppn.ppn() == (common_cr3 >> LOG_FRAME_SIZE)) {
    return;
  }

  auto pml4 = (uint64_t *)VA(oldCr3Ppn);
  for (uint64_t i = 0; i < 256; i++) {
    auto entry = pml4[i];
    if ((entry & presentBit) == 0) {
      continue;
    }
    auto child = entryPpn(entry);
    freeTableSubtree(child, 2);
    physMem.free(child);
    pml4[i] = 0;
  }

  set_cr3(common_cr3);
  physMem.free(oldCr3Ppn);
}
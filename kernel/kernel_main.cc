#include "elf.h"
#include "ext2.h"
#include "per_core.h"
#include "print.h"
#include "ramdisk.h"
#include "thread.h"
#include "vmm.h"
#include "x86_64.h"

void kernel_main() {
  StrongRef<BlockIO> ide{new RamDisk("/boot/ramdisk", 0)};
  auto fs = StrongRef<Ext2>::make(ide);

  KPRINT("block size is ?\n", Dec(fs->get_block_size()));
  KPRINT("inode size is ?\n", Dec(fs->get_inode_size()));

  auto init = fs->find(fs->root, "init");

  auto entry = ELF::load(init);
  KPRINT("entry: ?\n", entry);

  auto rsp = UINT64_C(0x7ffffff0000);

  const uint64_t stack_size = 64 * 1024;

  auto limit = rsp - stack_size;
  impl::map_range(VA(limit), stack_size, true, true);

  KPRINT("STAR = ?\n", Msr::IA32_STAR{}.read());
  KPRINT("LSTAR = ?\n", Msr::IA32_LSTAR{}.read());
  KPRINT("FMASK = ?\n", Msr::I32_FMASK{}.read());

  auto me = impl::TCB::current();
  auto stack_bottom = me->stack_bottom;
  disable();
  PerCore::get()->tss.rsp0 = (uint64_t)stack_bottom;

  switch_to_user(entry, rsp);
}

#include "elf.h"
#include "debug.h"
#include "shared.h"
#include "vmm.h"

uint64_t ELF::load(StrongRef<Node> file, uint64_t* end_addr) {
  ElfHeader hdr;

  file->read(0, hdr);

  uint32_t hoff = hdr.phoff;
  uint64_t max_end = 0;

  KPRINT("phnum = ?\n", hdr.phnum);

  for (uint32_t i = 0; i < hdr.phnum; i++) {
    ProgramHeader phdr;
    file->read(hoff, phdr);
    hoff += hdr.phentsize;

    if (phdr.type == 1) {
      char *p = (char *)phdr.vaddr;
      uint32_t memsz = phdr.memsz;
      uint32_t filesz = phdr.filesz;

      auto seg_end = phdr.vaddr + phdr.memsz;

      if (seg_end > max_end) {
        max_end = seg_end;
      }

      impl::map_range(VA(uint64_t(p)), phdr.memsz, true, true);

            KPRINT("vaddr:? memsz:? filesz:? fileoff:?\n", uint64_t(p), memsz, filesz,
              phdr.offset);
      uint32_t n = file->read_all(phdr.offset, filesz, p);
      ASSERT(n == filesz);
    }
  }

  if (end_addr != nullptr) {
    *end_addr = max_end;
  }

  return hdr.entry;
}

#include "elf.h"
#include "debug.h"
#include "shared.h"
#include "vmm.h"

uint64_t ELF::load(StrongRef<Node> file) {
  ElfHeader hdr;

  file->read(0, hdr);

  uint32_t hoff = hdr.phoff;

  KPRINT("phnum = ?\n", hdr.phnum);

  for (uint32_t i = 0; i < hdr.phnum; i++) {
    ProgramHeader phdr;
    file->read(hoff, phdr);
    hoff += hdr.phentsize;

    if (phdr.type == 1) {
      char *p = (char *)phdr.vaddr;
      uint32_t memsz = phdr.memsz;
      uint32_t filesz = phdr.filesz;

      impl::map_range(VA(uint64_t(p)), phdr.memsz, true, true);

      KPRINT("vaddr:? memsz:? filesz:? fileoff:?\n", uint64_t(p), memsz, filesz,
          phdr.offset);
      uint32_t n = file->read_all(phdr.offset, filesz, p);
      ASSERT(n == filesz);
    }
  }

  return hdr.entry;
}

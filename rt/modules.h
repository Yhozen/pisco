// The images the lifter lifted, in load order: the title's exe first, then the DLLs it ships.
#pragma once
typedef struct { unsigned long long va, off; unsigned len, vsize; } ModuleSection;
typedef struct {
  const char *name;                 // file name in the title's folder
  unsigned long long base, file_base, size;
  unsigned is_dll;
  unsigned long long entry, tls_dir, pdata; unsigned pdata_size;
  unsigned long long exports, stack_reserve;
  unsigned flags;
  const ModuleSection *sections; unsigned section_count;
  const unsigned long long *relocs; unsigned reloc_count;
  unsigned first_import, import_count;
} ModuleDef;

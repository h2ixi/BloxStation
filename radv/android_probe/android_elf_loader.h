/*
 * A minimal Bionic-style ELF loader for Android x86_64 shared libraries, for
 * the PS5 Android-loading work. It maps PT_LOAD segments into executable
 * memory, applies x86_64 RELA relocations (RELATIVE, GLOB_DAT, JUMP_SLOT, 64)
 * from DT_RELA, DT_JMPREL and Android's packed DT_ANDROID_RELA ("APS2"),
 * resolves imports from a host table (then an optional resolver callback,
 * e.g. one that hands out logging trap stubs), runs DT_INIT / DT_INIT_ARRAY,
 * and looks exports up by DT_HASH/DT_GNU_HASH.
 *
 * Header-only so the PS5 title and the host test share it. The caller
 * supplies the allocator for executable memory (ps5_exec_allocate on the
 * console, mmap on the host) and a log callback.
 */
#ifndef ANDROID_ELF_LOADER_H
#define ANDROID_ELF_LOADER_H

#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned char e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
   uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags; uint16_t e_ehsize, e_phentsize, e_phnum,
   e_shentsize, e_shnum, e_shstrndx; } ael_ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
   p_align; } ael_phdr;
typedef struct { int64_t d_tag; uint64_t d_val; } ael_dyn;
typedef struct { uint32_t st_name; unsigned char st_info, st_other; uint16_t st_shndx;
   uint64_t st_value, st_size; } ael_sym;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } ael_rela;

enum { AEL_PT_LOAD = 1, AEL_PT_DYNAMIC = 2, AEL_PT_TLS = 7 };
enum { AEL_DT_NULL = 0, AEL_DT_NEEDED = 1, AEL_DT_PLTRELSZ = 2, AEL_DT_HASH = 4, AEL_DT_STRTAB = 5,
   AEL_DT_SYMTAB = 6, AEL_DT_RELA = 7, AEL_DT_RELASZ = 8, AEL_DT_INIT = 12, AEL_DT_SONAME = 14,
   AEL_DT_JMPREL = 23, AEL_DT_INIT_ARRAY = 25, AEL_DT_INIT_ARRAYSZ = 27, AEL_DT_GNU_HASH = 0x6ffffef5,
   AEL_DT_ANDROID_RELA = 0x60000011, AEL_DT_ANDROID_RELASZ = 0x60000012 };
enum { AEL_R_NONE = 0, AEL_R_64 = 1, AEL_R_GLOB_DAT = 6, AEL_R_JUMP_SLOT = 7, AEL_R_RELATIVE = 8 };
enum { AEL_STB_WEAK = 2 };

struct ael_import { const char *name; void *address; };

typedef void *(*ael_alloc_fn)(size_t bytes);
typedef void (*ael_log_fn)(const char *line);

struct ael_library;

struct ael_options {
   const struct ael_import *imports; /* NULL-terminated, searched first */
   /* Then this, for names the table lacks (NULL leaves them missing). */
   void *(*resolve)(const char *name, void *ctx);
   void *resolve_ctx;
   ael_alloc_fn alloc;
   ael_log_fn log;
   bool skip_constructors;
   /* Test hook: every relocation as decoded, before it is applied. */
   void (*on_reloc)(const ael_rela *r, void *ctx);
   void *on_reloc_ctx;
   /* Called once the image is relocated, before its constructors run (for
    * dl_iterate_phdr, which C++ exceptions need while constructors run). */
   void (*before_constructors)(const struct ael_library *lib);
   /* Optional: replace a bound import's address (e.g. with a tracing thunk). */
   void *(*wrap)(const char *name, void *address);
};

struct ael_library {
   uint8_t *base;   /* load bias: address of vaddr 0 */
   size_t span;
   const ael_sym *symtab;
   const char *strtab;
   uint32_t nsyms;
   const char *soname;
   unsigned needed, relocs, packed, imports, resolved, weak_null, missing;
   uint64_t init, init_array, init_arraysz;
   const void *phdr; /* the program headers as mapped (the first PT_LOAD holds them) */
   uint16_t phnum;
};

static void
ael_say(ael_log_fn log, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void
ael_say(ael_log_fn log, const char *fmt, ...)
{
   char line[400];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(line, sizeof(line), fmt, ap);
   va_end(ap);
   if (log)
      log(line);
}

static uint32_t
ael_gnu_hash_count(const uint32_t *gh)
{
   const uint32_t nbuckets = gh[0], symoffset = gh[1], bloom_size = gh[2];
   const uint64_t *bloom = (const uint64_t *)(gh + 4);
   const uint32_t *buckets = (const uint32_t *)(bloom + bloom_size);
   const uint32_t *chain = buckets + nbuckets;
   uint32_t last = 0;
   for (uint32_t i = 0; i < nbuckets; i++)
      if (buckets[i] > last)
         last = buckets[i];
   if (last < symoffset)
      return symoffset;
   while ((chain[last - symoffset] & 1) == 0)
      last++;
   return last + 1;
}

static void *
ael_find_import(const struct ael_import *imports, const char *name)
{
   for (const struct ael_import *i = imports; i && i->name; i++)
      if (strcmp(i->name, name) == 0)
         return i->address;
   return NULL;
}

/* One relocation. 0, or a negative error. */
static int
ael_reloc_one(struct ael_library *lib, const ael_rela *r, const struct ael_options *o)
{
   if (o->on_reloc)
      o->on_reloc(r, o->on_reloc_ctx);
   const uint32_t type = (uint32_t)r->r_info, symi = (uint32_t)(r->r_info >> 32);
   if (type == AEL_R_NONE)
      return 0;
   if (r->r_offset + 8 > lib->span) {
      ael_say(o->log, "elf: relocation at 0x%llx outside the image", (unsigned long long)r->r_offset);
      return -9;
   }
   uint64_t *where = (uint64_t *)(lib->base + r->r_offset);
   if (type == AEL_R_RELATIVE) {
      *where = (uint64_t)(uintptr_t)lib->base + (uint64_t)r->r_addend;
      lib->relocs++;
      return 0;
   }
   if (lib->nsyms && symi >= lib->nsyms) {
      ael_say(o->log, "elf: relocation symbol %u past the table", symi);
      return -12;
   }
   const ael_sym *s = &lib->symtab[symi];
   const char *name = lib->strtab + s->st_name;
   uint64_t value = 0;
   if (s->st_shndx != 0) {
      value = (uint64_t)(uintptr_t)lib->base + s->st_value;
   } else {
      void *address = ael_find_import(o->imports, name);
      if (address) {
         lib->imports++;
         /* Only functions: a data import (stdout, __stack_chk_guard, ...)
          * must point at the data itself. */
         if (o->wrap && (type == AEL_R_JUMP_SLOT || (s->st_info & 0xf) == 2))
            address = o->wrap(name, address);
      } else if ((s->st_info >> 4) == AEL_STB_WEAK) {
         /* An optional symbol nothing provides is null, as Bionic's linker
          * leaves it -- never a trap thunk, which code would mistake for
          * the real thing (e.g. __start_/__stop_ section bounds). */
         lib->weak_null++;
      } else if (o->resolve && (address = o->resolve(name, o->resolve_ctx))) {
         lib->resolved++;
      } else {
         ael_say(o->log, "elf: missing import %s", name);
         lib->missing++;
         return 0;
      }
      value = (uint64_t)(uintptr_t)address;
   }
   switch (type) {
   case AEL_R_64: *where = value + (uint64_t)r->r_addend; break;
   case AEL_R_GLOB_DAT:
   case AEL_R_JUMP_SLOT: *where = value; break;
   default:
      ael_say(o->log, "elf: unsupported relocation type %u for %s", type, name);
      return -10;
   }
   lib->relocs++;
   return 0;
}

/* Android's packed relocations ("APS2", bionic's packed reloc iterator):
 * SLEB128s -- the count, the first offset, then groups of relocations that
 * share an offset delta, an info word or an addend. */
struct ael_sleb {
   const uint8_t *p, *end;
   bool bad;
};

static int64_t
ael_sleb_next(struct ael_sleb *d)
{
   int64_t value = 0;
   unsigned shift = 0;
   uint8_t byte;
   do {
      if (d->p >= d->end || shift >= 64) {
         d->bad = true;
         return 0;
      }
      byte = *d->p++;
      value |= (int64_t)(byte & 0x7f) << shift;
      shift += 7;
   } while (byte & 0x80);
   if (shift < 64 && (byte & 0x40))
      value |= -((int64_t)1 << shift);
   return value;
}

static int
ael_apply_packed(struct ael_library *lib, const uint8_t *data, size_t bytes, const struct ael_options *o)
{
   enum { BY_INFO = 1, BY_OFFSET_DELTA = 2, BY_ADDEND = 4, HAS_ADDEND = 8 };
   if (bytes < 4 || memcmp(data, "APS2", 4) != 0) {
      ael_say(o->log, "elf: packed relocations without the APS2 magic");
      return -13;
   }
   struct ael_sleb d = {data + 4, data + bytes, false};
   const int64_t count = ael_sleb_next(&d);
   ael_rela r = {.r_offset = (uint64_t)ael_sleb_next(&d)};
   int64_t done = 0;
   while (done < count && !d.bad) {
      const int64_t group = ael_sleb_next(&d);
      const int64_t flags = ael_sleb_next(&d);
      int64_t offset_delta = 0;
      if (flags & BY_OFFSET_DELTA)
         offset_delta = ael_sleb_next(&d);
      if (flags & BY_INFO)
         r.r_info = (uint64_t)ael_sleb_next(&d);
      if ((flags & HAS_ADDEND) && (flags & BY_ADDEND))
         r.r_addend += ael_sleb_next(&d);
      else if (!(flags & HAS_ADDEND))
         r.r_addend = 0;
      if (d.bad || group <= 0 || group > count - done) {
         d.bad = true;
         break;
      }
      for (int64_t i = 0; i < group && !d.bad; i++) {
         r.r_offset += (flags & BY_OFFSET_DELTA) ? (uint64_t)offset_delta : (uint64_t)ael_sleb_next(&d);
         if (!(flags & BY_INFO))
            r.r_info = (uint64_t)ael_sleb_next(&d);
         if ((flags & HAS_ADDEND) && !(flags & BY_ADDEND))
            r.r_addend += ael_sleb_next(&d);
         if (d.bad)
            break;
         const int rc = ael_reloc_one(lib, &r, o);
         if (rc)
            return rc;
         lib->packed++;
      }
      done += group;
   }
   if (d.bad || done != count) {
      ael_say(o->log, "elf: packed relocations malformed after %lld of %lld", (long long)done, (long long)count);
      return -14;
   }
   return 0;
}

/* Loads image (size bytes, an ET_DYN x86_64 ELF). 0 on success. */
static int
ael_load_ex(const uint8_t *image, size_t size, const struct ael_options *o, struct ael_library *lib)
{
   memset(lib, 0, sizeof(*lib));
   if (size < sizeof(ael_ehdr))
      return -1;
   const ael_ehdr *eh = (const ael_ehdr *)image;
   if (memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0 || eh->e_ident[4] != 2 || eh->e_ident[5] != 1) {
      ael_say(o->log, "elf: not a 64-bit little-endian ELF");
      return -2;
   }
   if (eh->e_type != 3 || eh->e_machine != 62) {
      ael_say(o->log, "elf: type %u machine %u, want ET_DYN x86_64", eh->e_type, eh->e_machine);
      return -3;
   }
   if (eh->e_phentsize != sizeof(ael_phdr) || eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(ael_phdr) > size) {
      ael_say(o->log, "elf: bad program headers");
      return -4;
   }
   const ael_phdr *ph = (const ael_phdr *)(image + eh->e_phoff);
   uint64_t lo = UINT64_MAX, hi = 0;
   const ael_phdr *dynamic = NULL;
   for (unsigned i = 0; i < eh->e_phnum; i++) {
      if (ph[i].p_type == AEL_PT_LOAD) {
         if (ph[i].p_offset + ph[i].p_filesz > size || ph[i].p_filesz > ph[i].p_memsz) {
            ael_say(o->log, "elf: segment %u outside the file", i);
            return -5;
         }
         const uint64_t start = ph[i].p_vaddr & ~(uint64_t)0xfff;
         const uint64_t end = (ph[i].p_vaddr + ph[i].p_memsz + 0xfff) & ~(uint64_t)0xfff;
         if (start < lo) lo = start;
         if (end > hi) hi = end;
      } else if (ph[i].p_type == AEL_PT_DYNAMIC) {
         dynamic = &ph[i];
      } else if (ph[i].p_type == AEL_PT_TLS) {
         ael_say(o->log, "elf: PT_TLS is not supported yet");
         return -15;
      }
   }
   if (lo != 0 || hi == 0 || !dynamic) {
      ael_say(o->log, "elf: lowest vaddr 0x%llx, span 0x%llx, dynamic %s", (unsigned long long)lo,
              (unsigned long long)hi, dynamic ? "yes" : "no");
      return -6;
   }
   lib->span = hi;
   lib->base = o->alloc(hi);
   if (!lib->base) {
      ael_say(o->log, "elf: no executable memory for 0x%zx bytes", lib->span);
      return -7;
   }
   memset(lib->base, 0, hi);
   for (unsigned i = 0; i < eh->e_phnum; i++)
      if (ph[i].p_type == AEL_PT_LOAD)
         memcpy(lib->base + ph[i].p_vaddr, image + ph[i].p_offset, ph[i].p_filesz);
   ael_say(o->log, "elf: mapped 0x%zx bytes at %p (executable)", lib->span, (void *)lib->base);
   lib->phdr = lib->base + eh->e_phoff; /* the first PT_LOAD starts at file offset 0 */
   lib->phnum = eh->e_phnum;

   const ael_dyn *dyn = (const ael_dyn *)(lib->base + dynamic->p_vaddr);
   uint64_t rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0, packed = 0, packedsz = 0;
   uint64_t soname = 0, needed[16];
   const uint32_t *hash = NULL, *gnu_hash = NULL;
   for (; dyn->d_tag != AEL_DT_NULL; dyn++) {
      switch (dyn->d_tag) {
      case AEL_DT_NEEDED: if (lib->needed < 16) needed[lib->needed] = dyn->d_val; lib->needed++; break;
      case AEL_DT_STRTAB: lib->strtab = (const char *)(lib->base + dyn->d_val); break;
      case AEL_DT_SYMTAB: lib->symtab = (const ael_sym *)(lib->base + dyn->d_val); break;
      case AEL_DT_HASH: hash = (const uint32_t *)(lib->base + dyn->d_val); break;
      case AEL_DT_GNU_HASH: gnu_hash = (const uint32_t *)(lib->base + dyn->d_val); break;
      case AEL_DT_RELA: rela = dyn->d_val; break;
      case AEL_DT_RELASZ: relasz = dyn->d_val; break;
      case AEL_DT_JMPREL: jmprel = dyn->d_val; break;
      case AEL_DT_PLTRELSZ: pltrelsz = dyn->d_val; break;
      case AEL_DT_ANDROID_RELA: packed = dyn->d_val; break;
      case AEL_DT_ANDROID_RELASZ: packedsz = dyn->d_val; break;
      case AEL_DT_INIT: lib->init = dyn->d_val; break;
      case AEL_DT_INIT_ARRAY: lib->init_array = dyn->d_val; break;
      case AEL_DT_INIT_ARRAYSZ: lib->init_arraysz = dyn->d_val; break;
      case AEL_DT_SONAME: soname = dyn->d_val; break;
      default: break;
      }
   }
   if (!lib->strtab || !lib->symtab) {
      ael_say(o->log, "elf: no dynamic symbol table");
      return -8;
   }
   lib->soname = soname ? lib->strtab + soname : "(none)";
   lib->nsyms = hash ? hash[1] : gnu_hash ? ael_gnu_hash_count(gnu_hash) : 0;
   for (unsigned i = 0; i < lib->needed && i < 16; i++)
      ael_say(o->log, "elf: DT_NEEDED %s", lib->strtab + needed[i]);

   if (packed) {
      const int rc = ael_apply_packed(lib, lib->base + packed, packedsz, o);
      if (rc)
         return rc;
   }
   const struct { uint64_t at, bytes; } tables[2] = {{rela, relasz}, {jmprel, pltrelsz}};
   for (int t = 0; t < 2; t++) {
      const ael_rela *r = (const ael_rela *)(lib->base + tables[t].at);
      const size_t n = tables[t].at ? tables[t].bytes / sizeof(ael_rela) : 0;
      for (size_t i = 0; i < n; i++) {
         const int rc = ael_reloc_one(lib, &r[i], o);
         if (rc)
            return rc;
      }
   }
   ael_say(o->log,
           "elf: %u relocations (%u packed), %u imports from the table, %u resolved, %u weak left null, %u missing",
           lib->relocs, lib->packed, lib->imports, lib->resolved, lib->weak_null, lib->missing);
   if (lib->missing)
      return -11;
   if (o->before_constructors)
      o->before_constructors(lib);
   if (o->skip_constructors)
      return 0;
   if (lib->init)
      ((void (*)(void))(lib->base + lib->init))();
   if (lib->init_array) {
      void (**fns)(void) = (void (**)(void))(lib->base + lib->init_array);
      for (size_t i = 0; i < lib->init_arraysz / sizeof(void *); i++)
         if (fns[i] && (uintptr_t)fns[i] != (uintptr_t)-1)
            fns[i]();
   }
   ael_say(o->log, "elf: constructors ran (DT_INIT %s, %zu in DT_INIT_ARRAY)", lib->init ? "yes" : "no",
           (size_t)(lib->init_arraysz / sizeof(void *)));
   return 0;
}

static int
ael_load(const uint8_t *image, size_t size, const struct ael_import *imports, ael_alloc_fn alloc,
         ael_log_fn log, struct ael_library *lib)
{
   const struct ael_options o = {.imports = imports, .alloc = alloc, .log = log};
   return ael_load_ex(image, size, &o, lib);
}

static void *
ael_symbol(const struct ael_library *lib, const char *name)
{
   for (uint32_t i = 1; i < lib->nsyms; i++) {
      const ael_sym *s = &lib->symtab[i];
      if (s->st_shndx != 0 && strcmp(lib->strtab + s->st_name, name) == 0)
         return lib->base + s->st_value;
   }
   return NULL;
}

#endif

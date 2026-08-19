// Alpha is a 64-bit RISC ISA developed by DEC (Digital Equipment
// Corporation) in the early '90s. It aimed to be an ISA that would last
// 25 years. DEC expected Alpha would become 1000x faster during that time
// span. Since the ISA was developed from scratch for future machines,
// it's 64-bit from the beginning. There's no 32-bit variant.
//
// DEC ported its own Unix (Tru64) to Alpha. Microsoft also ported Windows
// NT to it. But it wasn't a huge commercial success.
//
// DEC was acquired by Compaq in 1997. In the late '90s, Intel and
// Hewlett-Packard were advertising that their upcoming Itanium processor
// would achieve significantly better performance than RISC processors, so
// Compaq decided to discontinue the Alpha processor line to switch to
// Itanium. Itanium resulted in a miserable failure, but it still suceeded
// to wipe out several RISC processors just by promising overly optimistic
// perf numbers. Alpha as an ISA would probably have been fine after 25
// years since its introduction (which is 1992 + 25 = 2017), but the
// company and its market didn't last that long.
//
// From the linker's point of view, there are a few peculiarities in its
// psABI as shown below:
//
//  - Alpha lacks PC-relative memory load/store instructions, so it uses
//    register-relative load/store instructions in position-independent
//    code. Specifically, GP (which is an alias for $r29) is always
//    maintained to refer to .got+0x8000, and global variables' addresses
//    are loaded in a GP-relative manner.
//
//  - It looks like even function addresses are first loaded to register
//    in a GP-relative manner before calling it. We can relax it to
//    convert the instruction sequence with a direct branch instruction,
//    but by default, object files don't use a direct branch to call a
//    function. Therefore, by default, we don't need to create a PLT.
//    Any function call is made by first reading its address from GOT and
//    jump to the address.

#if MOLD_ALPHA

#include "mold.h"

#include <tbb/parallel_for_each.h>

namespace mold {

using E = MOLD_TARGET;

// A 32-bit immediate can be materialized in a register with a "load high"
// and a "load low" instruction sequence. The first instruction sets the
// upper 16 bits in a register, and the second one set the lower 16
// bits. When doing so, they sign-extend an immediate.  Therefore, if the
// 15th bit of an immediate happens to be 1, setting a "low half" value
// negates the upper 16 bit values that has already been set in a
// register. To compensate that, we need to add 0x8000 when setting the
// upper 16 bits.
static u32 hi(u32 val) {
  return bits(val + 0x8000, 31, 16);
}

template <>
void write_plt_header(Context<E> &ctx, u8 *buf) {}

template <>
void write_plt_entry(Context<E> &ctx, u8 *buf, Symbol<E> &sym) {}

template <>
void write_pltgot_entry(Context<E> &ctx, u8 *buf, Symbol<E> &sym) {}

template <>
void EhFrameSection<E>::apply_eh_reloc(Context<E> &ctx, const ElfRel<E> &rel,
                                       u64 offset, u64 val) {
  u8 *loc = ctx.buf + this->shdr.sh_offset + offset;

  switch (rel.r_type) {
  case R_NONE:
    break;
  case R_ALPHA_SREL32:
    *(ul32 *)loc = val - this->shdr.sh_addr - offset;
    break;
  default:
    Fatal(ctx) << "unsupported relocation in .eh_frame: " << rel;
  }
}

template <>
void InputSection<E>::apply_reloc_alloc(Context<E> &ctx, u8 *base) {
  std::span<const ElfRel<E>> rels = get_rels(ctx);

  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_NONE || record_undef_error(ctx, rel))
      continue;

    Symbol<E> &sym = *file->symbols[rel.r_sym];
    u8 *loc = base + rel.r_offset;

    u64 S = sym.get_addr(ctx);
    u64 A = rel.r_addend;
    u64 P = get_addr() + rel.r_offset;
    u64 GP = ctx.extra.got->get_gp(ctx, *file);

    auto check = [&](i64 val, i64 lo, i64 hi) {
      check_range(ctx, i, val, lo, hi);
    };

    switch (rel.r_type) {
    case R_ALPHA_GPREL32:
      *(ul32 *)loc = S + A - GP;
      break;
    case R_ALPHA_LITERAL:
      *(ul16 *)loc =
        ctx.extra.got->get_addr(*file, {&sym, (i64)A, ALPHA_GOT_REGULAR}) - GP;
      break;
    case R_ALPHA_BRSGP: {
      // A BRSGP call is a direct branch to a function that is known to
      // use the same GP as the caller. Since a direct branch doesn't set
      // $27 to the callee's address, the callee's `ldgp` prologue would
      // compute a bogus GP, so we have to branch past it. A function
      // that begins with such a prologue is marked STO_ALPHA_STD_GPLOAD,
      // and one that doesn't need GP at all is marked STO_ALPHA_NOPV.
      i64 val = S + A - P - 4;
      if ((sym.esym().alpha_st_other & STO_ALPHA_STD_GPLOAD) ==
          STO_ALPHA_STD_GPLOAD)
        val += 8;

      // The caller resumes with the GP it had before the call, so the
      // callee must be assigned the same GP as we are.
      if (sym.file && !sym.file->is_dso &&
          GP != ctx.extra.got->get_gp(ctx, *(ObjectFile<E> *)&*sym.file))
        Error(ctx) << *this << ": " << sym << ": BRSGP call between input"
                   << " files that are assigned different GP values";

      // The displacement is a 21-bit signed word offset.
      check(val, -(1 << 22), 1 << 22);
      *(ul32 *)loc |= bits(val, 22, 2);
      break;
    }
    case R_ALPHA_GPDISP:
      *(ul16 *)loc = hi(GP - P);
      *(ul16 *)(loc + A) = GP - P;
      break;
    case R_ALPHA_SREL32:
      *(ul32 *)loc = S + A - P;
      break;
    case R_ALPHA_GPRELHIGH:
      *(ul16 *)loc = hi(S + A - GP);
      break;
    case R_ALPHA_GPRELLOW:
      *(ul16 *)loc = S + A - GP;
      break;
    case R_ALPHA_TLSGD:
      *(ul16 *)loc =
        ctx.extra.got->get_addr(*file, {&sym, 0, ALPHA_GOT_TLSGD}) - GP;
      break;
    case R_ALPHA_TLSLDM:
      *(ul16 *)loc =
        ctx.extra.got->get_addr(*file, {nullptr, 0, ALPHA_GOT_TLSLD}) - GP;
      break;
    case R_ALPHA_DTPRELHI:
      *(ul16 *)loc = hi(S + A - ctx.dtp_addr);
      break;
    case R_ALPHA_DTPRELLO:
      *(ul16 *)loc = S + A - ctx.dtp_addr;
      break;
    case R_ALPHA_GOTTPREL:
      *(ul16 *)loc =
        ctx.extra.got->get_addr(*file, {&sym, 0, ALPHA_GOT_GOTTP}) + A - GP;
      break;
    case R_ALPHA_TPRELHI:
      *(ul16 *)loc = hi(S + A - ctx.tp_addr);
      break;
    case R_ALPHA_TPRELLO:
      *(ul16 *)loc = S + A - ctx.tp_addr;
      break;
    case R_ALPHA_REFQUAD:
    case R_ALPHA_LITUSE:
    case R_ALPHA_HINT:
      break;
    default:
      unreachable();
    }
  }
}

template <>
void InputSection<E>::apply_reloc_nonalloc(Context<E> &ctx, u8 *base) {
  std::span<const ElfRel<E>> rels = get_rels(ctx);

  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_NONE || record_undef_error(ctx, rel))
      continue;

    Symbol<E> &sym = *file->symbols[rel.r_sym];
    u8 *loc = base + rel.r_offset;

    SectionFragment<E> *frag;
    i64 frag_addend;
    std::tie(frag, frag_addend) = get_fragment(ctx, rel);

    u64 S = frag ? frag->get_addr(ctx) : sym.get_addr(ctx);
    u64 A = frag ? frag_addend : (i64)rel.r_addend;

    switch (rel.r_type) {
    case R_ALPHA_REFLONG:
      if (std::optional<u64> val = get_tombstone(sym, frag))
        *(ul32 *)loc = *val;
      else
        *(ul32 *)loc = S + A;
      break;
    case R_ALPHA_REFQUAD:
      if (std::optional<u64> val = get_tombstone(sym, frag))
        *(ul64 *)loc = *val;
      else
        *(ul64 *)loc = S + A;
      break;
    default:
      Fatal(ctx) << *this << ": invalid relocation for non-allocated sections: "
                 << rel;
    }
  }
}

template <>
void InputSection<E>::scan_relocations(Context<E> &ctx) {
  assert(shdr().sh_flags & SHF_ALLOC);
  std::span<const ElfRel<E>> rels = get_rels(ctx);

  // GOT entries are collected on a per-file basis because we need to
  // know which file refers to which GOT entries to split the GOT into
  // GP-addressable groups. This function is called only once for each
  // input section, and input sections of the same file are scanned
  // serially, so we don't need to take a lock here.
  auto add_got_entry = [&](AlphaGotEntry ent) {
    file->extra.got_entries.push_back(ent);
  };

  // Alpha has no PLT, so there's no link-time address we can give to an
  // imported function. A PC-relative relocation can't be promoted to a
  // dynamic relocation either, so we have to report an error.
  auto scan_alpha_pcrel = [&](Symbol<E> &sym, const ElfRel<E> &rel) {
    if (sym.is_imported && sym.get_type() == STT_FUNC)
      Error(ctx) << *this << ": " << rel << " against an imported function `"
                 << sym << "' is not supported";
    else
      scan_pcrel(ctx, sym, rel);
  };

  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_NONE || record_undef_error(ctx, rel))
      continue;

    Symbol<E> &sym = *file->symbols[rel.r_sym];

    switch (rel.r_type) {
    case R_ALPHA_LITERAL:
      add_got_entry({&sym, (i64)rel.r_addend, ALPHA_GOT_REGULAR});
      break;
    case R_ALPHA_SREL32:
      scan_alpha_pcrel(sym, rel);
      break;
    case R_ALPHA_BRSGP:
      // A BRSGP call is a direct branch to a function that uses the same
      // GP as the caller. We have no PLT to indirect through, so we can't
      // call an imported function that way. An ifunc has no address until
      // its resolver has run, so it can't be called directly either.
      if (sym.is_imported)
        Error(ctx) << *this << ": " << sym << ": " << rel
                   << " against an imported symbol is not supported";
      else if (sym.is_ifunc())
        Error(ctx) << *this << ": " << sym << ": " << rel
                   << " against an ifunc symbol is not supported";
      else if ((sym.esym().alpha_st_other & STO_ALPHA_STD_GPLOAD) !=
                 STO_ALPHA_NOPV &&
               (sym.esym().alpha_st_other & STO_ALPHA_STD_GPLOAD) !=
                 STO_ALPHA_STD_GPLOAD)
        Error(ctx) << *this << ": " << sym << ": " << rel
                   << " against a symbol without .prologue";
      break;
    case R_ALPHA_TLSGD:
      add_got_entry({&sym, 0, ALPHA_GOT_TLSGD});
      break;
    case R_ALPHA_TLSLDM:
      add_got_entry({nullptr, 0, ALPHA_GOT_TLSLD});
      break;
    case R_ALPHA_GOTTPREL:
      add_got_entry({&sym, 0, ALPHA_GOT_GOTTP});
      break;
    case R_ALPHA_TPRELHI:
    case R_ALPHA_TPRELLO:
      check_tlsle(ctx, sym, rel);
      break;
    case R_ALPHA_REFQUAD:
    case R_ALPHA_GPREL32:
    case R_ALPHA_LITUSE:
    case R_ALPHA_GPDISP:
    case R_ALPHA_HINT:
    case R_ALPHA_GPRELHIGH:
    case R_ALPHA_GPRELLOW:
    case R_ALPHA_DTPRELHI:
    case R_ALPHA_DTPRELLO:
      break;
    default:
      Fatal(ctx) << *this << ": unknown relocation: " << rel;
    }
  }
}

// Alpha lacks PC-relative memory load/store instructions, so all memory
// accesses to global data are made relative to GP. A GOT entry is read
// with a single instruction that takes a 16-bit signed displacement from
// GP, which means that a single GP value can address only 64 KiB worth of
// GOT entries. That's not enough for large programs.
//
// To work around the limitation, we split the GOT into multiple chunks,
// each of which is 64 KiB or smaller, and assign a different GP value to
// each input file. We call each chunk a "GOT group". GNU ld does the same
// thing and calls it "multi-GOT".
//
// Since each input file has its own GP value, a GOT entry that is referred
// to by input files belonging to different groups is duplicated for each
// group.
//
// Note that Alpha's GOT entries are not only for symbol addresses; TLS
// entries are also read relative to GP, so they are managed by this class
// as well. Also note that, unlike other targets, an R_ALPHA_LITERAL
// relocation may refer to a symbol with a non-zero addend, so a GOT entry
// is keyed not only by a symbol but also by an addend.
//
// We use our own GOT section instead of the common GotSection because the
// common one cannot represent duplicated entries.
bool operator<(const AlphaGotEntry &a, const AlphaGotEntry &b) {
  // We sort entries by a key that is unique to each entry so that the
  // output is deterministic. A symbol is identified by its file and its
  // index within the file. A symbol that has no file (i.e. an undefined
  // symbol that is not resolved to any file) is identified by its name,
  // as such a symbol is always a global symbol whose name is unique.
  auto get_key = [](const AlphaGotEntry &e) {
    if (!e.sym)
      return std::tuple((i64)e.kind, (i64)0, (i64)0, (i64)0,
                        std::string_view(), e.addend);
    if (!e.sym->file)
      return std::tuple((i64)e.kind, (i64)1, (i64)0, (i64)0,
                        e.sym->name(), e.addend);
    return std::tuple((i64)e.kind, (i64)2, e.sym->file->priority,
                      (i64)e.sym->sym_idx, std::string_view(), e.addend);
  };
  return get_key(a) < get_key(b);
}

static i64 get_size(std::span<const AlphaGotEntry> entries) {
  i64 sz = 0;
  for (const AlphaGotEntry &e : entries)
    sz += e.size() * sizeof(Word<E>);
  return sz;
}

// Group input files so that all GOT entries referred to by a single file
// are addressable from that file's GP.
void AlphaGotSection::finalize(Context<E> &ctx) {
  tbb::parallel_for_each(ctx.objs, [](ObjectFile<E> *file) {
    std::vector<Entry> &vec = file->extra.got_entries;
    std::sort(vec.begin(), vec.end());
    remove_duplicates(vec);
  });

  std::vector<Entry> cur;

  auto flush = [&] {
    if (!cur.empty()) {
      Group group;
      group.entries = std::move(cur);
      cur.clear();
      groups.push_back(std::move(group));
    }
  };

  for (ObjectFile<E> *file : ctx.objs) {
    std::vector<Entry> &vec = file->extra.got_entries;

    // A file with no GOT entry of its own still needs a GP, because it
    // may have GP-relative relocations or make a BRSGP call that has to
    // agree with the callee's GP. Put it in the group we are currently
    // filling rather than leaving it in group 0.
    if (vec.empty()) {
      file->extra.got_group = groups.size();
      continue;
    }

    std::vector<Entry> merged;
    std::set_union(cur.begin(), cur.end(), vec.begin(), vec.end(),
                   std::back_inserter(merged));

    if (get_size(merged) > GROUP_SIZE) {
      flush();
      merged = vec;

      // A single file needing more than 64 KiB of GOT entries is
      // unlinkable because we can't assign more than one GP to a file.
      if (get_size(merged) > GROUP_SIZE)
        Fatal(ctx) << *file << ": GOT entries referred to by this file exceed"
                   << " 64 KiB; recompile it as multiple smaller files";
    }

    cur = std::move(merged);
    file->extra.got_group = groups.size();
  }
  flush();

  // Assign offsets to GOT entries.
  i64 offset = 0;

  for (Group &group : groups) {
    for (const Entry &e : group.entries)
      if (e.kind == ALPHA_GOT_GOTTP)
        has_gottp = true;

    group.offset = offset;
    group.offsets.resize(group.entries.size());

    for (i64 i = 0; i < group.entries.size(); i++) {
      group.offsets[i] = offset;
      offset += group.entries[i].size() * sizeof(Word<E>);
    }
    group.size = offset - group.offset;
  }

  this->shdr.sh_size = offset;
}

// A GP value is defined as the address of the beginning of a GOT group
// plus 0x8000, so that a 16-bit signed displacement from GP can cover
// the entire 64 KiB group.
u64 AlphaGotSection::get_gp(Context<E> &ctx, ObjectFile<E> &file) const {
  // If this section is empty, it is removed from the output file and its
  // sh_addr is left at zero. GP still has to point somewhere inside the
  // loaded image, though, because GP-relative relocations that don't
  // involve the GOT at all are computed against it. Use the common .got,
  // which always contains at least the reserved first word.
  if (groups.empty())
    return ctx.got->shdr.sh_addr + 0x8000;
  return this->shdr.sh_addr + groups[file.extra.got_group].offset + 0x8000;
}

u64 AlphaGotSection::get_addr(ObjectFile<E> &file, Entry ent) const {
  const Group &group = groups[file.extra.got_group];
  auto it = std::lower_bound(group.entries.begin(), group.entries.end(), ent);
  assert(it != group.entries.end() && *it == ent);
  return this->shdr.sh_addr + group.offsets[it - group.entries.begin()];
}

namespace {
struct Slot {
  i64 offset = 0;
  u64 val = 0;
  i64 r_type = R_NONE;
  Symbol<E> *sym = nullptr;
};
}

// Compute the contents of .alpha_got and the dynamic relocations that
// are needed to fix them up at load-time. This mirrors get_got_entries()
// for the common GOT section.
static std::vector<Slot>
get_slots(Context<E> &ctx, const AlphaGotSection &sec) {
  std::vector<Slot> vec;
  auto add = [&](Slot slot) { vec.push_back(slot); };
  constexpr i64 wordsize = sizeof(Word<E>);

  for (const AlphaGotSection::Group &group : sec.groups) {
    for (i64 i = 0; i < group.entries.size(); i++) {
      const AlphaGotEntry &e = group.entries[i];
      i64 off = group.offsets[i];
      Symbol<E> *sym = e.sym;

      switch (e.kind) {
      case ALPHA_GOT_REGULAR:
        if (sym->is_ifunc()) {
          // An ifunc has no address until its resolver has run, so the
          // slot always has to be filled in at load-time. Alpha has no
          // PLT, so this GOT entry is how the function is called.
          add({off, sym->get_addr(ctx, NO_PLT) + e.addend, E::R_IRELATIVE});
        } else if (sym->is_imported) {
          // R_ALPHA_GLOB_DAT ignores an addend, so we need to use
          // R_ALPHA_REFQUAD if we have a non-zero addend.
          if (e.addend)
            add({off, (u64)e.addend, E::R_ABS, sym});
          else
            add({off, 0, E::R_GLOB_DAT, sym});
        } else if (ctx.arg.pic && sym->is_relative()) {
          add({off, sym->get_addr(ctx, NO_PLT) + e.addend, E::R_RELATIVE});
        } else {
          add({off, sym->get_addr(ctx, NO_PLT) + e.addend});
        }
        break;
      case ALPHA_GOT_TLSGD:
        if (sym->is_imported) {
          add({off, 0, E::R_DTPMOD, sym});
          add({off + wordsize, 0, E::R_DTPOFF, sym});
        } else if (ctx.arg.shared) {
          add({off, 0, E::R_DTPMOD});
          add({off + wordsize, sym->get_addr(ctx) - ctx.dtp_addr});
        } else {
          add({off, 1}); // 1 means the main executable
          add({off + wordsize, sym->get_addr(ctx) - ctx.dtp_addr});
        }
        break;
      case ALPHA_GOT_GOTTP:
        if (sym->is_imported)
          add({off, 0, E::R_TPOFF, sym});
        else if (ctx.arg.shared)
          add({off, sym->get_addr(ctx) - ctx.tls_begin, E::R_TPOFF});
        else
          add({off, sym->get_addr(ctx) - ctx.tp_addr});
        break;
      case ALPHA_GOT_TLSLD:
        if (ctx.arg.shared)
          add({off, 0, E::R_DTPMOD});
        else
          add({off, 1}); // 1 means the main executable
        break;
      }
    }
  }
  return vec;
}

i64 AlphaGotSection::get_num_dynrels(Context<E> &ctx) const {
  i64 n = 0;

  for (const Group &group : groups) {
    for (const Entry &e : group.entries) {
      switch (e.kind) {
      case ALPHA_GOT_REGULAR:
        if (e.sym->is_ifunc() || e.sym->is_imported ||
            (ctx.arg.pic && e.sym->is_relative()))
          n++;
        break;
      case ALPHA_GOT_TLSGD:
        if (e.sym->is_imported)
          n += 2;
        else if (ctx.arg.shared)
          n++;
        break;
      case ALPHA_GOT_GOTTP:
        if (e.sym->is_imported || ctx.arg.shared)
          n++;
        break;
      case ALPHA_GOT_TLSLD:
        if (ctx.arg.shared)
          n++;
        break;
      }
    }
  }
  return n;
}

void AlphaGotSection::write_dynrels(Context<E> &ctx, ElfRel<E> *buf) const {
  for (Slot &slot : get_slots(ctx, *this))
    if (slot.r_type != R_NONE)
      *buf++ = ElfRel<E>(this->shdr.sh_addr + slot.offset, slot.r_type,
                         slot.sym ? slot.sym->get_dynsym_idx(ctx) : 0,
                         slot.val);
}

void AlphaGotSection::copy_buf(Context<E> &ctx) {
  u8 *base = ctx.buf + this->shdr.sh_offset;
  memset(base, 0, this->shdr.sh_size);

  for (Slot &slot : get_slots(ctx, *this))
    if (slot.r_type == R_NONE || ctx.arg.apply_dynamic_relocs)
      *(Word<E> *)(base + slot.offset) = slot.val;
}

} // namespace mold

#endif

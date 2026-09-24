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
//    code. Specifically, GP (which is an alias for $r29) refers to
//    .alpha_got+0x8000, and global variables' addresses are loaded in a
//    GP-relative manner.
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
void EhFrameSection<E>::apply_eh_reloc(Context<E> &ctx, InputSection<E> &isec,
                                       const ElfRel<E> &rel, u64 offset, u64 val) {
  u8 *loc = ctx.buf + this->shdr.sh_offset + offset;
  u64 P = this->shdr.sh_addr + offset;

  auto check = [&](i64 val, i64 lo, i64 hi) {
    check_range(ctx, isec, rel, val, lo, hi);
  };

  switch (rel.r_type) {
  case R_NONE:
    break;
  case R_ALPHA_SREL32:
    check(val - P, -(1LL << 31), 1LL << 31);
    *(ul32 *)loc = val - P;
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
    u64 GP = ctx.extra.got->get_gp(ctx);

    switch (rel.r_type) {
    case R_ALPHA_GPREL32:
      *(ul32 *)loc = S + A - GP;
      break;
    case R_ALPHA_LITERAL:
      *(ul16 *)loc =
        ctx.extra.got->get_addr({&sym, (i64)A, ALPHA_GOT_REGULAR}) - GP;
      break;
    case R_ALPHA_BRSGP:
      *(ul32 *)loc |= bits(S + A - P - 4, 22, 0);
      break;
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
      *(ul16 *)loc = ctx.extra.got->get_addr({&sym, 0, ALPHA_GOT_TLSGD}) - GP;
      break;
    case R_ALPHA_TLSLDM:
      *(ul16 *)loc =
        ctx.extra.got->get_addr({nullptr, 0, ALPHA_GOT_TLSLD}) - GP;
      break;
    case R_ALPHA_DTPRELHI:
      *(ul16 *)loc = hi(S + A - ctx.dtp_addr);
      break;
    case R_ALPHA_DTPRELLO:
      *(ul16 *)loc = S + A - ctx.dtp_addr;
      break;
    case R_ALPHA_GOTTPREL:
      *(ul16 *)loc =
        ctx.extra.got->get_addr({&sym, 0, ALPHA_GOT_GOTTP}) + A - GP;
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

  // Input sections of the same file are scanned serially, so collecting
  // GOT entries per file doesn't need a lock.
  auto add_got_entry = [&](AlphaGotEntry ent) {
    file->extra.got_entries.push_back(ent);
  };

  // Alpha has neither a PLT nor copy relocations, so an imported symbol
  // has no link-time address. A PC-relative relocation can't be promoted
  // to a dynamic relocation either, so we have to report an error.
  auto scan_alpha_pcrel = [&](Symbol<E> &sym, const ElfRel<E> &rel) {
    if (sym.is_imported)
      Error(ctx) << *this << ": " << rel << " against an imported symbol `"
                 << sym << "' is not supported";
    else
      scan_pcrel(ctx, sym, rel);
  };

  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_NONE || record_undef_error(ctx, rel))
      continue;

    Symbol<E> &sym = *file->symbols[rel.r_sym];

    if (sym.is_ifunc())
      Error(ctx) << sym << ": GNU ifunc symbol is not supported on Alpha";

    switch (rel.r_type) {
    case R_ALPHA_LITERAL:
      add_got_entry({&sym, (i64)rel.r_addend, ALPHA_GOT_REGULAR});
      break;
    case R_ALPHA_SREL32:
      scan_alpha_pcrel(sym, rel);
      break;
    case R_ALPHA_BRSGP:
      if (sym.is_imported)
        sym.flags |= NEEDS_PLT;
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

// Alpha lacks PC-relative memory load/store instructions, so a GOT entry
// is read with a single instruction that takes a 16-bit signed
// displacement from GP. GP is .alpha_got+0x8000, so .alpha_got can be up
// to 64 KiB.
//
// Unlike other targets, an R_ALPHA_LITERAL relocation may refer to a
// symbol with a non-zero addend, so a GOT entry is keyed not only by a
// symbol but also by an addend. The common GotSection can't represent
// that, so we keep all GOT entries for Alpha, TLS entries included, in
// this section instead.
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

void AlphaGotSection::finalize(Context<E> &ctx) {
  for (ObjectFile<E> *file : ctx.objs)
    append(entries, file->extra.got_entries);

  std::sort(entries.begin(), entries.end());
  remove_duplicates(entries);

  i64 offset = 0;
  offsets.resize(entries.size());

  for (i64 i = 0; i < entries.size(); i++) {
    if (entries[i].kind == ALPHA_GOT_GOTTP)
      has_gottp = true;
    offsets[i] = offset;
    offset += entries[i].size() * sizeof(Word<E>);
  }

  if (offset > 65536)
    Fatal(ctx) << "GOT is " << offset << " bytes, but GP can address only"
               << " 64 KiB of GOT entries";

  this->shdr.sh_size = offset;
}

u64 AlphaGotSection::get_gp(Context<E> &ctx) const {
  // If this section is empty, it is removed from the output file and its
  // sh_addr is left at zero. GP still has to point somewhere inside the
  // loaded image, though, because GP-relative relocations that don't
  // involve the GOT at all are computed against it. Use the common .got,
  // which always contains at least the reserved first word.
  if (entries.empty())
    return ctx.got->shdr.sh_addr + 0x8000;
  return this->shdr.sh_addr + 0x8000;
}

u64 AlphaGotSection::get_addr(Entry ent) const {
  auto it = std::lower_bound(entries.begin(), entries.end(), ent);
  assert(it != entries.end() && *it == ent);
  return this->shdr.sh_addr + offsets[it - entries.begin()];
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

  for (i64 i = 0; i < sec.entries.size(); i++) {
    const AlphaGotEntry &e = sec.entries[i];
    i64 off = sec.offsets[i];
    Symbol<E> *sym = e.sym;

    switch (e.kind) {
    case ALPHA_GOT_REGULAR:
      if (sym->is_imported) {
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
  return vec;
}

i64 AlphaGotSection::get_num_dynrels(Context<E> &ctx) const {
  i64 n = 0;

  for (const Entry &e : entries) {
    switch (e.kind) {
    case ALPHA_GOT_REGULAR:
      if (e.sym->is_imported || (ctx.arg.pic && e.sym->is_relative()))
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

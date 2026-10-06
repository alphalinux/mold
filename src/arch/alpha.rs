//! Alpha is a 64-bit RISC ISA developed by DEC (Digital Equipment
//! Corporation) in the early '90s. It aimed to be an ISA that would last
//! 25 years. DEC expected Alpha would become 1000x faster during that time
//! span. Since the ISA was developed from scratch for future machines,
//! it's 64-bit from the beginning. There's no 32-bit variant.
//!
//! DEC ported its own Unix (Tru64) to Alpha. Microsoft also ported Windows
//! NT to it. But it wasn't a huge commercial success.
//!
//! DEC was acquired by Compaq in 1997. In the late '90s, Intel and
//! Hewlett-Packard were advertising that their upcoming Itanium processor
//! would achieve significantly better performance than RISC processors, so
//! Compaq decided to discontinue the Alpha processor line to switch to
//! Itanium. Itanium resulted in a miserable failure, but it still succeeded
//! to wipe out several RISC processors just by promising overly optimistic
//! perf numbers. Alpha as an ISA would probably have been fine after 25
//! years since its introduction (which is 1992 + 25 = 2017), but the
//! company and its market didn't last that long.
//!
//! From the linker's point of view, there are a few peculiarities in its
//! psABI as shown below:
//!
//!  - Alpha lacks PC-relative memory load/store instructions, so it uses
//!    register-relative load/store instructions in position-independent
//!    code. Specifically, GP (which is an alias for $r29) points 32 KiB
//!    past the start of the GOT group of the current input file, and
//!    global variables' addresses are loaded in a GP-relative manner. See
//!    [`crate::chunks::alpha_got`] for GOT groups.
//!
//!  - It looks like even function addresses are first loaded to register
//!    in a GP-relative manner before calling it. We can relax it to
//!    convert the instruction sequence with a direct branch instruction,
//!    but by default, object files don't use a direct branch to call a
//!    function. Therefore, by default, we don't need to create a PLT.
//!    Any function call is made by first reading its address from GOT and
//!    jump to the address.

use crate::arch::{Family, Target};
use crate::chunks::alpha_got::{self, AlphaGotEntry, AlphaGotKind};
use crate::chunks::eh_frame;
use crate::context::Context;
use crate::elf::*;
use crate::input_files::FileId;
use crate::input_sections::NonAllocReloc;
use crate::input_sections::{InputSection, check_tlsle, scan_pcrel};
use crate::symbol::Symbol;
use crate::util::bits;
use crate::util::endian::{read_ul32, write_ul16, write_ul32, write_ul64};
use crate::{error, fatal};

#[derive(Clone, Copy, Debug, Default)]
pub struct Alpha;

/// A 32-bit immediate can be materialized in a register with a "load high"
/// and a "load low" instruction sequence. The first instruction sets the
/// upper 16 bits in a register, and the second one set the lower 16
/// bits. When doing so, they sign-extend an immediate. Therefore, if the
/// 15th bit of an immediate happens to be 1, setting a "low half" value
/// negates the upper 16 bit values that has already been set in a
/// register. To compensate that, we need to add 0x8000 when setting the
/// upper 16 bits.
fn hi(val: u64) -> u16 {
    bits(u64::from((val as u32).wrapping_add(0x8000)), 31, 16) as u16
}

fn or32(loc: &mut [u8], v: u64) {
    let cur = read_ul32(loc);
    write_ul32(loc, cur | v as u32);
}

/// The bits of `st_other` that tell how a function sets up its GP.
fn gpload(ctx: &Context<Alpha>, sym: &Symbol) -> u32 {
    u32::from(sym.esym(ctx).alpha_st_other()) & STO_ALPHA_STD_GPLOAD
}

impl Target for Alpha {
    const IS_LITTLE: bool = true;
    type Word = U64<Self>;
    type Sym = Elf64Sym<Self>;
    type Phdr = Elf64Phdr<Self>;
    type Chdr = Elf64Chdr<Self>;
    type Rel = ElfRela<Self>;

    type InputSectionExtra = ();

    const NAME: &'static str = "alpha";
    const FAMILY: Family = Family::Alpha;
    const PAGE_SIZE: u64 = 65536;
    const E_MACHINE: u32 = EM_ALPHA;
    const PLT_HDR_SIZE: u64 = 0;
    const PLT_SIZE: u64 = 0;
    const PLTGOT_SIZE: u64 = 0;
    const TRAP: &'static [u8] = &[0x81, 0x00, 0x00, 0x00]; // call_pal bugchk

    const R_COPY: u32 = R_ALPHA_COPY;
    const R_GLOB_DAT: u32 = R_ALPHA_GLOB_DAT;
    const R_JUMP_SLOT: u32 = R_ALPHA_JMP_SLOT;
    const R_ABS: u32 = R_ALPHA_REFQUAD;
    const R_RELATIVE: u32 = R_ALPHA_RELATIVE;
    const R_IRELATIVE: Option<u32> = Some(R_ALPHA_IRELATIVE);
    const R_DTPOFF: u32 = R_ALPHA_DTPREL64;
    const R_TPOFF: u32 = R_ALPHA_TPREL64;
    const R_DTPMOD: u32 = R_ALPHA_DTPMOD64;
    const R_FUNCALL: &'static [u32] = &[];

    fn rel_to_string(r_type: u32) -> std::borrow::Cow<'static, str> {
        alpha_rel_to_string(r_type)
    }

    // Alpha has no PLT. Functions are always called through the GOT.
    fn write_plt_header(_ctx: &Context<Self>, _buf: &mut [u8]) {}

    fn write_plt_entry(_ctx: &Context<Self>, _buf: &mut [u8], _sym: &Symbol) {}

    fn write_pltgot_entry(_ctx: &Context<Self>, _buf: &mut [u8], _sym: &Symbol) {}

    fn apply_eh_reloc(
        ctx: &Context<Self>,
        isec: &InputSection<Self>,
        rel: &ElfRel<Self>,
        loc: &mut [u8],
        p: u64,
        val: u64,
    ) {
        match rel.r_type() {
            R_NONE => {}
            R_ALPHA_SREL32 => {
                eh_frame::check_range(
                    ctx,
                    isec,
                    rel,
                    val.wrapping_sub(p) as i64,
                    -(1 << 31),
                    1 << 31,
                );
                write_ul32(loc, val.wrapping_sub(p) as u32);
            }
            _ => eh_frame::unsupported::<Self>(rel),
        }
    }

    fn scan_relocations(ctx: &Context<Self>, isec: &InputSection<Self>) {
        debug_assert!(isec.is_alloc());
        let file = &ctx.objs[isec.file.index()];
        let mut got_entries = Vec::new();

        // Alpha has neither a PLT nor copy relocations, so an imported symbol
        // or an ifunc has no link-time address. A PC-relative or GP-relative
        // relocation can't be promoted to a dynamic relocation either, so we
        // have to report an error.
        let scan_alpha_pcrel = |sym: &Symbol, rel: &ElfRel<Self>| {
            if sym.is_imported() {
                error!(
                    "{}: {} against an imported symbol `{}' is not supported",
                    isec.display(file),
                    rel.type_name::<Self>(),
                    sym
                );
            } else if sym.is_ifunc() {
                error!(
                    "{}: {} against an ifunc symbol `{}' is not supported",
                    isec.display(file),
                    rel.type_name::<Self>(),
                    sym
                );
            } else {
                scan_pcrel(ctx, isec, sym, rel);
            }
        };

        // R_ALPHA_IRELATIVE's addend is the resolver's address, so there's no
        // way to add an offset to the resolved address.
        let check_ifunc_addend = |sym: &Symbol, rel: &ElfRel<Self>| {
            if sym.is_ifunc() && rel.r_addend() != 0 {
                error!(
                    "{}: {} against an ifunc symbol `{}' with a non-zero addend is not supported",
                    isec.display(file),
                    rel.type_name::<Self>(),
                    sym
                );
            }
        };

        for rel in isec.rels(file) {
            if rel.r_type() == R_NONE || isec.record_undef_error(ctx, rel) {
                continue;
            }
            let id = file.base.symbols[rel.r_sym() as usize];
            let sym = &ctx.symbols[id];
            let mut add_got_entry = |kind: AlphaGotKind| {
                got_entries.push(AlphaGotEntry { sym: Some(id), addend: rel.r_addend(), kind });
            };

            match rel.r_type() {
                R_ALPHA_LITERAL => {
                    check_ifunc_addend(sym, rel);
                    add_got_entry(AlphaGotKind::Regular);
                }
                R_ALPHA_SREL32 | R_ALPHA_BRADDR | R_ALPHA_GPREL32 | R_ALPHA_GPRELHIGH
                | R_ALPHA_GPRELLOW => scan_alpha_pcrel(sym, rel),
                R_ALPHA_BRSGP => {
                    // A BRSGP call is a direct branch to a function that uses the
                    // same GP as the caller. We have no PLT to indirect through,
                    // so we can't call an imported function that way. An ifunc has
                    // no address until its resolver has run, so it can't be called
                    // directly either. The callee must be marked either
                    // STO_ALPHA_STD_GPLOAD, meaning it begins with an `ldgp`
                    // prologue that we can skip, or STO_ALPHA_NOPV, meaning it
                    // doesn't use GP.
                    let reason = if sym.is_imported() {
                        "an imported symbol is not supported"
                    } else if sym.is_ifunc() {
                        "an ifunc symbol is not supported"
                    } else if !matches!(gpload(ctx, sym), STO_ALPHA_NOPV | STO_ALPHA_STD_GPLOAD) {
                        "a symbol without .prologue"
                    } else {
                        continue;
                    };
                    error!(
                        "{}: {}: {} against {}",
                        isec.display(file),
                        sym,
                        rel.type_name::<Self>(),
                        reason
                    );
                }
                R_ALPHA_TLSGD => add_got_entry(AlphaGotKind::Tlsgd),
                R_ALPHA_TLSLDM => got_entries.push(AlphaGotEntry::TLSLD),
                R_ALPHA_GOTTPREL => add_got_entry(AlphaGotKind::Gottp),
                R_ALPHA_TPRELHI | R_ALPHA_TPRELLO => check_tlsle(ctx, isec, sym, rel),
                R_ALPHA_REFQUAD => check_ifunc_addend(sym, rel),
                R_ALPHA_LITUSE | R_ALPHA_GPDISP | R_ALPHA_HINT | R_ALPHA_DTPRELHI
                | R_ALPHA_DTPRELLO => {}
                _ => fatal!(
                    "{}: unknown relocation: {}",
                    isec.display(file),
                    rel.type_name::<Self>()
                ),
            }
        }

        // Input sections of the same file are scanned serially, so this lock
        // is never contended.
        if !got_entries.is_empty() {
            file.alpha_got_entries.lock().unwrap().extend(got_entries);
        }
    }

    fn apply_reloc_alloc(
        ctx: &Context<Self>,
        isec: &InputSection<Self>,
        rels: &mut [ElfRel<Self>],
        buf: &mut [u8],
    ) {
        let file = &ctx.objs[isec.file.index()];
        let isec_addr = isec.addr(ctx);
        let gp = alpha_got::gp(ctx, file);

        for rel in rels.iter() {
            // scan_relocations skips relocations against undefined symbols, so
            // no GOT entry exists for them. We get here with such a relocation
            // if the link continues past the error with --noinhibit-exec.
            if rel.r_type() == R_NONE
                || Self::is_absrel(rel)
                || isec.record_undef_error_with_file(ctx, file, rel)
            {
                continue;
            }
            let id = file.base.symbols[rel.r_sym() as usize];
            let sym = &ctx.symbols[id];

            let off = rel.r_offset() as usize;
            let s = sym.addr(ctx);
            let a = rel.r_addend() as u64;
            let p = isec_addr + rel.r_offset();
            let sa = s.wrapping_add(a);
            let check = |val: i64, lo: i64, hi: i64| isec.check_range(ctx, rel, val, lo, hi);
            let got = |kind: AlphaGotKind| {
                let ent = AlphaGotEntry { sym: Some(id), addend: rel.r_addend(), kind };
                alpha_got::entry_addr(ctx, file, ent).wrapping_sub(gp)
            };
            let loc = &mut buf[off..];

            match rel.r_type() {
                R_ALPHA_GPREL32 => {
                    let val = sa.wrapping_sub(gp);
                    check(val as i64, -(1 << 31), 1 << 31);
                    write_ul32(loc, val as u32);
                }
                R_ALPHA_LITERAL => write_ul16(loc, got(AlphaGotKind::Regular) as u16),
                R_ALPHA_BRSGP | R_ALPHA_BRADDR => {
                    let mut val = sa.wrapping_sub(p).wrapping_sub(4);

                    if rel.r_type() == R_ALPHA_BRSGP {
                        // A direct branch doesn't set $27 to the callee's
                        // address, so we have to skip the callee's `ldgp`
                        // prologue, which would compute a bogus GP from it.
                        if gpload(ctx, sym) == STO_ALPHA_STD_GPLOAD {
                            val = val.wrapping_add(8);
                        }

                        // The caller resumes with the GP it had before the
                        // call, so the callee must be assigned the same GP as
                        // we are.
                        if let Some(FileId::Obj(callee)) = sym.file()
                            && gp != alpha_got::gp(ctx, &ctx.objs[callee.index()])
                        {
                            error!(
                                "{}: {}: BRSGP call between input files that are assigned different GP values",
                                isec.display(file),
                                sym
                            );
                        }
                    }

                    // The displacement is a 21-bit signed word offset.
                    check(val as i64, -(1 << 22), 1 << 22);
                    or32(loc, bits(val, 22, 2));
                }
                R_ALPHA_GPDISP => {
                    // An ldah/lda pair can materialize a displacement in
                    // [-0x80008000, 0x7fff8000). The addend is not a value to
                    // add but the distance from the ldah to the lda.
                    let val = gp.wrapping_sub(p);
                    check(val as i64, -(1 << 31) - 0x8000, (1 << 31) - 0x8000);
                    write_ul16(loc, hi(val));
                    write_ul16(&mut loc[a as usize..], val as u16);
                }
                R_ALPHA_SREL32 => {
                    let val = sa.wrapping_sub(p);
                    check(val as i64, -(1 << 31), 1 << 31);
                    write_ul32(loc, val as u32);
                }
                R_ALPHA_GPRELHIGH => {
                    let val = sa.wrapping_sub(gp);
                    check(val as i64, -(1 << 31) - 0x8000, (1 << 31) - 0x8000);
                    write_ul16(loc, hi(val));
                }
                R_ALPHA_GPRELLOW => write_ul16(loc, sa.wrapping_sub(gp) as u16),
                R_ALPHA_TLSGD => write_ul16(loc, got(AlphaGotKind::Tlsgd) as u16),
                R_ALPHA_TLSLDM => {
                    let addr = alpha_got::entry_addr(ctx, file, AlphaGotEntry::TLSLD);
                    write_ul16(loc, addr.wrapping_sub(gp) as u16);
                }
                R_ALPHA_DTPRELHI => write_ul16(loc, hi(sa.wrapping_sub(ctx.dtp_addr))),
                R_ALPHA_DTPRELLO => write_ul16(loc, sa.wrapping_sub(ctx.dtp_addr) as u16),
                R_ALPHA_GOTTPREL => write_ul16(loc, got(AlphaGotKind::Gottp) as u16),
                R_ALPHA_TPRELHI => write_ul16(loc, hi(sa.wrapping_sub(ctx.tp_addr))),
                R_ALPHA_TPRELLO => write_ul16(loc, sa.wrapping_sub(ctx.tp_addr) as u16),
                R_ALPHA_LITUSE | R_ALPHA_HINT => {}
                _ => unreachable!(),
            }
        }
    }

    fn apply_reloc_nonalloc(ctx: &Context<Self>, isec: &InputSection<Self>, buf: &mut [u8]) {
        let mut fragment_cache = crate::input_sections::FragmentLookup::default();
        let file = &ctx.objs[isec.file.index()];
        for rel in isec.relocations(ctx) {
            let Some(NonAllocReloc { sym, s, a, frag }) =
                isec.resolve_nonalloc(ctx, file, &rel, &mut fragment_cache)
            else {
                continue;
            };
            let sa = s.wrapping_add(a);
            let tombstone = isec.tombstone(ctx, sym, frag);
            let loc = &mut buf[rel.r_offset() as usize..];

            match rel.r_type() {
                R_ALPHA_REFLONG => write_ul32(loc, tombstone.unwrap_or(sa) as u32),
                R_ALPHA_REFQUAD => write_ul64(loc, tombstone.unwrap_or(sa)),
                _ => fatal!(
                    "{}: invalid relocation for non-allocated sections: {}",
                    isec.display(file),
                    rel.type_name::<Self>()
                ),
            }
        }
    }
}

//! `.alpha_got`, the GOT of DEC Alpha.
//!
//! Alpha lacks PC-relative memory load/store instructions, so all memory
//! accesses to global data are made relative to GP. A GOT entry is read
//! with a single instruction that takes a 16-bit signed displacement from
//! GP, which means that a single GP value can address only 64 KiB worth of
//! GOT entries. That's not enough for large programs.
//!
//! To work around the limitation, we split the GOT into multiple chunks,
//! each of which is 64 KiB or smaller, and assign a different GP value to
//! each input file. We call each chunk a "GOT group". GNU ld does the same
//! thing and calls it "multi-GOT".
//!
//! Since each input file has its own GP value, a GOT entry that is referred
//! to by input files belonging to different groups is duplicated for each
//! group.
//!
//! Note that Alpha's GOT entries are not only for symbol addresses; TLS
//! entries are also read relative to GP, so they are managed here as well.
//! Also note that, unlike other targets, an `R_ALPHA_LITERAL` relocation
//! may refer to a symbol with a non-zero addend, so a GOT entry is keyed
//! not only by a symbol but also by an addend.
//!
//! We use our own GOT section instead of the common `.got` because the
//! common one cannot represent duplicated entries.

use bstr::BStr;
use rayon::prelude::*;

use crate::arch::Target;
use crate::chunks::ChunkHeader;
use crate::context::Context;
use crate::elf::*;
use crate::fatal;
use crate::input_files::ObjectFile;
use crate::symbol::{AddrFlags, SymbolId};

/// The size of the GOT that a single GP value can address.
const GROUP_SIZE: u64 = 65536;

const WORD_SIZE: u64 = 8;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum AlphaGotKind {
    Regular,
    Tlsgd,
    Gottp,
    Tlsld,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AlphaGotEntry {
    /// `None` only for the module ID slot of the local-dynamic TLS model.
    pub sym: Option<SymbolId>,
    pub addend: i64,
    pub kind: AlphaGotKind,
}

impl AlphaGotEntry {
    pub const TLSLD: Self = Self { sym: None, addend: 0, kind: AlphaGotKind::Tlsld };

    fn size(&self) -> u64 {
        match self.kind {
            AlphaGotKind::Tlsgd | AlphaGotKind::Tlsld => WORD_SIZE * 2,
            AlphaGotKind::Regular | AlphaGotKind::Gottp => WORD_SIZE,
        }
    }
}

type Key = (AlphaGotKind, u8, u32, u32, &'static BStr, i64);

/// We sort entries by a key that is unique to each entry so that the
/// output is deterministic. A symbol is identified by its file and its
/// index within the file. A symbol that has no file (i.e. an undefined
/// symbol that is not resolved to any file) is identified by its name, as
/// such a symbol is always a global symbol whose name is unique.
fn key<E: Target>(ctx: &Context<E>, e: &AlphaGotEntry) -> Key {
    let empty = BStr::new(b"");
    let Some(id) = e.sym else {
        return (e.kind, 0, 0, 0, empty, e.addend);
    };
    let sym = &ctx.symbols[id];
    match sym.file() {
        None => (e.kind, 1, 0, 0, sym.name(), e.addend),
        Some(file) => (e.kind, 2, ctx.file(file).priority, sym.sym_idx(), empty, e.addend),
    }
}

/// A set of GOT entries that are addressed with the same GP value.
#[derive(Debug, Default)]
pub struct Group {
    /// Sorted by `key`.
    pub entries: Vec<AlphaGotEntry>,
    /// The offset of each entry from the beginning of the section.
    pub offsets: Vec<u64>,
    /// The offset of the group from the beginning of the section.
    pub offset: u64,
}

#[derive(Debug)]
pub struct AlphaGotSection<E: Target> {
    pub hdr: ChunkHeader<E>,
    pub groups: Vec<Group>,
    pub has_gottp: bool,
}

impl<E: Target> AlphaGotSection<E> {
    pub fn new() -> Self {
        let mut hdr =
            ChunkHeader::<E>::new(".alpha_got", SHT_PROGBITS, (SHF_ALLOC | SHF_WRITE) as u64);
        hdr.shdr.sh_addralign.set(WORD_SIZE);
        hdr.is_relro = true;
        Self { hdr, groups: Vec::new(), has_gottp: false }
    }
}

impl<E: Target> Default for AlphaGotSection<E> {
    fn default() -> Self {
        Self::new()
    }
}

fn section<E: Target>(ctx: &Context<E>) -> &AlphaGotSection<E> {
    ctx.alpha_got.as_ref().expect("Alpha has an .alpha_got section")
}

fn size_of(entries: &[AlphaGotEntry]) -> u64 {
    entries.iter().map(AlphaGotEntry::size).sum()
}

/// Returns the union of two sorted lists of entries.
fn merge<E: Target>(
    ctx: &Context<E>,
    a: &[AlphaGotEntry],
    b: &[AlphaGotEntry],
) -> Vec<AlphaGotEntry> {
    let mut out = Vec::with_capacity(a.len() + b.len());
    let (mut i, mut j) = (0, 0);
    while i < a.len() && j < b.len() {
        match key(ctx, &a[i]).cmp(&key(ctx, &b[j])) {
            std::cmp::Ordering::Less => {
                out.push(a[i]);
                i += 1;
            }
            std::cmp::Ordering::Greater => {
                out.push(b[j]);
                j += 1;
            }
            std::cmp::Ordering::Equal => {
                out.push(a[i]);
                i += 1;
                j += 1;
            }
        }
    }
    out.extend_from_slice(&a[i..]);
    out.extend_from_slice(&b[j..]);
    out
}

/// Groups input files so that all GOT entries referred to by a single file
/// are addressable from that file's GP.
pub fn finalize<E: Target>(ctx: &mut Context<E>) {
    let mut lists: Vec<Vec<AlphaGotEntry>> = ctx
        .objs
        .iter_mut()
        .map(|file| std::mem::take(file.alpha_got_entries.get_mut().unwrap()))
        .collect();

    {
        let ctx: &Context<E> = ctx;
        lists.par_iter_mut().for_each(|vec| {
            vec.sort_by_cached_key(|e| key(ctx, e));
            vec.dedup();
        });
    }

    let mut groups: Vec<Group> = Vec::new();
    let mut file_groups = Vec::with_capacity(lists.len());
    let mut cur: Vec<AlphaGotEntry> = Vec::new();

    fn flush(groups: &mut Vec<Group>, cur: &mut Vec<AlphaGotEntry>) {
        if !cur.is_empty() {
            groups.push(Group { entries: std::mem::take(cur), ..Group::default() });
        }
    }

    for (file, vec) in ctx.objs.iter().zip(lists) {
        // A file with no GOT entry of its own still needs a GP, because it
        // may have GP-relative relocations or make a BRSGP call that has to
        // agree with the callee's GP. Put it in the group we are currently
        // filling.
        if vec.is_empty() {
            file_groups.push(groups.len() as u32);
            continue;
        }

        let mut merged = merge(ctx, &cur, &vec);

        if size_of(&merged) > GROUP_SIZE {
            flush(&mut groups, &mut cur);
            merged = vec;

            // A single file needing more than 64 KiB of GOT entries is
            // unlinkable because we can't assign more than one GP to a file.
            if size_of(&merged) > GROUP_SIZE {
                fatal!(
                    "{file}: GOT entries referred to by this file exceed 64 KiB; recompile it as multiple smaller files"
                );
            }
        }

        cur = merged;
        file_groups.push(groups.len() as u32);
    }
    flush(&mut groups, &mut cur);

    // Assign offsets to GOT entries.
    let mut offset = 0;
    let mut has_gottp = false;

    for group in &mut groups {
        has_gottp |= group.entries.iter().any(|e| e.kind == AlphaGotKind::Gottp);
        group.offset = offset;
        group.offsets = group
            .entries
            .iter()
            .map(|e| {
                let off = offset;
                offset += e.size();
                off
            })
            .collect();
    }

    for (file, group) in ctx.objs.iter_mut().zip(file_groups) {
        file.alpha_got_group = group;
    }

    let sec = ctx.alpha_got.as_mut().expect("Alpha has an .alpha_got section");
    sec.groups = groups;
    sec.has_gottp = has_gottp;
    sec.hdr.shdr.sh_size.set(offset);
}

/// A GP value is defined as the address of the beginning of a GOT group
/// plus 0x8000, so that a 16-bit signed displacement from GP can cover
/// the entire 64 KiB group.
pub fn gp<E: Target>(ctx: &Context<E>, file: &ObjectFile<E>) -> u64 {
    let sec = section(ctx);

    // If this section is empty, it is removed from the output file and its
    // sh_addr is left at zero. GP still has to point somewhere inside the
    // loaded image, though, because GP-relative relocations that don't
    // involve the GOT at all are computed against it. Use the common .got,
    // which always contains at least the reserved first word.
    if sec.groups.is_empty() {
        return ctx.got.hdr.shdr.sh_addr.get() + 0x8000;
    }
    sec.hdr.shdr.sh_addr.get() + sec.groups[file.alpha_got_group as usize].offset + 0x8000
}

/// Returns the address of the given entry in the GOT group of `file`.
pub fn entry_addr<E: Target>(ctx: &Context<E>, file: &ObjectFile<E>, ent: AlphaGotEntry) -> u64 {
    let sec = section(ctx);
    let group = &sec.groups[file.alpha_got_group as usize];
    let i = group
        .entries
        .binary_search_by_key(&key(ctx, &ent), |e| key(ctx, e))
        .expect("GOT entry was registered by scan_relocations");
    sec.hdr.shdr.sh_addr.get() + group.offsets[i]
}

struct Slot {
    offset: u64,
    val: u64,
    r_type: u32,
    sym: Option<SymbolId>,
}

/// Computes the contents of `.alpha_got` and the dynamic relocations that
/// are needed to fix them up at load-time.
fn for_each_slot<E: Target>(ctx: &Context<E>, mut f: impl FnMut(Slot)) {
    let mut add = |offset: u64, val: u64, r_type: u32, sym: Option<SymbolId>| {
        f(Slot { offset, val, r_type, sym })
    };

    for group in &section(ctx).groups {
        for (e, &off) in group.entries.iter().zip(&group.offsets) {
            let addend = e.addend as u64;

            if e.kind == AlphaGotKind::Tlsld {
                if ctx.args.shared {
                    add(off, 0, E::R_DTPMOD, None);
                } else {
                    add(off, 1, R_NONE, None); // 1 means the main executable
                }
                continue;
            }

            let id = e.sym.expect("GOT entry has a symbol");
            let sym = &ctx.symbols[id];
            let addr = || sym.addr_with(ctx, AddrFlags::NO_PLT).wrapping_add(addend);

            match e.kind {
                AlphaGotKind::Regular => {
                    if sym.is_ifunc() {
                        // An ifunc has no address until its resolver has run, so
                        // the slot always has to be filled in at load-time. Alpha
                        // has no PLT, so this GOT entry is how the function is
                        // called.
                        add(off, addr(), E::R_IRELATIVE.unwrap(), None);
                    } else if sym.is_imported() {
                        // R_ALPHA_GLOB_DAT ignores an addend, so we need to use
                        // R_ALPHA_REFQUAD if we have a non-zero addend.
                        if e.addend != 0 {
                            add(off, addend, E::R_ABS, Some(id));
                        } else {
                            add(off, 0, E::R_GLOB_DAT, Some(id));
                        }
                    } else if ctx.args.pic && sym.is_relative() {
                        add(off, addr(), E::R_RELATIVE, None);
                    } else {
                        add(off, addr(), R_NONE, None);
                    }
                }
                AlphaGotKind::Tlsgd => {
                    if sym.is_imported() {
                        add(off, 0, E::R_DTPMOD, Some(id));
                        add(off + WORD_SIZE, addend, E::R_DTPOFF, Some(id));
                    } else if ctx.args.shared {
                        add(off, 0, E::R_DTPMOD, None);
                        add(off + WORD_SIZE, addr().wrapping_sub(ctx.dtp_addr), R_NONE, None);
                    } else {
                        add(off, 1, R_NONE, None); // 1 means the main executable
                        add(off + WORD_SIZE, addr().wrapping_sub(ctx.dtp_addr), R_NONE, None);
                    }
                }
                AlphaGotKind::Gottp => {
                    if sym.is_imported() {
                        add(off, addend, E::R_TPOFF, Some(id));
                    } else if ctx.args.shared {
                        add(off, addr().wrapping_sub(ctx.tls_begin), E::R_TPOFF, None);
                    } else {
                        add(off, addr().wrapping_sub(ctx.tp_addr), R_NONE, None);
                    }
                }
                AlphaGotKind::Tlsld => unreachable!(),
            }
        }
    }
}

fn count_slots<E: Target>(ctx: &Context<E>, pred: impl Fn(u32) -> bool) -> u64 {
    let mut n = 0;
    for_each_slot(ctx, |slot| n += u64::from(pred(slot.r_type)));
    n
}

pub fn num_irelative_relocs<E: Target>(ctx: &Context<E>) -> u64 {
    if ctx.alpha_got.is_none() {
        return 0;
    }
    count_slots(ctx, |r_type| Some(r_type) == E::R_IRELATIVE)
}

pub fn num_dynrels<E: Target>(ctx: &Context<E>) -> u64 {
    count_slots(ctx, |r_type| r_type != R_NONE)
}

pub fn write_dynrels<E: Target>(ctx: &Context<E>, out: &mut [ElfRel<E>]) {
    let sh_addr = section(ctx).hdr.shdr.sh_addr.get();
    let mut i = 0;
    for_each_slot(ctx, |slot| {
        if slot.r_type != R_NONE {
            out[i] = ElfRel::<E>::new(
                sh_addr + slot.offset,
                slot.r_type,
                slot.sym.and_then(|s| ctx.symbols[s].dynsym_idx(&ctx.symbols)).unwrap_or(0),
                slot.val as i64,
            );
            i += 1;
        }
    });
    debug_assert_eq!(i, out.len());
}

pub fn copy_buf<E: Target>(ctx: &Context<E>, buf: &mut [u8]) {
    buf.fill(0);
    for_each_slot(ctx, |slot| {
        if slot.r_type == R_NONE || ctx.args.apply_dynamic_relocs {
            E::write_u64(&mut buf[slot.offset as usize..], slot.val);
        }
    });
}

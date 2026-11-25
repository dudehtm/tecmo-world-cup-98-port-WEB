#!/usr/bin/env python3
"""Offline Static SH-2 AOT Recompiler Generator for Tecmo World Cup '98 (twcup98).

Reads clean decrypted Classic '98 High WRAM (after 820f cold boot) and dynamic
basic-block execution profiles, extracts hot basic blocks in [0x06001000, 0x06070000),
groups them into address-local C++17 cluster functions with direct intra-cluster
branch chaining and zero-overhead validated fast-paths, and emits zero-STL portable C++17:
  - port/src/sh2_aot_twcup98.hpp
  - port/src/sh2_aot_twcup98.cpp

Every generated instruction preserves 100% bit-exact semantics with sh2_step_inl.hpp
and stv_machine.cpp (all registers, flags, cycles, retired, cur_pc, delay,
last_write_ea/val, cart_reads, MMIO boundaries, hooks, and fault rollbacks).
"""

import argparse
import pathlib
import struct

CODE_LO = 0x06001000
CODE_HI = 0x06070000
HIGH_WRAM_BASE = 0x06000000
CODE_SLOTS = (CODE_HI - CODE_LO) // 2
LINE_SHIFT = 6  # 64-byte validation lines
LINE_SIZE = 1 << LINE_SHIFT
NUM_LINES = (CODE_HI - HIGH_WRAM_BASE) >> LINE_SHIFT  # 7168


def fnv1a32_line(data: bytes) -> int:
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return 1 if h == 0 else h


def sext8(v: int) -> int:
    v &= 0xFF
    return v - 0x100 if (v & 0x80) else v


def sext12(v: int) -> int:
    v &= 0xFFF
    return v - 0x1000 if (v & 0x800) else v


def sext16(v: int) -> int:
    v &= 0xFFFF
    return v - 0x10000 if (v & 0x8000) else v


def u32(v: int) -> int:
    return v & 0xFFFFFFFF


def is_hook_pc(pc: int) -> bool:
    """Matches is_hook_pc_range(pc, pc, true) in stv_machine.cpp."""
    if pc in (
        0x060053E0,
        0x0600582A,
        0x06005888,
        0x06007DAE,
        0x060085FE,
        0x06008624,
        0x0604EDFA,
        0x060529A6,
        0x06052A94,
    ):
        return True
    if 0x06005894 <= pc <= 0x06005920:
        return True
    if 0x060519C0 <= pc <= 0x0605219C:
        return True
    return False


def decode_sh2(op: int) -> str:
    """Exact mirror of decode_sh2_class() in port/src/sh2_core.cpp."""
    t = (op >> 12) & 0xF
    n = (op >> 8) & 0xF
    lo1 = op & 0xF
    lo2 = op & 0xFF
    if t == 0x0:
        m0 = {
            0x02: "STCSR",
            0x08: "CLRT",
            0x09: "NOP",
            0x0A: "STSMACH",
            0x0B: "RTS",
            0x12: "STCSGR",
            0x18: "SETT",
            0x19: "DIV0U",
            0x1A: "STSMACL",
            0x1B: "NOP",  # SLEEP
            0x22: "STCVBR",
            0x23: "BRAF",
            0x28: "CLRMAC",
            0x29: "MOVT",
            0x2A: "STSPR",
            0x2B: "RTE",
        }
        if (op & 0x3F) in m0:
            return m0[op & 0x3F]
        if lo1 == 0x3:
            return "BSRF" if lo2 == 0x03 else "INVALID"
        return {
            0x4: "MOVBS0",
            0x5: "MOVWS0",
            0x6: "MOVLS0",
            0x7: "MULL",
            0xC: "MOVBL0",
            0xD: "MOVWL0",
            0xE: "MOVLL0",
            0xF: "MAC_L",
        }.get(lo1, "INVALID")
    if t == 0x1:
        return "MOVLS4"
    if t == 0x2:
        return {
            0x0: "MOVBS",
            0x1: "MOVWS",
            0x2: "MOVLS",
            0x4: "MOVBM",
            0x5: "MOVWM",
            0x6: "MOVLM",
            0x7: "DIV0S",
            0x8: "TST",
            0x9: "AND",
            0xA: "XOR",
            0xB: "OR",
            0xD: "XTRCT",
            0xE: "MULU",
            0xF: "MULS",
        }.get(lo1, "INVALID")
    if t == 0x3:
        return {
            0x0: "CMPEQ",
            0x2: "CMPHS",
            0x3: "CMPGE",
            0x4: "DIV1",
            0x5: "DMULU",
            0x6: "CMPHI",
            0x7: "CMPGT",
            0x8: "SUB",
            0xA: "SUBC",
            0xB: "SUBV",
            0xC: "ADD",
            0xD: "DMULS",
            0xE: "ADDC",
            0xF: "ADDV",
        }.get(lo1, "INVALID")
    if t == 0x4:
        return {
            0x00: "SHLL",
            0x01: "SHLR",
            0x02: "STSMMACH",
            0x03: "STCMSR",
            0x04: "ROTL",
            0x05: "ROTR",
            0x06: "LDSMMACH",
            0x07: "LDCMSR",
            0x08: "SHLL2",
            0x09: "SHLR2",
            0x0A: "LDSMACH",
            0x0B: "JSR",
            0x0E: "LDCSR",
            0x10: "DT",
            0x11: "CMPPZ",
            0x12: "STSMMACL",
            0x13: "STCMGBR",
            0x15: "CMPPL",
            0x16: "LDSMMACL",
            0x17: "LDCMGBR",
            0x18: "SHLL8",
            0x19: "SHLR8",
            0x1A: "LDSMACL",
            0x1B: "TAS",
            0x1E: "LDCGBR",
            0x20: "SHAL",
            0x21: "SHAR",
            0x22: "STSMPR",
            0x23: "STCMVBR",
            0x24: "ROTCL",
            0x25: "ROTCR",
            0x26: "LDSMPR",
            0x27: "LDCMVBR",
            0x28: "SHLL16",
            0x29: "SHLR16",
            0x2A: "LDSPR",
            0x2B: "JMP",
            0x2E: "LDCVBR",
        }.get(lo2 & 0x3F, "INVALID")
    if t == 0x5:
        return "MOVLL4"
    if t == 0x6:
        return {
            0x0: "MOVBL",
            0x1: "MOVWL",
            0x2: "MOVLL",
            0x3: "MOV",
            0x4: "MOVBP",
            0x5: "MOVWP",
            0x6: "MOVLP",
            0x7: "NOT",
            0x8: "SWAPB",
            0x9: "SWAPW",
            0xA: "NEGC",
            0xB: "NEG",
            0xC: "EXTUB",
            0xD: "EXTUW",
            0xE: "EXTSB",
            0xF: "EXTSW",
        }.get(lo1, "INVALID")
    if t == 0x7:
        return "ADDI"
    if t == 0x8:
        return {
            0x0: "MOVBS4",
            0x1: "MOVWS4",
            0x4: "MOVBL4",
            0x5: "MOVWL4",
            0x8: "CMPIM",
            0x9: "BT",
            0xB: "BF",
            0xD: "BTS",
            0xF: "BFS",
        }.get(n, "INVALID")
    if t == 0x9:
        return "MOVWI"
    if t == 0xA:
        return "BRA"
    if t == 0xB:
        return "BSR"
    if t == 0xC:
        return {
            0x0: "MOVBSG",
            0x1: "MOVWSG",
            0x2: "MOVLSG",
            0x4: "MOVBLG",
            0x5: "MOVWLG",
            0x6: "MOVLLG",
            0x7: "MOVA",
            0x8: "TSTI",
            0x9: "ANDI",
            0xA: "XORI",
            0xB: "ORI",
            0xC: "TSTM",
            0xD: "ANDM",
            0xE: "XORM",
            0xF: "ORM",
        }.get(n, "INVALID")
    if t == 0xD:
        return "MOVLI"
    if t == 0xE:
        return "MOVI"
    return "INVALID"


FORBIDDEN_OPS = {"INVALID", "RTE", "TAS"}
UNCOND_DELAY_OPS = {"RTS", "JMP", "JSR", "BRA", "BRAF", "BSR", "BSRF"}
COND_NODELAY_OPS = {"BT", "BF"}
COND_DELAY_OPS = {"BTS", "BFS"}
ALL_BRANCH_OPS = UNCOND_DELAY_OPS | COND_NODELAY_OPS | COND_DELAY_OPS
FORBIDDEN_DELAY_SLOT_OPS = FORBIDDEN_OPS | ALL_BRANCH_OPS | {"MOVWI", "MOVLI", "MOVA"}
STOP_AFTER_OPS = {"LDCSR", "LDCMSR"}
HLE_ENTRY_PCS = {0x060631DE, 0x0605CD86}


def insn_base_cycles(cls: str) -> int:
    if cls in ("STCMSR", "STCMGBR", "STCMVBR"):
        return 2
    if cls in ("LDCMSR", "LDCMGBR", "LDCMVBR"):
        return 3
    if cls in UNCOND_DELAY_OPS:
        return 2
    return 1


def insn_max_cycles(cls: str) -> int:
    if cls in COND_NODELAY_OPS:
        return 3
    if cls in COND_DELAY_OPS:
        return 2
    return insn_base_cycles(cls)


class BasicBlock:
    __slots__ = (
        "start_pc",
        "insns",  # list of (pc, op, cls)
        "term_kind",  # FALLTHROUGH, UNCOND_DELAY, COND_NODELAY, COND_DELAY, STOP_AFTER
        "target_pc",  # int or None (dynamic target)
        "fallthrough_pc",  # int or None
        "lines",  # sorted tuple of 64-byte line indices
        "weight",
        "is_dt_bf_self",
        "dt_reg",
    )

    def __init__(self, start_pc: int):
        self.start_pc = start_pc
        self.insns = []
        self.term_kind = "FALLTHROUGH"
        self.target_pc = None
        self.fallthrough_pc = None
        self.lines = ()
        self.weight = 0.0
        self.is_dt_bf_self = False
        self.dt_reg = -1


def extract_blocks(
    hw: bytes, ep, tw, ti, bb_hits, l64w, max_block_insns: int, hot_block_insns: int, max_total_insns: int
):
    tot_ep = sum(ep) or 1
    tot_tw = sum(tw) or 1
    tot_ti = sum(ti) or 1

    def pc_idx(pc: int) -> int:
        return (pc - CODE_LO) >> 1

    def rd16(pc: int) -> int:
        off = pc - HIGH_WRAM_BASE
        return (hw[off] << 8) | hw[off + 1]

    def is_forbidden_pc(pc: int) -> bool:
        if pc < CODE_LO or pc >= CODE_HI or (pc & 1):
            return True
        if l64w[(pc - HIGH_WRAM_BASE) >> LINE_SHIFT] > 0:
            return True
        if is_hook_pc(pc):
            return True
        if decode_sh2(rd16(pc)) in FORBIDDEN_OPS:
            return True
        return False

    def pc_hit_any(pc: int) -> bool:
        if pc < CODE_LO or pc >= CODE_HI or (pc & 1):
            return False
        i = pc_idx(pc)
        return (ep[i] + tw[i] + ti[i]) > 0

    def block_cap_for(start_pc: int) -> int:
        # Use tighter chunk cap (e.g. 4 insns) on super-hot PCs so 32-step round
        # boundaries rarely bail into mid-block interpreter fallback!
        idx = pc_idx(start_pc)
        if ep[idx] >= 15000 or ti[idx] >= 15000:
            return hot_block_insns
        return max_block_insns

    # 1. Discover all candidate block entry PCs from dynamic BB entries + static targets/splits
    entry_set = set()
    for pc in HLE_ENTRY_PCS:
        if not is_forbidden_pc(pc):
            entry_set.add(pc)
    for i in range(CODE_SLOTS):
        if bb_hits[i] > 0:
            pc = CODE_LO + i * 2
            if not is_forbidden_pc(pc):
                entry_set.add(pc)

    for i in range(CODE_SLOTS):
        if (ep[i] + tw[i] + ti[i]) > 0:
            pc = CODE_LO + i * 2
            if is_forbidden_pc(pc):
                continue
            if i == 0 or (ep[i - 1] + tw[i - 1] + ti[i - 1]) == 0:
                entry_set.add(pc)

    # Iteratively trace and add static targets/fallthroughs that were executed
    changed = True
    while changed:
        changed = False
        new_entries = set()
        for start_pc in list(entry_set):
            pc = start_pc
            count = 0
            cap = block_cap_for(start_pc)
            while True:
                if pc != start_pc and (is_forbidden_pc(pc) or pc in entry_set or count >= cap):
                    if not is_forbidden_pc(pc) and pc_hit_any(pc) and pc not in entry_set:
                        new_entries.add(pc)
                    break
                if is_forbidden_pc(pc):
                    break
                op = rd16(pc)
                cls = decode_sh2(op)
                if cls in COND_NODELAY_OPS:
                    tgt = u32(pc + 4 + (sext8(op & 0xFF) << 1))
                    ft = u32(pc + 2)
                    for t in (tgt, ft):
                        if not is_forbidden_pc(t) and pc_hit_any(t) and t not in entry_set:
                            new_entries.add(t)
                    break
                if cls in UNCOND_DELAY_OPS or cls in COND_DELAY_OPS:
                    slot_pc = u32(pc + 2)
                    if is_forbidden_pc(slot_pc) or decode_sh2(rd16(slot_pc)) in FORBIDDEN_DELAY_SLOT_OPS:
                        break
                    if cls in ("BRA", "BSR"):
                        tgt = u32(pc + 4 + (sext12(op & 0xFFF) << 1))
                        if not is_forbidden_pc(tgt) and pc_hit_any(tgt) and tgt not in entry_set:
                            new_entries.add(tgt)
                    if cls in ("BSR", "JSR", "BSRF"):
                        ret_pc = u32(pc + 4)
                        if not is_forbidden_pc(ret_pc) and pc_hit_any(ret_pc) and ret_pc not in entry_set:
                            new_entries.add(ret_pc)
                    if cls in COND_DELAY_OPS:
                        tgt = u32(pc + 4 + (sext8(op & 0xFF) << 1))
                        ft = u32(pc + 4)
                        for t in (tgt, ft):
                            if not is_forbidden_pc(t) and pc_hit_any(t) and t not in entry_set:
                                new_entries.add(t)
                    break
                if cls in STOP_AFTER_OPS:
                    ft = u32(pc + 2)
                    if not is_forbidden_pc(ft) and pc_hit_any(ft) and ft not in entry_set:
                        new_entries.add(ft)
                    break
                count += 1
                pc = u32(pc + 2)
        if new_entries:
            entry_set |= new_entries
            changed = True

    # 2. Materialize all candidate BasicBlocks
    blocks = []
    for start_pc in sorted(entry_set):
        if is_forbidden_pc(start_pc):
            continue
        blk = BasicBlock(start_pc)
        pc = start_pc
        cap = block_cap_for(start_pc)
        while True:
            if pc != start_pc and (is_forbidden_pc(pc) or pc in entry_set or len(blk.insns) >= cap):
                blk.term_kind = "FALLTHROUGH"
                blk.fallthrough_pc = pc
                break
            if is_forbidden_pc(pc):
                break
            op = rd16(pc)
            cls = decode_sh2(op)
            if cls in COND_NODELAY_OPS:
                blk.insns.append((pc, op, cls))
                blk.term_kind = "COND_NODELAY"
                blk.target_pc = u32(pc + 4 + (sext8(op & 0xFF) << 1))
                blk.fallthrough_pc = u32(pc + 2)
                break
            if cls in UNCOND_DELAY_OPS or cls in COND_DELAY_OPS:
                slot_pc = u32(pc + 2)
                if is_forbidden_pc(slot_pc) or decode_sh2(rd16(slot_pc)) in FORBIDDEN_DELAY_SLOT_OPS:
                    if blk.insns:
                        blk.term_kind = "FALLTHROUGH"
                        blk.fallthrough_pc = pc
                    break
                slot_op = rd16(slot_pc)
                slot_cls = decode_sh2(slot_op)
                blk.insns.append((pc, op, cls))
                blk.insns.append((slot_pc, slot_op, slot_cls))
                if cls in UNCOND_DELAY_OPS:
                    blk.term_kind = "UNCOND_DELAY"
                    if cls in ("BRA", "BSR"):
                        blk.target_pc = u32(pc + 4 + (sext12(op & 0xFFF) << 1))
                    else:
                        blk.target_pc = None
                    blk.fallthrough_pc = None
                else:
                    blk.term_kind = "COND_DELAY"
                    blk.target_pc = u32(pc + 4 + (sext8(op & 0xFF) << 1))
                    blk.fallthrough_pc = u32(pc + 4)
                break
            if cls in STOP_AFTER_OPS:
                blk.insns.append((pc, op, cls))
                blk.term_kind = "STOP_AFTER"
                blk.fallthrough_pc = u32(pc + 2)
                break
            blk.insns.append((pc, op, cls))
            pc = u32(pc + 2)

        if not blk.insns:
            continue

        if (
            len(blk.insns) == 2
            and blk.insns[0][2] == "DT"
            and blk.insns[1][2] == "BF"
            and blk.target_pc == blk.start_pc
        ):
            blk.is_dt_bf_self = True
            blk.dt_reg = (blk.insns[0][1] >> 8) & 0xF

        lines = sorted({(ipc - HIGH_WRAM_BASE) >> LINE_SHIFT for ipc, _, _ in blk.insns})
        blk.lines = tuple(lines)

        w = 0.0
        for ipc, _, _ in blk.insns:
            idx = pc_idx(ipc)
            w += ep[idx] / tot_ep + tw[idx] / tot_tw + ti[idx] / tot_ti
        blk.weight = w
        if w > 0.0:
            blocks.append(blk)

    # 3. Rank by weight-per-instruction * len (total weight) and select up to max_total_insns
    blocks.sort(key=lambda b: -b.weight)
    selected = []
    total_insns = 0
    for blk in blocks:
        if total_insns + len(blk.insns) > max_total_insns:
            continue
        selected.append(blk)
        total_insns += len(blk.insns)

    selected.sort(key=lambda b: b.start_pc)

    covered_pcs = set()
    for blk in selected:
        for ipc, _, _ in blk.insns:
            covered_pcs.add(ipc)
    cov_ep = sum(ep[pc_idx(pc)] for pc in covered_pcs) / tot_ep * 100.0
    cov_tw = sum(tw[pc_idx(pc)] for pc in covered_pcs) / tot_tw * 100.0
    cov_ti = sum(ti[pc_idx(pc)] for pc in covered_pcs) / tot_ti * 100.0
    return selected, total_insns, (cov_ep, cov_tw, cov_ti)


def build_clusters(blocks, target_cluster_insns: int):
    clusters = []
    cur = []
    cur_insns = 0
    for blk in blocks:
        if cur and cur_insns >= target_cluster_insns:
            prev = cur[-1]
            prev_end = prev.insns[-1][0] + 2
            is_good_cut = (
                (prev.term_kind == "UNCOND_DELAY" and prev.insns[-2][2] == "RTS")
                or (blk.start_pc - prev_end >= 16)
                or (cur_insns >= int(target_cluster_insns * 1.4))
            )
            if is_good_cut:
                clusters.append(cur)
                cur = []
                cur_insns = 0
        cur.append(blk)
        cur_insns += len(blk.insns)
    if cur:
        clusters.append(cur)
    return clusters


def analyze_cluster_regs(cluster):
    used_r = set()
    written_r = set()
    uses_sr = writes_sr = False
    uses_pr = writes_pr = False
    uses_gbr = writes_gbr = False
    uses_mach = writes_mach = False
    uses_macl = writes_macl = False

    for blk in cluster:
        for _, op, cls in blk.insns:
            n = (op >> 8) & 0xF
            m = (op >> 4) & 0xF

            if cls == "NOP":
                pass
            elif cls in ("CLRT", "SETT"):
                uses_sr = writes_sr = True
            elif cls == "CLRMAC":
                writes_mach = writes_macl = True
            elif cls == "STCSR":
                uses_sr = True
                written_r.add(n)
            elif cls == "STCSGR":
                uses_gbr = True
                written_r.add(n)
            elif cls == "STCVBR":
                written_r.add(n)
            elif cls == "LDCSR":
                used_r.add(n)
                writes_sr = True
            elif cls == "LDCGBR":
                used_r.add(n)
                writes_gbr = True
            elif cls == "LDCVBR":
                used_r.add(n)
            elif cls == "STSMACH":
                uses_mach = True
                written_r.add(n)
            elif cls == "STSMACL":
                uses_macl = True
                written_r.add(n)
            elif cls == "STSPR":
                uses_pr = True
                written_r.add(n)
            elif cls == "LDSMACH":
                used_r.add(n)
                writes_mach = True
            elif cls == "LDSMACL":
                used_r.add(n)
                writes_macl = True
            elif cls == "LDSPR":
                used_r.add(n)
                writes_pr = True
            elif cls == "STSMMACH":
                used_r.add(n); written_r.add(n); uses_mach = True
            elif cls == "STSMMACL":
                used_r.add(n); written_r.add(n); uses_macl = True
            elif cls == "STSMPR":
                used_r.add(n); written_r.add(n); uses_pr = True
            elif cls == "LDSMMACH":
                used_r.add(n); written_r.add(n); writes_mach = True
            elif cls == "LDSMMACL":
                used_r.add(n); written_r.add(n); writes_macl = True
            elif cls == "LDSMPR":
                used_r.add(n); written_r.add(n); writes_pr = True
            elif cls == "STCMSR":
                used_r.add(n); written_r.add(n); uses_sr = True
            elif cls == "STCMGBR":
                used_r.add(n); written_r.add(n); uses_gbr = True
            elif cls == "STCMVBR":
                used_r.add(n); written_r.add(n)
            elif cls == "LDCMSR":
                used_r.add(n); written_r.add(n); writes_sr = True
            elif cls == "LDCMGBR":
                used_r.add(n); written_r.add(n); writes_gbr = True
            elif cls == "LDCMVBR":
                used_r.add(n); written_r.add(n)
            elif cls == "RTS":
                uses_pr = True
            elif cls == "JMP":
                used_r.add(n)
            elif cls == "JSR":
                used_r.add(n); writes_pr = True
            elif cls == "BRA":
                pass
            elif cls == "BRAF":
                used_r.add(n)
            elif cls == "BSR":
                writes_pr = True
            elif cls == "BSRF":
                used_r.add(n); writes_pr = True
            elif cls in ("BT", "BF", "BTS", "BFS"):
                uses_sr = True
            elif cls in ("MOV", "NEG", "NOT", "EXTSB", "EXTSW", "EXTUB", "EXTUW", "SWAPB", "SWAPW"):
                used_r.add(m); written_r.add(n)
            elif cls in ("MOVI", "MOVWI", "MOVLI"):
                written_r.add(n)
            elif cls == "MOVA":
                written_r.add(0)
            elif cls == "MOVT":
                uses_sr = True; written_r.add(n)
            elif cls in ("MOVBS", "MOVWS", "MOVLS"):
                used_r.add(n); used_r.add(m)
            elif cls in ("MOVBL", "MOVWL", "MOVLL", "MOVLL4"):
                used_r.add(m); written_r.add(n)
            elif cls in ("MOVBM", "MOVWM", "MOVLM"):
                used_r.add(n); used_r.add(m); written_r.add(n)
            elif cls in ("MOVBP", "MOVWP", "MOVLP"):
                used_r.add(m); written_r.add(n)
                if n != m:
                    written_r.add(m)
            elif cls in ("MOVBS0", "MOVWS0", "MOVLS0"):
                used_r.add(n); used_r.add(m); used_r.add(0)
            elif cls in ("MOVBL0", "MOVWL0", "MOVLL0"):
                used_r.add(m); used_r.add(0); written_r.add(n)
            elif cls == "MOVLS4":
                used_r.add(n); used_r.add(m)
            elif cls in ("MOVBS4", "MOVWS4"):
                used_r.add(m); used_r.add(0)
            elif cls in ("MOVBL4", "MOVWL4"):
                used_r.add(m); written_r.add(0)
            elif cls in ("MOVBSG", "MOVWSG", "MOVLSG"):
                uses_gbr = True; used_r.add(0)
            elif cls in ("MOVBLG", "MOVWLG", "MOVLLG"):
                uses_gbr = True; written_r.add(0)
            elif cls in ("ADD", "SUB", "AND", "OR", "XOR", "XTRCT"):
                used_r.add(n); used_r.add(m); written_r.add(n)
            elif cls == "ADDI":
                used_r.add(n); written_r.add(n)
            elif cls in ("ADDC", "ADDV", "SUBC", "SUBV"):
                used_r.add(n); used_r.add(m); written_r.add(n); uses_sr = writes_sr = True
            elif cls == "NEGC":
                used_r.add(m); written_r.add(n); uses_sr = writes_sr = True
            elif cls in ("ANDI", "ORI", "XORI"):
                used_r.add(0); written_r.add(0)
            elif cls in ("ANDM", "ORM", "XORM"):
                uses_gbr = True; used_r.add(0)
            elif cls in ("TST", "CMPEQ", "CMPGE", "CMPGT", "CMPHI", "CMPHS", "DIV0S"):
                used_r.add(n); used_r.add(m); uses_sr = writes_sr = True
            elif cls in ("TSTI", "CMPIM"):
                used_r.add(0); uses_sr = writes_sr = True
            elif cls == "TSTM":
                uses_gbr = True; used_r.add(0); uses_sr = writes_sr = True
            elif cls in ("CMPPL", "CMPPZ"):
                used_r.add(n); uses_sr = writes_sr = True
            elif cls == "DT":
                used_r.add(n); written_r.add(n); uses_sr = writes_sr = True
            elif cls in ("SHLL", "SHAL", "SHLR", "SHAR", "ROTL", "ROTR", "ROTCL", "ROTCR"):
                used_r.add(n); written_r.add(n); uses_sr = writes_sr = True
            elif cls in ("SHLL2", "SHLR2", "SHLL8", "SHLR8", "SHLL16", "SHLR16"):
                used_r.add(n); written_r.add(n)
            elif cls in ("MULL", "MULS", "MULU"):
                used_r.add(n); used_r.add(m); writes_macl = True
            elif cls in ("DMULS", "DMULU"):
                used_r.add(n); used_r.add(m); writes_mach = writes_macl = True
            elif cls == "MAC_L":
                used_r.add(n); used_r.add(m); written_r.add(n); written_r.add(m)
                uses_mach = writes_mach = uses_macl = writes_macl = uses_sr = True
            elif cls == "DIV0U":
                uses_sr = writes_sr = True
            elif cls == "DIV1":
                used_r.add(n); used_r.add(m); written_r.add(n); uses_sr = writes_sr = True
            else:
                raise ValueError(f"Unhandled op class in reg analysis: {cls}")

    used_r |= written_r
    if writes_sr:
        uses_sr = True
    if writes_pr:
        uses_pr = True
    if writes_gbr:
        uses_gbr = True
    if writes_mach:
        uses_mach = True
    if writes_macl:
        uses_macl = True
    return (
        sorted(used_r),
        sorted(written_r),
        uses_sr,
        writes_sr,
        uses_pr,
        writes_pr,
        uses_gbr,
        writes_gbr,
        uses_mach,
        writes_mach,
        uses_macl,
        writes_macl,
    )


def emit_insn_code(
    out,
    hw: bytes,
    blk_lines: set,
    ipc: int,
    op: int,
    cls: str,
    step_idx: int,
    cyc_off_expr: str,
    prev_pc: int,
    is_delay_slot: bool,
    delay_target_expr: str,
    next_pc_after_stop: str,
    total_cyc_after_stop: str,
    steps_after_stop: int,
):
    """Emit compact, zero-fault-overhead-on-fast-path C++ statements for one SH-2 instruction."""
    n = (op >> 8) & 0xF
    m = (op >> 4) & 0xF
    d4 = op & 0xF
    d8 = op & 0xFF
    rn = f"r{n}"
    rm = f"r{m}"

    def fault_stmt():
        if is_delay_slot:
            return (
                f"pc = 0x{ipc:08X}u; fault_delay = {delay_target_expr}; "
                f"steps_left -= {step_idx}; ret += {step_idx}ULL; cyc += {cyc_off_expr}; "
                f"cur_pc = 0x{prev_pc:08X}u; goto exit_fault;"
            )
        if step_idx > 0:
            return (
                f"pc = 0x{ipc:08X}u; steps_left -= {step_idx}; ret += {step_idx}ULL; "
                f"cyc += {cyc_off_expr}; cur_pc = 0x{prev_pc:08X}u; goto exit_fault;"
            )
        return f"pc = 0x{ipc:08X}u; goto exit_fault;"

    def stop_stmt(extra_after=""):
        pre = f"{extra_after} " if extra_after else ""
        return (
            f"{pre}pc = {next_pc_after_stop}; steps_left -= {steps_after_stop}; "
            f"ret += {steps_after_stop}ULL; cyc += {total_cyc_after_stop}; "
            f"cur_pc = 0x{ipc:08X}u; goto exit_stop;"
        )

    def emit_load(width: int, ea_expr: str, dst_assign: str):
        # Single-branch High WRAM fast-path: NEVER reads fault.ok on fast path!
        if width == 4:
            cond = "(_h | (_h << 30)) < kHighWramSize"
            fast_rd = "aot_hw_be32(hw, _h)"
        elif width == 2:
            cond = "(_h | (_h << 31)) < kHighWramSize"
            fast_rd = "aot_hw_be16(hw, _h)"
        else:
            cond = "_h < kHighWramSize"
            fast_rd = "hw[_h]"
        out.append(
            f"    {{ const uint32_t _ea = {ea_expr}; const uint32_t _h = (_ea & 0xDFFFFFFFu) - kHighWramBase; uint32_t _v;"
        )
        out.append(f"      if (__builtin_expect({cond}, 1)) {{ _v = {fast_rd}; }} else {{")
        out.append(
            f"        _v = aot_load_slow<{width}>(cpu_id, m, _ea, 0x{ipc:08X}u, cyc + {cyc_off_expr}, fault);"
        )
        out.append(f"        if (__builtin_expect(!fault.ok, 0)) {{ {fault_stmt()} }}")
        out.append(f"      }} {dst_assign} }}")

    def emit_store(width: int, ea_expr: str, val_expr: str, pre_stmt="", post_stmt=""):
        # Single-branch full High WRAM fast-path (covers both [0x06070000..0x060FFFFF]
        # and scratch/local writes inside [0x06000000..0x0606FFFF], e.g. SGL 0x060632C4).
        if width == 4:
            cond = "(_h | (_h << 30)) < kHighWramSize"
            fast_wr = "aot_hw_st32(hw, _h, _val);"
        elif width == 2:
            cond = "(_h | (_h << 31)) < kHighWramSize"
            fast_wr = "aot_hw_st16(hw, _h, _val);"
        else:
            cond = "_h < kHighWramSize"
            fast_wr = "hw[_h] = static_cast<uint8_t>(_val);"
        out.append(
            f"    {{ {pre_stmt}const uint32_t _sea = {ea_expr}; const uint32_t _val = {val_expr}; "
            f"const uint32_t _h = (_sea & 0xDFFFFFFFu) - kHighWramBase;"
        )
        out.append(
            f"      if (__builtin_expect({cond}, 1)) {{ m.last_write_ea = kHighWramBase + _h; "
            f"m.last_write_val = _val; {fast_wr} "
            f"if (__builtin_expect(_h < StvMachineState::kAotCodeRangeBytes, 0)) {{ "
            f"if (__builtin_expect(aot_mark_code_write(m, _h), 0)) {{ {stop_stmt(post_stmt)} }} }} }} else {{"
        )
        out.append(
            f"        const int _st = aot_store_slow<{width}>(cpu_id, m, _sea, _val, 0x{ipc:08X}u, cyc + {cyc_off_expr}, fault);"
        )
        out.append(f"        if (__builtin_expect(_st != 0, 0)) {{")
        out.append(f"          if (__builtin_expect(_st < 0, 0)) {{ {fault_stmt()} }}")
        out.append(f"          {stop_stmt(post_stmt)}")
        out.append(f"        }}")
        if post_stmt:
            out.append(f"      }} {post_stmt} }}")
        else:
            out.append(f"      }} }}")

    if cls == "NOP":
        pass
    elif cls == "CLRT":
        out.append("    sr &= ~kShSrT;")
    elif cls == "SETT":
        out.append("    sr |= kShSrT;")
    elif cls == "CLRMAC":
        out.append("    mach = 0u; macl = 0u;")
    elif cls == "STCSR":
        out.append(f"    {rn} = sr & 0x3F3u;")
    elif cls == "STCSGR":
        out.append(f"    {rn} = gbr;")
    elif cls == "STCVBR":
        out.append(f"    {rn} = cpu.vbr;")
    elif cls == "LDCSR":
        out.append(f"    sr = {rn} & 0x3F3u;")
    elif cls == "LDCGBR":
        out.append(f"    gbr = {rn};")
    elif cls == "LDCVBR":
        out.append(f"    cpu.vbr = {rn};")
    elif cls == "STSMACH":
        out.append(f"    {rn} = mach;")
    elif cls == "STSMACL":
        out.append(f"    {rn} = macl;")
    elif cls == "STSPR":
        out.append(f"    {rn} = pr;")
    elif cls == "LDSMACH":
        out.append(f"    mach = {rn};")
    elif cls == "LDSMACL":
        out.append(f"    macl = {rn};")
    elif cls == "LDSPR":
        out.append(f"    pr = {rn};")

    elif cls == "STSMMACH":
        emit_store(4, "_nea", "mach", f"const uint32_t _nea = {rn} - 4u; ", f"{rn} = _nea;")
    elif cls == "STSMMACL":
        emit_store(4, "_nea", "macl", f"const uint32_t _nea = {rn} - 4u; ", f"{rn} = _nea;")
    elif cls == "STSMPR":
        emit_store(4, "_nea", "pr", f"const uint32_t _nea = {rn} - 4u; ", f"{rn} = _nea;")
    elif cls == "LDSMMACH":
        emit_load(4, rn, f"mach = _v; {rn} += 4u;")
    elif cls == "LDSMMACL":
        emit_load(4, rn, f"macl = _v; {rn} += 4u;")
    elif cls == "LDSMPR":
        emit_load(4, rn, f"pr = _v; {rn} += 4u;")
    elif cls == "STCMSR":
        emit_store(4, "_nea", "sr & 0x3F3u", f"const uint32_t _nea = {rn} - 4u; ", f"{rn} = _nea;")
    elif cls == "STCMGBR":
        emit_store(4, "_nea", "gbr", f"const uint32_t _nea = {rn} - 4u; ", f"{rn} = _nea;")
    elif cls == "STCMVBR":
        emit_store(4, "_nea", "cpu.vbr", f"const uint32_t _nea = {rn} - 4u; ", f"{rn} = _nea;")
    elif cls == "LDCMSR":
        emit_load(4, rn, f"sr = _v & 0x3F3u; {rn} += 4u;")
    elif cls == "LDCMGBR":
        emit_load(4, rn, f"gbr = _v; {rn} += 4u;")
    elif cls == "LDCMVBR":
        emit_load(4, rn, f"cpu.vbr = _v; {rn} += 4u;")

    elif cls == "MOV":
        if n != m:
            out.append(f"    {rn} = {rm};")
    elif cls == "MOVI":
        out.append(f"    {rn} = 0x{u32(sext8(d8)):08X}u;")
    elif cls == "MOVWI":
        lit_ea = u32(ipc + 4 + (d8 << 1))
        lit_off = lit_ea - HIGH_WRAM_BASE
        lit_ln = lit_off >> LINE_SHIFT
        if HIGH_WRAM_BASE <= lit_ea < CODE_HI and lit_ln in blk_lines:
            val16 = (hw[lit_off] << 8) | hw[lit_off + 1]
            out.append(f"    {rn} = 0x{u32(sext16(val16)):08X}u;")
        elif HIGH_WRAM_BASE <= lit_ea < HIGH_WRAM_BASE + 0x100000 and (lit_ea & 1) == 0:
            out.append(f"    {rn} = sign_extend_16(aot_hw_be16(hw, 0x{lit_off:06X}u));")
        else:
            emit_load(2, f"0x{lit_ea:08X}u", f"{rn} = sign_extend_16(_v);")
    elif cls == "MOVLI":
        lit_ea = u32(((ipc + 4) & ~3) + (d8 << 2))
        lit_off = lit_ea - HIGH_WRAM_BASE
        lit_ln = lit_off >> LINE_SHIFT
        if HIGH_WRAM_BASE <= lit_ea < CODE_HI and lit_ln in blk_lines:
            val32 = struct.unpack(">I", hw[lit_off : lit_off + 4])[0]
            out.append(f"    {rn} = 0x{val32:08X}u;")
        elif HIGH_WRAM_BASE <= lit_ea < HIGH_WRAM_BASE + 0x100000 and (lit_ea & 3) == 0:
            out.append(f"    {rn} = aot_hw_be32(hw, 0x{lit_off:06X}u);")
        else:
            emit_load(4, f"0x{lit_ea:08X}u", f"{rn} = _v;")
    elif cls == "MOVA":
        ea = u32(((ipc + 4) & ~3) + (d8 << 2))
        out.append(f"    r0 = 0x{ea:08X}u;")
    elif cls == "MOVT":
        out.append(f"    {rn} = sr & kShSrT;")

    elif cls == "MOVBS":
        emit_store(1, rn, f"{rm} & 0xFFu")
    elif cls == "MOVWS":
        emit_store(2, rn, f"{rm} & 0xFFFFu")
    elif cls == "MOVLS":
        emit_store(4, rn, rm)
    elif cls == "MOVBL":
        emit_load(1, rm, f"{rn} = sign_extend_8(_v);")
    elif cls == "MOVWL":
        emit_load(2, rm, f"{rn} = sign_extend_16(_v);")
    elif cls == "MOVLL":
        emit_load(4, rm, f"{rn} = _v;")

    elif cls == "MOVBM":
        emit_store(1, "_nea", "_sd", f"const uint32_t _sd = {rm} & 0xFFu; const uint32_t _nea = {rn} - 1u; ", f"{rn} = _nea;")
    elif cls == "MOVWM":
        emit_store(2, "_nea", "_sd", f"const uint32_t _sd = {rm} & 0xFFFFu; const uint32_t _nea = {rn} - 2u; ", f"{rn} = _nea;")
    elif cls == "MOVLM":
        emit_store(4, "_nea", "_sd", f"const uint32_t _sd = {rm}; const uint32_t _nea = {rn} - 4u; ", f"{rn} = _nea;")
    elif cls == "MOVBP":
        inc = f" {rm} += 1u;" if n != m else ""
        emit_load(1, rm, f"{rn} = sign_extend_8(_v);{inc}")
    elif cls == "MOVWP":
        inc = f" {rm} += 2u;" if n != m else ""
        emit_load(2, rm, f"{rn} = sign_extend_16(_v);{inc}")
    elif cls == "MOVLP":
        inc = f" {rm} += 4u;" if n != m else ""
        emit_load(4, rm, f"{rn} = _v;{inc}")

    elif cls == "MOVBS0":
        emit_store(1, f"{rn} + r0", f"{rm} & 0xFFu")
    elif cls == "MOVWS0":
        emit_store(2, f"{rn} + r0", f"{rm} & 0xFFFFu")
    elif cls == "MOVLS0":
        emit_store(4, f"{rn} + r0", rm)
    elif cls == "MOVBL0":
        emit_load(1, f"{rm} + r0", f"{rn} = sign_extend_8(_v);")
    elif cls == "MOVWL0":
        emit_load(2, f"{rm} + r0", f"{rn} = sign_extend_16(_v);")
    elif cls == "MOVLL0":
        emit_load(4, f"{rm} + r0", f"{rn} = _v;")

    elif cls == "MOVLS4":
        emit_store(4, f"{rn} + {d4 * 4}u", rm)
    elif cls == "MOVLL4":
        emit_load(4, f"{rm} + {d4 * 4}u", f"{rn} = _v;")
    elif cls == "MOVBS4":
        emit_store(1, f"{rm} + {d4}u", "r0 & 0xFFu")
    elif cls == "MOVWS4":
        emit_store(2, f"{rm} + {d4 * 2}u", "r0 & 0xFFFFu")
    elif cls == "MOVBL4":
        emit_load(1, f"{rm} + {d4}u", "r0 = sign_extend_8(_v);")
    elif cls == "MOVWL4":
        emit_load(2, f"{rm} + {d4 * 2}u", "r0 = sign_extend_16(_v);")

    elif cls == "MOVBSG":
        emit_store(1, f"gbr + {d8}u", "r0 & 0xFFu")
    elif cls == "MOVWSG":
        emit_store(2, f"gbr + {d8 * 2}u", "r0 & 0xFFFFu")
    elif cls == "MOVLSG":
        emit_store(4, f"gbr + {d8 * 4}u", "r0")
    elif cls == "MOVBLG":
        emit_load(1, f"gbr + {d8}u", "r0 = sign_extend_8(_v);")
    elif cls == "MOVWLG":
        emit_load(2, f"gbr + {d8 * 2}u", "r0 = sign_extend_16(_v);")
    elif cls == "MOVLLG":
        emit_load(4, f"gbr + {d8 * 4}u", "r0 = _v;")

    elif cls == "ADD":
        out.append(f"    {rn} += {rm};")
    elif cls == "ADDI":
        out.append(f"    {rn} += 0x{u32(sext8(d8)):08X}u;")
    elif cls == "ADDC":
        out.append(f"    {{ const uint32_t _t0 = {rn}, _t1 = _t0 + {rm}, _res = _t1 + (sr & kShSrT); {rn} = _res; aot_set_t(sr, (_t0 > _t1) || (_t1 > _res)); }}")
    elif cls == "ADDV":
        out.append(f"    {{ const uint32_t _a = {rn}, _b = {rm}, _res = _a + _b; {rn} = _res; aot_set_t(sr, (((_a ^ _b) & 0x80000000u) == 0u) && (((_a ^ _res) & 0x80000000u) != 0u)); }}")
    elif cls == "SUB":
        out.append(f"    {rn} -= {rm};")
    elif cls == "SUBC":
        out.append(f"    {{ const uint32_t _t0 = {rn}, _t1 = _t0 - {rm}, _res = _t1 - (sr & kShSrT); {rn} = _res; aot_set_t(sr, (_t0 < _t1) || (_t1 < _res)); }}")
    elif cls == "SUBV":
        out.append(f"    {{ const uint32_t _a = {rn}, _b = {rm}, _res = _a - _b; {rn} = _res; aot_set_t(sr, (((_a ^ _b) & 0x80000000u) != 0u) && (((_a ^ _res) & 0x80000000u) != 0u)); }}")
    elif cls == "NEG":
        out.append(f"    {rn} = 0u - {rm};")
    elif cls == "NEGC":
        out.append(f"    {{ const uint32_t _tmp = 0u - {rm}, _res = _tmp - (sr & kShSrT); {rn} = _res; aot_set_t(sr, (0u < _tmp) || (_tmp < _res)); }}")

    elif cls == "AND":
        out.append(f"    {rn} &= {rm};")
    elif cls == "ANDI":
        out.append(f"    r0 &= 0x{d8:02X}u;")
    elif cls in ("ANDM", "ORM", "XORM"):
        op_c = "&" if cls == "ANDM" else ("|" if cls == "ORM" else "^")
        out.append(f"    {{ const uint32_t _ea = gbr + r0; const uint32_t _v = aot_load_slow<1>(cpu_id, m, _ea, 0x{ipc:08X}u, cyc + {cyc_off_expr}, fault);")
        out.append(f"      if (__builtin_expect(!fault.ok, 0)) {{ {fault_stmt()} }}")
        out.append(f"      const int _st = aot_store_slow<1>(cpu_id, m, _ea, (_v {op_c} 0x{d8:02X}u) & 0xFFu, 0x{ipc:08X}u, cyc + {cyc_off_expr}, fault);")
        out.append(f"      if (__builtin_expect(_st != 0, 0)) {{ if (__builtin_expect(_st < 0, 0)) {{ {fault_stmt()} }} {stop_stmt()} }} }}")
    elif cls == "OR":
        out.append(f"    {rn} |= {rm};")
    elif cls == "ORI":
        out.append(f"    r0 |= 0x{d8:02X}u;")
    elif cls == "XOR":
        out.append(f"    {rn} ^= {rm};")
    elif cls == "XORI":
        out.append(f"    r0 ^= 0x{d8:02X}u;")
    elif cls == "NOT":
        out.append(f"    {rn} = ~{rm};")

    elif cls == "TST":
        out.append(f"    aot_set_t(sr, ({rn} & {rm}) == 0u);")
    elif cls == "TSTI":
        out.append(f"    aot_set_t(sr, (r0 & 0x{d8:02X}u) == 0u);")
    elif cls == "TSTM":
        emit_load(1, "gbr + r0", f"aot_set_t(sr, (_v & 0x{d8:02X}u) == 0u);")
    elif cls == "CMPEQ":
        out.append(f"    aot_set_t(sr, {rn} == {rm});")
    elif cls == "CMPGE":
        out.append(f"    aot_set_t(sr, to_s32({rn}) >= to_s32({rm}));")
    elif cls == "CMPGT":
        out.append(f"    aot_set_t(sr, to_s32({rn}) > to_s32({rm}));")
    elif cls == "CMPHI":
        out.append(f"    aot_set_t(sr, {rn} > {rm});")
    elif cls == "CMPHS":
        out.append(f"    aot_set_t(sr, {rn} >= {rm});")
    elif cls == "CMPIM":
        out.append(f"    aot_set_t(sr, r0 == 0x{u32(sext8(d8)):08X}u);")
    elif cls == "CMPPL":
        out.append(f"    aot_set_t(sr, to_s32({rn}) > 0);")
    elif cls == "CMPPZ":
        out.append(f"    aot_set_t(sr, to_s32({rn}) >= 0);")
    elif cls == "DT":
        out.append(f"    {rn} -= 1u; aot_set_t(sr, {rn} == 0u);")

    elif cls == "EXTSB":
        out.append(f"    {rn} = sign_extend_8({rm});")
    elif cls == "EXTSW":
        out.append(f"    {rn} = sign_extend_16({rm});")
    elif cls == "EXTUB":
        out.append(f"    {rn} = {rm} & 0xFFu;")
    elif cls == "EXTUW":
        out.append(f"    {rn} = {rm} & 0xFFFFu;")
    elif cls == "SWAPB":
        out.append(f"    {{ const uint32_t _v = {rm}; {rn} = (_v & 0xFFFF0000u) | ((_v & 0xFFu) << 8) | ((_v >> 8) & 0xFFu); }}")
    elif cls == "SWAPW":
        out.append(f"    {{ const uint32_t _v = {rm}; {rn} = (_v << 16) | (_v >> 16); }}")
    elif cls == "XTRCT":
        out.append(f"    {rn} = (({rm} & 0xFFFFu) << 16) | ({rn} >> 16);")

    elif cls in ("SHLL", "SHAL"):
        out.append(f"    aot_set_t(sr, ({rn} >> 31) != 0u); {rn} <<= 1;")
    elif cls == "SHLR":
        out.append(f"    aot_set_t(sr, ({rn} & 1u) != 0u); {rn} >>= 1;")
    elif cls == "SHAR":
        out.append(f"    aot_set_t(sr, ({rn} & 1u) != 0u); {rn} = arith_shift_right1({rn});")
    elif cls == "ROTL":
        out.append(f"    {{ const uint32_t _b = {rn} >> 31; aot_set_t(sr, _b != 0u); {rn} = ({rn} << 1) | _b; }}")
    elif cls == "ROTR":
        out.append(f"    {{ const uint32_t _b = {rn} & 1u; aot_set_t(sr, _b != 0u); {rn} = ({rn} >> 1) | (_b << 31); }}")
    elif cls == "ROTCL":
        out.append(f"    {{ const uint32_t _b = {rn} >> 31; {rn} = ({rn} << 1) | (sr & kShSrT); aot_set_t(sr, _b != 0u); }}")
    elif cls == "ROTCR":
        out.append(f"    {{ const uint32_t _c = (sr & kShSrT) << 31; aot_set_t(sr, ({rn} & 1u) != 0u); {rn} = ({rn} >> 1) | _c; }}")
    elif cls == "SHLL2":
        out.append(f"    {rn} <<= 2;")
    elif cls == "SHLR2":
        out.append(f"    {rn} >>= 2;")
    elif cls == "SHLL8":
        out.append(f"    {rn} <<= 8;")
    elif cls == "SHLR8":
        out.append(f"    {rn} >>= 8;")
    elif cls == "SHLL16":
        out.append(f"    {rn} <<= 16;")
    elif cls == "SHLR16":
        out.append(f"    {rn} >>= 16;")

    elif cls == "MULL":
        out.append(f"    macl = {rn} * {rm};")
    elif cls == "MULS":
        out.append(f"    macl = static_cast<uint32_t>(static_cast<int64_t>(to_s32(sign_extend_16({rn}))) * static_cast<int64_t>(to_s32(sign_extend_16({rm}))));")
    elif cls == "MULU":
        out.append(f"    macl = ({rn} & 0xFFFFu) * ({rm} & 0xFFFFu);")
    elif cls == "DMULS":
        out.append(f"    {{ const uint64_t _p = static_cast<uint64_t>(static_cast<int64_t>(to_s32({rn})) * static_cast<int64_t>(to_s32({rm}))); mach = static_cast<uint32_t>(_p >> 32); macl = static_cast<uint32_t>(_p); }}")
    elif cls == "DMULU":
        out.append(f"    {{ const uint64_t _p = static_cast<uint64_t>({rn}) * static_cast<uint64_t>({rm}); mach = static_cast<uint32_t>(_p >> 32); macl = static_cast<uint32_t>(_p); }}")
    elif cls == "MAC_L":
        # Fast inline High WRAM non-saturated MAC.L path, fallback to noinline aot_mac_l_slow
        out.append(
            f"    {{ const uint32_t _hn = ({rn} & 0xDFFFFFFFu) - kHighWramBase, _hm = ({rm} & 0xDFFFFFFFu) - kHighWramBase;"
        )
        if n != m:
            out.append(
                f"      if (__builtin_expect(((_hn | (_hn << 30)) < kHighWramSize) & ((_hm | (_hm << 30)) < kHighWramSize) & ((sr & kShSrS) == 0u), 1)) {{"
            )
            out.append(
                f"        const int64_t _p = static_cast<int64_t>(to_s32(aot_hw_be32(hw, _hn))) * static_cast<int64_t>(to_s32(aot_hw_be32(hw, _hm)));"
            )
            out.append(
                f"        {rn} += 4u; {rm} += 4u; const uint64_t _tot = ((static_cast<uint64_t>(mach) << 32) | macl) + static_cast<uint64_t>(_p);"
            )
            out.append(
                f"        mach = static_cast<uint32_t>(_tot >> 32); macl = static_cast<uint32_t>(_tot);"
            )
            out.append(f"      }} else {{")
            out.append(
                f"        if (__builtin_expect(!aot_mac_l_slow(cpu_id, m, hw, {rn}, {rm}, mach, macl, sr, 0x{ipc:08X}u, cyc + {cyc_off_expr}, fault), 0)) {{ {fault_stmt()} }}"
            )
            out.append(f"      }} }}")
        else:
            out.append(
                f"      if (__builtin_expect(!aot_mac_l_slow(cpu_id, m, hw, {rn}, {rm}, mach, macl, sr, 0x{ipc:08X}u, cyc + {cyc_off_expr}, fault), 0)) {{ {fault_stmt()} }} }}"
            )
    elif cls == "DIV0S":
        out.append(f"    aot_set_flag(sr, kShSrQ, ({rn} & 0x80000000u) != 0u); aot_set_flag(sr, kShSrM, ({rm} & 0x80000000u) != 0u); aot_set_t(sr, (({rm} ^ {rn}) & 0x80000000u) != 0u);")
    elif cls == "DIV0U":
        out.append("    sr &= ~(kShSrQ | kShSrM | kShSrT);")
    elif cls == "DIV1":
        out.append(f"    {rn} = div1_step_core({rn}, {rm}, sr);")
    else:
        raise ValueError(f"Unhandled SH-2 op class in emit: {cls} (op=0x{op:04X} @ 0x{ipc:08X})")


def emit_cluster_fn(cid: int, cluster, hw: bytes) -> str:
    out = []
    local_map = {blk.start_pc: blk for blk in cluster}
    (
        used_r,
        written_r,
        uses_sr,
        writes_sr,
        uses_pr,
        writes_pr,
        uses_gbr,
        writes_gbr,
        uses_mach,
        writes_mach,
        uses_macl,
        writes_macl,
    ) = analyze_cluster_regs(cluster)

    out.append(
        f"static bool aot_cluster_{cid}(int cpu_id, Sh2CpuState& cpu, StvMachineState& m, "
        f"uint32_t entry_id, int& steps_left, uint64_t cycle_limit, BusAccessFault& fault) {{"
    )
    out.append("    uint8_t* __restrict const hw = m.inner.high_wram.data();")
    out.append("    (void)hw; (void)fault;")
    for r in used_r:
        out.append(f"    uint32_t r{r} = cpu.r[{r}];")
    if uses_sr:
        out.append("    uint32_t sr = cpu.sr;")
    if uses_pr:
        out.append("    uint32_t pr = cpu.pr;")
    if uses_gbr:
        out.append("    uint32_t gbr = cpu.gbr;")
    if uses_mach:
        out.append("    uint32_t mach = cpu.mach;")
    if uses_macl:
        out.append("    uint32_t macl = cpu.macl;")
    out.append("    uint64_t cyc = cpu.cycles;")
    out.append("    uint64_t ret = cpu.retired;")
    out.append("    uint32_t cur_pc = cpu.cur_pc;")
    out.append("    uint32_t pc = cpu.pc;")
    out.append("    uint32_t fault_delay = 0u;")
    out.append("    (void)fault_delay;")
    out.append("    switch (entry_id) {")
    for idx, blk in enumerate(cluster):
        out.append(f"        case {idx}u: goto bb_{blk.start_pc:08X};")
    out.append("        default: return false;")
    out.append("    }")

    # Pre-scan which blocks get jumped to via fast validated label (same/subset lines)
    fast_label_needed = set()
    for blk in cluster:
        if blk.term_kind == "STOP_AFTER":
            continue
        src_lines = set(blk.lines)
        for tgt in (blk.target_pc, blk.fallthrough_pc):
            if tgt is not None and tgt in local_map and tgt not in HLE_ENTRY_PCS and set(local_map[tgt].lines).issubset(src_lines):
                fast_label_needed.add(tgt)

    def emit_jump_to(src_lines: set, target_pc: int, indent="    "):
        if target_pc in local_map and target_pc not in HLE_ENTRY_PCS:
            if set(local_map[target_pc].lines).issubset(src_lines):
                out.append(f"{indent}goto bb_{target_pc:08X}_v;")
            else:
                out.append(f"{indent}goto bb_{target_pc:08X};")
        else:
            out.append(f"{indent}pc = 0x{target_pc:08X}u; goto exit_chain;")

    for blk in cluster:
        n_steps = len(blk.insns)
        c_max = sum(insn_max_cycles(c) for _, _, c in blk.insns)
        blk_lines = set(blk.lines)

        out.append(f"  bb_{blk.start_pc:08X}:")
        line_cond = " || ".join(f"!aot_line_ok(m, {ln}u)" for ln in blk.lines)
        out.append(f"    if (__builtin_expect({line_cond}, 0)) {{ pc = 0x{blk.start_pc:08X}u; goto exit_stop; }}")
        if blk.start_pc in fast_label_needed:
            out.append(f"  bb_{blk.start_pc:08X}_v:")
        out.append("  {")

        if blk.is_dt_bf_self:
            rname = f"r{blk.dt_reg}"
            bf_pc = blk.insns[1][0]
            out.append(f"    if ({rname} != 1u) {{")
            out.append(f"      const uint32_t _rem = ({rname} == 0u) ? 0xFFFFFFFFu : ({rname} - 1u);")
            out.append(f"      uint32_t _k = static_cast<uint32_t>(steps_left >> 1);")
            out.append(f"      if (cpu_id == 0) {{")
            out.append(f"        const uint64_t _ck = (cyc < cycle_limit) ? ((cycle_limit - cyc) >> 2) : 0ULL;")
            out.append(f"        if (_ck < _k) _k = static_cast<uint32_t>(_ck);")
            out.append(f"      }}")
            out.append(f"      if (_rem < _k) _k = _rem;")
            out.append(f"      if (_k > 0u) {{")
            out.append(f"        {rname} -= _k; sr &= ~kShSrT;")
            out.append(f"        steps_left -= static_cast<int>(_k << 1);")
            out.append(f"        ret += static_cast<uint64_t>(_k << 1);")
            out.append(f"        cyc += static_cast<uint64_t>(_k << 2);")
            out.append(f"        cur_pc = 0x{bf_pc:08X}u;")
            out.append(f"      }}")
            out.append(f"    }}")

        out.append(
            f"    if (__builtin_expect(steps_left < {n_steps} || (cpu_id == 0 && cyc + {c_max}ULL > cycle_limit), 0)) "
            f"{{ pc = 0x{blk.start_pc:08X}u; goto exit_stop; }}"
        )

        if blk.term_kind == "FALLTHROUGH" or blk.term_kind == "STOP_AFTER":
            cyc_acc = 0
            for k, (ipc, op, cls) in enumerate(blk.insns):
                ic = insn_base_cycles(cls)
                prev_pc = blk.insns[k - 1][0] if k > 0 else 0
                emit_insn_code(
                    out, hw, blk_lines, ipc, op, cls, k, f"{cyc_acc}ULL", prev_pc,
                    False, "0u", f"0x{u32(ipc + 2):08X}u", f"{cyc_acc + ic}ULL", k + 1,
                )
                cyc_acc += ic
            last_pc = blk.insns[-1][0]
            out.append(f"    steps_left -= {n_steps}; ret += {n_steps}ULL; cyc += {cyc_acc}ULL; cur_pc = 0x{last_pc:08X}u;")
            if blk.term_kind == "STOP_AFTER":
                out.append(f"    pc = 0x{blk.fallthrough_pc:08X}u; goto exit_stop;")
            else:
                emit_jump_to(blk_lines, blk.fallthrough_pc)

        elif blk.term_kind == "COND_NODELAY":
            cyc_acc = 0
            for k, (ipc, op, cls) in enumerate(blk.insns[:-1]):
                ic = insn_base_cycles(cls)
                prev_pc = blk.insns[k - 1][0] if k > 0 else 0
                emit_insn_code(
                    out, hw, blk_lines, ipc, op, cls, k, f"{cyc_acc}ULL", prev_pc,
                    False, "0u", f"0x{u32(ipc + 2):08X}u", f"{cyc_acc + ic}ULL", k + 1,
                )
                cyc_acc += ic
            br_pc, br_op, br_cls = blk.insns[-1]
            cond = "((sr & kShSrT) != 0u)" if br_cls == "BT" else "((sr & kShSrT) == 0u)"
            out.append(f"    steps_left -= {n_steps}; ret += {n_steps}ULL; cur_pc = 0x{br_pc:08X}u;")
            out.append(f"    if ({cond}) {{")
            out.append(f"      cyc += {cyc_acc + 3}ULL;")
            emit_jump_to(blk_lines, blk.target_pc, "      ")
            out.append(f"    }} else {{")
            out.append(f"      cyc += {cyc_acc + 1}ULL;")
            emit_jump_to(blk_lines, blk.fallthrough_pc, "      ")
            out.append(f"    }}")

        elif blk.term_kind == "UNCOND_DELAY":
            cyc_acc = 0
            for k, (ipc, op, cls) in enumerate(blk.insns[:-2]):
                ic = insn_base_cycles(cls)
                prev_pc = blk.insns[k - 1][0] if k > 0 else 0
                emit_insn_code(
                    out, hw, blk_lines, ipc, op, cls, k, f"{cyc_acc}ULL", prev_pc,
                    False, "0u", f"0x{u32(ipc + 2):08X}u", f"{cyc_acc + ic}ULL", k + 1,
                )
                cyc_acc += ic
            br_pc, br_op, br_cls = blk.insns[-2]
            sl_pc, sl_op, sl_cls = blk.insns[-1]
            bn = (br_op >> 8) & 0xF
            sl_cyc = insn_base_cycles(sl_cls)

            if br_cls == "RTS":
                out.append("    const uint32_t _tgt = pr;")
                tgt_expr = "_tgt"
            elif br_cls == "JMP":
                out.append(f"    const uint32_t _tgt = r{bn};")
                tgt_expr = "_tgt"
            elif br_cls == "JSR":
                out.append(f"    const uint32_t _tgt = r{bn}; pr = 0x{u32(br_pc + 4):08X}u;")
                tgt_expr = "_tgt"
            elif br_cls == "BRA":
                tgt_expr = f"0x{blk.target_pc:08X}u"
            elif br_cls == "BSR":
                out.append(f"    pr = 0x{u32(br_pc + 4):08X}u;")
                tgt_expr = f"0x{blk.target_pc:08X}u"
            elif br_cls == "BRAF":
                out.append(f"    const uint32_t _tgt = 0x{u32(br_pc + 4):08X}u + r{bn};")
                tgt_expr = "_tgt"
            elif br_cls == "BSRF":
                out.append(f"    const uint32_t _tgt = 0x{u32(br_pc + 4):08X}u + r{bn}; pr = 0x{u32(br_pc + 4):08X}u;")
                tgt_expr = "_tgt"
            else:
                raise ValueError(f"Unexpected UNCOND_DELAY op: {br_cls}")

            emit_insn_code(
                out, hw, blk_lines, sl_pc, sl_op, sl_cls, n_steps - 1, f"{cyc_acc + 2}ULL",
                br_pc, True, tgt_expr, tgt_expr, f"{cyc_acc + 2 + sl_cyc}ULL", n_steps,
            )
            out.append(
                f"    steps_left -= {n_steps}; ret += {n_steps}ULL; cyc += {cyc_acc + 2 + sl_cyc}ULL; cur_pc = 0x{sl_pc:08X}u;"
            )
            if blk.target_pc is not None:
                emit_jump_to(blk_lines, blk.target_pc)
            else:
                out.append(f"    pc = {tgt_expr}; goto exit_chain;")

        elif blk.term_kind == "COND_DELAY":
            cyc_acc = 0
            for k, (ipc, op, cls) in enumerate(blk.insns[:-2]):
                ic = insn_base_cycles(cls)
                prev_pc = blk.insns[k - 1][0] if k > 0 else 0
                emit_insn_code(
                    out, hw, blk_lines, ipc, op, cls, k, f"{cyc_acc}ULL", prev_pc,
                    False, "0u", f"0x{u32(ipc + 2):08X}u", f"{cyc_acc + ic}ULL", k + 1,
                )
                cyc_acc += ic
            br_pc, br_op, br_cls = blk.insns[-2]
            sl_pc, sl_op, sl_cls = blk.insns[-1]
            sl_cyc = insn_base_cycles(sl_cls)
            cond = "((sr & kShSrT) != 0u)" if br_cls == "BTS" else "((sr & kShSrT) == 0u)"
            out.append(f"    const bool _tk = {cond};")
            out.append(f"    const uint64_t _br_c = _tk ? {cyc_acc + 2}ULL : {cyc_acc + 1}ULL;")
            emit_insn_code(
                out, hw, blk_lines, sl_pc, sl_op, sl_cls, n_steps - 1, "_br_c",
                br_pc, True, f"(_tk ? 0x{blk.target_pc:08X}u : 0u)",
                f"(_tk ? 0x{blk.target_pc:08X}u : 0x{blk.fallthrough_pc:08X}u)",
                f"(_br_c + {sl_cyc}ULL)", n_steps,
            )
            out.append(
                f"    steps_left -= {n_steps}; ret += {n_steps}ULL; cyc += _br_c + {sl_cyc}ULL; cur_pc = 0x{sl_pc:08X}u;"
            )
            out.append("    if (_tk) {")
            emit_jump_to(blk_lines, blk.target_pc, "      ")
            out.append("    } else {")
            emit_jump_to(blk_lines, blk.fallthrough_pc, "      ")
            out.append("    }")
        else:
            raise ValueError(f"Unknown term_kind: {blk.term_kind}")

        out.append("  }")

    def emit_writeback(indent="    "):
        for r in written_r:
            out.append(f"{indent}cpu.r[{r}] = r{r};")
        if writes_sr:
            out.append(f"{indent}cpu.sr = sr;")
        if writes_pr:
            out.append(f"{indent}cpu.pr = pr;")
        if writes_gbr:
            out.append(f"{indent}cpu.gbr = gbr;")
        if writes_mach:
            out.append(f"{indent}cpu.mach = mach;")
        if writes_macl:
            out.append(f"{indent}cpu.macl = macl;")
        out.append(f"{indent}cpu.pc = pc; cpu.cur_pc = cur_pc; cpu.retired = ret; cpu.cycles = cyc;")

    body_str = "\n".join(out)
    if "goto exit_chain;" in body_str:
        out.append("  exit_chain:")
        emit_writeback()
        out.append("    return true;")
    if "goto exit_stop;" in body_str:
        out.append("  exit_stop:")
        emit_writeback()
        out.append("    return false;")
    if "goto exit_fault;" in body_str:
        out.append("  exit_fault:")
        emit_writeback()
        out.append("    cpu.delay = fault_delay;")
        out.append("    return false;")
    out.append("}")
    return "\n".join(out)


def verify_patch_hash_collisions(hw: bytes, line_hashes):
    patches = [
        ("GK1_1", 0x035D2E, b"\xE0\x01"),
        ("GK1_2", 0x032F04, b"\x00\x09"),
        ("GK1_3", 0x035A84, b"\x00\x09\x00\x09"),
        ("GK1_4", 0x0305AC, b"\x8B\xF7"),
        ("GK1_5", 0x02FEB8, b"\x8B\xF8"),
        ("COOP_1", 0x01418E, b"\xA0\xF3\x00\x09"),
        ("COOP_2", 0x00CF2E, b"\x00\x09"),
        ("COOP_3", 0x00CF42, b"\xA0\x0A\x00\x09"),
        ("COOP_4", 0x00CF84, b"\x06\x07\x65\x8F"),
        ("HARD_1", 0x03EB84, b"\xE0\x1F"),
        ("HARD_2", 0x03F872, b"\xEE\x1F"),
        ("COPA_EURO", 0x0050F8, b"\x00\x0B\x60\x43"),
        ("COPA_P2CUR", 0x005906, b"\xE1\x03"),
        ("COPA_R1", 0x0060A2, b"\xEC\x07"),
        ("COPA_R2", 0x007D02, b"\x88\x06"),
        ("COPA_R3", 0x0080C6, b"\xE2\x06"),
        ("COPA_R4", 0x0529BC, b"\x88\x06"),
        ("COPA_SP3_1", 0x04EE32, b"\x00\x00"),
        ("COPA_SP3_2", 0x04EE6C, b"\x00\x09"),
        ("COPA_SP3_3", 0x04EE7E, b"\x00\x09"),
        ("COPA_SP3_4", 0x04EFE0, b"\x06\x04\x0D\x46"),
    ]
    for name, off, val in patches:
        ln = off >> LINE_SHIFT
        if line_hashes[ln] == 0:
            continue
        buf = bytearray(hw[ln * LINE_SIZE : (ln + 1) * LINE_SIZE])
        rel = off & (LINE_SIZE - 1)
        if buf[rel : rel + len(val)] == val:
            continue
        buf[rel : rel + len(val)] = val
        ph = fnv1a32_line(bytes(buf))
        if ph == line_hashes[ln]:
            raise RuntimeError(f"HASH COLLISION on {name} (line {ln}): 0x{ph:08X}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--high-wram", default="port/data/sh2-aot-work/classic98_clean_high_wram.bin")
    ap.add_argument("--profile-dir", default="port/data/sh2-aot-work")
    ap.add_argument("--out-dir", default="port/src")
    ap.add_argument("--max-insns", type=int, default=11000)
    ap.add_argument("--cluster-insns", type=int, default=260)
    ap.add_argument("--max-block-insns", type=int, default=6)
    ap.add_argument("--hot-block-insns", type=int, default=4)
    args = ap.parse_args()

    hw = pathlib.Path(args.high_wram).read_bytes()
    assert len(hw) == 0x100000, f"Expected 1 MiB high_wram, got {len(hw)}"

    pdir = pathlib.Path(args.profile_dir)
    ep = struct.unpack(f"<{CODE_SLOTS}Q", (pdir / "prof_pc_hits_ep120.bin").read_bytes())
    tw = struct.unpack(f"<{CODE_SLOTS}Q", (pdir / "prof_pc_hits_twcr_full.bin").read_bytes())
    ti = struct.unpack(f"<{CODE_SLOTS}Q", (pdir / "prof_pc_hits_title.bin").read_bytes())
    bb_hits = struct.unpack(f"<{CODE_SLOTS}Q", (pdir / "prof_bb_hits_comb.bin").read_bytes())
    l64w = struct.unpack(f"<{NUM_LINES}Q", (pdir / "prof_l64_writes_comb.bin").read_bytes())

    blocks, total_insns, (cov_ep, cov_tw, cov_ti) = extract_blocks(
        hw, ep, tw, ti, bb_hits, l64w, args.max_block_insns, args.hot_block_insns, args.max_insns
    )
    clusters = build_clusters(blocks, args.cluster_insns)

    used_lines = set()
    for blk in blocks:
        used_lines.update(blk.lines)
    line_hashes = [0] * NUM_LINES
    for ln in used_lines:
        line_hashes[ln] = fnv1a32_line(hw[ln * LINE_SIZE : (ln + 1) * LINE_SIZE])
    verify_patch_hash_collisions(hw, line_hashes)

    num_blocks = len(blocks)
    table_bits = 12
    while (1 << table_bits) < num_blocks * 2.4:
        table_bits += 1
    table_size = 1 << table_bits
    hash_shift = 32 - table_bits
    hash_mask = table_size - 1
    table = [(0, 0, 0)] * table_size
    max_probe = 0
    for cid, cl in enumerate(clusters):
        for lid, blk in enumerate(cl):
            h = (((blk.start_pc >> 1) * 2654435761) & 0xFFFFFFFF) >> hash_shift
            probes = 1
            while table[h][0] != 0:
                h = (h + 1) & hash_mask
                probes += 1
            table[h] = (blk.start_pc, cid, lid)
            if probes > max_probe:
                max_probe = probes

    out_dir = pathlib.Path(args.out_dir)
    hpp_text = """// Auto-generated by scripts/gen_sh2_aot.py — DO NOT EDIT BY HAND.
// Profiled Offline Static AOT Recompiler for Tecmo World Cup '98 (twcup98) SH-2 EPRG.
#pragma once

#include "sh2_core.hpp"

#include <cstdint>

namespace twc98 {

class StvMachineState;

// Returns expected FNV-1a32 hash for a 64-byte High WRAM line in [0x06000000, 0x06070000),
// or 0 if no AOT basic blocks reside on that line.
uint32_t sh2_aot_expected_line_hash(uint32_t line_idx) noexcept;

// Executes up to `max_steps` SH-2 instructions via statically AOT-compiled hot basic blocks,
// strictly preserving cycle_limit (CPU 0), dirty-page validity, MMIO boundaries, hooks, and faults.
// Returns the exact number of retired SH-2 instructions (0 if PC is uncompiled/invalid/boundary).
uint32_t execute_sh2_aot_quantum(int cpu_id, Sh2CpuState& cpu, StvMachineState& m,
                                 int max_steps, uint64_t cycle_limit, BusAccessFault& fault);

}  // namespace twc98
"""
    (out_dir / "sh2_aot_twcup98.hpp").write_text(hpp_text, encoding="utf-8")

    cpp_parts = []
    cpp_parts.append(
        f"""// Auto-generated by scripts/gen_sh2_aot.py — DO NOT EDIT BY HAND.
// Blocks: {num_blocks}, Instructions: {total_insns}, Clusters: {len(clusters)}, Active 64B lines: {len(used_lines)}
// Profile coverage: ep120={cov_ep:.2f}%, twcr={cov_tw:.2f}%, title_ts_view={cov_ti:.2f}%
#include "sh2_aot_twcup98.hpp"

#include "sh2_hle_sgl.hpp"
#include "sh2_step_inl.hpp"
#include "stv_machine.hpp"

#include <cstdint>
#include <cstring>

namespace twc98 {{
namespace {{

inline void aot_set_t(uint32_t& sr, bool on) noexcept {{
    if (on) sr |= kShSrT;
    else sr &= ~kShSrT;
}}

inline void aot_set_flag(uint32_t& sr, uint32_t bit, bool on) noexcept {{
    if (on) sr |= bit;
    else sr &= ~bit;
}}

__attribute__((always_inline)) inline uint16_t aot_hw_be16(const uint8_t* hw, uint32_t off) noexcept {{
    uint16_t v;
    std::memcpy(&v, hw + off, 2);
    return __builtin_bswap16(v);
}}

__attribute__((always_inline)) inline uint32_t aot_hw_be32(const uint8_t* hw, uint32_t off) noexcept {{
    uint32_t v;
    std::memcpy(&v, hw + off, 4);
    return __builtin_bswap32(v);
}}

__attribute__((always_inline)) inline void aot_hw_st16(uint8_t* hw, uint32_t off, uint32_t val) noexcept {{
    const uint16_t v = __builtin_bswap16(static_cast<uint16_t>(val));
    std::memcpy(hw + off, &v, 2);
}}

__attribute__((always_inline)) inline void aot_hw_st32(uint8_t* hw, uint32_t off, uint32_t val) noexcept {{
    const uint32_t v = __builtin_bswap32(val);
    std::memcpy(hw + off, &v, 4);
}}

__attribute__((always_inline)) inline bool aot_line_ok(StvMachineState& m, uint32_t ln) noexcept {{
    const uint32_t wi = ln >> 5;
    const uint32_t mask = 1u << (ln & 31u);
    if (__builtin_expect((m.aot_page_valid_bits[wi] & mask) != 0u, 1)) return true;
    return m.revalidate_aot_page(ln);
}}

__attribute__((always_inline)) inline bool aot_mark_code_write(StvMachineState& m, uint32_t h_off) noexcept {{
    const uint32_t ln = h_off >> StvMachineState::kAotLineShift;
    const uint32_t wi = ln >> 5;
    const uint32_t mask = 1u << (ln & 31u);
    const bool was_valid = (m.aot_page_valid_bits[wi] & mask) != 0u;
    m.code_dirty_bits[wi] |= mask;
    m.aot_page_valid_bits[wi] &= ~mask;
    return was_valid;
}}

template <uint8_t W>
__attribute__((noinline)) uint32_t aot_load_slow(
    int cpu_id, StvMachineState& m, uint32_t ea,
    uint32_t instr_pc, uint64_t insn_cyc, BusAccessFault& fault) {{
    if constexpr (W == 1u) {{
        if (__builtin_expect(ea == 0xFFFFFE11u, 0)) {{
            return m.inner.frt[static_cast<size_t>(cpu_id)].read_ftcsr(insn_cyc);
        }}
    }}
    if constexpr (W == 4u) {{
        if (__builtin_expect(ea >= 0xFFFFFF00u && ea <= 0xFFFFFF1Fu, 0)) {{
            const uint32_t v = m.inner.divu[static_cast<size_t>(cpu_id)].read_reg(ea, W, fault);
            if (__builtin_expect(!fault.ok, 0)) fault.pc = instr_pc;
            return v;
        }}
    }}
    if (__builtin_expect(ea < 0x40000000u, 1)) {{
        const uint32_t cea = ea & 0x07FFFFFFu;
        const uint32_t h_off = cea - kHighWramBase;
        if (h_off < kHighWramSize && ((cea & (W - 1u)) == 0u)) {{
            const uint8_t* p = m.inner.high_wram.data() + h_off;
            if constexpr (W == 4u) {{ uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v); }}
            else if constexpr (W == 2u) {{ uint16_t v; std::memcpy(&v, p, 2); return __builtin_bswap16(v); }}
            else {{ return p[0]; }}
        }}
        const uint32_t l_off = cea - kLowWramBase;
        if (__builtin_expect(l_off < kLowWramSize && ((cea & (W - 1u)) == 0u), 1)) {{
            const uint8_t* p = m.inner.low_wram.data() + l_off;
            if constexpr (W == 4u) {{ uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v); }}
            else if constexpr (W == 2u) {{ uint16_t v; std::memcpy(&v, p, 2); return __builtin_bswap16(v); }}
            else {{ return p[0]; }}
        }}
        const uint32_t c_rel = cea - 0x02000000u;
        if (c_rel < (StvMachineState::kCartProtStart - 0x02000000u) && !m.cart_prot_enabled && m.has_cart &&
            m.smpc_gamebank_select == 0u && ((cea & (W - 1u)) == 0u)) {{
            ++m.cart_reads;
            if (__builtin_expect(c_rel + W <= StvMachineState::kCartPopulatedSize, 1)) {{
                const uint8_t* p = m.cart_rom + c_rel;
                if constexpr (W == 4u) {{ uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v); }}
                else if constexpr (W == 2u) {{ uint16_t v; std::memcpy(&v, p, 2); return __builtin_bswap16(v); }}
                else {{ return p[0]; }}
            }}
            return 0u;
        }}
    }}
    m.cpus[cpu_id].cycles = insn_cyc;
    return m.read(cpu_id, ea, W, instr_pc, insn_cyc, fault);
}}

template <uint8_t W>
__attribute__((noinline)) int aot_store_slow(
    int cpu_id, StvMachineState& m, uint32_t ea, uint32_t val,
    uint32_t instr_pc, uint64_t insn_cyc, BusAccessFault& fault) {{
    if constexpr (W == 4u) {{
        if (__builtin_expect(ea >= 0xFFFFFF00u && ea <= 0xFFFFFF1Fu, 0)) {{
            m.last_write_ea = ea;
            m.last_write_val = val;
            m.inner.divu[static_cast<size_t>(cpu_id)].write_reg(ea, W, val, fault);
            if (__builtin_expect(!fault.ok, 0)) {{ fault.pc = instr_pc; return -1; }}
            return 0;
        }}
    }}
    if (__builtin_expect(ea < 0x40000000u, 1)) {{
        const uint32_t cea = ea & 0x07FFFFFFu;
        const uint32_t h_off = cea - kHighWramBase;
        if (h_off < kHighWramSize && ((cea & (W - 1u)) == 0u)) {{
            m.last_write_ea = cea;
            m.last_write_val = val;
            uint8_t* p = m.inner.high_wram.data() + h_off;
            if constexpr (W == 4u) {{ const uint32_t v = __builtin_bswap32(val); std::memcpy(p, &v, 4); }}
            else if constexpr (W == 2u) {{ const uint16_t v = __builtin_bswap16(static_cast<uint16_t>(val)); std::memcpy(p, &v, 2); }}
            else {{ p[0] = static_cast<uint8_t>(val); }}
            if (h_off < StvMachineState::kAotCodeRangeBytes) {{
                const uint32_t ln = h_off >> StvMachineState::kAotLineShift;
                const uint32_t wi = ln >> 5;
                const uint32_t mask = 1u << (ln & 31u);
                const bool was_valid = (m.aot_page_valid_bits[wi] & mask) != 0u;
                m.code_dirty_bits[wi] |= mask;
                m.aot_page_valid_bits[wi] &= ~mask;
                if (__builtin_expect(was_valid, 0)) return 1;
            }}
            return 0;
        }}
        const uint32_t l_off = cea - kLowWramBase;
        if (l_off < kLowWramSize && ((cea & (W - 1u)) == 0u)) {{
            m.last_write_ea = cea;
            m.last_write_val = val;
            uint8_t* p = m.inner.low_wram.data() + l_off;
            if constexpr (W == 4u) {{ const uint32_t v = __builtin_bswap32(val); std::memcpy(p, &v, 4); }}
            else if constexpr (W == 2u) {{ const uint16_t v = __builtin_bswap16(static_cast<uint16_t>(val)); std::memcpy(p, &v, 2); }}
            else {{ p[0] = static_cast<uint8_t>(val); }}
            return 0;
        }}
    }}
    // Flush exact instruction cycle before slow write (critical for SMPC SSHON reading cpus[0].cycles!)
    m.cpus[cpu_id].cycles = insn_cyc;
    m.write(cpu_id, ea, W, val, instr_pc, insn_cyc, fault);
    if (__builtin_expect(!fault.ok, 0)) return -1;
    if (!is_sh2_cache_area(ea)) {{
        const uint32_t cea = canonicalize_stv_addr(ea);
        if ((cea >= 0xFFFFFF00u && cea <= 0xFFFFFF1Fu) ||
            (cea >= 0x05C00000u && cea < 0x05C80000u) ||
            (cea >= 0x05E00000u && cea < 0x05E80000u) ||
            (cea >= 0x05F00000u && cea < 0x05F01000u) ||
            (cea >= 0x05F80000u && cea < 0x05F80200u)) {{
            return 0;
        }}
    }}
    return 1;
}}

__attribute__((noinline)) bool aot_mac_l_slow(
    int cpu_id, StvMachineState& m, const uint8_t* hw,
    uint32_t& rn, uint32_t& rm, uint32_t& mach, uint32_t& macl, uint32_t sr,
    uint32_t instr_pc, uint64_t insn_cyc, BusAccessFault& fault) {{
    (void)hw;
    const uint32_t raw_n = aot_load_slow<4>(cpu_id, m, rn, instr_pc, insn_cyc, fault);
    if (__builtin_expect(!fault.ok, 0)) return false;
    rn += 4u;
    const uint32_t raw_m = aot_load_slow<4>(cpu_id, m, rm, instr_pc, insn_cyc, fault);
    if (__builtin_expect(!fault.ok, 0)) {{
        rn -= 4u;
        return false;
    }}
    rm += 4u;
    const int64_t prod = static_cast<int64_t>(to_s32(raw_n)) * static_cast<int64_t>(to_s32(raw_m));
    if ((sr & kShSrS) != 0u) {{
        const int64_t mach_sext = to_s32(sign_extend_16(mach));
        const int64_t acc = (mach_sext * (1LL << 32)) + static_cast<int64_t>(macl);
        const int64_t tot = acc + prod;
        constexpr int64_t kSatMin = -(1LL << 47);
        constexpr int64_t kSatMax = (1LL << 47) - 1LL;
        if (tot < kSatMin) {{
            mach = 0x00008000u;
            macl = 0x00000000u;
        }} else if (tot > kSatMax) {{
            mach = 0x00007FFFu;
            macl = 0xFFFFFFFFu;
        }} else {{
            const uint64_t u = static_cast<uint64_t>(tot);
            mach = static_cast<uint32_t>(u >> 32);
            macl = static_cast<uint32_t>(u & 0xFFFFFFFFu);
        }}
    }} else {{
        const uint64_t acc = (static_cast<uint64_t>(mach) << 32) | macl;
        const uint64_t tot = acc + static_cast<uint64_t>(prod);
        mach = static_cast<uint32_t>(tot >> 32);
        macl = static_cast<uint32_t>(tot & 0xFFFFFFFFu);
    }}
    return true;
}}

// 64-byte line FNV-1a32 hashes for [0x06000000, 0x06070000) baked from clean Classic '98 EPRG.
static constexpr uint32_t kSh2AotLineHashes[{NUM_LINES}] = {{
"""
    )
    for i in range(0, NUM_LINES, 8):
        row = ", ".join(f"0x{h:08X}u" for h in line_hashes[i : i + 8])
        cpp_parts.append(f"    {row},")
    cpp_parts.append("};\n")

    for cid, cl in enumerate(clusters):
        cpp_parts.append(emit_cluster_fn(cid, cl, hw))
        cpp_parts.append("")

    cpp_parts.append(
        "using AotClusterFn = bool (*)(int, Sh2CpuState&, StvMachineState&, uint32_t, int&, uint64_t, BusAccessFault&);"
    )
    cpp_parts.append(f"static constexpr AotClusterFn kAotClusterFns[{len(clusters)}] = {{")
    for cid in range(len(clusters)):
        cpp_parts.append(f"    &aot_cluster_{cid},")
    cpp_parts.append("};\n")

    cpp_parts.append("struct AotDispatchEntry { uint32_t pc; uint16_t cluster; uint16_t local_id; };")
    cpp_parts.append(f"static constexpr uint32_t kAotHashShift = {hash_shift}u;")
    cpp_parts.append(f"static constexpr uint32_t kAotHashMask = {hash_mask}u;")
    cpp_parts.append(f"static constexpr AotDispatchEntry kAotDispatchTable[{table_size}] = {{")
    for i in range(0, table_size, 4):
        row = ", ".join(f"{{0x{p:08X}u,{c}u,{l}u}}" for p, c, l in table[i : i + 4])
        cpp_parts.append(f"    {row},")
    cpp_parts.append("};\n")

    cpp_parts.append(
        """}  // namespace

uint32_t sh2_aot_expected_line_hash(uint32_t line_idx) noexcept {
    if (line_idx >= StvMachineState::kAotNumLines) return 0u;
    return kSh2AotLineHashes[line_idx];
}

uint32_t execute_sh2_aot_quantum(int cpu_id, Sh2CpuState& cpu, StvMachineState& m,
                                 int max_steps, uint64_t cycle_limit, BusAccessFault& fault) {
    if (max_steps <= 0 || cpu.delay != 0u) return 0u;
    if (cpu_id == 0 && cpu.cycles >= cycle_limit) return 0u;
    int steps_left = max_steps;
    while (steps_left > 0) {
        const uint32_t pc = cpu.pc;
        if (pc < StvMachineState::kAotCodeLo || pc >= StvMachineState::kAotCodeHi || (pc & 1u) != 0u) {
            break;
        }
        if (cpu_id == 0 && cpu.cycles >= cycle_limit) {
            break;
        }
        if (__builtin_expect(pc == kHleSglSlaveVtxLoopPc || pc == kHleSglMasterVtxLoopPc, 0)) {
            const uint32_t hle_steps = execute_sh2_hle_sgl(cpu_id, cpu, m, steps_left, cycle_limit);
            if (__builtin_expect(hle_steps > 0u, 1)) {
                steps_left -= static_cast<int>(hle_steps);
                continue;
            }
        }
        uint32_t h = ((pc >> 1) * 2654435761u) >> kAotHashShift;
        uint16_t cluster = 0xFFFFu;
        uint16_t local_id = 0u;
        for (;;) {
            const AotDispatchEntry& e = kAotDispatchTable[h];
            if (e.pc == pc) {
                cluster = e.cluster;
                local_id = e.local_id;
                break;
            }
            if (e.pc == 0u) {
                return static_cast<uint32_t>(max_steps - steps_left);
            }
            h = (h + 1u) & kAotHashMask;
        }
        if (!kAotClusterFns[cluster](cpu_id, cpu, m, local_id, steps_left, cycle_limit, fault)) {
            break;
        }
    }
    return static_cast<uint32_t>(max_steps - steps_left);
}

}  // namespace twc98
"""
    )

    cpp_path = out_dir / "sh2_aot_twcup98.cpp"
    cpp_path.write_text("\n".join(cpp_parts), encoding="utf-8")
    print(
        f"Generated {cpp_path} ({cpp_path.stat().st_size:,} bytes): "
        f"{num_blocks} blocks, {total_insns} insns, {len(clusters)} clusters, "
        f"{len(used_lines)} active 64B lines, table={table_size} (max_probe={max_probe})"
    )
    print(f"Dynamic coverage: ep120={cov_ep:.2f}%, twcr={cov_tw:.2f}%, title_ts_view={cov_ti:.2f}%")


if __name__ == "__main__":
    main()

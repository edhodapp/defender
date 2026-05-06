#!/usr/bin/env python3
"""
Aggregate the per-PC cycle table written by profile.c (raw uint64_t *
SIM_PC_TABLE_SIZE), parse `avr-objdump -d defender.elf` to map instruction
addresses to functions, and report cycles spent per function for the
profiled workload.

Usage:
    profile_report.py [path_to_defender.elf]   (default: ../build/defender.elf)
"""

import os, sys, re, struct, subprocess, glob

PC_TABLE_SIZE = 16384
PROFILE_BIN   = "/tmp/sim_prof_profile.bin"

def load_profile():
    with open(PROFILE_BIN, "rb") as f:
        data = f.read()
    if len(data) != PC_TABLE_SIZE * 8:
        sys.exit(f"ERROR: {PROFILE_BIN} is {len(data)} bytes "
                 f"(expected {PC_TABLE_SIZE*8})")
    return struct.unpack(f"<{PC_TABLE_SIZE}Q", data)

def parse_objdump(elf_path):
    """Return dict {function_name: [(byte_addr, asm_text), ...]}."""
    out = subprocess.check_output(["avr-objdump", "-d", elf_path], text=True)
    funcs = {}
    cur = None
    func_re  = re.compile(r"^[0-9a-fA-F]+ <([^>]+)>:")
    instr_re = re.compile(r"^\s*([0-9a-fA-F]+):\s+([0-9a-f ]+)\s+(.*)$")
    for line in out.splitlines():
        m = func_re.match(line)
        if m:
            cur = m.group(1)
            funcs.setdefault(cur, [])
            continue
        if cur is None:
            continue
        m = instr_re.match(line)
        if m:
            funcs[cur].append((int(m.group(1), 16), m.group(3).strip()))
    return funcs

# Symbols that are tables / sprite data, not code.
DATA_SYMS = {
    "diamond", "ship_right", "ship_left", "lander_sprite",
    "height_table", "mask_table", "bit_mask_lut",
    "_oled_init_seq", "_oled_init_seq_end",
    "blit_dispatch", "update_dispatch", "render_dispatch",
}

def main():
    elf_path = "../build/defender.elf"
    for arg in sys.argv[1:]:
        if not arg.startswith("-"):
            elf_path = arg

    cycles = load_profile()
    funcs  = parse_objdump(elf_path)

    rows = []
    grand_total = 0
    for name, instrs in funcs.items():
        if name in DATA_SYMS or not instrs:
            continue
        total = sum(cycles[addr // 2] for addr, _ in instrs
                    if addr // 2 < PC_TABLE_SIZE)
        if total > 0:
            rows.append((name, total, len(instrs)))
            grand_total += total

    rows.sort(key=lambda r: -r[1])

    print(f"Profile total: {grand_total:,} cycles across {len(rows)} live functions")
    print()
    print(f"{'Function':<28} {'Cycles':>14} {'%':>7} {'Instrs':>8}")
    print("-" * 60)
    cumulative = 0
    for name, total, n in rows:
        pct = 100 * total / grand_total if grand_total else 0
        cumulative += pct
        marker = " *" if cumulative <= 90 else "  "
        print(f"{name:<28} {total:>14,} {pct:>6.1f}%{marker}{n:>6}")
    print("-" * 60)
    print(f"  (* = cumulative top until 90% of total)")

if __name__ == "__main__":
    main()

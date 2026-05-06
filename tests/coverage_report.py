#!/usr/bin/env python3
"""
Aggregate per-test coverage .bin files (raw uint32_t * 16384, indexed by AVR
instruction word address), parse `avr-objdump -d defender.elf` to enumerate
real instruction addresses + their containing function, and report
instruction (line) coverage per function and overall.

Usage:
    coverage_report.py [path_to_defender.elf]   (default: ../build/defender.elf)
    coverage_report.py --uncovered              also list uncovered instructions
"""

import os, sys, re, struct, subprocess, glob

PC_TABLE_SIZE = 16384
COV_GLOB = "/tmp/sim_cov_*.bin"

def aggregate_coverage():
    """Sum visit counts across every per-test coverage file."""
    visits = [0] * PC_TABLE_SIZE
    files = sorted(glob.glob(COV_GLOB))
    if not files:
        print(f"ERROR: no coverage files matching {COV_GLOB}", file=sys.stderr)
        sys.exit(2)
    for path in files:
        with open(path, "rb") as f:
            data = f.read()
        if len(data) != PC_TABLE_SIZE * 4:
            print(f"WARN: {path} is {len(data)} bytes (expected {PC_TABLE_SIZE*4})",
                  file=sys.stderr)
            continue
        per_test = struct.unpack(f"<{PC_TABLE_SIZE}I", data)
        for i, v in enumerate(per_test):
            visits[i] += v
    return visits, files

def parse_objdump(elf_path):
    """Return dict {function_name: [(byte_addr, asm_text), ...]}."""
    out = subprocess.check_output(["avr-objdump", "-d", elf_path], text=True)
    funcs = {}
    cur = None
    func_re = re.compile(r"^[0-9a-fA-F]+ <([^>]+)>:")
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
            addr = int(m.group(1), 16)
            asm  = m.group(3).strip()
            # Skip data lines that objdump renders as ".word" or invalid
            # decodes — they're sprite/table bytes inside the .text section.
            funcs[cur].append((addr, asm))
    return funcs

def is_data_func(name):
    """Symbols that are tables / sprite data, not code. Skip them in coverage."""
    return name in {
        "diamond", "ship_right", "ship_left", "lander_sprite",
        "height_table", "mask_table", "bit_mask_lut",
        "_oled_init_seq", "_oled_init_seq_end",
        "blit_dispatch", "update_dispatch", "render_dispatch",
    }

def main():
    elf_path = "../build/defender.elf"
    show_uncovered = False
    for arg in sys.argv[1:]:
        if arg == "--uncovered":
            show_uncovered = True
        elif not arg.startswith("-"):
            elf_path = arg

    visits, cov_files = aggregate_coverage()
    funcs = parse_objdump(elf_path)

    print(f"Coverage from {len(cov_files)} test runs:")
    for f in cov_files:
        print(f"  {os.path.basename(f)}")
    print()

    total_instr = 0
    total_hit   = 0
    rows = []
    for name, instrs in sorted(funcs.items()):
        if is_data_func(name):
            continue
        if not instrs:
            continue
        hit = 0
        uncov = []
        for addr, asm in instrs:
            word = addr // 2
            if word < PC_TABLE_SIZE and visits[word] > 0:
                hit += 1
            else:
                uncov.append((addr, asm))
        rows.append((name, len(instrs), hit, uncov))
        total_instr += len(instrs)
        total_hit   += hit

    rows.sort(key=lambda r: (r[2] / r[1] if r[1] else 1.0, r[0]))

    print(f"{'Function':<28} {'Hit/Total':>11} {'Coverage':>10}")
    print("-" * 53)
    for name, n, hit, _ in rows:
        pct = 100 * hit / n if n else 100.0
        print(f"{name:<28} {hit:>5}/{n:<5} {pct:>9.1f}%")
    print("-" * 53)
    if total_instr:
        print(f"{'TOTAL':<28} {total_hit:>5}/{total_instr:<5} "
              f"{100*total_hit/total_instr:>9.1f}%")

    if show_uncovered:
        print("\nUncovered instructions:")
        for name, n, hit, uncov in rows:
            if not uncov:
                continue
            print(f"\n  {name}:  ({len(uncov)} of {n} not hit)")
            for addr, asm in uncov:
                print(f"    {addr:>6x}: {asm}")

if __name__ == "__main__":
    main()

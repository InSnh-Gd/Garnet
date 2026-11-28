#!/usr/bin/env python3
"""Count FP/SIMD instructions in Mach-O x86_64 objects, skipping LC_DATA_IN_CODE ranges.

A linear sweep disassembles clang switch jump tables inside __text as bogus x87/SSE
opcodes. This script counts only lines whose address lies outside every LC_DATA_IN_CODE
range reported by llvm-objdump, logging excluded lines on stderr.

usage: fpu_count.py <llvm-objdump> <obj.o> [...]   -> stdout: count; stderr: per-object detail
"""
import re
import subprocess
import sys

FP_REGISTER = re.compile(r'%(?:[xyz]mm\d+|mm[0-7]|k[0-7]|st)(?![a-z0-9_])')
FP_MNEMONIC = re.compile(r'^(?:f[a-z][a-z0-9]+|wait|vzero(?:all|upper)|emms|v?(?:ld|st)mxcsr|'
                         r'x(?:save(?:c|opt|s)?|rstors?)(?:64)?)$')
INSN = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)([^#]*)')
DIC = re.compile(r'^0x([0-9a-f]+)\s+(\d+)\s+\S+')


def ranges(od, obj):
    out = subprocess.run([od, '--macho', '--data-in-code', obj], capture_output=True, text=True, check=True).stdout
    r = []
    for line in out.splitlines():
        m = DIC.match(line.strip())
        if m:
            start = int(m.group(1), 16)
            r.append((start, start + int(m.group(2))))
    return r


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    od, objs = sys.argv[1], sys.argv[2:]
    total = 0
    for obj in objs:
        rs = ranges(od, obj)
        dis = subprocess.run([od, '-d', '--no-show-raw-insn', obj], capture_output=True, text=True, check=True).stdout
        real = excluded = 0
        for line in dis.splitlines():
            m = INSN.match(line)
            if not m or not (FP_MNEMONIC.fullmatch(m.group(2)) or FP_REGISTER.search(m.group(3))):
                continue
            a = int(m.group(1), 16)
            if any(lo <= a < hi for lo, hi in rs):
                excluded += 1
                print(f'  {obj}: data-in-code, not counted: {line.strip()}', file=sys.stderr)
            else:
                real += 1
                print(f'  {obj}: FP/SIMD: {line.strip()}', file=sys.stderr)
        print(f'{obj}: {len(rs)} data-in-code range(s); FP/SIMD lines {real}, excluded as data {excluded}', file=sys.stderr)
        total += real
    print(total)
    return 0


if __name__ == '__main__':
    sys.exit(main())


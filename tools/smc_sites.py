#!/usr/bin/env python3
"""Finds self-modified bytes in the MVE player's decoder (kpintro.exe) and the values they can take.

The decoder loads a template address into edx and patches single bytes with values taken from
lookup tables (ecx = table base, table indexed by a byte, 4 bytes per entry). Output: one line
per patched instruction: "<insn addr> <offset of patched byte> <value,value,...>", which
tools/x86recomp.py (--smc) turns into a runtime switch over the variants.
usage: smc_sites.py <image> <code-lo> <code-hi> > smc.txt
"""
import collections
import sys

sys.path.insert(0, __import__('os').path.dirname(__file__))
import x86recomp as R
from capstone import x86 as X

im = R.Image(sys.argv[1])
prog = R.Program([im])
lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)

# Linear sweep: instruction boundaries of the decoder region.
starts, a = {}, lo
while a < hi:
    i = prog.decode(a)
    if i is None:
        a += 1
        continue
    starts[a] = i
    a += i.size
owner = {}
for s, i in starts.items():
    for k in range(i.size):
        owner[s + k] = s

# Patch writes: mov byte [edx + K], r8 after mov edx, imm (template) with ecx = table.
sites = collections.defaultdict(set)  # patched address -> tables
edx = ecx = None
for a in sorted(starts):
    i = starts[a]
    m, ops = i.mnemonic, i.operands
    if m == 'mov' and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_IMM:
        r, v = i.reg_name(ops[0].reg), ops[1].imm & 0xFFFFFFFF
        if r == 'edx':
            edx = v if lo <= v < hi else None
            continue
        if r == 'ecx':
            ecx = v
            continue
    if edx is not None and ops and ops[0].type == X.X86_OP_REG and i.reg_name(ops[0].reg) == 'edx':
        # The template pointer advances within a routine: add/sub edx, imm or lea edx, [edx + imm].
        if m in ('add', 'sub') and ops[1].type == X.X86_OP_IMM:
            edx = (edx + (ops[1].imm if m == 'add' else -ops[1].imm)) & 0xFFFFFFFF
            continue
        if m == 'lea' and ops[1].mem.base and i.reg_name(ops[1].mem.base) == 'edx' and not ops[1].mem.index:
            edx = (edx + ops[1].mem.disp) & 0xFFFFFFFF
            continue
    if ops and ops[0].type == X.X86_OP_REG and i.reg_name(ops[0].reg) in ('edx', 'dx', 'dl', 'dh') and m not in ('cmp', 'test', 'push'):
        edx = None
    if m == 'mov' and ops and ops[0].type == X.X86_OP_MEM and edx is not None:
        mm = ops[0].mem
        if mm.base and i.reg_name(mm.base) == 'edx' and not mm.index:
            sites[edx + mm.disp].add(ecx)
    if m in ('ret', 'jmp'):
        edx = None

# Values: every byte of the referenced table (256 entries x 4 bytes).
table_bytes = {}
for t in {t for ts in sites.values() for t in ts if t}:
    table_bytes[t] = set(im.read(t, 1024))
per_insn = collections.defaultdict(lambda: collections.defaultdict(set))
for p, ts in sites.items():
    s = owner.get(p)
    if s is None:
        print('# patched byte %08X is not inside an instruction' % p, file=sys.stderr)
        continue
    vals = set()
    for t in ts:
        vals |= table_bytes.get(t, set(range(256)))
    vals.add(im.read(p, 1)[0])
    per_insn[s][p - s] |= vals
for s in sorted(per_insn):
    for off, vals in sorted(per_insn[s].items()):
        i = starts[s]
        print('%08X %d %s  # %s %s' % (s, off, ','.join('%02X' % v for v in sorted(vals)), i.mnemonic, i.op_str))

#!/usr/bin/env python3
"""Static recompiler: 32-bit x86 PE images -> C++ (one host function per guest function).

usage: x86recomp.py <out-dir> <image>... [--seeds file]

Model (runtime/x86/cpu.h):
  * Guest memory is a flat 4 GB host mapping (g_mem); guest pointers are offsets into it.
  * Each guest function becomes `void x86_XXXXXXXX(Cpu& c)`. A guest `call` pushes the return
    address and calls the host function; `ret` pops it and returns. Indirect calls/jumps go
    through call_ind(), which maps a guest address to a host function or an HLE import thunk.
  * EFLAGS live in function-local variables and are only computed when a later instruction
    may read them (backward liveness over the function's control flow graph). Flags are not
    carried across calls, returns or indirect jumps.
  * The x87 stack lives in Cpu (st[8], top, sw, cw); register forms are decoded from the raw
    opcode bytes.
"""
import collections
import types
import os
import struct
import sys

import capstone
import pefile
from capstone import x86 as X

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True

REG32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
REG16 = ['ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di']
REG8L = ['al', 'cl', 'dl', 'bl']
REG8H = ['ah', 'ch', 'dh', 'bh']
UT = {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t', 8: 'uint64_t'}
ST = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t', 8: 'int64_t'}
MASK = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}
ALL = frozenset('CZSOP')
COND = {
    'o': ('of', 'O'), 'no': ('!of', 'O'), 'b': ('cf', 'C'), 'ae': ('!cf', 'C'), 'e': ('zf', 'Z'),
    'ne': ('!zf', 'Z'), 'be': ('(cf | zf)', 'CZ'), 'a': ('!(cf | zf)', 'CZ'), 's': ('sf', 'S'),
    'ns': ('!sf', 'S'), 'p': ('pf', 'P'), 'np': ('!pf', 'P'), 'l': ('(sf != of)', 'SO'),
    'ge': ('(sf == of)', 'SO'), 'le': ('(zf | (sf != of))', 'ZSO'), 'g': ('(!zf & (sf == of))', 'ZSO'),
}
PREFIX_BYTES = {0x66, 0x67, 0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65, 0xF0}


class Image:
    def __init__(self, path):
        self.path = path
        pe = pefile.PE(path)
        self.pe = pe
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.size = pe.OPTIONAL_HEADER.SizeOfImage
        self.mem = bytearray(self.size)
        self.mem[:len(pe.header)] = pe.header
        self.text = []
        for s in pe.sections:
            data = s.get_data()
            n = min(len(data), s.Misc_VirtualSize or len(data))
            self.mem[s.VirtualAddress:s.VirtualAddress + n] = data[:n]
            if s.Characteristics & 0x20000000:  # executable
                self.text.append((self.base + s.VirtualAddress, self.base + s.VirtualAddress + s.Misc_VirtualSize))
        self.entry = self.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.exports = {}
        if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
            for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
                self.exports[(e.name or b'').decode()] = self.base + e.address
        self.imports = {}  # IAT address -> (dll, name)
        for e in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
            for i in e.imports:
                self.imports[i.address] = (e.dll.decode().lower(), (i.name or b'#%d' % i.ordinal).decode())
        self.relocs = set()
        for r in getattr(pe, 'DIRECTORY_ENTRY_BASERELOC', []):
            for e in r.entries:
                if e.type == 3:
                    self.relocs.add(e.rva + self.base)

    def contains(self, a):
        return self.base <= a < self.base + self.size

    def in_text(self, a):
        return any(lo <= a < hi for lo, hi in self.text)

    def read(self, a, n):
        o = a - self.base
        return bytes(self.mem[o:o + n])

    def u32(self, a):
        return struct.unpack_from('<I', self.mem, a - self.base)[0]


class Program:
    def __init__(self, images):
        self.images = images
        self.cache = {}

    def image(self, a):
        for im in self.images:
            if im.contains(a):
                return im
        return None

    def in_text(self, a):
        im = self.image(a)
        return im is not None and im.in_text(a)

    def decode(self, a):
        if a in self.cache:
            return self.cache[a]
        im = self.image(a)
        ins = None
        if im and im.in_text(a):
            code = im.read(a, 16)
            ins = next(md.disasm(code, a, 1), None)
        self.cache[a] = ins
        return ins

    def u32(self, a):
        return self.image(a).u32(a)

    def iat(self, a):
        im = self.image(a)
        return im.imports.get(a) if im else None


def mem_op(i):
    for o in i.operands:
        if o.type == X.X86_OP_MEM:
            return o
    return None


def is_jcc(m):
    return m.startswith('j') and m not in ('jmp', 'jecxz', 'jcxz') and m[1:] in COND


def branch_target(i):
    if i.operands and i.operands[0].type == X.X86_OP_IMM:
        return i.operands[0].imm & 0xFFFFFFFF
    return None


def jump_table(prog, func_insns, i):
    """Targets of `jmp [reg*4 + table]`, bounded by a preceding `cmp reg, n` / `ja` when found."""
    m = mem_op(i)
    if not m or m.mem.base != 0 or m.mem.scale != 4 or m.mem.index == 0:
        return None
    table = m.mem.disp & 0xFFFFFFFF
    im = prog.image(table)
    if not im:
        return None
    idx = i.reg_name(m.mem.index)
    bound = None
    a = i.address
    # Walk back a few instructions looking for `cmp idx, imm`.
    prevs = []
    cur = a
    for _ in range(8):
        p = next((cur - k for k in range(1, 16) if (cur - k) in func_insns and prog.decode(cur - k) and cur - k + prog.decode(cur - k).size == cur), None)
        if p is None:
            break
        prevs.append(p)
        cur = p
    for p in prevs:
        pi = prog.decode(p)
        if pi and pi.mnemonic in ('cmp', 'and') and len(pi.operands) == 2 and pi.operands[0].type == X.X86_OP_REG \
                and pi.operands[1].type == X.X86_OP_IMM and pi.reg_name(pi.operands[0].reg) == idx:
            bound = (pi.operands[1].imm & 0xFFFFFFFF) + 1
            break
    def entry(e):
        if not im.contains(e + 3):
            return None
        if im.relocs and e not in im.relocs:
            return None
        t = im.u32(e)
        return t if prog.in_text(t) else None

    targets = {}  # index -> target (indices may be negative: table addressed from its end)
    for k in range(bound if bound is not None and bound <= 4096 else 512):
        t = entry(table + 4 * k)
        if t is None:
            if bound is not None:
                continue
            break
        targets[k] = t
    if not targets and bound is None:
        for k in range(-1, -64, -1):
            t = entry(table + 4 * k)
            if t is None:
                break
            targets[k] = t
    return targets or None


class Discovery:
    def __init__(self, prog, extra_seeds):
        self.prog = prog
        self.strong = set()  # entry points, exports, call targets
        self.weak = set()    # code addresses referenced from data / immediates
        for im in prog.images:
            self.strong.add(im.entry)
            self.strong.update(im.exports.values())
            for r in im.relocs:
                t = im.u32(r)
                if im.in_text(t):
                    self.weak.add(t)
            if not im.relocs:
                # No relocations: scan data for code pointers.
                for s in im.pe.sections:
                    if s.Characteristics & 0x20000000:
                        continue
                    lo = im.base + s.VirtualAddress
                    data = im.mem[s.VirtualAddress:s.VirtualAddress + s.Misc_VirtualSize]
                    for o in range(0, len(data) - 3, 4):
                        t = struct.unpack_from('<I', data, o)[0]
                        if im.in_text(t):
                            self.weak.add(t)
        self.strong |= set(extra_seeds)
        self.funcs = {}

    def descend(self, start):
        """Instructions reachable from start without following calls. Returns (addrs, tables)."""
        prog = self.prog
        seen, tables, work = set(), {}, [start]
        while work:
            a = work.pop()
            while a not in seen:
                i = prog.decode(a)
                if i is None:
                    seen.add(a)  # becomes a trap
                    break
                seen.add(a)
                m = i.mnemonic
                nxt = a + i.size
                if m == 'call':
                    t = branch_target(i)
                    if t is not None and prog.in_text(t):
                        self.new_strong.add(t)
                for o in i.operands:
                    if o.type == X.X86_OP_IMM and o.size == 4 and prog.in_text(o.imm & 0xFFFFFFFF) and m not in ('call', 'jmp') and not is_jcc(m):
                        self.weak.add(o.imm & 0xFFFFFFFF)
                if m in ('ret', 'retf', 'iretd', 'hlt', 'int3', 'ud2'):
                    break
                if m == 'jmp':
                    t = branch_target(i)
                    if t is not None:
                        if t != start and t in self.strong:
                            break  # tail call
                        a = t
                        continue
                    tt = jump_table(prog, seen, i)
                    if tt:
                        tables[a] = tt
                        work.extend(tt.values())
                    break
                if is_jcc(m) or m in ('jecxz', 'loop', 'loope', 'loopne'):
                    t = branch_target(i)
                    if t is not None and not (t != start and t in self.strong):
                        work.append(t)
                a = nxt
        return seen, tables

    def run(self):
        """Strong starts first (to a fixed point); then weak candidates that are not inside
        already discovered code, or that look like function starts (follow padding/ret)."""
        tried = set()
        self.new_strong = set()

        def close(todo):
            while todo:
                self.new_strong = set()
                for f in sorted(todo):
                    tried.add(f)
                    if self.prog.decode(f) is not None:
                        self.funcs[f] = self.descend(f)
                self.strong |= self.new_strong
                todo = {f for f in self.new_strong if f not in tried}

        close(set(self.strong))
        while True:
            covered = set()
            for addrs, _ in self.funcs.values():
                covered |= addrs
            todo = set()
            for f in self.weak:
                if f in tried:
                    continue
                if f in covered:
                    im = self.prog.image(f)
                    prev = im.read(f - 1, 1)[0] if im.contains(f - 1) else 0
                    if not (f % 16 == 0 and prev in (0xCC, 0x90, 0xC3)):
                        tried.add(f)
                        continue
                todo.add(f)
            if not todo:
                break
            close(todo)
        # A strong start discovered late may sit inside an earlier function's body; jumps to it
        # are then local jumps, which is still correct.


# ---------------------------------------------------------------- code generation

class Gen:
    def __init__(self, prog, disc):
        self.prog = prog
        self.disc = disc
        self.unimpl = collections.Counter()
        self.entry_needs, self.returns_flags = {}, set()
        self.warn = collections.Counter()

    # --- operands
    def reg(self, name):
        if name in REG32:
            return ('r32', REG32.index(name))
        if name in REG16:
            return ('r16', REG16.index(name))
        if name in REG8L:
            return ('r8l', REG8L.index(name))
        if name in REG8H:
            return ('r8h', REG8H.index(name))
        raise KeyError(name)

    def addr(self, i, o):
        m = o.mem
        parts = []
        if m.base:
            parts.append('c.r[%d]' % REG32.index(i.reg_name(m.base)))
        if m.index:
            r = 'c.r[%d]' % REG32.index(i.reg_name(m.index))
            parts.append(r if m.scale == 1 else '%s * %d' % (r, m.scale))
        if m.disp or not parts:
            parts.append('0x%Xu' % (m.disp & 0xFFFFFFFF))
        if m.segment and i.reg_name(m.segment) == 'fs':
            parts.append('c.fsbase')
        return '(uint32_t)(%s)' % ' + '.join(parts)

    def rd(self, i, o, size=None):
        size = size or o.size
        if o.type == X.X86_OP_REG:
            k, n = self.reg(i.reg_name(o.reg))
            return {'r32': 'c.r[%d]' % n, 'r16': '(uint16_t)c.r[%d]' % n,
                    'r8l': '(uint8_t)c.r[%d]' % n, 'r8h': '(uint8_t)(c.r[%d] >> 8)' % n}[k]
        if o.type == X.X86_OP_IMM:
            return '(%s)0x%Xu' % (UT[size], o.imm & MASK[size])
        return 'rd%d(%s)' % (size * 8, self.addr(i, o))

    def wr(self, i, o, v, a_var=None):
        if o.type == X.X86_OP_REG:
            k, n = self.reg(i.reg_name(o.reg))
            if k == 'r32':
                return 'c.r[%d] = (uint32_t)(%s);' % (n, v)
            if k == 'r16':
                return 'c.r[%d] = (c.r[%d] & 0xFFFF0000u) | (uint16_t)(%s);' % (n, n, v)
            if k == 'r8l':
                return 'c.r[%d] = (c.r[%d] & 0xFFFFFF00u) | (uint8_t)(%s);' % (n, n, v)
            return 'c.r[%d] = (c.r[%d] & 0xFFFF00FFu) | ((uint32_t)(uint8_t)(%s) << 8);' % (n, n, v)
        return 'wr%d(%s, (%s)(%s));' % (o.size * 8, a_var or self.addr(i, o), UT[o.size], v)

    def rmw(self, i, o):
        """(prologue, read expr, write fn) for a read-modify-write destination."""
        if o.type == X.X86_OP_MEM:
            pro = 'const uint32_t a_ = %s; ' % self.addr(i, o)
            return pro, 'rd%d(a_)' % (o.size * 8), lambda v: self.wr(i, o, v, 'a_')
        return '', self.rd(i, o), lambda v: self.wr(i, o, v)

    # --- flags
    def flags_szp(self, live, size, r='r'):
        out = []
        bits = size * 8
        if 'Z' in live:
            out.append('zf = %s == 0;' % r)
        if 'S' in live:
            out.append('sf = (uint32_t)%s >> %d;' % (r, bits - 1))
        if 'P' in live:
            out.append('pf = parity8(%s);' % r)
        return ' '.join(out)

    def flags_arith(self, kind, live, size):
        bits = size * 8
        out = []
        if kind in ('add', 'adc'):
            if 'C' in live:
                out.append('cf = %s;' % ('r < d' if kind == 'add' else '(r < d) | ((r == d) & cin)'))
            if 'O' in live:
                out.append('of = (uint32_t)((d ^ r) & (s ^ r)) >> %d & 1;' % (bits - 1))
        elif kind in ('sub', 'sbb'):
            if 'C' in live:
                out.append('cf = %s;' % ('d < s' if kind == 'sub' else '(uint64_t)d < (uint64_t)s + cin'))
            if 'O' in live:
                out.append('of = (uint32_t)((d ^ s) & (d ^ r)) >> %d & 1;' % (bits - 1))
        elif kind == 'logic':
            if 'C' in live:
                out.append('cf = 0;')
            if 'O' in live:
                out.append('of = 0;')
        out.append(self.flags_szp(live, size))
        return ' '.join(x for x in out if x)

    # --- per-instruction use/def of flags
    def flag_effects(self, i):
        m = i.mnemonic.replace('lock ', '')
        if m.startswith('rep'):
            m = m.split()[-1]
        if m in ('add', 'sub', 'cmp', 'and', 'or', 'xor', 'test', 'neg', 'mul', 'imul', 'div', 'idiv',
                 'cmpsb', 'cmpsw', 'cmpsd', 'scasb', 'scasw', 'scasd', 'bsf', 'bsr', 'aam', 'aad', 'xadd', 'cmpxchg'):
            return frozenset(), ALL
        if m in ('adc', 'sbb'):
            return frozenset('C'), ALL
        if m in ('inc', 'dec'):
            return frozenset(), frozenset('ZSOP')
        if m in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'shld', 'shrd'):
            cnt = i.operands[-1]
            if cnt.type == X.X86_OP_IMM:
                n = cnt.imm & 31
                if n == 0:
                    return frozenset(), frozenset()
                if m in ('rol', 'ror'):
                    return frozenset(), frozenset('CO')
                return frozenset(), ALL
            # Count in CL: flags are unchanged when it is zero.
            d = frozenset('CO') if m in ('rol', 'ror') else ALL
            return d, d
        if m in ('rcl', 'rcr'):
            return frozenset('C'), frozenset('CO')
        if m in ('bt', 'bts', 'btr', 'btc', 'stc', 'clc'):
            return frozenset(), frozenset('C')
        if m == 'cmc':
            return frozenset('C'), frozenset('C')
        if m == 'sahf':
            return frozenset(), frozenset('CZSP')
        if m == 'lahf':
            return frozenset('CZSP'), frozenset()
        if m in ('pushfd', 'pushf'):
            return ALL, frozenset()
        if m in ('popfd', 'popf'):
            return frozenset(), ALL
        if m.startswith('set') and m[3:] in COND:
            return frozenset(COND[m[3:]][1]), frozenset()
        if m.startswith('cmov') and m[4:] in COND:
            return frozenset(COND[m[4:]][1]), frozenset()
        if is_jcc(m):
            return frozenset(COND[m[1:]][1]), frozenset()
        if m in ('loope', 'loopne'):
            return frozenset('Z'), frozenset()
        if m in ('daa', 'das', 'aaa', 'aas'):
            return ALL, ALL
        if m == 'call':
            return frozenset(), ALL
        return frozenset(), frozenset()

    # --- function
    def analyze(self, start):
        prog = self.prog
        addrs, tables = self.disc.funcs[start]
        order = sorted(addrs)
        insns = {a: prog.decode(a) for a in order}
        strong = self.disc.strong

        def local_target(t):
            return t in addrs and (t == start or t not in strong)

        # Successors for liveness.
        succ = {}
        for a in order:
            i = insns[a]
            if i is None:
                succ[a] = []
                continue
            m = i.mnemonic
            nxt = a + i.size
            s = []
            if m in ('ret', 'retf', 'iretd', 'hlt', 'int3', 'ud2'):
                pass
            elif m == 'jmp':
                t = branch_target(i)
                if t is not None:
                    if local_target(t):
                        s.append(t)
                elif a in tables:
                    s.extend(t for t in tables[a].values() if t in addrs)
            elif is_jcc(m) or m in ('jecxz', 'loop', 'loope', 'loopne'):
                t = branch_target(i)
                if t is not None and local_target(t):
                    s.append(t)
                s.append(nxt)
            else:
                s.append(nxt)
            succ[a] = [x for x in s if x in addrs]
        bit = {'C': 1, 'Z': 2, 'S': 4, 'O': 8, 'P': 16}
        tomask = lambda fs: sum(bit[f] for f in fs)
        eff = {}
        for a in order:
            u, d = self.flag_effects(insns[a]) if insns[a] else (frozenset(), frozenset())
            u = tomask(u)
            i = insns[a]
            if i is not None:
                # Flags passed across call boundaries (MSVC CRT math helpers do this).
                t = branch_target(i) if (i.mnemonic in ('call', 'jmp') or is_jcc(i.mnemonic)) else None
                if t is not None and not (i.mnemonic != 'call' and local_target(t)):
                    u |= self.entry_needs.get(t, 0)
                if i.mnemonic in ('ret', 'retf') and start in self.returns_flags:
                    u |= 31
            eff[a] = (u, tomask(d))
        preds = collections.defaultdict(list)
        for a in order:
            for t in succ[a]:
                preds[t].append(a)
        live_in = dict.fromkeys(order, 0)
        work = list(order)
        inwork = set(order)
        while work:
            a = work.pop()
            inwork.discard(a)
            out = 0
            for t in succ[a]:
                out |= live_in[t]
            use, d = eff[a]
            li = use | (out & ~d)
            if li != live_in[a]:
                live_in[a] = li
                for p in preds[a]:
                    if p not in inwork:
                        inwork.add(p)
                        work.append(p)
        frommask = lambda m: frozenset(f for f, b in bit.items() if m & b)
        live_out = {}
        live_out_mask = {}
        for a in order:
            out = 0
            for t in succ[a]:
                out |= live_in[t]
            live_out[a] = frommask(out)
            live_out_mask[a] = out
        return types.SimpleNamespace(addrs=addrs, tables=tables, order=order, insns=insns, succ=succ,
                                     live_in=live_in, live_out=live_out, live_out_mask=live_out_mask,
                                     local_target=local_target)

    def analyze_all(self, funcs):
        """Fixed point of which functions need flags at entry and which must return flags."""
        self.entry_needs, self.returns_flags = {}, set()
        for _ in range(6):
            changed = False
            for f in funcs:
                r = self.analyze(f)
                if r.live_in[f] != self.entry_needs.get(f, 0):
                    self.entry_needs[f] = r.live_in[f]
                    changed = True
                for a in r.order:
                    i = r.insns[a]
                    if i is not None and i.mnemonic == 'call' and r.live_out_mask[a]:
                        t = branch_target(i)
                        if t is not None and t not in self.returns_flags:
                            self.returns_flags.add(t)
                            changed = True
            if not changed:
                break
        self.entry_needs = {f: m for f, m in self.entry_needs.items() if m}
        print('flags across calls: %d functions take flags, %d return flags' % (len(self.entry_needs), len(self.returns_flags)))

    def gen_function(self, start):
        r = self.analyze(start)
        addrs, tables, order, insns, succ = r.addrs, r.tables, r.order, r.insns, r.succ
        live_in, live_out, local_target = r.live_in, r.live_out, r.local_target
        FL = [('cf', 1), ('zf', 2), ('sf', 4), ('of', 8), ('pf', 16)]
        store = lambda m: ' '.join('c.fl[%d] = %s;' % (k, n) for k, (n, b) in enumerate(FL) if m & b)
        load = lambda m: ' '.join('%s = c.fl[%d];' % (n, k) for k, (n, b) in enumerate(FL) if m & b)

        targets = set()
        for a in order:
            for s in succ[a]:
                m_ = insns[a].mnemonic if insns[a] else ''
                if s != a + (insns[a].size if insns[a] else 0) or m_ == 'jmp' or is_jcc(m_) or m_ in ('jecxz', 'loop', 'loope', 'loopne'):
                    targets.add(s)
        # Threaded code: code addresses pushed (directly or from jump tables) and reached by `ret`.
        ret_targets = set()
        for a in order:
            i = insns[a]
            if i is not None and i.mnemonic == 'push' and i.operands and i.operands[0].type == X.X86_OP_IMM:
                v = i.operands[0].imm & 0xFFFFFFFF
                if v in addrs:
                    ret_targets.add(v)
        if ret_targets:
            for tt in tables.values():
                ret_targets |= {t for t in tt.values() if t in addrs}
        self.ret_targets = ret_targets
        # Fallthrough into an address that is not the next decoded instruction also needs a label.
        lines = ['void x86_%08X(Cpu& c)' % start, '{',
                 '    uint32_t cf = 0, zf = 0, sf = 0, of = 0, pf = 0; (void)cf; (void)zf; (void)sf; (void)of; (void)pf;']
        if live_in[start]:
            lines.append('    %s' % load(live_in[start]))
        if ret_targets:
            lines.append('    const uint32_t entry_ret_ = rd32(c.r[4]);')
        if start != order[0]:
            lines.append('    goto L_%08X;' % start)
        need_label = set(targets) | {start} | ret_targets
        for k, a in enumerate(order):
            i = insns[a]
            if i is not None and i.mnemonic not in ('ret', 'retf', 'jmp', 'int3', 'hlt', 'iretd', 'ud2'):
                nxt = a + i.size
                if k + 1 >= len(order) or order[k + 1] != nxt:
                    need_label.add(nxt)
        for k, a in enumerate(order):
            i = insns[a]
            if a in need_label:
                lines.append('L_%08X:' % a)
            if i is None:
                lines.append('    trap(c, 0x%08Xu, "undecodable");' % a)
                continue
            if a in SMC:
                body = self.gen_smc(i, live_out[a], addrs, local_target, tables)
            else:
                body = self.gen_insn(i, live_out[a], addrs, local_target, tables)
            pre = post = ''
            t = branch_target(i) if (i.mnemonic in ('call', 'jmp') or is_jcc(i.mnemonic)) else None
            if t is not None and not (i.mnemonic != 'call' and local_target(t)) and self.entry_needs.get(t):
                pre = store(self.entry_needs[t]) + ' '
            if i.mnemonic == 'call' and t in self.returns_flags and r.live_out_mask[a]:
                post = ' ' + load(r.live_out_mask[a])
            if i.mnemonic in ('ret', 'retf') and start in self.returns_flags:
                pre = store(31) + ' '
            lines.append('    %s%s%s // %s %s' % (pre, body, post, i.mnemonic, i.op_str))
            nxt = a + i.size
            if i.mnemonic not in ('ret', 'retf', 'jmp', 'int3', 'hlt', 'iretd', 'ud2') and (k + 1 >= len(order) or order[k + 1] != nxt):
                lines.append('    goto L_%08X;' % nxt if nxt in addrs else '    { x86_fallthrough(c, 0x%08Xu); return; }' % nxt)
        lines.append('}')
        return '\n'.join(lines)

    def gen_smc(self, i, live, addrs, local_target, tables):
        """Self-modified instruction: switch over the values its patched byte can take."""
        sites = SMC[i.address]
        if len(sites) != 1:
            return 'trap(c, 0x%08Xu, "self-modified instruction with several patched bytes");' % i.address
        off, values = sites[0]
        orig = bytearray(self.prog.image(i.address).read(i.address, i.size))
        cases = []
        for v in values:
            b = bytearray(orig)
            b[off] = v
            vi = next(md.disasm(bytes(b), i.address, 1), None)
            if vi is None or vi.size != i.size:
                continue
            cases.append('case 0x%02X: %s break; /* %s %s */' % (v, self.gen_insn(vi, live, addrs, local_target, tables), vi.mnemonic, vi.op_str))
        return 'switch (rd8(0x%08Xu)) { %s default: trap(c, 0x%08Xu, "unexpected self-modified byte"); }' % (
            i.address + off, ' '.join(cases), i.address)

    # --- instructions
    def gen_insn(self, i, live, addrs, local_target, tables):
        m = i.mnemonic
        a = i.address
        nxt = (a + i.size) & 0xFFFFFFFF
        ops = i.operands
        prog = self.prog
        b0 = prog.image(a).read(a, 16)
        k = 0
        while b0[k] in PREFIX_BYTES:
            k += 1
        if 0xD8 <= b0[k] <= 0xDF:
            return self.gen_x87(i, b0[k], b0[k + 1])
        lock = m.startswith('lock ')
        if lock:
            m = m[5:]
        rep = None
        if m.startswith('rep'):
            rep, m = m.split()

        def jump(t):
            if t is None:
                return None
            if local_target(t):
                return 'goto L_%08X;' % t
            if prog.in_text(t):
                if t in self.disc.funcs:
                    return '{ x86_%08X(c); return; }' % t
                return '{ x86_fallthrough(c, 0x%08Xu); return; }' % t
            return '{ call_ind(c, 0x%08Xu); return; }' % t

        if m == 'nop' or m in ('wait', 'fwait', 'cld') and False:
            return ';'
        if m in ('nop', 'wait', 'fwait', 'lfence', 'sfence', 'mfence', 'prefetchnta', 'prefetcht0'):
            return ';'
        if m == 'cld':
            return 'c.df = 0;'
        if m == 'std':
            return 'c.df = 1;'
        if m == 'mov':
            d, s = ops
            if s.type == X.X86_OP_REG and i.reg_name(s.reg) in ('cs', 'ds', 'es', 'ss', 'fs', 'gs'):
                return self.wr(i, d, '0')
            if d.type == X.X86_OP_REG and i.reg_name(d.reg) in ('cs', 'ds', 'es', 'ss', 'fs', 'gs'):
                return ';'
            return self.wr(i, d, self.rd(i, s, d.size))
        if m in ('movzx', 'movsx'):
            d, s = ops
            v = self.rd(i, s)
            if m == 'movsx':
                v = '(%s)(%s)(%s)' % (UT[d.size], ST[s.size], v)
            return self.wr(i, d, v)
        if m == 'lea':
            return self.wr(i, ops[0], self.addr(i, ops[1]).replace(' + c.fsbase', ''))
        if m == 'push':
            o = ops[0]
            if o.type == X.X86_OP_REG and i.reg_name(o.reg) in ('cs', 'ds', 'es', 'ss', 'fs', 'gs'):
                return 'push32(c, 0);'
            if o.size == 2:
                return '{ const uint16_t v_ = %s; c.r[4] -= 2; wr16(c.r[4], v_); }' % self.rd(i, o)
            return '{ const uint32_t v_ = %s; push32(c, v_); }' % self.rd(i, o, 4)
        if m == 'pop':
            o = ops[0]
            if o.type == X.X86_OP_REG and i.reg_name(o.reg) in ('cs', 'ds', 'es', 'ss', 'fs', 'gs'):
                return 'c.r[4] += 4;'
            if o.type == X.X86_OP_REG and i.reg_name(o.reg) == 'esp':
                return 'c.r[4] = rd32(c.r[4]);'
            if o.size == 2:
                return '{ const uint16_t v_ = rd16(c.r[4]); c.r[4] += 2; %s }' % self.wr(i, o, 'v_')
            return '{ const uint32_t v_ = rd32(c.r[4]); c.r[4] += 4; %s }' % self.wr(i, o, 'v_')
        if m == 'pushal':
            return '{ const uint32_t s_ = c.r[4]; for (int k = 0; k < 8; k++) push32(c, k == 4 ? s_ : c.r[k]); }'
        if m == 'popal':
            return '{ for (int k = 7; k >= 0; k--) { const uint32_t v_ = rd32(c.r[4]); c.r[4] += 4; if (k != 4) c.r[k] = v_; } }'
        if m in ('pushfd', 'pushf'):
            return 'push32(c, 0x202u | cf | pf << 2 | zf << 6 | sf << 7 | c.df << 10 | of << 11);'
        if m in ('popfd', 'popf'):
            return '{ const uint32_t v_ = rd32(c.r[4]); c.r[4] += 4; cf = v_ & 1; pf = v_ >> 2 & 1; zf = v_ >> 6 & 1; sf = v_ >> 7 & 1; c.df = v_ >> 10 & 1; of = v_ >> 11 & 1; }'
        if m == 'xchg':
            d, s = ops
            if d.type == X.X86_OP_REG and s.type == X.X86_OP_REG and d.reg == s.reg:
                return ';'
            if d.type == X.X86_OP_MEM:
                d, s = s, d
            if s.type == X.X86_OP_MEM:
                return '{ const uint32_t a_ = %s; const %s t_ = rd%d(a_); wr%d(a_, %s); %s }' % (
                    self.addr(i, s), UT[s.size], s.size * 8, s.size * 8, self.rd(i, d), self.wr(i, d, 't_'))
            return '{ const %s t_ = %s; %s %s }' % (UT[d.size], self.rd(i, d), self.wr(i, d, self.rd(i, s)), self.wr(i, s, 't_'))
        if m in ('add', 'sub', 'adc', 'sbb', 'cmp', 'and', 'or', 'xor', 'test'):
            d, s = ops
            size = d.size
            t = UT[size]
            if m in ('xor', 'sub') and d.type == X.X86_OP_REG and s.type == X.X86_OP_REG and d.reg == s.reg:
                fl = []
                if 'Z' in live: fl.append('zf = 1;')
                if 'S' in live: fl.append('sf = 0;')
                if 'P' in live: fl.append('pf = 1;')
                if 'C' in live: fl.append('cf = 0;')
                if 'O' in live: fl.append('of = 0;')
                return '%s %s' % (self.wr(i, d, '0'), ' '.join(fl))
            pro, rdx, wrf = self.rmw(i, d)
            op = {'add': '+', 'adc': '+', 'sub': '-', 'sbb': '-', 'cmp': '-', 'and': '&', 'or': '|', 'xor': '^', 'test': '&'}[m]
            kind = {'add': 'add', 'adc': 'adc', 'sub': 'sub', 'sbb': 'sub', 'cmp': 'sub'}.get(m, 'logic')
            if m == 'sbb':
                kind = 'sbb'
            expr = 'd %s s' % op
            cin = ''
            if m in ('adc', 'sbb'):
                cin = 'const %s cin = (%s)cf; ' % (t, t)
                expr = 'd %s s %s cin' % (op, op)
            fl = self.flags_arith(kind, live, size)
            write = '' if m in ('cmp', 'test') else wrf('r')
            if m in ('cmp', 'test') and not live:
                return ';'
            return '{ %s%sconst %s d = %s; const %s s = %s; const %s r = (%s)(%s); %s %s }' % (
                pro, cin, t, rdx, t, self.rd(i, s, size), t, t, expr, fl, write)
        if m in ('inc', 'dec', 'neg', 'not'):
            d = ops[0]
            size = d.size
            t = UT[size]
            pro, rdx, wrf = self.rmw(i, d)
            sign = '0x%Xu' % (1 << (size * 8 - 1))
            if m == 'not':
                return '{ %sconst %s d = %s; %s }' % (pro, t, rdx, wrf('~d'))
            if m == 'neg':
                fl = []
                if 'C' in live: fl.append('cf = d != 0;')
                if 'O' in live: fl.append('of = d == %s;' % sign)
                fl.append(self.flags_szp(live, size))
                return '{ %sconst %s d = %s; const %s r = (%s)(0 - d); %s %s }' % (pro, t, rdx, t, t, ' '.join(fl), wrf('r'))
            fl = []
            if 'O' in live:
                fl.append('of = r == %s;' % sign if m == 'inc' else 'of = d == %s;' % sign)
            fl.append(self.flags_szp(live, size))
            return '{ %sconst %s d = %s; const %s r = (%s)(d %s 1); %s %s }' % (
                pro, t, rdx, t, t, '+' if m == 'inc' else '-', ' '.join(fl), wrf('r'))
        if m in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr'):
            d = ops[0]
            size = d.size
            bits = size * 8
            t = UT[size]
            cnt = '(%s & 31)' % (self.rd(i, ops[1], 1) if len(ops) > 1 else '1')
            if len(ops) > 1 and ops[1].type == X.X86_OP_IMM:
                n = ops[1].imm & 31
                if n == 0:
                    return ';'
                cnt = str(n)
            pro, rdx, wrf = self.rmw(i, d)
            fl = []
            if m in ('shl', 'sal'):
                core = 'const %s r = (%s)((uint32_t)d << n);' % (t, t)
                if 'C' in live: fl.append('cf = n <= %d ? ((uint32_t)d >> (%d - n)) & 1 : 0;' % (bits, bits))
                if 'O' in live: fl.append('of = ((uint32_t)r >> %d & 1) ^ (n <= %d ? ((uint32_t)d >> (%d - n)) & 1 : 0);' % (bits - 1, bits, bits))
                fl.append(self.flags_szp(live, size))
            elif m == 'shr':
                core = 'const %s r = (%s)(n < 32 ? (uint32_t)d >> n : 0);' % (t, t)
                if 'C' in live: fl.append('cf = ((uint64_t)d >> (n - 1)) & 1;')
                if 'O' in live: fl.append('of = (uint32_t)d >> %d & 1;' % (bits - 1))
                fl.append(self.flags_szp(live, size))
            elif m == 'sar':
                core = 'const %s r = (%s)((int32_t)(%s)d >> (n > 31 ? 31 : n));' % (t, t, ST[size])
                if 'C' in live: fl.append('cf = ((int64_t)(%s)d >> (n - 1)) & 1;' % ST[size])
                if 'O' in live: fl.append('of = 0;')
                fl.append(self.flags_szp(live, size))
            elif m in ('rol', 'ror'):
                rot = 'n %% %d' % bits
                if m == 'rol':
                    core = 'const uint32_t k_ = %s; const %s r = (%s)(k_ ? (%s)(d << k_) | (%s)(d >> (%d - k_)) : d);' % (rot, t, t, t, t, bits)
                    if 'C' in live: fl.append('cf = r & 1;')
                    if 'O' in live: fl.append('of = ((uint32_t)r >> %d & 1) ^ (r & 1);' % (bits - 1))
                else:
                    core = 'const uint32_t k_ = %s; const %s r = (%s)(k_ ? (%s)(d >> k_) | (%s)(d << (%d - k_)) : d);' % (rot, t, t, t, t, bits)
                    if 'C' in live: fl.append('cf = (uint32_t)r >> %d & 1;' % (bits - 1))
                    if 'O' in live: fl.append('of = ((uint32_t)r >> %d ^ (uint32_t)r >> %d) & 1;' % (bits - 1, bits - 2))
            else:  # rcl / rcr (count assumed small)
                if m == 'rcl':
                    core = ('%s r = d; uint32_t c_ = cf; for (uint32_t k_ = 0; k_ < n %% %d; k_++) { const uint32_t o_ = (uint32_t)r >> %d & 1; r = (%s)((r << 1) | c_); c_ = o_; } cf = c_;'
                            % (t, bits + 1, bits - 1, t))
                    if 'O' in live: fl.append('of = ((uint32_t)r >> %d & 1) ^ cf;' % (bits - 1))
                else:
                    core = ('%s r = d; uint32_t c_ = cf; for (uint32_t k_ = 0; k_ < n %% %d; k_++) { const uint32_t o_ = r & 1; r = (%s)((r >> 1) | ((uint32_t)c_ << %d)); c_ = o_; } cf = c_;'
                            % (t, bits + 1, t, bits - 1))
                    if 'O' in live: fl.append('of = ((uint32_t)r >> %d ^ (uint32_t)r >> %d) & 1;' % (bits - 1, bits - 2))
            body = '%sconst %s d = %s; %s %s %s' % (pro, t, rdx, core, ' '.join(fl), wrf('r'))
            if not cnt.isdigit():
                return '{ const uint32_t n = %s; if (n) { %s } }' % (cnt, body)
            return '{ const uint32_t n = %s; %s }' % (cnt, body)
        if m in ('shld', 'shrd'):
            d, s, cnto = ops
            assert d.size == 4
            pro, rdx, wrf = self.rmw(i, d)
            cnt = str(cnto.imm & 31) if cnto.type == X.X86_OP_IMM else '(%s & 31)' % self.rd(i, cnto, 1)
            if m == 'shld':
                core = 'const uint32_t r = n ? (d << n) | (s >> (32 - n)) : d; '
                cfx = 'cf = n ? (d >> (32 - n)) & 1 : cf;'
            else:
                core = 'const uint32_t r = n ? (d >> n) | (s << (32 - n)) : d; '
                cfx = 'cf = n ? (d >> (n - 1)) & 1 : cf;'
            fl = (cfx if 'C' in live else '') + ' ' + self.flags_szp(live, 4)
            return '{ %sconst uint32_t d = %s; const uint32_t s = %s; const uint32_t n = %s; %s%s %s }' % (pro, rdx, self.rd(i, s), cnt, core, fl, wrf('r'))
        if m in ('mul', 'imul', 'div', 'idiv'):
            if m == 'imul' and len(ops) >= 2:
                d = ops[0]
                src = ops[1] if len(ops) == 2 else ops[1]
                a_ = self.rd(i, d) if len(ops) == 2 else self.rd(i, ops[1])
                b_ = self.rd(i, ops[1]) if len(ops) == 2 else self.rd(i, ops[2], d.size)
                size = d.size
                fl = []
                if 'C' in live or 'O' in live:
                    fl.append('cf = of = p_ != (int64_t)(%s)p_;' % ST[size])
                fl.append(self.flags_szp(live, size, '(%s)p_' % UT[size]))
                return '{ const int64_t p_ = (int64_t)(%s)%s * (int64_t)(%s)%s; %s %s }' % (ST[size], a_, ST[size], b_, ' '.join(fl), self.wr(i, d, 'p_'))
            o = ops[0]
            size = o.size
            src = self.rd(i, o)
            if size == 4:
                if m == 'mul':
                    fl = 'cf = of = (p_ >> 32) != 0;' if ('C' in live or 'O' in live) else ''
                    return '{ const uint64_t p_ = (uint64_t)c.r[0] * %s; c.r[0] = (uint32_t)p_; c.r[2] = (uint32_t)(p_ >> 32); %s %s }' % (src, fl, self.flags_szp(live, 4, '(uint32_t)p_'))
                if m == 'imul':
                    fl = 'cf = of = (int64_t)p_ != (int64_t)(int32_t)p_;' if ('C' in live or 'O' in live) else ''
                    return '{ const int64_t p_ = (int64_t)(int32_t)c.r[0] * (int32_t)%s; c.r[0] = (uint32_t)p_; c.r[2] = (uint32_t)((uint64_t)p_ >> 32); %s %s }' % (src, fl, self.flags_szp(live, 4, '(uint32_t)p_'))
                if m == 'div':
                    return '{ const uint64_t n_ = (uint64_t)c.r[2] << 32 | c.r[0]; const uint32_t d_ = %s; if (!d_) x86_divide_error(c, 0x%08Xu); c.r[0] = (uint32_t)(n_ / d_); c.r[2] = (uint32_t)(n_ %% d_); }' % (src, i.address)
                return '{ const int64_t n_ = (int64_t)((uint64_t)c.r[2] << 32 | c.r[0]); const int32_t d_ = (int32_t)%s; if (!d_) x86_divide_error(c, 0x%08Xu); c.r[0] = (uint32_t)(int32_t)(n_ / d_); c.r[2] = (uint32_t)(int32_t)(n_ %% d_); }' % (src, i.address)
            if size == 2:
                if m == 'mul':
                    return '{ const uint32_t p_ = (uint32_t)(uint16_t)c.r[0] * %s; c.r[0] = (c.r[0] & 0xFFFF0000u) | (p_ & 0xFFFF); c.r[2] = (c.r[2] & 0xFFFF0000u) | (p_ >> 16); cf = of = (p_ >> 16) != 0; }' % src
                if m == 'imul':
                    return '{ const int32_t p_ = (int32_t)(int16_t)c.r[0] * (int16_t)%s; c.r[0] = (c.r[0] & 0xFFFF0000u) | ((uint32_t)p_ & 0xFFFF); c.r[2] = (c.r[2] & 0xFFFF0000u) | ((uint32_t)p_ >> 16); cf = of = p_ != (int16_t)p_; }' % src
                if m == 'div':
                    return '{ const uint32_t n_ = (c.r[2] & 0xFFFF) << 16 | (c.r[0] & 0xFFFF); const uint32_t d_ = %s; if (!d_) x86_divide_error(c, 0x%08Xu); c.r[0] = (c.r[0] & 0xFFFF0000u) | ((n_ / d_) & 0xFFFF); c.r[2] = (c.r[2] & 0xFFFF0000u) | (n_ %% d_); }' % (src, i.address)
                return '{ const int32_t n_ = (int32_t)((c.r[2] & 0xFFFF) << 16 | (c.r[0] & 0xFFFF)); const int32_t d_ = (int16_t)%s; if (!d_) x86_divide_error(c, 0x%08Xu); c.r[0] = (c.r[0] & 0xFFFF0000u) | ((uint32_t)(n_ / d_) & 0xFFFF); c.r[2] = (c.r[2] & 0xFFFF0000u) | ((uint32_t)(n_ %% d_) & 0xFFFF); }' % (src, i.address)
            if m == 'mul':
                return '{ const uint32_t p_ = (uint32_t)(uint8_t)c.r[0] * %s; c.r[0] = (c.r[0] & 0xFFFF0000u) | (p_ & 0xFFFF); cf = of = (p_ >> 8) != 0; }' % src
            if m == 'imul':
                return '{ const int32_t p_ = (int32_t)(int8_t)c.r[0] * (int8_t)%s; c.r[0] = (c.r[0] & 0xFFFF0000u) | ((uint32_t)p_ & 0xFFFF); cf = of = p_ != (int8_t)p_; }' % src
            if m == 'div':
                return '{ const uint32_t n_ = c.r[0] & 0xFFFF; const uint32_t d_ = %s; if (!d_) x86_divide_error(c, 0x%08Xu); c.r[0] = (c.r[0] & 0xFFFF0000u) | (n_ %% d_) << 8 | ((n_ / d_) & 0xFF); }' % (src, i.address)
            return '{ const int32_t n_ = (int16_t)c.r[0]; const int32_t d_ = (int8_t)%s; if (!d_) x86_divide_error(c, 0x%08Xu); c.r[0] = (c.r[0] & 0xFFFF0000u) | ((uint32_t)(n_ %% d_) & 0xFF) << 8 | ((uint32_t)(n_ / d_) & 0xFF); }' % (src, i.address)
        if m == 'cdq':
            return 'c.r[2] = (uint32_t)((int32_t)c.r[0] >> 31);'
        if m == 'cwde':
            return 'c.r[0] = (uint32_t)(int32_t)(int16_t)c.r[0];'
        if m == 'cbw':
            return 'c.r[0] = (c.r[0] & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)c.r[0];'
        if m == 'cwd':
            return 'c.r[2] = (c.r[2] & 0xFFFF0000u) | (uint16_t)((int16_t)c.r[0] >> 15);'
        if m.startswith('set') and m[3:] in COND:
            return self.wr(i, ops[0], '%s ? 1 : 0' % COND[m[3:]][0])
        if m.startswith('cmov') and m[4:] in COND:
            return 'if (%s) %s' % (COND[m[4:]][0], self.wr(i, ops[0], self.rd(i, ops[1])))
        if m == 'sahf':
            return '{ const uint32_t h_ = c.r[0] >> 8 & 0xFF; cf = h_ & 1; pf = h_ >> 2 & 1; zf = h_ >> 6 & 1; sf = h_ >> 7 & 1; }'
        if m == 'lahf':
            return 'c.r[0] = (c.r[0] & 0xFFFF00FFu) | (2u | cf | pf << 2 | zf << 6 | sf << 7) << 8;'
        if m == 'stc':
            return 'cf = 1;'
        if m == 'clc':
            return 'cf = 0;'
        if m == 'cmc':
            return 'cf ^= 1;'
        if m in ('bt', 'bts', 'btr', 'btc'):
            d, s = ops
            size = d.size
            if d.type == X.X86_OP_MEM and s.type == X.X86_OP_REG:
                addr = '(uint32_t)(%s + ((int32_t)%s >> 3))' % (self.addr(i, d), self.rd(i, s))
                bit = '(%s & 7)' % self.rd(i, s)
                pro = 'const uint32_t a_ = %s; const uint32_t b_ = %s; const uint8_t d = rd8(a_);' % (addr, bit)
                upd = {'bt': '', 'bts': 'wr8(a_, d | (1u << b_));', 'btr': 'wr8(a_, d & ~(1u << b_));', 'btc': 'wr8(a_, d ^ (1u << b_));'}[m]
                return '{ %s cf = d >> b_ & 1; %s }' % (pro, upd)
            pro, rdx, wrf = self.rmw(i, d)
            bit = '(%s & %d)' % (self.rd(i, s, 1) if s.type == X.X86_OP_REG else self.rd(i, s), size * 8 - 1)
            upd = {'bt': '', 'bts': wrf('d | (1u << b_)'), 'btr': wrf('d & ~(1u << b_)'), 'btc': wrf('d ^ (1u << b_)')}[m]
            return '{ %sconst %s d = %s; const uint32_t b_ = %s; cf = d >> b_ & 1; %s }' % (pro, UT[size], rdx, bit, upd)
        if m in ('bsf', 'bsr'):
            d, s = ops
            v = self.rd(i, s)
            f = '__builtin_ctz(v_)' if m == 'bsf' else '31 - __builtin_clz(v_)'
            return '{ const uint32_t v_ = %s; zf = v_ == 0; if (v_) %s }' % (v, self.wr(i, d, f))
        if m == 'bswap':
            return self.wr(i, ops[0], '__builtin_bswap32(%s)' % self.rd(i, ops[0]))
        if m == 'xadd':
            d, s = ops
            pro, rdx, wrf = self.rmw(i, d)
            t = UT[d.size]
            return '{ %sconst %s d = %s; const %s s = %s; const %s r = (%s)(d + s); %s %s %s }' % (
                pro, t, rdx, t, self.rd(i, s), t, t, self.flags_arith('add', live, d.size), self.wr(i, s, 'd'), wrf('r'))
        if m == 'cmpxchg':
            d, s_ = ops
            pro, rdx, wrf = self.rmw(i, d)
            t = UT[d.size]
            acc = {1: '(uint8_t)c.r[0]', 2: '(uint16_t)c.r[0]', 4: 'c.r[0]'}[d.size]
            setacc = {1: 'c.r[0] = (c.r[0] & 0xFFFFFF00u) | d;', 2: 'c.r[0] = (c.r[0] & 0xFFFF0000u) | d;', 4: 'c.r[0] = d;'}[d.size]
            return '{ %sconst %s d = %s; zf = d == %s; if (zf) { %s } else { %s } }' % (pro, t, rdx, acc, wrf(self.rd(i, s_)), setacc)
        if m == 'leave':
            return 'c.r[4] = c.r[5]; c.r[5] = rd32(c.r[4]); c.r[4] += 4;'
        if m == 'enter':
            n, lvl = ops[0].imm & 0xFFFF, ops[1].imm & 31
            if lvl:
                self.unimpl['enter level'] += 1
            return 'push32(c, c.r[5]); c.r[5] = c.r[4]; c.r[4] -= %d;' % n
        if m in ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb', 'lodsw', 'lodsd',
                 'scasb', 'scasw', 'scasd', 'cmpsb', 'cmpsw', 'cmpsd'):
            size = {'b': 1, 'w': 2, 'd': 4}[m[-1]]
            kind = m[:-1]
            fn = 'str_%s%d' % (kind, size * 8)
            r = {None: '0', 'rep': '1', 'repe': '1', 'repz': '1', 'repne': '2', 'repnz': '2'}[rep]
            if kind in ('scas', 'cmps'):
                fl = 'str_flags%d(c, cf, zf, sf, of, pf);' % (size * 8)
                return 'if (%s(c, %s)) { %s }' % (fn, r, fl)
            return '%s(c, %s);' % (fn, r)
        if m == 'xlatb':
            return 'c.r[0] = (c.r[0] & 0xFFFFFF00u) | rd8(c.r[3] + (uint8_t)c.r[0]);'
        # ---- control flow
        if m == 'call':
            o = ops[0]
            t = branch_target(i)
            if t is not None:
                if t in OVERRIDES:
                    return OVERRIDES[t](nxt)
                if t in self.disc.funcs:
                    return 'push32(c, 0x%08Xu); x86_%08X(c);' % (nxt, t)
                return 'push32(c, 0x%08Xu); call_ind(c, 0x%08Xu);' % (nxt, t)
            tgt = self.rd(i, o, 4)
            if o.type == X.X86_OP_MEM and o.mem.base == 0 and o.mem.index == 0:
                imp = prog.iat(o.mem.disp & 0xFFFFFFFF)
                if imp:
                    return '{ push32(c, 0x%08Xu); call_ind(c, rd32(0x%08Xu)); } /* %s!%s */' % (nxt, o.mem.disp & 0xFFFFFFFF, imp[0], imp[1])
            return '{ const uint32_t t_ = %s; push32(c, 0x%08Xu); call_ind(c, t_); }' % (tgt, nxt)
        if m in ('ret', 'retf'):
            n = (ops[0].imm & 0xFFFF) if ops else 0
            rt = getattr(self, 'ret_targets', None)
            if rt:
                cases = ' '.join('case 0x%08Xu: goto L_%08X;' % (t, t) for t in sorted(rt))
                return ('{ const uint32_t t_ = rd32(c.r[4]); c.r[4] += %d; if (t_ == entry_ret_) return; '
                        'switch (t_) { %s default: call_ind(c, t_); return; } }' % (4 + n, cases))
            return 'c.r[4] += %d; return;' % (4 + n)
        if m == 'jmp':
            t = branch_target(i)
            if t is not None:
                return jump(t)
            if i.address in tables:
                o = ops[0]
                idx = 'c.r[%d]' % REG32.index(i.reg_name(o.mem.index))
                cases = ' '.join('case %d: goto L_%08X;' % (k, tt) for k, tt in sorted(tables[i.address].items()) if tt in addrs and local_target(tt))
                return '{ const uint32_t t_ = rd32(%s); switch ((int32_t)%s) { %s default: call_ind(c, t_); return; } }' % (self.addr(i, o), idx, cases)
            return '{ const uint32_t t_ = %s; call_ind(c, t_); return; }' % self.rd(i, ops[0], 4)
        if is_jcc(m):
            return 'if (%s) %s' % (COND[m[1:]][0], jump(branch_target(i)))
        if m == 'jecxz':
            return 'if (c.r[1] == 0) %s' % jump(branch_target(i))
        if m in ('loop', 'loope', 'loopne'):
            cond = {'loop': '', 'loope': ' && zf', 'loopne': ' && !zf'}[m]
            return 'if (--c.r[1] != 0%s) %s' % (cond, jump(branch_target(i)))
        if m in ('int3', 'int', 'int1', 'hlt', 'ud2', 'into', 'iretd', 'in', 'out', 'insb', 'insd', 'outsb', 'outsd', 'cli', 'sti', 'arpl', 'bound', 'ljmp', 'lcall'):
            return 'trap(c, 0x%08Xu, "%s");' % (i.address, m)
        self.unimpl[m] += 1
        return 'trap(c, 0x%08Xu, "unimplemented %s");' % (i.address, m)

    # --- x87 (decoded from raw bytes)
    def gen_x87(self, i, op, modrm):
        mod, reg, rm = modrm >> 6, (modrm >> 3) & 7, modrm & 7
        a = i.address
        if mod != 3:
            mo = mem_op(i)
            ea = self.addr(i, mo)
            arith = ['fadd', 'fmul', 'fcom', 'fcomp', 'fsub', 'fsubr', 'fdiv', 'fdivr']
            if op in (0xD8, 0xDC, 0xDA, 0xDE):
                src = {0xD8: 'rdf32(%s)', 0xDC: 'rdf64(%s)', 0xDA: '(double)(int32_t)rd32(%s)', 0xDE: '(double)(int16_t)rd16(%s)'}[op] % ea
                return self.x87_arith(arith[reg], 'ST(0)', src, store_to_st0=True)
            if op == 0xD9:
                return {0: 'fpush(c, rdf32(%s));' % ea, 2: 'wrf32(%s, ST(0));' % ea, 3: 'wrf32(%s, ST(0)); fpop(c);' % ea,
                        4: 'fldenv(c, %s);' % ea, 5: 'c.cw = rd16(%s);' % ea, 6: 'fstenv(c, %s);' % ea,
                        7: 'wr16(%s, (uint16_t)c.cw);' % ea}.get(reg, 'trap(c, 0x%08Xu, "x87 D9 /%d");' % (a, reg))
            if op == 0xDB:
                return {0: 'fpush(c, (double)(int32_t)rd32(%s));' % ea, 2: 'wr32(%s, (uint32_t)fist32(c, ST(0)));' % ea,
                        3: 'wr32(%s, (uint32_t)fist32(c, ST(0))); fpop(c);' % ea, 5: 'fpush(c, rdf80(%s));' % ea,
                        7: 'wrf80(%s, ST(0)); fpop(c);' % ea}.get(reg, 'trap(c, 0x%08Xu, "x87 DB /%d");' % (a, reg))
            if op == 0xDD:
                return {0: 'fpush(c, rdf64(%s));' % ea, 2: 'wrf64(%s, ST(0));' % ea, 3: 'wrf64(%s, ST(0)); fpop(c);' % ea,
                        4: 'frstor(c, %s);' % ea, 6: 'fsave(c, %s);' % ea, 7: 'wr16(%s, fstsw(c));' % ea}.get(reg, 'trap(c, 0x%08Xu, "x87 DD /%d");' % (a, reg))
            if op == 0xDF:
                return {0: 'fpush(c, (double)(int16_t)rd16(%s));' % ea, 2: 'wr16(%s, (uint16_t)fist16(c, ST(0)));' % ea,
                        3: 'wr16(%s, (uint16_t)fist16(c, ST(0))); fpop(c);' % ea, 5: 'fpush(c, (double)(int64_t)rd64(%s));' % ea,
                        7: 'wr64(%s, (uint64_t)fist64(c, ST(0))); fpop(c);' % ea}.get(reg, 'trap(c, 0x%08Xu, "x87 DF /%d");' % (a, reg))
        # register forms
        sti = 'ST(%d)' % rm
        if op == 0xD8:
            return self.x87_arith(['fadd', 'fmul', 'fcom', 'fcomp', 'fsub', 'fsubr', 'fdiv', 'fdivr'][reg], 'ST(0)', sti, store_to_st0=True)
        if op == 0xDC:
            # st(i) = st(i) op st(0); note DC E0 = fsubr, E8 = fsub, F0 = fdivr, F8 = fdiv (Intel)
            name = ['fadd', 'fmul', 'fcom', 'fcomp', 'fsubr', 'fsub', 'fdivr', 'fdiv'][reg]
            return self.x87_arith_sti(name, rm, pop=False)
        if op == 0xDE:
            if reg == 3 and rm == 1:
                return 'fcom(c, ST(0), ST(1)); fpop(c); fpop(c);'
            name = ['fadd', 'fmul', 'fcom', 'fcomp', 'fsubr', 'fsub', 'fdivr', 'fdiv'][reg]
            return self.x87_arith_sti(name, rm, pop=True)
        if op == 0xD9:
            if reg == 0:
                return '{ const double v_ = %s; fpush(c, v_); }' % sti
            if reg == 1:
                return '{ const double t_ = ST(0); ST(0) = %s; %s = t_; }' % (sti, sti)
            if modrm == 0xD0:
                return ';'
            simple = {0xE0: 'ST(0) = -ST(0);', 0xE1: 'ST(0) = fabs(ST(0));', 0xE4: 'fcom(c, ST(0), 0.0);', 0xE5: 'fxam(c);',
                      0xE8: 'fpush(c, 1.0);', 0xE9: 'fpush(c, 3.321928094887362347870319429489390175864831393);',
                      0xEA: 'fpush(c, 1.442695040888963407359924681001892137426645954);', 0xEB: 'fpush(c, 3.141592653589793238462643383279502884197169399);',
                      0xEC: 'fpush(c, 0.301029995663981195213738894724493026768189881);', 0xED: 'fpush(c, 0.693147180559945309417232121458176568075500134);',
                      0xEE: 'fpush(c, 0.0);', 0xF0: 'ST(0) = exp2(ST(0)) - 1.0;', 0xF1: 'ST(1) = ST(1) * log2(ST(0)); fpop(c);',
                      0xF2: 'ST(0) = tan(ST(0)); fpush(c, 1.0); c.sw &= ~0x400u;', 0xF3: 'ST(1) = atan2(ST(1), ST(0)); fpop(c);',
                      0xF5: 'fprem(c, true);', 0xF6: 'c.top = (c.top - 1) & 7;', 0xF7: 'c.top = (c.top + 1) & 7;',
                      0xF8: 'fprem(c, false);', 0xF9: 'ST(1) = ST(1) * log2(ST(0) + 1.0); fpop(c);',
                      0xFA: 'ST(0) = sqrt(ST(0));', 0xFB: '{ const double v_ = ST(0); ST(0) = sin(v_); fpush(c, cos(v_)); c.sw &= ~0x400u; }',
                      0xFC: 'ST(0) = frnd(c, ST(0));', 0xFD: 'ST(0) = ldexp(ST(0), (int)trunc(ST(1)));',
                      0xFE: 'ST(0) = sin(ST(0)); c.sw &= ~0x400u;', 0xFF: 'ST(0) = cos(ST(0)); c.sw &= ~0x400u;',
                      0xF4: '{ const double v_ = ST(0); int e_; const double m_ = frexp(v_, &e_); ST(0) = (double)(e_ - 1); fpush(c, m_ * 2.0); }'}
            if modrm in simple:
                return simple[modrm]
        if op == 0xDA and modrm == 0xE9:
            return 'fcom(c, ST(0), ST(1)); fpop(c); fpop(c);'
        if op == 0xDB:
            if modrm == 0xE2:
                return 'c.sw &= 0x3800u;'
            if modrm == 0xE3:
                return 'finit(c);'
        if op == 0xDD:
            if reg == 0:
                return ';'  # ffree
            if reg == 2:
                return '%s = ST(0);' % sti
            if reg == 3:
                return '%s = ST(0); fpop(c);' % sti
            if reg == 4:
                return 'fcom(c, ST(0), %s);' % sti
            if reg == 5:
                return 'fcom(c, ST(0), %s); fpop(c);' % sti
        if op == 0xDF and modrm == 0xE0:
            return 'c.r[0] = (c.r[0] & 0xFFFF0000u) | fstsw(c);'
        self.unimpl['x87 %02X %02X' % (op, modrm)] += 1
        return 'trap(c, 0x%08Xu, "x87 %02X %02X");' % (a, op, modrm)

    def x87_arith(self, name, dst, src, store_to_st0):
        if name == 'fcom':
            return 'fcom(c, ST(0), %s);' % src
        if name == 'fcomp':
            return 'fcom(c, ST(0), %s); fpop(c);' % src
        expr = {'fadd': 'ST(0) + s_', 'fmul': 'ST(0) * s_', 'fsub': 'ST(0) - s_', 'fsubr': 's_ - ST(0)',
                'fdiv': 'ST(0) / s_', 'fdivr': 's_ / ST(0)'}[name]
        return '{ const double s_ = %s; ST(0) = %s; }' % (src, expr)

    def x87_arith_sti(self, name, i, pop):
        if name in ('fcom', 'fcomp'):
            return 'fcom(c, ST(0), ST(%d));%s' % (i, ' fpop(c);' if name == 'fcomp' or pop else '')
        expr = {'fadd': 'ST(%d) + ST(0)', 'fmul': 'ST(%d) * ST(0)', 'fsub': 'ST(%d) - ST(0)', 'fsubr': 'ST(0) - ST(%d)',
                'fdiv': 'ST(%d) / ST(0)', 'fdivr': 'ST(0) / ST(%d)'}[name] % i
        return 'ST(%d) = %s;%s' % (i, expr, ' fpop(c);' if pop else '')


SMC = {}  # instruction address -> [(patched byte offset, [values])] (filled from --smc)
OVERRIDES = {}  # call target -> fn(return address) -> C code (filled from --overrides)
# setjmp must run in the caller's host frame (the host longjmp returns there); longjmp unwinds
# the host stack to it. The guest jmp_buf still receives the saved registers.
OVERRIDE_KINDS = {
    'setjmp': lambda ret: ('{ push32(c, 0x%08Xu); const uint32_t jb_ = rd32(c.r[4] + 4); '
                           'if (_setjmp(*kp_jmpbuf(jb_)) == 0) { kp_setjmp_save(c, jb_); c.r[0] = 0; c.r[4] += 4; } '
                           'else kp_setjmp_restore(c, jb_); }' % ret),
    'longjmp': lambda ret: 'push32(c, 0x%08Xu); kp_longjmp(c);' % ret,
}


HEADER = '''// Generated by tools/x86recomp.py. Do not edit.
#include "../runtime/x86/cpu.h"
'''


def main():
    args = sys.argv[1:]
    out = args[0]
    seeds = []
    images = []
    k = 1
    while k < len(args):
        if args[k] == '--seeds':
            for l in open(args[k + 1]):
                l = l.split('#')[0].strip()
                if l:
                    seeds.append(int(l, 16))
            k += 2
            continue
        if args[k] == '--smc':
            for l in open(args[k + 1]):
                l = l.split('#')[0].split()
                if len(l) >= 3:
                    SMC.setdefault(int(l[0], 16), []).append((int(l[1]), [int(v, 16) for v in l[2].split(',')]))
            k += 2
            continue
        if args[k] == '--overrides':
            for l in open(args[k + 1]):
                l = l.split('#')[0].strip()
                if l:
                    addr, name = l.split()
                    a = int(addr, 16)
                    OVERRIDES[a] = OVERRIDE_KINDS[name] if name != 'indirect' else (
                        lambda t: lambda ret: 'push32(c, 0x%08Xu); call_ind(c, 0x%08Xu);' % (ret, t))(a)
            k += 2
            continue
        images.append(Image(args[k]))
        k += 1
    prog = Program(images)
    disc = Discovery(prog, seeds)
    disc.run()
    gen = Gen(prog, disc)
    gen.analyze_all(sorted(disc.funcs))
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.endswith('.gen.cpp'):
            os.remove(os.path.join(out, f))
    funcs = sorted(disc.funcs)
    files, cur, n, total = [], [], 0, 0
    for f in funcs:
        code = gen.gen_function(f)
        cur.append(code)
        cnt = len(disc.funcs[f][0])
        n += cnt
        total += cnt
        if n >= 6000:
            files.append(cur)
            cur, n = [], 0
    if cur:
        files.append(cur)
    with open(os.path.join(out, 'funcs.gen.h'), 'w') as fh:
        fh.write('// Generated by tools/x86recomp.py. Do not edit.\n#pragma once\n#include "../runtime/x86/cpu.h"\n\n')
        for f in funcs:
            fh.write('void x86_%08X(Cpu&);\n' % f)
    for k, body in enumerate(files):
        with open(os.path.join(out, 'funcs_%03d.gen.cpp' % k), 'w') as fh:
            fh.write(HEADER + '#include "funcs.gen.h"\n\n')
            fh.write('\n\n'.join(body) + '\n')
    with open(os.path.join(out, 'registry.gen.cpp'), 'w') as fh:
        fh.write(HEADER + '\n')
        for f in funcs:
            fh.write('void x86_%08X(Cpu&);\n' % f)
        fh.write('\nconst X86Func g_x86Funcs[] = {\n')
        for f in funcs:
            fh.write('    { 0x%08Xu, x86_%08X },\n' % (f, f))
        fh.write('};\nconst size_t g_x86FuncCount = %d;\n\n' % len(funcs))
        fh.write('const X86Import g_x86Imports[] = {\n')
        nimp = 0
        for im in images:
            for iat, (dll, name) in sorted(im.imports.items()):
                fh.write('    { 0x%08Xu, "%s", "%s" },\n' % (iat, dll, name))
                nimp += 1
        fh.write('};\nconst size_t g_x86ImportCount = %d;\n' % nimp)
    print('%d functions, %d instructions (with overlap), %d files' % (len(funcs), total, len(files)))
    if gen.unimpl:
        print('unimplemented:', dict(gen.unimpl.most_common(40)))
    if gen.warn:
        print('warnings:', dict(gen.warn))


if __name__ == '__main__':
    main()

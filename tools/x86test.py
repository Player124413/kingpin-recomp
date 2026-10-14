#!/usr/bin/env python3
"""Differential test of the recompiler's instruction semantics against Unicorn (QEMU x86).

usage: x86test.py <out-dir>   then build and run <out-dir>/x86test.cpp (see the printed command)

Each case is a snippet of 32-bit x86 (Intel syntax) with initial registers, flags and a data
area at 0x2000. Unicorn computes the expected registers, defined flags and data; the recompiler
generates C++ for the same bytes, which runs from the same initial state.
"""
import os
import random
import struct
import subprocess
import sys
import tempfile

from elftools.elf.elffile import ELFFile
import unicorn as U
from unicorn import x86_const as UX

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import x86recomp as R

CODE, DATA, STACK = 0x1000, 0x2000, 0x3000
DATA_SIZE = 0x200
F = {'C': 1, 'P': 4, 'Z': 0x40, 'S': 0x80, 'O': 0x800}
UREGS = [UX.UC_X86_REG_EAX, UX.UC_X86_REG_ECX, UX.UC_X86_REG_EDX, UX.UC_X86_REG_EBX,
         UX.UC_X86_REG_ESP, UX.UC_X86_REG_EBP, UX.UC_X86_REG_ESI, UX.UC_X86_REG_EDI]
rng = random.Random(1234)
EDGE = [0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE]


def rval():
    return rng.choice(EDGE) if rng.random() < 0.4 else rng.getrandbits(32)


cases = []  # (snippet list, regs[8], eflags, data bytes, flag mask)


def add(snippet, mask, regs=None, eflags=None, data=None, n=1):
    for _ in range(n):
        r = regs or [rval() for _ in range(8)]
        r = list(r)
        r[4] = STACK
        fl = eflags if eflags is not None else rng.choice([0x2, 0x2 | 0x8D5, 0x3, 0x2 | 0x40, 0x2 | 0x801])
        d = data if data is not None else bytes(rng.getrandbits(8) for _ in range(DATA_SIZE))
        cases.append((snippet, r, fl, d, mask))


ALLF = 'CZSOP'
R32 = ['eax', 'ecx', 'edx', 'ebx', 'esi', 'edi']
R16 = ['ax', 'cx', 'dx', 'bx', 'si', 'di']
R8 = ['al', 'cl', 'dl', 'bl', 'ah', 'ch', 'dh', 'bh']

for regs in (R32, R16, R8):
    for op in ['add', 'adc', 'sub', 'sbb', 'cmp', 'and', 'or', 'xor', 'test']:
        for _ in range(3):
            a, b = rng.choice(regs), rng.choice(regs)
            add(['%s %s, %s' % (op, a, b)], ALLF, n=8)
        imm = rng.choice([1, 0x7F, -1, -128])
        add(['%s %s, %d' % (op, regs[0], imm)], ALLF, n=8)
        add(['%s %s, %d' % (op, regs[1], imm)], ALLF, n=8)
    for op in ['neg', 'not', 'inc', 'dec']:
        add(['%s %s' % (op, rng.choice(regs))], {'neg': ALLF, 'not': '', 'inc': 'ZSOP', 'dec': 'ZSOP'}[op], n=16)
    for op in ['shl', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr']:
        bits = {R32[0]: 32, R16[0]: 16, R8[0]: 8}[regs[0]]
        for cnt in [1, 2, 3, 5, 7] + ([12, 15] if bits >= 16 else []) + ([17, 31] if bits == 32 else []):
            m = ALLF if cnt == 1 else 'CZSP'
            if op in ('rol', 'ror', 'rcl', 'rcr'):
                m = 'CO' if cnt == 1 else 'C'
            if op in ('rcl', 'rcr') and bits < 32:
                continue  # rotate-through-carry modulo (bits+1) for narrow operands: rare
            add(['%s %s, %d' % (op, rng.choice(regs), cnt)], m, n=6)
        if op in ('shl', 'shr', 'sar'):
            for cl in [0, 1, 4, 9, 31] if bits == 32 else [0, 1, 4, 7]:
                r = [rval() for _ in range(8)]
                r[1] = (r[1] & ~0xFF) | cl
                m = '' if cl == 0 else (ALLF if cl == 1 else 'CZSP')
                add(['%s %s, cl' % (op, regs[0])], m, regs=r, n=1)
                add(['%s %s, cl' % (op, regs[2])], m, regs=r, n=1)

for op in ['mul', 'imul']:
    for reg in ['ecx', 'ebx', 'cx', 'bl', 'ah']:
        add(['%s %s' % (op, reg)], 'CO', n=10)
add(['imul eax, ecx'], 'CO', n=20)
add(['imul edx, ebx, 12'], 'CO', n=10)
add(['imul ecx, esi, -100000'], 'CO', n=10)
add(['imul cx, si'], 'CO', n=10)
for op in ['div', 'idiv']:
    for reg in ['ecx', 'ebx', 'cx', 'bl']:
        for _ in range(20):
            r = [rval() for _ in range(8)]
            r[2] = rng.choice([0, 0xFFFFFFFF, 1, rng.getrandbits(8)]) if op == 'idiv' else rng.getrandbits(4)
            add(['%s %s' % (op, reg)], '', regs=r)
add(['cdq'], '', n=10)
add(['cwde'], '', n=10)
add(['cbw'], '', n=10)
add(['cwd'], '', n=10)
for cc in ['o', 'no', 'b', 'ae', 'e', 'ne', 'be', 'a', 's', 'ns', 'p', 'np', 'l', 'ge', 'le', 'g']:
    add(['cmp eax, ecx', 'set%s dl' % cc], ALLF, n=12)
    add(['sub bl, cl', 'set%s ah' % cc], ALLF, n=6)
    add(['cmp eax, ecx', 'cmov%s edx, esi' % cc], ALLF, n=6)
add(['movsx eax, cl'], '', n=10)
add(['movsx eax, cx'], '', n=10)
add(['movzx edx, bh'], '', n=10)
add(['movsx cx, dl'], '', n=10)
add(['xchg eax, ecx', 'xchg dl, bh', 'xchg si, di'], '', n=10)
add(['lea eax, [ecx + edx*4 + 0x10]', 'lea esi, [esi + esi*2]'], '', n=10)
add(['sahf'], 'CZSP', n=10)
add(['lahf'], '', eflags=0x8C7, n=3)
add(['lahf'], '', eflags=0x2, n=3)
add(['bt ecx, 5', 'setb al'], '', n=10)
add(['bts ecx, edx', 'btr esi, 3', 'btc edi, ebx'], 'C', n=10)
add(['bsf eax, ecx'], 'Z', n=10)
add(['bsr eax, ecx'], 'Z', n=10)
add(['bswap ecx'], '', n=5)
add(['xadd ecx, edx'], ALLF, n=10)
add(['shld edx, eax, 4', 'shrd esi, edi, 9'], 'CZSP', n=10)
add(['shld edx, eax, cl'], '', n=10)  # flags depend on count; only results
add(['stc', 'cmc', 'adc eax, 0'], ALLF, n=4)
add(['clc', 'sbb ecx, ecx'], ALLF, n=4)
add(['neg eax', 'sbb eax, eax', 'neg eax'], ALLF, n=10)
# memory forms
add(['add dword ptr [0x2100], ecx', 'sub byte ptr [0x2105], dl', 'inc word ptr [0x2108]', 'xor dword ptr [0x2110], 0x55'], ALLF, n=10)
add(['shl dword ptr [0x2120], 3', 'sar word ptr [0x2124], 1', 'not dword ptr [0x2128]'], 'CZSP', n=10)
add(['push ecx', 'push word ptr [0x2100]', 'pop dx', 'pop esi'], '', n=5)
add(['mov ecx, 3', 'mov esi, 0x2100', 'mov edi, 0x2140', 'cld', 'rep movsd'], '', n=3)
add(['mov ecx, 5', 'mov edi, 0x2140', 'mov eax, 0x11223344', 'cld', 'rep stosb'], '', n=3)
add(['mov ecx, 9', 'mov esi, 0x2100', 'mov edi, 0x2101', 'std', 'rep movsb', 'cld'], '', n=3)
add(['mov byte ptr [0x2108], 0', 'mov ecx, 0xffffffff', 'mov edi, 0x2100', 'xor eax, eax', 'cld', 'repne scasb', 'not ecx', 'dec ecx'], 'Z', n=5)
add(['mov ecx, 4', 'mov esi, 0x2100', 'mov edi, 0x2100', 'cld', 'repe cmpsd'], ALLF, n=3)
add(['mov ecx, 4', 'mov esi, 0x2100', 'mov edi, 0x2104', 'cld', 'repe cmpsb'], ALLF, n=3)
add(['xlatb'], '', regs=[0, 0, 0, 0x2100, 0, 0, 0, 0], n=1)


# ---- x87: three doubles at 0x2000/8/10 are loaded (ST0 = third); results via fstp to 0x2180+
def fdata(vals, cw=0x27F, extra=b''):
    d = bytearray(DATA_SIZE)
    for k, v in enumerate(vals):
        struct.pack_into('<d', d, 8 * k, v)
    struct.pack_into('<H', d, 0x18, cw)
    struct.pack_into('<i', d, 0x1C, rng.randint(-1000, 1000))
    struct.pack_into('<f', d, 0x20, rng.uniform(-100, 100))
    struct.pack_into('<h', d, 0x24, rng.randint(-300, 300))
    d[0x30:0x30 + len(extra)] = extra
    return bytes(d)


FLD = ['fldcw word ptr [0x2018]', 'fld qword ptr [0x2000]', 'fld qword ptr [0x2008]', 'fld qword ptr [0x2010]']
FOUT = ['fnstsw ax', 'fstp qword ptr [0x2180]', 'fstp qword ptr [0x2188]', 'fstp qword ptr [0x2190]', 'fstp qword ptr [0x2198]']


def fvals():
    return [rng.choice([rng.uniform(-1000, 1000), rng.uniform(-1, 1), float(rng.randint(-50, 50)), 0.0, 2.5, -2.5, 1e10])
            for _ in range(3)]


x87ops = []
for name in ['fadd', 'fmul', 'fsub', 'fsubr', 'fdiv', 'fdivr']:
    x87ops += ['%s st(0), st(2)' % name, '%s st(2), st(0)' % name, '%sp st(1), st(0)' % name, '%sp st(2), st(0)' % name,
               '%s qword ptr [0x2000]' % name, '%s dword ptr [0x2020]' % name]
    x87ops += ['fi%s dword ptr [0x201c]' % name[1:], 'fi%s word ptr [0x2024]' % name[1:]]
x87ops += ['fcom st(2)', 'fcomp st(1)', 'fcompp', 'fucompp', 'fcom qword ptr [0x2000]', 'fcomp dword ptr [0x2020]', 'ftst',
           'fxch st(2)', 'fchs', 'fabs', 'fsqrt', 'frndint', 'fscale', 'fprem', 'fprem1', 'fsin', 'fcos', 'fpatan', 'fptan',
           'fld st(1)', 'fst st(2)', 'fstp st(1)', 'fld1', 'fldz', 'fldpi', 'fxam', 'fyl2x', 'f2xm1',
           'fist dword ptr [0x2040]', 'fistp dword ptr [0x2040]', 'fistp qword ptr [0x2040]', 'fistp word ptr [0x2040]',
           'fild dword ptr [0x201c]', 'fild word ptr [0x2024]', 'fst dword ptr [0x2040]', 'fstp dword ptr [0x2040]',
           'fld dword ptr [0x2020]', 'fnstcw word ptr [0x2040]', 'fld tbyte ptr [0x2030]', 'fstp tbyte ptr [0x2040]']
for op in x87ops:
    for _ in range(6):
        vals = fvals()
        if op in ('fsqrt', 'fyl2x'):
            vals = [abs(v) + 0.5 for v in vals]
        if op == 'f2xm1':
            vals = [rng.uniform(-1, 1) for _ in vals]
        if op in ('fprem', 'fprem1', 'fscale'):
            vals = [rng.uniform(-100, 100), rng.uniform(1, 9), rng.uniform(-100, 100)]
        cw = rng.choice([0x27F, 0x67F, 0xA7F, 0xE7F]) if ('ist' in op or 'rndint' in op) else 0x27F
        extra = b''
        if 'tbyte' in op:
            import math
            v = rng.uniform(-1e6, 1e6)
            m, e = math.frexp(abs(v))
            mant = int(m * (1 << 64))
            exp = e - 1 + 16383
            extra = struct.pack('<QH', mant, exp | (0x8000 if v < 0 else 0))
        add(FLD + [op] + FOUT, '', regs=[0] * 8, eflags=2, data=fdata(vals, cw, extra))


# ---------------------------------------------------------------- assemble
snips = sorted({tuple(c[0]) for c in cases})
src = ['.intel_syntax noprefix', '.text']
for k, s in enumerate(snips):
    src += s
    src.append('hlt')
tmp = tempfile.mkdtemp()
with open(os.path.join(tmp, 't.s'), 'w') as f:
    f.write('\n'.join(src) + '\n')
subprocess.check_call(['clang', '-target', 'i386-unknown-linux-gnu', '-c', os.path.join(tmp, 't.s'), '-o', os.path.join(tmp, 't.o')])
text = ELFFile(open(os.path.join(tmp, 't.o'), 'rb')).get_section_by_name('.text').data()
md = R.md
snip_bytes = {}
off = 0
for s in snips:
    start = off
    for _ in range(len(s)):
        i = next(md.disasm(text[off:], off, 1))
        off += i.size
    assert text[off] == 0xF4, s
    snip_bytes[s] = text[start:off]
    off += 1


# ---------------------------------------------------------------- reference
def run_unicorn(code, regs, eflags, data):
    mu = U.Uc(U.UC_ARCH_X86, U.UC_MODE_32)
    mu.mem_map(0, 0x10000)
    mu.mem_write(CODE, code + b'\xf4')
    mu.mem_write(DATA, data)
    for k, r in enumerate(UREGS):
        mu.reg_write(r, regs[k])
    mu.reg_write(UX.UC_X86_REG_EFLAGS, eflags)
    try:
        mu.emu_start(CODE, CODE + len(code))
    except U.UcError:
        return None
    return [mu.reg_read(r) for r in UREGS], mu.reg_read(UX.UC_X86_REG_EFLAGS), bytes(mu.mem_read(DATA, DATA_SIZE))


class FakeImage:
    def __init__(self, base, data):
        self.base, self.data = base, data
        self.imports, self.relocs = {}, set()

    def contains(self, a):
        return self.base <= a < self.base + len(self.data)

    def in_text(self, a):
        return self.contains(a)

    def read(self, a, n):
        o = a - self.base
        return bytes(self.data[o:o + n]) + bytes(max(0, n - (len(self.data) - o)))


class FakeProg(R.Program):
    def __init__(self, im):
        super().__init__([])
        self.im = im

    def image(self, a):
        return self.im if self.im.contains(a) else None

    def decode(self, a):
        return next(md.disasm(self.im.read(a, 16), a, 1), None)


class FakeDisc:
    funcs, strong = {}, set()


out = sys.argv[1]
os.makedirs(out, exist_ok=True)
lines = ['#include "../runtime/x86/cpu.h"', '#include <cstdio>', '#include <sys/mman.h>', '',
         'uint8_t* g_mem;',
         'void call_ind(Cpu&, uint32_t) {} void trap(Cpu&, uint32_t a, const char* w) { printf("trap %08x %s\\n", a, w); throw 1; }',
         'void x86_fallthrough(Cpu&, uint32_t) {} void x86_divide_error(Cpu&, uint32_t) { throw 2; }', '']
fn_of = {}
for k, s in enumerate(snips):
    code = snip_bytes[s]
    prog = FakeProg(FakeImage(CODE, code))
    gen = R.Gen(prog, FakeDisc())
    body = []
    a = CODE
    while a < CODE + len(code):
        i = prog.decode(a)
        body.append('    %s // %s %s' % (gen.gen_insn(i, R.ALL, set(), lambda t: False, {}), i.mnemonic, i.op_str))
        a += i.size
    lines.append('static void t%d(Cpu& c, uint32_t* fl)\n{\n    uint32_t cf = fl[0], zf = fl[1], sf = fl[2], of = fl[3], pf = fl[4];' % k)
    lines += body
    lines.append('    fl[0] = cf; fl[1] = zf; fl[2] = sf; fl[3] = of; fl[4] = pf;\n}')
    fn_of[s] = k

lines.append('struct Case { void (*fn)(Cpu&, uint32_t*); uint32_t regs[8], eflags, mask; uint32_t oregs[8], oeflags; const char* name; uint32_t data, odata; };')
blobs = []
crows = []
skipped = 0
for s, regs, fl, data, mask in cases:
    s = tuple(s)
    res = run_unicorn(snip_bytes[s], regs, fl, data)
    if res is None:
        skipped += 1
        continue
    oregs, ofl, odata = res
    m = sum(F[x] for x in mask)
    blobs.append(data)
    blobs.append(odata)
    crows.append('    { t%d, { %s }, 0x%X, 0x%X, { %s }, 0x%X, "%s", %d, %d },' % (
        fn_of[s], ', '.join('0x%X' % x for x in regs), fl, m, ', '.join('0x%X' % x for x in oregs), ofl,
        ' ; '.join(s).replace('"', ''), len(blobs) - 2, len(blobs) - 1))
lines.append('static const uint8_t kData[][%d] = {' % DATA_SIZE)
for b in blobs:
    lines.append('    { %s },' % ', '.join(str(x) for x in b))
lines.append('};')
lines.append('static const Case kCases[] = {')
lines += crows
lines.append('};')
lines.append(r'''
static uint32_t pack(const uint32_t* f) { return f[0] | f[4] << 2 | f[1] << 6 | f[2] << 7 | f[3] << 11; }
int main()
{
    g_mem = (uint8_t*)mmap(nullptr, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    int fails = 0, n = 0;
    for (const Case& k : kCases)
    {
        n++;
        Cpu c{};
        memcpy(c.r, k.regs, 32);
        memcpy(g_mem + 0x2000, kData[k.data], sizeof kData[0]);
        uint32_t fl[5] = { k.eflags & 1, k.eflags >> 6 & 1, k.eflags >> 7 & 1, k.eflags >> 11 & 1, k.eflags >> 2 & 1 };
        c.df = k.eflags >> 10 & 1;
        try { k.fn(c, fl); } catch (int) { printf("FAIL (exception) %s\n", k.name); fails++; continue; }
        const bool x87 = strstr(k.name, "fnstsw") != nullptr;
        bool ok = memcmp(c.r + 1, k.oregs + 1, 28) == 0 && ((pack(fl) ^ k.oeflags) & k.mask) == 0 &&
                  ((c.r[0] ^ k.oregs[0]) & (x87 ? (strstr(k.name, "fxam") || strstr(k.name, "fprem") ? 0xFFFF7F00u : 0xFFFF7D00u) : ~0u)) == 0;
        // x87 results (doubles at 0x2180..0x219F) compare with a relative tolerance
        const uint8_t* od = kData[k.odata];
        for (int o = 0; o < 0x200 && ok; o++)
        {
            if (o >= 0x180 && o < 0x1A0) continue;
            if (g_mem[0x2000 + o] != od[o]) ok = false;
        }
        const int depth = (8 - ((k.oregs[0] >> 11) & 7)) & 7;
        for (int s = 0; s < 4 && ok; s++)
        {
            if (strstr(k.name, "fnstsw") == nullptr) break;
            if (s >= depth) break;
            double a, b;
            memcpy(&a, g_mem + 0x2180 + 8 * s, 8);
            memcpy(&b, od + 0x180 + 8 * s, 8);
            if (!(a == b || (a != a && b != b) || fabs(a - b) <= 1e-9 * fmax(1.0, fabs(b)))) ok = false;
        }
        if (!ok)
        {
            if (fails++ < 60)
            {
                printf("FAIL %s\n  in  regs", k.name);
                for (int r = 0; r < 8; r++) printf(" %08X", k.regs[r]);
                printf(" fl %03X\n  got regs", k.eflags);
                for (int r = 0; r < 8; r++) printf(" %08X", c.r[r]);
                printf(" fl %03X\n  exp regs", pack(fl));
                for (int r = 0; r < 8; r++) printf(" %08X", k.oregs[r]);
                printf(" fl %03X mask %03X\n", k.oeflags, k.mask);
                if (strstr(k.name, "fnstsw"))
                    for (int s = 0; s < 4; s++) { double a, b; memcpy(&a, g_mem + 0x2180 + 8 * s, 8); memcpy(&b, od + 0x180 + 8 * s, 8); printf("  st%d got %.17g exp %.17g\n", s, a, b); }
            }
        }
    }
    printf("%d/%d x86 cases passed\n", n - fails, n);
    return fails != 0;
}
''')
with open(os.path.join(out, 'x86test.cpp'), 'w') as f:
    f.write('\n'.join(lines) + '\n')
print('%d cases (%d skipped as faulting), %d snippets' % (len(crows), skipped, len(snips)))
print('build: clang++ -std=c++20 -O1 -w -fexceptions %s/x86test.cpp runtime/x86/x87.cpp runtime/x86/strings.cpp -o %s/x86test' % (out, out))

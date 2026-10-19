# Static recompiler: GameCube main.dol (Gekko PowerPC) -> C.
#
# Every guest entry point becomes `void fn_XXXXXXXX(PPCContext *c)`.
#   * Entry points = dtk functions + every branch target that lands outside the
#     branching function (secondary entries / gap code) + fallthrough targets.
#   * Branches inside a function become gotos; branches out become tail calls.
#   * bl -> direct C call; blr -> return; bctr/bctrl/blrl -> call_indirect().
#   * bctr inside a function with jump-table targets -> switch on CTR.
# Functions listed in the HLE override file are emitted as orig_XXXXXXXX so the
# runtime can supply fn_XXXXXXXX itself.
#
# usage: ppcrecomp.py main.dol symbols.txt hle_overrides.txt outdir
import os, re, struct, sys, bisect, io

class Out(io.StringIO):
    """Text file that is only rewritten when its content changes (keeps rebuilds incremental)."""
    def __init__(self, path): super().__init__(); self.path = path
    def __enter__(self): return self
    def __exit__(self, *a):
        new = self.getvalue()
        try: old = open(self.path).read()
        except OSError: old = None
        if old != new: open(self.path, 'w').write(new)

DOL, SYMS, HLE, OUT = sys.argv[1:5]
dol = open(DOL, 'rb').read()
off = struct.unpack('>18I', dol[0:0x48]); addr = struct.unpack('>18I', dol[0x48:0x90]); size = struct.unpack('>18I', dol[0x90:0xd8])
TEXT = [(addr[i], size[i], off[i]) for i in range(7) if size[i]]
ALLSECS = [(addr[i], size[i], off[i]) for i in range(18) if size[i]]

def in_text(a): return any(b <= a < b + s for b, s, o in TEXT)
def word(a):
    for b, s, o in ALLSECS:
        if b <= a and a + 4 <= b + s: return struct.unpack('>I', dol[o + a - b:o + a - b + 4])[0]
    return None

# ---------------------------------------------------------------- symbols
funcs, names, jtables = {}, {}, []
for l in open(SYMS):
    m = re.match(r'^(\S+) = \.(\w+):0x([0-9A-Fa-f]+); // (.*)$', l)
    if not m: continue
    nm, sec, a, attrs = m[1], m[2], int(m[3], 16), m[4]
    sz = re.search(r'size:0x([0-9A-Fa-f]+)', attrs)
    if 'type:function' in attrs and sz:
        funcs[a] = int(sz[1], 16); names[a] = nm
    if nm.startswith('jumptable_') and sz:
        jtables.append((a, int(sz[1], 16)))
fstarts = sorted(funcs)

jt_targets = set()
for a, s in jtables:
    for k in range(0, s, 4):
        v = word(a + k)
        if v and in_text(v): jt_targets.add(v)

overrides = set()
if os.path.exists(HLE):
    for l in open(HLE):
        l = l.split('#')[0].strip()
        if not l: continue
        if re.fullmatch(r'(0x)?[0-9A-Fa-f]{8}', l): overrides.add(int(l, 16))
        else:
            hit = [a for a, n in names.items() if n == l]
            if not hit: print('warning: HLE override not found:', l)
            overrides.update(hit)

def region_end(e):
    """End of the code region starting at entry e."""
    i = bisect.bisect_right(fstarts, e) - 1
    if i >= 0 and fstarts[i] <= e < fstarts[i] + funcs[fstarts[i]]:
        return fstarts[i] + funcs[fstarts[i]]
    j = bisect.bisect_right(fstarts, e)
    nxt = fstarts[j] if j < len(fstarts) else None
    for b, s, o in TEXT:
        if b <= e < b + s:
            return min(nxt, b + s) if nxt else b + s
    raise ValueError(hex(e))

# ---------------------------------------------------------------- helpers
def sext16(v): return v - 0x10000 if v & 0x8000 else v
def mask(mb, me):
    m1 = 0xFFFFFFFF >> mb
    m2 = (0xFFFFFFFF << (31 - me)) & 0xFFFFFFFF
    return (m1 & m2) if mb <= me else (m1 | m2)
R = lambda n: f'c->r[{n}]'
F = lambda n: f'c->f[{n}]'
P = lambda n: f'c->ps1[{n}]'
def RA0(n): return '0' if n == 0 else R(n)
def ea_d(ra, d): return f'({RA0(ra)} + {d:#x}u)' if d >= 0 else f'({RA0(ra)} - {-d:#x}u)'
def ea_x(ra, rb): return f'({RA0(ra)} + {R(rb)})'

def branch_targets(w, a):
    op = w >> 26
    if op == 18:
        li = w & 0x03FFFFFC
        if li & 0x02000000: li -= 0x04000000
        return ((li if w & 2 else a + li) & 0xFFFFFFFF, w & 1, True)
    if op == 16:
        bd = sext16(w & 0xFFFC)
        uncond = ((w >> 21) & 0x14) == 0x14
        return ((bd if w & 2 else a + bd) & 0xFFFFFFFF, w & 1, uncond)
    return None

def bc_cond(w):
    bo, bi = (w >> 21) & 31, (w >> 16) & 31
    parts = []
    if not bo & 4:
        parts.append('(--c->ctr == 0)' if bo & 2 else '(--c->ctr != 0)')
    if not bo & 16:
        bit = f'(c->cr & {1 << (31 - bi):#x}u)'
        parts.append(bit if bo & 8 else f'!{bit}')
    return ' && '.join(parts) if parts else None

# ---------------------------------------------------------------- per-insn C
class Gen:
    def __init__(self, start, end, labels):
        self.start, self.end, self.labels = start, end, labels
    def inside(self, t): return self.start <= t < self.end

    def call_or_goto(self, t, link, nxt):
        call = f'fn_{t:08X}(c)' if in_text(t) else f'call_indirect(c, {t:#x}u)'
        if link:
            if t == nxt: return f'c->lr = {nxt:#x}u;'
            return f'c->lr = {nxt:#x}u; {call};'
        if self.inside(t):
            if t <= nxt - 4:  # backward branch: interrupt safe point
                return f'{{ if (g_irq_pending) irq_check(c); goto L_{t:08X}; }}'
            return f'goto L_{t:08X};'
        return f'{{ {call}; return; }}'

    def insn(self, a, w):
        op = w >> 26
        rd, ra, rb, rc = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
        rec = w & 1
        imm = w & 0xFFFF; simm = sext16(imm)
        nxt = a + 4
        U = lambda: f'ppc_unimpl(c, {a:#x}u, {w:#x}u);'
        def recd(reg): return f' REC(c, {R(reg)});' if rec else ''

        if op == 14:  # addi
            return f'{R(rd)} = {RA0(ra)} + {simm & 0xFFFFFFFF:#x}u;'
        if op == 15:  # addis
            return f'{R(rd)} = {RA0(ra)} + {(imm << 16) & 0xFFFFFFFF:#x}u;'
        if op == 7:   return f'{R(rd)} = (uint32_t)((int32_t){R(ra)} * {simm});'
        if op == 8:   # subfic
            return (f'{{ uint32_t a_ = {R(ra)}; c->xer_ca = {simm & 0xFFFFFFFF:#x}u >= a_; '
                    f'{R(rd)} = {simm & 0xFFFFFFFF:#x}u - a_; }}')
        if op in (12, 13):  # addic / addic.
            s = (f'{{ uint32_t a_ = {R(ra)}; uint32_t t_ = a_ + {simm & 0xFFFFFFFF:#x}u; '
                 f'c->xer_ca = t_ < a_; {R(rd)} = t_; }}')
            return s + (f' REC(c, {R(rd)});' if op == 13 else '')
        if op == 10: return f'cmp_u(c, {rd >> 2}, {R(ra)}, {imm:#x}u);'
        if op == 11: return f'cmp_s(c, {rd >> 2}, (int32_t){R(ra)}, {simm});'
        if op == 24: return f'{R(ra)} = {R(rd)} | {imm:#x}u;'
        if op == 25: return f'{R(ra)} = {R(rd)} | {imm << 16:#x}u;'
        if op == 26: return f'{R(ra)} = {R(rd)} ^ {imm:#x}u;'
        if op == 27: return f'{R(ra)} = {R(rd)} ^ {imm << 16:#x}u;'
        if op == 28: return f'{R(ra)} = {R(rd)} & {imm:#x}u; REC(c, {R(ra)});'
        if op == 29: return f'{R(ra)} = {R(rd)} & {imm << 16:#x}u; REC(c, {R(ra)});'
        if op in (20, 21, 23):
            sh, mb, me = rb, rc, (w >> 1) & 31
            m = mask(mb, me)
            rot = f'rotl32({R(rd)}, {sh})' if op != 23 else f'rotl32({R(rd)}, {R(rb)} & 31)'
            if op == 21 and sh == 0: rot = R(rd)
            if op == 20:
                s = f'{R(ra)} = ({rot} & {m:#x}u) | ({R(ra)} & {(~m) & 0xFFFFFFFF:#x}u);'
            else:
                s = f'{R(ra)} = {rot} & {m:#x}u;'
            return s + recd(ra)
        if op == 3:   # twi
            to = rd
            conds = []
            if to & 16: conds.append(f'(int32_t){R(ra)} < {simm}')
            if to & 8:  conds.append(f'(int32_t){R(ra)} > {simm}')
            if to & 4:  conds.append(f'{R(ra)} == {simm & 0xFFFFFFFF:#x}u')
            if to & 2:  conds.append(f'{R(ra)} < {simm & 0xFFFFFFFF:#x}u')
            if to & 1:  conds.append(f'{R(ra)} > {simm & 0xFFFFFFFF:#x}u')
            return f'if ({" || ".join(conds) or "0"}) ppc_trap(c, {a:#x}u);'
        if op == 17: return f'ppc_sc(c, {a:#x}u);'
        if op == 18:
            t, link, _ = branch_targets(w, a)
            return self.call_or_goto(t, link, nxt)
        if op == 16:
            t, link, _ = branch_targets(w, a)
            cond = bc_cond(w)
            if link:  # bcl: LR is written whether or not the branch is taken
                if t == nxt: return f'c->lr = {nxt:#x}u;'
                call = f'fn_{t:08X}(c)' if in_text(t) else f'call_indirect(c, {t:#x}u)'
                return f'c->lr = {nxt:#x}u; ' + (f'if ({cond}) ' if cond else '') + f'{call};'
            body = self.call_or_goto(t, link, nxt)
            return f'if ({cond}) {body}' if cond else body
        if op == 19: return self.op19(a, w)
        if op == 31: return self.op31(a, w)
        # ---- loads / stores (D-form)
        LS = {32: ('l', 32), 34: ('l', 8), 40: ('l', 16), 42: ('a', 16),
              36: ('s', 32), 38: ('s', 8), 44: ('s', 16)}
        LSU = {33: 32, 35: 34, 41: 40, 43: 42, 37: 36, 39: 38, 45: 44}
        base = LSU.get(op, op)
        if base in LS:
            kind, bits = LS[base]
            upd = op in LSU
            ea = f'({R(ra)} + {simm & 0xFFFFFFFF:#x}u)' if upd else ea_d(ra, simm)
            if kind == 'l':   v = f'rd{bits}(ea_)'
            elif kind == 'a': v = '(uint32_t)(int32_t)(int16_t)rd16(ea_)'
            if kind == 's':
                cast = {8: '(uint8_t)', 16: '(uint16_t)', 32: ''}[bits]
                s = f'wr{bits}(ea_, {cast}{R(rd)});'
            else:
                s = f'{R(rd)} = {v};'
            return f'{{ uint32_t ea_ = {ea}; {s}' + (f' {R(ra)} = ea_;' if upd else '') + ' }'
        if op == 46:  # lmw
            return '{ uint32_t ea_ = ' + ea_d(ra, simm) + '; ' + ' '.join(
                f'{R(i)} = rd32(ea_ + {4 * (i - rd)}u);' for i in range(rd, 32)) + ' }'
        if op == 47:  # stmw
            return '{ uint32_t ea_ = ' + ea_d(ra, simm) + '; ' + ' '.join(
                f'wr32(ea_ + {4 * (i - rd)}u, {R(i)});' for i in range(rd, 32)) + ' }'
        FLS = {48: 'lfs', 49: 'lfsu', 50: 'lfd', 51: 'lfdu', 52: 'stfs', 53: 'stfsu', 54: 'stfd', 55: 'stfdu'}
        if op in FLS:
            upd = FLS[op].endswith('u')
            ea = f'({R(ra)} + {simm & 0xFFFFFFFF:#x}u)' if upd else ea_d(ra, simm)
            return '{ uint32_t ea_ = ' + ea + '; ' + self.fls(FLS[op].rstrip('u'), rd) + \
                   (f' {R(ra)} = ea_;' if upd else '') + ' }'
        if op in (56, 57, 60, 61):  # psq_l / psq_lu / psq_st / psq_stu
            d = w & 0xFFF
            if d & 0x800: d -= 0x1000
            W, I = (w >> 15) & 1, (w >> 12) & 7
            upd = op in (57, 61)
            ea = f'({R(ra)} + {d & 0xFFFFFFFF:#x}u)' if upd else ea_d(ra, d)
            fn = 'psq_load' if op in (56, 57) else 'psq_store'
            return f'{{ uint32_t ea_ = {ea}; {fn}(c, {rd}, ea_, {W}, {I});' + \
                   (f' {R(ra)} = ea_;' if upd else '') + ' }'
        if op == 59: return self.fpa(a, w, single=True)
        if op == 63: return self.op63(a, w)
        if op == 4:  return self.op4(a, w)
        return U()

    def fls(self, kind, r):
        if kind == 'lfs':  return f'{{ double v_ = (double)u2f(rd32(ea_)); {F(r)} = v_; {P(r)} = v_; }}'
        if kind == 'lfd':  return f'{F(r)} = u2d(rd64(ea_));'
        if kind == 'stfs': return f'wr32(ea_, f2u((float){F(r)}));'
        if kind == 'stfd': return f'wr64(ea_, d2u({F(r)}));'

    def op19(self, a, w):
        xo = (w >> 1) & 0x3FF
        rd, ra, rb = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        nxt = a + 4
        if xo == 16:  # bclr
            cond = bc_cond(w)
            pre = f'if ({cond}) ' if cond else ''
            if w & 1:
                return f'{{ uint32_t t_ = c->lr; c->lr = {nxt:#x}u; {pre}call_indirect(c, t_); }}'
            return pre + 'return;'
        if xo == 528:  # bcctr
            cond = bc_cond(w)
            pre = f'if ({cond}) ' if cond else ''
            if w & 1:
                return f'c->lr = {nxt:#x}u; {pre}call_indirect(c, c->ctr);'
            cases = sorted(t for t in jt_targets if self.inside(t))
            sw = ''
            if cases:
                sw = 'switch (c->ctr) { ' + ' '.join(f'case {t:#x}u: goto L_{t:08X};' for t in cases) + ' } '
            return pre + '{ ' + sw + 'call_indirect(c, c->ctr); return; }'
        if xo == 0:  # mcrf
            return f'cr_set(c, {rd >> 2}, (c->cr >> {(7 - (ra >> 2)) * 4}) & 0xF);'
        CROPS = {257: '{A} & {B}', 449: '{A} | {B}', 193: '{A} ^ {B}', 225: '!({A} & {B})', 33: '!({A} | {B})',
                 289: '!({A} ^ {B})', 129: '{A} & !{B}', 417: '{A} | !{B}'}
        if xo in CROPS:
            e = CROPS[xo].format(A=f'cr_bit(c, {ra})', B=f'cr_bit(c, {rb})')
            return f'cr_setbit(c, {rd}, {e});'
        if xo == 150: return ';'  # isync
        if xo == 50:  return f'ppc_rfi(c, {a:#x}u); return;'
        return f'ppc_unimpl(c, {a:#x}u, {w:#x}u);'

    def op31(self, a, w):
        rd, ra, rb = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        rec = w & 1
        xo = (w >> 1) & 0x3FF
        xo9 = xo & 0x1FF
        recd = lambda reg: f' REC(c, {R(reg)});' if rec else ''
        A, B, D, S = R(ra), R(rb), R(rd), R(rd)
        # XO-form arithmetic (OE bit ignored: overflow tracking not modelled)
        ARITH = {
            266: f'{D} = {A} + {B};',
            40:  f'{D} = {B} - {A};',
            104: f'{D} = 0u - {A};',
            235: f'{D} = (uint32_t)((int32_t){A} * (int32_t){B});',
            75:  f'{D} = (uint32_t)(((int64_t)(int32_t){A} * (int64_t)(int32_t){B}) >> 32);',
            11:  f'{D} = (uint32_t)(((uint64_t){A} * (uint64_t){B}) >> 32);',
            491: (f'{{ int32_t a_ = (int32_t){A}, b_ = (int32_t){B}; '
                  f'{D} = (b_ == 0 || (a_ == INT32_MIN && b_ == -1)) ? (a_ < 0 ? 0xFFFFFFFFu : 0u) : (uint32_t)(a_ / b_); }}'),
            459: f'{{ uint32_t b_ = {B}; {D} = b_ ? {A} / b_ : 0u; }}',
            10:  f'{{ uint32_t a_ = {A}, t_ = a_ + {B}; c->xer_ca = t_ < a_; {D} = t_; }}',
            8:   f'{{ uint32_t a_ = {A}, b_ = {B}; c->xer_ca = b_ >= a_; {D} = b_ - a_; }}',
            138: f'{{ uint64_t t_ = (uint64_t){A} + {B} + c->xer_ca; c->xer_ca = (uint32_t)(t_ >> 32); {D} = (uint32_t)t_; }}',
            136: f'{{ uint64_t t_ = (uint64_t)(uint32_t)~{A} + {B} + c->xer_ca; c->xer_ca = (uint32_t)(t_ >> 32); {D} = (uint32_t)t_; }}',
            202: f'{{ uint64_t t_ = (uint64_t){A} + c->xer_ca; c->xer_ca = (uint32_t)(t_ >> 32); {D} = (uint32_t)t_; }}',
            234: f'{{ uint64_t t_ = (uint64_t){A} + c->xer_ca + 0xFFFFFFFFu; c->xer_ca = (uint32_t)(t_ >> 32); {D} = (uint32_t)t_; }}',
            200: f'{{ uint64_t t_ = (uint64_t)(uint32_t)~{A} + c->xer_ca; c->xer_ca = (uint32_t)(t_ >> 32); {D} = (uint32_t)t_; }}',
            232: f'{{ uint64_t t_ = (uint64_t)(uint32_t)~{A} + c->xer_ca + 0xFFFFFFFFu; c->xer_ca = (uint32_t)(t_ >> 32); {D} = (uint32_t)t_; }}',
        }
        if xo9 in ARITH and xo9 not in (0,):
            # Make sure 10-bit XOs that collide with 9-bit arith codes are excluded.
            if xo in ARITH or (xo & 0x200 and xo9 in ARITH):
                return ARITH[xo9] + recd(rd)
        LOG = {28: f'{S} & {B}', 60: f'{S} & ~{B}', 444: f'{S} | {B}', 412: f'{S} | ~{B}',
               316: f'{S} ^ {B}', 124: f'~({S} | {B})', 476: f'~({S} & {B})', 284: f'~({S} ^ {B})'}
        if xo in LOG: return f'{A} = {LOG[xo]};' + recd(ra)
        if xo == 0:   return f'cmp_s(c, {rd >> 2}, (int32_t){A}, (int32_t){B});'
        if xo == 32:  return f'cmp_u(c, {rd >> 2}, {A}, {B});'
        if xo == 24:  return f'{{ uint32_t n_ = {B} & 63; {A} = n_ > 31 ? 0u : {S} << n_; }}' + recd(ra)
        if xo == 536: return f'{{ uint32_t n_ = {B} & 63; {A} = n_ > 31 ? 0u : {S} >> n_; }}' + recd(ra)
        if xo == 792:
            return (f'{{ int32_t s_ = (int32_t){S}; uint32_t n_ = {B} & 63; if (n_ > 31) {{ {A} = (uint32_t)(s_ >> 31); c->xer_ca = s_ < 0; }} '
                    f'else {{ {A} = (uint32_t)(s_ >> n_); c->xer_ca = s_ < 0 && n_ && (s_ & ((1u << n_) - 1)) != 0; }} }}') + recd(ra)
        if xo == 824:
            sh = rb
            ca = f's_ < 0 && (s_ & {(1 << sh) - 1:#x}) != 0' if sh else '0'
            return f'{{ int32_t s_ = (int32_t){S}; {A} = (uint32_t)(s_ >> {sh}); c->xer_ca = {ca}; }}' + recd(ra)
        if xo == 26:  return f'{A} = cntlz32({S});' + recd(ra)
        if xo == 922: return f'{A} = (uint32_t)(int32_t)(int16_t){S};' + recd(ra)
        if xo == 954: return f'{A} = (uint32_t)(int32_t)(int8_t){S};' + recd(ra)
        # ---- indexed loads/stores
        XL = {23: ('rd32', 0), 55: ('rd32', 1), 87: ('rd8', 0), 119: ('rd8', 1), 279: ('rd16', 0), 311: ('rd16', 1),
              343: ('lha', 0), 375: ('lha', 1), 20: ('rd32', 0)}
        XS = {151: ('wr32', 0, ''), 183: ('wr32', 1, ''), 215: ('wr8', 0, '(uint8_t)'), 247: ('wr8', 1, '(uint8_t)'),
              407: ('wr16', 0, '(uint16_t)'), 439: ('wr16', 1, '(uint16_t)')}
        if xo in XL:
            fn, upd = XL[xo]
            ea = f'({A} + {B})' if upd else ea_x(ra, rb)
            v = '(uint32_t)(int32_t)(int16_t)rd16(ea_)' if fn == 'lha' else f'{fn}(ea_)'
            return f'{{ uint32_t ea_ = {ea}; {D} = {v};' + (f' {A} = ea_;' if upd else '') + ' }'
        if xo in XS:
            fn, upd, cast = XS[xo]
            ea = f'({A} + {B})' if upd else ea_x(ra, rb)
            return f'{{ uint32_t ea_ = {ea}; {fn}(ea_, {cast}{S});' + (f' {A} = ea_;' if upd else '') + ' }'
        if xo == 150:  # stwcx.
            return f'{{ wr32({ea_x(ra, rb)}, {S}); cr_set(c, 0, 2 | (c->xer_so & 1)); }}'
        if xo == 534: return f'{D} = BSWAP32(rd32({ea_x(ra, rb)}));'
        if xo == 790: return f'{D} = BSWAP16(rd16({ea_x(ra, rb)}));'
        if xo == 662: return f'wr32({ea_x(ra, rb)}, BSWAP32({S}));'
        if xo == 918: return f'wr16({ea_x(ra, rb)}, BSWAP16((uint16_t){S}));'
        FX = {535: ('lfs', 0), 567: ('lfs', 1), 599: ('lfd', 0), 631: ('lfd', 1),
              663: ('stfs', 0), 695: ('stfs', 1), 727: ('stfd', 0), 759: ('stfd', 1)}
        if xo in FX:
            kind, upd = FX[xo]
            ea = f'({A} + {B})' if upd else ea_x(ra, rb)
            return f'{{ uint32_t ea_ = {ea}; ' + self.fls(kind, rd) + (f' {A} = ea_;' if upd else '') + ' }'
        if xo == 983: return f'wr32({ea_x(ra, rb)}, (uint32_t)d2u({F(rd)}));'
        if xo == 597: return f'ppc_lswi(c, {rd}, {RA0(ra)}, {rb or 32});'
        if xo == 725: return f'ppc_stswi(c, {rd}, {RA0(ra)}, {rb or 32});'
        # ---- system
        spr = ra | (rb << 5)
        if xo == 339:
            if spr == 8: return f'{D} = c->lr;'
            if spr == 9: return f'{D} = c->ctr;'
            if spr == 1: return f'{D} = (c->xer_so << 31) | (c->xer_ov << 30) | (c->xer_ca << 29);'
            return f'{D} = ppc_mfspr(c, {spr});'
        if xo == 467:
            if spr == 8: return f'c->lr = {S};'
            if spr == 9: return f'c->ctr = {S};'
            if spr == 1: return f'{{ uint32_t x_ = {S}; c->xer_so = x_ >> 31; c->xer_ov = (x_ >> 30) & 1; c->xer_ca = (x_ >> 29) & 1; }}'
            return f'ppc_mtspr(c, {spr}, {S});'
        if xo == 371: return f'{D} = ppc_mftb(c, {1 if spr == 269 else 0});'
        if xo == 19:  return f'{D} = c->cr;'
        if xo == 144:
            crm = (w >> 12) & 0xFF
            m = 0
            for i in range(8):
                if crm & (0x80 >> i): m |= 0xF << ((7 - i) * 4)
            return f'c->cr = (c->cr & {(~m) & 0xFFFFFFFF:#x}u) | ({S} & {m:#x}u);'
        if xo == 83:  return f'{D} = c->msr;'
        if xo == 146: return f'c->msr = {S}; if (g_irq_pending) irq_check(c);'
        if xo == 512:
            return (f'cr_set(c, {rd >> 2}, (c->xer_so << 3) | (c->xer_ov << 2) | (c->xer_ca << 1)); '
                    f'c->xer_so = c->xer_ov = c->xer_ca = 0;')
        if xo == 4:   return f'/* tw */ if ({rd} == 31) ppc_trap(c, {a:#x}u);'
        if xo == 1014: return f'ppc_dcbz({ea_x(ra, rb)});'
        if xo in (54, 86, 470, 246, 278, 982, 598, 854, 306, 566): return ';'  # cache/sync no-ops
        if xo == 210 or xo == 242: return ';'   # mtsr / mtsrin
        if xo == 595 or xo == 659: return f'{D} = 0;'  # mfsr / mfsrin
        return f'ppc_unimpl(c, {a:#x}u, {w:#x}u);'

    def fpa(self, a, w, single):
        d, fa, fb, fc = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
        xo = (w >> 1) & 31
        A, B, C = F(fa), F(fb), F(fc)
        E = {18: f'{A} / {B}', 20: f'{A} - {B}', 21: f'{A} + {B}', 22: f'sqrt({B})',
             25: f'{A} * {C}', 28: f'{A} * {C} - {B}', 29: f'{A} * {C} + {B}',
             30: f'-({A} * {C} - {B})', 31: f'-({A} * {C} + {B})',
             24: f'1.0 / {B}', 26: f'1.0 / sqrt({B})', 23: f'({A} >= 0.0 ? {C} : {B})'}
        if xo not in E: return f'ppc_unimpl(c, {a:#x}u, {w:#x}u);'
        if single:
            s = f'{{ double v_ = rnd_s({E[xo]}); {F(d)} = v_; {P(d)} = v_; }}'
        else:
            s = f'{F(d)} = {E[xo]};'
        return s + (' cr_set(c, 1, c->fpscr >> 28);' if w & 1 else '')

    def op63(self, a, w):
        d, fa, fb = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        xo = (w >> 1) & 0x3FF
        if (xo & 31) in (18, 20, 21, 22, 23, 25, 26, 28, 29, 30, 31) and (xo & 31) >= 18:
            return self.fpa(a, w, single=False)
        rc = ' cr_set(c, 1, c->fpscr >> 28);' if w & 1 else ''
        if xo in (0, 32): return f'cmp_f(c, {d >> 2}, {F(fa)}, {F(fb)});'
        if xo == 12: return f'{{ double v_ = rnd_s({F(fb)}); {F(d)} = v_; {P(d)} = v_; }}' + rc
        if xo == 14: return f'{F(d)} = ppc_fctiw(c, {F(fb)}, 0);' + rc
        if xo == 15: return f'{F(d)} = ppc_fctiw(c, {F(fb)}, 1);' + rc
        if xo == 40: return f'{F(d)} = -{F(fb)};' + rc
        if xo == 72: return f'{F(d)} = {F(fb)};' + rc
        if xo == 136: return f'{F(d)} = -fabs({F(fb)});' + rc
        if xo == 264: return f'{F(d)} = fabs({F(fb)});' + rc
        if xo == 583: return f'{F(d)} = u2d(0xFFF8000000000000ull | c->fpscr);' + rc
        if xo == 711:
            fm = (w >> 17) & 0xFF
            m = 0
            for i in range(8):
                if fm & (0x80 >> i): m |= 0xF << ((7 - i) * 4)
            return f'c->fpscr = (c->fpscr & {(~m) & 0xFFFFFFFF:#x}u) | ((uint32_t)d2u({F(fb)}) & {m:#x}u);' + rc
        if xo == 134:
            crf, im = d >> 2, (w >> 12) & 0xF
            sh = (7 - crf) * 4
            return f'c->fpscr = (c->fpscr & ~{0xF << sh:#x}u) | {im << sh:#x}u;' + rc
        if xo == 38: return f'c->fpscr |= {1 << (31 - d):#x}u;' + rc
        if xo == 70: return f'c->fpscr &= ~{1 << (31 - d):#x}u;' + rc
        if xo == 64: return f'cr_set(c, {d >> 2}, (c->fpscr >> {(7 - (fa >> 2)) * 4}) & 0xF);'
        return f'ppc_unimpl(c, {a:#x}u, {w:#x}u);'

    def op4(self, a, w):
        d, fa, fb, fc = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
        xo10, xo5, xo6 = (w >> 1) & 0x3FF, (w >> 1) & 31, (w >> 1) & 0x3F
        a0, a1, b0, b1, c0, c1 = F(fa), P(fa), F(fb), P(fb), F(fc), P(fc)
        def pair(e0, e1, rnd=True):
            r = (lambda e: f'rnd_s({e})') if rnd else (lambda e: e)
            return f'{{ double x_ = {r(e0)}, y_ = {r(e1)}; {F(d)} = x_; {P(d)} = y_; }}'
        if xo6 in (6, 7, 38, 39):  # psq_lx / psq_stx / psq_lux / psq_stux
            W, I = (w >> 10) & 1, (w >> 7) & 7
            upd = xo6 in (38, 39)
            ea = f'({R(fa)} + {R(fb)})' if upd else ea_x(fa, fb)
            fn = 'psq_load' if xo6 in (6, 38) else 'psq_store'
            return f'{{ uint32_t ea_ = {ea}; {fn}(c, {d}, ea_, {W}, {I});' + (f' {R(fa)} = ea_;' if upd else '') + ' }'
        A5 = {18: (f'{a0} / {b0}', f'{a1} / {b1}'), 20: (f'{a0} - {b0}', f'{a1} - {b1}'),
              21: (f'{a0} + {b0}', f'{a1} + {b1}'), 25: (f'{a0} * {c0}', f'{a1} * {c1}'),
              28: (f'{a0} * {c0} - {b0}', f'{a1} * {c1} - {b1}'), 29: (f'{a0} * {c0} + {b0}', f'{a1} * {c1} + {b1}'),
              30: (f'-({a0} * {c0} - {b0})', f'-({a1} * {c1} - {b1})'), 31: (f'-({a0} * {c0} + {b0})', f'-({a1} * {c1} + {b1})'),
              24: (f'1.0 / {b0}', f'1.0 / {b1}'), 26: (f'1.0 / sqrt({b0})', f'1.0 / sqrt({b1})'),
              10: (f'{a0} + {b1}', c1), 11: (c0, f'{a0} + {b1}'),
              12: (f'{a0} * {c0}', f'{a1} * {c0}'), 13: (f'{a0} * {c1}', f'{a1} * {c1}'),
              14: (f'{a0} * {c0} + {b0}', f'{a1} * {c0} + {b1}'), 15: (f'{a0} * {c1} + {b0}', f'{a1} * {c1} + {b1}')}
        if xo5 == 23:
            return pair(f'({a0} >= 0.0 ? {c0} : {b0})', f'({a1} >= 0.0 ? {c1} : {b1})', rnd=False)
        if xo5 in A5:
            return pair(*A5[xo5])
        if xo10 == 0 or xo10 == 32:  return f'cmp_f(c, {d >> 2}, {a0}, {b0});'
        if xo10 == 64 or xo10 == 96: return f'cmp_f(c, {d >> 2}, {a1}, {b1});'
        if xo10 == 40:  return pair(f'-{b0}', f'-{b1}', rnd=False)
        if xo10 == 72:  return pair(b0, b1, rnd=False)
        if xo10 == 136: return pair(f'-fabs({b0})', f'-fabs({b1})', rnd=False)
        if xo10 == 264: return pair(f'fabs({b0})', f'fabs({b1})', rnd=False)
        if xo10 == 528: return pair(a0, b0, rnd=False)
        if xo10 == 560: return pair(a0, b1, rnd=False)
        if xo10 == 592: return pair(a1, b0, rnd=False)
        if xo10 == 624: return pair(a1, b1, rnd=False)
        if xo10 == 1014: return f'ppc_dcbz_l({ea_x(fa, fb)});'
        return f'ppc_unimpl(c, {a:#x}u, {w:#x}u);'

# ---------------------------------------------------------------- discovery
def is_terminal(w):
    op = w >> 26
    if op == 18 and not w & 1: return True
    if op == 19 and ((w >> 1) & 0x3FF) in (16, 528) and not w & 1 and ((w >> 21) & 0x14) == 0x14: return True
    if op == 19 and ((w >> 1) & 0x3FF) == 50: return True
    return False

entries = set(fstarts)
work = list(fstarts)
regions = {}
while work:
    e = work.pop()
    if e in regions: continue
    end = region_end(e)
    regions[e] = end
    labels = set()
    last = None
    for a in range(e, end, 4):
        w = word(a); last = w
        bt = branch_targets(w, a)
        if bt:
            t, link, _ = bt
            if link and t != a + 4:
                if in_text(t) and t not in entries: entries.add(t); work.append(t)
            elif not link:
                if e <= t < end: labels.add(t)
                elif in_text(t) and t not in entries: entries.add(t); work.append(t)
    if last is not None and not is_terminal(last) and in_text(end) and end not in entries:
        entries.add(end); work.append(end)

print(f'dtk functions {len(fstarts)}, total entry points {len(entries)}, overrides {len(overrides)}')

# ---------------------------------------------------------------- emit
os.makedirs(OUT, exist_ok=True)
def cname(e): return f'orig_{e:08X}' if e in overrides else f'fn_{e:08X}'
with Out(os.path.join(OUT, 'funcs.h')) as h:
    h.write('#pragma once\n#include "ppc.h"\n')
    for e in sorted(entries):
        h.write(f'void fn_{e:08X}(PPCContext *c);\n')
        if e in overrides: h.write(f'void orig_{e:08X}(PPCContext *c);\n')
with Out(os.path.join(OUT, 'symbols.h')) as h:
    h.write('#pragma once\n// SYM_<name> -> recompiled function, for HLE code that refers to symbols by name.\n')
    for e in sorted(entries):
        nm = names.get(e)
        if nm and re.fullmatch(r'[A-Za-z_]\w*', nm) and not nm.startswith(('fn_', 'lbl_')):
            h.write(f'#define SYM_{nm} fn_{e:08X}\n')
with Out(os.path.join(OUT, 'func_table.c')) as t:
    t.write('#include "funcs.h"\n#include "func_table.h"\n')
    t.write('const FuncEntry g_func_table[] = {\n')
    for e in sorted(entries):
        t.write(f'  {{ {e:#010x}u, fn_{e:08X}, "{names.get(e, "")}" }},\n')
    t.write('};\nconst unsigned g_func_count = sizeof(g_func_table) / sizeof(g_func_table[0]);\n')

order = sorted(entries)
CHUNK = 6000
files, cur, n, nins = [], [], 0, 0
for e in order:
    cur.append(e); n += (regions[e] - e) // 4
    if n >= CHUNK: files.append(cur); cur, n = [], 0
if cur: files.append(cur)
unimpl = 0
for fi, group in enumerate(files):
    with Out(os.path.join(OUT, f'recomp_{fi:03d}.c')) as o:
        o.write('#include "funcs.h"\n#if defined(_MSC_VER)\n#pragma warning(disable: 4102 4146 4244)\n#endif\n\n')
        for e in group:
            end = regions[e]
            labels = set(t for t in jt_targets if e <= t < end)
            for a in range(e, end, 4):
                bt = branch_targets(word(a), a)
                if bt and not bt[1] and e <= bt[0] < end: labels.add(bt[0])
            g = Gen(e, end, labels)
            nm = names.get(e)
            o.write(f'// {nm}\n' if nm else '')
            o.write(f'void {cname(e)}(PPCContext *c) {{\n')
            last = None
            for a in range(e, end, 4):
                w = word(a); last = w
                if a in labels: o.write(f'L_{a:08X}:\n')
                s = g.insn(a, w)
                if 'ppc_unimpl' in s: unimpl += 1
                o.write(f'  {s}\n'); nins += 1
            if last is not None and not is_terminal(last):
                o.write(f'  fn_{end:08X}(c);\n' if in_text(end) else f'  ppc_bad_jump(c, {end - 4:#x}u, {end:#x}u);\n')
            o.write('}\n\n')
print(f'wrote {len(files)} C files, {nins} instructions, {unimpl} unimplemented')

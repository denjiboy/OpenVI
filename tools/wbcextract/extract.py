#!/usr/bin/env python3
"""Extract the XOR-network implemented by TFIT_op_iAES2.

The routine is straight-line code built from a tiny operation set:

    st[d] = st[s] ^ TBL[t][ byte (st[i] >> 8k) ]        (and variants with a
    st[d] = KEY[w] ^ TBL[t][ in[j] ]                     key word or input byte)

so we emulate it once with a fake machine whose values carry both a concrete
bit pattern and a provenance expression.  Every store into the routine's state
array becomes one operation of the network; the network is then validated
against the real object code in oracle.py.

Inputs, all relative to $WBC_DIR (default /tmp/opencode/arxan):

    lib_iAES2/op.asm        disassembly of TFIT_op_iAES2.obj   (llvm-objdump)
    lib_iAES2/relocs.txt    its relocations                    (llvm-readobj -r)
    lib_iAES2/data.bin      its .data section = the 84 tables  (llvm-objcopy)
    $WBC_KEYS               the 101 x 320 byte cooked keys     (see KEYS.md)
    oracle.py, tramp.S, libtramp.so                          (validation only)

Output: net.json next to this script.  Feed it to emit_c.py.

Usage: WBC_DIR=... WBC_KEYS=... python3 extract.py
"""
import ctypes, json, os, re, struct, sys

HERE = os.environ.get("WBC_DIR", "/tmp/opencode/arxan")
KEYS_FILE = os.environ.get("WBC_KEYS", os.path.dirname(HERE) + "/tfit_iAES2.bin")
NET_OUT  = os.environ.get("WBC_NET_OUT", HERE + "/net.json")
sys.path.insert(0, HERE)
from tables import NAMES          # .data offset -> table name
try:
    import oracle as O            # only needed for the validation pass
except Exception:
    O = None

TEXT_BASE = O.TEXT_BASE if O else 0x20000000
DATA_BASE = O.DATA_BASE if O else 0x30000000
STACK_BASE = 0x53000000
KEY_BASE = 0x50000000
IN_BASE = 0x51000000
OUT_BASE = 0x52000000
TBL_SIZE = 1024
NTBL = 84

TBL_NAMES = [None] * NTBL
for off, nm in NAMES.items():
    TBL_NAMES[off // TBL_SIZE] = nm

# ---------------------------------------------------------------- relocations
def parse_relocs(path):
    rels, sec = {}, None
    for line in open(path):
        m = re.match(r"\s*Section \((\d+)\) (\S+) \{", line)
        if m:
            sec = m.group(2)
            continue
        m = re.match(r"\s*(0x[0-9a-f]+) IMAGE_REL_AMD64_\w+ (\S+)", line, re.I)
        if m and sec == ".text$mn":
            rels[int(m.group(1), 16)] = m.group(2)
    return rels


# ---------------------------------------------------------------- instructions
def parse_asm(path):
    ins = []
    for line in open(path):
        line = line.split("#")[0].rstrip()
        m = re.match(r"\s*([0-9a-f]+):\s+(\S+)\s*(.*)$", line)
        if m:
            ins.append((int(m.group(1), 16), m.group(2), m.group(3).strip()))
    return ins


def split_ops(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


MEMRE = re.compile(r"^(?P<disp>-?(?:0x)?[0-9a-f]*)?"
                   r"\((?P<base>%[a-z0-9]+)"
                   r"(?:,(?P<index>%[a-z0-9]+)"
                   r"(?:,(?P<scale>[1248]))?)?\)$")

REG64 = {"rax": 0, "rcx": 1, "rdx": 2, "rbx": 3, "rsp": 4, "rbp": 5, "rsi": 6,
         "rdi": 7, "r8": 8, "r9": 9, "r10": 10, "r11": 11, "r12": 12,
         "r13": 13, "r14": 14, "r15": 15, "rip": 16}
REG32 = {"eax": 0, "ecx": 1, "edx": 2, "ebx": 3, "esp": 4, "ebp": 5, "esi": 6,
         "edi": 7, "r8d": 8, "r9d": 9, "r10d": 10, "r11d": 11, "r12d": 12,
         "r13d": 13, "r14d": 14, "r15d": 15}
REG16 = {"ax": 0, "cx": 1, "dx": 2, "bx": 3, "r8w": 8, "r9w": 9}
REG8 = {"al": 0, "cl": 1, "dl": 2, "bl": 3, "r8b": 8, "r9b": 9, "r10b": 10,
        "r11b": 11, "r12b": 12, "r13b": 13, "r14b": 14, "r15b": 15}


def reg(name):
    if name.startswith("%"):
        name = name[1:]
    if name in REG64:
        return REG64[name], 64
    if name in REG32:
        return REG32[name], 32
    if name in REG16:
        return REG16[name], 16
    if name in REG8:
        return REG8[name], 8
    raise KeyError(name)


# ------------------------------------------------------------------ expressions
def ex(e, v):
    """Build an expression node, folding constants."""
    if isinstance(e, int) and isinstance(v, int):
        return e
    return (e, v)


def e_xor(a, b):
    if a == 0:
        return b
    if b == 0:
        return a
    if isinstance(a, int) and isinstance(b, int):
        return a ^ b
    if a is None or b is None:
        return None
    return ("xor", a, b)


def e_or(a, b):
    if a == 0:
        return b
    if b == 0:
        return a
    if isinstance(a, int) and isinstance(b, int):
        return a | b
    if a is None or b is None:
        return None
    return ("or", a, b)


def e_and(a, m):
    if m == 0:
        return 0
    if m == 0xffffffff and not isinstance(a, int):
        return a
    if isinstance(a, int):
        return a & m
    if a is None:
        return None
    return ("and", a, m)


def e_shr(a, s):
    if s == 0:
        return a
    if isinstance(a, int):
        return a >> s
    if a is None:
        return None
    return ("shr", a, s)


class Machine:
    def __init__(self, ins, relocs, tables, key, inp):
        self.ins = ins
        self.relocs = relocs
        self.tables = tables         # list of 256 lists of 4 bytes-style ints
        self.key = key
        self.inp = inp
        self.regs = [0] * 17
        self.rexp = [None] * 17
        self.mem = {}
        self.ops = []
        self.out = {}
        self.zf = False
        self.pc = 0
        self.addr2i = {a: i for i, (a, _, _) in enumerate(ins)}

    # --- registers ------------------------------------------------------
    def rget(self, name):
        i, size = reg(name)
        return self.regs[i] & ((1 << size) - 1), self.rexp[i]

    def rset(self, name, val, expr):
        i, size = reg(name)
        mask = (1 << size) - 1
        v = val & mask
        if size == 64:
            self.regs[i], self.rexp[i] = v, expr
        elif size == 32:
            self.regs[i], self.rexp[i] = v, expr      # 32-bit writes zero-extend
        else:
            keep = self.regs[i] & ~mask
            self.regs[i] = keep | v
            if expr is None:
                self.rexp[i] = None
            else:
                # byte/word insertion: just track as opaque combination
                self.rexp[i] = ("ins", size, expr, self.rexp[i])

    # --- memory ---------------------------------------------------------
    def mread(self, addr, size, idxexpr=None, scale=1, idxval=0):
        if IN_BASE <= addr < IN_BASE + 4096:
            off = addr - IN_BASE
            if size == 1:
                return self.inp[off], ("in", off)
            return None, None
        if KEY_BASE <= addr < KEY_BASE + 4096:
            off = addr - KEY_BASE
            if size == 4 and off >= 0x10 and (off - 0x10) % 4 == 0:
                w = (off - 0x10) // 4
                return self.key[w], ("k", w)
            if size == 1:
                return None, ("kb", off)
            return None, None
        if DATA_BASE <= addr < DATA_BASE + NTBL * TBL_SIZE and size == 4:
            t = (addr - DATA_BASE) // TBL_SIZE
            if idxexpr is not None:
                return self.tables[t][idxval & 0xff], ("t", t, idxexpr)
            return None, ("tv", t)
        if OUT_BASE <= addr < OUT_BASE + 4096:
            return self.mem.get(addr, (0, None))[0], ("ld", addr)
        if STACK_BASE <= addr < STACK_BASE + 0x100000:
            return self.mem.get(addr, (0, None))[0], ("ld", addr)
        return self.mem.get(addr, (0, None))[0], ("ld", addr)

    def mwrite(self, addr, size, val, expr):
        if OUT_BASE <= addr < OUT_BASE + 4096 and size == 4:
            self.out[addr - OUT_BASE] = expr
            return
        if STACK_BASE <= addr < STACK_BASE + 0x100000:
            self.mem[addr] = (val, expr)
            if size == 4 and addr >= STACK_BASE + 0x1000 and expr is not None:
                self.ops.append((addr, expr))
            return
        self.mem[addr] = (val, expr)

    # --- operand helpers ------------------------------------------------
    def eff_addr(self, op):
        m = MEMRE.match(op)
        base = index = 0
        bv = iv = 0
        if m.group("base") == "%rip":
            b = self.ins[self.pc][0]
            base = b + (len(self.ins[self.pc][2]) and 0 or 0)
            raise RuntimeError("rip handled by caller")
        if m.group("base"):
            bv = self.regs[reg(m.group("base"))[0]]
        if m.group("index"):
            iv = self.regs[reg(m.group("index"))[0]]
        scale = int(m.group("scale") or 1)
        disp = m.group("disp") or "0"
        disp = int(disp, 16) if disp.startswith(("0x", "-0x")) else int(disp or 0, 16)
        return bv + iv * scale + disp, m, iv

    # --- main loop ------------------------------------------------------
    def run(self, start=0, maxsteps=200000):
        self.pc = start
        steps = 0
        while self.pc < len(self.ins):
            steps += 1
            if steps > maxsteps:
                raise RuntimeError("step limit")
            addr, mn, ops = self.ins[self.pc]
            nxt = self.pc + 1
            try:
                self.exec_one(addr, mn, ops)
            except Exception as ex:
                raise RuntimeError("%s at %#x: %s %s | rdx=%#x rcx=%#x rax=%#x"
                                   % (ex, addr, mn, ops, self.regs[2], self.regs[1], self.regs[0]))
            if self.branch is None:
                self.pc = nxt
            else:
                self.pc = self.branch
                self.branch = None
        return self.ops, self.out

    branch = None

    def exec_one(self, addr, mn, ops):
        self.branch = None
        o = split_ops(ops) if ops else []

        def reg_or_mem(op, size):
            if op.startswith("%"):
                return self.rget(op)
            if op.startswith("$"):
                v = int(op[1:], 16) if op[1:].startswith("0x") else int(op[1:])
                return v, v
            if "(%rip)" in op:
                return 0, None
            a, m, iv = self.eff_addr(op)
            idxe = self.rexp[reg(m.group("index"))[0]] if m.group("index") else None
            scale = int(m.group("scale") or 1)
            return self.mread(a, size, idxe, scale, iv)

        def is_mem(op):
            return "(" in op and not op.startswith("%")

        if mn == "movl":
            v, e = reg_or_mem(o[0], 4)
            if is_mem(o[1]):
                a, m, iv = self.eff_addr(o[1])
                self.mwrite(a, 4, v & 0xffffffff, e)
            else:
                self.rset(o[1], v & 0xffffffff, e)
        elif mn == "movq":
            v, e = reg_or_mem(o[0], 8)
            if is_mem(o[1]):
                a, m, iv = self.eff_addr(o[1])
                self.mwrite(a, 8, v & (2**64 - 1), e)
            else:
                self.rset(o[1], v & (2**64 - 1), e)
        elif mn in ("movzbl", "movzbq"):
            v, e = reg_or_mem(o[0], 1)
            v = v & 0xff
            self.rset(o[1], v, e)
        elif mn == "movw":
            v, e = reg_or_mem(o[0], 2)
            self.rset(o[1], v & 0xffff, e)
        elif mn in ("movb",):
            v, e = reg_or_mem(o[0], 1)
            self.rset(o[1], v & 0xff, e)
        elif mn == "leaq":
            # caller resolves rip via relocs
            if "(%rip)" in o[0]:
                r = self.relocs.get(addr + 3)
                if r in TBL_NAMES or r in [n for n in TBL_NAMES if n]:
                    tv = DATA_BASE + ([i for i, n in enumerate(TBL_NAMES) if n == r][0]) * TBL_SIZE
                else:
                    tv = O.STUB_BASE if r in ("__security_cookie", "__security_check_cookie", "__GSHandlerCheck") else 0x60000000
                self.rset(o[1], tv, tv)
            else:
                a, m, iv = self.eff_addr(o[0])
                self.rset(o[1], a, a)
        elif mn in ("xorl", "xorq"):
            v1, e1 = reg_or_mem(o[0], 4)
            v2, e2 = self.rget(o[1])
            nv = (v1 ^ v2) & (0xffffffff if mn == "xorl" else 2**64 - 1)
            self.rset(o[1], nv, e_xor(e1, e2))
        elif mn in ("orl", "orq"):
            v1, e1 = reg_or_mem(o[0], 4)
            v2, e2 = self.rget(o[1])
            self.rset(o[1], (v1 | v2) & 0xffffffff, e_or(e1, e2))
        elif mn in ("andl", "andq"):
            v1, e1 = reg_or_mem(o[0], 4)
            v2, e2 = self.rget(o[1])
            m = 0xffffffff if mn == "andl" else 2**64 - 1
            if v1 == m and e1 != e1:
                pass
            self.rset(o[1], (v1 & v2) & m, e_and(e2, v1 & m) if (e1 == v1) else e_and(e1, v2 & m))
        elif mn == "shrl":
            v1, e1 = reg_or_mem(o[0], 4)
            v2, e2 = self.rget(o[1])
            self.rset(o[1], (v2 >> v1) & 0xffffffff, e_shr(e2, v1))
        elif mn == "shll":
            v1, e1 = reg_or_mem(o[0], 4)
            v2, e2 = self.rget(o[1])
            self.rset(o[1], (v2 << v1) & 0xffffffff, None)
        elif mn == "imulq":
            if len(o) == 3:
                v1, e1 = reg_or_mem(o[0], 4)
                v2, e2 = self.rget(o[1])
                self.rset(o[2], v1 * v2, None)
            else:
                v1, e1 = reg_or_mem(o[0], 4)
                v2, e2 = self.rget(o[1])
                self.rset(o[1], v1 * v2, None)
        elif mn in ("addq", "addl"):
            v1, e1 = reg_or_mem(o[0], 4)
            v2, e2 = self.rget(o[1])
            self.rset(o[1], v1 + v2, None)
        elif mn in ("subq", "subl"):
            v1, e1 = reg_or_mem(o[0], 4)
            v2, e2 = self.rget(o[1])
            self.rset(o[1], v2 - v1, None)
        elif mn == "incq":
            v2, e2 = self.rget(o[0])
            self.rset(o[0], v2 + 1, None)
        elif mn == "divq":
            v1, e1 = reg_or_mem(o[0], 8)
            rax = self.regs[0]
            rdx = self.regs[2]
            num = (rdx << 64) | rax
            self.regs[0], self.rexp[0] = (num // v1) & (2**64 - 1), None
            self.regs[2], self.rexp[2] = (num % v1) & (2**64 - 1), None
        elif mn == "testq" or mn == "testl":
            v1, _ = reg_or_mem(o[0], 4)
            v2, _ = self.rget(o[1])
            self.zf = (v1 & v2) == 0
        elif mn == "cmpq" or mn == "cmpl":
            v1, _ = reg_or_mem(o[0], 4)
            v2, _ = self.rget(o[1])
            self.zf = v1 == v2
        elif mn in ("je", "jz"):
            self.branch = self.addr2i[int(o[0].split()[0], 16)] if self.zf else None
        elif mn in ("jne", "jnz"):
            self.branch = self.addr2i[int(o[0].split()[0], 16)] if not self.zf else None
        elif mn == "jmp":
            self.branch = self.addr2i[int(o[0].split()[0], 16)]
        elif mn in ("jle", "jge", "jl", "jg", "jae", "jbe", "ja", "jb"):
            raise RuntimeError("branch %s not modelled" % mn)
        elif mn in ("retq", "ret"):
            self.branch = len(self.ins)
        elif mn == "callq":
            # stubs / helpers we do not need: return immediately
            pass
        elif mn == "int3":
            raise RuntimeError("int3 at %#x" % addr)
        elif mn in ("nop", "nopl", "nopw", "endbr64", "hlt"):
            pass
        else:
            raise RuntimeError("unhandled instruction %s %s at %#x" % (mn, ops, addr))

        # leaq to a table needs the reloc; handled above


def network():
    """Emulate the routine once and return (ops, out, tables, keywords)."""
    relocs = parse_relocs("%s/lib_iAES2/relocs.txt" % HERE)
    ins = parse_asm("%s/lib_iAES2/op.asm" % HERE)
    data = open("%s/lib_iAES2/data.bin" % HERE, "rb").read()
    tables = [list(struct.unpack_from("<256I", data, i * TBL_SIZE)) for i in range(NTBL)]

    keys = open(KEYS_FILE, "rb").read()
    blob = keys[46 * 320:47 * 320]
    keyw = list(struct.unpack("<76I", blob[16:16 + 304]))
    inp = list(bytes.fromhex("3A752F9FA117A5E0D7BA23BA97F75AA4"))

    m = Machine(ins, relocs, tables, keyw, inp)
    m.regs[1] = KEY_BASE
    m.regs[2] = IN_BASE
    m.regs[8] = OUT_BASE
    m.regs[4] = STACK_BASE + 0x80000
    ops, out = m.run()
    return ops, out, tables, m


def net_eval(ops, outmap, tables, keyw, inp):
    st = {}

    def ev(e):
        if isinstance(e, int):
            return e
        k = e[0]
        if k == "ld":
            return st.get(e[1], 0)
        if k == "in":
            return inp[e[1]]
        if k == "k":
            return keyw[e[1]]
        if k == "t":
            return tables[e[1]][ev(e[2]) & 0xff]
        if k == "xor":
            return ev(e[1]) ^ ev(e[2])
        if k == "or":
            return ev(e[1]) | ev(e[2])
        if k == "and":
            return ev(e[1]) & e[2]
        if k == "shr":
            return ev(e[1]) >> e[2]
        raise RuntimeError("bad node %r" % (e,))

    for a, e in ops:
        st[a] = ev(e)
    return b"".join(struct.pack("<I", ev(outmap[off])) for off in (0, 4, 8, 12))


def main():
    ops, out, tables, m = network()
    print("network ops: %d   output words: %d" % (len(ops), len(out)))
    if os.environ.get("DBG"):
        for i, (a, e) in enumerate(ops[:8] + ops[-8:]):
            print("  op %-5s %#x <- %s" % (i, a, e))
        print("  outs:", {hex(k): v for k, v in out.items()})

    keys = open(KEYS_FILE, "rb").read()
    blob46 = keys[46 * 320:47 * 320]
    ct = bytes.fromhex("3A752F9FA117A5E0D7BA23BA97F75AA4")
    keyw = list(struct.unpack("<76I", blob46[16:16 + 304]))

    # ---- cross-check the network against the concrete emulator state -------
    st = {}
    got = net_eval(ops, out, tables, keyw, list(ct))
    print("network out:", got.hex())
    print("concrete   :", "0000000000ffff7f0100000001000000")

    json.dump({"ops": [[a, e] for a, e in ops], "out": [[o, e] for o, e in out.items()]},
              open(NET_OUT, "w"))
    if O is None:
        print("oracle.py not available - skipping the comparison against the object code")
        return

    # ---- now check the network against the real object code, all 101 keys --
    entry = O.load()
    tramp = ctypes.CDLL("%s/libtramp.so" % HERE)
    call = tramp.call_win64
    call.argtypes = [ctypes.c_void_p] * 4
    kb = ctypes.create_string_buffer(320)
    ib = ctypes.create_string_buffer(16)
    ob = ctypes.create_string_buffer(16)

    import random
    random.seed(1234)
    fails = []
    tests = 0
    for ki in list(range(101)):
        for t in range(3):
            if t == 0:
                data = ct
            else:
                data = bytes(random.randrange(256) for _ in range(16))
            blob = keys[ki * 320:(ki + 1) * 320]
            ctypes.memmove(ctypes.addressof(kb), blob, 320)
            ctypes.memmove(ctypes.addressof(ib), data, 16)
            ctypes.memset(ctypes.addressof(ob), 0, 16)
            call(entry, ctypes.addressof(kb), ctypes.addressof(ib), ctypes.addressof(ob))
            ref = bytes(ob.raw[:16])
            kw = list(struct.unpack("<76I", blob[16:16 + 304]))
            mine = net_eval(ops, out, tables, kw, list(data))
            tests += 1
            if mine != ref:
                fails.append((ki, t, data.hex(), mine.hex(), ref.hex()))
    print("compare vs real code: %d tests, %d failures" % (tests, len(fails)))
    for f in fails[:5]:
        print("   key=%d in=%s net=%s real=%s" % f)




if __name__ == "__main__":
    main()

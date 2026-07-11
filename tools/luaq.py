"""Parser/disassembler for Naughty Bear's dumped Lua chunks (lua_dump/).

Format: Lua 5.1 bytecode, header \x1bLuaQ, LITTLE-endian throughout (the
PC-authored chunks are byte-swapped by the console runtime at load). Standard
5.1 layout except constants use the LNUM patch: tag 0xFE = int64 LE, next to
3 = double and 4 = string. Debug info is mostly stripped.

Usage:
  python tools/luaq.py <chunk> [filter]   # disassemble; optional case-
                                          # insensitive substring filter keeps
                                          # only functions that mention it in
                                          # constants (with full listing).
"""
import struct
import sys

OPCODES = [
    'MOVE', 'LOADK', 'LOADBOOL', 'LOADNIL', 'GETUPVAL', 'GETGLOBAL',
    'GETTABLE', 'SETGLOBAL', 'SETUPVAL', 'SETTABLE', 'NEWTABLE', 'SELF',
    'ADD', 'SUB', 'MUL', 'DIV', 'MOD', 'POW', 'UNM', 'NOT', 'LEN', 'CONCAT',
    'JMP', 'EQ', 'LT', 'LE', 'TEST', 'TESTSET', 'CALL', 'TAILCALL', 'RETURN',
    'FORLOOP', 'FORPREP', 'TFORLOOP', 'SETLIST', 'CLOSE', 'CLOSURE', 'VARARG',
]
# Opcodes whose B or C operand is a constant index when the RK bit is set.
RK_OPS = {'GETTABLE': 'C', 'SETTABLE': 'BC', 'SELF': 'C', 'ADD': 'BC',
          'SUB': 'BC', 'MUL': 'BC', 'DIV': 'BC', 'MOD': 'BC', 'POW': 'BC',
          'EQ': 'BC', 'LT': 'BC', 'LE': 'BC'}


class Reader:
    def __init__(self, data):
        self.d = data
        self.o = 0

    def u8(self):
        v = self.d[self.o]
        self.o += 1
        return v

    def u32(self):
        v = struct.unpack_from('<I', self.d, self.o)[0]
        self.o += 4
        return v

    def i64(self):
        v = struct.unpack_from('<q', self.d, self.o)[0]
        self.o += 8
        return v

    def f64(self):
        v = struct.unpack_from('<d', self.d, self.o)[0]
        self.o += 8
        return v

    def string(self):
        n = self.u32()
        if n == 0:
            return None
        s = self.d[self.o:self.o + n - 1].decode('latin-1')  # strip NUL
        self.o += n
        return s


class Proto:
    def __init__(self, r, name):
        self.source = r.string()
        self.linedefined = r.u32()
        self.lastlinedefined = r.u32()
        self.nups = r.u8()
        self.numparams = r.u8()
        self.is_vararg = r.u8()
        self.maxstack = r.u8()
        self.code = [r.u32() for _ in range(r.u32())]
        self.consts = []
        for _ in range(r.u32()):
            t = r.u8()
            if t == 0:
                self.consts.append(None)
            elif t == 1:
                self.consts.append(bool(r.u8()))
            elif t == 3:
                self.consts.append(r.f64())
            elif t == 4:
                self.consts.append(r.string())
            elif t == 0xFE:
                self.consts.append(r.i64())
            else:
                raise ValueError(f'unknown const tag {t:#x} at {r.o:#x}')
        self.protos = [Proto(r, f'{name}.{i}') for i in range(r.u32())]
        self.name = name
        # debug sections (usually stripped -> zero counts)
        for _ in range(r.u32()):
            r.u32()  # lineinfo
        for _ in range(r.u32()):
            r.string(); r.u32(); r.u32()  # locvars
        for _ in range(r.u32()):
            r.string()  # upvalues

    def kstr(self, i):
        v = self.consts[i]
        return repr(v) if not isinstance(v, str) else '"' + v + '"'

    def rk(self, x):
        return f'K({self.kstr(x - 256)})' if x >= 256 else f'r{x}'

    def disasm(self):
        out = [f'function {self.name} (params={self.numparams} '
               f'stack={self.maxstack} nups={self.nups})']
        for pc, ins in enumerate(self.code):
            op = OPCODES[ins & 0x3F]
            a = (ins >> 6) & 0xFF
            c = (ins >> 14) & 0x1FF
            b = (ins >> 23) & 0x1FF
            bx = (ins >> 14) & 0x3FFFF
            sbx = bx - 131071
            if op == 'LOADK':
                txt = f'r{a} = {self.kstr(bx)}'
            elif op == 'GETGLOBAL':
                txt = f'r{a} = _G[{self.kstr(bx)}]'
            elif op == 'SETGLOBAL':
                txt = f'_G[{self.kstr(bx)}] = r{a}'
            elif op == 'GETTABLE':
                txt = f'r{a} = r{b}[{self.rk(c)}]'
            elif op == 'SETTABLE':
                txt = f'r{a}[{self.rk(b)}] = {self.rk(c)}'
            elif op == 'SELF':
                txt = f'r{a+1} = r{b}; r{a} = r{b}:[{self.rk(c)}]'
            elif op == 'CALL':
                nargs = '...' if b == 0 else str(b - 1)
                nres = '...' if c == 0 else str(c - 1)
                txt = f'r{a}(..) nargs={nargs} nres={nres}'
            elif op == 'CLOSURE':
                txt = f'r{a} = closure {self.name}.{bx}'
            elif op == 'JMP':
                txt = f'-> {pc + 1 + sbx}'
            elif op in ('EQ', 'LT', 'LE'):
                txt = f'if ({self.rk(b)} {op} {self.rk(c)}) != {a} then skip'
            elif op == 'LOADBOOL':
                txt = f'r{a} = {bool(b)}' + (' ; skip' if c else '')
            elif op == 'MOVE':
                txt = f'r{a} = r{b}'
            elif op == 'RETURN':
                txt = f'return {b - 1 if b else "..."} vals from r{a}'
            else:
                txt = f'A={a} B={b} C={c} Bx={bx}'
            out.append(f'  [{pc:3}] {op:<10} {txt}')
        return '\n'.join(out)

    def walk(self):
        yield self
        for p in self.protos:
            yield from p.walk()


def load(path):
    data = open(path, 'rb').read()
    if data[:5] != b'\x1bLuaQ':
        raise ValueError('not a LuaQ chunk')
    r = Reader(data)
    r.o = 12  # header: \x1bLua, 0x51, fmt, endian, int, size_t, instr, num, integral
    return Proto(r, 'main')


def main():
    root = load(sys.argv[1])
    filt = sys.argv[2].lower() if len(sys.argv) > 2 else None
    for p in root.walk():
        if filt is not None:
            hay = ' '.join(str(c) for c in p.consts if isinstance(c, str)).lower()
            if filt not in hay:
                continue
        print(p.disasm())
        print()


if __name__ == '__main__':
    main()

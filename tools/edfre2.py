"""RE helpers over the installed EDF.dll (TimeDateStamp 0x678CCB46).

Small PE/capstone helpers over the installed EDF.dll, used by the two verify scripts.
Needs `pefile` and `capstone` (pip install pefile capstone).
"""
import re
import struct
import sys

import capstone
import pefile

# Where the game is installed. Override with the EDF6_DIR environment variable (the folder that holds
# EDF.dll), or pass the DLL path as the first argument.
import os
P = os.environ.get('EDF6_DLL') or os.path.join(
    os.environ.get('EDF6_DIR', r'E:\SteamLibrary\steamapps\common\EARTH DEFENSE FORCE 6'), 'EDF.dll')
if len(sys.argv) > 1 and os.path.isfile(sys.argv[1]) and sys.argv[1].lower().endswith('.dll'):
    P = sys.argv[1]
if not os.path.isfile(P):
    raise SystemExit(f'EDF.dll not found at {P}\n'
                     f'set EDF6_DIR (or EDF6_DLL) to your game folder and run again.')
pe = pefile.PE(P, fast_load=True)
img: bytes = pe.get_memory_mapped_image()
BASE = pe.OPTIONAL_HEADER.ImageBase
text = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
TVA: int = text.VirtualAddress
TSZ: int = text.Misc_VirtualSize
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
_pdata = None
_d32 = None


def pdata():
    global _pdata
    if _pdata is None:
        pe.parse_data_directories(
            directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
        _pdata = sorted((e.struct.BeginAddress, e.struct.EndAddress)
                        for e in pe.DIRECTORY_ENTRY_EXCEPTION)
    return _pdata


def func_start(rva: int) -> int:
    import bisect
    i = bisect.bisect_right(pdata(), (rva, 1 << 40)) - 1
    if i >= 0 and pdata()[i][0] <= rva < pdata()[i][1]:
        return pdata()[i][0]
    return -1


def func_end(rva: int) -> int:
    f = func_start(rva)
    if f < 0:
        return -1
    for b, e in pdata():
        if b == f:
            return e
    return -1


def dis(rva: int, n: int = 40, stop_ret: bool = True) -> None:
    for i, ins in enumerate(md.disasm(img[rva:rva + n * 16], rva)):
        print(f'{ins.address:#08x}: {ins.mnemonic:<8} {ins.op_str}')
        if i + 1 >= n:
            break
        if stop_ret and ins.mnemonic in ('ret', 'int3'):
            break


def dis_func(rva: int, limit: int = 4000) -> None:
    f = func_start(rva)
    print(f'# func {f:#x} .. {func_end(rva):#x}  (asked {rva:#x})')
    n = 0
    for ins in md.disasm(img[f:func_end(rva)], f):
        print(f'{ins.address:#08x}: {ins.mnemonic:<8} {ins.op_str}')
        n += 1
        if n >= limit:
            print('... truncated')
            break


def disp32():
    global _d32
    if _d32 is None:
        import numpy as np
        t = memoryview(img)[TVA:TVA + TSZ]
        n = len(t) - 4
        a = (int.from_bytes(bytes(t[0:n]), 'little')
             if False else None)
        arr = np.frombuffer(bytes(t), dtype=np.uint8).astype(np.int64)
        d = arr[0:n] | (arr[1:n + 1] << 8) | (arr[2:n + 2] << 16) | (arr[3:n + 3] << 24)
        d = np.where(d >= 2**31, d - 2**32, d)
        _d32 = d
    return _d32


def xrefs(rva: int) -> list:
    """Instruction start RVAs whose rip-relative disp32 hits rva."""
    import numpy as np
    d = disp32()
    pos = np.arange(len(d), dtype=np.int64)
    out = set()
    for imm in (0, 1, 4):
        tgt = TVA + pos + 4 + imm + d
        for p in np.nonzero(tgt == rva)[0]:
            for back in range(2, 9):
                st = TVA + int(p) - back
                for ins in md.disasm(img[st:st + 16], st):
                    if ins.address + ins.size == TVA + int(p) + 4 + imm:
                        out.add(st)
                    break
    return sorted(out)


def callers(target: int) -> list:
    import numpy as np
    d = disp32()
    pos = np.arange(len(d), dtype=np.int64)
    hit = np.nonzero(TVA + pos + 4 + d == target)[0]
    out = []
    for p in hit:
        op = img[TVA + int(p) - 1]
        if op in (0xE8, 0xE9):
            out.append(TVA + int(p) - 1)
    return out


def q(rva: int) -> int:
    return struct.unpack_from('<Q', img, rva)[0]


def d(rva: int) -> int:
    return struct.unpack_from('<I', img, rva)[0]


def cstr(rva: int, n: int = 64) -> str:
    return img[rva:rva + n].split(b'\0')[0].decode('latin1')


def wstr(rva: int, n: int = 96) -> str:
    return img[rva:rva + n * 2].decode('utf-16le', 'ignore').split('\0')[0]


def strs(s: str, wide: bool = False) -> list:
    b = s.encode('utf-16le') + b'\0\0' if wide else s.encode() + b'\0'
    return [m.start() for m in re.finditer(re.escape(b), img)]


def sig(rva: int, n: int = 16) -> str:
    return ' '.join(f'{b:02X}' for b in img[rva:rva + n])


def find_sig(pattern: str) -> list:
    """pattern: hex bytes with ?? wildcards."""
    parts = pattern.split()
    rx = b''
    mask = []
    for p in parts:
        if p in ('??', '?'):
            rx += b'.'
            mask.append(False)
        else:
            rx += re.escape(bytes([int(p, 16)]))
            mask.append(True)
    return [m.start() for m in re.finditer(rx, img)]


def vt_owner(addr: int):
    a = addr
    while a > addr - 0x3000 and a - 8 >= 0:
        col = q(a - 8) - BASE
        if 0 < col < len(img) - 24:
            s, off, cdo, tdr, hr, selfr = struct.unpack_from('<IIIIII', img, col)
            if s == 1 and selfr == col:
                name = img[tdr + 0x10:tdr + 0x80].split(b'\0')[0].decode('latin1')
                return name, a, (addr - a) // 8
        a -= 8
    return '?', 0, 0


def vtable_of(cls: str) -> list:
    name = ('.?AV' + cls + '@@').encode() + b'\0'
    res = []
    for td in [m.start() - 0x10 for m in re.finditer(re.escape(name), img)]:
        packed = struct.pack('<I', td)
        for m in re.finditer(re.escape(packed), img):
            col = m.start() - 12
            if col < 0:
                continue
            s, off, cdo, tdr, hr, selfr = struct.unpack_from('<IIIIII', img, col)
            if s != 1 or tdr != td or selfr != col or off != 0:
                continue
            colva = struct.pack('<Q', BASE + col)
            for v in re.finditer(re.escape(colva), img):
                res.append(v.start() + 8)
    return res


def classes(pat: str = '') -> list:
    """RTTI type names containing `pat`."""
    out = set()
    for m in re.finditer(rb'\.\?AV([^\x00]{1,90})@@', img):
        n = m.group(1).decode('latin1')
        if pat.lower() in n.lower():
            out.add(n)
    return sorted(out)


def find_disp(value: int, must: str = '', mnem: tuple = ()) -> list:
    pat = struct.pack('<I', value)
    hits = set()
    for m in re.finditer(re.escape(pat), img[TVA:TVA + TSZ]):
        f = func_start(TVA + m.start())
        if f >= 0:
            hits.add(f)
    out = []
    tag = f'+ {value:#x}]'
    for b, e in pdata():
        if b not in hits:
            continue
        for ins in md.disasm(img[b:e], b):
            s = f'{ins.mnemonic} {ins.op_str}'
            if tag in s and (not mnem or ins.mnemonic in mnem) and must in s:
                out.append((ins.address, s))
    return out


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'help'
    a = [int(x, 16) if x.startswith('0x') else x for x in sys.argv[2:]]
    if cmd == 'dis':
        dis(int(a[0], 16) if isinstance(a[0], str) else a[0],
            int(a[1]) if len(a) > 1 else 40)
    elif cmd == 'disfunc':
        dis_func(int(a[0], 16) if isinstance(a[0], str) else a[0])
    elif cmd == 'xrefs':
        for x in xrefs(int(a[0], 16) if isinstance(a[0], str) else a[0]):
            print(f'{x:#x}  {func_start(x):#x}  {sig(x, 8)}')
    elif cmd == 'callers':
        for x in callers(int(a[0], 16) if isinstance(a[0], str) else a[0]):
            print(f'{x:#x}  func {func_start(x):#x}')
    elif cmd == 'str':
        for x in strs(' '.join(sys.argv[2:])):
            print(f'{x:#x}')
    elif cmd == 'wstr':
        for x in strs(' '.join(sys.argv[2:]), wide=True):
            print(f'{x:#x}')
    elif cmd == 'classes':
        for c in classes(' '.join(sys.argv[2:])):
            print(c)
    elif cmd == 'vt':
        for v in vtable_of(sys.argv[2]):
            print(f'{v:#x}')
    elif cmd == 'vtowner':
        print(vt_owner(int(a[0], 16) if isinstance(a[0], str) else a[0]))
    elif cmd == 'sig':
        for x in find_sig(' '.join(sys.argv[2:]))[:40]:
            print(f'{x:#x}')
    elif cmd == 'disp':
        for ad, s in find_disp(int(a[0], 16) if isinstance(a[0], str) else a[0])[:200]:
            print(f'{ad:#x}  {s}')
    else:
        print(__doc__)

#!/usr/bin/env python3
"""elf2hunk.py - one-section m68k ELF (linked at 0 with --emit-relocs) to an
AmigaOS hunk executable: HUNK_HEADER, one HUNK_CODE, HUNK_RELOC32, HUNK_END.
Only R_68K_32 relocations need fixing up; PC-relative ones are position
independent already. SPDX-License-Identifier: GPL-2.0-only"""
import struct, sys

R_68K_NONE, R_68K_32, R_68K_PC32, R_68K_PC16, R_68K_PC8 = 0, 1, 4, 5, 6

def main(src, dst):
    e = open(src, 'rb').read()
    assert e[:4] == b'\x7fELF' and e[4] == 1 and e[5] == 2, 'not a 32-bit big-endian ELF'
    shoff, = struct.unpack('>I', e[32:36]); shentsize, shnum, shstrndx = struct.unpack('>HHH', e[46:52])
    secs = [struct.unpack('>IIIIIIIIII', e[shoff + i * shentsize: shoff + i * shentsize + 40]) for i in range(shnum)]
    strtab = secs[shstrndx]
    name = lambda off: e[strtab[4] + off: e.index(b'\0', strtab[4] + off)].decode()
    text = None; tidx = None
    for i, s in enumerate(secs):
        if name(s[0]) == '.text': text, tidx = s, i
    assert text, 'no .text'
    assert text[3] == 0, '.text must be linked at address 0'
    data = bytearray(e[text[4]: text[4] + text[5]])
    while len(data) % 4: data.append(0)
    relocs = []
    for s in secs:
        if s[1] == 4 and s[7] == tidx:                     # SHT_RELA for .text
            symtab = secs[s[6]]
            for k in range(s[5] // 12):
                off, info, add = struct.unpack('>IIi', e[s[4] + k * 12: s[4] + k * 12 + 12])
                typ = info & 0xff
                if typ in (R_68K_NONE, R_68K_PC32, R_68K_PC16, R_68K_PC8): continue
                if typ != R_68K_32: sys.exit('unsupported relocation type %d at %#x' % (typ, off))
                relocs.append(off)
        elif s[1] == 9 and s[7] == tidx:
            sys.exit('REL relocations not supported')
    relocs = sorted(set(relocs))
    for off in relocs:
        v, = struct.unpack('>I', data[off:off + 4])
        assert v <= len(data) + 0x100000, 'relocated value %#x at %#x points outside the hunk' % (v, off)
    n = len(data) // 4
    out = struct.pack('>IIIIII', 0x3f3, 0, 1, 0, 0, n)      # HUNK_HEADER: no names, 1 hunk, 0..0, size
    out += struct.pack('>II', 0x3e9, n) + bytes(data)       # HUNK_CODE
    if relocs:
        out += struct.pack('>III', 0x3ec, len(relocs), 0) + b''.join(struct.pack('>I', r) for r in relocs) + struct.pack('>I', 0)
    out += struct.pack('>I', 0x3f2)                         # HUNK_END
    open(dst, 'wb').write(out)
    print('%s: %d bytes code, %d relocations' % (dst, len(data), len(relocs)))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])

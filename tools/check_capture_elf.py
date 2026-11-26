#!/usr/bin/env python3
"""Check ARM ELF32 load segments/allocated sections against Capture DDR.

The linker MEMORY restriction and this post-link check are both required.
This checks the ELF, not external DMA buffers or a second CPU's allocations.
Only an unstripped ARM32 executable with a defined absolute reservation symbol
is accepted. Malformed/ambiguous symbol tables fail instead of being trusted.

ELF definitions: https://gabi.xinuos.com/elf/08-pheader.html and
https://gabi.xinuos.com/elf/05-symtab.html
"""
import argparse
import pathlib
import struct

BASE, LIMIT = 0x10000000, 0x10100000


def check(data):
    if len(data) < 52 or data[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('expected little-endian ELF32')
    h = struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
    etype, machine, version, entry, phoff, shoff, flags, ehsize, phsize, phnum, shsize, shnum, shstr = h
    if machine != 40 or etype != 2 or version != 1 or ehsize != 52:
        raise ValueError('expected an ARM executable ELF')
    if phsize != 32 or not phnum or phnum == 0xFFFF or shsize != 40 or not shnum or shstr >= shnum:
        raise ValueError('missing/unsupported program or section table')
    if phoff < ehsize or shoff < ehsize:
        raise ValueError('ELF table overlaps executable header')
    if phoff + phsize*phnum > len(data) or shoff + shsize*shnum > len(data):
        raise ValueError('truncated ELF tables')
    if max(phoff, shoff) < min(phoff + phsize*phnum, shoff + shsize*shnum):
        raise ValueError('overlapping ELF tables')
    checked = []
    loads = []

    def span(label, address, size):
        if address + size > 0x100000000:
            raise ValueError(label + ' address overflow')
        if size and address < LIMIT and address + size > BASE:
            raise ValueError(f'{label} overlaps Capture: {address:#x} + {size:#x}')
        checked.append((label, address, size))

    for i in range(phnum):
        typ, offset, va, pa, filesz, memsz, pflags, align = struct.unpack_from('<8I', data, phoff+i*phsize)
        if typ == 1:
            if filesz > memsz or offset+filesz > len(data):
                raise ValueError('invalid PT_LOAD size')
            if align not in (0, 1) and (align & (align - 1) or va % align != offset % align):
                raise ValueError('invalid PT_LOAD alignment')
            span(f'PT_LOAD[{i}] virtual', va, memsz)
            span(f'PT_LOAD[{i}] physical', pa, memsz)
            if memsz:
                loads.append((va, memsz, pflags))
    if not loads:
        raise ValueError('no nonempty PT_LOAD segment')
    if not any(va <= (entry & ~1) < va + size and fl & 1 for va, size, fl in loads):
        raise ValueError('entry point outside executable PT_LOAD')
    sections = [struct.unpack_from('<10I', data, shoff+i*shsize) for i in range(shnum)]
    if sections[0] != (0,) * 10:
        raise ValueError('unsupported/malformed null section')

    def section_bytes(s):
        if s[4] + s[5] > len(data):
            raise ValueError('truncated section')
        return data[s[4]:s[4]+s[5]]

    def string_table(index):
        if index >= len(sections) or sections[index][1] != 3:  # SHT_STRTAB
            raise ValueError('expected ELF string table')
        strings = section_bytes(sections[index])
        if not strings or strings[0] != 0 or strings[-1] != 0:
            raise ValueError('malformed ELF string table')
        return strings

    strings = string_table(shstr)

    def name(strings, offset):
        if offset >= len(strings):
            raise ValueError('bad ELF string offset')
        end = strings.find(b'\0', offset)
        if end < 0:
            raise ValueError('unterminated ELF string')
        return strings[offset:end].decode('utf-8', 'replace')

    markers = {'__aragatz_app_ddr_end': None,
               '__aragatz_capture_start': None,
               '__aragatz_capture_end': None}
    allocated = 0
    for i, s in enumerate(sections):
        label = name(strings, s[0])
        if s[1] not in (0, 8):  # SHT_NOBITS occupies memory but no file bytes.
            section_bytes(s)
        if s[2] & 2:  # SHF_ALLOC, including NOBITS heap/stacks
            span(label, s[3], s[5])
            allocated += bool(s[5])
        if s[1] != 2:
            continue
        if s[9] != 16 or not s[5] or s[5] % 16 or s[6] >= len(sections) or s[7] > s[5] // 16:
            raise ValueError('bad symbol table')
        symbols, names = section_bytes(s), string_table(s[6])
        if symbols[:16] != b'\0' * 16:
            raise ValueError('malformed first symbol entry')
        for offset in range(0, len(symbols), 16):
            n, value, size, info, other, index = struct.unpack_from('<IIIBBH', symbols, offset)
            marker = name(names, n)
            if marker in markers:
                valid_value = (0x00100000 < value <= BASE
                               if marker == '__aragatz_app_ddr_end' else
                               value == (BASE if marker == '__aragatz_capture_start' else LIMIT))
                if (index != 0xfff1 or info != 0x10 or other != 0 or size != 0 or
                        not valid_value):
                    raise ValueError('invalid linker DDR reservation symbol')
                if markers[marker] is not None:
                    raise ValueError('ambiguous duplicate linker DDR reservation symbol')
                markers[marker] = value
    if not allocated:
        raise ValueError('no allocated executable sections')
    if any(value is None for value in markers.values()):
        raise ValueError('reservation symbol missing; use reserve_capture_ddr.py before linking')
    return {'app_ddr_end_exclusive': hex(markers['__aragatz_app_ddr_end']), 'checked_ranges': len(checked),
            'capture_base': hex(BASE), 'capture_limit_exclusive': hex(LIMIT)}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('elf', type=pathlib.Path)
    a = p.parse_args()
    try:
        result = check(a.elf.read_bytes())
    except (OSError, ValueError, struct.error) as e:
        p.exit(1, f'ELF CHECK FAILED: {e}\n')
    print('ELF CHECK PASS:', result)


if __name__ == '__main__':
    main()

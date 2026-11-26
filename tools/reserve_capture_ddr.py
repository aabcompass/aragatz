#!/usr/bin/env python3
"""Reserve Capture DDR in an existing Vitis linker script, without changing PS DDR.

Writes a separate output file. Only a single fully literal MEMORY block is
accepted: hex, decimal, C-style octal and optional K/M scale suffixes. GNU ld
ORIGIN/org/o and LENGTH/len/l spellings are supported. Expressions, INCLUDE,
duplicate names and unparsed MEMORY text fail for review instead of guessing.

Grammar reference: https://sourceware.org/binutils/docs/ld/MEMORY.html
Constants: https://sourceware.org/binutils/docs/ld/Constants.html
"""
import argparse
import pathlib
import re

BASE = 0x10000000
LIMIT = 0x10100000
LITERAL = r"(?:0[xX][0-9a-fA-F]+|0[0-7]*|[1-9][0-9]*)(?:[KM])?(?![A-Za-z0-9_])"
REGION = re.compile(
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)(?:\s*\([rwxailRWXAIL!\s]*\))?\s*:\s*"
    r"(?:ORIGIN|org|o)\s*=\s*(?P<origin>" + LITERAL + r")\s*,\s*"
    r"(?:LENGTH|len|l)\s*=\s*(?P<length>" + LITERAL + r")"
)


def _mask_comments_and_strings(text):
    """Keep exact character offsets, but make comments/strings non-code."""
    masked = list(text)
    i = 0
    while i < len(text):
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            if end < 0:
                raise ValueError("unterminated linker-script comment")
            end += 2
        elif text[i] == '"':
            end = i + 1
            while end < len(text) and text[end] != '"':
                if text[end] == "\\":
                    end += 1
                end += 1
            if end >= len(text):
                raise ValueError("unterminated linker-script string")
            end += 1
        else:
            i += 1
            continue
        for j in range(i, end):
            if text[j] not in "\r\n":
                masked[j] = " "
        i = end
    return "".join(masked)


def _literal(token):
    multiplier = 1
    if token[-1:] in ("K", "M"):
        multiplier = 1024 if token[-1] == "K" else 1024 * 1024
        token = token[:-1]
    if token.lower().startswith("0x"):
        value = int(token, 16)
    elif token.startswith("0"):
        value = int(token, 8)
    else:
        value = int(token, 10)
    return value * multiplier


def _regions(body):
    """Every non-whitespace byte must belong to a supported region entry.

    finditer() alone is unsafe here: it can silently ignore expressions or
    an unrecognized overlapping region, and can match only a numeric prefix.
    """
    entries, names, cursor = [], set(), 0
    while cursor < len(body):
        whitespace = re.match(r"\s*", body[cursor:])
        cursor += whitespace.end()
        if cursor == len(body):
            break
        match = REGION.match(body, cursor)
        if match is None:
            raise ValueError("unsupported/malformed MEMORY entry near: " + repr(body[cursor:cursor+70]))
        if match["name"] in names:
            raise ValueError("duplicate MEMORY region: " + match["name"])
        origin, length = _literal(match["origin"]), _literal(match["length"])
        if origin > 0xFFFFFFFF or origin + length > 0x100000000:
            raise ValueError("MEMORY region exceeds ARM32 address space: " + match["name"])
        names.add(match["name"])
        entries.append((match, origin, length))
        cursor = match.end()
    return entries


def reserve(text, region_name=None):
    masked = _mask_comments_and_strings(text)
    if re.search(r"\b__aragatz_(?:app_ddr_end|capture_start|capture_end)\b", masked):
        raise ValueError("already patched: use the original Vitis lscript.ld as input")
    if re.search(r"\bINCLUDE\b", masked):
        raise ValueError("INCLUDE may hide another MEMORY block; use a complete literal script")
    keywords = list(re.finditer(r"\bMEMORY\b", masked))
    if len(keywords) != 1:
        raise ValueError("expected one literal MEMORY block")
    block = re.match(r"MEMORY\s*\{([^{}]*)\}", masked[keywords[0].start():])
    if block is None:
        raise ValueError("malformed MEMORY block")
    body_start = keywords[0].start() + block.start(1)
    regions = _regions(block.group(1))
    candidates = [(r, origin, length) for r, origin, length in regions if
                  (r['name'] == region_name if region_name else 'ddr' in r['name'].lower())]
    if len(candidates) != 1:
        raise ValueError("expected one DDR region; supply --region with the exact name")
    r, origin, length = candidates[0]
    if not (0x00100000 <= origin < BASE) or length <= 0:
        raise ValueError("unexpected DDR origin/length; compare the actual xparameters.h")
    new_length = min(length, BASE - origin)
    if origin + new_length > BASE:
        raise ValueError("DDR region still overlaps the capture reservation")
    start = body_start + r.start('length')
    end = body_start + r.end('length')
    result = text[:start] + hex(new_length) + text[end:]
    for entry, lo, size in regions:
        if entry['name'] == r['name']:
            size = new_length
        if size and lo < LIMIT and lo + size > BASE:
            raise ValueError("another MEMORY region overlaps Capture: " + entry['name'])
    name = r['name']
    result += f'''\n/* Aragatz Capture reservation [0x10000000, 0x10100000).
 * This changes ELF placement only. Keep the existing PS DDR preset. */
__aragatz_app_ddr_end = ORIGIN({name}) + LENGTH({name});
__aragatz_capture_start = 0x10000000;
__aragatz_capture_end = 0x10100000;
ASSERT(__aragatz_app_ddr_end <= 0x10000000, "Application DDR overlaps Capture")
'''
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('input', type=pathlib.Path)
    p.add_argument('output', type=pathlib.Path)
    p.add_argument('--region')
    a = p.parse_args()
    if a.input.resolve() == a.output.resolve():
        p.error('output must differ from input; keep the original linker script')
    try:
        result = reserve(a.input.read_text(), a.region)
        with a.output.open('x') as f:
            f.write(result)
    except (OSError, ValueError) as e:
        p.exit(1, str(e) + '\n')
    print(f'Created {a.output}; select it as the application linker script, then check the ELF.')


if __name__ == '__main__':
    main()

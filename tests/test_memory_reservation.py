#!/usr/bin/env python3
"""Negative tests for the real linker/ELF reservation tools.

ELF fixtures are constructed from the ELF32 layout, independently of the
checker. They are parser fixtures, not Vitis output or target-hardware tests.
No pyelftools, target compiler, GNU ld or third-party package is required.
"""
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import check_capture_elf as elf_tool
import reserve_capture_ddr as linker_tool

BASE, LIMIT = 0x10000000, 0x10100000
MARKER = "__aragatz_app_ddr_end"
DEFAULT_REGION = "ps7_ddr_0 : ORIGIN = 0x00100000, LENGTH = 0x3ff00000"


def script(body=DEFAULT_REGION):
    return "ENTRY(_start)\nMEMORY {\n" + body + "\n}\nSECTIONS { .text : { *(.text) } > ps7_ddr_0 }\n"


class LinkerReservationTests(unittest.TestCase):
    def test_standard_vitis_region_and_other_safe_regions(self):
        text = script(DEFAULT_REGION + "\n"
                      "ps7_ram_0 : ORIGIN = 0, LENGTH = 192K\n"
                      "ps7_ram_1 : ORIGIN = 0xffff0000, LENGTH = 0xfe00")
        result = linker_tool.reserve(text)
        self.assertIn("ps7_ddr_0 : ORIGIN = 0x00100000, LENGTH = 0xff00000", result)
        self.assertIn("ps7_ram_1 : ORIGIN = 0xffff0000, LENGTH = 0xfe00", result)
        self.assertIn(MARKER + " = ORIGIN(ps7_ddr_0) + LENGTH(ps7_ddr_0);", result)
        self.assertIn("ASSERT(" + MARKER + " <= 0x10000000", result)
        self.assertIn("__aragatz_capture_start = 0x10000000;", result)
        self.assertIn("__aragatz_capture_end = 0x10100000;", result)
        self.assertIn("LENGTH = 0x3ff00000", text)  # input was not modified

    def test_supported_literal_forms_and_keyword_aliases(self):
        cases = (
            "ps7_ddr_0(rwx):ORIGIN=1048576,LENGTH=1072693248",
            "ps7_ddr_0 (rw!x) : org = 1M, len = 1023M",
            "ps7_ddr_0 : o = 1024K, l = 1047552K",
            "ps7_ddr_0 : ORIGIN = 04000000, LENGTH = 07770000000",
            "ps7_ddr_0\n(rwx)\n:\nORIGIN\n=\n0X100000\n,\nLENGTH\n=\n0X3FF00000",
        )
        for body in cases:
            with self.subTest(body=body):
                result = linker_tool.reserve(script(body))
                self.assertIn("0xff00000", result)
                self.assertIn(MARKER, result)

    def test_already_shorter_ddr_is_not_enlarged(self):
        result = linker_tool.reserve(script("ps7_ddr_0 : ORIGIN=1M, LENGTH=64M"))
        self.assertIn("LENGTH=0x4000000", result)

    def test_comments_and_strings_cannot_define_phantom_regions(self):
        text = ('/* MEMORY { fake_ddr : ORIGIN=0, LENGTH=1 } ' + MARKER + ' */\n'
                'ASSERT(1, "MEMORY { appears in a diagnostic only }")\n' +
                script("ps7_ddr_0 /* explanatory } { */ (rwx) :\n"
                       "ORIGIN /* unit */ = 0x100000, LENGTH=0x3ff00000"))
        result = linker_tool.reserve(text)
        self.assertIn("/* explanatory } { */", result)
        self.assertIn("LENGTH=0xff00000", result)

    def test_expression_prefix_cannot_be_patched_as_a_literal(self):
        # Regression: the old finditer-based parser accepted the first
        # literal, left +0x20000000 intact, and incorrectly reported success.
        for bad in (
            "ps7_ddr_0 : ORIGIN=0x100000, LENGTH=0x3ff00000 + 0x20000000",
            "ps7_ddr_0 : ORIGIN=0x100000 + 0, LENGTH=0x3ff00000",
            "ps7_ddr_0 : ORIGIN=DDR_BASE, LENGTH=0x3ff00000",
            "ps7_ddr_0 : ORIGIN=0x100000, LENGTH=(0x40000000 - 0x100000)",
            "ps7_ddr_0 : ORIGIN=0x100000, LENGTH=0x3ff00000 /* gap */ + 1",
        ):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                linker_tool.reserve(script(bad))

    def test_unrecognized_other_region_cannot_hide_capture_overlap(self):
        for extra in (
            "unsafe : ORIGIN=0x10000000, LENGTH=CAPTURE_BYTES",
            "unsafe : ORIGIN=0x0ffff000+0x1000, LENGTH=0x100000",
            "unsafe : ORIGIN=0x10000000, LENGTH=0x100000 + 0",
            "unsafe : ORIGIN=0x10000000 LENGTH=0x100000",
            "unsafe : ORIGIN=0x10000000, LENGTH=1G",
            "unsafe : ORG=0x10000000, LENGTH=0x100000",  # GNU ld ORG is not org
        ):
            with self.subTest(extra=extra), self.assertRaises(ValueError):
                linker_tool.reserve(script(DEFAULT_REGION + "\n" + extra))

    def test_overlap_and_exclusive_boundaries(self):
        for origin, length in ((BASE, 1), (BASE - 1, 2), (LIMIT - 1, 1), (BASE - 16, 0x200000)):
            with self.subTest(origin=origin, length=length), self.assertRaisesRegex(ValueError, "overlaps Capture"):
                linker_tool.reserve(script(DEFAULT_REGION + f"\nother : ORIGIN={origin:#x}, LENGTH={length:#x}"))
        for origin, length in ((BASE - 16, 16), (LIMIT, 16), (BASE, 0), (BASE + 16, 0)):
            with self.subTest(safe_origin=origin, length=length):
                linker_tool.reserve(script(DEFAULT_REGION + f"\nother : ORIGIN={origin:#x}, LENGTH={length:#x}"))

    def test_ambiguous_duplicate_missing_and_out_of_range_regions(self):
        bad_bodies = (
            DEFAULT_REGION + "\nextra_ddr : ORIGIN=0x20000000, LENGTH=0x100000",
            DEFAULT_REGION + "\nps7_ddr_0 : ORIGIN=0x20000000, LENGTH=0x100000",
            "ram : ORIGIN=0x100000, LENGTH=0x3ff00000",
            "ps7_ddr_0 : ORIGIN=0x10000000, LENGTH=1M",
            "ps7_ddr_0 : ORIGIN=0, LENGTH=1M",
            "ps7_ddr_0 : ORIGIN=1M, LENGTH=0",
            DEFAULT_REGION + "\nother : ORIGIN=0xffffffff, LENGTH=2",
            DEFAULT_REGION + "\nother : ORIGIN=0x100000000, LENGTH=0",
            "ps7_ddr_0 : ORIGIN=1M, LENGTH=08",  # malformed C-style octal
            "ps7_ddr_0 : ORIGIN=1M, LENGTH=-1",
            "ps7_ddr_0 : ORIGIN=1M, LENGTH=0x3ff00000 trailing junk",
        )
        for body in bad_bodies:
            with self.subTest(body=body), self.assertRaises(ValueError):
                linker_tool.reserve(script(body))

    def test_explicit_region_selection(self):
        text = script(DEFAULT_REGION + "\nextra_ddr : ORIGIN=0x20000000, LENGTH=1M")
        result = linker_tool.reserve(text, "ps7_ddr_0")
        self.assertIn("LENGTH = 0xff00000", result)
        with self.assertRaises(ValueError):
            linker_tool.reserve(text, "missing")

    def test_malformed_multiple_blocks_and_include_are_rejected(self):
        for text in (
            "MEMORY { " + DEFAULT_REGION,
            "MEMORY { { " + DEFAULT_REGION + " } }",
            script() + "\nMEMORY { x : ORIGIN=0, LENGTH=1 }",
            script() + "\nINCLUDE another_board.ld",
            "/* unterminated " + script(),
            script() + '\nASSERT(1, "unterminated)',
        ):
            with self.subTest(text=text), self.assertRaises(ValueError):
                linker_tool.reserve(text)
        with self.assertRaisesRegex(ValueError, "already patched"):
            linker_tool.reserve(linker_tool.reserve(script()))

    def test_cli_does_not_overwrite_input_or_existing_output(self):
        with tempfile.TemporaryDirectory(prefix="aragatz-linker-test-") as tmp:
            input_path = pathlib.Path(tmp) / "original.ld"
            output_path = pathlib.Path(tmp) / "reserved.ld"
            original = script()
            input_path.write_text(original)
            command = [sys.executable, str(ROOT / "tools/reserve_capture_ddr.py"),
                       str(input_path), str(output_path)]
            subprocess.run(command, check=True, capture_output=True)
            reserved = output_path.read_text()
            self.assertEqual(input_path.read_text(), original)
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.assertEqual(output_path.read_text(), reserved)
            same = command[:-1] + [str(input_path)]
            self.assertNotEqual(subprocess.run(same, capture_output=True).returncode, 0)
            self.assertEqual(input_path.read_text(), original)


class ELF32Fixture:
    """Minimal ARM ET_EXEC with .text, string tables, and an absolute symbol."""
    def __init__(self, extra=(), symbols=None, segments=None):
        # Segment tuple = type, file offset, virtual, physical, filesz,
        # memsz, flags, alignment. text contains inert ARM instruction bytes.
        self.phoff = 52
        if segments is None:
            segments = [(1, 0x100, 0x00100000, 0x00100000, 16, 0x1000, 5, 4)]
        self.data = bytearray(0x100)
        self.data.extend(struct.pack("<4I", 0xEAFFFFFE, 0, 0, 0))
        self.sections = [dict(name="", typ=0, flags=0, addr=0, off=0, size=0, link=0, info=0, align=0, ent=0),
                         dict(name=".text", typ=1, flags=6, addr=0x00100000, off=0x100, size=16,
                              link=0, info=0, align=4, ent=0)]
        for name, typ, flags, address, size in extra:
            off = len(self.data)
            if typ != 8:
                self.data.extend(bytes(size))
            self.sections.append(dict(name=name, typ=typ, flags=flags, addr=address, off=off,
                                      size=size, link=0, info=0, align=4, ent=0))
        self.shstr_index = len(self.sections)
        self.str_index = self.shstr_index + 1
        self.sym_index = self.shstr_index + 2
        self.section_names = b"\0"
        self.name_offsets = {"": 0}
        for name in [s["name"] for s in self.sections[1:]] + [".shstrtab", ".strtab", ".symtab"]:
            self.name_offsets[name] = len(self.section_names)
            self.section_names += name.encode("ascii") + b"\0"
        if symbols is None:
            symbols = [(MARKER, BASE, 0, 0x10, 0, 0xFFF1),
                       ('__aragatz_capture_start', BASE, 0, 0x10, 0, 0xFFF1),
                       ('__aragatz_capture_end', LIMIT, 0, 0x10, 0, 0xFFF1)]
        symbol_names = b"\0"
        symbol_data = bytes(16)  # STN_UNDEF entry
        for name, value, size, info, other, index in symbols:
            offset = len(symbol_names)
            symbol_names += name.encode("ascii") + b"\0"
            symbol_data += struct.pack("<IIIBBH", offset, value, size, info, other, index)
        self.shstr_off = self._append(self.section_names)
        self.str_off = self._append(symbol_names)
        self.sym_off = self._append(symbol_data, align=4)
        for name, typ, off, content, link, info, align, ent in (
            (".shstrtab", 3, self.shstr_off, self.section_names, 0, 0, 1, 0),
            (".strtab", 3, self.str_off, symbol_names, 0, 0, 1, 0),
            (".symtab", 2, self.sym_off, symbol_data, self.str_index, 1, 4, 16),
        ):
            self.sections.append(dict(name=name, typ=typ, flags=0, addr=0, off=off, size=len(content),
                                      link=link, info=info, align=align, ent=ent))
        self.shoff = self._append(b"", align=4)
        for s in self.sections:
            self.data.extend(struct.pack("<10I", self.name_offsets[s["name"]], s["typ"], s["flags"],
                                         s["addr"], s["off"], s["size"], s["link"], s["info"], s["align"], s["ent"]))
        self.data[:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
        struct.pack_into("<HHIIIIIHHHHHH", self.data, 16, 2, 40, 1, 0x00100000,
                         self.phoff, self.shoff, 0x05000000, 52, 32, len(segments),
                         40, len(self.sections), self.shstr_index)
        for i, segment in enumerate(segments):
            struct.pack_into("<8I", self.data, self.phoff + 32*i, *segment)

    def _append(self, content, align=1):
        self.data.extend(bytes((-len(self.data)) % align))
        offset = len(self.data)
        self.data.extend(content)
        return offset

    def ph(self, field, value, index=0):
        struct.pack_into("<I", self.data, self.phoff + 32*index + 4*field, value)

    def sh(self, index, field, value):
        struct.pack_into("<I", self.data, self.shoff + 40*index + 4*field, value)


class ELFReservationTests(unittest.TestCase):
    def test_valid_arm_elf_and_empty_or_nonoverlapping_allocations(self):
        for extra in ((), ((".bss", 8, 3, 0x00100020, 0x100),),
                      ((".heap", 8, 3, BASE - 0x100, 0x100),),
                      ((".stack", 8, 3, LIMIT, 0x100),),
                      ((".zero", 8, 3, BASE, 0),)):
            with self.subTest(extra=extra):
                fixture = ELF32Fixture(extra=extra)
                result = elf_tool.check(fixture.data)
                self.assertEqual(result["app_ddr_end_exclusive"], hex(BASE))
                self.assertGreaterEqual(result["checked_ranges"], 3)

    def test_nobits_bss_heap_and_stack_overlap(self):
        for name, address, size in ((".bss", BASE - 16, 32), (".heap", BASE, 256),
                                    (".stack", LIMIT - 16, 32)):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "overlaps Capture"):
                elf_tool.check(ELF32Fixture(extra=[(name, 8, 3, address, size)]).data)

    def test_nobits_has_no_file_image_but_still_requires_address_bounds(self):
        fixture = ELF32Fixture(extra=[(".bss", 8, 3, 0x00100020, 0x100)])
        fixture.sh(2, 4, len(fixture.data) + 0x1000)
        elf_tool.check(fixture.data)  # NOBITS file offset does not allocate bytes.
        fixture.sh(2, 3, 0xFFFFFFF0)
        fixture.sh(2, 5, 32)
        with self.assertRaisesRegex(ValueError, "address overflow"):
            elf_tool.check(fixture.data)

    def test_pt_load_checks_both_virtual_and_physical_addresses(self):
        for field, expected in ((2, "virtual"), (3, "physical")):
            fixture = ELF32Fixture()
            fixture.ph(field, BASE)
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, expected + " overlaps"):
                elf_tool.check(fixture.data)

    def test_pt_load_zero_fill_tail_and_exclusive_boundary(self):
        baseline = (1, 0x100, 0x00100000, 0x00100000, 16, 0x1000, 5, 4)
        safe = (1, 0, BASE - 0x1000, BASE - 0x1000, 0, 0x1000, 6, 1)
        elf_tool.check(ELF32Fixture(segments=[baseline, safe]).data)
        unsafe = (1, 0, BASE - 0x1000, BASE - 0x1000, 0, 0x1001, 6, 1)
        with self.assertRaisesRegex(ValueError, "overlaps Capture"):
            elf_tool.check(ELF32Fixture(segments=[baseline, unsafe]).data)

    def test_missing_and_ambiguous_reservation_symbol(self):
        with self.assertRaisesRegex(ValueError, "symbol missing"):
            elf_tool.check(ELF32Fixture(symbols=[]).data)
        for second in (BASE, BASE - 0x1000):
            fixture = ELF32Fixture(symbols=[(MARKER, BASE, 0, 0x10, 0, 0xFFF1),
                                             (MARKER, second, 0, 0x10, 0, 0xFFF1)])
            with self.subTest(second=second), self.assertRaisesRegex(ValueError, "duplicate"):
                elf_tool.check(fixture.data)

    def test_capture_boundary_markers_are_required_and_exact(self):
        valid = [(MARKER, BASE, 0, 0x10, 0, 0xFFF1),
                 ('__aragatz_capture_start', BASE, 0, 0x10, 0, 0xFFF1),
                 ('__aragatz_capture_end', LIMIT, 0, 0x10, 0, 0xFFF1)]
        for missing in (1, 2):
            with self.subTest(missing=missing), self.assertRaisesRegex(ValueError, 'symbol missing'):
                elf_tool.check(ELF32Fixture(symbols=valid[:missing] + valid[missing+1:]).data)
        for index in (1, 2):
            wrong = list(valid)
            name, value, *tail = wrong[index]
            wrong[index] = (name, value + 32, *tail)
            with self.subTest(index=index), self.assertRaisesRegex(ValueError, 'invalid linker'):
                elf_tool.check(ELF32Fixture(symbols=wrong).data)

    def test_malformed_reservation_symbols(self):
        cases = ((BASE + 1, 0, 0x10, 0, 0xFFF1), (0x00100000, 0, 0x10, 0, 0xFFF1),
                 (0, 0, 0x10, 0, 0xFFF1), (BASE, 0, 0x10, 0, 0),
                 (BASE, 0, 0x10, 0, 1), (BASE, 0, 0x00, 0, 0xFFF1),
                 (BASE, 0, 0x20, 0, 0xFFF1), (BASE, 0, 0x11, 0, 0xFFF1),
                 (BASE, 4, 0x10, 0, 0xFFF1), (BASE, 0, 0x10, 3, 0xFFF1))
        for item in cases:
            with self.subTest(item=item), self.assertRaisesRegex(ValueError, "invalid linker"):
                elf_tool.check(ELF32Fixture(symbols=[(MARKER,) + item]).data)

    def test_bad_symbol_table_shape_link_and_strings(self):
        modifications = (
            lambda f: f.sh(f.sym_index, 9, 8),
            lambda f: f.sh(f.sym_index, 5, 31),
            lambda f: f.sh(f.sym_index, 6, len(f.sections)),
            lambda f: f.sh(f.sym_index, 6, 1),  # PROGBITS is not a string table
            lambda f: f.sh(f.sym_index, 4, len(f.data) + 1),
            lambda f: f.sh(f.sym_index, 7, 10000),
            lambda f: f.data.__setitem__(f.sym_off, 1),  # malformed STN_UNDEF
            lambda f: struct.pack_into("<I", f.data, f.sym_off + 16, 0xFFFFFFFF),
            lambda f: f.data.__setitem__(f.str_off + len(MARKER) + 1, ord("X")),
        )
        for modify in modifications:
            fixture = ELF32Fixture()
            modify(fixture)
            with self.subTest(modify=modify), self.assertRaises(ValueError):
                elf_tool.check(fixture.data)

    def test_bad_section_tables_and_file_backed_section_size(self):
        for kind in ("section_name", "shstr_type", "shstr_offset", "null_section", "text_file_size"):
            fixture = ELF32Fixture()
            if kind == "section_name": fixture.sh(1, 0, 0xFFFFFFFF)
            elif kind == "shstr_type": fixture.sh(fixture.shstr_index, 1, 1)
            elif kind == "shstr_offset": fixture.sh(fixture.shstr_index, 4, len(fixture.data) + 1)
            elif kind == "null_section": fixture.sh(0, 1, 1)
            else: fixture.sh(1, 4, len(fixture.data) + 1)
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                elf_tool.check(fixture.data)

    def test_non_load_empty_or_invalid_segments_are_not_a_valid_check(self):
        for field, value in ((0, 0), (5, 0), (4, 0x2000), (1, 0xFFFFFF00),
                             (7, 3), (7, 4096), (2, 0xFFFFFFF0), (3, 0xFFFFFFF0), (6, 4)):
            fixture = ELF32Fixture()
            fixture.ph(field, value)
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                elf_tool.check(fixture.data)
        fixture = ELF32Fixture()
        fixture.sh(1, 2, 0)  # valid segment but no allocated sections
        with self.assertRaisesRegex(ValueError, "no allocated"):
            elf_tool.check(fixture.data)

    def test_malformed_elf_headers_and_truncation(self):
        for size in (0, 6, 51, 60, 100):
            with self.subTest(size=size), self.assertRaises(ValueError):
                elf_tool.check(ELF32Fixture().data[:size])
        for offset, fmt, value in ((4, "<B", 2), (5, "<B", 2), (16, "<H", 1),
                                   (18, "<H", 62), (28, "<I", 0), (32, "<I", 52),
                                   (42, "<H", 0), (44, "<H", 0), (46, "<H", 0),
                                   (48, "<H", 0), (50, "<H", 0xFFFF)):
            fixture = ELF32Fixture()
            struct.pack_into(fmt, fixture.data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                elf_tool.check(fixture.data)


if __name__ == "__main__":
    unittest.main(verbosity=2)

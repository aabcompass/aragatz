#!/usr/bin/env python3
"""Bound MDIO polling in the exact 2023.1 emacps driver; no in-place writes.

Example: python apply_mdio_timeout.py /stock/xemacps_control.c --output-file /new/xemacps_control.c
The budget bounds status-register poll iterations, not elapsed wall time.
No PHY reset, skew, clock, or register transaction contents are changed.
"""
from __future__ import annotations

import argparse
import difflib
import hashlib
from pathlib import Path
import re

UPSTREAM_BLOB = "4c72f20e2154ae647e2b4f79d891ec8681fd4afb"
POLL_BUDGET = 100000


def blob_sha(source: str) -> str:
    data = source.encode("utf-8")
    return hashlib.sha1(b"blob " + str(len(data)).encode("ascii") + b"\0" + data).hexdigest()


def extract_function(source: str, name: str) -> str:
    start = source.index("LONG " + name + "(")
    end = source.index("\n}\n", start) + 3
    return source[start:end]


def transform_function(body: str, temporary: str) -> str:
    if body.count("\tLONG Status;") != 1:
        raise ValueError("unexpected MDIO function declarations")
    body = body.replace("\tLONG Status;", "\tLONG Status;\n\tu32 PollCount;", 1)
    pattern = (r"\tdo\s*\{\s*Ipisr = XEmacPs_ReadReg\(InstancePtr->Config.BaseAddress,\s*"
               r"XEMACPS_NWSR_OFFSET\);\s*" + temporary + r" = Ipisr;\s*\} while \(\("
               + temporary + r" & XEMACPS_NWSR_MDIOIDLE_MASK\) == 0x00000000U\);")
    replacement = (
        f"\t/* Finite poll budget; not a wall-clock timeout guarantee. */\n"
        f"\tfor (PollCount = 0U; PollCount < {POLL_BUDGET}U; ++PollCount) {{\n"
        "\t\tIpisr = XEmacPs_ReadReg(InstancePtr->Config.BaseAddress,\n"
        "\t\t\t\tXEMACPS_NWSR_OFFSET);\n"
        f"\t\t{temporary} = Ipisr;\n"
        f"\t\tif (({temporary} & XEMACPS_NWSR_MDIOIDLE_MASK) != 0U)\n"
        "\t\t\tbreak;\n\t}\n"
        f"\tif (PollCount == {POLL_BUDGET}U) {{\n"
        "\t\treturn (LONG)XST_FAILURE;\n\t}")
    body, count = re.subn(pattern, lambda _: replacement, body)
    if count != 1:
        raise ValueError("unexpected MDIO poll loop")
    return body


def transform(source: str) -> str:
    if blob_sha(source) != UPSTREAM_BLOB:
        raise ValueError("upstream blob mismatch; expected exact xilinx_v2023.1 " + UPSTREAM_BLOB)
    for name, temp in (("XEmacPs_PhyRead", "IpReadTemp"),
                       ("XEmacPs_PhyWrite", "IpWriteTemp")):
        original = extract_function(source, name)
        source = source.replace(original, transform_function(original, temp), 1)
    return source


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_file", type=Path)
    parser.add_argument("--output-file", type=Path)
    parser.add_argument("--diff", action="store_true")
    args = parser.parse_args()
    try:
        source = args.source_file.read_text(encoding="utf-8")
        patched = transform(source)
        if args.output_file:
            if args.output_file.resolve() == args.source_file.resolve() or args.output_file.exists():
                raise ValueError("output must be a new file, separate from the installed driver")
            args.output_file.parent.mkdir(parents=True, exist_ok=True)
            args.output_file.write_text(patched, encoding="utf-8", newline="\n")
        if args.diff:
            print("".join(difflib.unified_diff(source.splitlines(True), patched.splitlines(True),
                  fromfile="a/xemacps_control.c", tofile="b/xemacps_control.c")), end="")
        else:
            print(f"verified upstream {UPSTREAM_BLOB}; patched blob={blob_sha(patched)}")
            print(f"MDIO poll budget: {POLL_BUDGET} status reads per operation")
            if not args.output_file:
                print("Dry-run only. Add --output-file to create the driver overlay.")
    except (OSError, ValueError, UnicodeError) as exc:
        parser.exit(2, f"error: {exc}\n")


if __name__ == "__main__":
    main()

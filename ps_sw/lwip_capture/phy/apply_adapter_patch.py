#!/usr/bin/env python3
"""Build a reviewed KSZ9031 overlay; never edit the installed BSP in place.

Input: a netif directory from Xilinx embeddedsw tag xilinx_v2023.1.
The three input files must match the recorded upstream Git blob SHA1s after
CRLF normalization. No --force override: an already modified BSP needs review.
Without --output-dir this performs verification/dry-run only.
"""
from __future__ import annotations

import argparse
import difflib
import hashlib
from pathlib import Path


UPSTREAM = {
    "xemacpsif_physpeed.c": "8ccc7ba895b9513679abfa029408e55bae78ebd9",
    "xemacpsif_hw.c": "5f13438759251827cbf3f2aa0dd9c1065eb726b6",
    "xadapter.c": "feffb83184d9db171666dfbc046c01891db1be7e",
}


def git_blob_sha(text: str) -> str:
    data = text.encode("utf-8")
    return hashlib.sha1(b"blob " + str(len(data)).encode("ascii") + b"\0" + data).hexdigest()


def replace_once(source: str, old: str, new: str) -> str:
    if source.count(old) != 1:
        raise ValueError("patch context is not unique: " + repr(old[:90]))
    return source.replace(old, new, 1)


def include(name: str) -> str:
    return Path(__file__).with_name(name).read_text(encoding="utf-8")


def replace_function(source: str, signature: str, replacement: str) -> str:
    if source.count(signature) != 1:
        raise ValueError("function signature is not unique: " + signature)
    start = source.index(signature)
    end = source.index("\n}\n", start) + 3
    return source[:start] + replacement.rstrip() + "\n" + source[end:]



def patch_recovery(source: str) -> str:
    return replace_once(source,
        'void init_emacps_on_error (xemacpsif_s *xemacps, struct netif *netif)\n'
        '{\n\tXEmacPs *xemacpsp;\n\ts32_t status = XST_SUCCESS;\n\n'
        '\txemacpsp = &xemacps->emacps;',
        'void init_emacps_on_error (xemacpsif_s *xemacps, struct netif *netif)\n'
        '{\n\tXEmacPs *xemacpsp;\n\ts32_t status = XST_SUCCESS;\n\n'
        '\txemacpsp = &xemacps->emacps;\n'
        '\t/* Startup may have timed out before any valid negotiated speed. */\n'
        '\tif (link_speed != 10U && link_speed != 100U && link_speed != 1000U) {\n'
        '\t\txemacps->eth_link_status = ETH_LINK_DOWN;\n'
        '\t\txil_printf("GEM recovery has no valid negotiated PHY speed\\r\\n");\n'
        '\t\treturn;\n\t}')


def transform(name: str, source: str) -> str:
    if name == "xemacpsif_physpeed.c":
        helper = Path(__file__).with_name("ksz9031_impl.inc").read_text(encoding="utf-8")
        source = replace_once(source, '#include "xemac_ieee_reg.h"\n',
                              '#include "xemac_ieee_reg.h"\n#include "sleep.h"\n\n' + helper + '\n')
        source = replace_function(source, "void detect_phy(", include("ksz9031_detect.inc"))
        source = replace_once(source,
            '} else if (link_speed != XST_FAILURE){',
            '} else if (link_speed == 10){')
        old = ('\tu16_t phy_identity;\n\tu32_t RetStatus;\n\n'
               '\tXEmacPs_PhyRead(xemacpsp, phy_addr, PHY_IDENTIFIER_1_REG,\n'
               '\t\t\t\t\t&phy_identity);\n'
               '\tif (phy_identity == PHY_TI_IDENTIFIER) {')
        new = ('\tu16_t phy_identity, phy_model;\n\tu32_t RetStatus;\n\n'
               '\tif (XEmacPs_PhyRead(xemacpsp, phy_addr, PHY_IDENTIFIER_1_REG,\n'
               '\t\t&phy_identity) != XST_SUCCESS)\n'
               '\t\treturn XST_FAILURE;\n'
               '\tif (phy_identity == ARAGATZ_KSZ9031_ID1) {\n'
               '\t\tif (XEmacPs_PhyRead(xemacpsp, phy_addr, PHY_IDENTIFIER_2_REG,\n'
               '\t\t\t&phy_model) != XST_SUCCESS ||\n'
               '\t\t    (phy_model & ARAGATZ_KSZ9031_ID2_MASK) != ARAGATZ_KSZ9031_ID2) {\n'
               '\t\t\txil_printf("Unsupported Micrel PHY model or MDIO failure\\r\\n");\n'
               '\t\t\treturn XST_FAILURE;\n'
               '\t\t}\n'
               '\t\tRetStatus = get_KSZ9031_phy_speed(xemacpsp, phy_addr);\n'
               '\t} else if (phy_identity == PHY_TI_IDENTIFIER) {')
        source = replace_once(source, old, new)
    elif name == "xemacpsif_hw.c":
        source = replace_once(source, 'u32_t link_speed = 100;', 'u32_t link_speed = 0;')
        source = replace_once(source, '\tdetect_phy(xemacpsp);',
            '\tlink_speed = 0U;\n\tphyaddrforemac = 32U;\n\tdetect_phy(xemacpsp);')
        source = replace_once(source, 'for (i = 31; i > 0; i--) {',
                              'for (i = 32; i-- > 0;) {')
        start = source.index('\t/* If no PHY was detected, use broadcast PHY address of 0 */')
        end = source.index('\n#else\n\tlink_speed = pcs_setup_emacps', start)
        source = source[:start] + (
            '\t/* Address 0 is scanned explicitly; do not configure an absent PHY. */\n'
            '\tif (phyfoundforemac0 == FALSE && phyfoundforemac1 == FALSE) {\n'
            '\t\txemacps->eth_link_status = ETH_LINK_UNDEFINED;\n'
            '\t\txil_printf("No PHY detected; check reset/MDIO and restart application\\r\\n");\n'
            '\t\treturn;\n\t}') + source[end:]
        source = replace_once(source, 'if (link_speed == XST_FAILURE) {',
            'if (link_speed != 10U && link_speed != 100U && link_speed != 1000U) {')
        source = patch_recovery(source)
    elif name == "xadapter.c":
        source = replace_once(source, 'u32_t phyaddrforemac;',
            'u32_t phyaddrforemac;\nextern u32_t link_speed; /* GEM recovery cache */\n'
            '#if defined(XLWIP_CONFIG_INCLUDE_GEM)\n'
            'extern int aragatz_ksz9031_read_speed(XEmacPs *, u32_t, u32_t *);\n'
            '#endif')
        source = replace_function(source, 'void emacps_link_status(', include("ksz9031_link.inc"))
    else:
        raise ValueError("unexpected input file")
    return source


def prepare(source_dir: Path) -> dict[str, tuple[str, str]]:
    outputs = {}
    for name, expected in UPSTREAM.items():
        source = (source_dir / name).read_text(encoding="utf-8")
        actual = git_blob_sha(source)
        if actual != expected:
            raise ValueError(f"{name}: upstream blob mismatch: {actual}, expected {expected}. "
                             "Use the exact 2023.1 source, or review existing board modifications.")
        outputs[name] = (source, transform(name, source))
    return outputs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_dir", type=Path, help="2023.1 netif directory (three stock C files)")
    parser.add_argument("--output-dir", type=Path, help="new directory for the three patched C files")
    parser.add_argument("--diff", action="store_true", help="print a unified review diff")
    args = parser.parse_args()
    try:
        outputs = prepare(args.source_dir)
        if args.output_dir:
            if args.output_dir.resolve() == args.source_dir.resolve():
                raise ValueError("output must differ from source; in-place editing is disabled")
            if args.output_dir.exists() and any(args.output_dir.iterdir()):
                raise ValueError("output directory must be empty")
            args.output_dir.mkdir(parents=True, exist_ok=True)
            for name, (_, patched) in outputs.items():
                (args.output_dir / name).write_text(patched, encoding="utf-8", newline="\n")
        for name, (source, patched) in outputs.items():
            if args.diff:
                print("".join(difflib.unified_diff(source.splitlines(True), patched.splitlines(True),
                       fromfile="a/" + name, tofile="b/" + name)), end="")
            else:
                print(f"verified {name}; patched blob={git_blob_sha(patched)}")
        if not args.output_dir and not args.diff:
            print("Dry-run only. Add --output-dir to create the overlay.")
    except (OSError, ValueError, UnicodeError) as exc:
        parser.exit(2, f"error: {exc}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

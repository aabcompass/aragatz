#!/usr/bin/env python3
"""Syntax-check server.c against actual Xilinx 2023.1 lwIP 2.1.3 headers.

Supply --include-root .../lwip213/src/lwip-2.1.3/src/include and
--port-include .../lwip213/src/contrib/ports/xilinx/include from the tag.
The API headers are real and hash-checked. lwipopts.h and cpu.h are temporary
host-check configuration shims, NOT a target BSP. No link/hardware validation.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

EXPECTED = {
    "lwip/tcp.h": "daf759946068095ea578f4776c369a44ba05dffa",
    "lwip/pbuf.h": "5a4fc88b37da8e89497dc0b8450bed25ddc8fdfe",
    "lwip/opt.h": "fd9d2ca3d3e959abe0162af6d0467a4534c447e3",
}
PORT_SHA = "264ce0a6fef5766ac16a2f8e89574f67935d2bdc"


def blob_sha(path: Path) -> str:
    data = path.read_text(encoding="utf-8").encode("utf-8")
    return hashlib.sha1(b"blob " + str(len(data)).encode("ascii") + b"\0" + data).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--include-root", required=True, type=Path)
    parser.add_argument("--port-include", required=True, type=Path)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    for relative, expected in EXPECTED.items():
        if blob_sha(args.include_root / relative) != expected:
            parser.error(f"{relative}: expected exact xilinx_v2023.1 header")
    if blob_sha(args.port_include / "arch/cc.h") != PORT_SHA:
        parser.error("arch/cc.h: expected exact xilinx_v2023.1 Xilinx port")
    with tempfile.TemporaryDirectory(prefix="aragatz-headers-", dir=Path.cwd()) as temp:
        shim = Path(temp)
        (shim / "cpu.h").write_text("/* Host-only: target-generated CPU settings absent. */\n")
        (shim / "lwipopts.h").write_text(
            "#define NO_SYS 1\n#define LWIP_TCP 1\n#define LWIP_IPV4 1\n"
            "#define LWIP_IPV6 0\n#define LWIP_DHCP 0\n"
            "#define LWIP_CALLBACK_API 1\n#define LWIP_SOCKET 0\n"
            "#define LWIP_NETCONN 0\n#define MEM_ALIGNMENT 4\n"
            "#define TCP_MSS 1460\n#define TCP_SND_BUF 65535\n#define TCP_WND 65535\n")
        command = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
                   "-I", str(shim), "-I", str(args.include_root),
                   "-I", str(args.port_include),
                   "-I", str(repo / "ps_sw/lwip_capture"),
                   str(repo / "ps_sw/lwip_capture/server.c")]
        subprocess.run(command, check=True)
    print("PASS: server.c syntax/types against actual 2023.1 lwIP headers (host config only)")


if __name__ == "__main__":
    main()

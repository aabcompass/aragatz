#!/usr/bin/env python3
"""Run Linux host C contract tests; never access target hardware.

Requirements: Python 3, GCC-compatible cc, Linux mmap, GNU-compatible linker.
The fixture output is temporary unless --fixture is explicitly supplied.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
APP = HERE.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path,
                        help="write C-encoded 64-byte v1 header plus COUNTER payload")
    args = parser.parse_args()
    subprocess.run([sys.executable, str(HERE / "test_capture_host.py")], check=True)
    with tempfile.TemporaryDirectory(prefix="aragatz-server-host-") as tmp:
        exe = Path(tmp) / "test_server"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-Wconversion", "-Wshadow", "-I", str(HERE / "server_stubs"),
            "-I", str(APP), str(HERE / "test_server.c"), str(APP / "protocol.c"),
            "-o", str(exe),
        ], check=True)
        command = [str(exe)]
        if args.fixture:
            if args.fixture.exists():
                parser.error(f"fixture already exists: {args.fixture}")
            command.append(str(args.fixture))
        subprocess.run(command, check=True, timeout=10)
    print("PS host checks PASS. Vitis build, ARM cache, PL, PHY/link and Ethernet rate remain untested here.")


if __name__ == "__main__":
    main()

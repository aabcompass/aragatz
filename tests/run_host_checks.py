#!/usr/bin/env python3
"""Run reproducible software checks only; never talks to an FPGA or Ethernet PHY."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(*args):
    subprocess.run(args, cwd=ROOT, check=True)


def main():
    cc = shutil.which("cc") or shutil.which("gcc")
    if not cc:
        raise SystemExit("A host C compiler (cc/GCC) is required")
    run(sys.executable, "-m", "unittest", "discover", "-s", "host", "-p", "test_*.py", "-v")
    run(sys.executable, "ps_sw/lwip_capture/tests/run_ps_checks.py")
    run(sys.executable, "-m", "unittest", "discover", "-s", "tests", "-p", "test_memory*.py", "-v")
    phy = "ps_sw/lwip_capture/phy/tests/"
    run(sys.executable, phy + "test_mdio_budget.py")
    run(sys.executable, phy + "test_recovery.py")
    with tempfile.TemporaryDirectory(prefix="aragatz-phy-tests-") as folder:
        for name in ("test_ksz9031_mock", "test_discovery"):
            binary = str(Path(folder) / name)
            run(cc, "-std=c11", "-Wall", "-Wextra", "-Werror", phy + name + ".c", "-o", binary)
            run(binary)
    print("HOST CHECKS PASS: no Vitis build, FPGA execution, PHY/link qualification or board speed measurement.")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Test the exact transformed upstream MDIO functions with simulated MMIO.

No BSP is needed. The adjacent fixture has the two original 2023.1 functions
and upstream license. This checks code behavior, not the physical MDIO bus.
"""
from __future__ import annotations
import argparse
import importlib.util
from pathlib import Path
import subprocess
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    spec = importlib.util.spec_from_file_location("mdio_patch", here.parent / "apply_mdio_timeout.py")
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    fixture = (here / "mdio_stock_functions.c.inc").read_text(encoding="utf-8")
    functions = ""
    for name, temp in (("XEmacPs_PhyRead", "IpReadTemp"), ("XEmacPs_PhyWrite", "IpWriteTemp")):
        functions += module.transform_function(module.extract_function(fixture, name), temp)
    code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef long LONG;
typedef struct { struct { uintptr_t BaseAddress; } Config; } XEmacPs;
#define Xil_AssertNonvoid(x) assert(x)
#define TRUE 1
#define XST_SUCCESS 0
#define XST_FAILURE 1
#define XST_EMAC_MII_BUSY 2
/* MMIO model tokens; these are not hardware addresses. */
#define XEMACPS_NWSR_OFFSET 1
#define XEMACPS_PHYMNTNC_OFFSET 2
#define XEMACPS_NWSR_MDIOIDLE_MASK 4
#define XEMACPS_PHYMNTNC_OP_MASK 0x40000000U
#define XEMACPS_PHYMNTNC_OP_R_MASK 0x20000000U
#define XEMACPS_PHYMNTNC_OP_W_MASK 0x10000000U
#define XEMACPS_PHYMNTNC_PHAD_SHFT_MSK 23
#define XEMACPS_PHYMNTNC_PREG_SHFT_MSK 18
static int initial_idle, started, complete_after;
static unsigned polls, writes;
static u32 transaction;
static u32 XEmacPs_ReadReg(uintptr_t base,u32 reg) {
    (void)base;
    if (reg==XEMACPS_NWSR_OFFSET) {
        if (!started) return initial_idle?XEMACPS_NWSR_MDIOIDLE_MASK:0;
        ++polls;
        return complete_after>=0 && polls>=(unsigned)complete_after
            ? XEMACPS_NWSR_MDIOIDLE_MASK:0;
    }
    assert(reg==XEMACPS_PHYMNTNC_OFFSET);
    return 0x1234;
}
static void XEmacPs_WriteReg(uintptr_t base,u32 reg,u32 value) {
    (void)base;assert(reg==XEMACPS_PHYMNTNC_OFFSET);
    started=1;++writes;transaction=value;
}
static void reset(int idle,int done_after) {
    initial_idle=idle;complete_after=done_after;started=0;
    polls=writes=transaction=0;
}
'''
    code += f"\n#define EXPECTED_BUDGET {module.POLL_BUDGET}U\n" + functions
    code += r'''
int main(void) {
    XEmacPs mac={{0}};
    u16 value=0xbeef;
    reset(1,3);
    assert(XEmacPs_PhyRead(&mac,1,2,&value)==XST_SUCCESS);
    assert(value==0x1234 && polls==3 && writes==1);
    reset(1,3);
    assert(XEmacPs_PhyWrite(&mac,1,2,0xabcd)==XST_SUCCESS);
    assert((transaction&0xffffU)==0xabcd && polls==3 && writes==1);
    reset(0,3);
    assert(XEmacPs_PhyRead(&mac,1,2,&value)==XST_EMAC_MII_BUSY);
    assert(polls==0 && writes==0);
    reset(0,3);
    assert(XEmacPs_PhyWrite(&mac,1,2,0)==XST_EMAC_MII_BUSY);
    assert(polls==0 && writes==0);
    reset(1,-1);value=0xbeef;
    assert(XEmacPs_PhyRead(&mac,1,2,&value)==XST_FAILURE);
    assert(polls==EXPECTED_BUDGET && writes==1 && value==0xbeef);
    reset(1,-1);
    assert(XEmacPs_PhyWrite(&mac,1,2,0)==XST_FAILURE);
    assert(polls==EXPECTED_BUDGET && writes==1);
    reset(1,EXPECTED_BUDGET);
    assert(XEmacPs_PhyRead(&mac,1,2,&value)==XST_SUCCESS);
    assert(polls==EXPECTED_BUDGET && value==0x1234);
    puts("MDIO budget mock: PASS (success, busy, stuck read/write, final poll)");
    return 0;
}
'''
    try:
        module.transform(fixture)
    except ValueError as exc:
        assert "upstream blob mismatch" in str(exc)
    else:
        raise AssertionError("non-upstream file accepted")
    with tempfile.TemporaryDirectory(prefix="aragatz-mdio-test-", dir=Path.cwd()) as temp:
        source = Path(temp) / "mdio.c"
        executable = Path(temp) / "mdio-test"
        source.write_text(code,encoding="utf-8")
        subprocess.run([args.cc,"-std=c11","-Wall","-Wextra","-Werror",str(source),
                        "-o",str(executable)],check=True)
        subprocess.run([str(executable)],check=True)


if __name__ == "__main__":
    main()

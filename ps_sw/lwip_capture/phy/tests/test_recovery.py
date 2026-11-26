#!/usr/bin/env python3
"""Compile/test the exact transformed recovery functions with host MMIO mocks.

Usage: python test_recovery.py [/path/to/stock-2023.1/netif]
This is not a hardware or full BSP test. Requires a host C compiler.
"""
from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path
import subprocess
import tempfile


def function(source: str, name: str) -> str:
    start = source.index("void " + name + "(") if "void " + name + "(" in source else source.index("void " + name + " (")
    end = source.index("\n}\n", start) + 3
    return source[start:end]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_dir", nargs="?", type=Path)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    script = Path(__file__).resolve().parents[1] / "apply_adapter_patch.py"
    spec = importlib.util.spec_from_file_location("adapter_patch", script)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    if args.source_dir:
        outputs = module.prepare(args.source_dir)
        link_function = function(outputs["xadapter.c"][1], "emacps_link_status")
        recovery_function = function(outputs["xemacpsif_hw.c"][1], "init_emacps_on_error")
    else:
        fixture = Path(__file__).with_name("recovery_stock_function.c.inc").read_text(encoding="utf-8")
        link_function = module.include("ksz9031_link.inc")
        recovery_function = module.patch_recovery(function(fixture, "init_emacps_on_error"))
    code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32_t;
typedef uint32_t u32;
typedef uint16_t u16_t;
typedef int32_t s32_t;
typedef struct { unsigned IsReady; } XEmacPs;
enum { ETH_LINK_UNDEFINED, ETH_LINK_UP, ETH_LINK_DOWN, ETH_LINK_NEGOTIATING };
typedef struct { XEmacPs emacps; int eth_link_status; } xemacpsif_s;
struct netif { unsigned char hwaddr[6]; int up; };
#define XIL_COMPONENT_IS_READY 1U
#define XST_SUCCESS 0
#define XST_FAILURE 1
#define IEEE_STATUS_REG_OFFSET 1
#define IEEE_STAT_LINK_STATUS 4U
#define IEEE_STAT_AUTONEGOTIATE_COMPLETE 32U
#define LWIP_DEBUGF(...) ((void)0)
#define xil_printf(...) ((void)0)
static unsigned phyaddrforemac, link_speed;
static unsigned negotiated = 100, speed_sets, actual_speed;
static int mdio_failed, phy_state = 1;
static int XEmacPs_PhyRead(XEmacPs *p,unsigned a,unsigned r,u16_t *v)
{ (void)p;(void)a;(void)r; *v=0x24; return mdio_failed?1:0; }
static int aragatz_ksz9031_read_speed(XEmacPs *p,unsigned a,unsigned *speed)
{ u16_t value; if(XEmacPs_PhyRead(p,a,1,&value)) return -1;
  *speed=negotiated==1?1000:negotiated; return phy_state; }
static unsigned phy_setup_emacps(XEmacPs *p,unsigned a)
{ (void)p;(void)a; return negotiated; }
static void XEmacPs_SetOperatingSpeed(XEmacPs *p,unsigned v)
{ (void)p;assert(v==10||v==100||v==1000);++speed_sets;actual_speed=v; }
static int XEmacPs_SetMacAddress(XEmacPs *p,void *m,unsigned i)
{ (void)p;(void)m;(void)i;return 0; }
static void netif_set_link_up(struct netif *n) { n->up=1; }
static void netif_set_link_down(struct netif *n) { n->up=0; }
'''
    code += link_function
    code += recovery_function
    code += r'''
int main(void) {
    struct netif n={{0},1};
    xemacpsif_s ps={{1},ETH_LINK_UP};
    link_speed=1; /* initial PHY timeout */
    init_emacps_on_error(&ps,&n);
    assert(speed_sets==0 && ps.eth_link_status==ETH_LINK_DOWN);
    ps.eth_link_status=ETH_LINK_NEGOTIATING;
    negotiated=100;
    emacps_link_status(&n,&ps,&ps.emacps);
    assert(speed_sets==1 && actual_speed==100 && link_speed==100);
    assert(ps.eth_link_status==ETH_LINK_UP && n.up);
    init_emacps_on_error(&ps,&n);
    assert(speed_sets==2 && actual_speed==100);
    ps.eth_link_status=ETH_LINK_NEGOTIATING;
    negotiated=1000;
    emacps_link_status(&n,&ps,&ps.emacps);
    assert(speed_sets==3 && link_speed==1000);
    ps.eth_link_status=ETH_LINK_NEGOTIATING;
    negotiated=1;
    emacps_link_status(&n,&ps,&ps.emacps);
    assert(speed_sets==3 && link_speed==1000 && ps.eth_link_status==ETH_LINK_NEGOTIATING);
    ps.eth_link_status=ETH_LINK_UP;
    mdio_failed=1;
    emacps_link_status(&n,&ps,&ps.emacps);
    assert(!n.up && ps.eth_link_status==ETH_LINK_NEGOTIATING && speed_sets==3);
    mdio_failed=0;
    negotiated=100;
    ps.eth_link_status=ETH_LINK_UP;
    n.up=1;
    phy_state=-2; /* impossible speed, half duplex, forced BMCR */
    emacps_link_status(&n,&ps,&ps.emacps);
    assert(!n.up && ps.eth_link_status==ETH_LINK_NEGOTIATING && speed_sets==3);
    phy_state=1;
    emacps_link_status(&n,&ps,&ps.emacps);
    assert(n.up && link_speed==100 && speed_sets==4);
    emacps_link_status(&n,&ps,&ps.emacps);
    assert(speed_sets==4); /* stable link does not reprogram GEM */
    negotiated=1000; /* renegotiated between link polls */
    emacps_link_status(&n,&ps,&ps.emacps);
    assert(link_speed==1000 && speed_sets==5);
    puts("GEM recovery mock: PASS (initial failure, renegotiated speed, MDIO errors)");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="aragatz-phy-review-", dir=Path.cwd()) as tmp:
        source = Path(tmp) / "recovery.c"
        executable = Path(tmp) / "recovery"
        source.write_text(code, encoding="utf-8")
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(source), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()

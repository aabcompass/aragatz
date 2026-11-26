#!/usr/bin/env python3
"""Compile/partially link PS sources with exact 2023.1 networking headers.

This OPTIONAL host check uses real lwIP/Xilinx adapter and Cortex-A9 timer
headers, but deliberately small, generated BSP/driver shims. It checks API
usage and cross-file symbols; it is NOT a Vitis/ARM build or a BSP validation.
No vendor source files are downloaded or installed by this script.

Arguments refer to an Xilinx embeddedsw checkout at tag xilinx_v2023.1:
  --include-root ThirdParty/sw_services/lwip213/src/lwip-2.1.3/src/include
  --port-include ThirdParty/sw_services/lwip213/src/contrib/ports/xilinx/include
  --standalone-include lib/bsp/standalone/src/arm/cortexa9
  --echo-platform lib/sw_apps/lwip_echo_server/src/platform.c
"""
from __future__ import annotations
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
APP = ROOT / 'ps_sw/lwip_capture'
EXPECTED = {
    'lwip/tcp.h': 'daf759946068095ea578f4776c369a44ba05dffa',
    'lwip/pbuf.h': '5a4fc88b37da8e89497dc0b8450bed25ddc8fdfe',
    'lwip/opt.h': 'fd9d2ca3d3e959abe0162af6d0467a4534c447e3',
    'lwip/init.h': '6cabfc8fd4247b77a1e3de9c998d1d55f91f6463',
}
PORT_EXPECTED = {
    'arch/cc.h': '264ce0a6fef5766ac16a2f8e89574f67935d2bdc',
    'netif/xadapter.h': '1ec90f0f9ff1c1e0f5644599cdae60b7edf499a0',
    'netif/xtopology.h': 'f943e3eaf7a26b41c2a4e8ab4e60e3d47f1309f3',
}
TIMER_SHA = '9d17f5cb3c15c91734afecadab44afb6f783cc3e'
ECHO_PLATFORM_SHA = 'e54890a9852ed74151ea2044e1de1268da60203d'

# These files exist only inside the temporary host-build directory. They do
# not model hardware/register effects, and must never be used as a target BSP.
SHIMS = {
'cpu.h': '',
'lwipopts.h': '''
#define NO_SYS 1
#define LWIP_TCP 1
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_DHCP 0
#define LWIP_CALLBACK_API 1
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define MEM_ALIGNMENT 4
#define TCP_MSS 1460
#define TCP_SND_BUF 65535
#define TCP_WND 65535
''',
'xil_types.h': '''
#ifndef HOST_XIL_TYPES_H
#define HOST_XIL_TYPES_H
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef uintptr_t UINTPTR;
typedef intptr_t INTPTR;
#endif
''',
'xparameters.h': '''
#ifndef HOST_XPARAMETERS_H
#define HOST_XPARAMETERS_H
#define XPAR_XEMACPS_0_BASEADDR 0xe000b000U
#define XPAR_SCUGIC_SINGLE_DEVICE_ID 0U
#define XPAR_SCUTIMER_DEVICE_ID 0U
#define XPAR_SCUGIC_0_CPU_BASEADDR 0xf8f00100U
#define XPAR_SCUGIC_0_DIST_BASEADDR 0xf8f01000U
#define XPAR_SCUTIMER_INTR 29U
#define XPAR_CPU_CORTEXA9_0_CPU_CLK_FREQ_HZ 666666666U
#define XPAR_CPU_CORTEXA9_CORE_CLOCK_FREQ_HZ 666666666U
#define XPAR_GLOBAL_TMR_BASEADDR 0xf8f00200U
#define XPAR_CAMERA_CAPTURE_BD_0_BASEADDR 0x40000000U
#define XPAR_CAMERA_CAPTURE_BD_0_HIGHADDR 0x4000ffffU
#define XPAR_CAMERA_TEST_GEN_BD_0_BASEADDR 0x40010000U
#define XPAR_CAMERA_TEST_GEN_BD_0_HIGHADDR 0x4001ffffU
#define XPAR_PS7_DDR_0_S_AXI_BASEADDR 0x00100000U
#define XPAR_PS7_DDR_0_S_AXI_HIGHADDR 0x3fffffffU
#define XPAR_PS7_UART_0_BASEADDR 0xe0000000U
#define STDIN_BASEADDRESS XPAR_PS7_UART_0_BASEADDR
#endif
''',
'xparameters_ps.h': '#include "xparameters.h"\n',
'xil_io.h': '''
#include "xil_types.h"
u32 Xil_In32(UINTPTR);
void Xil_Out32(UINTPTR, u32);
''',
'xil_cache.h': '''
#include "xil_types.h"
void Xil_DCacheFlushRange(INTPTR, u32);
void Xil_DCacheInvalidateRange(INTPTR, u32);
void Xil_ICacheDisable(void);
void Xil_DCacheDisable(void);
''',
'xil_printf.h': 'void xil_printf(const char *, ...);\n',
'xil_exception.h': '''
#ifndef HOST_XIL_EXCEPTION_H
#define HOST_XIL_EXCEPTION_H
#include "xil_types.h"
typedef void (*Xil_ExceptionHandler)(void *);
typedef void (*Xil_InterruptHandler)(void *);
#define XIL_EXCEPTION_ID_IRQ_INT 5U
#define XIL_EXCEPTION_IRQ 0x80U
void Xil_ExceptionInit(void);
void Xil_ExceptionRegisterHandler(u32, Xil_ExceptionHandler, void *);
void Xil_ExceptionEnableMask(u32);
#endif
''',
'xscugic.h': '''
#include "xil_exception.h"
#define XST_SUCCESS 0
s32 XScuGic_DeviceInitialize(u32);
void XScuGic_DeviceInterruptHandler(void *);
void XScuGic_RegisterHandler(u32, s32, Xil_InterruptHandler, void *);
void XScuGic_EnableIntr(u32, u32);
''',
'xscutimer.h': '''
#include "xil_types.h"
typedef struct { u16 DeviceId; u32 BaseAddr; } XScuTimer_Config;
typedef struct { XScuTimer_Config Config; u32 IsReady, IsStarted; } XScuTimer;
XScuTimer_Config *XScuTimer_LookupConfig(u16);
s32 XScuTimer_CfgInitialize(XScuTimer *, XScuTimer_Config *, u32);
s32 XScuTimer_SelfTest(XScuTimer *);
void XScuTimer_EnableAutoReload(XScuTimer *);
void XScuTimer_LoadTimer(XScuTimer *, u32);
void XScuTimer_EnableInterrupt(XScuTimer *);
void XScuTimer_Start(XScuTimer *);
void XScuTimer_ClearInterruptStatus(XScuTimer *);
''',
'xemacps.h': '''
#ifndef HOST_XEMACPS_H
#define HOST_XEMACPS_H
/* Only opaque XEmacPs pointers are consumed by phy_diag.h in this check. */
typedef struct { unsigned host_placeholder; } XEmacPs;
#endif
''',
'xpseudo_asm.h': 'void host_dsb(void);\n#define dsb() host_dsb()\n',
}


def sha(path):
    # A CRLF working checkout is accepted as the same source text.
    data = path.read_text(encoding='utf-8').encode('utf-8')
    return hashlib.sha1(b'blob ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--include-root', required=True, type=Path)
    p.add_argument('--port-include', required=True, type=Path)
    p.add_argument('--standalone-include', required=True, type=Path)
    p.add_argument('--echo-platform', required=True, type=Path)
    p.add_argument('--cc', default='cc')
    p.add_argument('--nm', default='nm')
    args = p.parse_args()
    expected = [(args.include_root / path, value) for path, value in EXPECTED.items()]
    expected += [(args.port_include / path, value) for path, value in PORT_EXPECTED.items()]
    expected += [(args.standalone_include / 'xtime_l.h', TIMER_SHA),
                 (args.echo_platform, ECHO_PLATFORM_SHA)]
    for path, value in expected:
        if sha(path) != value:
            p.error(f'{path}: expected exact xilinx_v2023.1 source')
    with tempfile.TemporaryDirectory(prefix='aragatz-app-api-') as tmp:
        folder = Path(tmp)
        shim = folder / 'shims'
        shim.mkdir()
        for name, contents in SHIMS.items():
            (shim / name).write_text('/* HOST COMPILE SHIM, NOT A TARGET BSP. */\n' + contents)
        flags = [args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-D__arm__',
                 '-fno-pie', '-I', str(shim), '-I', str(args.include_root),
                 '-I', str(args.port_include), '-I', str(args.standalone_include),
                 '-I', str(APP), '-I', str(ROOT / 'ps_sw/baremetal/include')]
        objects = []
        sources = [APP / name for name in ('capture.c', 'protocol.c', 'server.c',
                                           'platform_zynq.c', 'platform_poll.c')]
        sources.append(args.echo_platform)
        for n, source in enumerate(sources):
            obj = folder / f'unit{n}.o'
            subprocess.run(flags + ['-c', str(source), '-o', str(obj)], check=True)
            objects.append(str(obj))
        # Both documented capture gate values must compile.
        for enabled in (0, 1):
            obj = folder / f'main{enabled}.o'
            subprocess.run(flags + [f'-DARAGATZ_ENABLE_CAPTURE={enabled}', '-c',
                                   str(APP / 'main.c'), '-o', str(obj)], check=True)
        linked = folder / 'app_host_partial.o'
        subprocess.run([args.cc, '-r', '-no-pie', *objects, str(folder / 'main1.o'),
                        '-o', str(linked)], check=True)
        defined = subprocess.check_output([args.nm, '-g', '--defined-only', str(linked)], text=True)
        names = [line.split()[-1] for line in defined.splitlines() if line.split()]
        for symbol in ('main', 'echo_netif', 'init_platform', 'cleanup_platform',
                       'aragatz_platform_ready', 'aragatz_platform_poll',
                       'TcpFastTmrFlag', 'TcpSlowTmrFlag', 'ArLinkPollFlag', 'ArRxPollFlag',
                       'ar_capture_init', 'ar_capture_poll', 'ar_server_init', 'ar_server_poll'):
            if names.count(symbol) != 1:
                raise RuntimeError(f'{symbol}: expected exactly one linked definition')
    print('PASS: main/platform/capture/server compile and partial link with real 2023.1 lwIP/Xilinx headers.')
    print('BSP/SCU/GIC declarations used host shims. This is not a Vitis build or hardware test.')


if __name__ == '__main__':
    main()

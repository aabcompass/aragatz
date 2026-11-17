# Aragatz Zynq-7000 bare-metal hardware regression V2

This is a standalone **Cortex-A9 (ps7_cortexa9_0)** program for **Vitis 2023.1**.
It tests the actual PL camera path through PS UART0, AXI-Lite, AXI3/HP0 and DDR3.

## Source organization

- `main.c`: UART0 menu and optional autorun (enabled by default).
- `include/aragatz_hw.h`: register-map definitions and **BSP `xparameters.h`** accessors.
- `tests/hw_tests.c`: self-checking AXI-Lite, smoke, prehistory and multi-fragment tests.
- `tests/hw_tests.h`: test entrypoints.

**Do not add `xparameters.h` or `xparameters_ps.h` to Git.** These are generated BSP files and
must come from the XSA exported from the *matching* Aragatz bitstream.
The supplied BSP definitions are:

```c
XPAR_CAMERA_CAPTURE_BD_0_BASEADDR  = 0x40000000
XPAR_CAMERA_TEST_GEN_BD_0_BASEADDR = 0x40010000
XPAR_PS7_DDR_0_S_AXI_BASEADDR     = 0x00100000
XPAR_PS7_DDR_0_S_AXI_HIGHADDR     = 0x3FFFFFFF
STDIN_BASEADDRESS / STDOUT_BASEADDRESS = 0xE0000000 (PS UART0)
```

## Vitis 2023.1 setup

1. Export XSA **including bitstream** from the verified Vivado build.
2. Generate/refresh the Vitis platform + **standalone Cortex-A9 BSP** from this XSA.
3. In the Vitis application project, **link** `main.c` and `tests/hw_tests.c`
   (do not only add the C file containing `main`).
4. Add **both** application header search paths: `ps_sw/baremetal/include/` and
   `ps_sw/baremetal/tests/` (these matter if Vitis links the C files as symlinks).
5. Driver dependencies: standard `xil_io.h`, `xil_cache.h`, `xil_printf.h`, `xuartps_hw.h`,
   `sleep.h` supplied by the standalone BSP.
6. Set BSP `stdin=ps7_uart_0`, `stdout=ps7_uart_0`; open PS UART0 at 115200 8N1.
7. Run via JTAG, with PS7 initialization + updated bitstream before ELF.

## Crucial linker rule

`ARAGATZ_TEST_DDR_BASE = 0x10000000` and `ARAGATZ_TEST_DDR_SIZE = 0x00100000`
are reserved for the FPGA writer. **The ELF image, stacks, heap, and any DMA must be outside this range.**
A simple standalone linker setup is to restrict the usable DDR region to
`ORIGIN=0x00100000, LENGTH=0x03F00000`, i.e. through `0x03FFFFFF`.
Do not replace the entire Vitis-generated linker script; change only the DDR
region's origin/length and keep the usual section descriptions. The code checks
PS DDR and HP0 address boundaries and checks its current stack location, but
cannot prove that arbitrary application data/heap never use the buffer.

The AXI HP0 Address Editor in Aragatz currently maps only 0x00000000..0x1FFFFFFF.
The code does not attempt to write above this window.

## Tests

- 1: AXI-Lite: versions + register readback.
- 2: Smoke: 1 fragment / 3 frames / trigger at frame 32 word 47.
- 3: Pretrigger: 1, 4, 16, 64 prehistory frames; 3 post-trigger frames.
- 4: Multi-fragment: 2, 4, 16 fragments, each 4 pre + 4 post frames;
  triggers at frame 128 + k*128.
- a: run all.
- q: return from the firmware.

Default firmware runs all on boot and then enters the menu. To disable autorun
add the compiler symbol `ARAGATZ_AUTORUN=0` to the Vitis application build.
The generator uses COUNTER, 96 words/frame, rate 24/125, algorithmic trigger
at word 47. Each DDR fragment is verified word-by-word including preamble,
trigger-frame index, all science words, and 256 guard bytes following the run.

The buffer is prefilled with a sentinel, flushed from the CPU cache, and the
entire region is invalidated after the PL writer has completed. To preserve
reproducibility, record the Git SHA of the firmware and both IP submodules,
and record whether the FPGA bitstream was rebuilt after RTL changes.

If a test times out or Capture enters ERROR, the firmware does not force-reset
PL automatically. Reset/reprogram PL and investigate status/error registers.

**Hardware results are not claimed for V2 before running it on the board.**

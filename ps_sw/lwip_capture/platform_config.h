#ifndef ARAGATZ_PLATFORM_CONFIG_H
#define ARAGATZ_PLATFORM_CONFIG_H
#include "xparameters.h"
#ifndef PLATFORM_ZYNQ
#define PLATFORM_ZYNQ
#endif
#if defined(XPAR_XEMACPS_0_BASEADDR)
#define PLATFORM_EMAC_BASEADDR XPAR_XEMACPS_0_BASEADDR
#elif defined(XPAR_PS7_ETHERNET_0_BASEADDR)
#define PLATFORM_EMAC_BASEADDR XPAR_PS7_ETHERNET_0_BASEADDR
#else
#error "GEM0 base address missing: regenerate xparameters.h from the existing Aragatz XSA"
#endif
#endif

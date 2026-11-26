/* Aragatz finite capture export, standalone lwIP raw API, Zynq-7000.
 * Build capture.c, protocol.c, server.c, platform_poll.c, platform_zynq.c
 * and phy/phy_diag.c with the Vitis 2023.1 echo template's platform.c.
 * Hardware regression remains a separate application in ../baremetal. */
#include <stdint.h>
#include "xparameters.h"
#include "xil_printf.h"
#include "xtime_l.h"
#include "platform.h"
#include "platform_config.h"
#include "lwip/init.h"
#include "lwip/tcp.h"
#include "netif/xadapter.h"
#include "capture.h"
#include "server.h"
#include "site_config.h"

#if !NO_SYS || !LWIP_TCP || !LWIP_IPV4 || LWIP_IPV6 || LWIP_DHCP
#error "Use the Vitis 2023.1 standalone raw IPv4 TCP BSP: NO_SYS=1, DHCP/IPv6=0"
#endif
#if !defined(PLATFORM_ZYNQ) || defined(__aarch64__)
#error "This application targets Zynq-7000 Cortex-A9 only"
#endif
#if ARAGATZ_ENABLE_CAPTURE != 0 && ARAGATZ_ENABLE_CAPTURE != 1
#error "ARAGATZ_ENABLE_CAPTURE must be 0 or 1"
#endif

extern volatile int TcpFastTmrFlag;
extern volatile int TcpSlowTmrFlag;
/* The stock 2023.1 echo main declares these timer entry points explicitly. */
void tcp_fasttmr(void);
void tcp_slowtmr(void);
static struct netif netif0;
/* The Xilinx platform template declares this global even though timer work
 * is deferred in our platform_zynq.c. Keep the expected symbol/type. */
struct netif *echo_netif = &netif0;

uint64_t ar_now_us(void)
{
    XTime t;
    const uint64_t hz = (uint64_t)COUNTS_PER_SECOND;
    XTime_GetTime(&t);
    /* Avoid overflowing t*1000000 during long running sessions. */
    return ((uint64_t)t / hz) * UINT64_C(1000000) +
           (((uint64_t)t % hz) * UINT64_C(1000000)) / hz;
}

int main(void)
{
    ip_addr_t ip, mask, gw;
    unsigned char mac[] = { ARAGATZ_MAC };
    uint32_t link_mbps, old_link = UINT32_MAX;
    uint64_t logged_id = 0;
    int reported_fault = 0;
    init_platform();
    if (!aragatz_platform_ready()) {
        xil_printf("Aragatz: platform timer setup failed; reset required\r\n");
        return 1;
    }
    lwip_init();
    IP4_ADDR(&ip,ARAGATZ_IP_A,ARAGATZ_IP_B,ARAGATZ_IP_C,ARAGATZ_IP_D);
    IP4_ADDR(&mask,255,255,255,0);
    IP4_ADDR(&gw,0,0,0,0);
    if (!xemac_add(&netif0,&ip,&mask,&gw,mac,PLATFORM_EMAC_BASEADDR)) {
        xil_printf("Aragatz: xemac_add failed\r\n");
        return 1;
    }
    netif_set_default(&netif0);
    netif_set_up(&netif0);
#if ARAGATZ_ENABLE_CAPTURE
    if (ar_capture_init()!=0)
        xil_printf("Capture unavailable: %s\r\n",ar_capture_fault_reason());
#endif
    if (ar_server_init(ARAGATZ_PORT,ARAGATZ_ENABLE_CAPTURE)!=0) {
        xil_printf("Aragatz: TCP listener creation failed\r\n");
        return 1;
    }
    platform_enable_interrupts();
    xil_printf("Aragatz TCP v1: %d.%d.%d.%d:%d, capture=%d, header=64\r\n",
        ARAGATZ_IP_A,ARAGATZ_IP_B,ARAGATZ_IP_C,ARAGATZ_IP_D,
        ARAGATZ_PORT,ARAGATZ_ENABLE_CAPTURE);
    for (;;) {
        /* Clear before work: a new ISR tick must not be erased afterwards. */
        if (TcpFastTmrFlag) { TcpFastTmrFlag=0; tcp_fasttmr(); }
        if (TcpSlowTmrFlag) { TcpSlowTmrFlag=0; tcp_slowtmr(); }
        xemacif_input(&netif0);
        link_mbps = aragatz_platform_poll(&netif0);
        ar_server_set_link(link_mbps);
        if (link_mbps != old_link) {
            xil_printf("KSZ9031 verified full-duplex link: %d Mbps (0=unavailable)\r\n",
                       (unsigned)link_mbps);
            old_link = link_mbps;
        }
        ar_capture_poll(ar_now_us());
        if (ar_capture_state()==AR_CAPTURE_FAULT && !reported_fault) {
            xil_printf("Capture fault, buffer locked: %s\r\n",ar_capture_fault_reason());
            reported_fault = 1;
        }
        if (ar_capture_state()==AR_CAPTURE_READY && ar_capture_meta()->id!=logged_id) {
            const struct ar_capture_metadata *m = ar_capture_meta();
            xil_printf("Capture %u ready: %u bytes crc=%08x; capture/cache/CRC us=%u/%u/%u\r\n",
                       (unsigned)m->id,(unsigned)m->payload_bytes,
                       (unsigned)m->payload_crc32,(unsigned)m->capture_us,
                       (unsigned)m->cache_us,(unsigned)m->crc_us);
            logged_id = m->id;
        }
        ar_server_poll(ar_now_us());
    }
}

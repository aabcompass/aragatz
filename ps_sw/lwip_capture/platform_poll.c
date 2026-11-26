/* Serialize all lwIP adapter/MDIO activity with the raw API main loop. */
#include "platform.h"
#include "netif/xadapter.h"
#include "phy/phy_diag.h"

extern volatile int ArLinkPollFlag;
extern volatile int ArRxPollFlag;

uint32_t aragatz_platform_poll(struct netif *netif)
{
    static int initialized;
    static uint32_t link_mbps;
    if (!netif || !netif->state) return 0;
    if (!initialized || ArLinkPollFlag) {
        /* Clear before work so the ISR may schedule another pass. */
        ArLinkPollFlag = 0;
        initialized = 1;
        eth_link_detect(netif);
        if (ar_phy_link_info(netif, &link_mbps) != 1) link_mbps = 0;
    }
    if (ArRxPollFlag) {
        ArRxPollFlag = 0;
        xemacpsif_resetrx_on_no_rxdata(netif);
    }
    return link_mbps;
}

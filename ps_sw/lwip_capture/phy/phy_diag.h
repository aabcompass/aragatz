#ifndef ARAGATZ_PHY_DIAG_H
#define ARAGATZ_PHY_DIAG_H

#include "xemacps.h"
#include <stdint.h>

struct netif;

/* Use an already initialized MAC. No PHY writes, reset, or reinitialization.
 * Run with MDIO callers serialized; avoid the timer ISR and transfer hot path.
 */
int aragatz_phy_scan(XEmacPs *mac);
int aragatz_phy_dump(XEmacPs *mac, unsigned phy_addr);
/* Silent, no writes/waits: 1 verified full-duplex up; 0 down/negotiating;
 * -1 MDIO/identity/duplex error. mbps is zero unless return value is 1.
 */
int ar_phy_link_info(struct netif *netif, uint32_t *mbps);

#endif

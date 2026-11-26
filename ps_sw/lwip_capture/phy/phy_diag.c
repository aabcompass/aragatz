#include "phy_diag.h"
#include "xil_printf.h"
#include "xstatus.h"
#include "netif/xadapter.h"
#include "netif/xemacpsif.h"

/* The 2023.1 adapter selects this address during init_emacps. */
extern u32_t phyaddrforemac;
/* Exported by the version-checked BSP overlay. Requiring this symbol also
 * prevents silently linking the capture app against an unpatched adapter. */
extern int aragatz_ksz9031_read_speed(XEmacPs *, u32_t, u32_t *);

int ar_phy_link_info(struct netif *netif, uint32_t *mbps)
{
    struct xemac_s *adapter;
    xemacpsif_s *ps;
    XEmacPs *mac;
    u16 id1, id2;
    u32_t speed = 0U;
    int result;
    if (mbps == 0) return -1;
    *mbps = 0;
    if (netif == 0 || netif->state == 0) return -1;
    adapter = (struct xemac_s *)netif->state;
    if (adapter->type != xemac_type_emacps || adapter->state == 0) return -1;
    ps = (xemacpsif_s *)adapter->state;
    if (ps->eth_link_status != ETH_LINK_UP) return 0;
    if (phyaddrforemac > 31) return -1;
    mac = &ps->emacps;
    if (mac->IsReady != XIL_COMPONENT_IS_READY) return -1;
    if (XEmacPs_PhyRead(mac, phyaddrforemac, 2, &id1) != XST_SUCCESS ||
        XEmacPs_PhyRead(mac, phyaddrforemac, 3, &id2) != XST_SUCCESS)
        return -1;
    if (id1 != 0x0022U || (id2 & 0xfff0U) != 0x1620U) return -1;
    result = aragatz_ksz9031_read_speed(mac, phyaddrforemac, &speed);
    if (result != 1) return result == 0 ? 0 : -1;
    *mbps = speed;
    return result;
}

int aragatz_phy_scan(XEmacPs *mac)
{
    unsigned addr;
    int found = 0;
    u16 id1, id2;
    if (mac == 0) return -1;
    for (addr = 0; addr < 32; ++addr) {
        if (XEmacPs_PhyRead(mac, addr, 2, &id1) != XST_SUCCESS ||
            XEmacPs_PhyRead(mac, addr, 3, &id2) != XST_SUCCESS) {
            xil_printf("MDIO addr=%d read error\r\n", addr);
            continue;
        }
        if ((id1 == 0xffffU && id2 == 0xffffU) ||
            (id1 == 0U && id2 == 0U)) continue;
        ++found;
        xil_printf("MDIO addr=%d ID=%04x:%04x%s\r\n", addr, id1, id2,
                   id1 == 0x0022U && (id2 & 0xfff0U) == 0x1620U
                       ? " KSZ9031" : " UNKNOWN");
    }
    return found;
}

int aragatz_phy_dump(XEmacPs *mac, unsigned phy_addr)
{
    static const unsigned regs[] = {0, 1, 2, 3, 4, 5, 9, 10, 31};
    u16 values[sizeof(regs) / sizeof(regs[0])], discarded;
    unsigned i, speed;
    if (mac == 0 || phy_addr > 31) return -1;
    for (i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i) {
        if (regs[i] == 1 &&
            XEmacPs_PhyRead(mac, phy_addr, 1, &discarded) != XST_SUCCESS)
            return -1;
        if (XEmacPs_PhyRead(mac, phy_addr, regs[i], &values[i]) != XST_SUCCESS) {
            xil_printf("PHY addr=%d reg=%d MDIO error\r\n", phy_addr, regs[i]);
            return -1;
        }
        xil_printf("PHY addr=%d reg=0x%02x value=0x%04x\r\n",
                   phy_addr, regs[i], values[i]);
    }
    if (values[2] != 0x0022U || (values[3] & 0xfff0U) != 0x1620U) {
        xil_printf("PHY is not a verified KSZ9031; status decode skipped\r\n");
        return -1;
    }
    switch (values[8] & 0x0070U) {
    case 0x0040U: speed = 1000; break;
    case 0x0020U: speed = 100; break;
    case 0x0010U: speed = 10; break;
    default: speed = 0; break;
    }
    xil_printf("KSZ9031 link=%d autoneg_done=%d speed_Mbps=%d full_duplex=%d\r\n",
               !!(values[1] & 4U), !!(values[1] & 0x20U), speed,
               !!(values[8] & 8U));
    return (values[0] & 0xDE00U) == 0x1000U &&
               (values[1] & 0x24U) == 0x24U &&
               (values[8] & 9U) == 8U && speed
               ? 0 : 1;
}

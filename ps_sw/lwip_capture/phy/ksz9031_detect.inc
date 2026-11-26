/* Aragatz has one RGMII KSZ9031 on the selected GEM MDIO bus.
 * Scan both identifier registers at addresses 0..31; never treat address 0
 * as a broadcast fallback. Discovery performs no PHY writes.
 */
void detect_phy(XEmacPs *mac)
{
	u32_t addr, selected = 32U, found = 0U, failed = 0U;
	u32_t *map = mac->Config.BaseAddress == XPAR_XEMACPS_0_BASEADDR
		? phymapemac0 : phymapemac1;
	u16_t id1, id2;
	for (addr = 0U; addr < 32U; ++addr)
		map[addr] = FALSE;
	for (addr = 0U; addr < 32U; ++addr) {
		if (XEmacPs_PhyRead(mac, addr, 2U, &id1) != XST_SUCCESS ||
		    XEmacPs_PhyRead(mac, addr, 3U, &id2) != XST_SUCCESS) {
			failed = 1U;
			xil_printf("MDIO discovery failed at address %d\r\n", addr);
			continue;
		}
		if (id1 == ARAGATZ_KSZ9031_ID1 &&
		    (id2 & ARAGATZ_KSZ9031_ID2_MASK) == ARAGATZ_KSZ9031_ID2) {
			selected = addr;
			++found;
			xil_printf("KSZ9031 detected: address %d ID %04x:%04x\r\n",
				addr, id1, id2);
		} else if (!((id1 == 0U && id2 == 0U) ||
			     (id1 == 0xFFFFU && id2 == 0xFFFFU))) {
			xil_printf("Ignoring non-KSZ9031 PHY: address %d ID %04x:%04x\r\n",
				addr, id1, id2);
		}
	}
	if (found == 1U && failed == 0U)
		map[selected] = TRUE;
	else
		xil_printf("KSZ9031 discovery rejected: count=%d MDIO_error=%d\r\n",
			found, failed);
}

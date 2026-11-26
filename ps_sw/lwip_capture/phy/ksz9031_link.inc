/* Main-context link polling. The helper reads status without writes/sleeps;
 * phy_setup_emacps reuses its configured state after startup.
 */
void emacps_link_status(struct netif *netif, xemacpsif_s *xemacs, XEmacPs *xemacp)
{
	u32_t detected_speed = 0U, new_link_speed;
	int status;
	if (xemacp->IsReady != (u32)XIL_COMPONENT_IS_READY ||
	    xemacs->eth_link_status == ETH_LINK_UNDEFINED || phyaddrforemac > 31U)
		return;
	status = aragatz_ksz9031_read_speed(xemacp, phyaddrforemac, &detected_speed);
	if (status != 1) {
		if (xemacs->eth_link_status == ETH_LINK_UP)
			xil_printf("Ethernet Link down or invalid PHY status\r\n");
		netif_set_link_down(netif);
		xemacs->eth_link_status = ETH_LINK_NEGOTIATING;
		return;
	}
	if (xemacs->eth_link_status == ETH_LINK_UP && link_speed == detected_speed)
		return;
	new_link_speed = phy_setup_emacps(xemacp, phyaddrforemac);
	if (new_link_speed != 10U && new_link_speed != 100U && new_link_speed != 1000U) {
		netif_set_link_down(netif);
		xemacs->eth_link_status = ETH_LINK_NEGOTIATING;
		return;
	}
	link_speed = new_link_speed;
	XEmacPs_SetOperatingSpeed(xemacp, new_link_speed);
	netif_set_link_up(netif);
	xemacs->eth_link_status = ETH_LINK_UP;
	xil_printf("Ethernet Link up: %d Mbps full duplex\r\n", new_link_speed);
}

/* Inserted by apply_adapter_patch.py into the 2023.1 PS GEM adapter.
 * Register definitions: Microchip DS00002117K, sections 4.1/4.2.
 * Asymmetric pause is disabled: DS80000692D, module 1.
 * Board reset, MMD skew, reference clock and master/slave settings are untouched.
 */
#if !defined(CONFIG_LINKSPEED_AUTODETECT)
#error "Aragatz KSZ9031 adapter requires linkspeed=autodetect"
#endif
#if defined(SGMII_FIXED_LINK) || XPAR_GIGE_PCS_PMA_1000BASEX_CORE_PRESENT == 1 || XPAR_GIGE_PCS_PMA_SGMII_CORE_PRESENT == 1
#error "Aragatz KSZ9031 overlay is for one RGMII PHY, not PCS/PMA/SGMII"
#endif

#define ARAGATZ_KSZ9031_ID1 0x0022U
#define ARAGATZ_KSZ9031_ID2 0x1620U
#define ARAGATZ_KSZ9031_ID2_MASK 0xFFF0U

#if defined(CONFIG_LINKSPEED_AUTODETECT)
/* 0: first attempt; 1: configured; 2: setup failed, restart application.
 * This avoids re-running a blocking negotiation from the stock link timer ISR.
 * The Aragatz application uses one GEM/one PHY. No concurrent MDIO callers.
 */
static u8_t aragatz_ksz9031_state[2][32];

/* Returns 1 when ready, 0 while negotiating/down, -1 on MDIO failure,
 * -2 on an invalid speed or half-duplex result. No writes or delays here.
 */
int aragatz_ksz9031_read_speed(XEmacPs *mac, u32_t addr, u32_t *speed)
{
	u16_t bmsr, mode, control;
	if (addr > 31U || speed == 0)
		return -1;
	*speed = 0U;
	if (XEmacPs_PhyRead(mac, addr, 0U, &control) != XST_SUCCESS ||
	    XEmacPs_PhyRead(mac, addr, 1U, &bmsr) != XST_SUCCESS ||
	    XEmacPs_PhyRead(mac, addr, 1U, &bmsr) != XST_SUCCESS)
		return -1;
	/* Do not accept the previous link while restart/reset is still pending. */
	if ((control & 0x8200U) != 0U)
		return 0;
	/* Reset, loopback, power-down, isolation or disabled auto-negotiation
	 * must not be mistaken for a valid result from stale status bits. */
	if ((control & 0x4C00U) != 0U || (control & 0x1000U) == 0U)
		return -2;
	/* BMSR link is latched low: the second read gives current state. */
	if ((bmsr & 0x0024U) != 0x0024U)
		return 0;
	if (XEmacPs_PhyRead(mac, addr, 0x1FU, &mode) != XST_SUCCESS)
		return -1;
	if ((mode & 0x0008U) == 0U || (mode & 0x0001U) != 0U)
		return -2;
	switch (mode & 0x0070U) {
	case 0x0040U: *speed = 1000U; break;
	case 0x0020U: *speed = 100U; break;
	case 0x0010U: *speed = 10U; break;
	default: return -2;
	}
	return 1;
}

static u32_t get_KSZ9031_phy_speed(XEmacPs *mac, u32_t addr)
{
	u16_t control, advertise, advertise1000;
	u32_t speed = 0U;
	u32_t emacnum, attempt, attempts;
	int result;
	if (addr > 31U)
		return XST_FAILURE;
	emacnum = mac->Config.BaseAddress == XPAR_XEMACPS_0_BASEADDR ? 0U : 1U;
	if (aragatz_ksz9031_state[emacnum][addr] == 2U)
		return XST_FAILURE;
	attempts = 1U;
	if (aragatz_ksz9031_state[emacnum][addr] == 0U) {
		/* Fail closed if any configuration operation fails. */
		aragatz_ksz9031_state[emacnum][addr] = 2U;
		if (XEmacPs_PhyRead(mac, addr, 0U, &control) != XST_SUCCESS ||
		    XEmacPs_PhyRead(mac, addr, 4U, &advertise) != XST_SUCCESS ||
		    XEmacPs_PhyRead(mac, addr, 9U, &advertise1000) != XST_SUCCESS) {
			xil_printf("KSZ9031 MDIO read failed during setup\r\n");
			return XST_FAILURE;
		}
		/* Do not guess reset/power/loopback/isolation handling. */
		if ((control & 0xCC00U) != 0U || (advertise & 0x001FU) != 1U) {
			xil_printf("KSZ9031 bad initial BMCR/ANAR: %04x/%04x\r\n",
				control, advertise);
			return XST_FAILURE;
		}
		/* Advertise 10/100/1000 full duplex only. Keep symmetric pause,
		 * existing master/slave policy and all unrelated fields.
		 * Disable asymmetric pause per DS80000692D module 1.
		 */
		advertise = (u16_t)((advertise & ~0x09E0U) | 0x0140U);
		advertise1000 = (u16_t)((advertise1000 & ~0x0300U) | 0x0200U);
		if (XEmacPs_PhyWrite(mac, addr, 4U, advertise) != XST_SUCCESS ||
		    XEmacPs_PhyWrite(mac, addr, 9U, advertise1000) != XST_SUCCESS ||
		    XEmacPs_PhyWrite(mac, addr, 0U,
			(u16_t)(control | 0x1200U)) != XST_SUCCESS) {
			xil_printf("KSZ9031 MDIO write failed during setup\r\n");
			return XST_FAILURE;
		}
		aragatz_ksz9031_state[emacnum][addr] = 1U;
		attempts = 100U; /* <= 10 s startup wait, 100 ms between polls. */
		xil_printf("KSZ9031: auto-negotiation, full duplex only\r\n");
	}
	for (attempt = 0U; attempt < attempts; ++attempt) {
		result = aragatz_ksz9031_read_speed(mac, addr, &speed);
		if (result == 1)
			return speed;
		if (result < 0) {
			xil_printf("KSZ9031 status error %d (MDIO or duplex/speed)\r\n", result);
			return XST_FAILURE;
		}
		if (attempts > 1U)
			usleep(100000U);
	}
	if (attempts > 1U)
		xil_printf("KSZ9031 link timeout; link monitor will retry status\r\n");
	return XST_FAILURE;
}
#endif

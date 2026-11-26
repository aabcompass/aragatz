/*
 * Copyright (C) 2010 - 2022 Xilinx, Inc.
 * Copyright (C) 2022 - 2023 Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 */

/* Exact function from Xilinx/embeddedsw xilinx_v2023.1,
 * xemacpsif_hw.c blob 5f13438759251827cbf3f2aa0dd9c1065eb726b6.
 */
void init_emacps_on_error (xemacpsif_s *xemacps, struct netif *netif)
{
	XEmacPs *xemacpsp;
	s32_t status = XST_SUCCESS;

	xemacpsp = &xemacps->emacps;

	/* set mac address */
	status = XEmacPs_SetMacAddress(xemacpsp, (void*)(netif->hwaddr), 1);
	if (status != XST_SUCCESS) {
		LWIP_DEBUGF(NETIF_DEBUG, ("In %s:Emac Mac Address set failed...\r\n",__func__));
	}

	XEmacPs_SetOperatingSpeed(xemacpsp, link_speed);

	/* Setting the operating speed of the MAC needs a delay. */
	{
		volatile s32_t wait;
		for (wait=0; wait < 20000; wait++);
	}
}

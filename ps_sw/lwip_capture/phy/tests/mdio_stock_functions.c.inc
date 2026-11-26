/******************************************************************************
* Copyright (C) 2009 - 2020 Xilinx, Inc.  All rights reserved.
* SPDX-License-Identifier: MIT
******************************************************************************/

/*****************************************************************************/
/**
 *
 * @file xemacps_control.c
* @addtogroup emacps_v3_16
* @{
 *
 * Functions in this file implement general purpose command and control related
 * functionality. See xemacps.h for a detailed description of the driver.
 *
 * <pre>
 * MODIFICATION HISTORY:
 *
 * Ver   Who  Date     Changes
 * ----- ---- -------- -------------------------------------------------------
 * 1.00a wsy  01/10/10 First release
 * 1.02a asa  11/05/12 Added a new API for deleting an entry from the HASH
 *					   register. Added a new API for setting the BURST length
 *					   in DMACR register.
 * 2.1   srt  07/15/14 Add support for Zynq Ultrascale Mp architecture.
 * 3.0   kvn  02/13/15 Modified code for MISRA-C:2012 compliance.
 * 3.0   hk   02/20/15 Added support for jumbo frames.
 * 3.2   hk   02/22/16 Added SGMII support for Zynq Ultrascale+ MPSoC.
 * </pre>
 *****************************************************************************/

/***************************** Include Files *********************************/

/* Host-test fixture: exact functions extracted from xilinx_v2023.1
 * XilinxProcessorIPLib/drivers/emacps/src/xemacps_control.c,
 * upstream Git blob 4c72f20e2154ae647e2b4f79d891ec8681fd4afb.
 * This is not a complete driver and must not be compiled into the target.
 */
LONG XEmacPs_PhyRead(XEmacPs *InstancePtr, u32 PhyAddress,
		     u32 RegisterNum, u16 *PhyDataPtr)
{
	u32 Mgtcr;
	volatile u32 Ipisr;
	u32 IpReadTemp;
	LONG Status;

	Xil_AssertNonvoid(InstancePtr != NULL);

	/* Make sure no other PHY operation is currently in progress */
	if ((!(XEmacPs_ReadReg(InstancePtr->Config.BaseAddress,
				XEMACPS_NWSR_OFFSET) &
	      XEMACPS_NWSR_MDIOIDLE_MASK))==TRUE) {
		Status = (LONG)(XST_EMAC_MII_BUSY);
	} else {

	/* Construct Mgtcr mask for the operation */
	Mgtcr = XEMACPS_PHYMNTNC_OP_MASK | XEMACPS_PHYMNTNC_OP_R_MASK |
			(PhyAddress << XEMACPS_PHYMNTNC_PHAD_SHFT_MSK) |
			(RegisterNum << XEMACPS_PHYMNTNC_PREG_SHFT_MSK);

	/* Write Mgtcr and wait for completion */
	XEmacPs_WriteReg(InstancePtr->Config.BaseAddress,
			   XEMACPS_PHYMNTNC_OFFSET, Mgtcr);

	do {
		Ipisr = XEmacPs_ReadReg(InstancePtr->Config.BaseAddress,
					  XEMACPS_NWSR_OFFSET);
			IpReadTemp = Ipisr;
		} while ((IpReadTemp & XEMACPS_NWSR_MDIOIDLE_MASK) == 0x00000000U);

	/* Read data */
		*PhyDataPtr = (u16)XEmacPs_ReadReg(InstancePtr->Config.BaseAddress,
					XEMACPS_PHYMNTNC_OFFSET);
		Status = (LONG)(XST_SUCCESS);
	}
	return Status;
}

LONG XEmacPs_PhyWrite(XEmacPs *InstancePtr, u32 PhyAddress,
		      u32 RegisterNum, u16 PhyData)
{
	u32 Mgtcr;
	volatile u32 Ipisr;
	u32 IpWriteTemp;
	LONG Status;

	Xil_AssertNonvoid(InstancePtr != NULL);

	/* Make sure no other PHY operation is currently in progress */
	if ((!(XEmacPs_ReadReg(InstancePtr->Config.BaseAddress,
				XEMACPS_NWSR_OFFSET) &
	      XEMACPS_NWSR_MDIOIDLE_MASK))==TRUE) {
		Status = (LONG)(XST_EMAC_MII_BUSY);
	} else {
	/* Construct Mgtcr mask for the operation */
	Mgtcr = XEMACPS_PHYMNTNC_OP_MASK | XEMACPS_PHYMNTNC_OP_W_MASK |
			(PhyAddress << XEMACPS_PHYMNTNC_PHAD_SHFT_MSK) |
			(RegisterNum << XEMACPS_PHYMNTNC_PREG_SHFT_MSK) | (u32)PhyData;

	/* Write Mgtcr and wait for completion */
	XEmacPs_WriteReg(InstancePtr->Config.BaseAddress,
			   XEMACPS_PHYMNTNC_OFFSET, Mgtcr);

	do {
		Ipisr = XEmacPs_ReadReg(InstancePtr->Config.BaseAddress,
					  XEMACPS_NWSR_OFFSET);
				IpWriteTemp = Ipisr;
		} while ((IpWriteTemp & XEMACPS_NWSR_MDIOIDLE_MASK) == 0x00000000U);

		Status = (LONG)(XST_SUCCESS);
	}
	return Status;
}

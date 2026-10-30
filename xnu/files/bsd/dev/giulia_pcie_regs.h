/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * PCIe0 / WCN7850 constants for giulia (OnePlus Ace 5, SM8650).
 *
 * Generated from dtb/giulia.dtb by tools/ace5/pcie-regs.py. The register
 * windows, pins and the PHY/SM sequences are this board's vendor data; the
 * GCC offsets at the end are from Linux drivers/clk/qcom/gcc-sm8650.c
 * (GPL-2.0), kept here rather than in the APSL driver.
 */
#ifndef GIULIA_PCIE_REGS_H
#define GIULIA_PCIE_REGS_H

#define GIULIA_PCIE_PARF             0x01c00000u
#define GIULIA_PCIE_PARF_SIZE        0x00003000u
#define GIULIA_PCIE_PHY              0x01c06000u
#define GIULIA_PCIE_PHY_SIZE         0x00002000u
#define GIULIA_PCIE_DM_CORE          0x60000000u
#define GIULIA_PCIE_DM_CORE_SIZE     0x00000f1du
#define GIULIA_PCIE_ELBI             0x60000f20u
#define GIULIA_PCIE_ELBI_SIZE        0x000000a8u
#define GIULIA_PCIE_IATU             0x60001000u
#define GIULIA_PCIE_IATU_SIZE        0x00001000u
#define GIULIA_PCIE_CONF             0x60100000u
#define GIULIA_PCIE_CONF_SIZE        0x00100000u
#define GIULIA_PCIE_PCIE_SM          0x01d07000u
#define GIULIA_PCIE_PCIE_SM_SIZE     0x00007000u

/* Root-complex window: io then mem, from the node's ranges. */
#define GIULIA_PCIE_IO_PA         0x60200000u
#define GIULIA_PCIE_IO_SIZE        0x00100000u
#define GIULIA_PCIE_MEM_PA         0x60300000u
#define GIULIA_PCIE_MEM_SIZE        0x03d00000u

#define GIULIA_PCIE_PERST_GPIO      94u
#define GIULIA_PCIE_CLKREQ_GPIO     95u
#define GIULIA_PCIE_WAKE_GPIO       96u
#define GIULIA_CNSS_WLAN_EN_GPIO    16u
#define GIULIA_CNSS_BT_EN_GPIO      17u
#define GIULIA_CNSS_SW_CTRL_GPIO    18u
#define GIULIA_CNSS_WLAN_SW_GPIO    19u
#define GIULIA_CNSS_QMP_MBOX        63u

#define GIULIA_PCIE_PHY_STATUS      0x214u /* bit 6 = PHY ready */
#define GIULIA_PCIE_PHY_STATUS_BIT  6u
#define GIULIA_PCIE_PHY_PWRDOWN     0x240u
#define GIULIA_PCIE_EP_LATENCY_MS   10u
#define GIULIA_PCIE_SMMU_SID_BASE   0x1400u

struct giulia_pcie_phy_step {
	uint32_t offset;
	uint32_t value;
	uint32_t delay_us;
};

/* 115 steps from the vendor qcom,phy-sequence. */
static const struct giulia_pcie_phy_step giulia_pcie_phy[] = {
	{ 0x240, 0x03, 0 },
	{ 0x0c0, 0x01, 0 },
	{ 0x0cc, 0x62, 0 },
	{ 0x0d0, 0x02, 0 },
	{ 0x060, 0xf8, 0 },
	{ 0x064, 0x01, 0 },
	{ 0x000, 0x93, 0 },
	{ 0x004, 0x01, 0 },
	{ 0x0e0, 0x90, 0 },
	{ 0x0e4, 0x82, 0 },
	{ 0x0f4, 0x07, 0 },
	{ 0x070, 0x02, 0 },
	{ 0x010, 0x02, 0 },
	{ 0x074, 0x16, 0 },
	{ 0x014, 0x16, 0 },
	{ 0x078, 0x36, 0 },
	{ 0x018, 0x36, 0 },
	{ 0x110, 0x08, 0 },
	{ 0x0bc, 0x0a, 0 },
	{ 0x120, 0x42, 0 },
	{ 0x080, 0x04, 0 },
	{ 0x084, 0x0d, 0 },
	{ 0x020, 0x0a, 0 },
	{ 0x024, 0x1a, 0 },
	{ 0x088, 0x41, 0 },
	{ 0x028, 0x34, 0 },
	{ 0x090, 0xab, 0 },
	{ 0x094, 0xaa, 0 },
	{ 0x098, 0x01, 0 },
	{ 0x030, 0x55, 0 },
	{ 0x034, 0x55, 0 },
	{ 0x038, 0x01, 0 },
	{ 0x140, 0x14, 0 },
	{ 0x164, 0x34, 0 },
	{ 0x03c, 0x01, 0 },
	{ 0x01c, 0x04, 0 },
	{ 0x174, 0x16, 0 },
	{ 0x1bc, 0x0f, 0 },
	{ 0x170, 0xa0, 0 },
	{ 0x11a4, 0x38, 0 },
	{ 0x10dc, 0x11, 0 },
	{ 0x1160, 0xbf, 0 },
	{ 0x1164, 0xbf, 0 },
	{ 0x1168, 0xb7, 0 },
	{ 0x116c, 0xea, 0 },
	{ 0x115c, 0x3f, 0 },
	{ 0x1174, 0x5c, 0 },
	{ 0x1178, 0x9c, 0 },
	{ 0x117c, 0x1a, 0 },
	{ 0x1180, 0x89, 0 },
	{ 0x1170, 0xdc, 0 },
	{ 0x1188, 0x94, 0 },
	{ 0x118c, 0x5b, 0 },
	{ 0x1190, 0x1a, 0 },
	{ 0x1194, 0x89, 0 },
	{ 0x10cc, 0x00, 0 },
	{ 0x1008, 0x09, 0 },
	{ 0x1014, 0x05, 0 },
	{ 0x104c, 0x08, 0 },
	{ 0x1050, 0x08, 0 },
	{ 0x10d8, 0x0f, 0 },
	{ 0x1118, 0x1c, 0 },
	{ 0x10f8, 0x07, 0 },
	{ 0x11f8, 0x08, 0 },
	{ 0x1600, 0x00, 0 },
	{ 0xe84, 0x15, 0 },
	{ 0xe90, 0x3f, 0 },
	{ 0xee4, 0x02, 0 },
	{ 0xe40, 0x09, 0 },
	{ 0xe3c, 0x15, 0 },
	{ 0x19a4, 0x38, 0 },
	{ 0x18dc, 0x11, 0 },
	{ 0x1960, 0xbf, 0 },
	{ 0x1964, 0xbf, 0 },
	{ 0x1968, 0xb7, 0 },
	{ 0x196c, 0xea, 0 },
	{ 0x195c, 0x3f, 0 },
	{ 0x1974, 0x5c, 0 },
	{ 0x1978, 0x9c, 0 },
	{ 0x197c, 0x1a, 0 },
	{ 0x1980, 0x89, 0 },
	{ 0x1970, 0xdc, 0 },
	{ 0x1988, 0x94, 0 },
	{ 0x198c, 0x5b, 0 },
	{ 0x1990, 0x1a, 0 },
	{ 0x1994, 0x89, 0 },
	{ 0x18cc, 0x00, 0 },
	{ 0x1808, 0x09, 0 },
	{ 0x1814, 0x05, 0 },
	{ 0x184c, 0x08, 0 },
	{ 0x1850, 0x08, 0 },
	{ 0x18d8, 0x0f, 0 },
	{ 0x1918, 0x1c, 0 },
	{ 0x18f8, 0x07, 0 },
	{ 0x19f8, 0x08, 0 },
	{ 0x1684, 0x15, 0 },
	{ 0x1690, 0x3f, 0 },
	{ 0x16e4, 0x02, 0 },
	{ 0x1640, 0x09, 0 },
	{ 0x163c, 0x15, 0 },
	{ 0x2dc, 0x05, 0 },
	{ 0x388, 0x77, 0 },
	{ 0x398, 0x0b, 0 },
	{ 0x6a4, 0x1e, 0 },
	{ 0x6f4, 0x27, 0 },
	{ 0x3e0, 0x0f, 0 },
	{ 0x60c, 0x1d, 0 },
	{ 0x614, 0x07, 0 },
	{ 0x620, 0xc1, 0 },
	{ 0x694, 0x00, 0 },
	{ 0x3d0, 0x8c, 0 },
	{ 0x368, 0x17, 0 },
	{ 0x370, 0x2e, 0 },
	{ 0x200, 0x00, 0 },
	{ 0x244, 0x03, 0 },
};

/* qcom,pcie-sm-seq: written to the SM window before link training. */
static const uint32_t giulia_pcie_sm_seq[] = {
	0x1c018081, 0x70074002, 0x50028000, 0x28007003, 0x80804002, 0x70021c01, 0x18001802, 0x70005000,
	0x10004000, 0x80814002, 0x18001c01, 0x1c018080, 0x00000100,
};

#define GIULIA_PCIE_SM_START_OFFSET 0x1090u
#define GIULIA_PCIE_SM_BRANCH_OFFSET 0x1000u
static const uint32_t giulia_pcie_sm_branch[] = {
	0x4, 0x1c, 0x24, 0x2c, 0x0, 0x0, 0x0,
};

/* Linux drivers/clk/qcom/gcc-sm8650.c: PCIe0 branches, GDSCs and resets. */
#define GIULIA_GCC_PCIE0_GDSCR        0x6b004u /* bit 0 on, bit 31 status */
#define GIULIA_GCC_PCIE0_PHY_GDSCR    0x6c000u
#define GIULIA_GCC_PCIE_VOTE          0x52008u /* bits 0-5: slv,mstr,ahb,aux,pipe,q2a */
#define GIULIA_GCC_PCIE0_SLV_HALT     0x6b020u
#define GIULIA_GCC_PCIE0_MSTR_HALT    0x6b02cu
#define GIULIA_GCC_PCIE0_AHB_HALT     0x6b038u
#define GIULIA_GCC_PCIE0_AUX_HALT     0x6b03cu
#define GIULIA_GCC_PCIE0_Q2A_HALT     0x6b01cu
#define GIULIA_GCC_PCIE0_PIPE_HALT    0x6b048u
#define GIULIA_GCC_PCIE0_CORE_BCR     0x6b000u
#define GIULIA_GCC_PCIE0_PHY_BCR      0x6c01cu

#endif /* GIULIA_PCIE_REGS_H */

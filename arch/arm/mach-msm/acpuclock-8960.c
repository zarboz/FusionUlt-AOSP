/* Copyright (c) 2011-2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/regulator/consumer.h>

#include <asm/mach-types.h>
#include <asm/cpu.h>

#include <mach/board.h>
#include <mach/msm_iomap.h>
#include <mach/rpm-regulator.h>
#include <mach/msm_bus.h>
#include <mach/msm_bus_board.h>
#include <mach/socinfo.h>
#include <mach/msm-krait-l2-accessors.h>
#include <mach/rpm-regulator.h>

#include "acpuclock.h"
#include <mach/board_htc.h>

#ifdef pr_err
#undef pr_err
#endif
#define pr_err(fmt, args...) \
	printk(KERN_ERR "[ACPU] " pr_fmt(fmt), ## args)

#ifdef pr_warning
#undef pr_warning
#endif
#define pr_warning(fmt, args...) \
	printk(KERN_WARNING "[ACPU] " pr_fmt(fmt), ## args)

#ifdef pr_info
#undef pr_info
#endif
#define pr_info(fmt, args...) \
	printk(KERN_INFO "[ACPU] " pr_fmt(fmt), ## args)

#if defined(DEBUG)
#ifdef pr_debug
#undef pr_debug
#endif
#define pr_debug(fmt, args...) \
	printk(KERN_DEBUG "[ACPU] " pr_fmt(fmt), ## args)
#endif

/*
PHY define in msm_iomap-8960.h, VIRT define in msm_iomap.h
The counters to check kernel exit for both cpu's
kernel foot print for cpu0  		: phy 0x889F1000 : virt 0xFE703000
kernel foot print for cpu1  		: phy 0x889F1004 : virt 0xFE703004
kernel exit counter from cpu0		: phy 0x889F1008 : virt 0xFE703008
kernel exit counter from cpu1		: phy 0x889F100C : virt 0xFE70300C
msm_pm_boot_entry			: phy 0x889F1010 : virt 0xFE703010
msm_pm_boot_vector			: phy 0x889F1014 : virt 0xFE703014
reset vector for cpu0(init)		: phy 0x889F1018 : virt 0xFE703018
reset vector for cpu1(init)		: phy 0x889F101C : virt 0xFE70301C
cpu0 reset vector address		: phy 0x889F1020 : virt 0xFE703020
cpu1 reset vector address		: phy 0x889F1024 : virt 0xFE703024
cpu0 reset vector address value		: phy 0x889F1028 : virt 0xFE703028
cpu1 reset vector address value		: phy 0x889F102C : virt 0xFE70302C
cpu0 frequency				: phy 0x889F1030 : virt 0xFE703030
cpu1 frequency				: phy 0x889F1034 : virt 0xFE703034
L2 frequency				: phy 0x889F1038 : virt 0xFE703038
acpuclk_set_rate footprint cpu0		: phy 0x889F103C : virt 0xFE70303C
acpuclk_set_rate footprint cpu1		: phy 0x889F1040 : virt 0xFE703040
*/
#define CPU_FOOT_PRINT_MAGIC				0xACBDFE00
#define CPU_FOOT_PRINT_BASE_CPU0_VIRT		(MSM_KERNEL_FOOTPRINT_BASE + 0x0)
static void set_acpuclk_foot_print(unsigned cpu, unsigned state)
{
	unsigned *status = (unsigned *)(CPU_FOOT_PRINT_BASE_CPU0_VIRT + 0x3C) + cpu;
	*status = (CPU_FOOT_PRINT_MAGIC | state);
	mb();
}

static void set_acpuclk_cpu_freq_foot_print(unsigned cpu, unsigned khz)
{
	unsigned *status = (unsigned *)(CPU_FOOT_PRINT_BASE_CPU0_VIRT + 0x30) + cpu;
	*status = khz;
	mb();
}

static void set_acpuclk_L2_freq_foot_print(unsigned khz)
{
	unsigned *status = (unsigned *)(CPU_FOOT_PRINT_BASE_CPU0_VIRT + 0x38);
	*status = khz;
	mb();
}

/*
 * Source IDs.
 * These must be negative to not overlap with the source IDs
 * used by the 8x60 local clock driver.
 */
#define PLL_8			 0
#define HFPLL			-1
#define QSB			-2

/* Mux source selects. */
#define PRI_SRC_SEL_SEC_SRC	0
#define PRI_SRC_SEL_HFPLL	1
#define PRI_SRC_SEL_HFPLL_DIV2	2
#define SEC_SRC_SEL_QSB		0
#define SEC_SRC_SEL_AUX		2

/* HFPLL registers offsets. */
#define HFPLL_MODE		0x00
#define HFPLL_CONFIG_CTL	0x04
#define HFPLL_L_VAL		0x08
#define HFPLL_M_VAL		0x0C
#define HFPLL_N_VAL		0x10
#define HFPLL_DROOP_CTL		0x14

/* CP15 L2 indirect addresses. */
#define L2CPMR_IADDR		0x500
#define L2CPUCPMR_IADDR		0x501

#define STBY_KHZ		1

#define MAX_VDD_SC    1350000 /* uV */	
#define MIN_VDD_SC     400000 /* uV */
#define HFPLL_NOMINAL_VDD	1100000
#define HFPLL_LOW_VDD		800000
#define HFPLL_LOW_VDD_PLL_L_MAX	0x28

#define SECCLKAGD		BIT(4)

/* PTE EFUSE register. */
#define QFPROM_PTE_EFUSE_ADDR	(MSM_QFPROM_BASE + 0x00C0)

/* HTC: Custom max frequency. */
#ifdef CONFIG_ACPU_CUSTOM_FREQ_SUPPORT
static int acpu_max_freq = CONFIG_ACPU_MAX_FREQ;
#else
static int acpu_max_freq = 0;
#endif

enum scalables {
	CPU0 = 0,
	CPU1,
	CPU2,
	CPU3,
	L2,
	NUM_SCALABLES
};

enum vregs {
	VREG_CORE,
	VREG_MEM,
	VREG_DIG,
	VREG_HFPLL_A,
	VREG_HFPLL_B,
	NUM_VREG
};

struct vreg {
	const char name[15];
	const unsigned int max_vdd;
	const int rpm_vreg_voter;
	const int rpm_vreg_id;
	struct regulator *reg;
	unsigned int cur_vdd;
};

struct core_speed {
	unsigned int		khz;
	int			src;
	unsigned int		pri_src_sel;
	unsigned int		sec_src_sel;
	unsigned int		pll_l_val;
};

struct l2_level {
	struct core_speed	speed;
	unsigned int		vdd_dig;
	unsigned int		vdd_mem;
	unsigned int		bw_level;
};

struct acpu_level {
	unsigned int		use_for_scaling;
	struct core_speed	speed;
	struct l2_level		*l2_level;
	unsigned int		vdd_core;
};

struct scalable {
	void * __iomem const hfpll_base;
	void * __iomem const aux_clk_sel;
	const uint32_t l2cpmr_iaddr;
	struct core_speed *current_speed;
	struct l2_level *l2_vote;
	struct vreg vreg[NUM_VREG];
	bool first_set_call;
};

static struct scalable scalable_8960[] = {
	[CPU0] = {
			.hfpll_base      = MSM_HFPLL_BASE + 0x200,
			.aux_clk_sel     = MSM_ACC0_BASE  + 0x014,
			.l2cpmr_iaddr    = L2CPUCPMR_IADDR,
			.vreg[VREG_CORE] = { "krait0",     1350000 },
			.vreg[VREG_MEM]  = { "krait0_mem", 1150000,
					     RPM_VREG_VOTER1,
					     RPM_VREG_ID_PM8921_L24 },
			.vreg[VREG_DIG]  = { "krait0_dig", 1150000,
					     RPM_VREG_VOTER1,
					     RPM_VREG_ID_PM8921_S3 },
			.vreg[VREG_HFPLL_A] = { "hfpll", 2100000,
					     RPM_VREG_VOTER1,
					     RPM_VREG_ID_PM8921_S8 },
			.vreg[VREG_HFPLL_B] = { "hfpll", 1800000,
					     RPM_VREG_VOTER1,
					     RPM_VREG_ID_PM8921_L23 },
		},
	[CPU1] = {
			.hfpll_base      = MSM_HFPLL_BASE + 0x300,
			.aux_clk_sel     = MSM_ACC1_BASE  + 0x014,
			.l2cpmr_iaddr    = L2CPUCPMR_IADDR,
			.vreg[VREG_CORE] = { "krait1",     1350000 },
			.vreg[VREG_MEM]  = { "krait0_mem", 1150000,
					     RPM_VREG_VOTER2,
					     RPM_VREG_ID_PM8921_L24 },
			.vreg[VREG_DIG]  = { "krait0_dig", 1150000,
					     RPM_VREG_VOTER2,
					     RPM_VREG_ID_PM8921_S3 },
			.vreg[VREG_HFPLL_A] = { "hfpll", 2100000,
					     RPM_VREG_VOTER2,
					     RPM_VREG_ID_PM8921_S8 },
			.vreg[VREG_HFPLL_B] = { "hfpll", 1800000,
					     RPM_VREG_VOTER2,
					     RPM_VREG_ID_PM8921_L23 },
		},
	[L2] = {
			.hfpll_base   = MSM_HFPLL_BASE    + 0x400,
			.aux_clk_sel  = MSM_APCS_GCC_BASE + 0x028,
			.l2cpmr_iaddr = L2CPMR_IADDR,
			.vreg[VREG_HFPLL_A] = { "hfpll", 2100000,
					     RPM_VREG_VOTER6,
					     RPM_VREG_ID_PM8921_S8 },
			.vreg[VREG_HFPLL_B] = { "hfpll", 1800000,
					     RPM_VREG_VOTER6,
					     RPM_VREG_ID_PM8921_L23 },
		},
};

static DEFINE_MUTEX(driver_lock);
static DEFINE_SPINLOCK(l2_lock);

static struct scalable *scalable;
static struct l2_level *l2_freq_tbl;
static struct acpu_level *acpu_freq_tbl;
static int l2_freq_tbl_size;
static unsigned int max_vdd = 0;

/* Instantaneous bandwidth requests in MB/s. */
#define BW_MBPS(_bw) \
	{ \
		.vectors = (struct msm_bus_vectors[]){ \
			{\
				.src = MSM_BUS_MASTER_AMPSS_M0, \
				.dst = MSM_BUS_SLAVE_EBI_CH0, \
				.ib = (_bw) * 1000000UL, \
				.ab = (_bw) *  100000UL, \
			}, \
			{ \
				.src = MSM_BUS_MASTER_AMPSS_M1, \
				.dst = MSM_BUS_SLAVE_EBI_CH0, \
				.ib = (_bw) * 1000000UL, \
				.ab = (_bw) *  100000UL, \
			}, \
		}, \
		.num_paths = 2, \
	}
static struct msm_bus_paths bw_level_tbl[] = {
	[0] =  BW_MBPS(640), /* At least  80 MHz on bus. */
	[1] = BW_MBPS(1064), /* At least 133 MHz on bus. */
	[2] = BW_MBPS(1600), /* At least 200 MHz on bus. */
	[3] = BW_MBPS(2128), /* At least 266 MHz on bus. */
	[4] = BW_MBPS(3200), /* At least 400 MHz on bus. */
	[5] = BW_MBPS(3600), /* At least 450 MHz on bus. */
	[6] = BW_MBPS(3936), /* At least 492 MHz on bus. */
	[7] = BW_MBPS(4264), /* At least 533 MHz on bus. */
	[8] = BW_MBPS(4480), /* At least 550 MHz on bus. */
	[9] = BW_MBPS(4800), /* At least 600 MHz on bus. */
	[10] = BW_MBPS(5200), /* At least 650 MHz on bus. */
};

static struct msm_bus_scale_pdata bus_client_pdata = {
	.usecase = bw_level_tbl,
	.num_usecases = ARRAY_SIZE(bw_level_tbl),
	.active_only = 1,
	.name = "acpuclock",
};

static uint32_t bus_perf_client;

/* TODO: Update vdd_dig and vdd_mem when voltage data is available. */

#define L2(x) (&l2_freq_tbl_8960_kraitv2[(x)])
static struct l2_level l2_freq_tbl_8960_kraitv2[] = {
	[0]  = { {STBY_KHZ, QSB,   0, 0, 0x00 }, 1050000, 1050000, 0 },
	[1]  = { {  192000, PLL_8, 0, 2, 0x00 }, 1050000, 1050000, 1 },
	[2]  = { {  384000, HFPLL, 2, 0, 0x20 }, 1050000, 1050000, 2 },
	[3]  = { {  486000, HFPLL, 2, 0, 0x24 }, 1050000, 1050000, 2 },
	[4]  = { {  540000, HFPLL, 2, 0, 0x28 }, 1050000, 1050000, 2 },
	[5]  = { {  594000, HFPLL, 1, 0, 0x16 }, 1050000, 1050000, 2 },
	[6]  = { {  648000, HFPLL, 1, 0, 0x18 }, 1050000, 1050000, 4 },
	[7]  = { {  702000, HFPLL, 1, 0, 0x1A }, 1050000, 1050000, 4 },
	[8]  = { {  756000, HFPLL, 1, 0, 0x1C }, 1150000, 1150000, 4 },
	[9]  = { {  810000, HFPLL, 1, 0, 0x1E }, 1150000, 1150000, 4 },
	[10] = { {  864000, HFPLL, 1, 0, 0x20 }, 1150000, 1150000, 4 },
	[11] = { {  918000, HFPLL, 1, 0, 0x22 }, 1150000, 1150000, 6 },
	[12] = { {  972000, HFPLL, 1, 0, 0x24 }, 1150000, 1150000, 6 },
	[13] = { { 1026000, HFPLL, 1, 0, 0x26 }, 1150000, 1150000, 6 },
	[14] = { { 1080000, HFPLL, 1, 0, 0x28 }, 1150000, 1150000, 6 },
	[15] = { { 1134000, HFPLL, 1, 0, 0x2A }, 1150000, 1150000, 6 },
	[16] = { { 1188000, HFPLL, 1, 0, 0x2C }, 1150000, 1150000, 6 },
	[17] = { { 1242000, HFPLL, 1, 0, 0x2E }, 1150000, 1150000, 6 },
	[18] = { { 1350000, HFPLL, 1, 0, 0x30 }, 1150000, 1150000, 6 },
	[19] = { { 1458000, HFPLL, 1, 0, 0x32 }, 1150000, 1150000, 6 },
	[20] = { { 1512000, HFPLL, 1, 0, 0x34 }, 1150000, 1150000, 7 },
	[21] = { { 1674000, HFPLL, 1, 0, 0x36 }, 1150000, 1150000, 7 },
	[22] = { { 1728000, HFPLL, 1, 0, 0x36 }, 1150000, 1150000, 7 },
};

static struct acpu_level acpu_freq_tbl_8960_fusion_slow[] = {
	{ 0, { STBY_KHZ, QSB,   0, 0, 0x00 }, L2(0),   850000 },
	{ 1, {   192000, PLL_8, 0, 2, 0x00 }, L2(1),   900000 },
	{ 1, {   384000, HFPLL, 2, 0, 0x20 }, L2(7),   900000 },
	{ 1, {   486000, HFPLL, 2, 0, 0x24 }, L2(7),   950000 },
	{ 0, {   540000, HFPLL, 2, 0, 0x28 }, L2(7),  1000000 },
	{ 1, {   594000, HFPLL, 1, 0, 0x16 }, L2(7),  1000000 },
	{ 0, {   648000, HFPLL, 1, 0, 0x18 }, L2(7),  1025000 },
	{ 1, {   702000, HFPLL, 1, 0, 0x1A }, L2(7),  1025000 },
	{ 0, {   756000, HFPLL, 1, 0, 0x1C }, L2(7),  1075000 },
	{ 1, {   810000, HFPLL, 1, 0, 0x1E }, L2(7),  1075000 },
	{ 0, {   864000, HFPLL, 1, 0, 0x20 }, L2(7),  1100000 },
	{ 1, {   918000, HFPLL, 1, 0, 0x22 }, L2(7),  1100000 },
	{ 0, {   972000, HFPLL, 1, 0, 0x24 }, L2(7),  1125000 },
	{ 1, {  1026000, HFPLL, 1, 0, 0x26 }, L2(7),  1125000 },
	{ 0, {  1080000, HFPLL, 1, 0, 0x28 }, L2(16), 1175000 },
	{ 1, {  1134000, HFPLL, 1, 0, 0x2A }, L2(16), 1175000 },
	{ 0, {  1188000, HFPLL, 1, 0, 0x2C }, L2(16), 1200000 },
	{ 1, {  1242000, HFPLL, 1, 0, 0x2E }, L2(16), 1200000 },
	{ 0, {  1296000, HFPLL, 1, 0, 0x30 }, L2(16), 1225000 },
	{ 1, {  1350000, HFPLL, 1, 0, 0x32 }, L2(16), 1225000 },
	{ 0, {  1404000, HFPLL, 1, 0, 0x34 }, L2(16), 1237500 },
	{ 1, {  1458000, HFPLL, 1, 0, 0x36 }, L2(16), 1237500 },
	{ 1, {  1512000, HFPLL, 1, 0, 0x38 }, L2(18), 1250000 },
	{ 1, {  1674000, HFPLL, 1, 0, 0x3A }, L2(18), 1275000 },
	{ 1, {  1728000, HFPLL, 1, 0, 0x3C }, L2(19), 1300000 },
	{ 1, {  1809000, HFPLL, 1, 0, 0x3E }, L2(19), 1325000 },
	{ 1, {  1890000, HFPLL, 1, 0, 0x40 }, L2(20), 1350000 },
	{ 1, {  1998000, HFPLL, 1, 0, 0x40 }, L2(21), 1350000 },
	{ 1, {  2160000, HFPLL, 1, 0, 0x40 }, L2(21), 1350000 },
	{ 0, { 0 } }
};

static struct acpu_level acpu_freq_tbl_8960_fusion_nom[] = {
	{ 0, { STBY_KHZ, QSB,   0, 0, 0x00 }, L2(0),   800000 },
	{ 1, {   192000, PLL_8, 0, 2, 0x00 }, L2(1),   800000 },
	{ 1, {   384000, HFPLL, 2, 0, 0x20 }, L2(7),   850000 },
	{ 1, {   486000, HFPLL, 2, 0, 0x24 }, L2(7),   900000 },
	{ 0, {   540000, HFPLL, 2, 0, 0x28 }, L2(7),   950000 },
	{ 1, {   594000, HFPLL, 1, 0, 0x16 }, L2(7),   950000 },
	{ 0, {   648000, HFPLL, 1, 0, 0x18 }, L2(7),   975000 },
	{ 1, {   702000, HFPLL, 1, 0, 0x1A }, L2(7),   975000 },
	{ 0, {   756000, HFPLL, 1, 0, 0x1C }, L2(7),  1025000 },
	{ 1, {   810000, HFPLL, 1, 0, 0x1E }, L2(7),  1025000 },
	{ 0, {   864000, HFPLL, 1, 0, 0x20 }, L2(7),  1050000 },
	{ 1, {   918000, HFPLL, 1, 0, 0x22 }, L2(7),  1050000 },
	{ 0, {   972000, HFPLL, 1, 0, 0x24 }, L2(7),  1075000 },
	{ 1, {  1026000, HFPLL, 1, 0, 0x26 }, L2(7),  1075000 },
	{ 0, {  1080000, HFPLL, 1, 0, 0x28 }, L2(16), 1100000 },
	{ 1, {  1134000, HFPLL, 1, 0, 0x2A }, L2(16), 1125000 },
	{ 0, {  1188000, HFPLL, 1, 0, 0x2C }, L2(16), 1125000 },
	{ 1, {  1242000, HFPLL, 1, 0, 0x2E }, L2(16), 1150000 },
	{ 0, {  1296000, HFPLL, 1, 0, 0x30 }, L2(16), 1150000 },
	{ 1, {  1350000, HFPLL, 1, 0, 0x32 }, L2(16), 1175000 },
	{ 0, {  1404000, HFPLL, 1, 0, 0x34 }, L2(16), 1175000 },
	{ 1, {  1458000, HFPLL, 1, 0, 0x36 }, L2(16), 1187500 },
	{ 1, {  1512000, HFPLL, 1, 0, 0x38 }, L2(18), 1200000 },	
	{ 1, {  1674000, HFPLL, 1, 0, 0x3A }, L2(18), 1225000 },
	{ 1, {  1728000, HFPLL, 1, 0, 0x3C }, L2(19), 1250000 },
	{ 1, {  1809000, HFPLL, 1, 0, 0x3E }, L2(19), 1275000 },	
	{ 1, {  1890000, HFPLL, 1, 0, 0x40 }, L2(19), 1300000 },
	{ 1, {  1998000, HFPLL, 1, 0, 0x40 }, L2(21), 1325000 },
	{ 1, {  2160000, HFPLL, 1, 0, 0x40 }, L2(21), 1350000 },
	{ 0, { 0 } }
};

static struct acpu_level acpu_freq_tbl_8960_fusion_fast[] = {
	{ 0, { STBY_KHZ, QSB,   0, 0, 0x00 }, L2(0),   800000 },
	{ 1, {   192000, PLL_8, 0, 2, 0x00 }, L2(1),   800000 },
	{ 1, {   384000, HFPLL, 2, 0, 0x20 }, L2(7),   800000 },
	{ 1, {   486000, HFPLL, 2, 0, 0x24 }, L2(7),   850000 },
	{ 0, {   540000, HFPLL, 2, 0, 0x28 }, L2(7),   900000 },
	{ 1, {   594000, HFPLL, 1, 0, 0x16 }, L2(7),   900000 },
	{ 0, {   648000, HFPLL, 1, 0, 0x18 }, L2(7),   925000 },
	{ 1, {   702000, HFPLL, 1, 0, 0x1A }, L2(7),   925000 },
	{ 0, {   756000, HFPLL, 1, 0, 0x1C }, L2(7),   975000 },
	{ 1, {   810000, HFPLL, 1, 0, 0x1E }, L2(7),   975000 },
	{ 0, {   864000, HFPLL, 1, 0, 0x20 }, L2(7),  1000000 },
	{ 1, {   918000, HFPLL, 1, 0, 0x22 }, L2(7),  1000000 },
	{ 0, {   972000, HFPLL, 1, 0, 0x24 }, L2(7),  1025000 },
	{ 1, {  1026000, HFPLL, 1, 0, 0x26 }, L2(7),  1025000 },
	{ 0, {  1080000, HFPLL, 1, 0, 0x28 }, L2(16), 1075000 },
	{ 1, {  1134000, HFPLL, 1, 0, 0x2A }, L2(16), 1075000 },
	{ 0, {  1188000, HFPLL, 1, 0, 0x2C }, L2(16), 1100000 },
	{ 1, {  1242000, HFPLL, 1, 0, 0x2E }, L2(16), 1100000 },
	{ 0, {  1296000, HFPLL, 1, 0, 0x30 }, L2(16), 1125000 },
	{ 1, {  1350000, HFPLL, 1, 0, 0x32 }, L2(16), 1125000 },
	{ 0, {  1404000, HFPLL, 1, 0, 0x34 }, L2(16), 1125000 },
	{ 1, {  1458000, HFPLL, 1, 0, 0x36 }, L2(16), 1137500 },
	{ 1, {  1512000, HFPLL, 1, 0, 0x38 }, L2(18), 1150000 },
	{ 1, {  1674000, HFPLL, 1, 0, 0x3A }, L2(18), 1175000 },
	{ 1, {  1728000, HFPLL, 1, 0, 0x3C }, L2(19), 1200000 },
	{ 1, {  1809000, HFPLL, 1, 0, 0x3E }, L2(19), 1250000 },
	{ 1, {  1890000, HFPLL, 1, 0, 0x40 }, L2(19), 1275000 },
	{ 1, {  1998000, HFPLL, 1, 0, 0x40 }, L2(21), 1300000 },
	{ 1, {  2160000, HFPLL, 1, 0, 0x40 }, L2(21), 1325000 },
	{ 0, { 0 } }
};

/* TODO: Update vdd_dig and vdd_mem when voltage data is available. */
#undef L2

static unsigned long acpuclk_8960_get_rate(int cpu)
{
	return scalable[cpu].current_speed->khz;
}

/* Get the selected source on primary MUX. */
static int get_pri_clk_src(struct scalable *sc)
{
	uint32_t regval;

	regval = get_l2_indirect_reg(sc->l2cpmr_iaddr);
	return regval & 0x3;
}

/* Set the selected source on primary MUX. */
static void set_pri_clk_src(struct scalable *sc, uint32_t pri_src_sel)
{
	uint32_t regval;

	regval = get_l2_indirect_reg(sc->l2cpmr_iaddr);
	regval &= ~0x3;
	regval |= (pri_src_sel & 0x3);
	set_l2_indirect_reg(sc->l2cpmr_iaddr, regval);
	/* Wait for switch to complete. */
	mb();
	udelay(1);
}

/* Get the selected source on secondary MUX. */
static int get_sec_clk_src(struct scalable *sc)
{
	uint32_t regval;

	regval = get_l2_indirect_reg(sc->l2cpmr_iaddr);
	return (regval >> 2) & 0x3;
}

/* Set the selected source on secondary MUX. */
static void set_sec_clk_src(struct scalable *sc, uint32_t sec_src_sel)
{
	uint32_t regval;

	/* Disable secondary source clock gating during switch. */
	regval = get_l2_indirect_reg(sc->l2cpmr_iaddr);
	regval |= SECCLKAGD;
	set_l2_indirect_reg(sc->l2cpmr_iaddr, regval);

	/* Program the MUX. */
	regval &= ~(0x3 << 2);
	regval |= ((sec_src_sel & 0x3) << 2);
	set_l2_indirect_reg(sc->l2cpmr_iaddr, regval);

	/* Wait for switch to complete. */
	mb();
	udelay(1);

	/* Re-enable secondary source clock gating. */
	regval &= ~SECCLKAGD;
	set_l2_indirect_reg(sc->l2cpmr_iaddr, regval);
}

/* Enable an already-configured HFPLL. */
static void hfpll_enable(struct scalable *sc)
{
	int rc;

	if (cpu_is_msm8960() || cpu_is_msm8930() || cpu_is_msm8627()) {
		rc = rpm_vreg_set_voltage(sc->vreg[VREG_HFPLL_A].rpm_vreg_id,
				sc->vreg[VREG_HFPLL_A].rpm_vreg_voter, 2100000,
				sc->vreg[VREG_HFPLL_A].max_vdd, 0);
		if (rc)
			pr_err("%s regulator enable failed (%d)\n",
				sc->vreg[VREG_HFPLL_A].name, rc);
		rc = rpm_vreg_set_voltage(sc->vreg[VREG_HFPLL_B].rpm_vreg_id,
				sc->vreg[VREG_HFPLL_B].rpm_vreg_voter, 1800000,
				sc->vreg[VREG_HFPLL_B].max_vdd, 0);
		if (rc)
			pr_err("%s regulator enable failed (%d)\n",
				sc->vreg[VREG_HFPLL_B].name, rc);
	}

	/* Disable PLL bypass mode. */
	writel_relaxed(0x2, sc->hfpll_base + HFPLL_MODE);

	/*
	 * H/W requires a 5us delay between disabling the bypass and
	 * de-asserting the reset. Delay 10us just to be safe.
	 */
	mb();
	udelay(10);

	/* De-assert active-low PLL reset. */
	writel_relaxed(0x6, sc->hfpll_base + HFPLL_MODE);

	/* Wait for PLL to lock. */
	mb();
	udelay(60);

	/* Enable PLL output. */
	writel_relaxed(0x7, sc->hfpll_base + HFPLL_MODE);
}

/* Disable a HFPLL for power-savings or while its being reprogrammed. */
static void hfpll_disable(struct scalable *sc)
{
	int rc;

	/*
	 * Disable the PLL output, disable test mode, enable
	 * the bypass mode, and assert the reset.
	 */
	writel_relaxed(0, sc->hfpll_base + HFPLL_MODE);

	if (cpu_is_msm8960() || cpu_is_msm8930() || cpu_is_msm8627()) {
		rc = rpm_vreg_set_voltage(sc->vreg[VREG_HFPLL_B].rpm_vreg_id,
				sc->vreg[VREG_HFPLL_B].rpm_vreg_voter, 0,
				0, 0);
		if (rc)
			pr_err("%s regulator enable failed (%d)\n",
				sc->vreg[VREG_HFPLL_B].name, rc);
		rc = rpm_vreg_set_voltage(sc->vreg[VREG_HFPLL_A].rpm_vreg_id,
				sc->vreg[VREG_HFPLL_A].rpm_vreg_voter, 0,
				0, 0);
		if (rc)
			pr_err("%s regulator enable failed (%d)\n",
				sc->vreg[VREG_HFPLL_A].name, rc);
	}
}

/* Program the HFPLL rate. Assumes HFPLL is already disabled. */
static void hfpll_set_rate(struct scalable *sc, struct core_speed *tgt_s)
{
	writel_relaxed(tgt_s->pll_l_val, sc->hfpll_base + HFPLL_L_VAL);
}

/* Return the L2 speed that should be applied. */
static struct l2_level *compute_l2_level(struct scalable *sc,
					 struct l2_level *vote_l)
{
	struct l2_level *new_l;
	int cpu;

	/* Bounds check. */
	BUG_ON(vote_l >= (l2_freq_tbl + l2_freq_tbl_size));

	/* Find max L2 speed vote. */
	sc->l2_vote = vote_l;
	new_l = l2_freq_tbl;
	for_each_present_cpu(cpu)
		new_l = max(new_l, scalable[cpu].l2_vote);

	return new_l;
}

/* Update the bus bandwidth request. */
static void set_bus_bw(unsigned int bw)
{
	int ret;

	/* Bounds check. */
	if (bw >= ARRAY_SIZE(bw_level_tbl)) {
		pr_err("invalid bandwidth request (%d)\n", bw);
		return;
	}

	/* Update bandwidth if request has changed. This may sleep. */
	ret = msm_bus_scale_client_update_request(bus_perf_client, bw);
	if (ret)
		pr_err("bandwidth request failed (%d)\n", ret);
}

/* Set the CPU or L2 clock speed. */
static void set_speed(struct scalable *sc, struct core_speed *tgt_s,
		      enum setrate_reason reason)
{
	struct core_speed *strt_s = sc->current_speed;

	if (tgt_s == strt_s)
		return;

	if (strt_s->src == HFPLL && tgt_s->src == HFPLL) {
		/*
		 * Move to an always-on source running at a frequency that does
		 * not require an elevated CPU voltage. PLL8 is used here.
		 */
		set_sec_clk_src(sc, SEC_SRC_SEL_AUX);
		set_pri_clk_src(sc, PRI_SRC_SEL_SEC_SRC);

		/* Program CPU HFPLL. */
		hfpll_disable(sc);
		hfpll_set_rate(sc, tgt_s);
		hfpll_enable(sc);

		/* Move CPU to HFPLL source. */
		set_pri_clk_src(sc, tgt_s->pri_src_sel);
	} else if (strt_s->src == HFPLL && tgt_s->src != HFPLL) {
		/*
		 * If responding to CPU_DEAD we must be running on another
		 * CPU.  Therefore, we can't access the downed CPU's CP15
		 * clock MUX registers from here and can't change clock sources.
		 * Just turn off the PLL- since the CPU is down already, halting
		 * its clock should be safe.
		 */
		if (reason != SETRATE_HOTPLUG || sc == &scalable[L2]) {
			set_sec_clk_src(sc, tgt_s->sec_src_sel);
			set_pri_clk_src(sc, tgt_s->pri_src_sel);
		}
		hfpll_disable(sc);
	} else if (strt_s->src != HFPLL && tgt_s->src == HFPLL) {
		hfpll_set_rate(sc, tgt_s);
		hfpll_enable(sc);
		/*
		 * If responding to CPU_UP_PREPARE, we can't change CP15
		 * registers for the CPU that's coming up since we're not
		 * running on that CPU.  That's okay though, since the MUX
		 * source was not changed on the way down, either.
		 */
		if (reason != SETRATE_HOTPLUG || sc == &scalable[L2])
			set_pri_clk_src(sc, tgt_s->pri_src_sel);
	} else {
		if (reason != SETRATE_HOTPLUG || sc == &scalable[L2])
			set_sec_clk_src(sc, tgt_s->sec_src_sel);
	}

	sc->current_speed = tgt_s;
}

/* Apply any per-cpu voltage increases. */
static int increase_vdd(int cpu, unsigned int vdd_core, unsigned int vdd_mem,
			unsigned int vdd_dig, enum setrate_reason reason)
{
	struct scalable *sc = &scalable[cpu];
	int rc = 0;

	/*
	 * Increase vdd_mem active-set before vdd_dig.
	 * vdd_mem should be >= vdd_dig.
	 */
	if (vdd_mem > sc->vreg[VREG_MEM].cur_vdd) {
		rc = rpm_vreg_set_voltage(sc->vreg[VREG_MEM].rpm_vreg_id,
				sc->vreg[VREG_MEM].rpm_vreg_voter, vdd_mem,
				sc->vreg[VREG_MEM].max_vdd, 0);
		if (rc) {
			pr_err("%s: vdd_mem (cpu%d) increase failed (%d)\n",
				__func__, cpu, rc);
			return rc;
		}
		 sc->vreg[VREG_MEM].cur_vdd = vdd_mem;
	}

	/* Increase vdd_dig active-set vote. */
	if (vdd_dig > sc->vreg[VREG_DIG].cur_vdd) {
		rc = rpm_vreg_set_voltage(sc->vreg[VREG_DIG].rpm_vreg_id,
				sc->vreg[VREG_DIG].rpm_vreg_voter, vdd_dig,
				sc->vreg[VREG_DIG].max_vdd, 0);
		if (rc) {
			pr_err("%s: vdd_dig (cpu%d) increase failed (%d)\n",
				__func__, cpu, rc);
			return rc;
		}
		sc->vreg[VREG_DIG].cur_vdd = vdd_dig;
	}

	/*
	 * Update per-CPU core voltage. Don't do this for the hotplug path for
	 * which it should already be correct. Attempting to set it is bad
	 * because we don't know what CPU we are running on at this point, but
	 * the CPU regulator API requires we call it from the affected CPU.
	 */
	if (vdd_core > sc->vreg[VREG_CORE].cur_vdd
						&& reason != SETRATE_HOTPLUG) {
		rc = regulator_set_voltage(sc->vreg[VREG_CORE].reg, vdd_core,
					   sc->vreg[VREG_CORE].max_vdd);
		if (rc) {
			pr_err("%s: vdd_core (cpu%d) increase failed (%d)\n",
				__func__, cpu, rc);
			return rc;
		}
		sc->vreg[VREG_CORE].cur_vdd = vdd_core;
	}

	return rc;
}

/* Apply any per-cpu voltage decreases. */
static void decrease_vdd(int cpu, unsigned int vdd_core, unsigned int vdd_mem,
			 unsigned int vdd_dig, enum setrate_reason reason)
{
	struct scalable *sc = &scalable[cpu];
	int ret;

	/*
	 * Update per-CPU core voltage. This must be called on the CPU
	 * that's being affected. Don't do this in the hotplug remove path,
	 * where the rail is off and we're executing on the other CPU.
	 */
	if (vdd_core < sc->vreg[VREG_CORE].cur_vdd
					&& reason != SETRATE_HOTPLUG) {
		ret = regulator_set_voltage(sc->vreg[VREG_CORE].reg, vdd_core,
					    sc->vreg[VREG_CORE].max_vdd);
		if (ret) {
			pr_err("%s: vdd_core (cpu%d) decrease failed (%d)\n",
			       __func__, cpu, ret);
			return;
		}
		sc->vreg[VREG_CORE].cur_vdd = vdd_core;
	}

	/* Decrease vdd_dig active-set vote. */
	if (vdd_dig < sc->vreg[VREG_DIG].cur_vdd) {
		ret = rpm_vreg_set_voltage(sc->vreg[VREG_DIG].rpm_vreg_id,
				sc->vreg[VREG_DIG].rpm_vreg_voter, vdd_dig,
				sc->vreg[VREG_DIG].max_vdd, 0);
		if (ret) {
			pr_err("%s: vdd_dig (cpu%d) decrease failed (%d)\n",
				__func__, cpu, ret);
			return;
		}
		sc->vreg[VREG_DIG].cur_vdd = vdd_dig;
	}

	/*
	 * Decrease vdd_mem active-set after vdd_dig.
	 * vdd_mem should be >= vdd_dig.
	 */
	if (vdd_mem < sc->vreg[VREG_MEM].cur_vdd) {
		ret = rpm_vreg_set_voltage(sc->vreg[VREG_MEM].rpm_vreg_id,
				sc->vreg[VREG_MEM].rpm_vreg_voter, vdd_mem,
				sc->vreg[VREG_MEM].max_vdd, 0);
		if (ret) {
			pr_err("%s: vdd_mem (cpu%d) decrease failed (%d)\n",
				__func__, cpu, ret);
			return;
		}
		 sc->vreg[VREG_MEM].cur_vdd = vdd_mem;
	}
}

static unsigned int calculate_vdd_mem(struct acpu_level *tgt)
{
	return tgt->l2_level->vdd_mem;
}

static unsigned int calculate_vdd_dig(struct acpu_level *tgt)
{
	unsigned int pll_vdd_dig;

	if (tgt->l2_level->speed.src != HFPLL)
		pll_vdd_dig = 0;
	else if (tgt->l2_level->speed.pll_l_val > HFPLL_LOW_VDD_PLL_L_MAX)
		pll_vdd_dig = HFPLL_NOMINAL_VDD;
	else
		pll_vdd_dig = HFPLL_LOW_VDD;

	return max(tgt->l2_level->vdd_dig, pll_vdd_dig);
}

static unsigned int calculate_vdd_core(struct acpu_level *tgt)
{
	unsigned int pll_vdd_core;

	if (tgt->speed.src != HFPLL)
		pll_vdd_core = 0;
	else if (tgt->speed.pll_l_val > HFPLL_LOW_VDD_PLL_L_MAX)
		pll_vdd_core = HFPLL_NOMINAL_VDD;
	else
		pll_vdd_core = HFPLL_LOW_VDD;

	return max(tgt->vdd_core, pll_vdd_core);
}

/* Set the CPU's clock rate and adjust the L2 rate, if appropriate. */
static int acpuclk_8960_set_rate(int cpu, unsigned long rate,
				 enum setrate_reason reason)
{
	struct core_speed *strt_acpu_s, *tgt_acpu_s;
	struct l2_level *tgt_l2_l;
	struct acpu_level *tgt;
	unsigned int vdd_mem, vdd_dig, vdd_core;
	unsigned long flags;
	int rc = 0;

	set_acpuclk_foot_print(cpu, 0x1);

	if (cpu > num_possible_cpus()) {
		rc = -EINVAL;
		goto out;
	}

	if (reason == SETRATE_CPUFREQ || reason == SETRATE_HOTPLUG)
		mutex_lock(&driver_lock);

	set_acpuclk_foot_print(cpu, 0x2);

	strt_acpu_s = scalable[cpu].current_speed;

	/* Return early if rate didn't change. */
	if (rate == strt_acpu_s->khz && scalable[cpu].first_set_call == false)
		goto out;

	/* Find target frequency. */
	for (tgt = acpu_freq_tbl; tgt->speed.khz != 0; tgt++) {
		if (tgt->speed.khz == rate) {
			tgt_acpu_s = &tgt->speed;
			break;
		}
	}
	if (tgt->speed.khz == 0) {
		rc = -EINVAL;
		goto out;
	}

	/* Calculate voltage requirements for the current CPU. */
	vdd_mem  = calculate_vdd_mem(tgt);
	vdd_dig  = calculate_vdd_dig(tgt);
	vdd_core = calculate_vdd_core(tgt);

	/* Increase VDD levels if needed. */
	if (reason == SETRATE_CPUFREQ || reason == SETRATE_HOTPLUG) {
		rc = increase_vdd(cpu, vdd_core, vdd_mem, vdd_dig, reason);
		if (rc)
			goto out;
	}

	pr_debug("Switching from ACPU%d rate %u KHz -> %u KHz\n",
		cpu, strt_acpu_s->khz, tgt_acpu_s->khz);
	udelay(60);
	set_acpuclk_foot_print(cpu, 0x3);

	/* Set the CPU speed. */
	set_speed(&scalable[cpu], tgt_acpu_s, reason);

	set_acpuclk_cpu_freq_foot_print(cpu, tgt_acpu_s->khz);
	set_acpuclk_foot_print(cpu, 0x4);

	/*
	 * Update the L2 vote and apply the rate change. A spinlock is
	 * necessary to ensure L2 rate is calulated and set atomically,
	 * even if acpuclk_8960_set_rate() is called from an atomic context
	 * and the driver_lock mutex is not acquired.
	 */
	spin_lock_irqsave(&l2_lock, flags);
	tgt_l2_l = compute_l2_level(&scalable[cpu], tgt->l2_level);
	set_speed(&scalable[L2], &tgt_l2_l->speed, reason);

	set_acpuclk_L2_freq_foot_print(tgt_l2_l->speed.khz);
	set_acpuclk_foot_print(cpu, 0x5);

	spin_unlock_irqrestore(&l2_lock, flags);

	/* Nothing else to do for power collapse or SWFI. */
	if (reason == SETRATE_PC || reason == SETRATE_SWFI)
		goto out;

	/* Update bus bandwith request. */
	set_bus_bw(tgt_l2_l->bw_level);

	set_acpuclk_foot_print(cpu, 0x6);

	/* Drop VDD levels if we can. */
	decrease_vdd(cpu, vdd_core, vdd_mem, vdd_dig, reason);

	set_acpuclk_foot_print(cpu, 0x7);

	scalable[cpu].first_set_call = false;
	pr_debug("ACPU%d speed change complete\n", cpu);

out:
	if (reason == SETRATE_CPUFREQ || reason == SETRATE_HOTPLUG)
		mutex_unlock(&driver_lock);

	set_acpuclk_foot_print(cpu, 0x8);

	return rc;
}

ssize_t acpuclk_get_vdd_levels_str(char *buf, int isApp) {

	int i, len = 0;

	if (buf) {
		mutex_lock(&driver_lock);

		if (isApp == 0)
		{
			for (i = 0; acpu_freq_tbl[i+1].speed.khz; i++)
				len += sprintf(buf + len, "%8u: %8d\n", acpu_freq_tbl[i+1].speed.khz, acpu_freq_tbl[i+1].vdd_core );
		}
		else
		{
			for (i = isApp-1; i >= 0; i--)
				len += sprintf(buf + len, "%dmhz: %d mV\n", acpu_freq_tbl[i+1].speed.khz/1000,acpu_freq_tbl[i+1].vdd_core/1000);
		}
		mutex_unlock(&driver_lock);

		}
	return len;
}


void acpuclk_set_vdd(unsigned int khz, int vdd_uv) {

	int i;
	unsigned int new_vdd_uv;

	mutex_lock(&driver_lock);

	for (i = 0; acpu_freq_tbl[i+1].speed.khz; i++) {
		if (khz == 0)
			new_vdd_uv = min(max((acpu_freq_tbl[i+1].vdd_core + vdd_uv), (unsigned int)MIN_VDD_SC), (unsigned int)MAX_VDD_SC);
		else if ( acpu_freq_tbl[i+1].speed.khz == khz)
			new_vdd_uv = min(max((unsigned int)vdd_uv, (unsigned int)MIN_VDD_SC), (unsigned int)MAX_VDD_SC);
		else 
			continue;

		acpu_freq_tbl[i+1].vdd_core = new_vdd_uv;
	}

	mutex_unlock(&driver_lock);
}

void acpuclk_UV_mV_table(int cnt, int vdd_uv[]) {

	int i;
	int j=0;
	mutex_lock(&driver_lock);

	if (vdd_uv[0] < vdd_uv[cnt-1])
	{
		for (i = 0; i < cnt; i++) {
		    if ((vdd_uv[i]*1000) >= MIN_VDD_SC && (vdd_uv[i]*1000) <= MAX_VDD_SC)
			acpu_freq_tbl[i+1].vdd_core = vdd_uv[i]*1000;
		}
	}
	else
	{
		j = cnt-1;
		for (i = 0; i < cnt; i++) {
		    if ((vdd_uv[j]*1000) >= MIN_VDD_SC && (vdd_uv[j]*1000) <= MAX_VDD_SC)
			acpu_freq_tbl[i+1].vdd_core = vdd_uv[j]*1000;
		    j--;
		}
	}
	mutex_unlock(&driver_lock);
}

/* Initialize a HFPLL at a given rate and enable it. */
static void __init hfpll_init(struct scalable *sc, struct core_speed *tgt_s)
{
	pr_debug("Initializing HFPLL%d\n", sc - scalable);

	/* Disable the PLL for re-programming. */
	hfpll_disable(sc);

	/* Configure PLL parameters for integer mode. */
	writel_relaxed(0x7845C665, sc->hfpll_base + HFPLL_CONFIG_CTL);
	writel_relaxed(0, sc->hfpll_base + HFPLL_M_VAL);
	writel_relaxed(1, sc->hfpll_base + HFPLL_N_VAL);

	/* Program droop controller. */
	writel_relaxed(0x0108C000, sc->hfpll_base + HFPLL_DROOP_CTL);

	/* Set an initial rate and enable the PLL. */
	hfpll_set_rate(sc, tgt_s);
	hfpll_enable(sc);
}

/* Voltage regulator initialization. */
static void __init regulator_init(void)
{
	int cpu, ret;
	struct scalable *sc;

	for_each_possible_cpu(cpu) {
		sc = &scalable[cpu];
		sc->vreg[VREG_CORE].reg = regulator_get(NULL,
					  sc->vreg[VREG_CORE].name);
		if (IS_ERR(sc->vreg[VREG_CORE].reg)) {
			pr_err("regulator_get(%s) failed (%ld)\n",
			       sc->vreg[VREG_CORE].name,
			       PTR_ERR(sc->vreg[VREG_CORE].reg));
			BUG();
		}

		ret = regulator_set_voltage(sc->vreg[VREG_CORE].reg, max_vdd, max_vdd);

		if (ret)
			pr_err("regulator_set_voltage(%s) failed"
			       " (%d)\n", sc->vreg[VREG_CORE].name, ret);

		ret = regulator_enable(sc->vreg[VREG_CORE].reg);
		if (ret)
			pr_err("regulator_enable(%s) failed (%d)\n",
			       sc->vreg[VREG_CORE].name, ret);
	}
}

/* Set initial rate for a given core. */
static void __init init_clock_sources(struct scalable *sc,
				      struct core_speed *tgt_s)
{
	uint32_t regval;

	/* Select PLL8 as AUX source input to the secondary MUX. */
	writel_relaxed(0x3, sc->aux_clk_sel);

	/* Switch away from the HFPLL while it's re-initialized. */
	set_sec_clk_src(sc, SEC_SRC_SEL_AUX);
	set_pri_clk_src(sc, PRI_SRC_SEL_SEC_SRC);
	hfpll_init(sc, tgt_s);

	/* Set PRI_SRC_SEL_HFPLL_DIV2 divider to div-2. */
	regval = get_l2_indirect_reg(sc->l2cpmr_iaddr);
	regval &= ~(0x3 << 6);
	set_l2_indirect_reg(sc->l2cpmr_iaddr, regval);

	/* Switch to the target clock source. */
	set_sec_clk_src(sc, tgt_s->sec_src_sel);
	set_pri_clk_src(sc, tgt_s->pri_src_sel);
	sc->current_speed = tgt_s;

	/*
	 * Set this flag so that the first call to acpuclk_8960_set_rate() can
	 * drop voltages and set initial bus bandwidth requests.
	 */
	sc->first_set_call = true;
}

static void __init per_cpu_init(void *data)
{
	struct acpu_level *max_acpu_level = data;
	int cpu = smp_processor_id();

	init_clock_sources(&scalable[cpu], &max_acpu_level->speed);
	scalable[cpu].l2_vote = max_acpu_level->l2_level;
}

/* Register with bus driver. */
static void __init bus_init(void)
{
	int ret;

	bus_perf_client = msm_bus_scale_register_client(&bus_client_pdata);
	if (!bus_perf_client) {
		pr_err("unable to register bus client\n");
		BUG();
	}

	ret = msm_bus_scale_client_update_request(bus_perf_client,
		(ARRAY_SIZE(bw_level_tbl)-1));
	if (ret)
		pr_err("initial bandwidth request failed (%d)\n", ret);
}

#ifdef CONFIG_CPU_FREQ_MSM
static struct cpufreq_frequency_table freq_table[NR_CPUS][30];

static void __init cpufreq_table_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		int i, freq_cnt = 0;
		/* Construct the freq_table tables from acpu_freq_tbl. */
		for (i = 0; acpu_freq_tbl[i].speed.khz != 0
				&& freq_cnt < ARRAY_SIZE(*freq_table); i++) {
			if (acpu_freq_tbl[i].use_for_scaling) {
				freq_table[cpu][freq_cnt].index = freq_cnt;
				freq_table[cpu][freq_cnt].frequency
					= acpu_freq_tbl[i].speed.khz;
				freq_cnt++;
			}
		}
		/* freq_table not big enough to store all usable freqs. */
		BUG_ON(acpu_freq_tbl[i].speed.khz != 0);

		freq_table[cpu][freq_cnt].index = freq_cnt;
		freq_table[cpu][freq_cnt].frequency = CPUFREQ_TABLE_END;

		pr_info("CPU%d: %d scaling frequencies supported.\n",
			cpu, freq_cnt);

		/* Register table with CPUFreq. */
		cpufreq_frequency_table_get_attr(freq_table[cpu], cpu);
	}
}
#else
static void __init cpufreq_table_init(void) {}
#endif

#define HOT_UNPLUG_KHZ STBY_KHZ
static int __cpuinit acpuclock_cpu_callback(struct notifier_block *nfb,
					    unsigned long action, void *hcpu)
{
	static int prev_khz[NR_CPUS];
	static int prev_pri_src[NR_CPUS];
	static int prev_sec_src[NR_CPUS];
	int cpu = (int)hcpu;

	switch (action) {
	case CPU_DYING:
	case CPU_DYING_FROZEN:
		/*
		 * The primary and secondary muxes must be set to QSB before L2
		 * power collapse and restored after.
		 */
		prev_sec_src[cpu] = get_sec_clk_src(&scalable[cpu]);
		prev_pri_src[cpu] = get_pri_clk_src(&scalable[cpu]);
		set_sec_clk_src(&scalable[cpu], SEC_SRC_SEL_QSB);
		set_pri_clk_src(&scalable[cpu], PRI_SRC_SEL_SEC_SRC);
		break;
	case CPU_DEAD:
	case CPU_DEAD_FROZEN:
		prev_khz[cpu] = acpuclk_8960_get_rate(cpu);
		/* Fall through. */
	case CPU_UP_CANCELED:
	case CPU_UP_CANCELED_FROZEN:
		acpuclk_8960_set_rate(cpu, HOT_UNPLUG_KHZ, SETRATE_HOTPLUG);
		break;
	case CPU_UP_PREPARE:
	case CPU_UP_PREPARE_FROZEN:
		if (WARN_ON(!prev_khz[cpu]))
			return NOTIFY_BAD;
		acpuclk_8960_set_rate(cpu, prev_khz[cpu], SETRATE_HOTPLUG);
		break;
	case CPU_STARTING:
	case CPU_STARTING_FROZEN:
		set_sec_clk_src(&scalable[cpu], prev_sec_src[cpu]);
		set_pri_clk_src(&scalable[cpu], prev_pri_src[cpu]);
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block __cpuinitdata acpuclock_cpu_notifier = {
	.notifier_call = acpuclock_cpu_callback,
};

static const int krait_needs_vmin(void)
{
	switch (read_cpuid_id()) {
	case 0x511F04D0:
	case 0x511F04D1:
	case 0x510F06F0:
		return 1;
	default:
		return 0;
	};
}

static void kraitv2_apply_vmin(struct acpu_level *tbl)
{
	for (; tbl->speed.khz != 0; tbl++)
		if (tbl->vdd_core < MIN_VDD_SC)
			tbl->vdd_core = MIN_VDD_SC;
}

static struct acpu_level * __init select_freq_plan(void)
{
	struct acpu_level *l, *max_acpu_level = NULL;
	unsigned int kernel_flag = get_kernel_flag();

	/* Select frequency tables. */
	if (cpu_is_msm8960()) {
		uint32_t pte_efuse, pvs;
		struct acpu_level *v2;

		pte_efuse = readl_relaxed(QFPROM_PTE_EFUSE_ADDR);
		pvs = (pte_efuse >> 10) & 0x7;
		if (pvs == 0x7)
			pvs = (pte_efuse >> 13) & 0x7;

		switch (pvs) {
		case 0x0:
		case 0x7:
			pr_info("ACPU PVS: Fusion SLOW\n");
			v2 = acpu_freq_tbl_8960_fusion_slow;
			break;
		case 0x1:
			pr_info("ACPU PVS: Fusion NOM\n");
			v2 = acpu_freq_tbl_8960_fusion_nom;
			break;
		case 0x3:
			pr_info("ACPU PVS: Fusion ULT\n");
			v2 = acpu_freq_tbl_8960_fusion_fast;
			break;
		default:
			pr_warn("ACPU PVS: Unknown. Defaulting to nom.\n");
			v2 = acpu_freq_tbl_8960_fusion_nom;
			break;
		}

		/* Force apply CPU table by writeconfig */
		if (!cpu_is_krait_v1()){
			if(kernel_flag & KERNEL_FLAG_PVS_SLOW_CPU){
				pr_info("ACPU PVS: Force SLOW by writeconfig\n");
				v2 = acpu_freq_tbl_8960_fusion_slow;
			}
			else if (kernel_flag & KERNEL_FLAG_PVS_NOM_CPU){
				pr_info("ACPU PVS: Force NOMINAL by writeconfig\n");
				v2 = acpu_freq_tbl_8960_fusion_nom;
			}
			else if (kernel_flag & KERNEL_FLAG_PVS_FAST_CPU){
				v2 = acpu_freq_tbl_8960_fusion_fast;
				pr_info("ACPU PVS: Force FAST by writeconfig\n");
			}
		}

		scalable = scalable_8960;
		if (cpu_is_krait_v1()) {
			
		} else {
			acpu_freq_tbl = v2;
			l2_freq_tbl = l2_freq_tbl_8960_kraitv2;
			l2_freq_tbl_size = ARRAY_SIZE(l2_freq_tbl_8960_kraitv2);
		}
	}  else {
		BUG();
	}
	if (krait_needs_vmin()) {
		pr_info("Applying min 1.15v fix for Krait Errata 26\n");
		kraitv2_apply_vmin(acpu_freq_tbl);
	}

	/* Adjust frequency table according to custom acpu_max_freq */
	if (acpu_max_freq) {
		for (l = acpu_freq_tbl; l->speed.khz != 0; l++) {
			if (l->speed.khz == acpu_max_freq) {
				/* Custom max freq found in table.
				 * Mark all subsequent frequencies
				 * as not supported.
				 */
				for (++l; l->speed.khz != 0; l++)
					l->use_for_scaling = 0;
				break;
			}
		}
	}

	/* Find the max supported scaling frequency. */
	for (l = acpu_freq_tbl; l->speed.khz != 0; l++)
		if (l->use_for_scaling && l->speed.khz==2160000)
			max_acpu_level = l;
	BUG_ON(!max_acpu_level);
	pr_info("Max ACPU freq: %u KHz\n", max_acpu_level->speed.khz);
	max_vdd = max_acpu_level->vdd_core;

	return max_acpu_level;
}

static struct acpuclk_data acpuclk_8960_data = {
	.set_rate = acpuclk_8960_set_rate,
	.get_rate = acpuclk_8960_get_rate,
	.power_collapse_khz = STBY_KHZ,
	.wait_for_irq_khz = STBY_KHZ,
};

static int __init acpuclk_8960_init(struct acpuclk_soc_data *soc_data)
{
	struct acpu_level *max_acpu_level = select_freq_plan();
	init_clock_sources(&scalable[L2], &max_acpu_level->l2_level->speed);
	on_each_cpu(per_cpu_init, max_acpu_level, true);

	regulator_init();
	bus_init();
	cpufreq_table_init();

	acpuclk_register(&acpuclk_8960_data);
	register_hotcpu_notifier(&acpuclock_cpu_notifier);

	return 0;
}

struct acpuclk_soc_data acpuclk_8960_soc_data __initdata = {
	.init = acpuclk_8960_init,
};

struct acpuclk_soc_data acpuclk_8930_soc_data __initdata = {
	.init = acpuclk_8960_init,
};
/* OnePlus Ace 5 / giulia PCIe0 host controller (WCN7850's bus).
 * Explicit pcie_step=N opt-in, in the same style as giulia_s3910.c and the
 * same split K90's mhi_pcie.c settled on after their own USB deaths:
 *
 *   pcie_step=1  read-only survey of the always-on blocks only: the clock
 *                controller's PCIe domains/clocks/resets and the card's six
 *                pins as the firmware left them. The PCIe windows are not
 *                even mapped. Writes nothing.
 *   pcie_step=2  read-only survey of the root complex itself, after
 *                pcie_pause (30 s by default) and only when both power
 *                domains are on and the four PCIe clocks are running
 *                (windows_clocked()). Otherwise it says so and reads nothing.
 *   pcie_step=3  power-up: RPMh votes for the card's seven supplies, PERST
 *                and WLAN_EN low, GDSCs, clocks, resets, PARF setup, the
 *                vendor PHY sequence, WLAN_EN high, the root port's header,
 *                PERST high, LTSSM enable, then the link poll; on a live
 *                link the step-2 survey runs too.
 *
 * pcie_pause=N seconds before each stage, so the host has the previous
 * stage's log if a read stalls the SoC (K90's kernels 474d6e9 and 32bbef4
 * stopped answering USB in 8 s without it; ours died the same way).
 *
 * The per-pin registers are in the TLMM GPIO bank at 0xf100000 -- the device
 * tree's pinctrl@f000000 plus 0x100000, the same address giulia_s3910_hw.h
 * calls S39_TLMM_PA. Reading pin*0x1000 from the pinctrl base itself is what
 * stopped K90's kernel 76c1ad6, and us (2026-11-06's four 900E rounds).
 *
 * Register windows, pins and the PHY sequence come from giulia_pcie_regs.h,
 * generated from this phone's device tree. GCC offsets are from Linux's
 * gcc-sm8650.c (see the header). The supplies are the seven regulators the
 * cnss-kiwi node's -supply phandles point at (rpmh-regulator-smpc2/smpc3/
 * ldob15/ldoc3/smpc6/smpc1/smpi4), voted through the apps RSC's DRV 2 at
 * 0x17a20000 -- the sequencer giulia_s3910.c already votes ldob4 through,
 * one rail per transaction in s39_power's exact slot discipline.
 */
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include "giulia_pcie_regs.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void loader_log_line(const char *line);

/*
 * printf's kernel message buffer is appended to the loader's log only at loader
 * return; mirror each line immediately, like a750_log does, so the host keeps
 * every line even when a later read stalls the SoC.
 */
static void
pcie_log(const char *format, ...) __printflike(1, 2);

static void
pcie_log(const char *format, ...)
{
	char line[384];
	va_list args;

	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	loader_log_line(line);
	printf("%s", line);
}

static uintptr_t parf, phy, dbi, sm, gcc, tlmm, rsc, cmd_db, tcsrcc, elbi;
static int pcie_step;
static unsigned pcie_pause_s;
static int pcie_supplies = 1;   /* boot-arg pcie_supplies=0: skip the RPMh votes (A/B) */
static int pcie_gdsc = 1;       /* boot-arg pcie_gdsc=0: skip the GDSC stage (A/B) */
static volatile unsigned pcie_pause_event;

static uint32_t
rd(uintptr_t base, uint32_t offset)
{
	return *(volatile uint32_t *)(base + offset);
}

static void
wr(uintptr_t base, uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(base + offset) = value;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
delay_us(unsigned us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

/*
 * Seconds of sleep before a stage, so the host has the previous stage's log
 * if this one stalls the SoC. Sleeps, not spins: the run's other work goes on.
 */
static void
pcie_pause(unsigned seconds, const char *stage)
{
	if (seconds == 0) {
		return;
	}
	pcie_log("pcie: next, in %u s: %s\n", seconds, stage);
	assert_wait_timeout((event_t)&pcie_pause_event, THREAD_UNINT,
	    seconds * 1000, NSEC_PER_MSEC);
	thread_block(THREAD_AWAKENED);
}

static int
poll(uintptr_t base, uint32_t offset, uint32_t mask, uint32_t want, unsigned timeout_us)
{
	uint32_t value = 0;

	for (unsigned us = 0; us < timeout_us; us += 10) {
		value = rd(base, offset);
		if ((value & mask) == want) {
			return 0;
		}
		delay_us(10);
	}
	pcie_log("pcie: poll timeout %#x=%#x (want %#x mask %#x)\n", offset, value, want, mask);
	return ETIMEDOUT;
}

static void
gpio_out(unsigned pin, bool high)
{
	/* TLMM sm8650 GPIO_CFG (pinctrl-sm8650.c): pull bits [1:0], function
	 * bits [5:2] (gpio = 0), drive bits [8:6], output enable bit 9. The old
	 * TLMM layout this code assumed (function [1:0], OE bit 2) never
	 * enabled the output -- the pads stayed inputs and read 0, which is
	 * exactly why PERST and WLAN_EN never reached the card. IN_OUT: IN
	 * bit 0, OUT bit 1. Latch the level before enabling the output. */
	uint32_t value = rd(tlmm, pin * 0x1000 + 4);

	wr(tlmm, pin * 0x1000 + 4, high ? (value | 2u) : (value & ~2u));
	value = rd(tlmm, pin * 0x1000);
	wr(tlmm, pin * 0x1000, (value & ~0x3ffu) | (6u << 6) | (1u << 9));
}

static int
gdsc_on(uint32_t offset, uint32_t vote_bit)
{
	/* The VOTABLE GDSC's collapse vote bit (GCC+0x5214c) must be cleared
	 * first -- Android leaves both set and a bare GDSCR write is ignored.
	 * Then Linux's gdsc_enable: clear SW_COLLAPSE (bit 0) and wait for the
	 * CFG GDSCR's POWER_UP_COMPLETE (gdscr+4, BIT(16) -- the OnePlus
	 * sm8650 kernel's gdsc_check_status for POLL_CFG_GDSCR gdscs). */
	uint32_t value, cfg;

	wr(gcc, GIULIA_GCC_COLLAPSE_VOTE, rd(gcc, GIULIA_GCC_COLLAPSE_VOTE) & ~vote_bit);
	value = rd(gcc, offset);
	wr(gcc, offset, value & ~1u);
	for (unsigned i = 0; i < 4000; i++) {
		cfg = rd(gcc, offset + 4);
		if (cfg & GIULIA_GCC_CFG_GDSCR_POWER_UP_COMPLETE) {
			pcie_log("pcie: power: gdsc at %#x on after %u00 us (gdscr %#x, cfg %#x)\n",
			    offset, i, rd(gcc, offset), cfg);
			return 0;
		}
		delay_us(100);
	}
	pcie_log("pcie: power: gdsc at %#x stuck (gdscr %#x, cfg %#x)\n", offset, rd(gcc, offset), cfg);
	return ETIMEDOUT;
}

static int
clocks_on(void)
{
	static const uint32_t halt[] = {
		GIULIA_GCC_PCIE0_SLV_HALT, GIULIA_GCC_PCIE0_MSTR_HALT,
		GIULIA_GCC_PCIE0_AHB_HALT, GIULIA_GCC_PCIE0_AUX_HALT,
		GIULIA_GCC_PCIE0_Q2A_HALT,
	};
	uint32_t vote = rd(gcc, GIULIA_GCC_PCIE_VOTE);
	uint32_t rchng;
	int error;

	/* PIPE too: the kernel's own order enables the whole PHY clock bulk
	 * before the PHY is calibrated, and the branch is BRANCH_HALT_SKIP
	 * precisely because it only ticks once the PHY is out of reset. */
	wr(gcc, GIULIA_GCC_PCIE_VOTE, vote | 0x3fu);
	for (unsigned i = 0; i < sizeof(halt) / sizeof(halt[0]); i++) {
		error = poll(gcc, halt[i], 0x80000000u, 0, 10000);
		if (error) {
			return error;
		}
	}

	/* The PHY's recharge clock: GPLL0_EVEN / 3 = 100 MHz (gcc-sm8650.c
	 * ftbl_gcc_pcie_0_phy_rchng_clk_src), branch vote 0x52000 BIT(22).
	 * The QMP PHY driver turns it on with its clock bulk; without the
	 * recharge domain the receiver detect never completes and the LTSSM
	 * sits in Detect.Active. */
	rchng = rd(gcc, GIULIA_GCC_RCHNG_VOTE);
	wr(gcc, GIULIA_GCC_PCIE0_RCHNG_CFG, 0x602u);
	wr(gcc, GIULIA_GCC_PCIE0_RCHNG_CMD, 1u);
	error = poll(gcc, GIULIA_GCC_PCIE0_RCHNG_CMD, 1u, 0, 10000);
	if (error) {
		return error;
	}
	wr(gcc, GIULIA_GCC_RCHNG_VOTE, rchng | (1u << 22));
	error = poll(gcc, GIULIA_GCC_PCIE0_RCHNG_HALT, 0x80000000u, 0, 10000);
	if (error) {
		return error;
	}
	pcie_log("pcie: rchng vote pre=%#x post=%#x, halt %#x\n", rchng,
	    rd(gcc, GIULIA_GCC_RCHNG_VOTE), rd(gcc, GIULIA_GCC_PCIE0_RCHNG_HALT));
	return 0;
}

static void
reset_pulse(uint32_t offset)
{
	wr(gcc, offset, 1u);
	delay_us(1000);
	wr(gcc, offset, 0u);
}

/*
 * Whether the PCIe register windows (PARF/PHY/DBI/SM) can be read at all. K90's rule
 * (mhi_pcie.c, step 2): both power domains on AND the cfg_ahb/slv_axi/mstr_axi/aux
 * clocks running. All of these are in the always-on GCC block. Their first run with
 * these reads in step 1 (kernel 474d6e9) stopped answering USB 8 s into the boot.
 */
static bool
windows_clocked(void)
{
	uint32_t core = rd(gcc, GIULIA_GCC_PCIE0_GDSCR);
	uint32_t phy = rd(gcc, GIULIA_GCC_PCIE0_PHY_GDSCR);
	uint32_t clocks = rd(gcc, GIULIA_GCC_PCIE0_SLV_HALT) | rd(gcc, GIULIA_GCC_PCIE0_MSTR_HALT) |
	    rd(gcc, GIULIA_GCC_PCIE0_AHB_HALT) | rd(gcc, GIULIA_GCC_PCIE0_AUX_HALT);

	return (core & 0x80000000u) != 0 && (phy & 0x80000000u) != 0 && (clocks & 0x80000000u) == 0;
}

/* ---- step 1: the always-on blocks -------------------------------------------------------------- */

static void
survey_gcc_tlmm(void)
{
	pcie_log("pcie: step 1 read-only survey (gcc/tlmm)\n");
	pcie_log("pcie: gcc gdsc=%#x phy_gdsc=%#x vote=%#x\n",
	    rd(gcc, GIULIA_GCC_PCIE0_GDSCR), rd(gcc, GIULIA_GCC_PCIE0_PHY_GDSCR),
	    rd(gcc, GIULIA_GCC_PCIE_VOTE));
	pcie_log("pcie: gcc halt slv=%#x mstr=%#x ahb=%#x aux=%#x q2a=%#x pipe=%#x\n",
	    rd(gcc, GIULIA_GCC_PCIE0_SLV_HALT), rd(gcc, GIULIA_GCC_PCIE0_MSTR_HALT),
	    rd(gcc, GIULIA_GCC_PCIE0_AHB_HALT), rd(gcc, GIULIA_GCC_PCIE0_AUX_HALT),
	    rd(gcc, GIULIA_GCC_PCIE0_Q2A_HALT), rd(gcc, GIULIA_GCC_PCIE0_PIPE_HALT));
	pcie_log("pcie: gcc bcr core=%#x phy=%#x\n",
	    rd(gcc, GIULIA_GCC_PCIE0_CORE_BCR), rd(gcc, GIULIA_GCC_PCIE0_PHY_BCR));
	pcie_log("pcie: gpio perst=%#x/%#x clkreq=%#x/%#x wake=%#x/%#x\n",
	    rd(tlmm, GIULIA_PCIE_PERST_GPIO * 0x1000), rd(tlmm, GIULIA_PCIE_PERST_GPIO * 0x1000 + 4),
	    rd(tlmm, GIULIA_PCIE_CLKREQ_GPIO * 0x1000), rd(tlmm, GIULIA_PCIE_CLKREQ_GPIO * 0x1000 + 4),
	    rd(tlmm, GIULIA_PCIE_WAKE_GPIO * 0x1000), rd(tlmm, GIULIA_PCIE_WAKE_GPIO * 0x1000 + 4));
	pcie_log("pcie: cnss wlan_en=%#x/%#x bt_en=%#x/%#x sw_ctrl=%#x/%#x\n",
	    rd(tlmm, GIULIA_CNSS_WLAN_EN_GPIO * 0x1000), rd(tlmm, GIULIA_CNSS_WLAN_EN_GPIO * 0x1000 + 4),
	    rd(tlmm, GIULIA_CNSS_BT_EN_GPIO * 0x1000), rd(tlmm, GIULIA_CNSS_BT_EN_GPIO * 0x1000 + 4),
	    rd(tlmm, GIULIA_CNSS_SW_CTRL_GPIO * 0x1000), rd(tlmm, GIULIA_CNSS_SW_CTRL_GPIO * 0x1000 + 4));
	pcie_log("pcie: step 1 done\n");
}

/* ---- step 2: the root complex, only with power and clocks on -------------------- */

static void
survey_windows(void)
{
	pcie_log("pcie: the root complex's registers, with the domains on and the clocks running\n");
	pcie_log("pcie: parf %#x %#x %#x %#x\n", rd(parf, 0), rd(parf, 0x40),
	    rd(parf, 0x1000), rd(parf, 0x2c00));
	pcie_log("pcie: phy status=%#x\n", rd(phy, GIULIA_PCIE_PHY_STATUS));
	pcie_log("pcie: dbi id=%#x class=%#x link=%#x cap=%#x ctrl=%#x\n",
	    rd(dbi, 0), rd(dbi, 8), rd(dbi, 0x80), rd(dbi, 0xa0), rd(dbi, 0x78));
	pcie_log("pcie: sm %#x %#x %#x %#x\n", rd(sm, 0), rd(sm, 4), rd(sm, 8), rd(sm, 0xc));
}

static void
survey_step2(void)
{
	pcie_pause(pcie_pause_s ? pcie_pause_s : 30, "the root complex's registers");
	if (!windows_clocked()) {
		pcie_log("pcie: domains off or clocks halted, nothing read "
		    "(gdsc=%#x phy_gdsc=%#x halts %#x/%#x/%#x/%#x)\n",
		    rd(gcc, GIULIA_GCC_PCIE0_GDSCR), rd(gcc, GIULIA_GCC_PCIE0_PHY_GDSCR),
		    rd(gcc, GIULIA_GCC_PCIE0_SLV_HALT), rd(gcc, GIULIA_GCC_PCIE0_MSTR_HALT),
		    rd(gcc, GIULIA_GCC_PCIE0_AHB_HALT), rd(gcc, GIULIA_GCC_PCIE0_AUX_HALT));
		pcie_log("pcie: step 2 done (nothing read; step 3 powers the card)\n");
		return;
	}
	/* The domains are on and the clocks running: only now map the windows. */
	parf = ml_io_map(GIULIA_PCIE_PARF, GIULIA_PCIE_PARF_SIZE);
	phy = ml_io_map(GIULIA_PCIE_PHY, GIULIA_PCIE_PHY_SIZE);
	dbi = ml_io_map(GIULIA_PCIE_DM_CORE, 0x2000);
	sm = ml_io_map(GIULIA_PCIE_PCIE_SM, GIULIA_PCIE_PCIE_SM_SIZE);
	if (!parf || !phy || !dbi || !sm) {
		pcie_log("pcie: map failed parf=%p phy=%p dbi=%p sm=%p\n",
		    (void *)parf, (void *)phy, (void *)dbi, (void *)sm);
		return;
	}
	survey_windows();
	pcie_log("pcie: step 2 done\n");
}

/* ---- C: RPMh -- the card's seven supplies --------------------------------------- */

/*
 * The apps RSC's DRV 2 at 0x17a20000 -- the sequencer giulia_s3910.c votes the
 * touch panel's ldob4 through, on this phone. RSC v3 (the driver checks the
 * layout's major in rsc_id): the three ACTIVE slot sets sit at 0xd00, 672 bytes
 * apart; per slot the wait/control/status/enable registers are at
 * 0x20/0x24/0x28/0x2c and the commands are 24 bytes from 0x34 (message id,
 * address, value, status). The interrupt registers are global at +0/+4/+8.
 * Every transaction votes at most three commands on an idle slot and gives the
 * slot back the way it was (s39_power's path); our votes are one rail each.
 */
#define PCIE_RSC_PHYS           0x17a20000UL
#define PCIE_RSC_SIZE           0x10000UL
#define PCIE_CMD_DB_PHYS        0x81c60000UL    /* dt: aop_cmd_db_region@81c60000; S39_DB_PA */
#define PCIE_CMD_DB_SIZE        0x20000UL

#define RSC_IRQ_ENABLE          0xd00                   /* the DRV's IRQ registers live in the */
#define RSC_IRQ_STATUS          0xd04                   /* TCS register block at 0xd00, not at */
#define RSC_IRQ_CLEAR           0xd08                   /* the RSC base (s39_power waits at +0xd04) */
#define RSC_ACTIVE_SLOTS        3
#define RSC_SLOT(n)             (0xd00 + 672 * (n))
#define TCS_WAIT                0x020
#define TCS_CONTROL             0x024
#define TCS_STATUS              0x028
#define TCS_CMD_ENABLE          0x02c
#define TCS_COMMAND(c)          (0x034 + 24 * (c))      /* message id, address, value, status */
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define MSGID_WRITE_WAITED      0x10108                 /* four bytes, an answer wanted, a write */
#define RSC_LAYOUT_MAJOR        3                       /* (rsc_id >> 16) & 255 must be this */
#define PCIE_TCS_COMMANDS       8                       /* command slots per ACTIVE TCS (slot 0 armed 0x7f) */

/*
 * VRM set: the voltage at +0 in millivolts, the enable at +4, the mode at +8;
 * cmd-db marks a regulator's kind in bits 19:16 of its address (4: VRM).
 */
#define VRM_VOLTAGE             0
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7
#define CMD_DB_VRM              4
#define VRM_CLOCK_ENABLE        4

/*
 * The card's seven regulators, in the order the vendor's cnss driver turns
 * them on. Names and ranges from this phone's device tree: the cnss-kiwi
 * node's -supply phandles point at rpmh-regulator-smpc2/smpc3/ldob15/ldoc3/
 * smpc6/smpc1/smpi4, and the voted millivolts sit inside each regulator's
 * min/max (the values Android's boot leaves them at, docs/ACE5-HW.md §1).
 */
static const struct supply {
	const char *name, *what;
	uint32_t mv, min_uv, max_uv;
	bool ldo;
} supplies[] = {
	{ "smpc2",  "card vdd-wlan-aon (SMPS C2)",   980,  500000, 1036000, false },
	{ "smpc3",  "card vdd-wlan-dig (SMPS C3)",   916,  904000, 1064000, false },
	{ "ldob15", "card vdd-wlan-io (LDO B15)",   1800, 1800000, 1800000, true },
	{ "ldoc3",  "card vdd-wlan-io12 (LDO C3)",  1200, 1200000, 1200000, true },
	{ "smpc6",  "card vdd-wlan-rfa1 (SMPS C6)", 1866, 1856000, 2000000, false },
	{ "smpc1",  "card vdd-wlan-rfa2 (SMPS C1)", 1316, 1224000, 1348000, false },
	{ "smpi4",  "card vdd-wlan (SMPS I4)",       938,  300000, 1004000, false },
};

static uint32_t
le32(const uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The regulator's RPMh address by its cmd-db id (s39_find_ldob4's walk). */
static uint32_t
rpmh_address(const char *name)
{
	const uint8_t *db = (const uint8_t *)cmd_db;
	uint8_t id[8] = { 0 };

	if (cmd_db == 0) {
		pcie_log("pcie: power: the command database is not mapped\n");
		return 0;
	}
	if (le32(db + 4) != 0x0c0330dbu) {
		pcie_log("pcie: power: the command database's magic is %#x\n", le32(db + 4));
		return 0;
	}
	for (unsigned i = 0; i < 8 && name[i]; i++) {
		id[i] = (uint8_t)name[i];
	}
	for (unsigned s = 0; s < 8; s++) {
		const uint8_t *d = db + 8 + s * 16;
		unsigned off = d[2] | d[3] << 8, count = d[6] | d[7] << 8;

		if ((d[0] == 0 && d[1] == 0) || count > 512 || off + count * 24 > PCIE_CMD_DB_SIZE - 144) {
			continue;
		}
		for (unsigned i = 0; i < count; i++) {
			const uint8_t *e = db + 144 + off + 24 * i;

			if (memcmp(e, id, sizeof(id)) != 0) {
				continue;
			}
			uint32_t address = le32(e + 16);
			if ((address >> 16 & 0xf) != CMD_DB_VRM) {
				pcie_log("pcie: power: %s is at %#x and is not tagged as a regulator\n", name, address);
				return 0;
			}
			return address;
		}
	}
	pcie_log("pcie: power: the command database has no \"%s\"\n", name);
	return 0;
}

/*
 * One transaction of at most three VRM commands on an idle ACTIVE slot, the
 * slot's previous commands saved and given back -- s39_power's exact path, so
 * nothing is written that the touch driver has not already proven works.
 */
static bool
rpmh_vote(const uint32_t *addresses, const uint32_t *values, unsigned count)
{
	uintptr_t base = rsc + 0xd00, t = 0;
	uint32_t saved[PCIE_TCS_COMMANDS * 4], irq, control;
	uint32_t mask;
	bool done = false;
	unsigned slot, i;

	if (count == 0 || count > 3) {
		return false;
	}
	uint32_t rsc_id = rd(rsc, 0);
	if (((rsc_id >> 16) & 255) != RSC_LAYOUT_MAJOR) {
		pcie_log("pcie: power: the sequencer's layout is not v%d (rsc_id %#x); nothing is voted\n",
		    RSC_LAYOUT_MAJOR, rsc_id);
		return false;
	}
	for (unsigned s = 0; s < RSC_ACTIVE_SLOTS; s++) {
		uintptr_t c = base + s * 672;

		pcie_log("pcie: power: slot %u: status %#x control %#x enable %#x\n", s,
		    rd(c, TCS_STATUS), rd(c, TCS_CONTROL), rd(c, TCS_CMD_ENABLE));
	}
	/* The armed resting state (control ENABLE|TRIGGER left set) is how a finished
	 * ACTIVE slot looks; Linux clears and re-triggers the same slot every vote.
	 * Prefer a disarmed slot, fall back to the first one. */
	for (slot = 0; slot < RSC_ACTIVE_SLOTS; slot++) {
		uintptr_t candidate = base + slot * 672;

		if ((rd(candidate, TCS_CONTROL) & TCS_CONTROL_TRIGGER) == 0) {
			t = candidate;
			break;
		}
	}
	if (t == 0) {
		slot = 0;
		t = base;
		pcie_log("pcie: power: all slots armed; re-triggering slot 0\n");
	}
	pcie_log("pcie: power: voting through slot %u\n", slot);
	mask = 1u << slot;
	irq = rd(rsc, RSC_IRQ_ENABLE);
	control = rd(t, TCS_CONTROL);
	for (i = 0; i < PCIE_TCS_COMMANDS; i++) {
		for (unsigned j = 0; j < 4; j++) {
			saved[i * 4 + j] = rd(t, TCS_COMMAND(i) + j * 4);
		}
	}
	wr(rsc, RSC_IRQ_CLEAR, mask);
	/* Write ALL command slots of the TCS: ours first, the unused ones zeroed.
	 * The armed slot keeps the firmware's boot vote in commands 2..6 (enable
	 * bits stuck on from before); leaving those live makes the trigger run
	 * stale commands to stale addresses and the transfer never completes. */
	for (i = 0; i < PCIE_TCS_COMMANDS; i++) {
		if (i < count) {
			wr(t, TCS_COMMAND(i), MSGID_WRITE_WAITED);
			wr(t, TCS_COMMAND(i) + 4, addresses[i]);
			wr(t, TCS_COMMAND(i) + 8, values[i]);
		} else {
			wr(t, TCS_COMMAND(i), 0);
			wr(t, TCS_COMMAND(i) + 4, 0);
			wr(t, TCS_COMMAND(i) + 8, 0);
		}
	}
	wr(t, TCS_CMD_ENABLE, mask);
	wr(rsc, RSC_IRQ_ENABLE, irq | mask);
	wr(t, TCS_CONTROL, control & ~TCS_CONTROL_ENABLE);
	wr(t, TCS_CONTROL, TCS_CONTROL_ENABLE);
	wr(t, TCS_CONTROL, TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);
	for (i = 0; i < 4000 && (rd(rsc, RSC_IRQ_STATUS) & mask) == 0; i++) {
		delay_us(100);
	}
	done = i < 4000;
	pcie_log("pcie: power: RPMh %s after %u00 us; the commands' status %#x, control %#x, enable %#x, irq %#x\n",
	    done ? "HAS DONE IT" : "HAS NOT SAID DONE", i, rd(t, TCS_COMMAND(0) + 12),
	    rd(t, TCS_CONTROL), rd(t, TCS_CMD_ENABLE), rd(rsc, RSC_IRQ_STATUS));
	/* Linux's tcs_tx_done reclaim: un-trigger, drop the enable mask, clear the
	 * IRQ. The timeout path runs the same un-wedging sequence -- a slot left
	 * armed with a dead transfer blocks every later vote (the 13:2x rounds). */
	wr(t, TCS_CONTROL, TCS_CONTROL_ENABLE);
	wr(t, TCS_CMD_ENABLE, 0);
	wr(rsc, RSC_IRQ_CLEAR, mask);
	wr(rsc, RSC_IRQ_ENABLE, irq);
	if (done) {
		for (unsigned a = 0; a < PCIE_TCS_COMMANDS; a++) {
			for (unsigned j = 0; j < 4; j++) {
				wr(t, TCS_COMMAND(a) + j * 4, saved[a * 4 + j]);
			}
		}
	}
	return done;
}

static bool
supplies_found(uint32_t *found, uint32_t *xo)
{
	static const char *const xos[] = { "rfclka1", "rfclka2", "rfclka3" };
	bool all = true;

	for (unsigned i = 0; i < sizeof(supplies) / sizeof(supplies[0]); i++) {
		found[i] = rpmh_address(supplies[i].name);
		pcie_log("pcie: power: %s for the %s at RPMh %#x\n", supplies[i].name, supplies[i].what, found[i]);
		if (found[i] == 0 || (found[i] >> 16 & 0xf) != CMD_DB_VRM) {
			all = false;
		}
	}
	*xo = 0;
	for (unsigned i = 0; i < sizeof(xos) / sizeof(xos[0]) && *xo == 0; i++) {
		*xo = rpmh_address(xos[i]);
		if (*xo != 0) {
			pcie_log("pcie: power: the card's crystal is %s at RPMh %#x\n", xos[i], *xo);
		}
	}
	if (*xo == 0) {
		pcie_log("pcie: power: no rfclk resource found; the crystal stays as firmware left it\n");
	}
	if (!all) {
		pcie_log("pcie: power: a supply is missing or not tagged as a regulator; nothing is voted\n");
	}
	return all;
}

static bool
supplies_on(const uint32_t *found, uint32_t xo)
{
	uint32_t addresses[3], values[3];

	for (unsigned i = 0; i < sizeof(supplies) / sizeof(supplies[0]); i++) {
		const struct supply *s = &supplies[i];
		bool done;

		if (s->mv * 1000 < s->min_uv || s->mv * 1000 > s->max_uv) {
			pcie_log("pcie: power: %u mV for %s is outside %u-%u uV; refused\n", s->mv, s->name, s->min_uv, s->max_uv);
			return false;
		}
		addresses[0] = found[i] + VRM_VOLTAGE; values[0] = s->mv;
		addresses[1] = found[i] + VRM_ENABLE; values[1] = 1;
		if (s->ldo) {
			addresses[2] = found[i] + VRM_MODE; values[2] = VRM_MODE_LDO_HPM;
			done = rpmh_vote(addresses, values, 3);
		} else {
			done = rpmh_vote(addresses, values, 2);
		}
		pcie_log("pcie: power: %s on at %u mV: %s\n", s->name, s->mv, done ? "confirmed" : "NOT confirmed");
		if (!done) {
			return false;
		}
	}
	if (xo != 0) {
		addresses[0] = xo + VRM_CLOCK_ENABLE; values[0] = 1;
		if (!rpmh_vote(addresses, values, 1)) {
			pcie_log("pcie: power: the card's crystal's vote was not confirmed\n");
			return false;
		}
		pcie_log("pcie: power: the card's crystal is on\n");
	}
	pcie_log("pcie: power: the card's supplies are on\n");
	return true;
}

/* ---- F: PARF (pcie-qcom.c init_2_7_0 and configure_dbi_atu_base) -------------------------------- */

static void
parf_setup(void)
{
	vm_offset_t p = parf;

	wr(p, 0x1000, 0x4);                                     /* DEVICE_TYPE: root complex */
	wr(p, 0x040, rd(p, 0x040) & ~1u);                       /* PHY_CTRL: PHY_TEST_PWR_DOWN off */
	wr(p, 0x350, GIULIA_PCIE_DM_CORE);                      /* DBI_BASE_ADDR_V2 */
	wr(p, 0x354, 0);
	wr(p, 0x634, GIULIA_PCIE_IATU);                         /* ATU_BASE_ADDR */
	wr(p, 0x638, 0);
	wr(p, 0x358, 0x04000000);                               /* SLV_ADDR_SPACE_SIZE: the DT's
	                                                           qcom,slv-addr-space-size -- the vendor
	                                                           writes it (0x358 = 0x4000000) and so
	                                                           does thyme's working kona ladder; we
	                                                           alone wrote 0. */
	wr(p, 0x35c, 0x80000000);
	wr(p, 0x000, rd(p, 0x000) & ~(1u << 29));               /* SYS_CTRL: MAC_PHY_POWERDOWN_IN_P2_D_MUX_EN */
	wr(p, 0x174, rd(p, 0x174) | (1u << 4));                 /* MHI_CLOCK_RESET_CTRL: BYPASS */
	wr(p, 0x020, rd(p, 0x020) & ~(1u << 5));                /* PM_CTRL: REQ_NOT_ENTR_L1 */
	wr(p, 0x1a8, rd(p, 0x1a8) | (1u << 31));                /* AXI_MSTR_WR_ADDR_HALT_V2: EN */
	wr(p, 0x2c48, rd(p, 0x2c48) & ~1u);                     /* vendor CESTA_CLKREQ_SEL off (qcom,pcie-clkreq-offset) */
}

/* ---- I: the root port's header (a part of dw_pcie_setup_rc) ------------------------------------- */

#define DBI_RO_WR_EN            0x8bc
#define DBI_LNKCTL2             0xa0                /* the express capability at 0x70 + 0x30 */
#define PARF_LTSSM              0x1b0
#define LTSSM_EN                (1u << 8)
#define DBI_LINKCTRLSTATUS      0x80
#define LINK_ACTIVE             (1u << 29)
/* The vendor's pre-training writes (pci-msm.c msm_pcie_enable_link / link_train). */
#define PARF_CFG_BITS_3         0x2c4               /* BIT(0): fal10_veto/Qactive override */
#define DBI_GEN3_GEN2_CTRL      0x80c               /* bits [12:8]: the controller's target speed */
#define DBI_LANE_SKEW           0x714               /* BIT(5): dynamic width upscaling */
#define ELBI_SYS_STTS           0x08                /* LTSSM [17:12], XMLH_LINK_UP BIT(10) */
#define XMLH_LINK_UP            (1u << 10)

static void
root_port_setup(void)
{
	vm_offset_t d = dbi;

	wr(d, DBI_RO_WR_EN, rd(d, DBI_RO_WR_EN) | 1);
	wr(d, 0x3c, (rd(d, 0x3c) & 0xffff00ffu) | 0x100);       /* interrupt pin A */
	wr(d, 0x18, (rd(d, 0x18) & 0xff000000u) | 0x00ff0100);  /* buses: primary 0, secondary 1, subordinate 0xff */
	wr(d, 0x04, rd(d, 0x04) | 0x107);                       /* IO, memory, bus master, SERR */
	wr(d, 0x08, (rd(d, 0x08) & 0x0000ffffu) | (0x0604u << 16));     /* class: PCI bridge */
	wr(d, DBI_LNKCTL2, (rd(d, DBI_LNKCTL2) & ~0xfu) | 1);   /* target speed Gen1, for this first batch */
	wr(d, DBI_RO_WR_EN, rd(d, DBI_RO_WR_EN) & ~1u);
}

/* ---- step 3: the whole power-up ----------------------------------------------------------------- */

static int
power_up(void)
{
	uint32_t found[sizeof(supplies) / sizeof(supplies[0])], xo = 0;
	uint32_t ltssm = 0xff;
	bool up = false;
	int error;

	pcie_log("pcie: step 3 power-up\n");
	/* The card's CLKREQ# reaches the controller only through the pad
	 * function: the vendor's active pinctrl (pcie0_clkreq_default) muxes
	 * gpio95 to pcie0_clk_req_n, while Android's sleep state leaves it a
	 * GPIO. Function 1 on pin 95, pull-up, input (no output enable) --
	 * sm8650 CFG: pull [1:0], function [5:2], OE bit 9. */
	wr(tlmm, GIULIA_PCIE_CLKREQ_GPIO * 0x1000,
	    (rd(tlmm, GIULIA_PCIE_CLKREQ_GPIO * 0x1000) & ~0x3ffu) | 0x7u);
	pcie_log("pcie: clkreq pin muxed to pcie0_clk_req_n (cfg %#x)\n",
	    rd(tlmm, GIULIA_PCIE_CLKREQ_GPIO * 0x1000));
	pcie_pause(pcie_pause_s, "PERST and the card's enable driven low");
	gpio_out(GIULIA_PCIE_PERST_GPIO, false);
	gpio_out(GIULIA_CNSS_WLAN_EN_GPIO, false);
	delay_us(1000);

	pcie_pause(pcie_pause_s, "the supplies");
	if (pcie_supplies) {
		if (!supplies_found(found, &xo)) {
			return EINVAL;
		}
		if (!supplies_on(found, xo)) {
			return EINVAL;
		}
	} else {
		pcie_log("pcie: power: supplies skipped (pcie_supplies=0)\n");
	}

	pcie_pause(pcie_pause_s, "power domains and clocks");
	if (!pcie_gdsc) {
		pcie_log("pcie: power: GDSC stage skipped (pcie_gdsc=0)\n");
		return 0;
	}
	error = gdsc_on(GIULIA_GCC_PCIE0_GDSCR, GIULIA_GCC_PCIE0_GDSC_VOTE_BIT);
	pcie_log("pcie: gdsc core %s (%#x)\n", error ? "failed" : "on", rd(gcc, GIULIA_GCC_PCIE0_GDSCR));
	if (error) {
		return error;
	}
	error = gdsc_on(GIULIA_GCC_PCIE0_PHY_GDSCR, GIULIA_GCC_PCIE0_PHY_GDSC_VOTE_BIT);
	pcie_log("pcie: gdsc phy %s (%#x)\n", error ? "failed" : "on", rd(gcc, GIULIA_GCC_PCIE0_PHY_GDSCR));
	if (error) {
		return error;
	}
	error = clocks_on();
	if (error) {
		return error;
	}
	pcie_log("pcie: clocks voted (%#x)\n", rd(gcc, GIULIA_GCC_PCIE_VOTE));

	/* The domains are on and the clocks running: only now create the PCIe
	 * window mappings (see pcie_thread's note on speculative reads). */
	parf = ml_io_map(GIULIA_PCIE_PARF, GIULIA_PCIE_PARF_SIZE);
	phy = ml_io_map(GIULIA_PCIE_PHY, GIULIA_PCIE_PHY_SIZE);
	dbi = ml_io_map(GIULIA_PCIE_DM_CORE, 0x2000);
	sm = ml_io_map(GIULIA_PCIE_PCIE_SM, GIULIA_PCIE_PCIE_SM_SIZE);
	if (!parf || !phy || !dbi || !sm) {
		pcie_log("pcie: map failed parf=%p phy=%p dbi=%p sm=%p\n",
		    (void *)parf, (void *)phy, (void *)dbi, (void *)sm);
		return ENOMEM;
	}

	pcie_pause(pcie_pause_s, "resets, then the controller's glue");
	reset_pulse(GIULIA_GCC_PCIE0_CORE_BCR);
	reset_pulse(GIULIA_GCC_PCIE0_PHY_BCR);
	parf_setup();
	pcie_log("pcie: resets pulsed, parf set up\n");

	pcie_pause(pcie_pause_s, "the PHY");
	for (unsigned i = 0; i < sizeof(giulia_pcie_phy) / sizeof(giulia_pcie_phy[0]); i++) {
		wr(phy, giulia_pcie_phy[i].offset, giulia_pcie_phy[i].value);
		if (giulia_pcie_phy[i].delay_us) {
			delay_us(giulia_pcie_phy[i].delay_us);
		}
	}
	wr(gcc, GIULIA_GCC_PCIE_VOTE, rd(gcc, GIULIA_GCC_PCIE_VOTE) | 0x10u); /* PIPE */
	error = poll(phy, GIULIA_PCIE_PHY_STATUS, 1u << GIULIA_PCIE_PHY_STATUS_BIT, 1u << GIULIA_PCIE_PHY_STATUS_BIT, 100000);
	pcie_log("pcie: phy %s (%#x)\n", error ? "not ready" : "ready", rd(phy, GIULIA_PCIE_PHY_STATUS));
	if (error) {
		return error;
	}
	delay_us(GIULIA_PCIE_EP_LATENCY_MS * 1000);

	pcie_pause(pcie_pause_s, "the card's enable");
	/* The card's reference clock: enable the TCSR clkref gate before the
	 * card comes out of reset -- without its refclk the card never answers
	 * and the LTSSM stays in 0. */
	tcsrcc = ml_io_map(GIULIA_TCSRCC, GIULIA_TCSRCC_SIZE);
	if (tcsrcc == 0) {
		pcie_log("pcie: power: the TCSR clock page cannot be mapped\n");
		return ENOMEM;
	}
	uint32_t clkref = rd(tcsrcc, GIULIA_TCSRCC_PCIE0_CLKREF_EN);
	wr(tcsrcc, GIULIA_TCSRCC_PCIE0_CLKREF_EN, clkref | 1u);
	pcie_log("pcie: power: the card's refclk gate pre=%#x post=%#x\n", clkref,
	    rd(tcsrcc, GIULIA_TCSRCC_PCIE0_CLKREF_EN));
	gpio_out(GIULIA_CNSS_WLAN_EN_GPIO, true);
	delay_us(100000);                       /* 100 ms from the enable to PERST (PCIE_T_PVPERL_MS) */
	pcie_log("pcie: wlan_en high; sw-ctrl pins read %u %u\n",
	    rd(tlmm, GIULIA_CNSS_SW_CTRL_GPIO * 0x1000 + 4) & 1, rd(tlmm, GIULIA_CNSS_WLAN_SW_GPIO * 0x1000 + 4) & 1);
	root_port_setup();
	/* The vendor driver's pre-training writes that our upstream-2_7_0 port
	 * lacked (pci-msm.c msm_pcie_enable_link, before PERST release):
	 * de-assert Qactive -- clear the fal10_veto override and set the
	 * dynamic-width-upscale bit -- then the controller's own Gen1 target
	 * and the lane-skew override bit. */
	wr(parf, PARF_CFG_BITS_3, (rd(parf, PARF_CFG_BITS_3) & ~1u) | (1u << 8));
	wr(dbi, DBI_GEN3_GEN2_CTRL, (rd(dbi, DBI_GEN3_GEN2_CTRL) & ~0x1f00u) | (1u << 8));
	wr(dbi, DBI_LANE_SKEW, rd(dbi, DBI_LANE_SKEW) | (1u << 5));
	/* The last structural write the vendor does before training and our
	 * upstream port did not: the full SYS_CTRL value (pci-msm.c
	 * enable_link). Our old write only cleared MAC_PHY_POWERDOWN_IN_P2;
	 * this replaces whatever state Android left with the vendor's. */
	pcie_log("pcie: vendor pre-train: cfg_bits_3 %#x gen3_gen2 %#x lane_skew %#x, sys_ctrl %#x -> 0x365e\n",
	    rd(parf, PARF_CFG_BITS_3), rd(dbi, DBI_GEN3_GEN2_CTRL), rd(dbi, DBI_LANE_SKEW),
	    rd(parf, 0));
	wr(parf, 0, 0x365e);
	delay_us(50000);

	gpio_out(GIULIA_PCIE_PERST_GPIO, true);
	delay_us(1000);
	pcie_log("pcie: perst released; perst in=%u wlan_en in=%u clkreq in=%u wake in=%u\n",
	    rd(tlmm, GIULIA_PCIE_PERST_GPIO * 0x1000 + 4) & 1,
	    rd(tlmm, GIULIA_CNSS_WLAN_EN_GPIO * 0x1000 + 4) & 1,
	    rd(tlmm, GIULIA_PCIE_CLKREQ_GPIO * 0x1000 + 4) & 1,
	    rd(tlmm, GIULIA_PCIE_WAKE_GPIO * 0x1000 + 4) & 1);
	elbi = ml_io_map(GIULIA_PCIE_ELBI, GIULIA_PCIE_ELBI_SIZE);
	if (elbi == 0) {
		pcie_log("pcie: power: the ELBI page cannot be mapped\n");
		return ENOMEM;
	}
	wr(sm, GIULIA_PCIE_SM_START_OFFSET, 1u);       /* qcom,pcie-sm-start-offset: start the machine */
	wr(parf, PARF_LTSSM, rd(parf, PARF_LTSSM) | LTSSM_EN);
	for (unsigned i = 0; i < 90 && !up; i++) {
		uint32_t state = rd(parf, PARF_LTSSM) & 0x3f;
		uint32_t el = rd(elbi, ELBI_SYS_STTS);

		if (state != ltssm) {
			pcie_log("pcie: link training, parf state %#x elbi %#x after %u ms\n",
			    state, el, i * 10);
			ltssm = state;
		}
		up = (rd(dbi, DBI_LINKCTRLSTATUS) & LINK_ACTIVE) != 0;
		if (!up) {
			delay_us(10000);
		}
	}
	pcie_log("pcie: link %s (parf %#x elbi %#x, link status %#x, speed=%u width=%u)\n", up ? "up" : "still down",
	    rd(parf, PARF_LTSSM) & 0x3f, rd(elbi, ELBI_SYS_STTS), rd(dbi, DBI_LINKCTRLSTATUS),
	    (rd(dbi, DBI_LINKCTRLSTATUS) >> 16) & 0xf, (rd(dbi, DBI_LINKCTRLSTATUS) >> 20) & 0x3f);
	if (!up) {
		/* The only window into the detect circuit: sweep the PARF test
		 * bus the way the vendor's pcie_parf_dump does (SYS_CTRL bits
		 * [23:16] select, 0xe4 reads; the DT counts 185 selectors) and
		 * log the nonzero ones, plus the vendor's own debug-register
		 * lists (qcom,parf-debug-reg, qcom,phy-debug-reg). */
		static const uint32_t parf_dbg[] = {
		    0x24u, 0x28u, 0x224u, 0x500u, 0x4d0u, 0x4d4u, 0x3c0u, 0x630u, 0x230u
		};
		static const uint32_t phy_dbg[] = {
		    0x1ccu, 0x1d0u, 0x1d4u, 0x1d8u, 0x1dcu, 0x1e0u, 0x1e4u, 0x1f8u, 0xed0u, 0x16d0u
		};
		uint32_t sys = rd(parf, 0);

		for (unsigned k = 0; k < sizeof(parf_dbg) / sizeof(parf_dbg[0]); k++) {
			pcie_log("pcie: parf[%#x] = %#x\n", parf_dbg[k], rd(parf, parf_dbg[k]));
		}
		for (unsigned k = 0; k < sizeof(phy_dbg) / sizeof(phy_dbg[0]); k++) {
			pcie_log("pcie: phy[%#x] = %#x\n", phy_dbg[k], rd(phy, phy_dbg[k]));
		}
		for (uint32_t sel = 0; sel <= 0xb9u; sel++) {
			uint32_t bus;

			wr(parf, 0, (sys & ~0xff0000u) | (sel << 16));
			bus = rd(parf, 0xe4);
			if (bus != 0) {
				pcie_log("pcie: testbus[%u] = %#x\n", sel, bus);
			}
		}
		wr(parf, 0, sys);
	}
	if (up) {
		delay_us(100000);
	}
	return up ? 0 : ETIMEDOUT;
}

static void
pcie_thread(__unused void *arg, __unused wait_result_t wait)
{
	int error = 0;

	pcie_log("pcie: thread start, step %d pause %u s\n", pcie_step, pcie_pause_s);
	gcc = ml_io_map(0x100000, 0x1f4200);
	tlmm = ml_io_map(GIULIA_TLMM_GPIO, GIULIA_TLMM_GPIO_SIZE);
	if (!gcc || !tlmm) {
		pcie_log("pcie: gcc/tlmm map failed gcc=%p tlmm=%p\n", (void *)gcc, (void *)tlmm);
		goto out;
	}
	if (pcie_step == 1) {
		survey_gcc_tlmm();
		goto out;
	}

	/* The PCIe windows (PARF/PHY/DBI/SM) are mapped ONLY when their clocks
	 * and power are confirmed running -- never from the boot path. A Device
	 * mapping of an unclocked window invites speculative reads that hang the
	 * bus (every wedge so far mapped the windows; step 1, which does not,
	 * always survived). power_up maps them right after clocks_on(). */
	if (pcie_step == 2) {
		survey_step2();
	} else if (pcie_step == 3) {
		rsc = ml_io_map(PCIE_RSC_PHYS, PCIE_RSC_SIZE);
		cmd_db = ml_io_map_wcomb(PCIE_CMD_DB_PHYS, PCIE_CMD_DB_SIZE);
		if (!rsc || !cmd_db) {
			pcie_log("pcie: map failed rsc=%p cmd_db=%p\n", (void *)rsc, (void *)cmd_db);
			goto out;
		}
		error = power_up();
		if (error == 0 && parf != 0) {
			survey_windows();
		}
	} else {
		pcie_log("pcie: step %d not implemented\n", pcie_step);
	}
out:
	pcie_log("pcie: step %d finished rc=%d\n", pcie_step, error);
	thread_terminate(current_thread());
}

void
giulia_pcie_init(void)
{
	uint32_t step = 0;
	thread_t thread;

	pcie_log("pcie: init entered\n");
	if (!PE_parse_boot_argn("pcie_step", &step, sizeof(step)) || step == 0) {
		pcie_log("pcie: no pcie_step\n");
		return;
	}
	pcie_log("pcie: init step=%u\n", step);
	pcie_step = (int)step;
	PE_parse_boot_argn("pcie_pause", &pcie_pause_s, sizeof(pcie_pause_s));
	PE_parse_boot_argn("pcie_supplies", &pcie_supplies, sizeof(pcie_supplies));
	PE_parse_boot_argn("pcie_gdsc", &pcie_gdsc, sizeof(pcie_gdsc));
	if (kernel_thread_start(pcie_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		pcie_log("pcie: thread start FAILED\n");
	}
}

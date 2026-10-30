/* OnePlus Ace 5 / giulia PCIe0 host controller (WCN7850's bus).
 * Explicit pcie_step=N opt-in, in the same style as giulia_s3910.c.
 *
 *   pcie_step=1  read-only survey: GDSC/clock/reset state, PARF/PHY/DBI
 *                registers and the PCIe pins. Writes nothing.
 *   pcie_step=2  power-up per Linux pcie-qcom: GDSCs, clock votes, core and
 *                PHY resets, the vendor PHY sequence from the device tree,
 *                PERST/WLAN_EN, then poll for link training. Never writes the
 *                endpoint: config space is M2.
 *
 * Register windows, pins and the PHY sequence come from giulia_pcie_regs.h,
 * generated from this phone's device tree. GCC offsets are from Linux's
 * gcc-sm8650.c (see the header).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include "giulia_pcie_regs.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);

static uintptr_t parf, phy, dbi, iatu, conf, sm, gcc, tlmm;
static int pcie_step;

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
	printf("pcie: poll timeout %#x=%#x (want %#x mask %#x)\n", offset, value, want, mask);
	return ETIMEDOUT;
}

static void
gpio_out(unsigned pin, bool high)
{
	/* TLMM_GPIO_CFG: function 0-1, output enable bit 2, drive 6-8, pull 9-10.
	 * TLMM_GPIO_IN_OUT bit 1 is the level. Latch it before enabling the output. */
	uint32_t value = rd(tlmm, pin * 0x1000 + 4);

	wr(tlmm, pin * 0x1000 + 4, high ? (value | 2u) : (value & ~2u));
	value = rd(tlmm, pin * 0x1000);
	wr(tlmm, pin * 0x1000, (value & ~0x7ffu) | 0x4u | (6u << 6));
}

static int
gdsc_on(uint32_t offset)
{
	uint32_t value = rd(gcc, offset);

	wr(gcc, offset, value | 1u);
	return poll(gcc, offset, 0x80000000u, 0x80000000u, 10000);
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

	wr(gcc, GIULIA_GCC_PCIE_VOTE, vote | 0x2fu); /* all but PIPE */
	for (unsigned i = 0; i < sizeof(halt) / sizeof(halt[0]); i++) {
		int error = poll(gcc, halt[i], 0x80000000u, 0, 10000);
		if (error) {
			return error;
		}
	}
	return 0;
}

static void
reset_pulse(uint32_t offset)
{
	wr(gcc, offset, 1u);
	delay_us(1000);
	wr(gcc, offset, 0u);
}

static void
survey(void)
{
	printf("pcie: step 1 read-only survey\n");
	printf("pcie: gcc gdsc=%#x phy_gdsc=%#x vote=%#x\n",
	    rd(gcc, GIULIA_GCC_PCIE0_GDSCR), rd(gcc, GIULIA_GCC_PCIE0_PHY_GDSCR),
	    rd(gcc, GIULIA_GCC_PCIE_VOTE));
	printf("pcie: gcc halt slv=%#x mstr=%#x ahb=%#x aux=%#x q2a=%#x pipe=%#x\n",
	    rd(gcc, GIULIA_GCC_PCIE0_SLV_HALT), rd(gcc, GIULIA_GCC_PCIE0_MSTR_HALT),
	    rd(gcc, GIULIA_GCC_PCIE0_AHB_HALT), rd(gcc, GIULIA_GCC_PCIE0_AUX_HALT),
	    rd(gcc, GIULIA_GCC_PCIE0_Q2A_HALT), rd(gcc, GIULIA_GCC_PCIE0_PIPE_HALT));
	printf("pcie: gcc bcr core=%#x phy=%#x\n",
	    rd(gcc, GIULIA_GCC_PCIE0_CORE_BCR), rd(gcc, GIULIA_GCC_PCIE0_PHY_BCR));
	printf("pcie: parf %#x %#x %#x %#x\n", rd(parf, 0), rd(parf, 0x40),
	    rd(parf, 0x1000), rd(parf, 0x2c00));
	printf("pcie: phy status=%#x\n", rd(phy, GIULIA_PCIE_PHY_STATUS));
	printf("pcie: dbi id=%#x class=%#x link=%#x cap=%#x ctrl=%#x\n",
	    rd(dbi, 0), rd(dbi, 8), rd(dbi, 0x80), rd(dbi, 0xa0), rd(dbi, 0x78));
	printf("pcie: gpio perst=%#x/%#x clkreq=%#x/%#x wake=%#x/%#x\n",
	    rd(tlmm, GIULIA_PCIE_PERST_GPIO * 0x1000), rd(tlmm, GIULIA_PCIE_PERST_GPIO * 0x1000 + 4),
	    rd(tlmm, GIULIA_PCIE_CLKREQ_GPIO * 0x1000), rd(tlmm, GIULIA_PCIE_CLKREQ_GPIO * 0x1000 + 4),
	    rd(tlmm, GIULIA_PCIE_WAKE_GPIO * 0x1000), rd(tlmm, GIULIA_PCIE_WAKE_GPIO * 0x1000 + 4));
	printf("pcie: cnss wlan_en=%#x/%#x sw_ctrl=%#x/%#x\n",
	    rd(tlmm, GIULIA_CNSS_WLAN_EN_GPIO * 0x1000), rd(tlmm, GIULIA_CNSS_WLAN_EN_GPIO * 0x1000 + 4),
	    rd(tlmm, GIULIA_CNSS_SW_CTRL_GPIO * 0x1000), rd(tlmm, GIULIA_CNSS_SW_CTRL_GPIO * 0x1000 + 4));
	printf("pcie: sm %#x %#x %#x %#x\n", rd(sm, 0), rd(sm, 4), rd(sm, 8), rd(sm, 0xc));
	printf("pcie: step 1 done\n");
}

static int
power_up(void)
{
	int error;

	printf("pcie: step 2 power-up\n");
	gpio_out(GIULIA_PCIE_PERST_GPIO, false);
	gpio_out(GIULIA_CNSS_WLAN_EN_GPIO, false);
	delay_us(1000);

	error = gdsc_on(GIULIA_GCC_PCIE0_GDSCR);
	printf("pcie: gdsc core %s (%#x)\n", error ? "failed" : "on",
	    rd(gcc, GIULIA_GCC_PCIE0_GDSCR));
	if (error) {
		return error;
	}
	error = gdsc_on(GIULIA_GCC_PCIE0_PHY_GDSCR);
	printf("pcie: gdsc phy %s (%#x)\n", error ? "failed" : "on",
	    rd(gcc, GIULIA_GCC_PCIE0_PHY_GDSCR));
	if (error) {
		return error;
	}

	error = clocks_on();
	if (error) {
		return error;
	}
	printf("pcie: clocks voted (%#x)\n", rd(gcc, GIULIA_GCC_PCIE_VOTE));

	reset_pulse(GIULIA_GCC_PCIE0_CORE_BCR);
	reset_pulse(GIULIA_GCC_PCIE0_PHY_BCR);
	printf("pcie: resets pulsed\n");

	for (unsigned i = 0; i < sizeof(giulia_pcie_phy) / sizeof(giulia_pcie_phy[0]); i++) {
		wr(phy, giulia_pcie_phy[i].offset, giulia_pcie_phy[i].value);
		if (giulia_pcie_phy[i].delay_us) {
			delay_us(giulia_pcie_phy[i].delay_us);
		}
	}
	wr(gcc, GIULIA_GCC_PCIE_VOTE, rd(gcc, GIULIA_GCC_PCIE_VOTE) | 0x10u); /* PIPE */
	error = poll(phy, GIULIA_PCIE_PHY_STATUS, 1u << GIULIA_PCIE_PHY_STATUS_BIT, 0, 100000);
	printf("pcie: phy %s (%#x)\n", error ? "not ready" : "ready",
	    rd(phy, GIULIA_PCIE_PHY_STATUS));
	if (error) {
		return error;
	}
	delay_us(GIULIA_PCIE_EP_LATENCY_MS * 1000);

	gpio_out(GIULIA_PCIE_PERST_GPIO, true);
	delay_us(1000);
	gpio_out(GIULIA_CNSS_WLAN_EN_GPIO, true);
	printf("pcie: perst released, wlan_en high\n");

	error = poll(dbi, 0x80, 1u << 29, 1u << 29, 100000);
	uint32_t link = rd(dbi, 0x80);
	printf("pcie: link %s status=%#x speed=%u width=%u\n",
	    error ? "down" : "up", link, (link >> 16) & 0xf, (link >> 20) & 0x3f);
	return error;
}

static void
pcie_thread(__unused void *arg, __unused wait_result_t wait)
{
	int error = 0;

	parf = ml_io_map(GIULIA_PCIE_PARF, GIULIA_PCIE_PARF_SIZE);
	phy = ml_io_map(GIULIA_PCIE_PHY, GIULIA_PCIE_PHY_SIZE);
	dbi = ml_io_map(GIULIA_PCIE_DM_CORE, 0x2000);
	iatu = ml_io_map(GIULIA_PCIE_IATU, GIULIA_PCIE_IATU_SIZE);
	conf = ml_io_map(GIULIA_PCIE_CONF, 0x1000);
	sm = ml_io_map(GIULIA_PCIE_PCIE_SM, GIULIA_PCIE_PCIE_SM_SIZE);
	gcc = ml_io_map(0x100000, 0x1f4200);
	tlmm = ml_io_map(0xf000000, 0x100000);
	if (!parf || !phy || !dbi || !iatu || !conf || !sm || !gcc || !tlmm) {
		printf("pcie: map failed parf=%p phy=%p dbi=%p iatu=%p conf=%p sm=%p gcc=%p tlmm=%p\n",
		    (void *)parf, (void *)phy, (void *)dbi, (void *)iatu,
		    (void *)conf, (void *)sm, (void *)gcc, (void *)tlmm);
		goto out;
	}

	if (pcie_step == 1) {
		survey();
	} else if (pcie_step == 2) {
		error = power_up();
	} else {
		printf("pcie: step %d not implemented\n", pcie_step);
	}
	printf("pcie: step %d finished rc=%d\n", pcie_step, error);
out:
	thread_terminate(current_thread());
}

void
giulia_pcie_init(void)
{
	uint32_t step = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("pcie_step", &step, sizeof(step)) || step == 0) {
		return;
	}
	pcie_step = (int)step;
	if (kernel_thread_start(pcie_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

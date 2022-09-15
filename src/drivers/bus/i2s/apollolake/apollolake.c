/*
 * Copyright (C) 2016 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <libpayload.h>

#include "base/container_of.h"
#include "drivers/bus/i2s/i2s.h"
#include "drivers/bus/i2s/apollolake/apollolake.h"
#include "drivers/bus/i2s/apollolake/apollolake-regs.h"
#include <arch/io.h>

#define LPE_SSP_FIFO_SIZE	16

/*
 * i2s_ensable -  enable SSP device
 * @regs: pointer to registers
 * Writes SSP register to enable the device
 */
static void i2s_enable(AplI2sRegs *regs)
{
	set_SSCR0_reg(regs, SSE);
	set_SSTSA_reg(regs, TXEN);
}

/**
 * i2s_disable -  disable SSP device
 * @regs: pointer to registers
 *
 * Writes SSP register to disable the device
 */
static void i2s_disable(AplI2sRegs *regs)
{
	clear_SSCR0_reg(regs, SSE);
	clear_SSTSA_reg(regs, TXEN);
}

/*
 * calculate_sspsp - Calculate sspsp register
 * @settings: config settings
 *
 * Calculate sspsp register.
 */
static uint32_t calculate_sspsp(const AplI2sSettings *settings)
{
	uint32_t sspsp = 0;

	sspsp = SSPSP_reg(FSRT, NEXT_FRMS_ASS_WITH_LSB_PREVIOUS_FRM) |
		SSPSP_reg(SFRMWDTH, settings->ssp_psp_T6) |
		SSPSP_reg(EDMYSTOP, settings->ssp_psp_T4);
	return sspsp;
}


/*
 * calculate_sscr0: Calculate sscr0 register
 * @settings: config settings
 * @bps: bits per sample
 *
 * Calculate sscr0 register
 */
static uint32_t calculate_sscr0(const AplI2sSettings *settings, int bps)
{
	uint32_t sscr0 = 0;

	if (bps > 16)
		sscr0 = SSCR0_reg(DSS, SSCR0_DataSize(bps - 16)) |
			SSCR0_reg(EDSS, EDSS_17_32_BITS);
	else
		sscr0 = SSCR0_reg(DSS, SSCR0_DataSize(bps)) |
			SSCR0_reg(EDSS, EDSS_4_16_BITS);

	sscr0 |= SSCR0_reg(MOD, settings->mode) |
		 SSCR0_reg(FRF, PSP_FORMAT) |
		 SSCR0_reg(SCR, 0x7) |
		 SSCR0_reg(RIM, SSP_FIFO_INT_DISABLE) |
		 SSCR0_reg(TIM, SSP_FIFO_INT_DISABLE) |
		 SSCR0_reg(ECS, DIV_DISABLE) |
		 SSCR0_reg(NCS, NETWORK_CLOCK_DISABLE) |
		 SSCR0_reg(FRDC,
			SSCR0_SlotsPerFrm(settings->frame_rate_divider_ctrl));
	return sscr0;
}

/**
 * calculate_sscr1 - Calculate sscr1 register.
 * @settings: config settings
 *
 * Calculate sscr1 register.
 */
static uint32_t calculate_sscr1(const AplI2sSettings *settings)
{
	uint32_t sscr1 = 0;

	sscr1 = SSCR1_reg(TTE, TXD_TRISTATE_ON) |
		SSCR1_reg(TTELP, TXD_TRISTATE_LAST_PHASE_ON) |
		SSCR1_reg(RSRE, 1) |
		SSCR1_reg(TSRE, 1) |
		SSCR1_reg(TRAIL, 1);
	return sscr1;
}

/*
 * calculate_ssioc- Calculate ssioc register.
 */
static uint32_t calculate_ssioc(void)
{
	uint32_t ssioc = 0;

	ssioc = SSIOC_reg(SCOE, SSP_ENABLE_CLOCK);
	return ssioc;
}

/*
 * calculate_sscr2 - Calculate sscr2 register.
 */
static uint32_t calculate_sscr2(void)

{
	uint32_t sscr2 = 0;

	sscr2 |= SSCR2_reg(SDFD, SSP_DMA_FINISH_DISABLE) |
		 SSCR2_reg(TURM1, TRANSMIT_UNDERRUN_MODE_1_ENABLE);

	return sscr2;
}

/*
* Power on DSP and Enable SSP for data transmission
*/

static int enable_DSP_SSP(AplI2s *bus)
{
	/* Power On Audio Controller and wait till its powered on */
	writel(0x1, bus->lpe_bar0 + POWER_OFFSET);
	for (int i = 0; i < RETRY_COUNT; i++) {
		if (readl(bus->lpe_bar0 + POWER_OFFSET) == 0x1)
			break;
		mdelay(1);
	}

	if (readl(bus->lpe_bar0 + POWER_OFFSET) != 0x1)
		return -1;

	/* Enable the ADSP bar fuctionality */
	writel(ENABLE_ADSP_BAR, bus->lpe_bar0 + BAR_OFFSET);
	for (int i = 0; i < RETRY_COUNT; i++) {
		if (readl(bus->lpe_bar0 + BAR_OFFSET) == ENABLE_ADSP_BAR)
			break;
		mdelay(1);
	}
	if (readl(bus->lpe_bar0 + BAR_OFFSET) != ENABLE_ADSP_BAR)
		return -1;

	/* power on dsp core to access ssp registeres*/
	writel(DSP_POWER_ON, bus->lpe_bar4 + DSP_POWER_OFFSET);
	/* wait till the DSP powers on */
	for (int i = 0; i < RETRY_COUNT; i++) {
		if (readl(bus->lpe_bar4 + DSP_POWER_OFFSET) == DSP_POWERED_UP)
			break;
		mdelay(1);
	}
	if (readl(bus->lpe_bar4 + DSP_POWER_OFFSET) != DSP_POWERED_UP)
		return -1;

	/* setup the clock to disable dynamic clock gating of SSP */
	writel(DISABLE_CLOCK_GATING, bus->lpe_bar4 + CLOCK_GATING_OFFSET);
	for (int i = 0; i < RETRY_COUNT; i++) {
		if (readl(bus->lpe_bar4 + CLOCK_GATING_OFFSET) ==
						DISABLED_CLOCK_GATING)
			break;
		mdelay(1);
	}
	if (readl(bus->lpe_bar4 + CLOCK_GATING_OFFSET) != DISABLED_CLOCK_GATING)
		return -1;

	return 0;
}
/*
 * set_ssp_i2s_hw - Configure SSP driver according to settings.
 * @regs: pointer to registers
 * @shim: pointer to shim registers
 * @settings: config settings
 * @bps: bits per sample
 *
 * Configure SSP driver according to settings.
 */
static void set_ssp_i2s_hw(AplI2sRegs *regs, AplI2sRegs *shim,
	const AplI2sSettings *settings, int bps)
{
	uint32_t sscr0;
	uint32_t sscr1;
	uint32_t sscr2;
	uint32_t sscr3;
	uint32_t sstsa;
	uint32_t ssrsa;
	uint32_t sspsp;
	uint32_t sspsp2 = 0;
	uint32_t sssr = 0;
	uint32_t ssioc;

	sscr0 = calculate_sscr0(settings, bps);
	sscr1 = calculate_sscr1(settings);
	sscr2 = calculate_sscr2();
	sscr3 = 0;
	sspsp = calculate_sspsp(settings);
	sstsa = SSTSA_reg(TTSA, settings->ssp_active_tx_slots_map);
	ssrsa = SSRSA_reg(RTSA, settings->ssp_active_rx_slots_map);
	ssioc = calculate_ssioc();

	write_SSCR0(sscr0, regs);
	write_SSCR1(sscr1, regs);
	write_SSCR2(sscr2, regs);
	write_SSCR3(sscr3, regs);
	write_SSPSP(sspsp, regs);
	write_SSPSP2(sspsp2, regs);
	write_SSTSA(sstsa, regs);
	write_SSRSA(ssrsa, regs);
	write_SSIOC(ssioc, regs);

	/* Clear status */
	write_SSSR(sssr, regs);

	/* set the time out for the reception */
	write_SSTO(SSP_TIMEOUT, regs);
}

/*
 * apl_i2s_init - Initialize I2s.
 * @bus: i2s config structure
 *
 * Initialize I2s.
 */
static int apl_i2s_init(AplI2s *bus)
{
	if (enable_DSP_SSP(bus))
		return -1;
	i2s_disable(bus->regs);
	set_ssp_i2s_hw(bus->regs,
		       bus->shim,
		       bus->settings,
		       bus->bits_per_sample);
	return 0;
}

/*
 * apl_i2s_send - Send audio samples to I2s controller.
 * @me: I2sOps structure
 * @data: Audio samples
 * @length: Number of samples
 *
 * Send audio samples to I2s controller.
*/
static int apl_i2s_send(I2sOps *me, unsigned int *data, unsigned int length)
{
	int i;
	uint64_t start;
	AplI2s *bus = container_of(me, AplI2s, ops);
	struct AplI2sRegs *i2s_reg = bus->regs;

	if (!bus->initialized) {
		if (apl_i2s_init(bus))
			return -1;
		bus->initialized = 1;
	}

	if (length < LPE_SSP_FIFO_SIZE) {
		printf("%s : Invalid data size\n", __func__);
		return -1;
	}

	gpio_set(bus->sdmode_gpio, 1);

	for (i = 0; i < LPE_SSP_FIFO_SIZE; i++)
		writel(*data++, &i2s_reg->ssdr);

	i2s_enable(bus->regs);
	length -= LPE_SSP_FIFO_SIZE;

	while (length > 0) {
		start = timer_us(0);
		if (read_SSSR(bus->regs) & 0x4) {
			writel(*data++, &i2s_reg->ssdr);
			length--;
		} else {
			if (timer_us(start) > 100000) {
				i2s_disable(bus->regs);
				gpio_set(bus->sdmode_gpio, 0);
				printf("I2S Transfer Timeout\n");
				return -1;
			}
		}
	}

	mdelay(1);
	gpio_set(bus->sdmode_gpio, 0);
	i2s_disable(bus->regs);
	return 0;
}

/*
 * new_apl_i2s - Allocate new I2s data structures.
 * @settings: Apollolake codec settigns
 * @bps: Bits per sample
 *
 * Allocate new I2s data structures.
 */
AplI2s *new_apl_i2s(const AplI2sSettings *settings, int bps, GpioOps *sdmode)
{
	AplI2s *bus = xzalloc(sizeof(*bus));
	pcidev_t lpe_pcidev = PCI_DEV(0, AUDIO_DEV, 0);

	bus->lpe_bar0 = pci_read_config32(lpe_pcidev, REG_BAR0) & (~0xf);
	bus->lpe_bar4 = pci_read_config32(lpe_pcidev, REG_BAR4) & (~0xf);
	bus->ops.send = &apl_i2s_send;
	bus->regs = (AplI2sRegs *)(bus->lpe_bar4 + APL_SSP5_START_ADDRESS);
	bus->shim = (AplI2sRegs *)(bus->lpe_bar4 + APL_SSP5_SHIM_START_ADDRESS);
	bus->settings = settings;
	bus->bits_per_sample = bps;
	bus->sdmode_gpio = sdmode;

	return bus;
}

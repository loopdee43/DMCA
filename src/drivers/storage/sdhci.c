/*
 * Copyright 2011, Marvell Semiconductor Inc.
 * Lei Wen <leiwen@marvell.com>
 *
 * See file CREDITS for list of people who contributed to this
 * project.
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
 *
 * Back ported to the 8xx platform (from the 8260 platform) by
 * Murray.Jensen@cmst.csiro.au, 27-Jan-01.
 */

#include <libpayload.h>

#include "drivers/storage/blockdev.h"
#include "drivers/storage/mmc.h"
#include "drivers/storage/sdhci.h"

static void sdhci_reset(SdhciHost *host, u8 mask)
{
	unsigned long timeout;

	/* Wait max 100 ms */
	timeout = 100;
	sdhci_writeb(host, mask, SDHCI_SOFTWARE_RESET);
	while (sdhci_readb(host, SDHCI_SOFTWARE_RESET) & mask) {
		if (timeout == 0) {
			printf("Reset 0x%x never completed.\n", (int)mask);
			return;
		}
		timeout--;
		udelay(1000);
	}
}

static void sdhci_cmd_done(SdhciHost *host, MmcCommand *cmd)
{
	int i;
	if (cmd->resp_type & MMC_RSP_136) {
		/* CRC is stripped so we need to do some shifting. */
		for (i = 0; i < 4; i++) {
			cmd->response[i] = sdhci_readl(host,
					SDHCI_RESPONSE + (3-i)*4) << 8;
			if (i != 3)
				cmd->response[i] |= sdhci_readb(host,
						SDHCI_RESPONSE + (3-i)*4-1);
		}
	} else {
		cmd->response[0] = sdhci_readl(host, SDHCI_RESPONSE);
	}
}

static void sdhci_transfer_pio(SdhciHost *host, MmcData *data)
{
	int i;
	char *offs;
	for (i = 0; i < data->blocksize; i += 4) {
		offs = data->dest + i;
		if (data->flags == MMC_DATA_READ)
			*(u32 *)offs = sdhci_readl(host, SDHCI_BUFFER);
		else
			sdhci_writel(host, *(u32 *)offs, SDHCI_BUFFER);
	}
}

static int sdhci_transfer_data(SdhciHost *host, MmcData *data,
			       unsigned int start_addr)
{
	unsigned int stat, rdy, mask, timeout, block = 0;

	timeout = 1000000;
	rdy = SDHCI_INT_SPACE_AVAIL | SDHCI_INT_DATA_AVAIL;
	mask = SDHCI_DATA_AVAILABLE | SDHCI_SPACE_AVAILABLE;
	do {
		stat = sdhci_readl(host, SDHCI_INT_STATUS);
		if (stat & SDHCI_INT_ERROR) {
			printf("Error detected in status(0x%X)!\n", stat);
			return -1;
		}
		if (stat & rdy) {
			if (!(sdhci_readl(host, SDHCI_PRESENT_STATE) & mask))
				continue;
			sdhci_writel(host, rdy, SDHCI_INT_STATUS);
			sdhci_transfer_pio(host, data);
			data->dest += data->blocksize;
			if (++block >= data->blocks)
				break;
		}
		if (timeout-- > 0)
			udelay(10);
		else {
			printf("Transfer data timeout\n");
			return -1;
		}
	} while (!(stat & SDHCI_INT_DATA_END));
	return 0;
}

static void sdhci_alloc_adma_descs(SdhciHost *host, u32 need_descriptors)
{
	if (host->adma_descs) {
		if (host->adma_desc_count < need_descriptors) {
			/* Previously allocated array is too small */
			free(host->adma_descs);
			host->adma_desc_count = 0;
			host->adma_descs = NULL;
		}
	}

	/* use dma_malloc() to make sure we get the coherent/uncached memory */
	if (!host->adma_descs) {
		host->adma_descs = dma_malloc(need_descriptors *
					      sizeof(*host->adma_descs));
		if (host->adma_descs == NULL)
			die("fail to malloc adma_descs\n");
		host->adma_desc_count = need_descriptors;
	}

	memset(host->adma_descs, 0, sizeof(*host->adma_descs) *
	       need_descriptors);
}

static void sdhci_alloc_adma64_descs(SdhciHost *host, u32 need_descriptors)
{
	if (host->adma64_descs) {
		if (host->adma_desc_count < need_descriptors) {
			/* Previously allocated array is too small */
			free(host->adma64_descs);
			host->adma_desc_count = 0;
			host->adma64_descs = NULL;
		}
	}

	/* use dma_malloc() to make sure we get the coherent/uncached memory */
	if (!host->adma64_descs) {
		host->adma64_descs = dma_malloc(need_descriptors *
					   sizeof(*host->adma64_descs));
		if (host->adma64_descs == NULL)
			die("fail to malloc adma64_descs\n");

		host->adma_desc_count = need_descriptors;
	}

	memset(host->adma64_descs, 0, sizeof(*host->adma64_descs) *
	       need_descriptors);
}

static int sdhci_setup_adma(SdhciHost *host, MmcData *data,
			    struct bounce_buffer *bbstate)
{
	int i, togo, need_descriptors;
	char *buffer_data;
	u16 attributes;

	togo = data->blocks * data->blocksize;
	if (!togo) {
		printf("%s: MmcData corrupted: %d blocks of %d bytes\n",
		       __func__, data->blocks, data->blocksize);
		return -1;
	}

	need_descriptors = 1 +  togo / SDHCI_MAX_PER_DESCRIPTOR;

	if (host->dma64)
		sdhci_alloc_adma64_descs(host, need_descriptors);
	else
		sdhci_alloc_adma_descs(host, need_descriptors);

	if (bbstate)
		buffer_data = (char *)bbstate->bounce_buffer;
	else
		buffer_data = data->dest;

	/* Now set up the descriptor chain. */
	for (i = 0; togo; i++) {
		unsigned desc_length;

		if (togo < SDHCI_MAX_PER_DESCRIPTOR)
			desc_length = togo;
		else
			desc_length = SDHCI_MAX_PER_DESCRIPTOR;
		togo -= desc_length;

		attributes = SDHCI_ADMA_VALID | SDHCI_ACT_TRAN;
		if (togo == 0)
			attributes |= SDHCI_ADMA_END;

		if (host->dma64) {
			host->adma64_descs[i].addr = (uintptr_t) buffer_data;
			host->adma64_descs[i].addr_hi = 0;
			host->adma64_descs[i].length = desc_length;
			host->adma64_descs[i].attributes = attributes;

		} else {
			host->adma_descs[i].addr = (uintptr_t) buffer_data;
			host->adma_descs[i].length = desc_length;
			host->adma_descs[i].attributes = attributes;
		}

		buffer_data += desc_length;
	}

	if (host->dma64)
		sdhci_writel(host, (uintptr_t) host->adma64_descs,
			     SDHCI_ADMA_ADDRESS);
	else
		sdhci_writel(host, (uintptr_t) host->adma_descs,
			     SDHCI_ADMA_ADDRESS);

	return 0;
}

static int sdhci_complete_adma(SdhciHost *host, MmcCommand *cmd)
{
	int retry;
	u32 stat = 0, mask;

	mask = SDHCI_INT_RESPONSE | SDHCI_INT_ERROR;

	retry = 10000; /* Command should be done in way less than 10 ms. */
	while (--retry) {
		stat = sdhci_readl(host, SDHCI_INT_STATUS);
		if (stat & mask)
			break;
		udelay(1);
	}

	sdhci_writel(host, SDHCI_INT_RESPONSE, SDHCI_INT_STATUS);

	if (retry && !(stat & SDHCI_INT_ERROR)) {
		/* Command OK, let's wait for data transfer completion. */
		mask = SDHCI_INT_DATA_END |
			SDHCI_INT_ERROR | SDHCI_INT_ADMA_ERROR;

		/* Transfer should take 10 seconds tops. */
		retry = 10 * 1000 * 1000;
		while (--retry) {
			stat = sdhci_readl(host, SDHCI_INT_STATUS);
			if (stat & mask)
				break;
			udelay(1);
		}

		sdhci_writel(host, stat, SDHCI_INT_STATUS);
		if (retry && !(stat & SDHCI_INT_ERROR)) {
			sdhci_cmd_done(host, cmd);
			return 0;
		}
	}

	printf("%s: transfer error, stat %#x, adma error %#x, retry %d\n",
	       __func__, stat, sdhci_readl(host, SDHCI_ADMA_ERROR), retry);

	sdhci_reset(host, SDHCI_RESET_CMD);
	sdhci_reset(host, SDHCI_RESET_DATA);

	if (stat & SDHCI_INT_TIMEOUT)
		return MMC_TIMEOUT;
	else
		return MMC_COMM_ERR;
}

static int sdhci_send_command_bounced(MmcCtrlr *mmc_ctrl, MmcCommand *cmd,
				      MmcData *data,
				      struct bounce_buffer *bbstate)
{
	unsigned int stat = 0;
	int ret = 0;
	u32 mask, flags;
	unsigned int timeout, start_addr = 0;
	uint64_t start;
	SdhciHost *host = container_of(mmc_ctrl, SdhciHost, mmc_ctrlr);

	/* Wait max 1 s */
	timeout = 1000;

	sdhci_writel(host, SDHCI_INT_ALL_MASK, SDHCI_INT_STATUS);
	mask = SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT;

	/* We shouldn't wait for data inihibit for stop commands, even
	   though they might use busy signaling */
	if (cmd->cmdidx == MMC_CMD_STOP_TRANSMISSION)
		mask &= ~SDHCI_DATA_INHIBIT;

	while (sdhci_readl(host, SDHCI_PRESENT_STATE) & mask) {
		if (timeout == 0) {
			printf("Controller never released inhibit bit(s), "
			       "present state %#8.8x.\n",
			       sdhci_readl(host, SDHCI_PRESENT_STATE));
			return MMC_COMM_ERR;
		}
		timeout--;
		udelay(1000);
	}

	mask = SDHCI_INT_RESPONSE;
	if (!(cmd->resp_type & MMC_RSP_PRESENT))
		flags = SDHCI_CMD_RESP_NONE;
	else if (cmd->resp_type & MMC_RSP_136)
		flags = SDHCI_CMD_RESP_LONG;
	else if (cmd->resp_type & MMC_RSP_BUSY) {
		flags = SDHCI_CMD_RESP_SHORT_BUSY;
		mask |= SDHCI_INT_DATA_END;
	} else
		flags = SDHCI_CMD_RESP_SHORT;

	if (cmd->resp_type & MMC_RSP_CRC)
		flags |= SDHCI_CMD_CRC;
	if (cmd->resp_type & MMC_RSP_OPCODE)
		flags |= SDHCI_CMD_INDEX;
	if (data)
		flags |= SDHCI_CMD_DATA;

	/* Set Transfer mode regarding to data flag */
	if (data) {
		u16 mode = 0;

		sdhci_writew(host, SDHCI_MAKE_BLKSZ(SDHCI_DEFAULT_BOUNDARY_ARG,
						    data->blocksize),
			     SDHCI_BLOCK_SIZE);

		if (data->flags == MMC_DATA_READ)
			mode |= SDHCI_TRNS_READ;

		if (data->blocks > 1)
			mode |= SDHCI_TRNS_BLK_CNT_EN |
				SDHCI_TRNS_MULTI | SDHCI_TRNS_ACMD12;

		sdhci_writew(host, data->blocks, SDHCI_BLOCK_COUNT);

		if (host->host_caps & MMC_AUTO_CMD12) {
			if (sdhci_setup_adma(host, data, bbstate))
				return -1;

			mode |= SDHCI_TRNS_DMA;
		}
		sdhci_writew(host, mode, SDHCI_TRANSFER_MODE);
	} else {
		/* Quirk: Some AMD chipsets require the cleraring the
		 * transfer mode 0 before sending a command without data.
		 * Commands with data always set the transfer mode */
		if (host->quirks & SDHCI_QUIRK_CLEAR_TRANSFER_BEFORE_CMD)
			sdhci_writew(host, 0, SDHCI_TRANSFER_MODE);
	}

	sdhci_writel(host, cmd->cmdarg, SDHCI_ARGUMENT);
	sdhci_writew(host, SDHCI_MAKE_CMD(cmd->cmdidx, flags), SDHCI_COMMAND);

	if (data && (host->host_caps & MMC_AUTO_CMD12))
		return sdhci_complete_adma(host, cmd);

	start = timer_us(0);
	do {
		stat = sdhci_readl(host, SDHCI_INT_STATUS);
		if (stat & SDHCI_INT_ERROR)
			break;

		/* Apply max timeout for R1b-type CMD defined in eMMC ext_csd
		   except for erase ones */
		if (timer_us(start) > 2550000) {
			if (host->quirks & SDHCI_QUIRK_BROKEN_R1B)
				return 0;
			else {
				printf("Timeout for status update!\n");
				return MMC_TIMEOUT;
			}
		}
	} while ((stat & mask) != mask);

	if ((stat & (SDHCI_INT_ERROR | mask)) == mask) {
		sdhci_cmd_done(host, cmd);
		sdhci_writel(host, mask, SDHCI_INT_STATUS);
	} else
		ret = -1;

	if (!ret && data)
		ret = sdhci_transfer_data(host, data, start_addr);

	if (host->quirks & SDHCI_QUIRK_WAIT_SEND_CMD)
		udelay(1000);

	stat = sdhci_readl(host, SDHCI_INT_STATUS);
	sdhci_writel(host, SDHCI_INT_ALL_MASK, SDHCI_INT_STATUS);

	if (!ret)
		return 0;

	sdhci_reset(host, SDHCI_RESET_CMD);
	sdhci_reset(host, SDHCI_RESET_DATA);
	if (stat & SDHCI_INT_TIMEOUT)
		return MMC_TIMEOUT;
	else
		return MMC_COMM_ERR;
}

static int sdhci_send_command(MmcCtrlr *mmc_ctrl, MmcCommand *cmd,
			      MmcData *data)
{
	void *buf;
	unsigned int bbflags;
	size_t len;
	struct bounce_buffer *bbstate = NULL;
	struct bounce_buffer bbstate_val;
	int ret;

	if (data) {
		if (data->flags & MMC_DATA_READ) {
			buf = data->dest;
			bbflags = GEN_BB_WRITE;
		} else {
			buf = (void *)data->src;
			bbflags = GEN_BB_READ;
		}
		len = data->blocks * data->blocksize;

		/*
		 * on some platform(like rk3399 etc) need to worry about
		 * cache coherency, so check the buffer, if not dma
		 * coherent, use bounce_buffer to do DMA management.
		 */
		if (!dma_coherent(buf)) {
			bbstate = &bbstate_val;
			if (bounce_buffer_start(bbstate, buf, len, bbflags)) {
				printf("ERROR: Failed to get bounce buffer.\n");
				return -1;
			}
		}
	}

	ret = sdhci_send_command_bounced(mmc_ctrl, cmd, data, bbstate);

	if (bbstate)
		bounce_buffer_stop(bbstate);

	return ret;
}

static int sdhci_is_clock_enabled(SdhciHost *host)
{
	return !!(sdhci_readw(host, SDHCI_CLOCK_CONTROL) & SDHCI_CLOCK_CARD_EN);
}

static int sdhci_set_clock(SdhciHost *host, unsigned int clock)
{
	unsigned int div, clk, timeout;

	sdhci_writew(host, 0, SDHCI_CLOCK_CONTROL);

	if (clock == 0)
		return 0;

	if ((host->version & SDHCI_SPEC_VER_MASK) >= SDHCI_SPEC_300) {
		/* Version 3.00 divisors must be a multiple of 2. */
		if (host->clock_base <= clock)
			div = 1;
		else {
			for (div = 2; div < SDHCI_MAX_DIV_SPEC_300; div += 2) {
				if ((host->clock_base / div) <= clock)
					break;
			}
		}
	} else {
		/* Version 2.00 divisors must be a power of 2. */
		for (div = 1; div < SDHCI_MAX_DIV_SPEC_200; div *= 2) {
			if ((host->clock_base / div) <= clock)
				break;
		}
	}
	div >>= 1;

	clk = (div & SDHCI_DIV_MASK) << SDHCI_DIVIDER_SHIFT;
	clk |= ((div & SDHCI_DIV_HI_MASK) >> SDHCI_DIV_MASK_LEN)
		<< SDHCI_DIVIDER_HI_SHIFT;
	clk |= SDHCI_CLOCK_INT_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	/* Wait max 20 ms */
	timeout = 20;
	while (!((clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL))
		& SDHCI_CLOCK_INT_STABLE)) {
		if (timeout == 0) {
			printf("Internal clock never stabilised.\n");
			return -1;
		}
		timeout--;
		udelay(1000);
	}

	clk |= SDHCI_CLOCK_CARD_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	host->clock = host->mmc_ctrlr.bus_hz;

	return 0;
}

/* Find leftmost set bit in a 32 bit integer */
static int fls(u32 x)
{
	int r = 32;

	if (!x)
		return 0;
	if (!(x & 0xffff0000u)) {
		x <<= 16;
		r -= 16;
	}
	if (!(x & 0xff000000u)) {
		x <<= 8;
		r -= 8;
	}
	if (!(x & 0xf0000000u)) {
		x <<= 4;
		r -= 4;
	}
	if (!(x & 0xc0000000u)) {
		x <<= 2;
		r -= 2;
	}
	if (!(x & 0x80000000u)) {
		x <<= 1;
		r -= 1;
	}
	return r;
}

static void sdhci_set_power(SdhciHost *host, unsigned short power)
{
	u8 pwr = 0;

	if (power != (unsigned short)-1) {
		switch (1 << power) {
		case MMC_VDD_165_195:
			pwr = SDHCI_POWER_180;
			break;
		case MMC_VDD_29_30:
		case MMC_VDD_30_31:
			pwr = SDHCI_POWER_300;
			break;
		case MMC_VDD_32_33:
		case MMC_VDD_33_34:
			pwr = SDHCI_POWER_330;
			break;
		}
	}

	if (pwr == 0) {
		sdhci_writeb(host, 0, SDHCI_POWER_CONTROL);
		return;
	}

	if (host->quirks & SDHCI_QUIRK_NO_SIMULT_VDD_AND_POWER)
		sdhci_writeb(host, pwr, SDHCI_POWER_CONTROL);

	pwr |= SDHCI_POWER_ON;

	sdhci_writeb(host, pwr, SDHCI_POWER_CONTROL);
}

void sdhci_set_uhs_signaling(SdhciHost *host, uint32_t timing)
{
	u16 ctrl_2;

	ctrl_2 = sdhci_readw(host, SDHCI_HOST_CONTROL2);
	/* Select Bus Speed Mode for host */
	ctrl_2 &= ~SDHCI_CTRL_UHS_MASK;

	if ((timing != MMC_TIMING_LEGACY) &&
	    (timing != MMC_TIMING_MMC_HS) &&
	    (timing != MMC_TIMING_SD_HS))
		ctrl_2 |= SDHCI_CTRL_VDD_180;

	if ((timing == MMC_TIMING_MMC_HS200) ||
	    (timing == MMC_TIMING_UHS_SDR104))
		ctrl_2 |= SDHCI_CTRL_UHS_SDR104 | SDHCI_CTRL_DRV_TYPE_A;
	else if (timing == MMC_TIMING_UHS_SDR12)
		ctrl_2 |= SDHCI_CTRL_UHS_SDR12;
	else if ((timing == MMC_TIMING_UHS_SDR25) ||
		(timing == MMC_TIMING_MMC_HS))
		ctrl_2 |= SDHCI_CTRL_UHS_SDR25;
	else if (timing == MMC_TIMING_UHS_SDR50)
		ctrl_2 |= SDHCI_CTRL_UHS_SDR50;
	else if ((timing == MMC_TIMING_UHS_DDR50) ||
		 (timing == MMC_TIMING_MMC_DDR52))
		ctrl_2 |= SDHCI_CTRL_UHS_DDR50;
	else if (timing == MMC_TIMING_MMC_HS400 ||
		 timing == MMC_TIMING_MMC_HS400ES)
		ctrl_2 |= SDHCI_CTRL_HS400 | SDHCI_CTRL_DRV_TYPE_A;

	sdhci_writew(host, ctrl_2, SDHCI_HOST_CONTROL2);
}

void sdhci_set_ios(MmcCtrlr *mmc_ctrlr)
{
	u32 ctrl;
	SdhciHost *host = container_of(mmc_ctrlr,
				       SdhciHost, mmc_ctrlr);

	if (host->set_control_reg)
		host->set_control_reg(host);

	/*
	 * Clock control register needs to be set if:
	 * 1. Clock is not enabled, or
	 * 2. Desired clock frequency is not the same as previously configured
	 * clock.
	 *
	 * #1 is important because any time the SD card controller is
	 * power-gated, it would end up clearing the clock control register. So,
	 * we cannot rely only on previously configured clock value.
	 */
	if (!sdhci_is_clock_enabled(host) || mmc_ctrlr->bus_hz != host->clock)
		sdhci_set_clock(host, mmc_ctrlr->bus_hz);

	/* Switch to 1.8 volt for HS200 */
	if (mmc_ctrlr->caps & MMC_MODE_1V8_VDD)
		if (mmc_ctrlr->bus_hz == MMC_CLOCK_200MHZ)
			sdhci_set_power(host, MMC_VDD_165_195_SHIFT);

	/* Set bus width */
	ctrl = sdhci_readb(host, SDHCI_HOST_CONTROL);
	if (mmc_ctrlr->bus_width == 8) {
		ctrl &= ~SDHCI_CTRL_4BITBUS;
		if ((host->version & SDHCI_SPEC_VER_MASK) >= SDHCI_SPEC_300)
			ctrl |= SDHCI_CTRL_8BITBUS;
	} else {
		if ((host->version & SDHCI_SPEC_VER_MASK) >= SDHCI_SPEC_300)
			ctrl &= ~SDHCI_CTRL_8BITBUS;
		if (mmc_ctrlr->bus_width == 4)
			ctrl |= SDHCI_CTRL_4BITBUS;
		else
			ctrl &= ~SDHCI_CTRL_4BITBUS;
	}

	if (!(mmc_ctrlr->timing == MMC_TIMING_LEGACY) &&
	    !(host->quirks & SDHCI_QUIRK_NO_HISPD_BIT))
		ctrl |= SDHCI_CTRL_HISPD;
	else
		ctrl &= ~SDHCI_CTRL_HISPD;

	sdhci_set_uhs_signaling(host, mmc_ctrlr->timing);

	if (host->host_caps & MMC_AUTO_CMD12) {
		ctrl &= ~SDHCI_CTRL_DMA_MASK;
		if (host->dma64)
			ctrl |= SDHCI_CTRL_ADMA64;
		else
			ctrl |= SDHCI_CTRL_ADMA32;
	}

	sdhci_writeb(host, ctrl, SDHCI_HOST_CONTROL);
}

/* Prepare SDHCI controller to be initialized */
static int sdhci_pre_init(SdhciHost *host)
{
	unsigned int caps, caps_1;

	if (host->attach) {
		int rv = host->attach(host);
		if (rv)
			return rv;
	}

	host->version = sdhci_readw(host, SDHCI_HOST_VERSION) & 0xff;

	caps = sdhci_readl(host, SDHCI_CAPABILITIES);
	caps_1 = sdhci_readl(host, SDHCI_CAPABILITIES_1);

	if ((caps_1 & SDHCI_SUPPORT_HS400) &&
	   (host->quirks & SDHCI_QUIRK_SUPPORTS_HS400ES))
		host->host_caps |= MMC_MODE_HS400ES;

	if (caps & SDHCI_CAN_DO_ADMA2)
		host->host_caps |= MMC_AUTO_CMD12;

	/* get base clock frequency from CAP register */
	if (!(host->quirks & SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN)) {
		if ((host->version & SDHCI_SPEC_VER_MASK) >= SDHCI_SPEC_300)
			host->clock_base = (caps & SDHCI_CLOCK_V3_BASE_MASK)
				>> SDHCI_CLOCK_BASE_SHIFT;
		else
			host->clock_base = (caps & SDHCI_CLOCK_BASE_MASK)
				>> SDHCI_CLOCK_BASE_SHIFT;
	}

	if (host->clock_base == 0) {
		printf("Hardware doesn't specify base clock frequency\n");
		return -1;
	}

	host->clock_base *= 1000000;

	if (host->clock_f_max)
		host->mmc_ctrlr.f_max = host->clock_f_max;
	else
		host->mmc_ctrlr.f_max = host->clock_base;

	if (host->clock_f_min) {
		host->mmc_ctrlr.f_min = host->clock_f_min;
	} else {
		if ((host->version & SDHCI_SPEC_VER_MASK) >= SDHCI_SPEC_300)
			host->mmc_ctrlr.f_min =
				host->clock_base / SDHCI_MAX_DIV_SPEC_300;
		else
			host->mmc_ctrlr.f_min =
				host->clock_base / SDHCI_MAX_DIV_SPEC_200;
	}

	if (caps & SDHCI_CAN_VDD_330)
		host->mmc_ctrlr.voltages |= MMC_VDD_32_33 | MMC_VDD_33_34;
	if (caps & SDHCI_CAN_VDD_300)
		host->mmc_ctrlr.voltages |= MMC_VDD_29_30 | MMC_VDD_30_31;
	if (caps & SDHCI_CAN_VDD_180)
		host->mmc_ctrlr.voltages |= MMC_VDD_165_195;

	if (host->quirks & SDHCI_QUIRK_BROKEN_VOLTAGE)
		host->mmc_ctrlr.voltages |= host->voltages;

	if (host->quirks & SDHCI_QUIRK_NO_EMMC_HS200)
		host->mmc_ctrlr.caps = MMC_MODE_HS | MMC_MODE_HS_52MHz |
			MMC_MODE_4BIT | MMC_MODE_HC;
	else
		host->mmc_ctrlr.caps = MMC_MODE_HS | MMC_MODE_HS_52MHz |
			MMC_MODE_4BIT | MMC_MODE_HC | MMC_MODE_HS_200MHz;

	if (host->quirks & SDHCI_QUIRK_EMMC_1V8_POWER)
		host->mmc_ctrlr.caps |= MMC_MODE_1V8_VDD;

	if (caps & SDHCI_CAN_DO_8BIT)
		host->mmc_ctrlr.caps |= MMC_MODE_8BIT;
	if (host->host_caps)
		host->mmc_ctrlr.caps |= host->host_caps;
	if (caps & SDHCI_CAN_64BIT)
		host->dma64 = 1;

	sdhci_reset(host, SDHCI_RESET_ALL);

	return 0;
}

static int sdhci_init(SdhciHost *host)
{
	int rv = sdhci_pre_init(host);

	if (rv)
		return rv; /* The error has been already reported */

	sdhci_set_power(host, fls(host->mmc_ctrlr.voltages) - 1);

	if (host->quirks & SDHCI_QUIRK_NO_CD) {
		unsigned int status;

		sdhci_writel(host, SDHCI_CTRL_CD_TEST_INS | SDHCI_CTRL_CD_TEST,
			SDHCI_HOST_CONTROL);

		status = sdhci_readl(host, SDHCI_PRESENT_STATE);
		while ((!(status & SDHCI_CARD_PRESENT)) ||
		    (!(status & SDHCI_CARD_STATE_STABLE)) ||
		    (!(status & SDHCI_CARD_DETECT_PIN_LEVEL)))
			status = sdhci_readl(host, SDHCI_PRESENT_STATE);
	}

	/* Enable only interrupts served by the SD controller */
	sdhci_writel(host, SDHCI_INT_DATA_MASK | SDHCI_INT_CMD_MASK,
		     SDHCI_INT_ENABLE);
	/* Mask all sdhci interrupt sources */
	sdhci_writel(host, 0x0, SDHCI_SIGNAL_ENABLE);

	/* Set timeout to maximum, shouldn't happen if everything's right. */
	sdhci_writeb(host, 0xe, SDHCI_TIMEOUT_CONTROL);

	udelay(10000);
	return 0;
}

static int sdhci_update(BlockDevCtrlrOps *me)
{
	SdhciHost *host = container_of
		(me, SdhciHost, mmc_ctrlr.ctrlr.ops);

	if (host->removable) {
		int present = (sdhci_readl(host, SDHCI_PRESENT_STATE) &
			       SDHCI_CARD_PRESENT) != 0;

		if (!present) {
			if (host->mmc_ctrlr.media) {
				/*
				 * A card was present but isn't any more. Get
				 * rid of it.
				 */
				list_remove
					(&host->mmc_ctrlr.media->dev.list_node);
				free(host->mmc_ctrlr.media);
				host->mmc_ctrlr.media = NULL;
			}
			return 0;
		}

		if (!host->mmc_ctrlr.media) {
			/*
			 * A card is present and not set up yet. Get it ready.
			 */
			if (sdhci_init(host))
				return -1;

			if (mmc_setup_media(&host->mmc_ctrlr))
				return -1;
			host->mmc_ctrlr.media->dev.name = "SDHCI card";
			list_insert_after
				(&host-> mmc_ctrlr.media->dev.list_node,
				 &removable_block_devices);
		}
	} else {
		if (!host->initialized && sdhci_init(host))
			return -1;

		host->initialized = 1;

		if (mmc_setup_media(&host->mmc_ctrlr))
			return -1;
		host->mmc_ctrlr.media->dev.name = "SDHCI fixed";
		list_insert_after(&host->mmc_ctrlr.media->dev.list_node,
				  &fixed_block_devices);
		host->mmc_ctrlr.ctrlr.need_update = 0;
	}

	host->mmc_ctrlr.media->dev.removable = host->removable;
	host->mmc_ctrlr.media->dev.ops.read = block_mmc_read;
	host->mmc_ctrlr.media->dev.ops.write = block_mmc_write;
	host->mmc_ctrlr.media->dev.ops.fill_write = block_mmc_fill_write;
	host->mmc_ctrlr.media->dev.ops.new_stream = new_simple_stream;

	return 0;
}

void add_sdhci(SdhciHost *host)
{
	host->mmc_ctrlr.send_cmd = &sdhci_send_command;
	host->mmc_ctrlr.set_ios = &sdhci_set_ios;

	host->mmc_ctrlr.ctrlr.ops.is_bdev_owned = block_mmc_is_bdev_owned;
	host->mmc_ctrlr.ctrlr.ops.update = &sdhci_update;
	host->mmc_ctrlr.ctrlr.need_update = 1;

	/* TODO(vbendeb): check if SDHCI spec allows to retrieve this value. */
	host->mmc_ctrlr.b_max = 65535;
}

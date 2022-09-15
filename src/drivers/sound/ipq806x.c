/*
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <assert.h>
#include <libpayload.h>
#include <stdint.h>

#include "base/cleanup_funcs.h"
#include "base/container_of.h"
#include "drivers/gpio/gpio.h"
#include "drivers/sound/ipq806x.h"
#include "drivers/sound/ipq806x-reg.h"
#include "drivers/sound/sound.h"

typedef struct __attribute__((packed)) {
	uint32_t control;
} Ipq806xI2sCtrlRegs;

typedef struct __attribute__((packed)) {
	uint32_t control;
	uint32_t base_address;
	uint32_t buffer_length;
	uint32_t UNUSED;
	uint32_t period_length;
} Ipq806xI2sDmaRegs;

typedef struct __attribute__((packed)) {
	uint32_t ns;
	uint32_t UNUSED;
	uint32_t status;
} Ipq806xLccMi2sRegs;

static size_t ipq806x_sound_make_tone(int16_t *buffer, size_t buffer_length,
		unsigned int channels, unsigned int frame_rate,
		unsigned int bitwidth, uint32_t frequency,
		uint16_t volume)
{
	const unsigned int period = frame_rate / frequency;
	const unsigned int half = period / 2;
	const unsigned int frame_size = channels * (bitwidth / 8);
	const unsigned int frames = buffer_length / frame_size;
	unsigned int frames_left = frames;
	unsigned int i, j;

	while (frames_left >= period) {
		for (i = 0; i < half; frames_left--, i++)
			for (j = 0; j < channels; j++)
				*buffer++ = volume;
		for (i = 0; i < period - half; frames_left--, i++)
			for (j = 0; j < channels; j++)
				*buffer++ = -volume;
	}

	return (frames - frames_left) * frame_size;
}

static int ipq806x_sound_init(Ipq806xSound *sound)
{
	Ipq806xI2sCtrlRegs *ctrl_regs = sound->ctrl_regs;
	Ipq806xI2sDmaRegs *dma_regs = sound->dma_regs;
	GpioOps *gpio = sound->gpio;
	const unsigned int bitwidth = sound->bitwidth;
	const unsigned int channels = sound->channels;
	uint32_t regval;

	regval = 0;
	switch (channels) {
	case 1:
		regval |= LPAIF_MI2SCTL_SPKMODE_SD0;
		regval |= LPAIF_MI2SCTL_SPKMONO_MONO;
		break;
	case 2:
		regval |= LPAIF_MI2SCTL_SPKMODE_SD0;
		regval |= LPAIF_MI2SCTL_SPKMONO_STEREO;
		break;
	case 4:
		regval |= LPAIF_MI2SCTL_SPKMODE_QUAD01;
		regval |= LPAIF_MI2SCTL_SPKMONO_STEREO;
		break;
	case 6:
		regval |= LPAIF_MI2SCTL_SPKMODE_6CH;
		regval |= LPAIF_MI2SCTL_SPKMONO_STEREO;
		break;
	case 8:
		regval |= LPAIF_MI2SCTL_SPKMODE_8CH;
		regval |= LPAIF_MI2SCTL_SPKMONO_STEREO;
		break;
	default:
		printf("%s: invalid channels given: %u\n", __func__, channels);
		return 1;
	}

	switch (bitwidth) {
	case 16:
		regval |= LPAIF_MI2SCTL_BITWIDTH_16;
		break;
	case 24:
		regval |= LPAIF_MI2SCTL_BITWIDTH_24;
		break;
	case 32:
		regval |= LPAIF_MI2SCTL_BITWIDTH_32;
		break;
	default:
		printf("%s: invalid bitwidth given: %u\n", __func__, bitwidth);
		return 1;
	}

	writel(regval, &ctrl_regs->control);

	regval = 0;
	regval |= LPAIF_DMACTL_BURST_EN;
	regval |= LPAIF_DMACTL_AUDIO_INTF_MI2S;
	regval |= LPAIF_DMACTL_FIFO_WM_8;

	switch (bitwidth) {
	case 16:
		switch (channels) {
		case 1:
		case 2:
			regval |= LPAIF_DMACTL_WPSCNT_SINGLE;
			break;
		case 4:
			regval |= LPAIF_DMACTL_WPSCNT_DOUBLE;
			break;
		case 6:
			regval |= LPAIF_DMACTL_WPSCNT_TRIPLE;
			break;
		case 8:
			regval |= LPAIF_DMACTL_WPSCNT_QUAD;
			break;
		default:
			printf("%s: invalid PCM config given: bw=%u, ch=%u\n",
					__func__, bitwidth, channels);
			return 1;
		}
		break;
	case 24:
	case 32:
		switch (channels) {
		case 1:
			regval |= LPAIF_DMACTL_WPSCNT_SINGLE;
			break;
		case 2:
			regval |= LPAIF_DMACTL_WPSCNT_DOUBLE;
			break;
		case 4:
			regval |= LPAIF_DMACTL_WPSCNT_QUAD;
			break;
		case 6:
			regval |= LPAIF_DMACTL_WPSCNT_SIXPACK;
			break;
		case 8:
			regval |= LPAIF_DMACTL_WPSCNT_OCTAL;
			break;
		default:
			printf("%s: invalid PCM config given: bw=%u, ch=%u\n",
					__func__, bitwidth, channels);
			return 1;
		}
		break;
	default:
		printf("%s: invalid PCM config given: bw=%d, ch=%d\n",
				__func__, bitwidth, channels);
		return 1;
	}

	writel(regval, &dma_regs->control);

	/* Initialize the GPIOs required for the board */
	board_dac_gpio_config();
	gpio_set(gpio, 1);
	board_i2s_gpio_config();

	return 0;
}

static int ipq806x_sound_start(SoundOps *me, uint32_t frequency)
{
	Ipq806xSound *sound = container_of(me, Ipq806xSound, ops);
	Ipq806xI2sCtrlRegs *ctrl_regs = sound->ctrl_regs;
	Ipq806xI2sDmaRegs *dma_regs = sound->dma_regs;
	GpioOps *gpio = sound->gpio;
	uint32_t buffer_val, length_val, regval;
	size_t audio_length;

	assert(frequency);

	if (!sound->initialized) {
		if (ipq806x_sound_init(sound))
			return 1;
		sound->initialized = 1;
	}

	audio_length = ipq806x_sound_make_tone(sound->buffer,
			sound->buffer_length, sound->channels,
			sound->frame_rate, sound->bitwidth,
			frequency, sound->volume);

	buffer_val = (uint32_t)sound->buffer;
	length_val = (audio_length & 0xFFFFFFF0) >> 2;

	writel(buffer_val, &dma_regs->base_address);
	writel(length_val - 1, &dma_regs->buffer_length);
	writel(length_val + 1, &dma_regs->period_length);

	regval = readl(&dma_regs->control);
	regval |= LPAIF_DMACTL_ENABLE;
	writel(regval, &dma_regs->control);

	regval = readl(&ctrl_regs->control);
	regval |= LPAIF_MI2SCTL_SPKEN;
	writel(regval, &ctrl_regs->control);

	mdelay(2);

	gpio_set(gpio, 1);

	return 0;
}

static int ipq806x_sound_stop(SoundOps *me)
{
	Ipq806xSound *sound = container_of(me, Ipq806xSound, ops);
	Ipq806xI2sCtrlRegs *ctrl_regs = sound->ctrl_regs;
	Ipq806xI2sDmaRegs *dma_regs = sound->dma_regs;
	GpioOps *gpio = sound->gpio;
	uint32_t regval;

	if (!sound->initialized)
		return 0;

	gpio_set(gpio, 0);

	mdelay(1);

	regval = readl(&ctrl_regs->control);
	regval &= ~LPAIF_MI2SCTL_SPKEN;
	writel(regval, &ctrl_regs->control);

	regval = readl(&dma_regs->control);
	regval &= ~LPAIF_DMACTL_ENABLE;
	writel(regval, &dma_regs->control);

	return 0;
}

static int ipq806x_sound_play(SoundOps *me, uint32_t msec, uint32_t frequency)
{
	int ret;

	ret = ipq806x_sound_start(me, frequency);
	if (ret)
		return ret;

	mdelay(msec);

	ret = ipq806x_sound_stop(me);

	return ret;
}

static int ipq806x_set_volume(SoundOps *me, uint32_t volume)
{
	Ipq806xSound *sound = container_of(me, Ipq806xSound, ops);

	if (volume > 100)
		volume = 100; /* Just in case. */

	/* Max IPQ volume setting is 16000. */
	sound->volume = 160 * volume;

	return 0;
}

static int ipq806x_sound_shutdown(struct CleanupFunc *cleanup, CleanupType type)
{
	Ipq806xSound *sound = (Ipq806xSound *)cleanup->data;
	Ipq806xLccMi2sRegs *mi2s_regs = sound->lcc_mi2s_regs;
	uint32_t regval;

	printf("Shutting off the MI2S audio clock.\n");
	regval = readl(&mi2s_regs->ns);
	regval &= ~LCC_MI2S_NS_OSR_CXC_ENABLE;
	regval &= ~LCC_MI2S_NS_BIT_CXC_ENABLE;
	writel(regval, &mi2s_regs->ns);

	udelay(10);

	regval = readl(&mi2s_regs->status);
	if (!(regval & LCC_MI2S_STAT_OSR_CLK_MASK))
		if (!(regval & LCC_MI2S_STAT_BIT_CLK_MASK))
			return 0;

	printf("%s: error disabling MI2S clocks: %u\n", __func__, regval);
	return 1;
}

Ipq806xSound *new_ipq806x_sound(GpioOps *gpio, unsigned int frame_rate,
		unsigned int channels, unsigned int bitwidth, uint16_t volume)
{
	Ipq806xSound *sound = xzalloc(sizeof(*sound));
	CleanupFunc *cleanup = xzalloc(sizeof(*cleanup));

	assert(gpio != NULL);

	sound->ops.start = &ipq806x_sound_start;
	sound->ops.stop = &ipq806x_sound_stop;
	sound->ops.play = &ipq806x_sound_play;
	sound->ops.set_volume = &ipq806x_set_volume;

	sound->gpio = gpio;

	sound->ctrl_regs = (void *)(IPQ806X_LPAIF_BASE +
			LPAIF_MI2S_CTL_OFFSET(LPAIF_I2S_PORT_MI2S));
	sound->dma_regs = (void *)(IPQ806X_LPAIF_BASE +
			LPAIF_DMA_ADDR(LPAIF_DMA_RD_CH_MI2S, 0x00));
	sound->lcc_mi2s_regs = (void *)(IPQ806X_LCC_BASE + LCC_MI2S_NS_REG);
	sound->buffer = (void *)(IPQ806X_LPM_BASE);

	sound->buffer_length = LPM_SIZE;
	sound->frame_rate = frame_rate;
	sound->channels = channels;
	sound->bitwidth = bitwidth;
	sound->volume = volume;

	cleanup->cleanup = &ipq806x_sound_shutdown;
	cleanup->types = CleanupOnHandoff | CleanupOnLegacy;
	cleanup->data = sound;
	list_insert_after(&cleanup->list_node, &cleanup_funcs);

	return sound;
}

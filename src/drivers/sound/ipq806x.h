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

#ifndef __DRIVERS_SOUND_IPQ806X_H__
#define __DRIVERS_SOUND_IPQ806X_H__

#include "drivers/gpio/gpio.h"
#include "drivers/sound/sound.h"

typedef struct
{
	SoundOps ops;
	GpioOps *gpio;

	void *ctrl_regs;
	void *dma_regs;
	void *lcc_mi2s_regs;
	void *buffer;

	size_t buffer_length;
	unsigned int frame_rate;
	unsigned int channels;
	unsigned int bitwidth;
	uint16_t volume;
	int initialized;
} Ipq806xSound;

Ipq806xSound *new_ipq806x_sound(GpioOps *gpio, unsigned int frame_rate,
		unsigned int channels, unsigned int bitwidth, uint16_t volume);
void board_dac_gpio_config(void);
void board_i2s_gpio_config(void);

#endif /* __DRIVERS_SOUND_IPQ806X_H__ */

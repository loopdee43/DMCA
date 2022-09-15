/*
 * Copyright 2011 Google Inc.
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
 * but without any warranty; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/* This file is derived from the flashrom project. */
#ifndef __DRIVERS_FLASH_ICH_H__
#define __DRIVERS_FLASH_ICH_H__

#include <stdint.h>

#include "drivers/flash/flash.h"

typedef struct IchFlash
{
	FlashOps ops;
	int initialized;

	uint8_t *opmenu;
	int menubytes;
	uint16_t *preop;
	uint16_t *optype;
	uint32_t *addr;
	uint8_t *data;
	unsigned databytes;
	uint8_t *status;
	uint16_t *control;
	uint32_t *bbar;
	uint8_t bios_cntl_set;
	uint8_t bios_cntl_clear;

	int (*get_lock)(struct IchFlash *me);
	int locked;

	uint32_t rom_size;
	uint8_t cache[];
} IchFlash;

#endif /* __DRIVERS_FLASH_ICH_H__ */

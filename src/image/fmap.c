/*
 * Copyright 2012 Google Inc.
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

#include <assert.h>
#include <libpayload.h>

#include "base/init_funcs.h"
#include "config.h"
#include "drivers/flash/flash.h"
#include "image/fmap.h"

static const Fmap *main_fmap;

static int fmap_check_signature(void)
{
	return memcmp(main_fmap->signature, (uint8_t *)FMAP_SIGNATURE,
		      sizeof(main_fmap->signature));
}

static void fmap_init(void)
{
	static int init_done = 0;

	if (init_done)
		return;

	main_fmap = flash_read(lib_sysinfo.fmap_offset, sizeof(Fmap));
	if (!main_fmap)
		halt();
	if (fmap_check_signature()) {
		printf("Bad signature on the FMAP.\n");
		halt();
	}
	uint32_t fmap_size = sizeof(Fmap) +
		main_fmap->nareas * sizeof(FmapArea);
	main_fmap = flash_read(lib_sysinfo.fmap_offset, fmap_size);
	if (!main_fmap)
		halt();

	init_done = 1;
	return;
}

const Fmap *fmap_base(void)
{
	fmap_init();
	return main_fmap;
}

const int fmap_find_area(const char *name, FmapArea *area)
{
	fmap_init();
	for (int i = 0; i < main_fmap->nareas; i++) {
		const FmapArea *cur = &(main_fmap->areas[i]);
		if (!strncmp(name, (const char *)cur->name,
				sizeof(cur->name))) {
			memcpy(area, cur, sizeof(FmapArea));
			return 0;
		}
	}
	return 1;
}

const char *fmap_find_string(const char *name, int *size)
{
	assert(size);

	FmapArea area;
	if (fmap_find_area(name, &area)) {
		*size = 0;
		return NULL;
	}
	*size = area.size;
	return flash_read(area.offset, area.size);
}

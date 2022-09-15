/*
 * Copyright 2014 Google Inc.
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

#include <libpayload.h>
#include <config.h>
#include <image/fmap.h>
#include <drivers/flash/flash.h>
#include <vboot_api.h>

/*
 * NVRAM storage in flash uses a block of flash memory to represent the NVRAM
 * blob. Typical flash memory allows changing of individual bits from one to
 * zero. Changing bits from zero to one requires an erase operation, which
 * affects entire blocks of storage.
 *
 * In a typical case the last non-erased blob of CONFIG_NVRAM_BLOCK_SIZE bytes
 * in the dedicated block is considered the current NVRAM contents. If there
 * is a need to change the NVRAM contents, the next blob worth of bytes is
 * written. It becomes the last non-erased blob, which is by definition the
 * current NVRAM contents.
 *
 * If the entire dedicated block is empty, the first blob is used as the
 * NVRAM. It will be considered invalid and overwritten by vboot as required.
 *
 * If there is no room in the dedicated block to store a new blob - the NVRAM
 * write operation would fail.
 *
 * This scheme expects the user space application to manage the allocated
 * block, erasing it and initializing the lowest blob with the current NVRAM
 * contents once it gets close to capacity.
 *
 */

/* FMAP descriptor of the NVRAM area */
static FmapArea nvram_area_descriptor;

/* Offset of the actual NVRAM blob offset in the NVRAM block. */
static int nvram_blob_offset;

/* Local cache of the NVRAM blob. */
static uint8_t nvram_cache[VBNV_BLOCK_SIZE];

static int flash_nvram_init(void)
{
	int used_below, empty_above;
	static int vbnv_flash_is_initialized = 0;
	uint8_t empty_nvram_block[sizeof(nvram_cache)];
	uint8_t *block;

	if (vbnv_flash_is_initialized)
		return 0;

	if (fmap_find_area("RW_NVRAM", &nvram_area_descriptor)) {
		printf("%s: failed to find NVRAM area\n", __func__);
		return -1;
	}

	/* Prepare an empty NVRAM block to compare against. */
	memset(empty_nvram_block, 0xff, sizeof(empty_nvram_block));

	/* Binary search for the border between used and empty */
	used_below = 0;
	empty_above = nvram_area_descriptor.size / sizeof(nvram_cache);

	while (used_below + 1 < empty_above) {
		int guess = (used_below + empty_above) / 2;
		block = flash_read(nvram_area_descriptor.offset +
				   guess * sizeof(nvram_cache),
				   sizeof(nvram_cache));
		if (!block) {
			printf("%s: failed to read NVRAM area\n", __func__);
			return -1;
		}

		if (!memcmp(block, empty_nvram_block, sizeof(nvram_cache)))
			empty_above = guess;
		else
			used_below = guess;
	}

	/*
	 * Offset points to the last non-empty blob.  Or if all blobs are empty
	 * (nvram is totally erased), point to the first blob.
	 */
	nvram_blob_offset = used_below * sizeof(nvram_cache);

	block = flash_read(nvram_area_descriptor.offset + nvram_blob_offset,
			   sizeof(nvram_cache));
	if (!block) {
		printf("%s: failed to read NVRAM area\n", __func__);
		return -1;
	}
	memcpy(nvram_cache, block, sizeof(nvram_cache));

	vbnv_flash_is_initialized = 1;
	return 0;
}

VbError_t VbExNvStorageRead(uint8_t *buf)
{
	if (flash_nvram_init())
		return VBERROR_UNKNOWN;

	memcpy(buf, nvram_cache, sizeof(nvram_cache));
	return VBERROR_SUCCESS;
}

static VbError_t erase_nvram(void)
{
	if (flash_nvram_init())
		return VBERROR_UNKNOWN;

	if (flash_erase(nvram_area_descriptor.offset,
			nvram_area_descriptor.size) !=
					nvram_area_descriptor.size)
		return VBERROR_UNKNOWN;

	return VBERROR_SUCCESS;
}

VbError_t VbExNvStorageWrite(const uint8_t *buf)
{
	int i;

	if (flash_nvram_init())
		return VBERROR_UNKNOWN;

	/* Bail out if there have been no changes. */
	if (!memcmp(buf, nvram_cache, sizeof(nvram_cache)))
		return VBERROR_SUCCESS;

	/* See if we can overwrite the current blob with the new one. */
	for (i = 0; i < sizeof(nvram_cache); i++)
		if ((nvram_cache[i] & buf[i]) != buf[i])
			break;

	if (i != sizeof(nvram_cache)) {
		int new_blob_offset;
		/*
		 * Won't be able to overwrite, need to use the next blob,
		 * let's see if it is available.
		 */
		new_blob_offset = nvram_blob_offset + sizeof(nvram_cache);
		if (new_blob_offset >= nvram_area_descriptor.size) {
			printf("nvram is used up. deleting it to start over\n");
			if (erase_nvram() != VBERROR_SUCCESS)
				return VBERROR_UNKNOWN;
			new_blob_offset = 0;
		}
		nvram_blob_offset = new_blob_offset;
	}

	if (flash_write(nvram_area_descriptor.offset + nvram_blob_offset,
			sizeof(nvram_cache), buf) != sizeof(nvram_cache))
		return VBERROR_UNKNOWN;

	memcpy(nvram_cache, buf, sizeof(nvram_cache));
	return VBERROR_SUCCESS;
}

int nvstorage_flash_get_offet(void)
{
	return nvram_blob_offset;
}

int nvstorage_flash_get_blob_size(void)
{
	return sizeof(nvram_cache);
}

/*
 * Copyright 2015 Google Inc.
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

#include "base/gpt.h"
#include "config.h"
#include "fastboot/backend.h"

#define BACKEND_DEBUG

#ifdef BACKEND_DEBUG
#define BE_LOG(args...)		printf(args);
#else
#define BE_LOG(args...)
#endif

/* Weak variables for board-specific data */
size_t fb_bdev_count __attribute__((weak)) = 0;
struct bdev_info fb_bdev_list[] __attribute__((weak)) = {{}} ;
size_t fb_part_count __attribute__((weak)) = 0;
struct part_info fb_part_list[] __attribute__((weak)) = {{}} ;

/* Image partition details */
struct image_part_details {
	struct bdev_info *bdev_entry;
	struct part_info *part_entry;
	uint64_t part_addr;
	uint64_t part_size_lba;
};

/********************** Stub implementations *****************************/
backend_ret_t __attribute__((weak)) board_write_partition(const char *name,
							  void *image_addr,
							  uint64_t image_size)
{
	return BE_NOT_HANDLED;
}


/********************** Sparse Image Handling ****************************/

/* Sparse Image Header */
struct sparse_image_hdr {
	/* Magic number for sparse image 0xed26ff3a. */
	uint32_t magic;
	/* Major version = 0x1 */
	uint16_t major_version;
	uint16_t minor_version;
	uint16_t file_hdr_size;
	uint16_t chunk_hdr_size;
	/* Size of block in bytes. */
	uint32_t blk_size;
	/* # of blocks in the non-sparse image. */
	uint32_t total_blks;
	/* # of chunks in the sparse image. */
	uint32_t total_chunks;
	uint32_t image_checksum;
};

#define SPARSE_IMAGE_MAGIC	0xed26ff3a
#define CHUNK_TYPE_RAW		0xCAC1
#define CHUNK_TYPE_FILL	0xCAC2
#define CHUNK_TYPE_DONT_CARE	0xCAC3
#define CHUNK_TYPE_CRC32	0xCAC4

/* Chunk header in sparse image */
struct sparse_chunk_hdr {
	uint16_t type;
	uint16_t reserved;
	/* Chunk size is in number of blocks */
	uint32_t size_in_blks;
	/* Size in bytes of chunk header and data */
	uint32_t total_size_bytes;
};

/* Check if given image is sparse */
int is_sparse_image(void *image_addr)
{
	struct sparse_image_hdr *hdr = image_addr;

	/* AOSP sparse format supports major version 0x1 only */
	return ((hdr->magic == SPARSE_IMAGE_MAGIC) &&
		(hdr->major_version == 0x1));
}

struct img_buff {
	void *data;
	uint64_t size;
};

/*
 * Initialize img_buff structure with passed in data and size. Verifies that
 * buff and data is not NULL.
 * Returns 0 on success and -1 on error.
 */
static int img_buff_init(struct img_buff *buff, void *data, uint64_t size)
{
	if ((buff == NULL) || (data == NULL))
		return -1;

	buff->data = data;
	buff->size = size;

	return 0;
}

/*
 * Obtain current pointer to data and advance data pointer by size. If there is
 * not enough data to advance, then it returns NULL.
 */
static void *img_buff_advance(struct img_buff *buff, uint64_t size)
{
	void *data;

	if (buff->size < size)
		return NULL;

	data = buff->data;
	buff->data = (uint8_t *)(buff->data) + size;
	buff->size -= size;

	return data;
}

/* Write sparse image to partition */
static backend_ret_t write_sparse_image(struct image_part_details *img,
					void *image_addr, uint64_t image_size)
{
	struct img_buff buff;
	struct sparse_image_hdr *img_hdr;
	struct sparse_chunk_hdr *chunk_hdr;
	uint64_t bdev_block_size = img->bdev_entry->bdev->block_size;

	if (img_buff_init(&buff, image_addr, image_size))
		return BE_IMAGE_INSUFFICIENT_DATA;

	img_hdr = img_buff_advance(&buff, sizeof(*img_hdr));

	if (img_hdr == NULL)
		return BE_IMAGE_INSUFFICIENT_DATA;

	BE_LOG("Magic          : %x\n", img_hdr->magic);
	BE_LOG("Major Version  : %x\n", img_hdr->major_version);
	BE_LOG("Minor Version  : %x\n", img_hdr->minor_version);
	BE_LOG("File Hdr Size  : %x\n", img_hdr->file_hdr_size);
	BE_LOG("Chunk Hdr Size : %x\n", img_hdr->chunk_hdr_size);
	BE_LOG("Blk Size       : %x\n", img_hdr->blk_size);
	BE_LOG("Total blks     : %x\n", img_hdr->total_blks);
	BE_LOG("Total chunks   : %x\n", img_hdr->total_chunks);
	BE_LOG("Checksum       : %x\n", img_hdr->image_checksum);

	/* Is image header size as expected? */
	if (img_hdr->file_hdr_size != sizeof(*img_hdr))
		return BE_SPARSE_HDR_ERR;

	/* Is image block size multiple of bdev block size? */
	if (img_hdr->blk_size != ALIGN_DOWN(img_hdr->blk_size, bdev_block_size))
		return BE_IMAGE_SIZE_MULTIPLE_ERR;

	/* Is chunk header size as expected? */
	if (img_hdr->chunk_hdr_size != sizeof(*chunk_hdr))
		return BE_CHUNK_HDR_ERR;

	int i;
	uint64_t part_addr = img->part_addr;
	uint64_t part_size_lba = img->part_size_lba;
	BlockDevOps *ops = &img->bdev_entry->bdev->ops;

	/* Perform the following operation on each chunk */
	for (i = 0; i < img_hdr->total_chunks; i++) {
		/* Get chunk header */
		chunk_hdr = img_buff_advance(&buff, sizeof(*chunk_hdr));

		if (chunk_hdr == NULL)
			return BE_IMAGE_INSUFFICIENT_DATA;

		BE_LOG("Chunk %d\n", i);
		BE_LOG("Type         : %x\n", chunk_hdr->type);
		BE_LOG("Size in blks : %x\n", chunk_hdr->size_in_blks);
		BE_LOG("Total size   : %x\n", chunk_hdr->total_size_bytes);
		BE_LOG("Part addr    : %llx\n", part_addr);

		/* Size in bytes and lba of the area occupied by chunk range */
		uint64_t chunk_size_bytes, chunk_size_lba;

		chunk_size_bytes = (uint64_t)chunk_hdr->size_in_blks *
			img_hdr->blk_size;
		chunk_size_lba = chunk_size_bytes / bdev_block_size;

		/* Should not write past partition size */
		if (part_size_lba < chunk_size_lba) {
			BE_LOG("part_size_lba:%llx\n", part_size_lba);
			BE_LOG("chunk_size_lba:%llx\n", chunk_size_lba);
			return BE_IMAGE_OVERFLOW_ERR;
		}

		switch (chunk_hdr->type) {
		case CHUNK_TYPE_RAW: {

			uint8_t *data_ptr;

			/*
			 * For Raw chunk type:
			 * chunk_size_bytes + chunk_hdr_size = chunk_total_size
			 */
			if ((chunk_size_bytes + sizeof(*chunk_hdr))!=
			    chunk_hdr->total_size_bytes) {
				BE_LOG("chunk_size_bytes:%llx\n",
				       chunk_size_bytes + sizeof(*chunk_hdr));
				BE_LOG("total_size_bytes:%x\n",
				       chunk_hdr->total_size_bytes);
				return BE_CHUNK_HDR_ERR;
			}

			data_ptr = img_buff_advance(&buff, chunk_size_bytes);
			if (data_ptr == NULL)
				return BE_IMAGE_INSUFFICIENT_DATA;

			if (ops->write(ops, part_addr, chunk_size_lba, data_ptr)
			    != chunk_size_lba)
				return BE_WRITE_ERR;

			break;
		}
		case CHUNK_TYPE_FILL: {

			uint32_t *data_fill;

			/*
			 * For fill chunk type:
			 * chunk_hdr_size + 4 bytes = chunk_total_size_bytes
			 */
			if (sizeof(uint32_t) + sizeof(*chunk_hdr) !=
			    chunk_hdr->total_size_bytes) {
				BE_LOG("chunk_size_bytes:%zx\n",
				       sizeof(uint32_t) + sizeof(*chunk_hdr));
				BE_LOG("total_size_bytes:%x\n",
				       chunk_hdr->total_size_bytes);
				return BE_CHUNK_HDR_ERR;
			}

			data_fill = img_buff_advance(&buff,
							sizeof(*data_fill));
			if (!data_fill)
				return BE_IMAGE_INSUFFICIENT_DATA;

			/* Perform fill_write operation */
			if (ops->fill_write(ops, part_addr, chunk_size_lba,
					    *data_fill)
			    != chunk_size_lba)
				return BE_WRITE_ERR;

			break;
		}
		case CHUNK_TYPE_DONT_CARE: {
			/*
			 * For dont care chunk type:
			 * chunk_hdr_size = chunk_total_size_bytes
			 * data in sparse image = 0 bytes
			 */
			if (sizeof(*chunk_hdr) != chunk_hdr->total_size_bytes) {
				BE_LOG("chunk_size_bytes:%zx\n",
				       sizeof(*chunk_hdr));
				BE_LOG("total_size_bytes:%x\n",
				       chunk_hdr->total_size_bytes);
				return BE_CHUNK_HDR_ERR;
			}
			break;
		}
		case CHUNK_TYPE_CRC32: {
			/*
			 * For crc32 chunk type:
			 * chunk_hdr_size + 4 bytes = chunk_total_size_bytes
			 */
			if (sizeof(uint32_t) + sizeof(*chunk_hdr) !=
			    chunk_hdr->total_size_bytes) {
				BE_LOG("chunk_size_bytes:%zx\n",
				       sizeof(uint32_t) + sizeof(*chunk_hdr));
				BE_LOG("total_size_bytes:%x\n",
				       chunk_hdr->total_size_bytes);
				return BE_CHUNK_HDR_ERR;
			}

			/* TODO(furquan): Verify CRC32 header? */

			/* Data present in chunk sparse image = 4 bytes */
			if (img_buff_advance(&buff, sizeof(uint32_t)) == NULL)
				return BE_IMAGE_INSUFFICIENT_DATA;
			break;
		}
		default: {
			/* Unknown chunk type */
			BE_LOG("Unknown chunk type %d\n", chunk_hdr->type);
			return BE_CHUNK_HDR_ERR;
		}
		}
		/* Update partition address and size accordingly */
		part_addr += chunk_size_lba;
		part_size_lba -= chunk_size_lba;
	}

	return BE_SUCCESS;
}

/********************** Raw Image Handling *******************************/

static backend_ret_t write_raw_image(struct image_part_details *img,
				     void *image_addr, uint64_t image_size)
{
	struct bdev_info *bdev_entry = img->bdev_entry;
	uint64_t part_addr = img->part_addr;
	uint64_t part_size_lba = img->part_size_lba;
	uint64_t image_size_lba;

	BlockDevOps *ops = &bdev_entry->bdev->ops;

	uint64_t block_size = bdev_entry->bdev->block_size;

	/* Ensure that image size is multiple of block size */
	if (image_size != ALIGN_DOWN(image_size, block_size))
		return BE_IMAGE_SIZE_MULTIPLE_ERR;

	image_size_lba = image_size / block_size;

	/* Ensure image size is less than partition size */
	if (part_size_lba < image_size_lba) {
		BE_LOG("part_size_lba:%llx\n", part_size_lba);
		BE_LOG("image_size_lba:%llx\n", image_size_lba);
		return BE_IMAGE_OVERFLOW_ERR;
	}

	if (ops->write(ops, part_addr, image_size_lba, image_addr) !=
	    image_size_lba)
		return BE_WRITE_ERR;

	return BE_SUCCESS;
}

/********************** Image Partition handling ******************************/

struct part_info *get_part_info(const char *name)
{
	int i;

	for (i = 0; i < fb_part_count; i++) {
		if (!strcmp(name, fb_part_list[i].part_name))
			return &fb_part_list[i];
	}

	return NULL;
}

static struct bdev_info *get_bdev_info(const char *name)
{
	int i;

	for (i = 0; i < fb_bdev_count; i++) {
		if (!strcmp(name, fb_bdev_list[i].name))
			return &fb_bdev_list[i];
	}

	return NULL;
}

static backend_ret_t backend_fill_bdev_info(void)
{
	ListNode *devs;
	int count = get_all_bdevs(BLOCKDEV_FIXED, &devs);

	if (count == 0)
		return BE_BDEV_NOT_FOUND;

	int i;

	for (i = 0; i < fb_bdev_count; i++) {
		BlockDev *bdev;
		BlockDevCtrlr *bdev_ctrlr;

		bdev_ctrlr = fb_bdev_list[i].bdev_ctrlr;

		if (bdev_ctrlr == NULL)
			return BE_BDEV_NOT_FOUND;

		list_for_each(bdev, *devs, list_node) {

			if (bdev_ctrlr->ops.is_bdev_owned(&bdev_ctrlr->ops,
							  bdev)) {
				fb_bdev_list[i].bdev = bdev;
				break;
			}
		}

		if (fb_bdev_list[i].bdev == NULL)
			return BE_BDEV_NOT_FOUND;
	}

	return BE_SUCCESS;
}

#if CONFIG_FASTBOOT_SLOTS
static inline int slot_is_first_instance(const char *name)
{
	return !strcmp(name + strlen(name) - 2,
		       CONFIG_FASTBOOT_SLOTS_STARTING_SUFFIX);
}

size_t fb_base_count;
struct part_base_info *fb_base_list;

/*
 * Identifies unique base names of all partitions.
 * e.g.: if the partitions are:
 * kernel-a
 * kernel-b
 * cache
 * system
 *
 * Then, fb_base_list would contain the following:
 * kernel
 * cache
 * system
 *
 * This information is useful while responding back to fastboot getvar variables
 * related to slots.
 */
static backend_ret_t backend_base_list_init(void)
{
	int i;

	fb_base_count = fb_part_count;

	/* Get unique base name count */
	for (i = 0; i < fb_part_count; i++) {
		if (fb_part_list[i].is_slotted == 0)
			continue;

		if (slot_is_first_instance(fb_part_list[i].part_name))
			continue;

		/*
		 * If a partition is slotted and not the first instance, then
		 * decrement base count. Finally, base count would contain
		 * total number of unique base names.
		 */
		fb_base_count--;
	}

	if (fb_base_count == 0)
		return BE_PART_NOT_FOUND;

	fb_base_list = xmalloc(fb_base_count * sizeof(*fb_base_list));

	size_t count;
	size_t str_len;
	char *base_name;

	for (i = 0, count = 0; i < fb_part_count; i++) {
		const char *name = fb_part_list[i].part_name;

		assert (count < fb_base_count);

		if (fb_part_list[i].is_slotted == 0) {
			fb_base_list[count].base_name = name;
			fb_base_list[count++].is_slotted = 0;
			continue;
		}

		str_len = strlen(name);
		/* Name should be > 2 characters. Ends with -<suffix> */
		assert(str_len > 2);

		if (!slot_is_first_instance(name))
			continue;

		/*
		 * Base name length = Part name length
		 *			- 2 (for -<suffix>)
		 *			+ 1 (for '\0')
		 */
		str_len = str_len - 2 + 1;
		base_name = xzalloc(str_len);
		strncpy(base_name, name, str_len - 1);
		fb_base_list[count].base_name = base_name;
		fb_base_list[count++].is_slotted = 1;
	}

	assert (count == fb_base_count);

	return BE_SUCCESS;
}
#endif

static backend_ret_t backend_do_init(void)
{
	static int backend_data_init = 0;

	if (backend_data_init)
		return BE_SUCCESS;

	if ((fb_bdev_count == 0) || (fb_bdev_list == NULL))
		return BE_BDEV_NOT_FOUND;

	if (backend_fill_bdev_info() != BE_SUCCESS)
		return BE_BDEV_NOT_FOUND;

	if((fb_part_count == 0) || (fb_part_list == NULL))
		return BE_PART_NOT_FOUND;

#if CONFIG_FASTBOOT_SLOTS
	backend_base_list_init();
#endif

	backend_data_init = 1;
	return BE_SUCCESS;
}

static backend_ret_t fill_img_part_info(struct image_part_details *img,
					const char *name)
{
	struct bdev_info *bdev_entry;
	struct part_info *part_entry;
	uint64_t part_addr;
	uint64_t part_size_lba;

	/* Get partition info from board-specific partition table */
	part_entry = get_part_info(name);
	if (part_entry == NULL)
		return BE_PART_NOT_FOUND;

	/* Get bdev info from part_entry */
	bdev_entry = part_entry->bdev_info;
	if (bdev_entry == NULL)
		return BE_BDEV_NOT_FOUND;

	/*
	 * If partition is GPT based, we have to go through a level of
	 * indirection to read the GPT entries and identify partition address
	 * and size. If the partition is not GPT based, board provides address
	 * and size of the partition on block device
	 */
	if (part_entry->gpt_based) {
		GptData *gpt = NULL;
		GptEntry *gpt_entry = NULL;

		/* Allocate GPT structure used by cgptlib */
		gpt = alloc_gpt(bdev_entry->bdev);

		if (gpt == NULL)
			return BE_GPT_ERR;

		/* Find nth entry based on GUID & instance provided by board */
		gpt_entry = GptFindNthEntry(gpt, &part_entry->guid,
					    part_entry->instance);

		if (gpt_entry == NULL) {
			free_gpt(bdev_entry->bdev, gpt);
			return BE_GPT_ERR;
		}

		/* Get partition addr and size from GPT entry */
		part_addr = gpt_entry->starting_lba;
		part_size_lba = GptGetEntrySizeLba(gpt_entry);

		free_gpt(bdev_entry->bdev, gpt);
	} else {
		/* Take board provided partition addr and size */
		part_addr = part_entry->base;
		part_size_lba = part_entry->size;
	}

	/* Fill image partition details structure */
	img->bdev_entry = bdev_entry;
	img->part_entry = part_entry;
	img->part_addr = part_addr;
	img->part_size_lba = part_size_lba;

	return BE_SUCCESS;
}

/********************** Backend API functions *******************************/

backend_ret_t backend_write_partition(const char *name, void *image_addr,
				      uint64_t image_size)
{
	backend_ret_t ret;
	struct image_part_details img;

	ret = backend_do_init();
	if (ret != BE_SUCCESS)
		return ret;

	ret = fill_img_part_info(&img, name);

	if (ret != BE_SUCCESS)
		return ret;

	if (is_sparse_image(image_addr)) {
		BE_LOG("Writing sparse image to %s...\n", name);
		ret = write_sparse_image(&img, image_addr, image_size);
	} else {
		BE_LOG("Writing raw image to %s...\n", name);
		ret = write_raw_image(&img, image_addr, image_size);
	}

	return ret;
}

backend_ret_t backend_erase_partition(const char *name)
{
	backend_ret_t ret;
	struct image_part_details img;

	ret = backend_do_init();
	if (ret != BE_SUCCESS)
		return ret;

	ret = fill_img_part_info(&img, name);

	if (ret != BE_SUCCESS)
		return ret;

	struct bdev_info *bdev_entry = img.bdev_entry;

	BlockDevOps *ops = &bdev_entry->bdev->ops;
	uint64_t part_size_lba = img.part_size_lba;
	uint64_t part_addr = img.part_addr;

	/* First try to perform erase operation, if ops for erase exist. */
	if ((ops->erase == NULL) ||
	    (ops->erase(ops, part_addr, part_size_lba) != part_size_lba)) {
		BE_LOG("Failed to erase. Falling back to fill_write\n");

		/* If erase fails, perform fill_write operation. */
		if (ops->fill_write(ops, part_addr, part_size_lba, 0xFFFFFFFF)
		    != part_size_lba)
			ret = BE_WRITE_ERR;
	}

	return ret;
}

uint64_t backend_get_part_size_bytes(const char *name)
{
	uint64_t ret = 0;
	struct image_part_details img;

	if (backend_do_init() != BE_SUCCESS)
		return ret;

	if (fill_img_part_info(&img, name) != BE_SUCCESS)
		return ret;

	ret = (uint64_t)img.part_size_lba * img.bdev_entry->bdev->block_size;

	return ret;
}

const char *backend_get_part_fs_type(const char *name)
{
	struct part_info *part_entry;

	if (backend_do_init() != BE_SUCCESS)
		return NULL;

	/* Get partition info from board-specific partition table */
	part_entry = get_part_info(name);
	if (part_entry == NULL)
		return NULL;

	return part_entry->part_fs_type;
}

uint64_t backend_get_bdev_size_bytes(const char *name)
{
	if (backend_do_init() != BE_SUCCESS)
		return 0;

	struct bdev_info *bdev_entry;

	/* Get bdev info from board-specific bdev table */
	bdev_entry = get_bdev_info(name);
	if (bdev_entry == NULL)
		return 0;

	BlockDev *bdev = bdev_entry->bdev;
	uint64_t size = (uint64_t)bdev->block_count * bdev->block_size;

	return size;
}

uint64_t backend_get_bdev_size_blocks(const char *name)
{
	if (backend_do_init() != BE_SUCCESS)
		return 0;

	struct bdev_info *bdev_entry;

	/* Get bdev info from board-specific bdev table */
	bdev_entry = get_bdev_info(name);
	if (bdev_entry == NULL)
		return 0;

	BlockDev *bdev = bdev_entry->bdev;
	uint64_t size = (uint64_t)bdev->block_count;

	return size;
}

int fb_fill_part_list(const char *name, uint64_t base, uint64_t size)
{
	struct part_info *part_entry = get_part_info(name);

	if (part_entry == NULL)
		return -1;

	part_entry->base = base;
	part_entry->size = size;

	return 0;
}

#if CONFIG_FASTBOOT_SLOTS
/**************************** Slots handling ******************************/

static struct bdev_info *kernel_bdev_entry;

static const Guid kernel_guid = GPT_ENT_TYPE_CHROMEOS_KERNEL;

static void get_kernel_bdev_entry(void)
{
	if (kernel_bdev_entry)
		return;

	int i;

	/* Scan through all part lists to find kernel partition. */
	for (i = 0; i < fb_part_count; i++) {
		if (fb_part_list[i].gpt_based &&
		    (!memcmp(&kernel_guid, &fb_part_list[i].guid,
			     sizeof(kernel_guid))))
			break;
	}

	assert (i < fb_part_count);

	/* Record bdev ptr for kernel partition. */
	kernel_bdev_entry = fb_part_list[i].bdev_info;
}

int backend_get_curr_slot(void)
{
	if (backend_do_init() != BE_SUCCESS)
		return -1;

	get_kernel_bdev_entry();

	GptData *gpt = NULL;
	GptEntry *gpt_entry = NULL;

	/* Setup GPT structure. */
	gpt = alloc_gpt(kernel_bdev_entry->bdev);

	if (gpt == NULL)
		return -1;

	int i;
	int curr_slot = -1;
	int curr_prio = -1;
	int prio;

	/*
	 * Current active slot is considered as the highest priority slot with
	 * success flag set.
	 */
	for (i = 0; i < CONFIG_FASTBOOT_SLOTS_COUNT; i++) {
		gpt_entry = GptFindNthEntry(gpt, &kernel_guid, i);
		if (gpt_entry == NULL)
			break;

		if (GetEntrySuccessful(gpt_entry)) {
			prio = GetEntryPriority(gpt_entry);
			if (prio > curr_prio) {
				curr_prio = prio;
				curr_slot = i;
			}
		}
	}

	if (gpt)
		free_gpt(kernel_bdev_entry->bdev, gpt);

	return curr_slot;

}

int backend_get_slot_flags(fb_getvar_t var, int index)
{
	if (index >= CONFIG_FASTBOOT_SLOTS_COUNT)
		return -1;

	if (backend_do_init() != BE_SUCCESS)
		return -1;

	get_kernel_bdev_entry();

	GptData *gpt = NULL;
	GptEntry *gpt_entry = NULL;
	int ret = -1;

	/* Setup GPT structure. */
	gpt = alloc_gpt(kernel_bdev_entry->bdev);

	if (gpt == NULL)
		goto fail;

	gpt_entry = GptFindNthEntry(gpt, &kernel_guid, index);
	if (gpt_entry == NULL)
		goto fail;

	switch (var) {
	case FB_SLOT_SUCCESSFUL: {
		ret = GetEntrySuccessful(gpt_entry);
		break;
	}
	case FB_SLOT_UNBOOTABLE: {
		ret = !(GetEntrySuccessful(gpt_entry) ||
			GetEntryTries(gpt_entry));
		break;
	}
	case FB_SLOT_RETRY_COUNT: {
		ret = GetEntryTries(gpt_entry);
		break;
	}
	default:
		break;
	}

fail:
	if (gpt)
		free_gpt(kernel_bdev_entry->bdev, gpt);

	return ret;

}

backend_ret_t backend_set_active_slot(int index)
{
	backend_ret_t ret = BE_SUCCESS;

	if (index >= CONFIG_FASTBOOT_SLOTS_COUNT)
		return BE_INVALID_SLOT_INDEX;

	ret = backend_do_init();
	if (ret != BE_SUCCESS)
		return ret;

	get_kernel_bdev_entry();

	GptData *gpt = NULL;
	GptEntry *gpt_entry = NULL;
	int i;

	/* Setup GPT structure. */
	gpt = alloc_gpt(kernel_bdev_entry->bdev);

	if (gpt == NULL) {
		ret = BE_GPT_ERR;
		goto fail;
	}

	/* First mark requested slot as active. */
	gpt_entry = GptFindNthEntry(gpt, &kernel_guid, index);
	if (gpt_entry == NULL) {
		ret = BE_GPT_ERR;
		goto fail;
	}
	GptUpdateKernelWithEntry(gpt, gpt_entry, GPT_UPDATE_ENTRY_ACTIVE);

	/* Mark remaining slots as inactive. */
	for (i = 0; i < CONFIG_FASTBOOT_SLOTS_COUNT; i++) {

		if (i == index)
			continue;

		gpt_entry = GptFindNthEntry(gpt, &kernel_guid, i);
		if (gpt_entry == NULL) {
			ret = BE_GPT_ERR;
			goto fail;
		}

		GptUpdateKernelWithEntry(gpt, gpt_entry,
					 GPT_UPDATE_ENTRY_INVALID);
	}

fail:
	if (gpt)
		free_gpt(kernel_bdev_entry->bdev, gpt);

	return ret;
}
#endif

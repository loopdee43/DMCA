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

#ifndef __FASTBOOT_CAPABILITIES_H__
#define __FASTBOOT_CAPABILITIES_H__

#include <libpayload.h>

/*
 * Enum to report back to calling function whether the fastboot command is
 * currently allowed or not.
 */
typedef enum {
	FB_CAP_FUNC_NOT_ALLOWED	= 0,
	FB_CAP_FUNC_ALLOWED		= 1,
} fb_cap_status_t;

/*
 * Function ids for fastboot functions.
 */
typedef enum {
	FB_ID_GETVAR			= (1 << 0),
	FB_ID_DOWNLOAD			= (1 << 1),
	FB_ID_VERIFY			= (1 << 2),
	FB_ID_FLASH			= (1 << 3),
	FB_ID_ERASE			= (1 << 4),
	FB_ID_BOOT			= (1 << 5),
	FB_ID_CONTINUE			= (1 << 6),
	FB_ID_REBOOT			= (1 << 7),
	FB_ID_REBOOT_BOOTLOADER	= (1 << 8),
	FB_ID_POWERDOWN		= (1 << 9),
	FB_ID_UNLOCK			= (1 << 10),
	FB_ID_LOCK			= (1 << 11),
	FB_ID_SETENV			= (1 << 12),
	FB_ID_OFF_MODE_CHARGE		= (1 << 13),
	FB_ID_GET_UNLOCK_ABILITY	= (1 << 14),
	FB_ID_BATTERY_CUTOFF		= (1 << 15),
	FB_ID_CLEAR_GBB		= (1 << 16),
	FB_ID_WRITE_PROTECT		= (1 << 17),
	FB_ID_DOUBLE_TAP_DISABLE	= (1 << 18),
	FB_ID_USB_RECOVERY		= (1 << 19),
	FB_ID_SET_ACTIVE_SLOT		= (1 << 20),
	FB_ID_MASK			= ((1 << 21) - 1),
} fb_func_id_t;

/*
 * Function to check if function id passed in is currently allowed or not.
 * If function is allowed it returns FB_CAP_FUNC_ALLOWED else
 * FB_CAP_FUNC_NOT_ALLOWED.
 */
fb_cap_status_t fb_cap_func_allowed(fb_func_id_t id);

/* Check if GBB flag is set to force full fastboot capability. */
uint8_t fb_check_gbb_override(void);

#endif /* __FASTBOOT_CAPABILITIES_H__ */

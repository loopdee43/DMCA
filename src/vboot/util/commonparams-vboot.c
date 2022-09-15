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

#include "vboot/util/commonparams.h"
#include "vboot/util/vboot_handoff.h"

int find_common_params(void **blob, int *size)
{
	struct vboot_handoff *vboot_handoff = lib_sysinfo.vboot_handoff;
	*blob = &vboot_handoff->shared_data[0];
	*size = ARRAY_SIZE(vboot_handoff->shared_data);
	return 0;
}

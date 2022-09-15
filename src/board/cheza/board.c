/*
 * This file is part of the coreboot project.
 *
 * Copyright (C) 2018, The Linux Foundation.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <assert.h>
#include <libpayload.h>

#include "base/init_funcs.h"
#include "config.h"
#include "drivers/gpio/gpio.h"
#include "vboot/util/flag.h"
#include "boot/fit.h"
#include "drivers/bus/usb/usb.h"
#include "drivers/power/psci.h"

static const VpdDeviceTreeMap vpd_dt_map[] = {
	{ "bluetooth_mac0", "bluetooth0/local-bd-address" },
	{ "wifi_mac0", "wifi0/local-mac-address" },
	{ "bluetooth_mac", "bluetooth0/local-bd-address" },
	{ "wifi_mac", "wifi0/local-mac-address" },
	{}
};

static int board_setup(void)
{
	/* stub out required GPIOs for vboot */
	flag_replace(FLAG_LIDSW, new_gpio_high());

	power_set_ops(&psci_power_ops);

	/* Support primary USB3.0 XHCI controller in firmware. */
	UsbHostController *usb_host0 = new_usb_hc(XHCI, 0xa600000);
	list_insert_after(&usb_host0->list_node, &usb_host_controllers);

	/* Support secondary USB3.0 XHCI controller in firmware. */
	UsbHostController *usb_host1 = new_usb_hc(XHCI, 0xa800000);
	list_insert_after(&usb_host1->list_node, &usb_host_controllers);

	dt_register_vpd_mac_fixup(vpd_dt_map);

	return 0;
}

INIT_FUNC(board_setup);

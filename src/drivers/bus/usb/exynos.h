/*
 * Copyright 2013 Google Inc.
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

#ifndef __DRIVERS_BUS_USB_EXYNOS_H__
#define __DRIVERS_BUS_USB_EXYNOS_H__

#include "drivers/bus/usb/usb.h"

void exynos5420_usbss_phy_tune(UsbHostController *hc);

#endif /* __DRIVERS_BUS_USB_EXYNOS_H__ */

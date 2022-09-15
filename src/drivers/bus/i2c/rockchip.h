/*
 * Copyright 2014 Rockchip Electronics Co., Ltd.
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

#ifndef __DRIVERS_BUS_I2C_RK3288_H__
#define __DRIVERS_BUS_I2C_RK3288_H__

#include "drivers/bus/i2c/i2c.h"

typedef struct RkI2c {
	I2cOps ops;
	void *reg_addr;
} RkI2c;

RkI2c *new_rockchip_i2c(uintptr_t *regs);

#endif				/* __DRIVERS_BUS_I2C_RK3288_H__ */

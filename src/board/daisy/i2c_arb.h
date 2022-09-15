/*
 * Copyright 2013 Google Inc.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __BOARD_DAISY_I2C_ARB_H__
#define __BOARD_DAISY_I2C_ARB_H__

#include "drivers/bus/i2c/i2c.h"
#include "drivers/gpio/gpio.h"

typedef struct SnowI2cArb
{
	I2cOps ops;
	I2cOps *bus;
	GpioOps *request;
	GpioOps *grant;
	int ready;
} SnowI2cArb;

SnowI2cArb *new_snow_i2c_arb(I2cOps *bus, GpioOps *request, GpioOps *grant);

#endif /* __BOARD_DAISY_I2C_ARB_H__ */

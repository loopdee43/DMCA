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

#ifndef __DRIVERS_GPIO_GPIO_H__
#define __DRIVERS_GPIO_GPIO_H__

typedef struct GpioOps {
	int (*get)(struct GpioOps *me);
	int (*set)(struct GpioOps *me, unsigned value);
} GpioOps;

static inline int gpio_get(GpioOps *gpio)
{
	return gpio->get(gpio);
}

static inline int gpio_set(GpioOps *gpio, unsigned val)
{
	return gpio->set(gpio, val);
}

GpioOps *new_gpio_high(void);
GpioOps *new_gpio_low(void);
GpioOps *new_gpio_not(GpioOps *a);
GpioOps *new_gpio_and(GpioOps *a, GpioOps *b);
GpioOps *new_gpio_or(GpioOps *a, GpioOps *b);

#endif /* __DRIVERS_GPIO_GPIO_H__ */

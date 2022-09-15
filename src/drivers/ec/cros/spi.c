/*
 * Chromium OS EC driver - SPI interface
 *
 * Copyright 2012 Google Inc.
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

/*
 * The Matrix Keyboard Protocol driver handles talking to the keyboard
 * controller chip. Mostly this is for keyboard functions, but some other
 * things have slipped in, so we provide generic services to talk to the
 * KBC.
 */

#include <assert.h>
#include <libpayload.h>

#include "base/container_of.h"
#include "config.h"
#include "drivers/bus/spi/spi.h"
#include "drivers/ec/cros/ec.h"
#include "drivers/ec/cros/spi.h"

// How long we have to leave CS deasserted between transactions.
static const uint64_t CsCooldownUs = 200;
// How long we'll wait for the EC to accept a packet and start handling it.
static const uint64_t AcceptTimeoutUs = 5 * 1000;
// How long we'll wait in total for a valid packet response from the EC.
static const uint64_t ProcessTimeoutUs = 1000 * 1000;

static void stop_bus(CrosEcSpiBus *bus)
{
	bus->spi->stop(bus->spi);
	bus->last_transfer = timer_us(0);
}

static int wait_for_frame(CrosEcSpiBus *bus, uint16_t command)
{
	uint64_t start = timer_us(0);
	int accept_timeout_us = AcceptTimeoutUs;
	int accepted = 0;
	uint8_t byte;

	// STM32 does XIP and can't handle interrupts timely while erasing.
	if (command == EC_CMD_GET_COMMS_STATUS)
		accept_timeout_us = ProcessTimeoutUs;

	while (1) {
		if (bus->spi->transfer(bus->spi, &byte, NULL, 1))
			return -1;

		switch (byte) {
		case EC_SPI_FRAME_START:
			// Done waiting, can start receiving response packet.
			return 0;
		case EC_SPI_PROCESSING:
			// EC has accepted our command and started processing.
			// It should continue sending 0xFA from here on out,
			// but we don't want to rely on that since the NPCX has
			// a bug corrupting every 256th byte it sends.
			accepted = 1;
			break;
		case EC_SPI_RX_BAD_DATA:
			printf("EC: Claims to have received bad data.\n");
			return -1;
		case EC_SPI_NOT_READY:
			printf("EC: Was not ready to receive host command.\n");
			return -1;
		default:
			// Probably EC_SPI_RECEIVING, or random garbage.
			break;
		}

		uint64_t waited = timer_us(start);
		if (!accepted && waited > accept_timeout_us) {
			// Don't spam if waiting to come back up after SW sync.
			if (command == EC_CMD_HELLO)
				return -1;
			printf("EC: Took too long to accept host command.\n");
			return -1;
		}
		if (waited > ProcessTimeoutUs) {
			printf("EC: Took too long to process host command.\n");
			return -1;
		}
	}
}

static int send_packet(CrosEcBusOps *me, const void *dout, uint32_t dout_len,
		       void *din, uint32_t din_len)
{
	CrosEcSpiBus *bus = container_of(me, CrosEcSpiBus, ops);

	while (timer_us(bus->last_transfer) < CsCooldownUs)
		;

	if (bus->spi->start(bus->spi))
		return -1;

	// Allow EC to ramp up clock after being awaken.
	// See chrome-os-partner:32223 for more details.
	udelay(CONFIG_DRIVER_EC_CROS_SPI_WAKEUP_DELAY_US);

	if (bus->spi->transfer(bus->spi, NULL, dout, dout_len)) {
		stop_bus(bus);
		return -1;
	}

	// Wait until the EC is ready. Do not print warnings for lack of reply
	// if the command is HELLO -- we use that to test if the EC is ready.
	const struct ec_host_request *rq = dout;
	if (wait_for_frame(bus, rq->command)) {
		stop_bus(bus);
		return -1;
	}

	if (bus->spi->transfer(bus->spi, din, NULL, din_len)) {
		stop_bus(bus);
		return -1;
	}

	stop_bus(bus);
	return 0;
}

static int send_command(CrosEcBusOps *me, uint8_t cmd, int cmd_version,
			const void *dout, uint32_t dout_len,
			void *din, uint32_t din_len)
{
	CrosEcSpiBus *bus = container_of(me, CrosEcSpiBus, ops);
	uint8_t *bytes;

	// Header + data + checksum.
	uint32_t out_bytes = CROS_EC_SPI_OUT_HDR_SIZE + dout_len + 1;
	uint32_t in_bytes = CROS_EC_SPI_IN_HDR_SIZE + din_len + 1;

	if (!bus->buf)
		bus->buf = xmalloc(MSG_BYTES);

	/*
	 * Sanity-check I/O sizes given transaction overhead in internal
	 * buffers.
	 */
	if (out_bytes > MSG_BYTES) {
		printf("%s: Cannot send %d bytes\n", __func__, dout_len);
		return -1;
	}
	if (in_bytes > MSG_BYTES) {
		printf("%s: Cannot receive %d bytes\n", __func__, din_len);
		return -1;
	}
	assert(dout_len >= 0);

	// Prepare the output.
	bytes = bus->buf;
	*bytes++ = EC_CMD_VERSION0 + cmd_version;
	*bytes++ = cmd;
	*bytes++ = dout_len;
	memcpy(bytes, dout, dout_len);
	bytes += dout_len;

	*bytes++ = cros_ec_calc_checksum(bus->buf,
					 CROS_EC_SPI_OUT_HDR_SIZE + dout_len);

	assert(out_bytes == bytes - (uint8_t *)bus->buf);

	// Send the output.
	cros_ec_dump_data("out", -1, bus->buf, out_bytes);

	while (timer_us(bus->last_transfer) < CsCooldownUs)
		;

	if (bus->spi->start(bus->spi))
		return -1;

	// Allow EC to ramp up clock after being awoken.
	// See chrome-os-partner:32223 for more details.
	udelay(CONFIG_DRIVER_EC_CROS_SPI_WAKEUP_DELAY_US);

	if (bus->spi->transfer(bus->spi, NULL, bus->buf, out_bytes)) {
		stop_bus(bus);
		return -1;
	}

	// Wait until the EC is ready. Do not print warnings for lack of reply
	// if the command is HELLO -- we use that to test if the EC is ready.
	if (wait_for_frame(bus, cmd)) {
		stop_bus(bus);
		return -1;
	}

	// Read the response code and the data length.
	bytes = bus->buf;
	if (bus->spi->transfer(bus->spi, bytes, NULL, 2)) {
		stop_bus(bus);
		return -1;
	}
	uint8_t result = *bytes++;
	uint8_t length = *bytes++;

	// Make sure there's enough room for the data.
	if (CROS_EC_SPI_IN_HDR_SIZE + length + 1 > MSG_BYTES) {
		printf("%s: Received length %#02x too large\n",
		       __func__, length);
		stop_bus(bus);
		return -1;
	}

	// Read the data and the checksum, and finish up.
	if (bus->spi->transfer(bus->spi, bytes, NULL, length + 1)) {
		stop_bus(bus);
		return -1;
	}
	bytes += length;
	int expected = *bytes++;
	stop_bus(bus);

	// Check the integrity of the response.
	if (result != EC_RES_SUCCESS) {
		printf("%s: Received bad result code %d\n", __func__, result);
		return -result;
	}

	int csum = cros_ec_calc_checksum(bus->buf,
					 CROS_EC_SPI_IN_HDR_SIZE + length);

	if (csum != expected) {
		printf("%s: Invalid checksum rx %#02x, calced %#02x\n",
		       __func__, expected, csum);
		return -1;
	}
	cros_ec_dump_data("in", -1, bus->buf,
			  CROS_EC_SPI_IN_HDR_SIZE + din_len + 1);

	// If the caller wants the response, copy it out for them.
	din_len = MIN(din_len, length);
	if (din) {
		memcpy(din, (uint8_t *)bus->buf + CROS_EC_SPI_IN_HDR_SIZE,
		       din_len);
	}

	return din_len;
}

CrosEcSpiBus *new_cros_ec_spi_bus(SpiOps *spi)
{
	assert(spi);

	CrosEcSpiBus *bus = xzalloc(sizeof(*bus));
	bus->ops.send_command = &send_command;
	bus->ops.send_packet = &send_packet;
	bus->spi = spi;

	return bus;
}

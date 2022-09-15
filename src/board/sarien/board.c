/*
 * Copyright 2018 Google LLC
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

#include <arch/io.h>
#include <libpayload.h>
#include <pci.h>
#include <pci/pci.h>
#include <sysinfo.h>

#include "base/init_funcs.h"
#include "base/list.h"
#include "config.h"
#include "drivers/bus/i2c/designware.h"
#include "drivers/bus/i2c/i2c.h"
#include "drivers/flash/flash.h"
#include "drivers/flash/memmapped.h"
#include "drivers/gpio/gpio.h"
#include "drivers/gpio/sysinfo.h"
#include "drivers/power/pch.h"
#include "drivers/soc/cannonlake.h"
#include "drivers/storage/ahci.h"
#include "drivers/storage/blockdev.h"
#include "drivers/storage/nvme.h"
#include "drivers/tpm/cr50_i2c.h"
#include "drivers/tpm/tpm.h"

static int cr50_irq_status(void)
{
	return cannonlake_get_gpe(GPE0_DW2_18);
}

static int board_setup(void)
{
	sysinfo_install_flags(NULL);

	/* 32MB SPI Flash */
	flash_set_ops(&new_mem_mapped_flash(0xfe000000, 0x2000000)->ops);

	/* H1 TPM on I2C bus 4 @ 400KHz, controller core is 133MHz */
	DesignwareI2c *i2c4 = new_pci_designware_i2c(
		PCI_DEV(0, 0x19, 0), 400000, CANNONLAKE_DW_I2C_MHZ);
	tpm_set_ops(&new_cr50_i2c(&i2c4->ops, 0x50,
				  &cr50_irq_status)->base.ops);

	/* Cannonlake PCH */
	power_set_ops(&cannonlake_power_ops);

	/* SATA AHCI */
	AhciCtrlr *ahci = new_ahci_ctrlr(PCI_DEV(0, 0x17, 0));
	list_insert_after(&ahci->ctrlr.list_node, &fixed_block_dev_controllers);

	/* M.2 2280 SSD x4 */
	NvmeCtrlr *nvme = new_nvme_ctrlr(PCI_DEV(0, 0x1d, 4));
	list_insert_after(&nvme->ctrlr.list_node, &fixed_block_dev_controllers);

	/* M.2 2280 SSD x4 (if root ports are coalesced)  */
	NvmeCtrlr *nvme_b = new_nvme_ctrlr(PCI_DEV(0, 0x1d, 0));
	list_insert_after(&nvme_b->ctrlr.list_node,
			  &fixed_block_dev_controllers);

	return 0;
}

INIT_FUNC(board_setup);

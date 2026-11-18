// SPDX-License-Identifier: GPL-2.0-only
/*
 * omap3-restart.c - Code common to all OMAP3xxx machines.
 *
 * Copyright (C) 2009, 2012 Texas Instruments
 * Copyright (C) 2010 Nokia Corporation
 * Tony Lindgren <tony@atomide.com>
 * Santosh Shilimkar <santosh.shilimkar@ti.com>
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/reboot.h>
#include <linux/of.h>
#include <linux/string.h>

#include "common.h"
#include "control.h"
#include "prm.h"

/* Global address base setup code */

/**
 * omap3xxx_restart - trigger a software restart of the SoC
 * @mode: the "reboot mode", see arch/arm/kernel/{setup,process}.c
 * @cmd: passed from the userspace program rebooting the system (if provided)
 *
 * Resets the SoC.  For @cmd, see the 'reboot' syscall in
 * kernel/sys.c.  No return value.
 */
void omap3xxx_restart(enum reboot_mode mode, const char *cmd)
{
	/* Samsung Latona SBL download protocol, from board-latona-reboot.c.
	 * These are on-chip control scratch registers, not storage partitions.
	 * Preserve the vendor OR semantics; SBL owns consumption/clearing.
	 */
	if (of_machine_is_compatible("samsung,latona")) {
		u32 reason = cmd && !strcmp(cmd, "download") ? BIT(5) : BIT(0);
		u32 scratch = omap_ctrl_readl(0x0918);

		omap_ctrl_writel(0x54455352, 0x09c4); /* little-endian "RSET" */
		omap_ctrl_writel(scratch | reason, 0x0918);
		/* Flush posted writes before the SoC reset. */
		omap_ctrl_readl(0x0918);
	} else {
		omap3_ctrl_write_boot_mode((cmd ? (u8)*cmd : 0));
	}
	omap_prm_reset_system();
}

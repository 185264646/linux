/* SPDX-License-Identifier: GPL-2.0 */
/*
 * HiSilicon specific clk ops
 */

#ifndef __LINUX_CLK_HISI_H_
#define __LINUX_CLK_HISI_H_

#include <linux/errno.h>

struct clk;

#ifdef CONFIG_COMMON_CLK_HISI_SDMMC_DLL
int hisi_sdmmc_dll_enable_tuning(struct clk *dll);
int hisi_sdmmc_dll_disable_tuning(struct clk *dll);
#else
inline static int hisi_sdmmc_dll_enable_tuning(struct clk *dll)
{
	return -ENOSYS;
}

inline static int hisi_sdmmc_dll_disable_tuning(struct clk *dll)
{
	return -ENOSYS;
}
#endif

#endif

//SPDX
/*
 * HiSilicon CRG controlled Delay-Locked-Loop(DLL) driver
 * Used by cclk_in_sample and cclk_in_drv for DesignWare SDMMC core found on many Hisilicon chips
 *
 * Copyright 2023 Yang Xiwen <forbidden405@outlook.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include "clk.h"

// reg definitions
// CTRL regs
// gate clk and reset bit (optional)
#define HISI_SDMMC_DLL_CKEN		BIT(0)
#define HISI_SDMMC_DLL_SRST_REQ		BIT(1)
// dsel (optional)
#define HISI_SDMMC_DLL_DSEL		GENMASK(3, 2)

#define HISI_SDMMC_DLL_TUNE		GENMASK(7, 4)
#define HISI_SDMMC_DLL_DLLSSEL		GENMASK(15, 8)
#define HISI_SDMMC_DLL_DLLMODE		BIT(16)
#define HISI_SDMMC_DLL_BYPASS		BIT(17)
#define HISI_SDMMC_DLL_STOP		BIT(18)
#define HISI_SDMMC_DLL_SLAVE_EN		BIT(19)

// STATUS regs
#define HISI_SDMMC_DLL_MDLY_TAP_FLAG	GENMASK(7, 0)
#define HISI_SDMMC_DLL_LOCKED_FLAG	BIT(8)
#define HISI_SDMMC_DLL_READY_FLAG	BIT(9)
#define HISI_SDMMC_DLL_OVERFLOW_FLAG	BIT(10)

struct hisi_sdmmc_dll_clk {
	struct clk_hw		hw;
	struct regmap		*map;
	struct clk		*parent;
	u32			offset[HISI_SDMMC_DLL_REG_CNT];
	spinlock_t		*lock;
	bool			has_gate_rst, has_dsel;
};

#define to_hisi_sdmmc_dll_clk(_hw) container_of(_hw, struct hisi_sdmmc_dll_clk, hw)

int hisi_sdmmc_dll_enable_tuning(struct clk *clk_dll)
{
	struct clk_hw *hw = __clk_get_hw(clk_dll);
	struct hisi_sdmmc_dll_clk *dll = to_hisi_sdmmc_dll_clk(hw);

	return regmap_clear_bits(dll->map, dll->offset[HISI_SDMMC_DLL_REG_CTRL], HISI_SDMMC_DLL_DLLMODE);
}
EXPORT_SYMBOL_GPL(hisi_sdmmc_dll_enable_tuning);

int hisi_sdmmc_dll_disable_tuning(struct clk *clk_dll)
{
	struct clk_hw *hw = __clk_get_hw(clk_dll);
	struct hisi_sdmmc_dll_clk *dll = to_hisi_sdmmc_dll_clk(hw);

	return regmap_set_bits(dll->map, dll->offset[HISI_SDMMC_DLL_REG_CTRL], HISI_SDMMC_DLL_DLLMODE);
}
EXPORT_SYMBOL_GPL(hisi_sdmmc_dll_disable_tuning);

static int hisi_sdmmc_dll_prepare(struct clk_hw *hw)
{
	struct hisi_sdmmc_dll_clk *dll = to_hisi_sdmmc_dll_clk(hw);
	struct clk_hw *parent_hw;
	u32 val, mask;

	parent_hw = clk_hw_get_parent(hw);
	if (!parent_hw)
		return -EINVAL;
	dll->parent = parent_hw->clk;

	mask = HISI_SDMMC_DLL_SLAVE_EN |
	       HISI_SDMMC_DLL_STOP |
	       HISI_SDMMC_DLL_BYPASS |
	       HISI_SDMMC_DLL_DLLMODE |
	       HISI_SDMMC_DLL_TUNE |
	       HISI_SDMMC_DLL_DLLSSEL;
	if (dll->has_dsel)
		mask |= HISI_SDMMC_DLL_DSEL;
	if (dll->has_gate_rst)
		mask |= HISI_SDMMC_DLL_SRST_REQ;

	val = HISI_SDMMC_DLL_SLAVE_EN |
	      HISI_SDMMC_DLL_DLLMODE;

	return regmap_update_bits(dll->map, dll->offset[HISI_SDMMC_DLL_REG_CTRL], mask, val);
}

static void hisi_sdmmc_dll_unprepare(struct clk_hw *hw) {
	struct hisi_sdmmc_dll_clk *dll = to_hisi_sdmmc_dll_clk(hw);
	u32 mask, val;

	mask = HISI_SDMMC_DLL_SLAVE_EN |
	       HISI_SDMMC_DLL_STOP |
	       HISI_SDMMC_DLL_BYPASS |
	       HISI_SDMMC_DLL_DLLMODE;
	val = HISI_SDMMC_DLL_STOP;

	if (dll->has_gate_rst) {
		mask |= HISI_SDMMC_DLL_SRST_REQ;
		val |= HISI_SDMMC_DLL_SRST_REQ;
	}

	regmap_update_bits(dll->map, dll->offset[HISI_SDMMC_DLL_REG_CTRL], mask, val);
}

inline static unsigned int hisi_sdmmc_dll_get_mdly_flag(struct hisi_sdmmc_dll_clk *dll)
{
	unsigned int val;
	
	regmap_read(dll->map, dll->offset[HISI_SDMMC_DLL_REG_STATUS], &val);

	return FIELD_GET(HISI_SDMMC_DLL_MDLY_TAP_FLAG, val);
}

inline static unsigned int hisi_sdmmc_dll_get_dllssel(struct hisi_sdmmc_dll_clk *dll)
{
	unsigned int val;

	regmap_read(dll->map, dll->offset[HISI_SDMMC_DLL_REG_CTRL], &val);

	return FIELD_GET(HISI_SDMMC_DLL_DLLSSEL, val);
}

inline static unsigned int hisi_sdmmc_dll_set_dllssel(struct hisi_sdmmc_dll_clk *dll, unsigned int val)
{
	u32 mask = HISI_SDMMC_DLL_DLLSSEL;
	val = FIELD_PREP(mask, val);

	return regmap_update_bits(dll->map, dll->offset[HISI_SDMMC_DLL_REG_CTRL], mask, val);
}

static int hisi_sdmmc_dll_get_phase(struct clk_hw *hw)
{
	struct hisi_sdmmc_dll_clk *dll = to_hisi_sdmmc_dll_clk(hw);
	unsigned int total, cur;
	int phase = clk_get_phase(dll->parent);
	unsigned long flags;

	if (!phase)
		return phase;

	spin_lock_irqsave(dll->lock, flags);

	total = hisi_sdmmc_dll_get_mdly_flag(dll);
	if (total == 0xFF) {
		pr_debug("%s: mdly flag overflow, dll is bypassed\n", __func__);
	} else {
		cur = hisi_sdmmc_dll_get_dllssel(dll);
		phase += cur * 360 / total;
	}

	spin_unlock_irqrestore(dll->lock, flags);
	return phase;
}

static int hisi_sdmmc_dll_set_phase(struct clk_hw *hw, int phase)
{
	struct hisi_sdmmc_dll_clk *dll = to_hisi_sdmmc_dll_clk(hw);
	unsigned int total, dsel;
	int parent_phase, self_phase, ret;
	unsigned long flags;

	spin_lock_irqsave(dll->lock, flags);

	total = hisi_sdmmc_dll_get_mdly_flag(dll);
	if (total == 0xFF) {
		pr_debug("%s: mdly flag overflow, dll is bypassed\n", __func__);
		spin_unlock_irqrestore(dll->lock, flags);
		ret = clk_set_phase(dll->parent, phase);
	} else {
		// only set parent phase to multiple of 45 degrees
		self_phase = phase % 45;
		parent_phase = phase - self_phase;
		dsel = self_phase * total / 360;

		ret = hisi_sdmmc_dll_set_dllssel(dll, dsel);

		spin_unlock_irqrestore(dll->lock, flags);
		ret = clk_set_phase(dll->parent, parent_phase);
	}

	return ret;
}

struct clk_ops hisi_sdmmc_dll_ops = {
	.prepare = hisi_sdmmc_dll_prepare,
	.unprepare = hisi_sdmmc_dll_unprepare,
	.get_phase = hisi_sdmmc_dll_get_phase,
	.set_phase = hisi_sdmmc_dll_set_phase,
};

struct clk *devm_clk_register_hisi_sdmmc_dll(struct device *dev, const struct hisi_sdmmc_dll *dll_data, spinlock_t *lock)
{
	struct hisi_sdmmc_dll_clk *dll;
	struct clk_init_data init;
	u32 flag = dll_data->dll_flags;

	dll = devm_kzalloc(dev, sizeof(*dll), GFP_KERNEL);
	if (!dll)
		return ERR_PTR(-ENOMEM);

	dll->map = dev_get_regmap(dev, NULL);
	if (!dll->map) {
		dev_err(dev, "failed to get regmap\n");
		return ERR_PTR(-ENODATA);
	}

	// parse flags
	dll->has_dsel = flag & HISI_SDMMC_DLL_HAS_DSEL;
	dll->has_gate_rst = flag & HISI_SDMMC_DLL_HAS_GATE_RST;

	dll->lock = lock;

	memcpy(dll->offset, dll_data->offset, sizeof(dll->offset));

	init.name = dll_data->name;
	init.ops = &hisi_sdmmc_dll_ops;
	init.flags = dll_data->flags;
	init.parent_names = &dll_data->parent_name;
	init.num_parents = 1;

	dll->hw.init = &init;

	return devm_clk_register(dev, &dll->hw);
}
EXPORT_SYMBOL_GPL(devm_clk_register_hisi_sdmmc_dll);

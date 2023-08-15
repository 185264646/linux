//SPDX
/*
 * HiSilicon CRG controlled Delay-Locked-Loop(DLL) driver
 * Used by cclk_in_sample and cclk_in_drv for DesignWare SDMMC core found on many Hisilicon chips
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>

// reg definitions
// CTRL regs
// gate clk and reset bit (optional)
#define HISI_SDMMC_DLL_CKEN		BIT(0)
#define HISI_SDMMC_DLL_SRST_REQ		BIT(1)
// dsel (optional)
#define HISI_SDMMC_DLL_DSEL		GENMASK(3, 2)

#define HISI_SDMMC_DLL_TUNE		GENMASK(7, 4)
#define HISI_SDMMC_DLL_DLLSSEL_SHIFT	8
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

// props
#define HISI_SDMMC_DLL_HAS_DSEL_PROP		"hisilicon,has-dsel"
#define HISI_SDMMC_DLL_HAS_GATE_RST_PROP	"hisilicon,has-gate-reset"

enum dll_type {
	HISI_EMMC_DLL,
	HISI_EMMC_SAP_DLL,
	HISI_SDIO_SAP_DLL,
};

struct hisi_sdmmc_dll {
	struct clk_hw hw;
	struct clk *parent;
	struct regmap *regmap_ctrl, *regmap_status;
	struct reset_control *rst;
	bool has_gate_rst, has_dsel;
};

#define to_hisi_sdmmc_dll(_hw) container_of(_hw, struct hisi_sdmmc_dll, hw)

static struct regmap_config ctrl_reg_config = {
	.name = "ctrl",
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.fast_io = true,
	.max_register = 1,
}, status_reg_config = {
	.name = "status",
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.fast_io = true,
	.max_register = 1,
};

int hisi_sdmmc_dll_enable_tuning(struct clk *clk_dll)
{
	struct clk_hw *hw = __clk_get_hw(clk_dll);
	struct hisi_sdmmc_dll *dll = to_hisi_sdmmc_dll(hw);

	return regmap_clear_bits(dll->regmap_ctrl, 0, HISI_SDMMC_DLL_DLLMODE);
}
EXPORT_SYMBOL_GPL(hisi_sdmmc_dll_enable_tuning);

int hisi_sdmmc_dll_disable_tuning(struct clk *clk_dll)
{
	struct clk_hw *hw = __clk_get_hw(clk_dll);
	struct hisi_sdmmc_dll *dll = to_hisi_sdmmc_dll(hw);

	return regmap_set_bits(dll->regmap_ctrl, 0, HISI_SDMMC_DLL_DLLMODE);
}
EXPORT_SYMBOL_GPL(hisi_sdmmc_dll_disable_tuning);

static int hisi_sdmmc_dll_enable(struct clk_hw *hw)
{
	struct hisi_sdmmc_dll *dll = to_hisi_sdmmc_dll(hw);
	u32 val, mask;

	mask = HISI_SDMMC_DLL_SLAVE_EN |
	       HISI_SDMMC_DLL_STOP |
	       HISI_SDMMC_DLL_BYPASS |
	       HISI_SDMMC_DLL_DLLMODE |
	       HISI_SDMMC_DLL_TUNE |
	       HISI_SDMMC_DLL_DLLSSEL;
	if (dll->has_dsel)
		mask |= HISI_SDMMC_DLL_DSEL;
	if (dll->has_gate_rst) {
		reset_control_deassert(dll->rst);
		mask |= HISI_SDMMC_DLL_CKEN;
	}
	val = HISI_SDMMC_DLL_SLAVE_EN |
	      HISI_SDMMC_DLL_DLLMODE;

	return regmap_update_bits(dll->regmap_ctrl, 0, mask, val);
}

static void hisi_sdmmc_dll_disable(struct clk_hw *hw) {
	struct hisi_sdmmc_dll *dll = to_hisi_sdmmc_dll(hw);
	u32 mask, val;

	mask = HISI_SDMMC_DLL_SLAVE_EN |
	       HISI_SDMMC_DLL_STOP |
	       HISI_SDMMC_DLL_BYPASS |
	       HISI_SDMMC_DLL_DLLMODE;
	if (dll->has_gate_rst) {
		reset_control_assert(dll->rst);
		mask |= HISI_SDMMC_DLL_CKEN;
	}

	val = HISI_SDMMC_DLL_STOP;

	regmap_update_bits(dll->regmap_ctrl, 0, mask, val);
}

inline static unsigned int hisi_sdmmc_dll_get_mdly_flag(struct regmap *status)
{
	unsigned int val;
	
	regmap_read(status, 0, &val);

	return val & HISI_SDMMC_DLL_MDLY_TAP_FLAG;
}

inline static unsigned int hisi_sdmmc_dll_get_dllssel(struct regmap *ctrl)
{
	unsigned int val;

	regmap_read(ctrl, 0, &val);

	val = (val & HISI_SDMMC_DLL_DLLSSEL) >> HISI_SDMMC_DLL_DLLSSEL_SHIFT;

	return val;
}

inline static unsigned int hisi_sdmmc_dll_set_dllssel(struct regmap *ctrl, unsigned int val)
{
	u32 mask = HISI_SDMMC_DLL_DLLSSEL;
	val <<= HISI_SDMMC_DLL_DLLSSEL_SHIFT;

	return regmap_update_bits(ctrl, 0, mask, val);
}

static int hisi_sdmmc_dll_get_phase(struct clk_hw *hw)
{
	struct hisi_sdmmc_dll *dll = to_hisi_sdmmc_dll(hw);
	unsigned int total, cur;
	int phase = clk_get_phase(dll->parent);

	if (phase < 0)
		return phase;

	total = hisi_sdmmc_dll_get_mdly_flag(dll->regmap_status);
	if (total == 0xFF) {
		pr_debug("hisi_sdmmc_dll: mdly flag overflow, dll is bypassed\n");
	} else {
		cur = hisi_sdmmc_dll_get_dllssel(dll->regmap_ctrl);
		phase += cur * 360 / total;
	}

	return phase;
}

static int hisi_sdmmc_dll_set_phase(struct clk_hw *hw, int phase)
{
	struct hisi_sdmmc_dll *dll = to_hisi_sdmmc_dll(hw);
	unsigned int total, dsel;
	int parent_phase, self_phase, ret;

	total = hisi_sdmmc_dll_get_mdly_flag(dll->regmap_status);
	if (total == 0xFF) {
		pr_debug("hisi_sdmmc_dll: mdly flag overflow, dll is bypassed\n");
		ret = clk_set_phase(dll->parent, phase);
	} else {
		// only set parent phase to multiple of 45 degrees
		self_phase = phase % 45;
		parent_phase = phase - self_phase;
		dsel = self_phase * total / 360;

		ret = clk_set_phase(dll->parent, parent_phase);
		if (ret < 0)
			return ret;
		ret = hisi_sdmmc_dll_set_dllssel(dll->regmap_ctrl, dsel);
	}

	return ret;
}

struct clk_ops hisi_sdmmc_dll_ops = {
	.enable = hisi_sdmmc_dll_enable,
	.disable = hisi_sdmmc_dll_disable,
	.get_phase = hisi_sdmmc_dll_get_phase,
	.set_phase = hisi_sdmmc_dll_set_phase,
};

static int hisi_sdmmc_dll_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct hisi_sdmmc_dll *dll;
	const struct clk_hw *hw;
	struct clk_init_data init;
	void __iomem *status_base, *ctrl_base;

	dll = devm_kzalloc(dev, sizeof(*dll), GFP_KERNEL);
	if (IS_ERR(dll))
		return PTR_ERR(dll);

	status_base = devm_platform_ioremap_resource_byname(pdev, "status");
	if (IS_ERR(status_base)) {
		dev_err(dev, "failed to remap status address space %ld\n", PTR_ERR(status_base));
		return PTR_ERR(status_base);
	}

	dll->regmap_status = devm_regmap_init_mmio(dev, status_base, &status_reg_config);
	if (IS_ERR(dll->regmap_status)) {
		dev_err(dev, "failed to register status regmap %ld\n", PTR_ERR(dll->regmap_status));
		return PTR_ERR(dll->regmap_status);
	};

	ctrl_base = devm_platform_ioremap_resource_byname(pdev, "ctrl");
	if (IS_ERR(ctrl_base)) {
		dev_err(dev, "failed to remap ctrl address space %ld\n", PTR_ERR(ctrl_base));
		return PTR_ERR(ctrl_base);
	}

	dll->regmap_ctrl = devm_regmap_init_mmio(dev, ctrl_base, &ctrl_reg_config);
	if (IS_ERR(dll->regmap_ctrl)) {
		dev_err(dev, "failed to register ctrl regmap %ld\n", PTR_ERR(dll->regmap_ctrl));
		return PTR_ERR(dll->regmap_ctrl);
	}

	// parse dt
	dll->has_dsel = of_property_read_bool(np, HISI_SDMMC_DLL_HAS_DSEL_PROP);
	dll->has_gate_rst = of_property_read_bool(np, HISI_SDMMC_DLL_HAS_GATE_RST_PROP);

	// get reset control
	if (dll->has_gate_rst) {
		dll->rst = devm_reset_control_get(dev, NULL);
		if (IS_ERR(dll->rst)) {
			dev_err(dev, "failed to get reset %ld\n", PTR_ERR(dll->rst));
			return PTR_ERR(dll->rst);
		}
	};

	// get parent
	dll->parent = devm_clk_get(dev, NULL);
	if (IS_ERR(dll->parent)) {
		dev_err(dev, "failed to get parent %ld\n", PTR_ERR(dll->parent));
		return PTR_ERR(dll->parent);
	}
	hw = __clk_get_hw(dll->parent);

	init.name = devm_kasprintf(dev, GFP_KERNEL, "%s", np->full_name);
	init.ops = &hisi_sdmmc_dll_ops;
	init.flags = 0;
	init.parent_hws = &hw;
	init.num_parents = 1;

	dll->hw.init = &init;

	// register clock
	// We can not use clk_regmap since it can only handle one regmap, but this clock gets two
	return devm_clk_hw_register(dev, &dll->hw);
}

static const struct of_device_id hisi_sdmmc_dll_device_ids[] = {
	{
		.compatible = "hisilicon,hisi-sdmmc-dll",
	}, {
		.compatible = "hisilicon,hi3798mv200-sdio-sap-dll",
	}, {
		.compatible = "hisilicon,hi3798mv200-emmc-sap-dll",
	}, {
		.compatible = "hisilicon,hi3798mv200-emmc-dll",
	}, {
		/* Senitiel */
	},
};
MODULE_DEVICE_TABLE(of, hisi_sdmmc_dll_device_ids);

static struct platform_driver hisi_sdmmc_dll_driver = {
	.probe = hisi_sdmmc_dll_probe,
	.driver = {
		.name = "hisi-sdmmc-dll",
		.of_match_table = hisi_sdmmc_dll_device_ids,
		.owner = THIS_MODULE,
	},
};
module_platform_driver(hisi_sdmmc_dll_driver);

MODULE_AUTHOR("Yang Xiwen <forbidden405@outlook.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Hisilicon CRG controlled Delay-Locked-Loop(DLL) driver");

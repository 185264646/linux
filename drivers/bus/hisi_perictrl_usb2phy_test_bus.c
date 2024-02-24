// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for the proprietary USB2 PHY configuration channel
 * on PERICTRL core for many HiSilicon HiSTB SoCs
 *
 * Copyright (C) 2024 Yang Xiwen <forbidden405@outlook.com>
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>

#define MAX_PHY_PORT_NUM	2

/* Register definitions */
/* Common parts */
#define PHY_TEST_WRDATA				GENMASK(7, 0)

/* For Hi3798MV100 */
#define HI3798MV100_PHY_TEST_ADDR		GENMASK(11, 8)
#define HI3798MV100_PHY_TEST_PORT		BIT(12)
#define HI3798MV100_PHY_TEST_WREN		BIT(13)
#define HI3798MV100_PHY_TEST_CLK		BIT(14)
#define HI3798MV100_PHY_TEST_RSTN		BIT(15)
#define HI3798MV100_PHY_TEST_RDDATA		GENMASK(23, 16)

/* For Hi3798CV200 */
#define HI3798CV200_PHY_TEST_ADDR		GENMASK(15, 8)
#define HI3798CV200_PHY_TEST_PORT		GENMASK(18, 16)
#define HI3798CV200_PHY_TEST_WREN		BIT(21)
#define HI3798CV200_PHY_TEST_CLK		BIT(22)
#define HI3798CV200_PHY_TEST_RSTN		BIT(23)
#define HI3798CV200_PHY_TEST_RDDATA		GENMASK(31, 24)

/* Virtual address encoding */
/*
 * Encode TEST_ADDR to bit0 - 7, TEST_PORT to bit8 - 10
 */
#define TEST_BUS_ADDR				GENMASK(7, 0)
#define TEST_BUS_PORT				GENMASK(10, 8)

/* Helper macros to extract TEST_ADDR and TEST_PORT from the virtual address */
#define EXTRACT_TEST_ADDR(addr)			FIELD_GET(TEST_BUS_ADDR, addr)
#define EXTRACT_TEST_PORT(addr)			FIELD_GET(TEST_BUS_PORT, addr)

enum hisi_perictrl_usb2phy_test_bus_type {
	HI3798MV100_PERICTRL,
	HI3798CV200_PERICTRL,
};

struct hisi_perictrl_usb2phy_test_bus_ctx {
	void __iomem *base;
	struct clk *test_clk;
	enum hisi_perictrl_usb2phy_test_bus_type type;
};

struct hisi_perictrl_usb2phy_test_bus_drvdata {
	struct regmap_config *cfg;
	struct regmap_bus *bus;
	enum hisi_perictrl_usb2phy_test_bus_type type;
};

static int hisi_perictrl_usb2phy_test_bus_write(struct hisi_perictrl_usb2phy_test_bus_ctx *ctx,
						unsigned int reg, unsigned int val)
{
	u32 reg_val;
	u8 addr, port;
	int ret;

	addr = EXTRACT_TEST_ADDR(reg);
	port = EXTRACT_TEST_PORT(reg);

	switch (ctx->type) {
	case HI3798MV100_PERICTRL:
		reg_val = FIELD_PREP(HI3798MV100_PHY_TEST_ADDR, addr) |
			FIELD_PREP(PHY_TEST_WRDATA, val) |
			FIELD_PREP(HI3798MV100_PHY_TEST_PORT, port) |
			HI3798MV100_PHY_TEST_WREN |
			HI3798MV100_PHY_TEST_RSTN;
		break;
	case HI3798CV200_PERICTRL:
		reg_val = FIELD_PREP(HI3798CV200_PHY_TEST_ADDR, addr) |
			FIELD_PREP(PHY_TEST_WRDATA, val) |
			FIELD_PREP(HI3798CV200_PHY_TEST_PORT, port) |
			HI3798CV200_PHY_TEST_WREN |
			HI3798CV200_PHY_TEST_RSTN;
		break;
	}

	writel(reg_val, ctx->base);

	ret = clk_enable(ctx->test_clk);
	if (!ret)
		return ret;

	clk_disable(ctx->test_clk);

	return 0;
}

static int hisi_perictrl_usb2phy_test_bus_read(struct hisi_perictrl_usb2phy_test_bus_ctx *ctx,
					       unsigned int reg, unsigned int *val)
{
	u32 reg_val;
	u8 addr, port;
	int ret;

	addr = EXTRACT_TEST_ADDR(reg);
	port = EXTRACT_TEST_PORT(reg);

	switch (ctx->type) {
	case HI3798MV100_PERICTRL:
		reg_val = FIELD_PREP(HI3798MV100_PHY_TEST_ADDR, addr) |
			FIELD_PREP(HI3798MV100_PHY_TEST_PORT, port) |
			HI3798MV100_PHY_TEST_RSTN;
		break;
	case HI3798CV200_PERICTRL:
		reg_val = FIELD_PREP(HI3798CV200_PHY_TEST_ADDR, addr) |
			FIELD_PREP(HI3798CV200_PHY_TEST_PORT, port) |
			HI3798CV200_PHY_TEST_RSTN;
		break;
	}

	writel(reg_val, ctx->base);

	ret = clk_enable(ctx->test_clk);
	if (!ret)
		return ret;

	reg_val = readl(ctx->base);

	switch (ctx->type) {
	case HI3798MV100_PERICTRL:
		*val = FIELD_GET(HI3798MV100_PHY_TEST_RDDATA, reg_val);
		break;
	case HI3798CV200_PERICTRL:
		*val = FIELD_GET(HI3798CV200_PHY_TEST_RDDATA, reg_val);
		break;
	}

	clk_disable(ctx->test_clk);

	return 0;
}

static int hisi_perictrl_usb2phy_test_bus_regmap_read(void *ctx, unsigned int reg,
						      unsigned int *val)
{
	return hisi_perictrl_usb2phy_test_bus_read(ctx, reg, val);
}

static int hisi_perictrl_usb2phy_test_bus_regmap_write(void *ctx, unsigned int reg,
						       unsigned int val)
{
	return hisi_perictrl_usb2phy_test_bus_write(ctx, reg, val);
}

struct regmap_bus hisi_perictrl_usb2phy_test_bus_regmap_bus = {
	// Really?
	.fast_io = true,
	.reg_write = hisi_perictrl_usb2phy_test_bus_regmap_write,
	.reg_read = hisi_perictrl_usb2phy_test_bus_regmap_read,
};

const struct regmap_range hi3798mv100_test_bus_regmap_ranges[] = {
	regmap_reg_range(0x0, 0xf),
	regmap_reg_range(0x100, 0x10f),
};

const struct regmap_access_table hi3798mv100_test_bus_regmap_access_table = {
	.yes_ranges = hi3798mv100_test_bus_regmap_ranges,
	.n_yes_ranges = ARRAY_SIZE(hi3798mv100_test_bus_regmap_ranges),
};

struct regmap_config hi3798mv100_test_bus_regmap_config = {
	.name = "hi3798mv100_perictrl_usb2phy_test_bus",
	.reg_bits = 9,
	.val_bits = 8,

	.rd_table = &hi3798mv100_test_bus_regmap_access_table,
	.wr_table = &hi3798mv100_test_bus_regmap_access_table,

	.max_register = 0x10f,
};

struct regmap_config hi3798cv200_test_bus_regmap_config = {
	.name = "hi3798cv200_perictrl_usb2phy_test_bus",
	.reg_bits = 12,
	.val_bits = 8,

	.max_register = 0xfff,
};

static int
hisi_perictrl_usb2phy_test_bus_clk_register(struct device *dev,
					    struct hisi_perictrl_usb2phy_test_bus_ctx *ctx)
{
	u32 bit_idx;
	static spinlock_t lock;
	struct clk_hw *hw;

	switch (ctx->type) {
	case HI3798MV100_PERICTRL:
		bit_idx = HI3798MV100_PHY_TEST_CLK;
		break;
	case HI3798CV200_PERICTRL:
		bit_idx = HI3798CV200_PHY_TEST_CLK;
		break;
	}

	hw = devm_clk_hw_register_gate(dev, "clk_usb2phy_test_bus", NULL, 0,
				       ctx->base, bit_idx, 0, &lock);
	if (IS_ERR(hw))
		return dev_err_probe(dev, PTR_ERR(hw), "Can not register test bus clock\n");

	ctx->test_clk = devm_clk_hw_get_clk(dev, hw, "test_bus");
	if (IS_ERR(ctx->test_clk))
		return dev_err_probe(dev, PTR_ERR(ctx->test_clk), "Can not get clk\n");

	return clk_prepare(ctx->test_clk);
}

static int hisi_perictrl_usb2phy_test_bus_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hisi_perictrl_usb2phy_test_bus_drvdata *drvdata = dev_get_drvdata(dev);
	struct hisi_perictrl_usb2phy_test_bus_ctx *ctx;
	struct regmap *regmap;
	int ret;

	if (!drvdata)
		return -EINVAL;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ctx->base))
		return PTR_ERR(ctx->base);

	ctx->type = drvdata->type;

	ret = hisi_perictrl_usb2phy_test_bus_clk_register(dev, ctx);
	if (ret)
		return ret;

	regmap = devm_regmap_init(dev, drvdata->bus, (void *)ctx, drvdata->cfg);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "Can not register regmap\n");

	return devm_of_platform_populate(dev);
}

struct hisi_perictrl_usb2phy_test_bus_drvdata hi3798mv100_test_bus_drvdata = {
	.cfg = &hi3798mv100_test_bus_regmap_config,
	.bus = &hisi_perictrl_usb2phy_test_bus_regmap_bus,
	.type = HI3798MV100_PERICTRL,
};

struct hisi_perictrl_usb2phy_test_bus_drvdata hi3798cv200_test_bus_drvdata = {
	.cfg = &hi3798cv200_test_bus_regmap_config,
	.bus = &hisi_perictrl_usb2phy_test_bus_regmap_bus,
	.type = HI3798CV200_PERICTRL,
};

static const struct of_device_id hisi_perictrl_usb2phy_test_bus_id_table[] = {
	{
		.compatible = "hisilicon,hi3798mv100-usb2phy-test-bus",
		.data = &hi3798mv100_test_bus_drvdata,
	}, {
		.compatible = "hisilicon,hi3798cv200-usb2phy-test-bus",
		.data = &hi3798cv200_test_bus_drvdata,
	}, { },
};

MODULE_DEVICE_TABLE(of, hisi_perictrl_usb2phy_test_bus_id_table);

static struct platform_driver hisi_perictrl_usb2phy_test_bus_driver = {
	.probe		= hisi_perictrl_usb2phy_test_bus_probe,
	.driver		= {
		.name	= "hisi-perictrl-usb2phy-test-bus",
		.of_match_table = of_match_ptr(hisi_perictrl_usb2phy_test_bus_id_table),
	},
};

module_platform_driver(hisi_perictrl_usb2phy_test_bus_driver);

MODULE_AUTHOR("Yang Xiwen <forbidden405@outlook.com>");
MODULE_DESCRIPTION("HiSilicon PERICTRL USB2 PHY test bus driver");
MODULE_LICENSE("GPL");

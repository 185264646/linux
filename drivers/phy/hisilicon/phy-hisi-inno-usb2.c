// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HiSilicon INNO USB2 PHY Driver.
 *
 * Copyright (c) 2016-2017 HiSilicon Technologies Co., Ltd.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#define INNO_PHY_PORT_NUM	2
#define REF_CLK_STABLE_TIME	100	/* unit:us */
#define UTMI_CLK_STABLE_TIME	200	/* unit:us */
#define TEST_CLK_STABLE_TIME	2	/* unit:ms */
#define PHY_CLK_STABLE_TIME	2	/* unit:ms */
#define UTMI_RST_COMPLETE_TIME	2	/* unit:ms */
#define POR_RST_COMPLETE_TIME	300	/* unit:us */

#define PHY_CLK_ENABLE		BIT(2)

struct hisi_inno_phy_port {
	struct reset_control *utmi_rst;
	struct hisi_inno_phy_priv *priv;
};

struct hisi_inno_phy_priv {
	struct regmap *regmap;
	unsigned int stride;
	struct clk *ref_clk;
	struct reset_control *por_rst;
	unsigned int type;
	struct hisi_inno_phy_port ports[INNO_PHY_PORT_NUM];
};

#define PORT_OFFSET		8

static int hisi_inno_phy_write_reg(struct hisi_inno_phy_priv *priv,
				    u8 port, u32 addr, u32 data)
{
	return regmap_write(priv->regmap, port << PORT_OFFSET | addr, data);
}

static void hisi_inno_phy_setup(struct hisi_inno_phy_priv *priv)
{
	int i;
	/* The phy clk is controlled by the port register 0x06. */
	for (i = 0; i < INNO_PHY_PORT_NUM; i++)
		hisi_inno_phy_write_reg(priv, i, 0x06 * priv->stride, PHY_CLK_ENABLE);
	msleep(PHY_CLK_STABLE_TIME);
}

static int hisi_inno_phy_init(struct phy *phy)
{
	struct hisi_inno_phy_port *port = phy_get_drvdata(phy);
	struct hisi_inno_phy_priv *priv = port->priv;
	int ret;

	ret = clk_prepare_enable(priv->ref_clk);
	if (ret)
		return ret;
	udelay(REF_CLK_STABLE_TIME);

	reset_control_deassert(priv->por_rst);
	udelay(POR_RST_COMPLETE_TIME);

	/* Set up phy registers */
	hisi_inno_phy_setup(priv);

	reset_control_deassert(port->utmi_rst);
	udelay(UTMI_RST_COMPLETE_TIME);

	return 0;
}

static int hisi_inno_phy_exit(struct phy *phy)
{
	struct hisi_inno_phy_port *port = phy_get_drvdata(phy);
	struct hisi_inno_phy_priv *priv = port->priv;

	reset_control_assert(port->utmi_rst);
	reset_control_assert(priv->por_rst);
	clk_disable_unprepare(priv->ref_clk);

	return 0;
}

static const struct phy_ops hisi_inno_phy_ops = {
	.init = hisi_inno_phy_init,
	.exit = hisi_inno_phy_exit,
	.owner = THIS_MODULE,
};

static struct regmap *hisi_inno_phy_get_regmap(struct device *dev, struct hisi_inno_phy_priv *priv)
{
	struct regmap *regmap;

	if (!dev->parent)
		return NULL;

	priv->stride = 1;
	/* First try getting regmap from parent device */
	regmap = dev_get_regmap(dev->parent, NULL);
	if (!regmap) {
		/*
		 * We are not under a perictrl usb2phy test bus.
		 * Use direct MMIO instead
		 *
		 * When MMIO is used, stride is 4 instead of 1.
		 */
		regmap = device_node_to_regmap(dev->parent->of_node);
		priv->stride = 4;
	}

	return regmap;
}

static int hisi_inno_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct hisi_inno_phy_priv *priv;
	struct phy_provider *provider;
	struct device_node *child;
	int i = 0;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->regmap = hisi_inno_phy_get_regmap(dev, priv);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	priv->ref_clk = devm_clk_get(dev, NULL);
	if (IS_ERR(priv->ref_clk))
		return PTR_ERR(priv->ref_clk);

	priv->por_rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(priv->por_rst))
		return PTR_ERR(priv->por_rst);

	for_each_child_of_node(np, child) {
		struct reset_control *rst;
		struct phy *phy;

		rst = of_reset_control_get_exclusive(child, NULL);
		if (IS_ERR(rst)) {
			of_node_put(child);
			return PTR_ERR(rst);
		}

		priv->ports[i].utmi_rst = rst;
		priv->ports[i].priv = priv;

		phy = devm_phy_create(dev, child, &hisi_inno_phy_ops);
		if (IS_ERR(phy)) {
			of_node_put(child);
			return PTR_ERR(phy);
		}

		phy_set_bus_width(phy, 8);
		phy_set_drvdata(phy, &priv->ports[i]);
		i++;

		if (i >= INNO_PHY_PORT_NUM) {
			dev_warn(dev, "Support %d ports in maximum\n", i);
			of_node_put(child);
			break;
		}
	}

	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static int __maybe_unused hisi_inno_phy_runtime_suspend(struct device *dev)
{
	int ret = 0;

	if (dev->parent)
		ret = pm_runtime_put_autosuspend(dev->parent);

	return ret;
}

static int __maybe_unused hisi_inno_phy_runtime_resume(struct device *dev)
{
	int ret = 0;

	if (dev->parent)
		ret = pm_runtime_resume_and_get(dev->parent);

	return ret;
}

static const struct dev_pm_ops hisi_inno_phy_pm_ops = {
	RUNTIME_PM_OPS(hisi_inno_phy_runtime_suspend, hisi_inno_phy_runtime_resume, NULL)
};

static const struct of_device_id hisi_inno_phy_of_match[] = {
	{ .compatible = "hisilicon,inno-usb2-phy", },
	{ .compatible = "hisilicon,hi3798cv200-usb2-phy", },
	{ .compatible = "hisilicon,hi3798mv100-usb2-phy", },
	{ .compatible = "hisilicon,hi3798mv200-usb2-phy", },
	{ },
};
MODULE_DEVICE_TABLE(of, hisi_inno_phy_of_match);

static struct platform_driver hisi_inno_phy_driver = {
	.probe	= hisi_inno_phy_probe,
	.driver = {
		.name	= "hisi-inno-phy",
		.of_match_table	= hisi_inno_phy_of_match,
		.pm	= &hisi_inno_phy_pm_ops,
	}
};
module_platform_driver(hisi_inno_phy_driver);

MODULE_DESCRIPTION("HiSilicon INNO USB2 PHY Driver");
MODULE_LICENSE("GPL v2");

// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018 HiSilicon Technologies Co., Ltd.
 */

#include <linux/clk.h>
#include <linux/mfd/syscon.h>
#include <linux/mmc/host.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include "dw_mmc.h"
#include "dw_mmc-pltfm.h"

#define ALL_INT_CLR		0x1ffff

enum dw_mci_histb_type {
	DW_MCI_TYPE_HI3798CV200,
};

static struct dw_mci_histb_compat {
	const char * const compatible;
	enum dw_mci_histb_type ctrl_type;
} histb_compat[] = {
	{
		.compatible = "hisilicon,hi3798cv200-dw-mshc",
		.ctrl_type = DW_MCI_TYPE_HI3798CV200,
	},
};

struct dw_mci_histb_priv {
	enum dw_mci_histb_type ctrl_type;
	struct clk *sample_clk;
	struct clk *drive_clk;
};

static void dw_mci_histb_set_ios(struct dw_mci *host, struct mmc_ios *ios)
{
	struct dw_mci_histb_priv *priv = host->priv;
	u32 val;

	val = mci_readl(host, UHS_REG);
	if (ios->timing == MMC_TIMING_MMC_DDR52 ||
	    ios->timing == MMC_TIMING_UHS_DDR50)
		val |= SDMMC_UHS_DDR;
	else
		val &= ~SDMMC_UHS_DDR;
	mci_writel(host, UHS_REG, val);

	val = mci_readl(host, ENABLE_SHIFT);
	if (ios->timing == MMC_TIMING_MMC_DDR52)
		val |= SDMMC_ENABLE_PHASE;
	else
		val &= ~SDMMC_ENABLE_PHASE;
	mci_writel(host, ENABLE_SHIFT, val);

	val = mci_readl(host, DDR_REG);
	if (ios->timing == MMC_TIMING_MMC_HS400)
		val |= SDMMC_DDR_HS400;
	else
		val &= ~SDMMC_DDR_HS400;
	mci_writel(host, DDR_REG, val);

	if (ios->timing == MMC_TIMING_MMC_HS ||
	    ios->timing == MMC_TIMING_LEGACY)
		clk_set_phase(priv->drive_clk, 180);
	else if (ios->timing == MMC_TIMING_MMC_HS200)
		clk_set_phase(priv->drive_clk, 135);
}

static int dw_mci_hi3798cv200_execute_tuning(struct dw_mci_slot *slot,
					     u32 opcode)
{
	static const int degrees[] = { 0, 45, 90, 135, 180, 225, 270, 315 };
	struct dw_mci *host = slot->host;
	struct dw_mci_histb_priv *priv = host->priv;
	int raise_point = -1, fall_point = -1;
	int err, prev_err = -1;
	int found = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(degrees); i++) {
		clk_set_phase(priv->sample_clk, degrees[i]);
		mci_writel(host, RINTSTS, ALL_INT_CLR);

		err = mmc_send_tuning(slot->mmc, opcode, NULL);
		if (!err)
			found = 1;

		if (i > 0) {
			if (err && !prev_err)
				fall_point = i - 1;
			if (!err && prev_err)
				raise_point = i;
		}

		if (raise_point != -1 && fall_point != -1)
			goto tuning_out;

		prev_err = err;
		err = 0;
	}

tuning_out:
	if (found) {
		if (raise_point == -1)
			raise_point = 0;
		if (fall_point == -1)
			fall_point = ARRAY_SIZE(degrees) - 1;
		if (fall_point < raise_point) {
			if ((raise_point + fall_point) >
			    (ARRAY_SIZE(degrees) - 1))
				i = fall_point / 2;
			else
				i = (raise_point + ARRAY_SIZE(degrees) - 1) / 2;
		} else {
			i = (raise_point + fall_point) / 2;
		}

		clk_set_phase(priv->sample_clk, degrees[i]);
		dev_dbg(host->dev, "Tuning clk_sample[%d, %d], set[%d]\n",
			raise_point, fall_point, degrees[i]);
	} else {
		dev_err(host->dev, "No valid clk_sample shift! use default\n");
		err = -EINVAL;
	}

	mci_writel(host, RINTSTS, ALL_INT_CLR);
	return err;
}

static int dw_mci_histb_init(struct dw_mci *host)
{
	struct dw_mci_histb_priv *priv;
	struct device_node *np = host->dev->of_node;
	int ret, idx;

	priv = devm_kzalloc(host->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	for (idx = 0; idx < ARRAY_SIZE(histb_compat); idx++) {
		if (of_device_is_compatible(np, histb_compat[idx].compatible))
			priv->ctrl_type = histb_compat[idx].ctrl_type;
	}

	priv->sample_clk = devm_clk_get(host->dev, "ciu-sample");
	if (IS_ERR(priv->sample_clk)) {
		dev_err(host->dev, "failed to get ciu-sample clock\n");
		return PTR_ERR(priv->sample_clk);
	}

	priv->drive_clk = devm_clk_get(host->dev, "ciu-drive");
	if (IS_ERR(priv->drive_clk)) {
		dev_err(host->dev, "failed to get ciu-drive clock\n");
		return PTR_ERR(priv->drive_clk);
	}

	ret = clk_prepare_enable(priv->sample_clk);
	if (ret) {
		dev_err(host->dev, "failed to enable ciu-sample clock\n");
		return ret;
	}

	ret = clk_prepare_enable(priv->drive_clk);
	if (ret) {
		dev_err(host->dev, "failed to enable ciu-drive clock\n");
		goto disable_sample_clk;
	}

	host->priv = priv;
	return 0;

disable_sample_clk:
	clk_disable_unprepare(priv->sample_clk);
	return ret;
}

static const struct dw_mci_drv_data hi3798cv200_data = {
	.common_caps = MMC_CAP_CMD23,
	.init = dw_mci_histb_init,
	.set_ios = dw_mci_histb_set_ios,
	.execute_tuning = dw_mci_hi3798cv200_execute_tuning,
};

static const struct of_device_id dw_mci_histb_match[] = {
	{ .compatible = "hisilicon,hi3798cv200-dw-mshc", .data = &hi3798cv200_data },
	{},
};

static int dw_mci_histb_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;

	match = of_match_node(dw_mci_histb_match, pdev->dev.of_node);

	return dw_mci_pltfm_register(pdev, match->data);
}

static int dw_mci_histb_remove(struct platform_device *pdev)
{
	struct dw_mci *host = platform_get_drvdata(pdev);
	struct dw_mci_histb_priv *priv = host->priv;

	clk_disable_unprepare(priv->drive_clk);
	clk_disable_unprepare(priv->sample_clk);

	dw_mci_pltfm_remove(pdev);

	return 0;
}

MODULE_DEVICE_TABLE(of, dw_mci_histb_match);
static struct platform_driver dw_mci_histb_driver = {
	.probe = dw_mci_histb_probe,
	.remove = dw_mci_histb_remove,
	.driver = {
		.name = "dwmmc_histb",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = dw_mci_histb_match,
	},
};
module_platform_driver(dw_mci_histb_driver);

MODULE_DESCRIPTION("HiSilicon HiSTB Specific DW-MSHC Driver Extension");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:dwmmc_histb");

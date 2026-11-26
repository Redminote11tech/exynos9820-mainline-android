// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Samsung Electronics. All rights reserved.
 * Adapted for mainline Linux 7.2 on Exynos 9820 (beyond2lte).
 */

#include "fingerprint.h"
#include "qbt2000_common.h"

int qbt2000_set_clk(struct qbt2000_drvdata *drvdata, bool onoff)
{
	int rc = 0;

	if (drvdata->enabled_clk == onoff)
		return 0;

	if (!drvdata->fp_spi_pclk || !drvdata->fp_spi_sclk) {
		drvdata->enabled_clk = onoff;
		return 0;
	}

	if (onoff) {
		rc = clk_prepare_enable(drvdata->fp_spi_pclk);
		if (rc)
			return rc;
		rc = clk_prepare_enable(drvdata->fp_spi_sclk);
		if (rc) {
			clk_disable_unprepare(drvdata->fp_spi_pclk);
			return rc;
		}
		if (drvdata->spi_speed > 0 &&
		    clk_get_rate(drvdata->fp_spi_sclk) != (unsigned long)drvdata->spi_speed * 4)
			clk_set_rate(drvdata->fp_spi_sclk, (unsigned long)drvdata->spi_speed * 4);
		if (drvdata->fp_spi_lock)
			__pm_stay_awake(drvdata->fp_spi_lock);
		drvdata->enabled_clk = true;
	} else {
		clk_disable_unprepare(drvdata->fp_spi_sclk);
		clk_disable_unprepare(drvdata->fp_spi_pclk);
		if (drvdata->fp_spi_lock)
			__pm_relax(drvdata->fp_spi_lock);
		drvdata->enabled_clk = false;
	}
	return 0;
}

int qbt2000_register_platform_variable(struct qbt2000_drvdata *drvdata)
{
	drvdata->fp_spi_pclk = devm_clk_get_optional(drvdata->dev, "fp-spi-pclk");
	if (IS_ERR(drvdata->fp_spi_pclk))
		drvdata->fp_spi_pclk = NULL;
	drvdata->fp_spi_sclk = devm_clk_get_optional(drvdata->dev, "fp-spi-sclk");
	if (IS_ERR(drvdata->fp_spi_sclk))
		drvdata->fp_spi_sclk = NULL;
	return 0;
}

int qbt2000_unregister_platform_variable(struct qbt2000_drvdata *drvdata)
{
	return 0;
}

int qbt2000_set_cpu_speedup(struct qbt2000_drvdata *drvdata, int onoff)
{
	return 0;
}

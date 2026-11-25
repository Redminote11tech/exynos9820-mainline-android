// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller driver for Samsung Exynos SoCs
 *
 * Copyright (C) 2013-2020 Samsung Electronics Co., Ltd.
 *		https://www.samsung.com
 *
 * Author: Jingoo Han <jg1.han@samsung.com>
 *	   Jaehoon Chung <jh80.chung@samsung.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/phy/phy.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/module.h>

#include "pcie-designware.h"

#define to_exynos_pcie(x)	dev_get_drvdata((x)->dev)

/* PCIe ELBI registers */
#define PCIE_IRQ_PULSE			0x000
#define IRQ_INTA_ASSERT			BIT(0)
#define IRQ_INTB_ASSERT			BIT(2)
#define IRQ_INTC_ASSERT			BIT(4)
#define IRQ_INTD_ASSERT			BIT(6)
#define PCIE_IRQ_LEVEL			0x004
#define PCIE_IRQ_SPECIAL		0x008
#define PCIE_IRQ_EN_PULSE		0x00c
#define PCIE_IRQ_EN_LEVEL		0x010
#define PCIE_IRQ_EN_SPECIAL		0x014
#define PCIE_SW_WAKE			0x018
#define PCIE_BUS_EN			BIT(1)
#define PCIE_CORE_RESET			0x01c
#define PCIE_CORE_RESET_ENABLE		BIT(0)
#define PCIE_STICKY_RESET		0x020
#define PCIE_NONSTICKY_RESET		0x024
#define PCIE_APP_INIT_RESET		0x028
#define PCIE_APP_LTSSM_ENABLE		0x02c
#define PCIE_ELBI_RDLH_LINKUP		0x074
#define PCIE_ELBI_XMLH_LINKUP		BIT(4)
#define PCIE_ELBI_LTSSM_ENABLE		0x1
#define PCIE_ELBI_SLV_AWMISC		0x11c
#define PCIE_ELBI_SLV_ARMISC		0x120
#define PCIE_ELBI_SLV_DBI_ENABLE	BIT(21)

struct exynos_pcie {
	struct dw_pcie			pci;
	struct clk_bulk_data		*clks;
	struct phy			*phy;
	struct regulator_bulk_data	supplies[2];
	bool				is_exynos9820;
	void __iomem			*phy_base;
	void __iomem			*pcs_base;
	void __iomem			*sysreg_base;
	struct regmap			*pmureg;
	struct gpio_desc		*perst_gpio;
	struct gpio_desc		*wl_reg_on_gpio;
};

static void exynos9820_pcie_phy_init(struct exynos_pcie *ep)
{
	static const u32 cmn_config_val[48] = {
		0x01, 0xE1, 0x05, 0x00, 0x88, 0x88, 0x88, 0x0C,
		0x61, 0x45, 0x65, 0x24, 0x33, 0x18, 0xE3, 0xFC,
		0xD8, 0x05, 0xE6, 0x80, 0x00, 0x00, 0x00, 0x00,
		0x60, 0x11, 0x00, 0xA0, 0x05, 0x04, 0x18, 0x88,
		0xC0, 0xFF, 0x9B, 0x52, 0x22, 0x30, 0x4F, 0xDC,
		0x40, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x80,
	};
	static const u32 trsv_config_val[80] = {
		0x31, 0x40, 0x37, 0x99, 0x85, 0x00, 0xC0, 0xFF,
		0xFF, 0x3F, 0x8C, 0xC8, 0x02, 0x01, 0x88, 0x80,
		0x06, 0x90, 0x6C, 0x66, 0x09, 0x61, 0x42, 0x44,
		0xC6, 0x50, 0x0A, 0x33, 0x58, 0xE7, 0x20, 0x22,
		0x80, 0x38, 0x05, 0x85, 0x00, 0x00, 0x00, 0x7E,
		0x00, 0x00, 0x55, 0x15, 0xAC, 0xAA, 0x3E, 0x00,
		0x00, 0x00, 0x20, 0x3F, 0x00, 0x03, 0x01, 0x00,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,
		0x05, 0x85, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	void __iomem *elbi = ep->pci.elbi_base;
	u32 val;
	int i;

	/* Set PCS_HIGH_SPEED and Lane0_enable */
	writel(readl(ep->sysreg_base) | BIT(1), ep->sysreg_base);
	val = readl(ep->sysreg_base + 0xC);
	val = (((val & ~(0xf << 4)) & ~(0xf << 2)) | (0x3 << 2)) & ~BIT(1);
	writel(val, ep->sysreg_base + 0xC);
	writel(readl(ep->sysreg_base + 0xC) | BIT(12), ep->sysreg_base + 0xC);

	/* pcs_g_rst */
	writel(0x1, elbi + 0x288);
	udelay(10);
	writel(0x0, elbi + 0x288);
	udelay(10);
	writel(0x1, elbi + 0x288);
	udelay(10);

	/* PHY Common & Transceiver/Receiver config (26MHz Gen2 Ch0) */
	for (i = 0; i < ARRAY_SIZE(cmn_config_val); i++)
		writel(cmn_config_val[i], ep->phy_base + (i * 4));
	for (i = 0; i < ARRAY_SIZE(trsv_config_val); i++)
		writel(trsv_config_val[i], ep->phy_base + ((0x30 + i) * 4));

	/* TX latency, PCS refclk out, L1SS timeout */
	writel(0x70, ep->pcs_base + 0xF8);
	writel(0x87, ep->pcs_base + 0x100);
	writel(0x50, ep->pcs_base + 0x104);
	val = readl(ep->pcs_base + 0xC);
	val = (val & ~BIT(1)) | BIT(4);
	writel(val, ep->pcs_base + 0xC);

	/* PCIE_MAC RST */
	writel(0x1, elbi + 0x290);
	udelay(10);
	writel(0x0, elbi + 0x290);
	udelay(10);
	writel(0x1, elbi + 0x290);
	udelay(10);

	/* PCIE_PHY PCS&PMA(CMN)_RST */
	writel(0x1, elbi + 0x28C);
	udelay(10);
	writel(0x0, elbi + 0x28C);
	udelay(10);
	writel(0x1, elbi + 0x28C);
	udelay(100);

	/* CDR Reset */
	val = readl(ep->pcs_base + 0xD0);
	val |= (0x3 << 6);
	writel(val, ep->pcs_base + 0xD0);
	udelay(20);
	val = readl(ep->pcs_base + 0xD0);
	val &= ~(0x3 << 6);
	val |= (0x2 << 6);
	writel(val, ep->pcs_base + 0xD0);
	val &= ~(0x3 << 6);
	writel(val, ep->pcs_base + 0xD0);
}

static void exynos_pcie_writel(void __iomem *base, u32 val, u32 reg)
{
	writel(val, base + reg);
}

static u32 exynos_pcie_readl(void __iomem *base, u32 reg)
{
	return readl(base + reg);
}

static void exynos_pcie_sideband_dbi_w_mode(struct exynos_pcie *ep, bool on)
{
	struct dw_pcie *pci = &ep->pci;
	u32 val;

	val = exynos_pcie_readl(pci->elbi_base, PCIE_ELBI_SLV_AWMISC);
	if (on)
		val |= PCIE_ELBI_SLV_DBI_ENABLE;
	else
		val &= ~PCIE_ELBI_SLV_DBI_ENABLE;
	exynos_pcie_writel(pci->elbi_base, val, PCIE_ELBI_SLV_AWMISC);
}

static void exynos_pcie_sideband_dbi_r_mode(struct exynos_pcie *ep, bool on)
{
	struct dw_pcie *pci = &ep->pci;
	u32 val;

	val = exynos_pcie_readl(pci->elbi_base, PCIE_ELBI_SLV_ARMISC);
	if (on)
		val |= PCIE_ELBI_SLV_DBI_ENABLE;
	else
		val &= ~PCIE_ELBI_SLV_DBI_ENABLE;
	exynos_pcie_writel(pci->elbi_base, val, PCIE_ELBI_SLV_ARMISC);
}

static void exynos_pcie_assert_core_reset(struct exynos_pcie *ep)
{
	struct dw_pcie *pci = &ep->pci;
	u32 val;

	val = exynos_pcie_readl(pci->elbi_base, PCIE_CORE_RESET);
	val &= ~PCIE_CORE_RESET_ENABLE;
	exynos_pcie_writel(pci->elbi_base, val, PCIE_CORE_RESET);
	exynos_pcie_writel(pci->elbi_base, 0, PCIE_STICKY_RESET);
	exynos_pcie_writel(pci->elbi_base, 0, PCIE_NONSTICKY_RESET);
}

static void exynos_pcie_deassert_core_reset(struct exynos_pcie *ep)
{
	struct dw_pcie *pci = &ep->pci;
	u32 val;

	val = exynos_pcie_readl(pci->elbi_base, PCIE_CORE_RESET);
	val |= PCIE_CORE_RESET_ENABLE;

	exynos_pcie_writel(pci->elbi_base, val, PCIE_CORE_RESET);
	exynos_pcie_writel(pci->elbi_base, 1, PCIE_STICKY_RESET);
	exynos_pcie_writel(pci->elbi_base, 1, PCIE_NONSTICKY_RESET);
	exynos_pcie_writel(pci->elbi_base, 1, PCIE_APP_INIT_RESET);
	exynos_pcie_writel(pci->elbi_base, 0, PCIE_APP_INIT_RESET);
}

static int exynos_pcie_start_link(struct dw_pcie *pci)
{
	struct exynos_pcie *ep = to_exynos_pcie(pci);
	u32 val;

	if (ep->is_exynos9820) {
		u32 pmu_val = 0;

		/* Post-dw_pcie_setup_rc Exynos 9820 RC tuning (matching downstream exynos_pcie_setup_rc) */
		dw_pcie_dbi_ro_wr_en(pci);
		dw_pcie_writel_dbi(pci, 0xB40, 0x1A);
		dw_pcie_writel_dbi(pci, 0xB44, 0xEA);
		val = dw_pcie_readl_dbi(pci, 0x70 + PCI_EXP_LNKCAP);
		val &= ~(PCI_EXP_LNKCAP_L1EL | PCI_EXP_LNKCAP_MLW | PCI_EXP_LNKCAP_SLS);
		val |= (0x7 << 15) | (0x1 << 4) | PCI_EXP_LNKCAP_SLS_5_0GB;
		dw_pcie_writel_dbi(pci, 0x70 + PCI_EXP_LNKCAP, val);
		dw_pcie_writel_dbi(pci, 0x40 + PCI_PM_CTRL, 0x0);
		val = dw_pcie_readl_dbi(pci, 0x70 + PCI_EXP_LNKCTL);
		dw_pcie_writel_dbi(pci, 0x70 + PCI_EXP_LNKCTL, val | PCI_EXP_LNKCTL_RL);
		val = dw_pcie_readl_dbi(pci, 0x70 + PCI_EXP_LNKCTL2);
		val = (val & ~PCI_EXP_LNKCTL2_TLS) | PCI_EXP_LNKCTL2_TLS_2_5GT;
		dw_pcie_writel_dbi(pci, 0x70 + PCI_EXP_LNKCTL2, val);
		dw_pcie_dbi_ro_wr_dis(pci);

		/* Enable LTSSM history buffer and power state masks */
		exynos_pcie_writel(pci->elbi_base, 0x0, 0x274);
		exynos_pcie_writel(pci->elbi_base, 0x3, 0x274);
		exynos_pcie_writel(pci->elbi_base, 0x200000, 0x2BC);
		exynos_pcie_writel(pci->elbi_base, 0xffffffff, 0x2C0);

		/* Re-enable RX elecidle check before LTSSM link training */
		val = readl(ep->pcs_base + 0xEC);
		val &= ~BIT(3);
		writel(val, ep->pcs_base + 0xEC);

		exynos_pcie_writel(pci->elbi_base, PCIE_ELBI_LTSSM_ENABLE,
				   PCIE_APP_LTSSM_ENABLE);
		msleep(50);
		regmap_read(ep->pmureg, 0x071C, &pmu_val);
		dev_info(pci->dev, "exynos9820-pcie: pmu=0x%x phy0=0x%x sys0=0x%x sysc=0x%x linkup=0x%x hist=0x%x,0x%x,0x%x,0x%x\n",
			 pmu_val, readl(ep->phy_base),
			 readl(ep->sysreg_base), readl(ep->sysreg_base + 0xC),
			 exynos_pcie_readl(pci->elbi_base, PCIE_ELBI_RDLH_LINKUP),
			 exynos_pcie_readl(pci->elbi_base, 0x138),
			 exynos_pcie_readl(pci->elbi_base, 0x13C),
			 exynos_pcie_readl(pci->elbi_base, 0x140),
			 exynos_pcie_readl(pci->elbi_base, 0x144));
		return 0;
	}

	val = exynos_pcie_readl(pci->elbi_base, PCIE_SW_WAKE);
	val &= ~PCIE_BUS_EN;
	exynos_pcie_writel(pci->elbi_base, val, PCIE_SW_WAKE);

	/* assert LTSSM enable */
	exynos_pcie_writel(pci->elbi_base, PCIE_ELBI_LTSSM_ENABLE,
			  PCIE_APP_LTSSM_ENABLE);
	return 0;
}

static void exynos_pcie_clear_irq_pulse(struct exynos_pcie *ep)
{
	struct dw_pcie *pci = &ep->pci;

	u32 val = exynos_pcie_readl(pci->elbi_base, PCIE_IRQ_PULSE);

	exynos_pcie_writel(pci->elbi_base, val, PCIE_IRQ_PULSE);
}

static irqreturn_t exynos_pcie_irq_handler(int irq, void *arg)
{
	struct exynos_pcie *ep = arg;

	exynos_pcie_clear_irq_pulse(ep);
	return IRQ_HANDLED;
}

static void exynos_pcie_enable_irq_pulse(struct exynos_pcie *ep)
{
	struct dw_pcie *pci = &ep->pci;

	u32 val = IRQ_INTA_ASSERT | IRQ_INTB_ASSERT |
		  IRQ_INTC_ASSERT | IRQ_INTD_ASSERT;

	if (ep->is_exynos9820) {
		val |= BIT(29); /* IRQ_MSI_CTRL_EN_RISING_EDG */
		exynos_pcie_writel(pci->elbi_base, 0, 0x34);
		exynos_pcie_writel(pci->elbi_base, 0, 0x38);
	}

	exynos_pcie_writel(pci->elbi_base, val, PCIE_IRQ_EN_PULSE);
	exynos_pcie_writel(pci->elbi_base, 0, PCIE_IRQ_EN_LEVEL);
	exynos_pcie_writel(pci->elbi_base, 0, PCIE_IRQ_EN_SPECIAL);
}

static void exynos9820_pcie_msi_isr(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct dw_pcie_rp *pp = irq_desc_get_handler_data(desc);
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	u32 val;

	chained_irq_enter(chip, desc);

	val = exynos_pcie_readl(pci->elbi_base, PCIE_IRQ_PULSE);
	exynos_pcie_writel(pci->elbi_base, val, PCIE_IRQ_PULSE);

	if (val & BIT(29)) {
		dw_handle_msi_irq(pp);
		dw_pcie_writel_dbi(pci, PCIE_MSI_INTR0_MASK, 0xffffffff);
		dw_pcie_writel_dbi(pci, PCIE_MSI_INTR0_MASK, 0x0);
	}

	val = exynos_pcie_readl(pci->elbi_base, PCIE_IRQ_LEVEL);
	exynos_pcie_writel(pci->elbi_base, val, PCIE_IRQ_LEVEL);
	val = exynos_pcie_readl(pci->elbi_base, PCIE_IRQ_SPECIAL);
	exynos_pcie_writel(pci->elbi_base, val, PCIE_IRQ_SPECIAL);

	chained_irq_exit(chip, desc);
}

static u32 exynos_pcie_read_dbi(struct dw_pcie *pci, void __iomem *base,
				u32 reg, size_t size)
{
	struct exynos_pcie *ep = to_exynos_pcie(pci);
	u32 val;

	if (!ep->is_exynos9820)
		exynos_pcie_sideband_dbi_r_mode(ep, true);
	dw_pcie_read(base + reg, size, &val);
	if (!ep->is_exynos9820)
		exynos_pcie_sideband_dbi_r_mode(ep, false);
	return val;
}

static void exynos_pcie_write_dbi(struct dw_pcie *pci, void __iomem *base,
				  u32 reg, size_t size, u32 val)
{
	struct exynos_pcie *ep = to_exynos_pcie(pci);

	if (!ep->is_exynos9820)
		exynos_pcie_sideband_dbi_w_mode(ep, true);
	dw_pcie_write(base + reg, size, val);
	if (!ep->is_exynos9820)
		exynos_pcie_sideband_dbi_w_mode(ep, false);
}

static int exynos_pcie_rd_own_conf(struct pci_bus *bus, unsigned int devfn,
				   int where, int size, u32 *val)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(bus->sysdata);

	if (PCI_SLOT(devfn))
		return PCIBIOS_DEVICE_NOT_FOUND;

	*val = dw_pcie_read_dbi(pci, where, size);
	return PCIBIOS_SUCCESSFUL;
}

static int exynos_pcie_wr_own_conf(struct pci_bus *bus, unsigned int devfn,
				   int where, int size, u32 val)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(bus->sysdata);

	if (PCI_SLOT(devfn))
		return PCIBIOS_DEVICE_NOT_FOUND;

	dw_pcie_write_dbi(pci, where, size, val);
	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops exynos_pci_ops = {
	.read = exynos_pcie_rd_own_conf,
	.write = exynos_pcie_wr_own_conf,
};

static bool exynos_pcie_link_up(struct dw_pcie *pci)
{
	struct exynos_pcie *ep = to_exynos_pcie(pci);
	u32 val = exynos_pcie_readl(pci->elbi_base, PCIE_ELBI_RDLH_LINKUP);

	if (ep->is_exynos9820) {
		val &= 0x1f;
		return val >= 0x0d && val <= 0x14;
	}

	return val & PCIE_ELBI_XMLH_LINKUP;
}

static int exynos_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct exynos_pcie *ep = to_exynos_pcie(pci);

	pp->bridge->ops = &exynos_pci_ops;

	if (ep->is_exynos9820) {
		u32 val;

		/* Power on BCM4375 WLAN chip via WL_REG_ON */
		if (ep->wl_reg_on_gpio) {
			gpiod_set_value_cansleep(ep->wl_reg_on_gpio, 1);
			msleep(250);
		}

		/* Release PMU PCIe0 PHY isolation (0x071C) */
		regmap_update_bits(ep->pmureg, 0x071C, BIT(0), 1);

		/* Clear PHY all power down */
		writel(readl(ep->pcs_base + 0x100) & ~BIT(6), ep->pcs_base + 0x100);
		writel(readl(ep->pcs_base + 0x104) & ~BIT(7), ep->pcs_base + 0x104);
		writel(0xC0, ep->phy_base + (0x20 * 4));
		writel(0x7E, ep->phy_base + (0x57 * 4));

		/* Ignore RX elecidle during DBI setup */
		writel(readl(ep->pcs_base + 0xEC) | BIT(3), ep->pcs_base + 0xEC);

		/* Pulse PCIE_SOFT_CORE_RESET (0x1D0) */
		exynos_pcie_writel(pci->elbi_base, 0x0, 0x1D0);
		udelay(20);
		exynos_pcie_writel(pci->elbi_base, 0x1, 0x1D0);

		/* Deassert PERST# */
		if (ep->perst_gpio) {
			gpiod_set_value_cansleep(ep->perst_gpio, 1);
			usleep_range(18000, 20000);
		}

		/* ELBI L1 exit, linkdown manual reset, Q-channel no gating */
		val = exynos_pcie_readl(pci->elbi_base, 0xF4);
		exynos_pcie_writel(pci->elbi_base, val | 0x11, 0xF4);
		exynos_pcie_writel(pci->elbi_base, 0x2, 0x1B8);
		val = exynos_pcie_readl(pci->elbi_base, 0x2C8);
		exynos_pcie_writel(pci->elbi_base, val & ~0xfff, 0x2C8);

		/* Program Exynos 9820 Ch0 Gen2 PHY */
		exynos9820_pcie_phy_init(ep);

		/* Set 26MHz aux clk, L1SS duration, LNKCAP x1 Gen2, target Gen1 */
		dw_pcie_dbi_ro_wr_en(pci);
		dw_pcie_writel_dbi(pci, 0xB40, 0x1A);
		dw_pcie_writel_dbi(pci, 0xB44, 0xEA);
		val = dw_pcie_readl_dbi(pci, 0x70 + PCI_EXP_LNKCAP);
		val &= ~(PCI_EXP_LNKCAP_L1EL | PCI_EXP_LNKCAP_MLW | PCI_EXP_LNKCAP_SLS);
		val |= (0x7 << 15) | (0x1 << 4) | PCI_EXP_LNKCAP_SLS_5_0GB;
		dw_pcie_writel_dbi(pci, 0x70 + PCI_EXP_LNKCAP, val);
		val = dw_pcie_readl_dbi(pci, 0x70 + PCI_EXP_LNKCTL2);
		val = (val & ~PCI_EXP_LNKCTL2_TLS) | PCI_EXP_LNKCTL2_TLS_2_5GT;
		dw_pcie_writel_dbi(pci, 0x70 + PCI_EXP_LNKCTL2, val);
		dw_pcie_dbi_ro_wr_dis(pci);

		dev_info(pci->dev, "exynos9820-pcie: wl=%d perst=%d linkup=0x%x dstate=0x%x\n",
			 ep->wl_reg_on_gpio ? gpiod_get_value_cansleep(ep->wl_reg_on_gpio) : -1,
			 ep->perst_gpio ? gpiod_get_value_cansleep(ep->perst_gpio) : -1,
			 exynos_pcie_readl(pci->elbi_base, PCIE_ELBI_RDLH_LINKUP),
			 exynos_pcie_readl(pci->elbi_base, 0x88));

		exynos_pcie_enable_irq_pulse(ep);
		return 0;
	}

	exynos_pcie_assert_core_reset(ep);

	phy_init(ep->phy);
	phy_power_on(ep->phy);

	exynos_pcie_deassert_core_reset(ep);
	exynos_pcie_enable_irq_pulse(ep);

	return 0;
}

static const struct dw_pcie_host_ops exynos_pcie_host_ops = {
	.init = exynos_pcie_host_init,
};

static int exynos_add_pcie_port(struct exynos_pcie *ep,
				       struct platform_device *pdev)
{
	struct dw_pcie *pci = &ep->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	struct device *dev = &pdev->dev;
	int ret;

	pp->irq = platform_get_irq(pdev, 0);
	if (pp->irq < 0)
		return pp->irq;

	pp->ops = &exynos_pcie_host_ops;

	if (ep->is_exynos9820) {
		/* Let dw_pcie_msi_host_init() set up DWC MSI on pp->irq, then
		 * wrap it with exynos9820_pcie_msi_isr to clear ELBI IRQ_PULSE. */
		pp->msi_irq[0] = pp->irq;
		ret = dw_pcie_host_init(pp);
		if (ret) {
			dev_err(dev, "failed to initialize host\n");
			return ret;
		}
		irq_set_chained_handler_and_data(pp->msi_irq[0],
						 exynos9820_pcie_msi_isr, pp);
		return 0;
	}

	ret = devm_request_irq(dev, pp->irq, exynos_pcie_irq_handler,
			       IRQF_SHARED, "exynos-pcie", ep);
	if (ret) {
		dev_err(dev, "failed to request irq\n");
		return ret;
	}

	pp->msi_irq[0] = -ENODEV;

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "failed to initialize host\n");
		return ret;
	}

	return 0;
}

static const struct dw_pcie_ops dw_pcie_ops = {
	.read_dbi = exynos_pcie_read_dbi,
	.write_dbi = exynos_pcie_write_dbi,
	.link_up = exynos_pcie_link_up,
	.start_link = exynos_pcie_start_link,
};

static int exynos_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos_pcie *ep;
	struct device_node *np = dev->of_node;
	int ret;

	ep = devm_kzalloc(dev, sizeof(*ep), GFP_KERNEL);
	if (!ep)
		return -ENOMEM;

	ep->pci.dev = dev;
	ep->pci.ops = &dw_pcie_ops;
	ep->is_exynos9820 = !!of_device_get_match_data(dev);

	if (ep->is_exynos9820) {
		ep->phy_base = devm_platform_ioremap_resource_byname(pdev, "phy");
		if (IS_ERR(ep->phy_base))
			return PTR_ERR(ep->phy_base);
		ep->pcs_base = devm_platform_ioremap_resource_byname(pdev, "pcs");
		if (IS_ERR(ep->pcs_base))
			return PTR_ERR(ep->pcs_base);
		ep->sysreg_base = devm_platform_ioremap_resource_byname(pdev, "sysreg");
		if (IS_ERR(ep->sysreg_base))
			return PTR_ERR(ep->sysreg_base);
		ep->pmureg = syscon_regmap_lookup_by_phandle(np, "samsung,pmu-syscon");
		if (IS_ERR(ep->pmureg))
			return PTR_ERR(ep->pmureg);
		ep->perst_gpio = devm_gpiod_get_optional(dev, "perst", GPIOD_OUT_LOW);
		if (IS_ERR(ep->perst_gpio))
			return PTR_ERR(ep->perst_gpio);
		ep->wl_reg_on_gpio = devm_gpiod_get_optional(dev, "wl-reg-on", GPIOD_OUT_LOW);
		if (IS_ERR(ep->wl_reg_on_gpio))
			return PTR_ERR(ep->wl_reg_on_gpio);

		ret = devm_clk_bulk_get_all_enabled(dev, &ep->clks);
		if (ret < 0)
			return ret;

		platform_set_drvdata(pdev, ep);
		return exynos_add_pcie_port(ep, pdev);
	}

	ep->phy = devm_of_phy_get(dev, np, NULL);
	if (IS_ERR(ep->phy))
		return PTR_ERR(ep->phy);

	ret = devm_clk_bulk_get_all_enabled(dev, &ep->clks);
	if (ret < 0)
		return ret;

	ep->supplies[0].supply = "vdd18";
	ep->supplies[1].supply = "vdd10";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ep->supplies),
				      ep->supplies);
	if (ret)
		return ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ep->supplies), ep->supplies);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, ep);

	ret = exynos_add_pcie_port(ep, pdev);
	if (ret < 0)
		goto fail_probe;

	return 0;

fail_probe:
	phy_exit(ep->phy);
	regulator_bulk_disable(ARRAY_SIZE(ep->supplies), ep->supplies);

	return ret;
}

static void exynos_pcie_remove(struct platform_device *pdev)
{
	struct exynos_pcie *ep = platform_get_drvdata(pdev);

	dw_pcie_host_deinit(&ep->pci.pp);
	exynos_pcie_assert_core_reset(ep);
	phy_power_off(ep->phy);
	phy_exit(ep->phy);
	regulator_bulk_disable(ARRAY_SIZE(ep->supplies), ep->supplies);
}

static int exynos_pcie_suspend_noirq(struct device *dev)
{
	struct exynos_pcie *ep = dev_get_drvdata(dev);

	exynos_pcie_assert_core_reset(ep);
	phy_power_off(ep->phy);
	phy_exit(ep->phy);
	regulator_bulk_disable(ARRAY_SIZE(ep->supplies), ep->supplies);

	return 0;
}

static int exynos_pcie_resume_noirq(struct device *dev)
{
	struct exynos_pcie *ep = dev_get_drvdata(dev);
	struct dw_pcie *pci = &ep->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ep->supplies), ep->supplies);
	if (ret)
		return ret;

	/* exynos_pcie_host_init controls ep->phy */
	exynos_pcie_host_init(pp);
	dw_pcie_setup_rc(pp);
	exynos_pcie_start_link(pci);
	return dw_pcie_wait_for_link(pci);
}

static const struct dev_pm_ops exynos_pcie_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(exynos_pcie_suspend_noirq,
				  exynos_pcie_resume_noirq)
};

static const struct of_device_id exynos_pcie_of_match[] = {
	{ .compatible = "samsung,exynos5433-pcie", },
	{ .compatible = "samsung,exynos9820-pcie", .data = (void *)1, },
	{ },
};

static struct platform_driver exynos_pcie_driver = {
	.probe		= exynos_pcie_probe,
	.remove		= exynos_pcie_remove,
	.driver = {
		.name	= "exynos-pcie",
		.of_match_table = exynos_pcie_of_match,
		.pm		= &exynos_pcie_pm_ops,
	},
};
module_platform_driver(exynos_pcie_driver);
MODULE_DESCRIPTION("Samsung Exynos PCIe host controller driver");
MODULE_LICENSE("GPL v2");
MODULE_DEVICE_TABLE(of, exynos_pcie_of_match);

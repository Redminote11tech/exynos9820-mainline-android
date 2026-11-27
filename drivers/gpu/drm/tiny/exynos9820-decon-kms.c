// SPDX-License-Identifier: GPL-2.0
/*
 * Exynos 9820 DECON0 "handover" KMS driver (adapted from pulpoff/linux-beyond0lte).
 *
 * Performs zero-copy page flips on the DECON0 + GF0 IDMA + DSIM0 pipeline that
 * S-Boot leaves running in MIPI command mode with HW TE trigger unmasked.
 * Supports hardware pixel formats XRGB8888, ARGB8888, XBGR8888, ABGR8888 and
 * 60 Hz hardware TE vblank interrupts (DECON_INT_FRAME_START).
 */

#include <linux/aperture.h>
#include <linux/backlight.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

/* DECON0 registers */
#define DECON_GLOBAL_CONTROL		0x0000
#define DECON_GLOBAL_RUN		(BIT(8) | BIT(4) | BIT(1) | BIT(0))
#define DECON_GC_IDLE_STATUS		BIT(5)
#define DECON_INTERRUPT_PENDING		0x004c
#define DECON_INT_FRAME_DONE		BIT(13)
#define DECON_INT_FRAME_START		BIT(12)
#define DECON_SHADOW_REG_UPDATE_REQ	0x0060
#define DECON_SHADOW_GLOBAL		BIT(31)
#define DECON_HW_SW_TRIG_CONTROL	0x0070
#define DECON_HW_TRIG_EN		BIT(0)
#define DECON_HW_TRIG_MASK_DECON	BIT(4)
#define DECON_BLENDER_BG_IMAGE_SIZE_0	0x0200
#define DECON_DATA_PATH_CONTROL_0	0x0214
#define DECON_DATA_PATH_CONTROL_1	0x0218
#define DECON_NR_WINDOWS		6

/* GF0 IDMA registers */
#define IDMA_IN_CON			0x0008
#define IDMA_IMG_FORMAT			GENMASK(16, 11)
#define IDMA_FMT_ARGB8888		0
#define IDMA_FMT_ABGR8888		1
#define IDMA_FMT_XRGB8888		4
#define IDMA_FMT_XBGR8888		5
#define IDMA_SRC_SIZE			0x0010
#define IDMA_IMG_SIZE			0x0018
#define IDMA_IN_BASE_ADDR_Y		0x0040

/* DSIM0 registers */
#define DSIM_SWRST			0x0004
#define DSIM_SWRST_RESET		BIT(0)
#define DSIM_CLK_CTRL			0x0020
#define DSIM_CLK_CTRL_CLOCK_SEL		BIT(26)
#define DSIM_CLK_CTRL_TX_REQUEST_HSCLK	BIT(20)
#define DSIM_CLK_CTRL_WORDCLK_EN	BIT(17)
#define DSIM_CONFIG			0x004c
#define DSIM_INTSRC			0x0050
#define DSIM_PKTHDR			0x0058
#define DSIM_PAYLOAD			0x005c
#define DSIM_RXFIFO			0x0060
#define DSIM_FIFOCTRL			0x0068
#define DSIM_FULL_PH_SFR		BIT(11)
#define DSIM_EMPTY_PH_SFR		BIT(10)
#define DSIM_FULL_PL_SFR		BIT(9)

/* PMU & SYSREG_DPU MIPI D-PHY control */
#define PMU_MIPI_PHY_M4S4_TOP_CTRL	0x070c
#define PMU_MIPI_PHY_M4S0_CTRL		0x0710
#define DISP_DPU_MIPI_PHY_CON		0x0008
#define DISP_SEL_RESET_DPHY_M4S0	BIT(5)
#define DISP_M_RESETN_M4S4_MODULE	BIT(1)

/* D-PHY M4S0 registers (0x19161000) */
#define DSIM_PHY_PLL_CON0		0x0100
#define DSIM_PHY_PLL_EN			BIT(12)
#define DSIM_PHY_PLL_STAT0		0x0140
#define DSIM_PHY_PLL_LOCK		BIT(0)
#define DSIM_PHY_MC_GNR_CON0		0x0300
#define DSIM_PHY_MD_GNR_CON0(n)		(0x0400 + 0x100 * (n))
#define DSIM_PHY_PHY_READY		BIT(1)
#define DSIM_PHY_PHY_ENABLE		BIT(0)

struct decon_reg_val {
	u32 off;
	u32 val;
};

#define DECON_MAX_SAVED_REGS		512
#define DPHY_TOP_SAVED_WORDS		8
#define DPHY_MAX_SAVED_REGS		96
#define DSIM_MAX_SAVED_REGS		96
#define IDMA_MAX_SAVED_REGS		128

/* S6E3HA9 (beyond2lte) AOR brightness range from Samsung panel table */
#define S6E3HA9_AOR_DIMMEST		0x0bc1
#define S6E3HA9_AOR_BRIGHTEST		0x0014
#define S6E3HA9_MAX_BRIGHTNESS		255

struct decon_kms {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;

	void __iomem *decon;
	void __iomem *idma;
	void __iomem *dsim;
	void __iomem *disp_ss;
	void __iomem *dphy_top;
	void __iomem *dphy;
	void __iomem *idma_com;
	void __iomem *dpp_gf0;
	struct regmap *pmu;
	unsigned int win;
	int irq;

	struct mutex dsi_lock;
	struct backlight_device *bl;
	bool display_on;

	u32 width;
	u32 height;
	u32 width_mm;
	u32 height_mm;

	spinlock_t trig_lock;
	unsigned int trig_frames;
	bool trig_vblank;
	bool trig_masked;
	bool sleeping;
	bool registered;

	void *boot_fb;
	u32 boot_base;
	u32 boot_in_con;
	u32 boot_src_size;
	u32 cur_base;
	u32 cur_in_con;
	u32 cur_src_size;

	/* S-Boot register snapshots for SYS_SLEEP (BLK_DPU power-cycle) restore */
	bool has_snapshot;
	u32 saved_decon_gctl;
	u32 saved_decon_trig;
	u32 saved_dsim_clk;
	u32 saved_dsim_cfg;
	u32 saved_dphy_top[DPHY_TOP_SAVED_WORDS];
	struct decon_reg_val saved_dphy[DPHY_MAX_SAVED_REGS];
	unsigned int nr_saved_dphy;
	struct decon_reg_val saved_dsim[DSIM_MAX_SAVED_REGS];
	unsigned int nr_saved_dsim;
	struct decon_reg_val saved_idma_com[IDMA_MAX_SAVED_REGS];
	unsigned int nr_saved_idma_com;
	struct decon_reg_val saved_dpp_gf0[IDMA_MAX_SAVED_REGS];
	unsigned int nr_saved_dpp_gf0;
	struct decon_reg_val saved_idma[IDMA_MAX_SAVED_REGS];
	unsigned int nr_saved_idma;
	struct decon_reg_val saved_decon[DECON_MAX_SAVED_REGS];
	unsigned int nr_saved_decon;
};

static bool psr = true;
module_param(psr, bool, 0644);
MODULE_PARM_DESC(psr, "send frames only when the picture changes (default: 1)");

static inline struct decon_kms *to_decon(struct drm_device *drm)
{
	return container_of(drm, struct decon_kms, drm);
}

static void rmw(void __iomem *base, u32 reg, u32 mask, u32 val)
{
	writel((readl(base + reg) & ~mask) | (val & mask), base + reg);
}

static int dsim_wait_clear(struct decon_kms *dk, u32 bits)
{
	u32 val;

	return readl_poll_timeout(dk->dsim + DSIM_FIFOCTRL, val, !(val & bits),
				  10, 100000);
}

static int dsim_dcs_write_locked(struct decon_kms *dk, const u8 *buf, size_t len)
{
	u32 val, hdr;
	size_t i;
	int ret;

	if (!dk->dsim || !len)
		return 0;

	ret = dsim_wait_clear(dk, DSIM_FULL_PH_SFR);
	if (ret)
		return ret;

	if (len == 1) {
		hdr = 0x05 | ((u32)buf[0] << 8);
	} else if (len == 2) {
		hdr = 0x15 | ((u32)buf[0] << 8) | ((u32)buf[1] << 16);
	} else {
		for (i = 0; i < len; i += 4) {
			size_t n = min_t(size_t, 4, len - i);

			val = 0;
			memcpy(&val, buf + i, n);
			ret = dsim_wait_clear(dk, DSIM_FULL_PL_SFR);
			if (ret)
				return ret;
			writel(le32_to_cpu((__force __le32)val),
			       dk->dsim + DSIM_PAYLOAD);
		}
		hdr = 0x39 | ((u32)(len & 0xffff) << 8);
	}

	writel(hdr, dk->dsim + DSIM_PKTHDR);
	return readl_poll_timeout(dk->dsim + DSIM_FIFOCTRL, val,
				  val & DSIM_EMPTY_PH_SFR, 10, 100000);
}

static void s6e3ha9_set_display_locked(struct decon_kms *dk, bool on)
{
	static const u8 key1_on[] = { 0x9f, 0xa5, 0xa5 };
	static const u8 key1_off[] = { 0x9f, 0x5a, 0x5a };
	u8 cmd = on ? 0x29 : 0x28;

	if (!dk->dsim || dk->display_on == on)
		return;

	dsim_dcs_write_locked(dk, key1_on, sizeof(key1_on));
	dsim_dcs_write_locked(dk, &cmd, 1);
	dsim_dcs_write_locked(dk, key1_off, sizeof(key1_off));
	dk->display_on = on;
}

static void s6e3ha9_set_aor_locked(struct decon_kms *dk, int level)
{
	static const u8 key2_on[] = { 0xf0, 0x5a, 0x5a };
	static const u8 key2_off[] = { 0xf0, 0xa5, 0xa5 };
	static const u8 gamma_upd[] = { 0xf7, 0x03 };
	u32 aor;
	u8 b1[3];

	if (!dk->dsim)
		return;

	level = clamp(level, 1, S6E3HA9_MAX_BRIGHTNESS);
	/* Quadratic curve so low slider values reach dark-room dimness */
	aor = S6E3HA9_AOR_DIMMEST -
	      (u32)((u64)(S6E3HA9_AOR_DIMMEST - S6E3HA9_AOR_BRIGHTEST) *
		    level * level / (S6E3HA9_MAX_BRIGHTNESS * S6E3HA9_MAX_BRIGHTNESS));
	b1[0] = 0xb1;
	b1[1] = aor >> 8;
	b1[2] = aor & 0xff;

	dsim_dcs_write_locked(dk, key2_on, sizeof(key2_on));
	dsim_dcs_write_locked(dk, b1, sizeof(b1));
	dsim_dcs_write_locked(dk, gamma_upd, sizeof(gamma_upd));
	dsim_dcs_write_locked(dk, key2_off, sizeof(key2_off));
}

static int decon_bl_update_status(struct backlight_device *bl)
{
	struct decon_kms *dk = bl_get_data(bl);
	int level = backlight_get_brightness(bl);

	mutex_lock(&dk->dsi_lock);
	if (level == 0) {
		s6e3ha9_set_display_locked(dk, false);
	} else {
		s6e3ha9_set_display_locked(dk, true);
		s6e3ha9_set_aor_locked(dk, level);
	}
	mutex_unlock(&dk->dsi_lock);
	return 0;
}

static const struct backlight_ops decon_bl_ops = {
	.update_status = decon_bl_update_status,
};

static void decon_trig_kick(struct decon_kms *dk, unsigned int frames)
{
	unsigned long flags;

	spin_lock_irqsave(&dk->trig_lock, flags);
	dk->trig_frames = max(dk->trig_frames, frames);
	if (dk->trig_masked && !dk->sleeping) {
		rmw(dk->decon, DECON_HW_SW_TRIG_CONTROL,
		    DECON_HW_TRIG_MASK_DECON, 0);
		dk->trig_masked = false;
	}
	spin_unlock_irqrestore(&dk->trig_lock, flags);
}

static void decon_trig_idle(struct decon_kms *dk)
{
	spin_lock(&dk->trig_lock);
	if (dk->trig_frames)
		dk->trig_frames--;
	if (READ_ONCE(psr) && !dk->trig_frames && !dk->trig_vblank &&
	    !dk->trig_masked &&
	    !readl(dk->decon + DECON_SHADOW_REG_UPDATE_REQ)) {
		rmw(dk->decon, DECON_HW_SW_TRIG_CONTROL,
		    DECON_HW_TRIG_MASK_DECON, DECON_HW_TRIG_MASK_DECON);
		dk->trig_masked = true;
	}
	spin_unlock(&dk->trig_lock);
}

static void decon_scanout(struct decon_kms *dk, u32 base, u32 in_con,
			  u32 src_size)
{
	u32 val;

	if (readl_poll_timeout(dk->decon + DECON_SHADOW_REG_UPDATE_REQ, val,
			       !val, 100, 50000))
		drm_warn_once(&dk->drm, "shadow update stuck: 0x%08x\n", val);

	dk->cur_base = base;
	dk->cur_in_con = in_con;
	dk->cur_src_size = src_size;

	writel(in_con, dk->idma + IDMA_IN_CON);
	writel(src_size, dk->idma + IDMA_SRC_SIZE);
	writel(base, dk->idma + IDMA_IN_BASE_ADDR_Y);
	writel(DECON_SHADOW_GLOBAL | BIT(dk->win),
	       dk->decon + DECON_SHADOW_REG_UPDATE_REQ);
}

static int decon_pipe_check(struct drm_simple_display_pipe *pipe,
			    struct drm_plane_state *plane_state,
			    struct drm_crtc_state *crtc_state)
{
	struct decon_kms *dk = to_decon(pipe->crtc.dev);
	struct drm_framebuffer *fb = plane_state->fb;
	int ret;

	ret = drm_atomic_helper_check_plane_state(plane_state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  false, false);
	if (ret || !fb || !plane_state->visible)
		return ret;

	if (plane_state->src.x1 || plane_state->src.y1 ||
	    fb->width < dk->width || fb->height < dk->height ||
	    fb->pitches[0] % 4 || fb->pitches[0] / 4 > 0xffff)
		return -EINVAL;

	return 0;
}

static u32 decon_idma_format(u32 fourcc)
{
	/*
	 * Android minigbm (gralloc.universal9820.so) labels RGBA_8888/RGBX_8888
	 * buffers as DRM_FORMAT_ARGB8888/XRGB8888; swap R<->B in GF0 IDMA
	 * hardware at zero CPU cost.
	 */
	switch (fourcc) {
	case DRM_FORMAT_ARGB8888:
		return IDMA_FMT_ABGR8888;
	case DRM_FORMAT_ABGR8888:
		return IDMA_FMT_ARGB8888;
	case DRM_FORMAT_XBGR8888:
		return IDMA_FMT_XRGB8888;
	case DRM_FORMAT_XRGB8888:
	default:
		return IDMA_FMT_XBGR8888;
	}
}

static void decon_pipe_update(struct drm_simple_display_pipe *pipe,
			      struct drm_plane_state *old_state)
{
	struct decon_kms *dk = to_decon(pipe->crtc.dev);
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_pending_vblank_event *event;
	struct drm_framebuffer *fb = state->fb;
	unsigned long flags;

	if (fb && state->visible) {
		dma_addr_t addr = drm_fb_dma_get_gem_addr(fb, state, 0);
		u32 fmt = decon_idma_format(fb->format->format);

		decon_scanout(dk, lower_32_bits(addr),
			      (dk->boot_in_con & ~IDMA_IMG_FORMAT) |
			      FIELD_PREP(IDMA_IMG_FORMAT, fmt),
			      (dk->height << 16) | (fb->pitches[0] / 4));
	}
	decon_trig_kick(dk, 2);

	event = crtc->state->event;
	if (!event)
		return;
	crtc->state->event = NULL;

	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (drm_crtc_vblank_get(crtc) == 0)
		drm_crtc_arm_vblank_event(crtc, event);
	else
		drm_crtc_send_vblank_event(crtc, event);
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static void decon_pipe_enable(struct drm_simple_display_pipe *pipe,
			      struct drm_crtc_state *crtc_state,
			      struct drm_plane_state *plane_state)
{
	struct decon_kms *dk = to_decon(pipe->crtc.dev);
	const struct drm_vblank_crtc_config config = {
		.offdelay_ms = 5000,
		.disable_immediate = true,
	};

	drm_crtc_vblank_on_config(&pipe->crtc, &config);
	if (dk->dsim) {
		mutex_lock(&dk->dsi_lock);
		s6e3ha9_set_display_locked(dk, true);
		if (dk->bl)
			s6e3ha9_set_aor_locked(dk, backlight_get_brightness(dk->bl));
		mutex_unlock(&dk->dsi_lock);
	}
}

static void decon_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct decon_kms *dk = to_decon(pipe->crtc.dev);

	drm_crtc_vblank_off(&pipe->crtc);
	if (dk->dsim) {
		mutex_lock(&dk->dsi_lock);
		s6e3ha9_set_display_locked(dk, false);
		mutex_unlock(&dk->dsi_lock);
	}
	memset(dk->boot_fb, 0, (size_t)dk->width * dk->height * 4);
	wmb();
	decon_scanout(dk, dk->boot_base, dk->boot_in_con, dk->boot_src_size);
	decon_trig_kick(dk, 2);
}

static int decon_pipe_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct decon_kms *dk = to_decon(pipe->crtc.dev);
	unsigned long flags;

	spin_lock_irqsave(&dk->trig_lock, flags);
	dk->trig_vblank = true;
	spin_unlock_irqrestore(&dk->trig_lock, flags);
	decon_trig_kick(dk, 1);
	return 0;
}

static void decon_pipe_disable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct decon_kms *dk = to_decon(pipe->crtc.dev);
	unsigned long flags;

	spin_lock_irqsave(&dk->trig_lock, flags);
	dk->trig_vblank = false;
	spin_unlock_irqrestore(&dk->trig_lock, flags);
}

static const struct drm_simple_display_pipe_funcs decon_pipe_funcs = {
	.check = decon_pipe_check,
	.update = decon_pipe_update,
	.enable = decon_pipe_enable,
	.disable = decon_pipe_disable,
	.enable_vblank = decon_pipe_enable_vblank,
	.disable_vblank = decon_pipe_disable_vblank,
};

static const u32 decon_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_ABGR8888,
};

static int decon_connector_get_modes(struct drm_connector *connector)
{
	struct decon_kms *dk = to_decon(connector->dev);
	struct drm_display_mode *mode;

	mode = drm_mode_create(connector->dev);
	if (!mode)
		return 0;

	mode->hdisplay = dk->width;
	mode->hsync_start = dk->width + 16;
	mode->hsync_end = dk->width + 32;
	mode->htotal = dk->width + 48;
	mode->vdisplay = dk->height;
	mode->vsync_start = dk->height + 8;
	mode->vsync_end = dk->height + 16;
	mode->vtotal = dk->height + 24;
	mode->clock = DIV_ROUND_CLOSEST((u64)mode->htotal * mode->vtotal * 60, 1000);
	mode->width_mm = dk->width_mm;
	mode->height_mm = dk->height_mm;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);

	connector->display_info.width_mm = dk->width_mm;
	connector->display_info.height_mm = dk->height_mm;
	drm_mode_probed_add(connector, mode);
	return 1;
}

static const struct drm_connector_helper_funcs decon_connector_helper_funcs = {
	.get_modes = decon_connector_get_modes,
};

static const struct drm_connector_funcs decon_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static irqreturn_t decon_irq(int irq, void *data)
{
	struct decon_kms *dk = data;
	u32 pend = readl(dk->decon + DECON_INTERRUPT_PENDING);

	if (!(pend & DECON_INT_FRAME_START))
		return IRQ_NONE;
	writel(pend, dk->decon + DECON_INTERRUPT_PENDING);

	drm_crtc_handle_vblank(&dk->pipe.crtc);
	decon_trig_idle(dk);
	return IRQ_HANDLED;
}

static const struct drm_mode_config_funcs decon_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

DEFINE_DRM_GEM_DMA_FOPS(decon_fops);

static const struct drm_driver decon_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	/* "simpledrm" matches our Android minigbm gralloc.universal9820.so backend */
	.name = "simpledrm",
	.desc = "Exynos 9820 DECON handover",
	.major = 1,
	.minor = 0,
	.fops = &decon_fops,
	DRM_GEM_DMA_DRIVER_OPS,
	DRM_FBDEV_DMA_DRIVER_OPS,
};

static int decon_check_handover(struct decon_kms *dk)
{
	struct device *dev = dk->drm.dev;
	u32 ctrl, trig, path0, path1, size;
	unsigned int w, nwin = 0;

	ctrl = readl(dk->decon + DECON_GLOBAL_CONTROL);
	trig = readl(dk->decon + DECON_HW_SW_TRIG_CONTROL);
	path0 = readl(dk->decon + DECON_DATA_PATH_CONTROL_0);
	path1 = readl(dk->decon + DECON_DATA_PATH_CONTROL_1);

	if ((ctrl & DECON_GLOBAL_RUN) != DECON_GLOBAL_RUN ||
	    (trig & (DECON_HW_TRIG_EN | DECON_HW_TRIG_MASK_DECON)) !=
	    DECON_HW_TRIG_EN) {
		dev_err(dev, "DECON not running continuously (0x%08x 0x%08x)\n",
			ctrl, trig);
		return -ENODEV;
	}

	for (w = 0; w < DECON_NR_WINDOWS; w++) {
		if (path0 & BIT(4 * w)) {
			dk->win = w;
			nwin++;
		}
	}
	if (nwin != 1 || ((path1 >> (4 * dk->win)) & 7) != 0) {
		dev_err(dev, "expected one window on GF0 (0x%08x 0x%08x)\n",
			path0, path1);
		return -ENODEV;
	}

	size = readl(dk->decon + DECON_BLENDER_BG_IMAGE_SIZE_0);
	dk->width = size & 0xffff;
	dk->height = size >> 16;
	dk->boot_in_con = readl(dk->idma + IDMA_IN_CON);
	dk->boot_src_size = readl(dk->idma + IDMA_SRC_SIZE);
	dk->boot_base = readl(dk->idma + IDMA_IN_BASE_ADDR_Y);

	if (dk->width < 640 || dk->height < 480 ||
	    readl(dk->idma + IDMA_IMG_SIZE) != size) {
		dev_err(dev, "unexpected boot scanout (size 0x%08x img 0x%08x IN_CON 0x%08x)\n",
			size, readl(dk->idma + IDMA_IMG_SIZE), dk->boot_in_con);
		return -ENODEV;
	}

	dev_info(dev, "taking over %ux%u window %u, GF0 at 0x%08x (IN_CON 0x%08x)\n",
		 dk->width, dk->height, dk->win, dk->boot_base, dk->boot_in_con);
	return 0;
}

static bool decon_frame_running(struct decon_kms *dk)
{
	return !(readl(dk->decon + DECON_GLOBAL_CONTROL) & DECON_GC_IDLE_STATUS) ||
	       (readl(dk->decon + DECON_INTERRUPT_PENDING) & DECON_INT_FRAME_START);
}

static void decon_save_range(void __iomem *base, u32 start, u32 end,
			     struct decon_reg_val *dst, unsigned int *cnt,
			     unsigned int max_cnt,
			     bool (*skip_fn)(u32 off))
{
	u32 off, val;

	for (off = start; off <= end; off += 4) {
		if (skip_fn && skip_fn(off))
			continue;
		val = readl(base + off);
		if (!val)
			continue;
		if (*cnt < max_cnt) {
			dst[*cnt].off = off;
			dst[*cnt].val = val;
			(*cnt)++;
		}
	}
}

static bool skip_dsim_reg(u32 off)
{
	switch (off) {
	case DSIM_SWRST:
	case DSIM_CLK_CTRL:
	case DSIM_CONFIG:
	case DSIM_INTSRC:
	case DSIM_PKTHDR:
	case DSIM_PAYLOAD:
	case DSIM_RXFIFO:
	case DSIM_FIFOCTRL:
		return true;
	default:
		return false;
	}
}

static bool skip_dphy_reg(u32 off)
{
	return off == DSIM_PHY_PLL_STAT0;
}

static bool skip_idma_reg(u32 off)
{
	/* Skip SW_RESET / IRQ status at 0x0000..0x0004 */
	return off <= 0x0004;
}

static bool skip_decon_reg(u32 off)
{
	if (off == DECON_GLOBAL_CONTROL ||
	    off == DECON_INTERRUPT_PENDING ||
	    off == 0x0050 || /* EXTRA_INTERRUPT_PENDING */
	    off == DECON_SHADOW_REG_UPDATE_REQ ||
	    off == DECON_HW_SW_TRIG_CONTROL ||
	    (off >= 0x0280 && off <= 0x02a4)) /* CRC / FRAME_COUNT */
		return true;
	return false;
}

static void decon_snapshot_boot_state(struct decon_kms *dk)
{
	struct device *dev = dk->drm.dev;
	unsigned int i;

	if (!dk->pmu || !dk->disp_ss || !dk->dphy_top || !dk->dphy ||
	    !dk->dsim || !dk->idma_com || !dk->dpp_gf0) {
		dev_info(dev, "S2R snapshot skipped (missing optional DPU blocks)\n");
		return;
	}

	dk->saved_decon_gctl = readl(dk->decon + DECON_GLOBAL_CONTROL);
	dk->saved_decon_trig = readl(dk->decon + DECON_HW_SW_TRIG_CONTROL);
	dk->saved_dsim_clk = readl(dk->dsim + DSIM_CLK_CTRL);
	dk->saved_dsim_cfg = readl(dk->dsim + DSIM_CONFIG);

	for (i = 0; i < DPHY_TOP_SAVED_WORDS; i++)
		dk->saved_dphy_top[i] = readl(dk->dphy_top + i * 4);

	decon_save_range(dk->dphy, 0x0100, 0x0120, dk->saved_dphy, &dk->nr_saved_dphy,
			 DPHY_MAX_SAVED_REGS, skip_dphy_reg);
	decon_save_range(dk->dphy, 0x0300, 0x0354, dk->saved_dphy, &dk->nr_saved_dphy,
			 DPHY_MAX_SAVED_REGS, skip_dphy_reg);
	for (i = 0; i < 4; i++)
		decon_save_range(dk->dphy, 0x0400 + i * 0x100, 0x0444 + i * 0x100,
				 dk->saved_dphy, &dk->nr_saved_dphy,
				 DPHY_MAX_SAVED_REGS, skip_dphy_reg);

	decon_save_range(dk->dsim, 0x0000, 0x02fc, dk->saved_dsim, &dk->nr_saved_dsim,
			 DSIM_MAX_SAVED_REGS, skip_dsim_reg);
	decon_save_range(dk->idma_com, 0x0000, 0x010c, dk->saved_idma_com,
			 &dk->nr_saved_idma_com, IDMA_MAX_SAVED_REGS, NULL);
	decon_save_range(dk->dpp_gf0, 0x0000, 0x00fc, dk->saved_dpp_gf0,
			 &dk->nr_saved_dpp_gf0, IDMA_MAX_SAVED_REGS, skip_idma_reg);
	decon_save_range(dk->idma, 0x0000, 0x00fc, dk->saved_idma,
			 &dk->nr_saved_idma, IDMA_MAX_SAVED_REGS, skip_idma_reg);

	decon_save_range(dk->decon, 0x0000, 0x027c, dk->saved_decon, &dk->nr_saved_decon,
			 DECON_MAX_SAVED_REGS, skip_decon_reg);
	decon_save_range(dk->decon, 0x1000, 0x15fc, dk->saved_decon, &dk->nr_saved_decon,
			 DECON_MAX_SAVED_REGS, skip_decon_reg);
	decon_save_range(dk->decon, 0x2000, 0x24fc, dk->saved_decon, &dk->nr_saved_decon,
			 DECON_MAX_SAVED_REGS, skip_decon_reg);
	decon_save_range(dk->decon, 0x4000, 0x41fc, dk->saved_decon, &dk->nr_saved_decon,
			 DECON_MAX_SAVED_REGS, skip_decon_reg);
	decon_save_range(dk->decon, 0x5000, 0x51fc, dk->saved_decon, &dk->nr_saved_decon,
			 DECON_MAX_SAVED_REGS, skip_decon_reg);

	dk->has_snapshot = true;
	dev_info(dev, "S2R snapshot saved: dphy=%u dsim=%u idma_com=%u dpp=%u idma=%u decon=%u\n",
		 dk->nr_saved_dphy, dk->nr_saved_dsim, dk->nr_saved_idma_com,
		 dk->nr_saved_dpp_gf0, dk->nr_saved_idma, dk->nr_saved_decon);
}

static void decon_restore_cold(struct decon_kms *dk)
{
	struct device *dev = dk->drm.dev;
	unsigned int i;
	u32 val;

	if (!dk->has_snapshot)
		return;

	dev_info(dev, "BLK_DPU lost state across SYS_SLEEP, replaying S-Boot display pipeline\n");

	/* 1. Release PMU MIPI D-PHY isolation */
	regmap_update_bits(dk->pmu, PMU_MIPI_PHY_M4S0_CTRL, BIT(0), BIT(0));
	regmap_update_bits(dk->pmu, PMU_MIPI_PHY_M4S4_TOP_CTRL, BIT(0), BIT(0));

	/* 2. SYSREG_DPU controls D-PHY reset */
	rmw(dk->disp_ss, DISP_DPU_MIPI_PHY_CON, DISP_SEL_RESET_DPHY_M4S0, 0);

	/* 3. DSIM0 OSC clock + SW reset */
	rmw(dk->dsim, DSIM_CLK_CTRL, DSIM_CLK_CTRL_CLOCK_SEL, 0);
	writel(DSIM_SWRST_RESET, dk->dsim + DSIM_SWRST);
	if (readl_poll_timeout(dk->dsim + DSIM_SWRST, val, !(val & DSIM_SWRST_RESET),
			       10, 10000))
		dev_warn(dev, "DSIM0 SW reset timeout (0x%08x)\n", val);

	writel(dk->saved_dsim_cfg, dk->dsim + DSIM_CONFIG);
	writel(dk->saved_dsim_clk & ~(DSIM_CLK_CTRL_CLOCK_SEL | DSIM_CLK_CTRL_TX_REQUEST_HSCLK),
	       dk->dsim + DSIM_CLK_CTRL);

	/* 4. Assert D-PHY module reset */
	rmw(dk->disp_ss, DISP_DPU_MIPI_PHY_CON, DISP_M_RESETN_M4S4_MODULE, 0);

	/* 5. Restore D-PHY BIAS and M4S0 (with PLL_EN & PHY_ENABLE masked off first) */
	for (i = 0; i < DPHY_TOP_SAVED_WORDS; i++)
		writel(dk->saved_dphy_top[i], dk->dphy_top + i * 4);

	for (i = 0; i < dk->nr_saved_dphy; i++) {
		u32 off = dk->saved_dphy[i].off;
		u32 v = dk->saved_dphy[i].val;

		if (off == DSIM_PHY_PLL_CON0)
			v &= ~DSIM_PHY_PLL_EN;
		else if (off == DSIM_PHY_MC_GNR_CON0 ||
			 off == DSIM_PHY_MD_GNR_CON0(0) ||
			 off == DSIM_PHY_MD_GNR_CON0(1) ||
			 off == DSIM_PHY_MD_GNR_CON0(2) ||
			 off == DSIM_PHY_MD_GNR_CON0(3))
			v &= ~(DSIM_PHY_PHY_ENABLE | DSIM_PHY_PHY_READY);
		writel(v, dk->dphy + off);
	}

	/* Enable D-PHY PLL and wait for lock */
	rmw(dk->dphy, DSIM_PHY_PLL_CON0, DSIM_PHY_PLL_EN, DSIM_PHY_PLL_EN);
	if (readl_poll_timeout(dk->dphy + DSIM_PHY_PLL_STAT0, val,
			       val & DSIM_PHY_PLL_LOCK, 10, 10000))
		dev_warn(dev, "D-PHY PLL lock timeout (0x%08x)\n", val);

	/* Enable clock + 4 data lanes and wait for PHY_READY */
	rmw(dk->dphy, DSIM_PHY_MC_GNR_CON0, DSIM_PHY_PHY_ENABLE, DSIM_PHY_PHY_ENABLE);
	if (readl_poll_timeout(dk->dphy + DSIM_PHY_MC_GNR_CON0, val,
			       val & DSIM_PHY_PHY_READY, 10, 10000))
		dev_warn(dev, "D-PHY MC ready timeout (0x%08x)\n", val);
	for (i = 0; i < 4; i++) {
		u32 reg = DSIM_PHY_MD_GNR_CON0(i);

		rmw(dk->dphy, reg, DSIM_PHY_PHY_ENABLE, DSIM_PHY_PHY_ENABLE);
		if (readl_poll_timeout(dk->dphy + reg, val,
				       val & DSIM_PHY_PHY_READY, 10, 10000))
			dev_warn(dev, "D-PHY MD%u ready timeout (0x%08x)\n", i, val);
	}

	/* 6. Release D-PHY module reset */
	rmw(dk->disp_ss, DISP_DPU_MIPI_PHY_CON, DISP_M_RESETN_M4S4_MODULE,
	    DISP_M_RESETN_M4S4_MODULE);

	/* 7. Switch DSIM0 to word clock, replay remaining DSIM0 regs, enable HSCLK */
	rmw(dk->dsim, DSIM_CLK_CTRL, DSIM_CLK_CTRL_CLOCK_SEL, DSIM_CLK_CTRL_CLOCK_SEL);
	for (i = 0; i < dk->nr_saved_dsim; i++)
		writel(dk->saved_dsim[i].val, dk->dsim + dk->saved_dsim[i].off);
	writel(0xffffffff, dk->dsim + DSIM_INTSRC);
	writel(dk->saved_dsim_clk, dk->dsim + DSIM_CLK_CTRL);

	/* 8. Restore IDMA_COMMON, DPP_GF0, IDMA_GF0 and active scanout buffer */
	for (i = 0; i < dk->nr_saved_idma_com; i++)
		writel(dk->saved_idma_com[i].val, dk->idma_com + dk->saved_idma_com[i].off);
	for (i = 0; i < dk->nr_saved_dpp_gf0; i++)
		writel(dk->saved_dpp_gf0[i].val, dk->dpp_gf0 + dk->saved_dpp_gf0[i].off);
	for (i = 0; i < dk->nr_saved_idma; i++)
		writel(dk->saved_idma[i].val, dk->idma + dk->saved_idma[i].off);
	writel(dk->cur_in_con, dk->idma + IDMA_IN_CON);
	writel(dk->cur_src_size, dk->idma + IDMA_SRC_SIZE);
	writel(dk->cur_base, dk->idma + IDMA_IN_BASE_ADDR_Y);

	/* 9. Restore DECON0, clear stale IRQs, start DECON0 and latch shadow regs */
	for (i = 0; i < dk->nr_saved_decon; i++)
		writel(dk->saved_decon[i].val, dk->decon + dk->saved_decon[i].off);
	writel(0xffffffff, dk->decon + DECON_INTERRUPT_PENDING);
	writel(dk->saved_decon_gctl & ~BIT(28), dk->decon + DECON_GLOBAL_CONTROL);
	writel(DECON_SHADOW_GLOBAL | BIT(dk->win),
	       dk->decon + DECON_SHADOW_REG_UPDATE_REQ);
	if (readl_poll_timeout(dk->decon + DECON_GLOBAL_CONTROL, val,
			       val & BIT(4), 10, 20000))
		dev_warn(dev, "DECON0 run timeout (0x%08x)\n", val);

	/* Keep TE trigger masked until decon_pm_resume() unmasks it */
	writel(dk->saved_decon_trig | DECON_HW_TRIG_MASK_DECON,
	       dk->decon + DECON_HW_SW_TRIG_CONTROL);
	dk->trig_masked = true;
}

static int decon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct decon_kms *dk;
	struct drm_device *drm;
	void __iomem *decon, *idma;
	u32 boot_base, size;
	int ret;

	decon = devm_platform_ioremap_resource_byname(pdev, "decon");
	if (IS_ERR(decon))
		return PTR_ERR(decon);
	idma = devm_platform_ioremap_resource_byname(pdev, "idma-gf0");
	if (IS_ERR(idma))
		return PTR_ERR(idma);

	/* Evict simpledrm BEFORE allocating our DRM minor so DECON gets card0. */
	size = readl(decon + DECON_BLENDER_BG_IMAGE_SIZE_0);
	boot_base = readl(idma + IDMA_IN_BASE_ADDR_Y);
	if ((size & 0xffff) >= 640 && (size >> 16) >= 480) {
		ret = aperture_remove_conflicting_devices(boot_base,
							  (size_t)(size & 0xffff) * (size >> 16) * 4,
							  decon_driver.name);
		if (ret)
			return ret;
	}

	dk = devm_drm_dev_alloc(dev, &decon_driver, struct decon_kms, drm);
	if (IS_ERR(dk))
		return PTR_ERR(dk);
	platform_set_drvdata(pdev, dk);
	spin_lock_init(&dk->trig_lock);
	mutex_init(&dk->dsi_lock);
	drm = &dk->drm;
	dk->decon = decon;
	dk->idma = idma;
	dk->dsim = devm_platform_ioremap_resource_byname(pdev, "dsim");
	if (IS_ERR(dk->dsim))
		dk->dsim = NULL;
	dk->disp_ss = devm_platform_ioremap_resource_byname(pdev, "disp-ss");
	if (IS_ERR(dk->disp_ss))
		dk->disp_ss = NULL;
	dk->dphy_top = devm_platform_ioremap_resource_byname(pdev, "dphy-top");
	if (IS_ERR(dk->dphy_top))
		dk->dphy_top = NULL;
	dk->dphy = devm_platform_ioremap_resource_byname(pdev, "dphy");
	if (IS_ERR(dk->dphy))
		dk->dphy = NULL;
	dk->idma_com = devm_platform_ioremap_resource_byname(pdev, "idma-com");
	if (IS_ERR(dk->idma_com))
		dk->idma_com = NULL;
	dk->dpp_gf0 = devm_platform_ioremap_resource_byname(pdev, "dpp-gf0");
	if (IS_ERR(dk->dpp_gf0))
		dk->dpp_gf0 = NULL;
	dk->pmu = syscon_regmap_lookup_by_phandle(dev->of_node, "samsung,pmu-syscon");
	if (IS_ERR(dk->pmu))
		dk->pmu = NULL;
	dk->display_on = true;

	ret = decon_check_handover(dk);
	if (ret)
		return ret;
	dk->cur_base = dk->boot_base;
	dk->cur_in_con = dk->boot_in_con;
	dk->cur_src_size = dk->boot_src_size;
	decon_snapshot_boot_state(dk);

	dk->width_mm = 70;
	dk->height_mm = 147;
	of_property_read_u32(dev->of_node, "width-mm", &dk->width_mm);
	of_property_read_u32(dev->of_node, "height-mm", &dk->height_mm);

	dk->boot_fb = devm_memremap(dev, dk->boot_base,
				    (size_t)dk->width * dk->height * 4,
				    MEMREMAP_WC);
	if (IS_ERR(dk->boot_fb))
		return PTR_ERR(dk->boot_fb);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	if (device_iommu_mapped(dev)) {
		ret = devm_pm_runtime_enable(dev);
		if (ret)
			return ret;
		ret = pm_runtime_resume_and_get(dev);
		if (ret)
			return ret;
	}

	dk->irq = platform_get_irq(pdev, 0);
	if (dk->irq < 0)
		return dk->irq;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.min_width = 640;
	drm->mode_config.max_width = 4096;
	drm->mode_config.min_height = 480;
	drm->mode_config.max_height = 4096;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &decon_mode_config_funcs;

	drm_connector_helper_add(&dk->connector, &decon_connector_helper_funcs);
	ret = drm_connector_init(drm, &dk->connector, &decon_connector_funcs,
				 DRM_MODE_CONNECTOR_DSI);
	if (ret)
		return ret;

	ret = drm_simple_display_pipe_init(drm, &dk->pipe, &decon_pipe_funcs,
					   decon_formats,
					   ARRAY_SIZE(decon_formats), NULL,
					   &dk->connector);
	if (ret)
		return ret;
	drm_plane_enable_fb_damage_clips(&dk->pipe.plane);

	ret = drm_vblank_init(drm, drm->mode_config.num_crtc);
	if (ret)
		return ret;

	ret = devm_request_irq(dev, dk->irq, decon_irq, 0, dev_name(dev), dk);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;
	dk->registered = true;

	if (dk->dsim) {
		struct backlight_properties props = {
			.type = BACKLIGHT_RAW,
			.max_brightness = S6E3HA9_MAX_BRIGHTNESS,
			.brightness = 128,
		};

		dk->bl = devm_backlight_device_register(dev, "panel", dev, dk,
							&decon_bl_ops, &props);
		if (IS_ERR(dk->bl)) {
			dev_warn(dev, "failed to register panel backlight: %ld\n",
				 PTR_ERR(dk->bl));
			dk->bl = NULL;
		}
	}

	drm_client_setup(drm, NULL);
	return 0;
}

static void decon_remove(struct platform_device *pdev)
{
	struct decon_kms *dk = platform_get_drvdata(pdev);

	if (dk->registered) {
		drm_dev_unplug(&dk->drm);
		drm_atomic_helper_shutdown(&dk->drm);
	}
}

static void decon_shutdown(struct platform_device *pdev)
{
	struct decon_kms *dk = platform_get_drvdata(pdev);

	if (dk->registered)
		drm_atomic_helper_shutdown(&dk->drm);
}

static int decon_pm_suspend(struct device *dev)
{
	struct decon_kms *dk = dev_get_drvdata(dev);
	unsigned long flags;
	bool running;

	spin_lock_irqsave(&dk->trig_lock, flags);
	dk->sleeping = true;
	if (!dk->trig_masked) {
		rmw(dk->decon, DECON_HW_SW_TRIG_CONTROL,
		    DECON_HW_TRIG_MASK_DECON, DECON_HW_TRIG_MASK_DECON);
		dk->trig_masked = true;
	}
	spin_unlock_irqrestore(&dk->trig_lock, flags);

	if (read_poll_timeout(decon_frame_running, running, !running, 100, 50000, false, dk))
		dev_warn(dev, "a frame did not end in 50 ms before sleep\n");
	return 0;
}

static int decon_pm_resume(struct device *dev)
{
	struct decon_kms *dk = dev_get_drvdata(dev);
	unsigned long flags;
	bool cold = false;

	if ((readl(dk->decon + DECON_GLOBAL_CONTROL) & DECON_GLOBAL_RUN) !=
	    DECON_GLOBAL_RUN) {
		decon_restore_cold(dk);
		cold = true;
	}

	spin_lock_irqsave(&dk->trig_lock, flags);
	dk->sleeping = false;
	if (dk->trig_masked &&
	    (cold || dk->trig_frames || dk->trig_vblank || !READ_ONCE(psr))) {
		rmw(dk->decon, DECON_HW_SW_TRIG_CONTROL,
		    DECON_HW_TRIG_MASK_DECON, 0);
		dk->trig_masked = false;
	}
	spin_unlock_irqrestore(&dk->trig_lock, flags);

	if (cold && dk->dsim && dk->display_on) {
		mutex_lock(&dk->dsi_lock);
		dk->display_on = false;
		s6e3ha9_set_display_locked(dk, true);
		if (dk->bl)
			s6e3ha9_set_aor_locked(dk, backlight_get_brightness(dk->bl));
		mutex_unlock(&dk->dsi_lock);
	}
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(decon_pm_ops, decon_pm_suspend, decon_pm_resume);

static const struct of_device_id decon_of_match[] = {
	{ .compatible = "samsung,exynos9820-decon-handover" },
	{ }
};
MODULE_DEVICE_TABLE(of, decon_of_match);

static struct platform_driver decon_platform_driver = {
	.driver = {
		.name = "exynos9820-decon-kms",
		.of_match_table = decon_of_match,
		.pm = pm_sleep_ptr(&decon_pm_ops),
	},
	.probe = decon_probe,
	.remove = decon_remove,
	.shutdown = decon_shutdown,
};
module_platform_driver(decon_platform_driver);

MODULE_DESCRIPTION("Exynos 9820 DECON0 handover KMS");
MODULE_LICENSE("GPL");

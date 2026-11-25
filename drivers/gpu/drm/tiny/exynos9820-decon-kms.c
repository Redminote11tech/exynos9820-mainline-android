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
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

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

struct decon_kms {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;

	void __iomem *decon;
	void __iomem *idma;
	unsigned int win;
	int irq;

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
	const struct drm_vblank_crtc_config config = {
		.offdelay_ms = 5000,
		.disable_immediate = true,
	};

	drm_crtc_vblank_on_config(&pipe->crtc, &config);
}

static void decon_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct decon_kms *dk = to_decon(pipe->crtc.dev);

	drm_crtc_vblank_off(&pipe->crtc);
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
	drm = &dk->drm;
	dk->decon = decon;
	dk->idma = idma;

	ret = decon_check_handover(dk);
	if (ret)
		return ret;

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

	spin_lock_irqsave(&dk->trig_lock, flags);
	dk->sleeping = false;
	if (dk->trig_masked &&
	    (dk->trig_frames || dk->trig_vblank || !READ_ONCE(psr))) {
		rmw(dk->decon, DECON_HW_SW_TRIG_CONTROL,
		    DECON_HW_TRIG_MASK_DECON, 0);
		dk->trig_masked = false;
	}
	spin_unlock_irqrestore(&dk->trig_lock, flags);
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

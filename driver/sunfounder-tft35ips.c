// SPDX-License-Identifier: GPL-2.0+
/*
 * DRM driver for Sunfounder TFT35IPS panels
 *
 * Copyright 2020 Kamlesh Gurudasani <kamlesh.gurudasani@gmail.com>
 * source from https://github.com/raspberrypi/linux/blob/rpi-6.12.y/drivers/gpu/drm/tiny/ili9486.c
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/spi/spi.h>

#include <video/mipi_display.h>

#include <linux/version.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_framebuffer.h>
#include <video/mipi_display.h>
#if defined(__has_include)
# if __has_include(<drm/clients/drm_client_setup.h>)
#  include <drm/clients/drm_client_setup.h>
#  define SF_TFT_FBDEV_CLIENT
# endif
# if __has_include(<drm/drm_fbdev_dma.h>)
#  include <drm/drm_fbdev_dma.h>
#  define SF_TFT_FBDEV_DMA
# elif !defined(SF_TFT_FBDEV_CLIENT)
#  include <drm/drm_fbdev_generic.h>
#  define SF_TFT_FBDEV_GENERIC
# endif
#else
# include <drm/drm_fbdev_dma.h>
# define SF_TFT_FBDEV_DMA
#endif
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_edid.h>
#include <drm/drm_mipi_dbi.h>
#include <drm/drm_modeset_helper.h>

#define ILI9486_ITFCTR1         0xb0
#define ILI9486_PWCTRL1         0xc2
#define ILI9486_VMCTRL1         0xc5
#define ILI9486_PGAMCTRL        0xe0
#define ILI9486_NGAMCTRL        0xe1
#define ILI9486_DGAMCTRL        0xe2
#define ILI9486_MADCTL_BGR      BIT(3)
#define ILI9486_MADCTL_MV       BIT(5)
#define ILI9486_MADCTL_MX       BIT(6)
#define ILI9486_MADCTL_MY       BIT(7)

#define ST7796_NOP         0x00
#define ST7796_SWRESET     0x01
#define ST7796_RDDID       0x04
#define ST7796_RDDST       0x09

#define ST7796_SLPIN       0x10
#define ST7796_SLPOUT      0x11
#define ST7796_PTLON       0x12
#define ST7796_NORON       0x13

#define ST7796_RDMODE      0x0A
#define ST7796_RDMADCTL    0x0B
#define ST7796_RDPIXFMT    0x0C
#define ST7796_RDIMGFMT    0x0A
#define ST7796_RDSELFDIAG  0x0F

#define ST7796_INVOFF      0x20
#define ST7796_INVON       0x21

#define ST7796_DISPOFF     0x28
#define ST7796_DISPON      0x29

#define ST7796_CASET       0x2A
#define ST7796_PASET       0x2B
#define ST7796_RAMWR       0x2C
#define ST7796_RAMRD       0x2E

#define ST7796_PTLAR       0x30
#define ST7796_VSCRDEF     0x33
#define ST7796_MADCTL      0x36
#define ST7796_VSCRSADD    0x37
#define ST7796_PIXFMT      0x3A

#define ST7796_WRDISBV     0x51
#define ST7796_RDDISBV     0x52
#define ST7796_WRCTRLD     0x53

#define ST7796_IFMODE      0xB0
#define ST7796_FRMCTR1     0xB1
#define ST7796_FRMCTR2     0xB2
#define ST7796_FRMCTR3     0xB3
#define ST7796_INVCTR      0xB4
#define ST7796_BPCCTR      0xB5
#define ST7796_DFUNCTR     0xB6
#define ST7796_EMSET       0xB7

#define ST7796_PWCTR1      0xC0
#define ST7796_PWCTR2      0xC1
#define ST7796_PWCTR3      0xC2

#define ST7796_VMCTR1      0xC5
#define ST7796_VMCOFF      0xC6

#define ST7796_RDID4       0xD3

#define ST7796_GMCTRP1     0xE0
#define ST7796_GMCTRN1     0xE1
#define ST7796_DOCACTL     0xE8

#define ST7796_CSCONCTL    0xF0

#define ST7796_MADCTL_MH	BIT(2)
#define ST7796_MADCTL_BGR	BIT(3)
#define ST7796_MADCTL_ML	BIT(4)
#define ST7796_MADCTL_MV	BIT(5)
#define ST7796_MADCTL_MX	BIT(6)
#define ST7796_MADCTL_MY	BIT(7)

/* EDID for the 3.5" panel: manufacturer SFT, product 0x0135. Without it the
 * connector has no identity, and compositors such as mutter cannot map the
 * touchscreen to this output (it then spans the whole desktop).
 */
static const u8 sf_tft_edid[128] = {
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
	0x4c, 0xce, 0x35, 0x01, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x24, 0x01, 0x03, 0x80, 0x07, 0x05, 0x78,
	0x0a, 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26,
	0x0f, 0x50, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xcd, 0x01,
	0xe0, 0x20, 0x10, 0x40, 0x08, 0x10, 0x0a, 0xa0,
	0x22, 0x00, 0x49, 0x00, 0x31, 0x00, 0x00, 0x1e,
	0x00, 0x00, 0x00, 0xfc, 0x00, 0x53, 0x75, 0x6e,
	0x46, 0x6f, 0x75, 0x6e, 0x64, 0x65, 0x72, 0x0a,
	0x20, 0x20, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x1e,
	0x28, 0x14, 0x28, 0x00, 0x00, 0x0a, 0x20, 0x20,
	0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0x10,
	0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x7e,
};static bool g_bgr_enabled;
static bool g_color_invert_enabled;

/*
 * The PiScreen/waveshare rpi-lcd-35 has a SPI to 16-bit parallel bus converter
 * in front of the  display controller. This means that 8-bit values have to be
 * transferred as 16-bit.
 */
static int waveshare_command(struct mipi_dbi *mipi, u8 *cmd, u8 *par,
			     size_t num)
{
	struct spi_device *spi = mipi->spi;
	unsigned int bpw = 8;
	void *data = par;
	u32 speed_hz;
	int i, ret;
	__be16 *buf;

	buf = kmalloc(32 * sizeof(u16), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/*
	 * The displays are Raspberry Pi HATs and connected to the 8-bit only
	 * SPI controller, so 16-bit command and parameters need byte swapping
	 * before being transferred as 8-bit on the big endian SPI bus.
	 */
	buf[0] = cpu_to_be16(*cmd);
	spi_bus_lock(spi->controller);
	gpiod_set_value_cansleep(mipi->dc, 0);
	speed_hz = mipi_dbi_spi_cmd_max_speed(spi, 2);
	ret = mipi_dbi_spi_transfer(spi, speed_hz, 8, buf, 2);
	spi_bus_unlock(spi->controller);
	if (ret || !num)
		goto free;

	/* 8-bit configuration data, not 16-bit pixel data */
	if (num <= 32) {
		for (i = 0; i < num; i++)
			buf[i] = cpu_to_be16(par[i]);
		num *= 2;
		data = buf;
	}

	/*
	 * Check whether pixel data bytes needs to be swapped or not
	 */
	if (*cmd == MIPI_DCS_WRITE_MEMORY_START && !mipi->swap_bytes)
		bpw = 16;

	spi_bus_lock(spi->controller);
	gpiod_set_value_cansleep(mipi->dc, 1);
	speed_hz = mipi_dbi_spi_cmd_max_speed(spi, num);
	ret = mipi_dbi_spi_transfer(spi, speed_hz, bpw, data, num);
	spi_bus_unlock(spi->controller);
 free:
	kfree(buf);

	return ret;
}

static void waveshare_enable(struct drm_simple_display_pipe *pipe,
			     struct drm_crtc_state *crtc_state,
			     struct drm_plane_state *plane_state)
{
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(pipe->crtc.dev);
	struct mipi_dbi *dbi = &dbidev->dbi;
	u8 addr_mode;
	int ret, idx;

	if (!drm_dev_enter(pipe->crtc.dev, &idx))
		return;

	ret = mipi_dbi_poweron_conditional_reset(dbidev);
	if (ret < 0)
		goto out_exit;
	if (ret == 1) {
		/*
		 * The panel is still powered (we come back from DPMS off, not
		 * from a power cycle): wake it up again, otherwise the display
		 * stays blank while the framebuffer keeps updating.
		 */
		mipi_dbi_command(dbi, ST7796_SLPOUT);
		msleep(120);
		mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);
		goto out_enable;
	}

	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_OFF);
	msleep(120);

	/* Software reset */
	// mipi_dbi_command(dbi, ST7796_SWRESET);
	// msleep(120);

	/* Sleep exit */
	mipi_dbi_command(dbi, ST7796_SLPOUT);
	msleep(255);

	/* Memory Data Access Control MX, MY, RGB mode */
	mipi_dbi_command(dbi, ST7796_MADCTL, 0x28);    //X-Mirror, Top-Left to right-Buttom, RGB  

	/* Interface Pixel Format */
	mipi_dbi_command(dbi, ST7796_PIXFMT, 0x55);

	/* Command Set control */
	mipi_dbi_command(dbi, ST7796_CSCONCTL, 0xC3);    //Enable extension command 2 partI

	/* Command Set control */
	mipi_dbi_command(dbi, ST7796_CSCONCTL, 0x96);    //Enable extension command 2 partII

	mipi_dbi_command(dbi, ST7796_INVCTR, 0x01);
	mipi_dbi_command(dbi, ST7796_EMSET, 0xC6);

	mipi_dbi_command(dbi, ST7796_PWCTR1, 0x80, 0x45);

	/* Power control2 */
	mipi_dbi_command(dbi, ST7796_PWCTR2, 0x13);

	/* Power control 3 */
	mipi_dbi_command(dbi, ST7796_PWCTR3, 0xA7);

	/* VCOM Control */
	mipi_dbi_command(dbi, ST7796_VMCTR1, 0x0a);
	msleep(120);

	/* Display Output Ctrl Adjust */
	mipi_dbi_command(dbi, ST7796_DOCACTL, 0x40, 0x8a, 0x00, 0x00, 0x29, 0x19, 0xa5, 0x33); 

	/* ST7796 Gamma Sequence */
	/* Gamma"+" */
	mipi_dbi_command(dbi, ST7796_GMCTRP1, 0xd0, 0x08, 0x0f, 0x06, 0x06, 0x33, 0x30, 0x33, 0x47, 0x17, 0x13, 0x13, 0x2b, 0x31); 
	/* Gamma"-" */
	mipi_dbi_command(dbi, ST7796_GMCTRN1, 0xd0, 0x0a, 0x11, 0x0b, 0x09, 0x07, 0x2f, 0x33, 0x47, 0x38, 0x15, 0x16, 0x2c, 0x32);
	msleep(120);

	/* Command Set control */
	mipi_dbi_command(dbi, ST7796_CSCONCTL, 0x3C);    //Disable extension command 2 partI
	mipi_dbi_command(dbi, ST7796_CSCONCTL, 0x69);    //Disable extension command 2 partII
	msleep(255);

	mipi_dbi_command(dbi, ST7796_INVON);
	mipi_dbi_command(dbi, ST7796_DISPON);
	msleep(100);

	// mipi_dbi_command(dbi, ILI9486_ITFCTR1);
	// mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);
	// msleep(250);

	// mipi_dbi_command(dbi, MIPI_DCS_SET_PIXEL_FORMAT, 0x55);

	// mipi_dbi_command(dbi, ILI9486_PWCTRL1, 0x44);

	// mipi_dbi_command(dbi, ILI9486_VMCTRL1, 0x00, 0x00, 0x00, 0x00);

	// mipi_dbi_command(dbi, ILI9486_PGAMCTRL,
	// 		 0x0F, 0x1F, 0x1C, 0x0C, 0x0F, 0x08, 0x48, 0x98,
	// 		 0x37, 0x0A, 0x13, 0x04, 0x11, 0x0D, 0x0);
	// mipi_dbi_command(dbi, ILI9486_NGAMCTRL,
	// 		 0x0F, 0x32, 0x2E, 0x0B, 0x0D, 0x05, 0x47, 0x75,
	// 		 0x37, 0x06, 0x10, 0x03, 0x24, 0x20, 0x00);
	// mipi_dbi_command(dbi, ILI9486_DGAMCTRL,
	// 		 0x0F, 0x32, 0x2E, 0x0B, 0x0D, 0x05, 0x47, 0x75,
	// 		 0x37, 0x06, 0x10, 0x03, 0x24, 0x20, 0x00);

	// mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);
	// msleep(100);
	if (g_color_invert_enabled) {
		mipi_dbi_command(dbi, MIPI_DCS_ENTER_INVERT_MODE, 0x00);
	}

 out_enable:
	switch (dbidev->rotation) {
	case 90:
		addr_mode = ILI9486_MADCTL_MY;
		break;
	case 180:
		addr_mode = ILI9486_MADCTL_MV;
		break;
	case 270:
		addr_mode = ILI9486_MADCTL_MX;
		break;
	default:
		addr_mode = ILI9486_MADCTL_MV | ILI9486_MADCTL_MY |
			ILI9486_MADCTL_MX;
		break;
	}
	if (g_bgr_enabled) {
		addr_mode |= ILI9486_MADCTL_BGR;
	} else {
		addr_mode &= ~ILI9486_MADCTL_BGR;
	}
	mipi_dbi_command(dbi, MIPI_DCS_SET_ADDRESS_MODE, addr_mode);
	mipi_dbi_enable_flush(dbidev, crtc_state, plane_state);
 out_exit:
	drm_dev_exit(idx);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
static void waveshare_set_window(struct mipi_dbi_dev *dbidev,
				 unsigned int xs, unsigned int xe,
				 unsigned int ys, unsigned int ye)
{
	struct mipi_dbi *dbi = &dbidev->dbi;

	mipi_dbi_command(dbi, MIPI_DCS_SET_COLUMN_ADDRESS,
			 (xs >> 8) & 0xff, xs & 0xff, (xe >> 8) & 0xff, xe & 0xff);
	mipi_dbi_command(dbi, MIPI_DCS_SET_PAGE_ADDRESS,
			 (ys >> 8) & 0xff, ys & 0xff, (ye >> 8) & 0xff, ye & 0xff);
}

static void waveshare_flush(struct mipi_dbi_dev *dbidev,
			    struct drm_plane_state *state, struct drm_rect *rect)
{
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(state);
	struct drm_framebuffer *fb = state->fb;
	struct mipi_dbi *dbi = &dbidev->dbi;
	const struct drm_format_info *dst_format;
	unsigned int width = drm_rect_width(rect);
	unsigned int height = drm_rect_height(rect);
	bool swap = dbi->swap_bytes;
	bool full = width == fb->width && height == fb->height;
	size_t len;
	void *tr;
	int ret;

	if (!dbi->dc || !full || swap ||
	    fb->format->format == DRM_FORMAT_XRGB8888) {
		tr = dbidev->tx_buf;
		ret = mipi_dbi_buf_copy(tr, &shadow->data[0], fb, rect, swap,
					&shadow->fmtcnv_state);
		if (ret)
			return;
	} else {
		tr = shadow->data[0].vaddr;
	}

	waveshare_set_window(dbidev, rect->x1, rect->x2 - 1, rect->y1, rect->y2 - 1);

	if (fb->format->format == DRM_FORMAT_XRGB8888)
		dst_format = drm_format_info(dbidev->pixel_format);
	else
		dst_format = fb->format;
	len = drm_format_info_min_pitch(dst_format, 0, width) * height;

	mipi_dbi_command_buf(dbi, MIPI_DCS_WRITE_MEMORY_START, tr, len);
}

static void waveshare_update(struct drm_simple_display_pipe *pipe,
			     struct drm_plane_state *old_state)
{
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(pipe->crtc.dev);
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_rect rect;
	int idx;

	if (!pipe->crtc.state->active || !state->fb)
		return;
	if (!drm_dev_enter(&dbidev->drm, &idx))
		return;

	if (drm_atomic_helper_damage_merged(old_state, state, &rect)) {
		waveshare_flush(dbidev, state, &rect);
	} else {
		/*
		 * Some user space (X11 with the modesetting driver) flips the
		 * framebuffer without reporting damage; push the whole frame
		 * so the panel does not stay on the first image.
		 */
		rect.x1 = 0;
		rect.y1 = 0;
		rect.x2 = state->fb->width;
		rect.y2 = state->fb->height;
		waveshare_flush(dbidev, state, &rect);
	}

	drm_dev_exit(idx);
}

#endif /* LINUX_VERSION_CODE >= 6.11 */

static const struct drm_simple_display_pipe_funcs waveshare_pipe_funcs = {
	DRM_MIPI_DBI_SIMPLE_DISPLAY_PIPE_FUNCS(waveshare_enable),
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
	.update = waveshare_update,
#endif
};

static const struct drm_display_mode waveshare_mode = {
	.clock = 480 * 320 * 30 / 1000, /* 30 Hz with 125MHz SPI */
	.hdisplay = 480,
	.hsync_start = 480,
	.hsync_end = 480,
	.htotal = 480,
	.vdisplay = 320,
	.vsync_start = 320,
	.vsync_end = 320,
	.vtotal = 320,
	.width_mm = 73,
	.height_mm = 49,
};

DEFINE_DRM_GEM_DMA_FOPS(sunfounder_tft35ips_fops);

#ifdef DRIVER_PRIME
#define SF_TFT_DRIVER_PRIME	DRIVER_PRIME |
#else
#define SF_TFT_DRIVER_PRIME
#endif

#ifdef SF_TFT_FBDEV_GENERIC
#define SF_TFT_FBDEV_FEATURE	| DRIVER_FBDEV_EMULATION
#else
#define SF_TFT_FBDEV_FEATURE
#endif

static const struct drm_driver sunfounder_tft35ips_driver = {
	.driver_features	= SF_TFT_DRIVER_PRIME DRIVER_GEM | DRIVER_MODESET |
				  DRIVER_ATOMIC SF_TFT_FBDEV_FEATURE,
	.fops			= &sunfounder_tft35ips_fops,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
#if defined(DRM_FBDEV_DMA_DRIVER_OPS)
	DRM_FBDEV_DMA_DRIVER_OPS,
#else
	.dumb_create		= drm_gem_dma_dumb_create,
#endif
	.debugfs_init		= mipi_dbi_debugfs_init,
	.name			= "sunfounder-tft35ips",
	.desc			= "Sunfounder TFT35IPS",
	.major			= 1,
	.minor			= 0,
};

static const struct of_device_id sunfounder_tft35ips_of_match[] = {
	{ .compatible = "sunfounder,tft35ips" },
	{},
};
MODULE_DEVICE_TABLE(of, sunfounder_tft35ips_of_match);

static const struct spi_device_id sunfounder_tft35ips_id[] = {
	{ "sunfounder-tft35ips", 0 },
	/* modalias generated from compatible = "sunfounder,tft35ips" */
	{ "tft35ips", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, sunfounder_tft35ips_id);

static int sunfounder_tft35ips_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct mipi_dbi_dev *dbidev;
	struct drm_device *drm;
	struct mipi_dbi *dbi;
	struct gpio_desc *dc;
	u32 rotation = 0;
	int ret;

    g_bgr_enabled = device_property_read_bool(dev, "bgr");
	g_color_invert_enabled = device_property_read_bool(dev, "color_invert");

	dbidev = devm_drm_dev_alloc(dev, &sunfounder_tft35ips_driver,
				    struct mipi_dbi_dev, drm);
	if (IS_ERR(dbidev))
		return PTR_ERR(dbidev);

	dbi = &dbidev->dbi;
	drm = &dbidev->drm;

	dbi->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(dbi->reset))
		return dev_err_probe(dev, PTR_ERR(dbi->reset), "Failed to get GPIO 'reset'\n");

	dc = devm_gpiod_get(dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(dc))
		return dev_err_probe(dev, PTR_ERR(dc), "Failed to get GPIO 'dc'\n");

	dbidev->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(dbidev->backlight))
		return PTR_ERR(dbidev->backlight);

	device_property_read_u32(dev, "rotation", &rotation);
	ret = mipi_dbi_spi_init(spi, dbi, dc);
	if (ret)
		return ret;

	dbi->command = waveshare_command;
	dbi->read_commands = NULL;

	ret = mipi_dbi_dev_init(dbidev, &waveshare_pipe_funcs,
				&waveshare_mode, rotation);
	if (ret)
		return ret;

	drm_connector_update_edid_property(&dbidev->connector,
					   (const struct edid *)sf_tft_edid);

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	spi_set_drvdata(spi, drm);

#if defined(SF_TFT_FBDEV_CLIENT)
	drm_client_setup_with_fourcc(drm, DRM_FORMAT_RGB565);
#elif defined(SF_TFT_FBDEV_DMA)
	drm_fbdev_dma_setup(drm, 0);
#else
	drm_fbdev_generic_setup(drm, 16);
#endif

	return 0;
}

static void sunfounder_tft35ips_remove(struct spi_device *spi)
{
	struct drm_device *drm = spi_get_drvdata(spi);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
}

static void sunfounder_tft35ips_shutdown(struct spi_device *spi)
{
	drm_atomic_helper_shutdown(spi_get_drvdata(spi));
}

static struct spi_driver sunfounder_tft35ips_spi_driver = {
	.driver = {
		.name = "sunfounder-tft35ips",
		.of_match_table = sunfounder_tft35ips_of_match,
	},
	.id_table = sunfounder_tft35ips_id,
	.probe = sunfounder_tft35ips_probe,
	.remove = sunfounder_tft35ips_remove,
	.shutdown = sunfounder_tft35ips_shutdown,
};
module_spi_driver(sunfounder_tft35ips_spi_driver);

MODULE_DESCRIPTION("Sunfounder TFT35IPS DRM driver");
MODULE_AUTHOR("Kamlesh Gurudasani <kamlesh.gurudasani@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");
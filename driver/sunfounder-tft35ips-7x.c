// SPDX-License-Identifier: GPL-2.0+
/*
 * DRM driver for the SunFounder 3.5" IPS display (ST7796S, 16-bit command
 * variant), kernel 7.x API: the mipi-dbi simple-pipe helpers are gone, so the
 * driver creates its own plane / CRTC / connector.
 *
 * The 6.x implementation lives in sunfounder-tft35ips.c; the Makefile picks
 * one of the two by kernel version.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/spi/spi.h>

#include <video/mipi_display.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_edid.h>
#include <drm/drm_mipi_dbi.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_print.h>

#define ILI9486_MADCTL_BGR      BIT(3)
#define ILI9486_MADCTL_MV       BIT(5)
#define ILI9486_MADCTL_MX       BIT(6)
#define ILI9486_MADCTL_MY       BIT(7)

#define ST7796_SLPOUT      0x11
#define ST7796_INVON       0x21
#define ST7796_DISPON      0x29
#define ST7796_MADCTL      0x36
#define ST7796_PIXFMT      0x3A
#define ST7796_INVCTR      0xB4
#define ST7796_EMSET       0xB7
#define ST7796_PWCTR1      0xC0
#define ST7796_PWCTR2      0xC1
#define ST7796_PWCTR3      0xC2
#define ST7796_VMCTR1      0xC5
#define ST7796_GMCTRP1     0xE0
#define ST7796_GMCTRN1     0xE1
#define ST7796_DOCACTL     0xE8
#define ST7796_CSCONCTL    0xF0

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
	0x00, 0x00, 0x00, 0xfc, 0x00, 0x4d, 0x48, 0x53,
	0x33, 0x35, 0x2d, 0x49, 0x50, 0x53, 0x0a, 0x20,
	0x20, 0x20, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x1e,
	0x28, 0x14, 0x28, 0x00, 0x00, 0x0a, 0x20, 0x20,
	0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0x10,
	0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0xfe,
};truct sunfounder_tft35ips_device {
	struct mipi_dbi_dev dbidev;

	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
};

static bool g_bgr_enabled;
static bool g_color_invert_enabled;


/*
 * The module has a SPI to 16-bit parallel bus converter in front of the
 * controller, so every command and parameter goes out as 16 bit.
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

static int sf_tft_panel_init(struct mipi_dbi_dev *dbidev)
{
	struct mipi_dbi *dbi = &dbidev->dbi;
	int ret;

	ret = mipi_dbi_poweron_conditional_reset(dbidev);
	if (ret < 0)
		return ret;
	if (ret == 1) {
		/* back from DPMS off: wake the panel up again */
		mipi_dbi_command(dbi, ST7796_SLPOUT);
		msleep(120);
		mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);
		return 1;
	}

	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_OFF);
	msleep(120);

	mipi_dbi_command(dbi, ST7796_SLPOUT);
	msleep(255);

	/* Memory Data Access Control: X mirror, RGB */
	mipi_dbi_command(dbi, ST7796_MADCTL, 0x28);

	/* Interface Pixel Format: 16 bit */
	mipi_dbi_command(dbi, ST7796_PIXFMT, 0x55);

	mipi_dbi_command(dbi, ST7796_CSCONCTL, 0xC3);
	mipi_dbi_command(dbi, ST7796_CSCONCTL, 0x96);

	mipi_dbi_command(dbi, ST7796_INVCTR, 0x01);
	mipi_dbi_command(dbi, ST7796_EMSET, 0xC6);

	mipi_dbi_command(dbi, ST7796_PWCTR1, 0x80, 0x45);
	mipi_dbi_command(dbi, ST7796_PWCTR2, 0x13);
	mipi_dbi_command(dbi, ST7796_PWCTR3, 0xA7);
	mipi_dbi_command(dbi, ST7796_VMCTR1, 0x0a);
	msleep(120);

	mipi_dbi_command(dbi, ST7796_DOCACTL, 0x40, 0x8a, 0x00, 0x00, 0x29, 0x19,
			 0xa5, 0x33);

	mipi_dbi_command(dbi, ST7796_GMCTRP1, 0xd0, 0x08, 0x0f, 0x06, 0x06, 0x33,
			 0x30, 0x33, 0x47, 0x17, 0x13, 0x13, 0x2b, 0x31);
	mipi_dbi_command(dbi, ST7796_GMCTRN1, 0xd0, 0x0a, 0x11, 0x0b, 0x09, 0x07,
			 0x2f, 0x33, 0x47, 0x38, 0x15, 0x16, 0x2c, 0x32);
	msleep(120);

	mipi_dbi_command(dbi, ST7796_CSCONCTL, 0x3C);
	mipi_dbi_command(dbi, ST7796_CSCONCTL, 0x69);
	msleep(255);

	mipi_dbi_command(dbi, ST7796_INVON);
	mipi_dbi_command(dbi, ST7796_DISPON);
	msleep(100);

	if (g_color_invert_enabled)
		mipi_dbi_command(dbi, MIPI_DCS_ENTER_INVERT_MODE, 0x00);

	return 0;
}

static u8 sf_tft_addr_mode(struct mipi_dbi_dev *dbidev)
{
	u8 addr_mode;

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

	if (g_bgr_enabled)
		addr_mode |= ILI9486_MADCTL_BGR;
	else
		addr_mode &= ~ILI9486_MADCTL_BGR;

	return addr_mode;
}

static const u32 sf_tft_plane_formats[] = {
	DRM_MIPI_DBI_PLANE_FORMATS,
};

static const u64 sf_tft_plane_format_modifiers[] = {
	DRM_MIPI_DBI_PLANE_FORMAT_MODIFIERS,
};

static const struct drm_plane_helper_funcs sf_tft_plane_helper_funcs = {
	DRM_MIPI_DBI_PLANE_HELPER_FUNCS,
};

static const struct drm_plane_funcs sf_tft_plane_funcs = {
	DRM_MIPI_DBI_PLANE_FUNCS,
	.destroy = drm_plane_cleanup,
};

static void sf_tft_crtc_helper_atomic_enable(struct drm_crtc *crtc,
					     struct drm_atomic_commit *state)
{
	struct drm_device *drm = crtc->dev;
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(drm);
	struct mipi_dbi *dbi = &dbidev->dbi;
	int idx;

	if (!drm_dev_enter(drm, &idx))
		return;

	if (sf_tft_panel_init(dbidev) < 0)
		goto out_exit;

	mipi_dbi_command(dbi, MIPI_DCS_SET_ADDRESS_MODE, sf_tft_addr_mode(dbidev));

	backlight_enable(dbidev->backlight);

out_exit:
	drm_dev_exit(idx);
}

static const struct drm_crtc_helper_funcs sf_tft_crtc_helper_funcs = {
	DRM_MIPI_DBI_CRTC_HELPER_FUNCS,
	.atomic_enable = sf_tft_crtc_helper_atomic_enable,
};

static const struct drm_crtc_funcs sf_tft_crtc_funcs = {
	DRM_MIPI_DBI_CRTC_FUNCS,
	.destroy = drm_crtc_cleanup,
};

static const struct drm_encoder_funcs sf_tft_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static const struct drm_connector_helper_funcs sf_tft_connector_helper_funcs = {
	DRM_MIPI_DBI_CONNECTOR_HELPER_FUNCS,
};

static const struct drm_connector_funcs sf_tft_connector_funcs = {
	DRM_MIPI_DBI_CONNECTOR_FUNCS,
	.destroy = drm_connector_cleanup,
};

static const struct drm_mode_config_helper_funcs sf_tft_mode_config_helper_funcs = {
	DRM_MIPI_DBI_MODE_CONFIG_HELPER_FUNCS,
};

static const struct drm_mode_config_funcs sf_tft_mode_config_funcs = {
	DRM_MIPI_DBI_MODE_CONFIG_FUNCS,
};

/* 30 Hz: 480 x 320 x 30 / 1000 = 4608 kHz, which is what 125 MHz SPI can feed */
static const struct drm_display_mode waveshare_mode = {
	.clock = 480 * 320 * 30 / 1000,
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

static const struct drm_driver sunfounder_tft35ips_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &sunfounder_tft35ips_fops,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.debugfs_init		= mipi_dbi_debugfs_init,
	.name			= "sunfounder-tft35ips",
	.desc			= "SunFounder TFT35IPS",
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
	{ "tft35ips", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, sunfounder_tft35ips_id);

static int sunfounder_tft35ips_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct sunfounder_tft35ips_device *tft;
	struct mipi_dbi_dev *dbidev;
	struct drm_device *drm;
	struct mipi_dbi *dbi;
	struct gpio_desc *dc;
	struct drm_plane *plane;
	struct drm_crtc *crtc;
	struct drm_encoder *encoder;
	struct drm_connector *connector;
	u32 rotation = 0;
	int ret;

	g_bgr_enabled = device_property_read_bool(dev, "bgr");
	g_color_invert_enabled = device_property_read_bool(dev, "color_invert");

	tft = devm_drm_dev_alloc(dev, &sunfounder_tft35ips_driver,
				 struct sunfounder_tft35ips_device, dbidev.drm);
	if (IS_ERR(tft))
		return PTR_ERR(tft);

	dbidev = &tft->dbidev;
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

	ret = drm_mipi_dbi_dev_init(dbidev, &waveshare_mode, sf_tft_plane_formats[0],
				    rotation, 0);
	if (ret)
		return ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.min_width = dbidev->mode.hdisplay;
	drm->mode_config.max_width = 4096;
	drm->mode_config.min_height = dbidev->mode.vdisplay;
	drm->mode_config.max_height = 4096;
	drm->mode_config.funcs = &sf_tft_mode_config_funcs;
	drm->mode_config.preferred_depth = 16;
	drm->mode_config.helper_private = &sf_tft_mode_config_helper_funcs;

	plane = &tft->plane;
	ret = drm_universal_plane_init(drm, plane, 0, &sf_tft_plane_funcs,
				       sf_tft_plane_formats,
				       ARRAY_SIZE(sf_tft_plane_formats),
				       sf_tft_plane_format_modifiers,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(plane, &sf_tft_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(plane);

	crtc = &tft->crtc;
	ret = drm_crtc_init_with_planes(drm, crtc, plane, NULL, &sf_tft_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(crtc, &sf_tft_crtc_helper_funcs);

	encoder = &tft->encoder;
	ret = drm_encoder_init(drm, encoder, &sf_tft_encoder_funcs,
			       DRM_MODE_ENCODER_NONE, NULL);
	if (ret)
		return ret;
	encoder->possible_crtcs = drm_crtc_mask(crtc);

	connector = &tft->connector;
	ret = drm_connector_init(drm, connector, &sf_tft_connector_funcs,
				 DRM_MODE_CONNECTOR_SPI);
	if (ret)
		return ret;
	drm_connector_helper_add(connector, &sf_tft_connector_helper_funcs);
	drm_connector_update_edid_property(connector, (const struct edid *)sf_tft_edid);

	ret = drm_connector_attach_encoder(connector, encoder);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	spi_set_drvdata(spi, drm);

	drm_client_setup(drm, NULL);

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

MODULE_DESCRIPTION("SunFounder TFT35IPS DRM driver (kernel 7.x API)");
MODULE_AUTHOR("Kamlesh Gurudasani <kamlesh.gurudasani@gmail.com>");
MODULE_LICENSE("GPL");

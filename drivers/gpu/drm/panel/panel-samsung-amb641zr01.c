// SPDX-License-Identifier: GPL-2.0-only
/* 
 * Panel driver for the Samsung AMB641ZR01.
 * Found in the Samsung Galaxy S21 FE phone.
 *
 * Copyright (c) 2026  <>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/backlight.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_panel.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>

#define L2_ACCESS_ENABLE	(const u8[]){ 0xf0, 0x5a, 0x5a }
#define L2_ACCESS_DISABLE	(const u8[]){ 0xf0, 0xa5, 0xa5 }
#define UPDATE_REFRESH_RATE	(const u8[]){ 0xf7, 0x0f }

struct amb641zr01_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_dsc_config *dsc;
	struct regulator *supply_vdd;
	struct regulator *supply_vddio;
	struct regulator *supply_vddr;
	struct regulator *supply_vddi;
	struct regulator *supply_avdd;
	struct gpio_desc *reset_gpio;
	const struct drm_display_mode *mode;
	bool prepared;
};

static const struct drm_display_mode amb641zr01_120hz_fhd_mode = {
	.clock = (1080 + 92 + 92 + 92) * (2340 + 16 + 8 + 12) * 120 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 92,
	.hsync_end = 1080 + 92 + 92,
	.htotal = 1080 + 92 + 92 + 92,
	.vdisplay = 2340,
	.vsync_start = 2340 + 16,
	.vsync_end = 2340 + 16 + 8,
	.vtotal = 2340 + 16 + 8 + 12,
	.width_mm = 67,
	.height_mm = 150,
	.type = DRM_MODE_TYPE_DRIVER,
};

static inline
struct amb641zr01_panel *to_amb641zr01_panel(struct drm_panel *panel)
{
	return container_of(panel, struct amb641zr01_panel, panel);
}

static void amb641zr01_panel_reset(struct amb641zr01_panel *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
}

static int amb641zr01_panel_on(struct amb641zr01_panel *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_write_seq(dsi, 0xc2, 0x14);
	mipi_dsi_dcs_write_seq(dsi, 0x9d, 0x01);

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to exit sleep mode: %d\n", ret);
		return ret;
	}
	msleep(121);
	mipi_dsi_dcs_write_seq(dsi, MIPI_DCS_SET_ADDRESS_MODE, 0x00);

	ret = mipi_dsi_dcs_set_tear_on(dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (ret < 0) {
		dev_err(dev, "Failed to set tear on: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_set_column_address(dsi, 0x0000, 0x0437);
	if (ret < 0) {
		dev_err(dev, "Failed to set column address: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_set_page_address(dsi, 0x0000, 0x095f);
	if (ret < 0) {
		dev_err(dev, "Failed to set page address: %d\n", ret);
		return ret;
	}

	mipi_dsi_dcs_write_buffer(dsi, L2_ACCESS_ENABLE, 3);
	msleep(50);
	mipi_dsi_dcs_write_seq(dsi, 0xb0, 0x27, 0xf2);
	mipi_dsi_dcs_write_seq(dsi, 0xf2, 0x00);
	mipi_dsi_dcs_write_buffer(dsi, L2_ACCESS_DISABLE, 3);
	mipi_dsi_dcs_write_buffer(dsi, L2_ACCESS_ENABLE, 3);
	mipi_dsi_dcs_write_seq(dsi, 0x60, 0x00, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xf7, 0x0f);
	mipi_dsi_dcs_write_seq(dsi, 0x35, 0x00);
	mipi_dsi_dcs_write_buffer(dsi, UPDATE_REFRESH_RATE, 2);
	mipi_dsi_dcs_write_buffer(dsi, L2_ACCESS_DISABLE, 3);
	mipi_dsi_dcs_write_seq(dsi, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display on: %d\n", ret);
		return ret;
	}

    return 0;
}

static int amb641zr01_panel_off(struct amb641zr01_panel *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_write_buffer(dsi, L2_ACCESS_ENABLE, 3);
	msleep(50);
	mipi_dsi_dcs_write_seq(dsi, 0x91, 0x02);
	mipi_dsi_dcs_write_seq(dsi, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);
	mipi_dsi_dcs_write_buffer(dsi, L2_ACCESS_DISABLE, 3);

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display off: %d\n", ret);
		return ret;
	}
	msleep(21);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to enter sleep mode: %d\n", ret);
		return ret;
	}
	msleep(101);

	return 0;
}

static int amb641zr01_panel_prepare(struct drm_panel *panel)
{
	struct amb641zr01_panel *ctx = to_amb641zr01_panel(panel);
	struct device *dev = &ctx->dsi->dev;
	struct drm_dsc_picture_parameter_set pps;
	int ret;

	if (ctx->prepared)
		return 0;

	ctx->dsc->pic_width = ctx->mode->hdisplay;
	ctx->dsc->pic_height = ctx->mode->vdisplay;
	drm_dsc_compute_rc_parameters(ctx->dsc);

	ret = regulator_enable(ctx->supply_vdd);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulator: %d\n", ret);
		return ret;
	}

	ret = regulator_enable(ctx->supply_vddio);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulator: %d\n", ret);
		return ret;
	}

	ret = regulator_enable(ctx->supply_vddr);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulator: %d\n", ret);
		return ret;
	}

	ret = regulator_enable(ctx->supply_vddi);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulator: %d\n", ret);
		return ret;
	}
	
	ret = regulator_enable(ctx->supply_avdd);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulator: %d\n", ret);
		return ret;
	}

	amb641zr01_panel_reset(ctx);

	ret = amb641zr01_panel_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		return ret;
	}

	drm_dsc_pps_payload_pack(&pps, ctx->dsc);

	ret = mipi_dsi_picture_parameter_set(ctx->dsi, &pps);
	if (ret < 0) {
		dev_err(panel->dev, "failed to transmit PPS: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_compression_mode(ctx->dsi, true);
	if (ret < 0) {
		dev_err(dev, "failed to enable compression mode: %d\n", ret);
		return ret;
	}
	
	msleep(28);

	ctx->prepared = true;
	return 0;
}

static int amb641zr01_panel_unprepare(struct drm_panel *panel)
{
	struct amb641zr01_panel *ctx = to_amb641zr01_panel(panel);

	if (!ctx->prepared)
		return 0;

	regulator_disable(ctx->supply_vdd);
	regulator_disable(ctx->supply_vddio);
	regulator_disable(ctx->supply_vddi);
	regulator_disable(ctx->supply_vddr);
	regulator_disable(ctx->supply_avdd);

	ctx->prepared = false;
	return 0;
}

static int amb641zr01_panel_disable(struct drm_panel *panel) {
	struct amb641zr01_panel *ctx = to_amb641zr01_panel(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = amb641zr01_panel_off(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to disable panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		return ret;
	}

	return ret;
}

static int amb641zr01_panel_get_modes(struct drm_panel *panel,
				      struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	struct amb641zr01_panel *ctx = to_amb641zr01_panel(panel);

	mode = drm_mode_duplicate(connector->dev, ctx->mode);
	if (!mode)
		return -ENOMEM;
	
	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs amb641zr01_panel_funcs = {
	.prepare = amb641zr01_panel_prepare,
	.unprepare = amb641zr01_panel_unprepare,
	.disable = amb641zr01_panel_disable,
	.get_modes = amb641zr01_panel_get_modes,
};

static int amb641zr01_panel_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
	if (ret < 0)
		return ret;
	
	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return 0;
}

static int amb641zr01_panel_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_get_display_brightness_large(dsi, &brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return brightness;
}

static const struct backlight_ops amb641zr01_panel_bl_ops = {
	.update_status = amb641zr01_panel_bl_update_status,
	.get_brightness = amb641zr01_panel_bl_get_brightness,
};

static struct backlight_device *amb641zr01_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 255,
		/* Panel can go higher in high brightness mode */
		.max_brightness = 486,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &amb641zr01_panel_bl_ops, &props);
}

static int amb641zr01_panel_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct amb641zr01_panel *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->mode = of_device_get_match_data(dev);

	if (!ctx->mode) {
		dev_err(dev, "Missing device mode\n");
		return -ENODEV;
	}

	ctx->supply_vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(ctx->supply_vdd))
		return dev_err_probe(dev, PTR_ERR(ctx->supply_vdd),
				     "Failed to get vdd regulator\n");
	
	ctx->supply_vddio = devm_regulator_get(dev, "vddio");
	if (IS_ERR(ctx->supply_vddio))
		return dev_err_probe(dev, PTR_ERR(ctx->supply_vddio),
				     "Failed to get vddio regulator\n");
	
	ctx->supply_avdd = devm_regulator_get(dev, "avdd");
	if (IS_ERR(ctx->supply_avdd))
		return dev_err_probe(dev, PTR_ERR(ctx->supply_avdd),
				     "Failed to get avdd regulator\n");
	
	ctx->supply_vddr = devm_regulator_get(dev, "vddr");
	if (IS_ERR(ctx->supply_vddr))
		return dev_err_probe(dev, PTR_ERR(ctx->supply_vddr),
				     "Failed to get vddr regulator\n");
	
	ctx->supply_vddi = devm_regulator_get(dev, "vddi");
	if (IS_ERR(ctx->supply_vddi))
		return dev_err_probe(dev, PTR_ERR(ctx->supply_vddi),
				     "Failed to get vddi regulator\n");
	
	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

    dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;

	dsi->mode_flags = MIPI_DSI_MODE_LPM;

	drm_panel_init(&ctx->panel, dev, &amb641zr01_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = amb641zr01_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	ctx->dsc = devm_kzalloc(dev, sizeof(*ctx->dsc), GFP_KERNEL);
	if (!ctx->dsc)
		return -ENOMEM;
	
	/* This panel only supports DSC */
	
	ctx->dsc->dsc_version_major = 1;
	ctx->dsc->dsc_version_minor = 1;
	
	dsi->slice_per_pkt = 1;
	ctx->dsc->slice_height = 30;
	ctx->dsc->slice_width = 540;

	WARN_ON(1080 % ctx->dsc->slice_width);
	ctx->dsc->slice_count = 1080 / ctx->dsc->slice_width;
	ctx->dsc->bits_per_component = 8;
	ctx->dsc->bits_per_pixel = 8 << 4; /* 4 fractional bits */
	ctx->dsc->block_pred_enable = true;

	dsi->dsc = ctx->dsc;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to attach to DSI host: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static const struct of_device_id amb641zr01_panel_of_match[] = {
	{
		.compatible = "samsung,amb641zr01",
		.data = &amb641zr01_120hz_fhd_mode,
	},
	{ /* sentinel */ }
};

static void amb641zr01_panel_remove(struct mipi_dsi_device *dsi)
{
	struct amb641zr01_panel *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

MODULE_DEVICE_TABLE(of, amb641zr01_panel_of_match);

static struct mipi_dsi_driver amb641zr01_panel_driver = {
	.probe = amb641zr01_panel_probe,
	.remove = amb641zr01_panel_remove,
	.driver = {
		.name = "panel-samsung-amb641zr01",
		.of_match_table = amb641zr01_panel_of_match,
	},
};

module_mipi_dsi_driver(amb641zr01_panel_driver);
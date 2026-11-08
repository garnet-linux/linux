// SPDX-License-Identifier: GPL-2.0-only
/*
 * Novatek NT37706 AMOLED DSI panel driver
 *
 * The NT37706 is used in the 1220x2712 120 Hz panel found in the
 * Xiaomi Redmi Note 13 Pro 5G (garnet).
 *
 * The initialization sequence was derived from the vendor device tree
 * (dsi-panel-n16-42-02-0b-dsc-vid.dtsi).
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#include <video/mipi_display.h>

struct nt37706_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_dsc_config dsc;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[3];
	bool prepared;
};

static const struct regulator_bulk_data nt37706_supplies[] = {
	{ .supply = "vddio" },
	{ .supply = "vci" },
	{ .supply = "vdd" },
};

static inline struct nt37706_panel *to_nt37706_panel(struct drm_panel *panel)
{
	return container_of(panel, struct nt37706_panel, panel);
}

/*
 * The vendor DSI stack sends every init command as a DCS long write (0x39),
 * even when the payload is only one or two bytes. The generic DCS helpers pick
 * a short write (0x05/0x15) for those, which the host/DDIC combination does not
 * accept (first short write after the page switch times out with -110).
 * Always use long writes for the init sequence.
 */
static void nt37706_long_write(struct mipi_dsi_multi_context *ctx,
			       const u8 *data, size_t len)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct mipi_dsi_msg msg = {
		.channel = dsi->channel,
		.type = MIPI_DSI_DCS_LONG_WRITE,
		.tx_buf = data,
		.tx_len = len,
	};
	ssize_t ret;

	if (ctx->accum_err)
		return;

	if (dsi->mode_flags & MIPI_DSI_MODE_LPM)
		msg.flags |= MIPI_DSI_MSG_USE_LPM;

	ret = dsi->host->ops->transfer(dsi->host, &msg);
	if (ret < 0) {
		dev_err(&dsi->dev, "long write 0x%02x failed: %zd\n",
			data[0], ret);
		ctx->accum_err = ret;
	}
}

#define nt37706_write_seq(ctx, cmd, seq...)				\
	do {								\
		static const u8 d[] = { cmd, seq };			\
		nt37706_long_write(ctx, d, ARRAY_SIZE(d));		\
	} while (0)

#define NT37706_SWITCH_PAGE	0xf0

#define nt37706_switch_page(dsi_ctx, page) \
	nt37706_write_seq((dsi_ctx), NT37706_SWITCH_PAGE, \
			  0x55, 0xaa, 0x52, 0x08, (page))

static void nt37706_reset(struct nt37706_panel *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(11000, 12000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(11000, 12000);
}

static int nt37706_on(struct nt37706_panel *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };
	struct drm_dsc_picture_parameter_set pps;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	/*
	 * The vendor timing node for this panel (n16_42_02_0b) does NOT have
	 * qcom,mdss-dsc-config-by-manufacture-cmd (that property only exists in
	 * the n16_36_0d_0a node), so the vendor DSI stack sends the DSC PPS
	 * (DSI type 0x0a) itself, before the DCS init sequence. Without it the
	 * DDIC never learns the slice/rate parameters, lights up for a moment
	 * and then shows nothing. The host has already filled in ctx->dsc
	 * (picture size, rc parameters, chunk size) at this point.
	 */
	drm_dsc_pps_payload_pack(&pps, &ctx->dsc);
	mipi_dsi_picture_parameter_set_multi(&dsi_ctx, &pps);

	nt37706_switch_page(&dsi_ctx, 0x00);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x03);
	nt37706_write_seq(&dsi_ctx, 0xc0, 0x21);
	nt37706_write_seq(&dsi_ctx, 0xdf, 0x09);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x01);
	nt37706_write_seq(&dsi_ctx, 0xdf, 0x40);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x31);
	nt37706_write_seq(&dsi_ctx, 0xdf, 0x00, 0x1a);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x34);
	nt37706_write_seq(&dsi_ctx, 0xdf, 0x23);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x2d);
	nt37706_write_seq(&dsi_ctx, 0xc0, 0x00, 0x32, 0x00, 0x04, 0x50);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x01);
	nt37706_write_seq(&dsi_ctx, 0xc7, 0x01);
	nt37706_switch_page(&dsi_ctx, 0x01);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x2a);
	nt37706_write_seq(&dsi_ctx, 0xb9, 0x10);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x0c);
	nt37706_write_seq(&dsi_ctx, 0xb9, 0x1a);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x05);
	nt37706_write_seq(&dsi_ctx, 0xe3, 0x00, 0x00);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x0a);
	nt37706_write_seq(&dsi_ctx, 0xe3, 0x00, 0x00, 0x00, 0x00);
	nt37706_switch_page(&dsi_ctx, 0x08);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0xce);
	nt37706_write_seq(&dsi_ctx, 0xbc, 0x01);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0xcf);
	nt37706_write_seq(&dsi_ctx, 0xbc, 0x01, 0x11, 0x00, 0xb6,
				     0x01, 0x11, 0x00, 0x44);
	nt37706_write_seq(&dsi_ctx, 0xbe, 0x03);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x0c);
	nt37706_write_seq(&dsi_ctx, 0xe9, 0x1d, 0x36, 0x0f, 0xd6,
				     0x1c, 0x51, 0x0f, 0xab);
	nt37706_write_seq(&dsi_ctx, 0x88, 0x81, 0x02, 0x61, 0x09, 0x85);
	nt37706_write_seq(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x80);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x2a);
	nt37706_write_seq(&dsi_ctx, 0xf4, 0x08);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x46);
	nt37706_write_seq(&dsi_ctx, 0xf4, 0x07, 0x09);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x4a);
	nt37706_write_seq(&dsi_ctx, 0xf4, 0x08, 0x0a);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x56);
	nt37706_write_seq(&dsi_ctx, 0xf4, 0x44, 0x44);
	nt37706_write_seq(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x81);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x3c);
	nt37706_write_seq(&dsi_ctx, 0xf5, 0x84);
	nt37706_write_seq(&dsi_ctx, 0x17, 0x03);
	nt37706_write_seq(&dsi_ctx, 0x71, 0x00);
	nt37706_write_seq(&dsi_ctx, 0x8d, 0x00, 0x00, 0x04, 0xc3,
				     0x00, 0x00, 0x0a, 0x97);
	nt37706_write_seq(&dsi_ctx, 0x2a, 0x00, 0x00, 0x04, 0xc3);
	nt37706_write_seq(&dsi_ctx, 0x2b, 0x00, 0x00, 0x0a, 0x97);
	nt37706_write_seq(&dsi_ctx, 0x90, 0x03, 0x43);
	nt37706_write_seq(&dsi_ctx, 0x91, 0xab, 0xa8, 0x00, 0x0c,
				     0xc2, 0x00, 0x02, 0x32, 0x01, 0x31,
				     0x00, 0x08, 0x08, 0xbb, 0x07, 0x7b,
				     0x10, 0xf0);
	nt37706_write_seq(&dsi_ctx, 0x3b, 0x00, 0x0b, 0x00, 0x2d,
				     0x00, 0x0b, 0x03, 0xcd, 0x00, 0x0b,
				     0x0a, 0xfd);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x10);
	nt37706_write_seq(&dsi_ctx, 0x3b, 0x00, 0x0b, 0x00, 0x2d);
	nt37706_write_seq(&dsi_ctx, 0x35, 0x00);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x04);
	/* TEST: initial DBV set here (no DSI cmd DMA once video is running) */
	nt37706_write_seq(&dsi_ctx, 0x51, 0x01, 0x33);
	nt37706_write_seq(&dsi_ctx, 0x53, 0x20);
	nt37706_write_seq(&dsi_ctx, 0x57, 0x00);
	nt37706_write_seq(&dsi_ctx, 0x2f, 0x00);
	nt37706_write_seq(&dsi_ctx, 0x26, 0x00);
	nt37706_write_seq(&dsi_ctx, 0x5f, 0x04, 0x40);
	nt37706_switch_page(&dsi_ctx, 0x00);
	nt37706_write_seq(&dsi_ctx, 0xbe, 0x47, 0x45);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x05);
	nt37706_write_seq(&dsi_ctx, 0xbe, 0x28);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x19);
	nt37706_write_seq(&dsi_ctx, 0xbe, 0x10, 0x91, 0x00, 0xab);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x0d);
	nt37706_write_seq(&dsi_ctx, 0xd8, 0x02);
	nt37706_switch_page(&dsi_ctx, 0x07);
	nt37706_write_seq(&dsi_ctx, 0xb0, 0x8c, 0xc0, 0x78,
				     0x70, 0x00, 0x00);
	nt37706_write_seq(&dsi_ctx, 0xb1, 0x1c, 0x0c, 0x00,
				     0x0c, 0x1c, 0x00);
	nt37706_write_seq(&dsi_ctx, 0xb2, 0x28, 0x28, 0x28,
				     0x28, 0x28, 0x28, 0x28, 0x28, 0x28);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x36);
	nt37706_write_seq(&dsi_ctx, 0xb2, 0x32, 0x32, 0x32,
				     0x32, 0x32, 0x32, 0x32, 0x32, 0x32,
				     0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c,
				     0x0c, 0x0c, 0x0c);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x09);
	nt37706_write_seq(&dsi_ctx, 0xb2, 0x28, 0x28, 0x28,
				     0x28, 0x28, 0x28, 0x28, 0x28, 0x28);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x48);
	nt37706_write_seq(&dsi_ctx, 0xb2, 0x32, 0x32, 0x32,
				     0x32, 0x32, 0x32, 0x32, 0x32, 0x32,
				     0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c,
				     0x0c, 0x0c, 0x0c);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x1b);
	nt37706_write_seq(&dsi_ctx, 0xb2, 0x28, 0x28, 0x28,
				     0x28, 0x28, 0x28, 0x28, 0x28, 0x28);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x6c);
	nt37706_write_seq(&dsi_ctx, 0xb2, 0x32, 0x32, 0x32,
				     0x32, 0x32, 0x32, 0x32, 0x32, 0x32,
				     0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c,
				     0x0c, 0x0c, 0x0c);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x24);
	nt37706_write_seq(&dsi_ctx, 0xb2, 0x28, 0x28, 0x28,
				     0x28, 0x28, 0x28, 0x28, 0x28, 0x28);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x7e);
	nt37706_write_seq(&dsi_ctx, 0xb2, 0x32, 0x32, 0x32,
				     0x32, 0x32, 0x32, 0x32, 0x32, 0x32,
				     0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c,
				     0x0c, 0x0c, 0x0c);
	nt37706_write_seq(&dsi_ctx, 0xb3, 0x6c);
	nt37706_switch_page(&dsi_ctx, 0x01);
	nt37706_write_seq(&dsi_ctx, 0xea, 0x91);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x08);
	nt37706_write_seq(&dsi_ctx, 0xea, 0x48);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x0b);
	nt37706_write_seq(&dsi_ctx, 0xea, 0x91);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x13);
	nt37706_write_seq(&dsi_ctx, 0xea, 0x72);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x04);
	nt37706_write_seq(&dsi_ctx, 0xc3, 0x0a);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x09);
	nt37706_write_seq(&dsi_ctx, 0xc3, 0x0a);
	nt37706_switch_page(&dsi_ctx, 0x01);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x03);
	nt37706_write_seq(&dsi_ctx, 0xc7, 0xc7);
	nt37706_switch_page(&dsi_ctx, 0x00);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x02);
	nt37706_write_seq(&dsi_ctx, 0xbe, 0x4c, 0x4b);
	nt37706_switch_page(&dsi_ctx, 0x01);
	nt37706_write_seq(&dsi_ctx, 0xd1, 0x07, 0x04, 0x04);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x08);
	nt37706_write_seq(&dsi_ctx, 0xd1, 0x01);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x0b);
	nt37706_write_seq(&dsi_ctx, 0xd1, 0x01);
	nt37706_write_seq(&dsi_ctx, 0x11, 0x00);
	mipi_dsi_msleep(&dsi_ctx, 120);
	nt37706_write_seq(&dsi_ctx, 0x29, 0x00);
	nt37706_switch_page(&dsi_ctx, 0x04);
	nt37706_write_seq(&dsi_ctx, 0xcb, 0x66);
	nt37706_write_seq(&dsi_ctx, 0xdd, 0x05, 0x08, 0x1f,
				     0x3e, 0x9b);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x05);
	nt37706_write_seq(&dsi_ctx, 0xdd, 0x05, 0x08, 0x1f,
				     0x3e, 0x9b);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x0a);
	nt37706_write_seq(&dsi_ctx, 0xdd, 0x05, 0x08, 0x1f,
				     0x3e, 0x9b);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x0f);
	nt37706_write_seq(&dsi_ctx, 0xdd, 0x05, 0x08, 0x1f,
				     0x3e, 0x9b);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x14);
	nt37706_write_seq(&dsi_ctx, 0xdd, 0x05, 0x08, 0x1f,
				     0x3e, 0x9b);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x19);
	nt37706_write_seq(&dsi_ctx, 0xdd, 0x05, 0x08, 0x1f,
				     0x3e, 0x9b);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x5a);
	nt37706_write_seq(&dsi_ctx, 0xd2, 0x10, 0x10, 0x10,
				     0x10, 0x10, 0x14);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x60);
	nt37706_write_seq(&dsi_ctx, 0xd2, 0x10, 0x10, 0x10,
				     0x10, 0x10, 0x0c);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x66);
	nt37706_write_seq(&dsi_ctx, 0xd2, 0x12, 0x12, 0x12,
				     0x12, 0x12, 0x0c);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x6c);
	nt37706_write_seq(&dsi_ctx, 0xd2, 0x12, 0x12, 0x12,
				     0x12, 0x12, 0x0a);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x72);
	nt37706_write_seq(&dsi_ctx, 0xd2, 0x12, 0x12, 0x12,
				     0x12, 0x12, 0x08);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x78);
	nt37706_write_seq(&dsi_ctx, 0xd2, 0x28, 0x28, 0x28,
				     0x28, 0x28, 0x14);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x7e);
	nt37706_write_seq(&dsi_ctx, 0xd2, 0x1a, 0x1a, 0x1a,
				     0x1a, 0x1a, 0x10);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x84);
	nt37706_write_seq(&dsi_ctx, 0xd2, 0x1a, 0x1a, 0x1a,
				     0x1a, 0x1a, 0x10);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x8a);
	nt37706_write_seq(&dsi_ctx, 0xd2, 0x1a, 0x1a, 0x1a,
				     0x1a, 0x1a, 0x10);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x90);
	nt37706_write_seq(&dsi_ctx, 0xd2, 0x1a, 0x1a, 0x1a,
				     0x1a, 0x1a, 0x08);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x96);
	nt37706_write_seq(&dsi_ctx, 0xd2, 0x14, 0x14, 0x14,
				     0x14, 0x14, 0x10);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0x9c);
	nt37706_write_seq(&dsi_ctx, 0xd2, 0x10, 0x10, 0x10,
				     0x10, 0x10, 0x08);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0xa2);
	nt37706_write_seq(&dsi_ctx, 0xd2, 0x0d, 0x0d, 0x0d,
				     0x0d, 0x0d, 0x08);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0xa8);
	nt37706_write_seq(&dsi_ctx, 0xd2, 0x0d, 0x0d, 0x0d,
				     0x0d, 0x0d, 0x08);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0xae);
	nt37706_write_seq(&dsi_ctx, 0xd2, 0x0b, 0x0b, 0x0b,
				     0x0b, 0x0b, 0x08);
	nt37706_switch_page(&dsi_ctx, 0x00);
	nt37706_write_seq(&dsi_ctx, 0xe6, 0x80);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0xea);
	nt37706_write_seq(&dsi_ctx, 0xe8, 0x10, 0x10, 0x20,
				     0x00, 0x00, 0x00);
	nt37706_write_seq(&dsi_ctx, 0xe6, 0x81);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0xea);
	nt37706_write_seq(&dsi_ctx, 0xe8, 0x10, 0x10, 0x20,
				     0x00, 0x00, 0x00);
	nt37706_write_seq(&dsi_ctx, 0xe6, 0x82);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0xea);
	nt37706_write_seq(&dsi_ctx, 0xe8, 0x10, 0x10, 0x20,
				     0x00, 0x00, 0x00);
	nt37706_write_seq(&dsi_ctx, 0xe6, 0x83);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0xea);
	nt37706_write_seq(&dsi_ctx, 0xe8, 0x10, 0x10, 0x20,
				     0x00, 0x00, 0x00);
	nt37706_write_seq(&dsi_ctx, 0xe6, 0x84);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0xea);
	nt37706_write_seq(&dsi_ctx, 0xe8, 0x10, 0x10, 0x20,
				     0x00, 0x00, 0x00);
	nt37706_write_seq(&dsi_ctx, 0xe6, 0x85);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0xea);
	nt37706_write_seq(&dsi_ctx, 0xe8, 0x10, 0x10, 0x20,
				     0x00, 0x00, 0x00);
	nt37706_write_seq(&dsi_ctx, 0xe6, 0x86);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0xea);
	nt37706_write_seq(&dsi_ctx, 0xe8, 0x10, 0x10, 0x20,
				     0x00, 0x00, 0x00);
	nt37706_write_seq(&dsi_ctx, 0xe6, 0x87);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0xea);
	nt37706_write_seq(&dsi_ctx, 0xe8, 0x10, 0x10, 0x20,
				     0x00, 0x00, 0x00);
	nt37706_write_seq(&dsi_ctx, 0xe6, 0x88);
	nt37706_write_seq(&dsi_ctx, 0x6f, 0xea);
	nt37706_write_seq(&dsi_ctx, 0xe8, 0x10, 0x10, 0x20,
				     0x00, 0x00, 0x00);
	nt37706_write_seq(&dsi_ctx, 0xe6, 0x00);

	mipi_dsi_msleep(&dsi_ctx, 28);

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	return dsi_ctx.accum_err;
}

static int nt37706_off(struct nt37706_panel *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x28, 0x00);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x10, 0x00);
	mipi_dsi_msleep(&dsi_ctx, 120);

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	return dsi_ctx.accum_err;
}

static int nt37706_prepare(struct drm_panel *panel)
{
	struct nt37706_panel *ctx = to_nt37706_panel(panel);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret)
		return ret;

	usleep_range(10000, 11000);

	nt37706_reset(ctx);

	ret = nt37706_on(ctx);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
		return ret;
	}

	ctx->prepared = true;

	dev_info(&ctx->dsi->dev,
		 "panel on: %dx%d, DSC %dx%d, %d slices, %d bpc, %d bpp\n",
		 ctx->dsc.pic_width, ctx->dsc.pic_height,
		 ctx->dsc.slice_width, ctx->dsc.slice_height,
		 ctx->dsc.slice_count, ctx->dsc.bits_per_component,
		 ctx->dsc.bits_per_pixel >> 4);

	return 0;
}

static int nt37706_unprepare(struct drm_panel *panel)
{
	struct nt37706_panel *ctx = to_nt37706_panel(panel);
	int ret;

	ctx->prepared = false;

	ret = nt37706_off(ctx);
	if (ret < 0)
		dev_err(&ctx->dsi->dev, "Failed to power off panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);

	return 0;
}

static const struct drm_display_mode nt37706_mode = {
	.clock = (1220 + 42 + 20 + 80) * (2712 + 45 + 2 + 9) * 120 / 1000,
	.hdisplay = 1220,
	.hsync_start = 1220 + 42,
	.hsync_end = 1220 + 42 + 20,
	.htotal = 1220 + 42 + 20 + 80,
	.vdisplay = 2712,
	.vsync_start = 2712 + 45,
	.vsync_end = 2712 + 45 + 2,
	.vtotal = 2712 + 45 + 2 + 9,
	.width_mm = 70,
	.height_mm = 154,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int nt37706_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &nt37706_mode);
}

static int nt37706_enable(struct drm_panel *panel)
{
	/*
	 * drm_panel_enable() turns the backlight on right after this hook.
	 * The vendor DT has qcom,bl-update-flag = "delay_until_first_frame":
	 * let a few video frames go out before the first brightness write.
	 */
	msleep(50);

	return 0;
}

static const struct drm_panel_funcs nt37706_panel_funcs = {
	.prepare = nt37706_prepare,
	.enable = nt37706_enable,
	.unprepare = nt37706_unprepare,
	.get_modes = nt37706_get_modes,
};

static int nt37706_bl_update_status(struct backlight_device *bl)
{
	struct nt37706_panel *ctx = bl_get_data(bl);
	struct mipi_dsi_device *dsi = ctx->dsi;
	unsigned int brightness = backlight_get_brightness(bl);
	unsigned long old_flags = dsi->mode_flags;
	int ret;

	/*
	 * The backlight core calls us at registration time, before the panel
	 * is powered and before the DSI host is up. Any DSI write then just
	 * burns a 200 ms DMA timeout (the 0x51 timeout seen after a failed
	 * init). The brightness is applied by drm_panel_enable() ->
	 * backlight_enable() once the panel has been prepared.
	 */
	if (!ctx->prepared)
		return 0;

	/* TEST: no DSI command DMA while video is running */
	return 0;

	/*
	 * Not inverted: the vendor HBM commands are 51 07 ff (normal max) and
	 * 51 0f ff (HBM), i.e. a bigger DBV is brighter. The vendor
	 * "bl-inverted-dbv" flag only swaps the two bytes for the vendor DCS
	 * helper, which the *_large() helper below already sends MSB first.
	 * Sent in LP mode: an HS write right as the video stream starts leaves
	 * DSI data lane 0 in a permanent PHY error state (dsi_err_worker
	 * status=4 flood, DPU vblank timeouts, black panel).
	 */
	dsi->mode_flags |= MIPI_DSI_MODE_LPM;
	ret = mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
	dsi->mode_flags = old_flags;

	return ret;
}

static const struct backlight_ops nt37706_bl_ops = {
	.update_status = nt37706_bl_update_status,
};

static struct backlight_device *nt37706_create_backlight(struct nt37706_panel *ctx)
{
	struct device *dev = &ctx->dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 307,
		.max_brightness = 4095,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, ctx,
					      &nt37706_bl_ops, &props);
}

static int nt37706_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct nt37706_panel *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct nt37706_panel, panel,
				   &nt37706_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	memcpy(ctx->supplies, nt37706_supplies, sizeof(nt37706_supplies));
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(nt37706_supplies),
				      ctx->supplies);
	if (ret)
		return ret;

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset GPIO\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB101010;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_DSC_ALL_SLICES_IN_PKT;

	/* This panel only supports DSC; unconditionally enable it. */
	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 2;
	ctx->dsc.slice_height = 12;
	ctx->dsc.slice_width = 610;
	ctx->dsc.slice_count = 2;
	ctx->dsc.bits_per_component = 10;
	ctx->dsc.bits_per_pixel = 8 << 4;
	ctx->dsc.block_pred_enable = true;
	dsi->dsc = &ctx->dsc;

	ctx->panel.prepare_prev_first = true;
	ctx->panel.backlight = nt37706_create_backlight(ctx);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void nt37706_remove(struct mipi_dsi_device *dsi)
{
	struct nt37706_panel *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id nt37706_of_match[] = {
	{ .compatible = "novatek,nt37706" },
	{ }
};
MODULE_DEVICE_TABLE(of, nt37706_of_match);

static struct mipi_dsi_driver nt37706_driver = {
	.probe = nt37706_probe,
	.remove = nt37706_remove,
	.driver = {
		.name = "panel-novatek-nt37706",
		.of_match_table = nt37706_of_match,
	},
};
module_mipi_dsi_driver(nt37706_driver);

MODULE_AUTHOR("ximi <ximiyadka124@proton.me>");
MODULE_DESCRIPTION("Novatek NT37706 AMOLED DSI panel");
MODULE_LICENSE("GPL");

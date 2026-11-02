// SPDX-License-Identifier: GPL-2.0-only
/*
 * FocalTech FT3683G SPI touchscreen driver
 *
 * Bus protocol, register map and touch report layout were derived from the
 * FocalTech downstream driver (Copyright (c) 2012-2020, FocalTech Systems,
 * Ltd., GPL-2.0). This is a rewrite on top of the mainline input, SPI, GPIO
 * and regulator APIs; firmware flashing, gesture/FOD handling and the vendor
 * debug interfaces are intentionally not included.
 *
 * The coordinates reported by the controller are in 1/16 pixel units, so
 * touchscreen-size-x/y in the device tree are expected to be the panel
 * resolution multiplied by 16.
 */

#include <linux/cache.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>

/* Registers */
#define FT_REG_TOUCH_DATA	0x01
#define FT_REG_CHIP_ID_L	0x9F
#define FT_REG_CHIP_ID_H	0xA3
#define FT_REG_POWER_MODE	0xA5
#define FT_REG_FW_VERSION	0xA6

#define FT_POWER_MODE_SLEEP	0x03

/* Application firmware and bootloader chip IDs */
#define FT_CHIP_ID_H		0x56
#define FT_CHIP_ID_L		0x72
#define FT_BL_ID_H		0x36
#define FT_BL_ID_L		0xB3

/* SPI framing: <reg> <cmd> <len_hi> <len_lo> <3 dummy bytes> <payload> [crc] */
#define FT_SPI_CMD_WRITE	0x00
#define FT_SPI_CMD_READ_CRC	0xA0
#define FT_SPI_HDR_LEN		4
#define FT_SPI_DUMMY_LEN	3
#define FT_SPI_CRC_LEN		2
#define FT_SPI_STATUS_IDX	3
#define FT_SPI_STATUS_ERR	0xA0
#define FT_SPI_RETRIES		3
#define FT_SPI_CS_GAP_US	150

/* Touch report (protocol v2): 4 byte header followed by 8 bytes per point */
#define FT_MAX_POINTS		10
#define FT_REPORT_HDR_LEN	4
#define FT_POINT_LEN		8
#define FT_REPORT_LEN		(FT_REPORT_HDR_LEN + FT_MAX_POINTS * FT_POINT_LEN)
#define FT_REPORT_TYPE_V2	0x2

#define FT_EVENT_DOWN		0
#define FT_EVENT_UP		1
#define FT_EVENT_CONTACT	2

#define FT_SPI_BUF_LEN		(FT_SPI_HDR_LEN + FT_SPI_DUMMY_LEN + \
				 FT_REPORT_LEN + FT_SPI_CRC_LEN)

/* Timing */
#define FT_RESET_PULSE_US	2000
#define FT_PRE_RESET_MS		20
#define FT_BOOT_DELAY_MS	200
#define FT_READY_RETRIES	5
#define FT_READY_INTERVAL_MS	200

struct ft3683g {
	struct spi_device *spi;
	struct input_dev *input;
	struct gpio_desc *reset;
	struct regulator *avdd;
	struct regulator *vddio;
	struct touchscreen_properties prop;

	u8 report[FT_REPORT_LEN];

	/* SPI transfer buffers must not share cachelines with other data */
	u8 tx[FT_SPI_BUF_LEN] __aligned(ARCH_DMA_MINALIGN);
	u8 rx[FT_SPI_BUF_LEN] __aligned(ARCH_DMA_MINALIGN);
};

/* ------------------------------------------------------------------------ */
/* Bus access                                                               */
/* ------------------------------------------------------------------------ */

/*
 * CRC-16/KERMIT (reflected, poly 0x8408, init 0xffff) as used by the
 * controller to protect the read payloads.  The controller appends the CRC
 * least significant byte first.
 */
static u16 ft_crc16_kermit(const u8 *data, size_t len)
{
	u16 crc = 0xffff;
	size_t i;
	int j;

	for (i = 0; i < len; i++) {
		crc ^= data[i];

		for (j = 0; j < 8; j++)
			crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
	}

	return crc;
}

static int ft_xfer(struct ft3683g *ts, unsigned int len)
{
	struct spi_transfer xfer = {
		.tx_buf = ts->tx,
		.rx_buf = ts->rx,
		.len = len,
	};
	int error;

	error = spi_sync_transfer(ts->spi, &xfer, 1);

	/* the controller needs CS to stay deasserted for a while */
	usleep_range(FT_SPI_CS_GAP_US, FT_SPI_CS_GAP_US + 50);

	return error;
}

static int ft_read(struct ft3683g *ts, u8 reg, u8 *data, u16 len)
{
	const unsigned int total = FT_SPI_HDR_LEN + FT_SPI_DUMMY_LEN + len +
				   FT_SPI_CRC_LEN;
	const u8 *payload = ts->rx + FT_SPI_HDR_LEN + FT_SPI_DUMMY_LEN;
	int error = -EIO;
	int i;

	if (!len || len > FT_REPORT_LEN)
		return -EINVAL;

	for (i = 0; i < FT_SPI_RETRIES; i++) {
		memset(ts->tx, 0, total);
		ts->tx[0] = reg;
		ts->tx[1] = FT_SPI_CMD_READ_CRC;
		put_unaligned_be16(len, &ts->tx[2]);

		error = ft_xfer(ts, total);
		if (error)
			continue;

		if (ts->rx[FT_SPI_STATUS_IDX] & FT_SPI_STATUS_ERR) {
			error = -EIO;
			continue;
		}

		if (ft_crc16_kermit(payload, len) !=
		    get_unaligned_le16(payload + len)) {
			error = -EILSEQ;
			continue;
		}

		memcpy(data, payload, len);
		return 0;
	}

	dev_dbg_ratelimited(&ts->spi->dev, "read of reg 0x%02x failed: %d\n",
			    reg, error);
	return error;
}

static int ft_read_reg(struct ft3683g *ts, u8 reg, u8 *val)
{
	return ft_read(ts, reg, val, 1);
}

static int ft_write_reg(struct ft3683g *ts, u8 reg, u8 val)
{
	const unsigned int total = FT_SPI_HDR_LEN + FT_SPI_DUMMY_LEN + 1;
	int error = -EIO;
	int i;

	for (i = 0; i < FT_SPI_RETRIES; i++) {
		memset(ts->tx, 0, total);
		ts->tx[0] = reg;
		ts->tx[1] = FT_SPI_CMD_WRITE;
		put_unaligned_be16(1, &ts->tx[2]);
		ts->tx[FT_SPI_HDR_LEN + FT_SPI_DUMMY_LEN] = val;

		error = ft_xfer(ts, total);
		if (error)
			continue;

		if (!(ts->rx[FT_SPI_STATUS_IDX] & FT_SPI_STATUS_ERR))
			return 0;

		error = -EIO;
	}

	dev_dbg_ratelimited(&ts->spi->dev, "write of reg 0x%02x failed: %d\n",
			    reg, error);
	return error;
}

/* ------------------------------------------------------------------------ */
/* Power and reset                                                          */
/* ------------------------------------------------------------------------ */

static int ft_power_on(struct ft3683g *ts)
{
	struct device *dev = &ts->spi->dev;
	int error;

	gpiod_set_value_cansleep(ts->reset, 1);

	error = regulator_enable(ts->avdd);
	if (error) {
		dev_err(dev, "failed to enable avdd: %d\n", error);
		return error;
	}

	error = regulator_enable(ts->vddio);
	if (error) {
		dev_err(dev, "failed to enable vddio: %d\n", error);
		regulator_disable(ts->avdd);
		return error;
	}

	usleep_range(FT_RESET_PULSE_US, FT_RESET_PULSE_US + 1000);
	gpiod_set_value_cansleep(ts->reset, 0);
	msleep(FT_BOOT_DELAY_MS);

	return 0;
}

static void ft_power_off(struct ft3683g *ts)
{
	/* let the firmware prepare for reset, then hold the chip in reset */
	ft_write_reg(ts, FT_REG_POWER_MODE, FT_POWER_MODE_SLEEP);
	msleep(FT_PRE_RESET_MS);

	gpiod_set_value_cansleep(ts->reset, 1);
	usleep_range(FT_RESET_PULSE_US, FT_RESET_PULSE_US + 1000);

	regulator_disable(ts->vddio);
	regulator_disable(ts->avdd);
}

static void ft_power_off_action(void *data)
{
	ft_power_off(data);
}

static int ft_wait_ready(struct ft3683g *ts)
{
	struct device *dev = &ts->spi->dev;
	u8 id_h = 0, id_l = 0;
	int i;

	for (i = 0; i < FT_READY_RETRIES; i++) {
		if (!ft_read_reg(ts, FT_REG_CHIP_ID_H, &id_h) &&
		    !ft_read_reg(ts, FT_REG_CHIP_ID_L, &id_l)) {
			if (id_h == FT_CHIP_ID_H && id_l == FT_CHIP_ID_L)
				return 0;

			if (id_h == FT_BL_ID_H && id_l == FT_BL_ID_L) {
				dev_err(dev, "controller is in bootloader mode, no firmware loaded\n");
				return -ENODEV;
			}
		}

		msleep(FT_READY_INTERVAL_MS);
	}

	dev_err(dev, "controller not ready or wrong chip ID (0x%02x%02x)\n",
		id_h, id_l);
	return -ENODEV;
}

/* ------------------------------------------------------------------------ */
/* Touch reporting                                                          */
/* ------------------------------------------------------------------------ */

static void ft_report_points(struct ft3683g *ts, unsigned int num_points)
{
	struct input_dev *input = ts->input;
	unsigned int i;

	for (i = 0; i < num_points; i++) {
		const u8 *p = &ts->report[FT_REPORT_HDR_LEN + i * FT_POINT_LEN];
		unsigned int event = p[0] >> 6;
		unsigned int id = p[2] >> 4;
		unsigned int x, y;

		if (id >= FT_MAX_POINTS)
			continue;

		if (event != FT_EVENT_DOWN && event != FT_EVENT_CONTACT)
			continue;	/* slot is dropped by input_mt_sync_frame() */

		x = ((p[0] & 0x0f) << 12) | (p[1] << 4) | (p[4] >> 4);
		y = ((p[2] & 0x0f) << 12) | (p[3] << 4) | (p[4] & 0x0f);

		input_mt_slot(input, id);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, true);
		touchscreen_report_pos(input, &ts->prop, x, y, true);
		input_report_abs(input, ABS_MT_TOUCH_MAJOR, p[5]);
		input_report_abs(input, ABS_MT_TOUCH_MINOR, p[6]);
	}

	input_mt_sync_frame(input);
	input_sync(input);
}

static irqreturn_t ft_irq_thread(int irq, void *data)
{
	struct ft3683g *ts = data;
	const u8 *r = ts->report;
	unsigned int type, num_points;

	if (ft_read(ts, FT_REG_TOUCH_DATA, ts->report, FT_REPORT_LEN)) {
		/*
		 * The interrupt is level triggered, so it stays pending until
		 * the report has been read.  Throttle a bit in case the
		 * controller keeps the line asserted.
		 */
		msleep(20);
		return IRQ_HANDLED;
	}

	/* all 0xff: firmware has just (re)started, nothing is touching */
	if (r[1] == 0xff && r[2] == 0xff && r[3] == 0xff && r[4] == 0xff) {
		ft_report_points(ts, 0);
		return IRQ_HANDLED;
	}

	type = r[1] >> 4;
	num_points = r[1] & 0x0f;

	if (!num_points) {
		/* nothing is touching, but a contact may have been dropped */
		ft_report_points(ts, 0);
		return IRQ_HANDLED;
	}

	if (type != FT_REPORT_TYPE_V2 || num_points > FT_MAX_POINTS) {
		dev_warn_ratelimited(&ts->spi->dev,
				     "ignoring report type %u, %u points\n",
				     type, num_points);
		return IRQ_HANDLED;
	}

	ft_report_points(ts, num_points);

	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------------ */
/* Probe and PM                                                             */
/* ------------------------------------------------------------------------ */

static int ft3683g_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct input_dev *input;
	struct ft3683g *ts;
	u8 fw_ver;
	int error;

	if (spi->irq <= 0)
		return dev_err_probe(dev, -EINVAL, "no interrupt specified\n");

	if (!device_property_present(dev, "touchscreen-size-x") ||
	    !device_property_present(dev, "touchscreen-size-y"))
		return dev_err_probe(dev, -EINVAL,
				     "touchscreen-size-x/y are required\n");

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	error = spi_setup(spi);
	if (error)
		return dev_err_probe(dev, error, "SPI setup failed\n");

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->spi = spi;
	spi_set_drvdata(spi, ts);

	ts->avdd = devm_regulator_get(dev, "avdd");
	if (IS_ERR(ts->avdd))
		return dev_err_probe(dev, PTR_ERR(ts->avdd),
				     "failed to get avdd\n");

	ts->vddio = devm_regulator_get(dev, "vddio");
	if (IS_ERR(ts->vddio))
		return dev_err_probe(dev, PTR_ERR(ts->vddio),
				     "failed to get vddio\n");

	ts->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset))
		return dev_err_probe(dev, PTR_ERR(ts->reset),
				     "failed to get reset GPIO\n");

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	ts->input = input;
	input->name = "FocalTech FT3683G Touchscreen";
	input->id.bustype = BUS_SPI;

	/*
	 * The controller reports the coordinates in 1/16 pixel units, so the
	 * axis ranges come straight from touchscreen-size-x/y.
	 */
	touchscreen_parse_properties(input, true, &ts->prop);

	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MINOR, 0, 255, 0, 0);

	error = input_mt_init_slots(input, FT_MAX_POINTS,
				    INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (error)
		return dev_err_probe(dev, error, "failed to init MT slots\n");

	error = ft_power_on(ts);
	if (error)
		return error;

	error = devm_add_action_or_reset(dev, ft_power_off_action, ts);
	if (error)
		return error;

	error = ft_wait_ready(ts);
	if (error)
		return error;

	if (!ft_read_reg(ts, FT_REG_FW_VERSION, &fw_ver))
		dev_info(dev, "FT3683G firmware version 0x%02x\n", fw_ver);

	error = input_register_device(input);
	if (error)
		return dev_err_probe(dev, error,
				     "failed to register input device\n");

	error = devm_request_threaded_irq(dev, spi->irq, NULL, ft_irq_thread,
					  IRQF_ONESHOT, dev_name(dev), ts);
	if (error)
		return dev_err_probe(dev, error, "failed to request IRQ\n");

	return 0;
}

static int ft3683g_suspend(struct device *dev)
{
	struct ft3683g *ts = dev_get_drvdata(dev);

	disable_irq(ts->spi->irq);

	/* ft_power_off() puts the controller to sleep and drops the rails */
	ft_power_off(ts);

	/* lift all fingers */
	ft_report_points(ts, 0);

	return 0;
}

static int ft3683g_resume(struct device *dev)
{
	struct ft3683g *ts = dev_get_drvdata(dev);
	int error;

	error = ft_power_on(ts);
	if (error)
		return error;

	error = ft_wait_ready(ts);
	if (error)
		return error;

	enable_irq(ts->spi->irq);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(ft3683g_pm_ops, ft3683g_suspend,
				ft3683g_resume);

static const struct of_device_id ft3683g_of_match[] = {
	{ .compatible = "focaltech,ft3683g" },
	{ }
};
MODULE_DEVICE_TABLE(of, ft3683g_of_match);

static const struct spi_device_id ft3683g_spi_id[] = {
	{ "ft3683g" },
	{ }
};
MODULE_DEVICE_TABLE(spi, ft3683g_spi_id);

static struct spi_driver ft3683g_driver = {
	.driver = {
		.name = "focaltech-ft3683g",
		.of_match_table = ft3683g_of_match,
		.pm = pm_sleep_ptr(&ft3683g_pm_ops),
	},
	.id_table = ft3683g_spi_id,
	.probe = ft3683g_probe,
};
module_spi_driver(ft3683g_driver);

MODULE_DESCRIPTION("FocalTech FT3683G SPI touchscreen driver");
MODULE_LICENSE("GPL");

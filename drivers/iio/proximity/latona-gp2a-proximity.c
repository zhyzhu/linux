// SPDX-License-Identifier: GPL-2.0-only
/* Latona GP2AP002A00F: bounded, on-demand normal-mode proximity reads. */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/iio/consumer.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#define GP2A_PROX 0
#define GP2A_GAIN 1
#define GP2A_HYS 2
#define GP2A_CYCLE 3
#define GP2A_OPMOD 4
#define GP2A_CON 6

struct latona_prox {
	struct i2c_client *client;
	struct regulator_bulk_data supplies[2];
	struct mutex lock;
};

/* Do not gate shared rails or the GPIO28 enable owned by the ALS supplier. */
static int latona_prox_standby(struct latona_prox *prox)
{
	int ret, second;

	ret = i2c_smbus_write_byte_data(prox->client, GP2A_OPMOD, 0);
	second = i2c_smbus_write_byte_data(prox->client, GP2A_CON, 0x18);
	if (ret < 0 || second < 0)
		dev_err(&prox->client->dev, "failed to stop proximity measurement\n");
	return ret < 0 ? ret : second;
}

static void latona_prox_release(void *data)
{
	struct latona_prox *prox = data;

	mutex_lock(&prox->lock);
	latona_prox_standby(prox);
	regulator_bulk_disable(ARRAY_SIZE(prox->supplies), prox->supplies);
	mutex_unlock(&prox->lock);
}

static int latona_prox_read_raw(struct iio_dev *indio,
		const struct iio_chan_spec *chan, int *val, int *val2, long mask)
{
	struct latona_prox *prox = iio_priv(indio);
	static const u8 setup[][2] = {
		{ GP2A_GAIN, 0x08 }, { GP2A_HYS, 0xc2 },
		{ GP2A_CYCLE, 0x04 }, { GP2A_OPMOD, 0x01 },
	};
	int ret, cleanup, i;

	if (mask != IIO_CHAN_INFO_RAW)
		return -EINVAL;
	mutex_lock(&prox->lock);
	/* Same normal mode already verified by the bounded RAM test. */
	ret = latona_prox_standby(prox);
	if (ret < 0)
		goto out;
	for (i = 0; i < ARRAY_SIZE(setup); i++) {
		ret = i2c_smbus_write_byte_data(prox->client, setup[i][0], setup[i][1]);
		if (ret < 0)
			goto out;
	}
	/* Fresh measurement; no persistent IR operation between sysfs reads. */
	msleep(100);
	ret = i2c_smbus_read_word_data(prox->client, GP2A_PROX);
	if (ret >= 0) {
		*val = (ret >> 8) & 1;
		ret = IIO_VAL_INT;
	}
out:
	cleanup = latona_prox_standby(prox);
	if (cleanup < 0)
		ret = cleanup;
	mutex_unlock(&prox->lock);
	return ret;
}

static const struct iio_info latona_prox_info = {
	.read_raw = latona_prox_read_raw,
};

static const struct iio_chan_spec latona_prox_channel = {
	.type = IIO_PROXIMITY,
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
};

static int latona_prox_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct iio_channel *ambient;
	struct iio_dev *indio;
	struct latona_prox *prox;
	int ret, i;
	static const int expected_uv[] = { 3000000, 2800000 };

	if (!i2c_check_functionality(client->adapter,
		I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA))
		return -EOPNOTSUPP;
	/* Defer until the existing ALS consumer owns the actual GPIO28 pad. */
	ambient = devm_iio_channel_get(dev, "ambient");
	if (IS_ERR(ambient))
		return dev_err_probe(dev, PTR_ERR(ambient), "ALS supplier unavailable\n");
	if (!device_link_add(dev, ambient->indio_dev->dev.parent,
			     DL_FLAG_AUTOREMOVE_CONSUMER))
		return -ENOMEM;
	indio = devm_iio_device_alloc(dev, sizeof(*prox));
	if (!indio)
		return -ENOMEM;
	prox = iio_priv(indio);
	prox->client = client;
	mutex_init(&prox->lock);
	prox->supplies[0].supply = "analog";
	prox->supplies[1].supply = "vio";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(prox->supplies), prox->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "sensor supplies unavailable\n");
	for (i = 0; i < ARRAY_SIZE(expected_uv); i++) {
		ret = regulator_get_voltage(prox->supplies[i].consumer);
		if (ret != expected_uv[i])
			return dev_err_probe(dev, ret < 0 ? ret : -EINVAL,
					     "sensor rail voltage mismatch\n");
	}
	ret = regulator_bulk_enable(ARRAY_SIZE(prox->supplies), prox->supplies);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, latona_prox_release, prox);
	if (ret)
		return ret;
	ret = latona_prox_standby(prox);
	if (ret < 0)
		return ret;
	indio->name = "latona-gp2a-proximity";
	indio->info = &latona_prox_info;
	indio->modes = INDIO_DIRECT_MODE;
	indio->channels = &latona_prox_channel;
	indio->num_channels = 1;
	i2c_set_clientdata(client, prox);
	ret = devm_iio_device_register(dev, indio);
	if (ret)
		return ret;
	dev_info(dev, "Latona GP2A proximity ready; on-demand reads, default standby\n");
	return 0;
}

static void latona_prox_shutdown(struct i2c_client *client)
{
	struct latona_prox *prox = i2c_get_clientdata(client);

	mutex_lock(&prox->lock);
	latona_prox_standby(prox);
	mutex_unlock(&prox->lock);
}

static const struct of_device_id latona_prox_match[] = {
	{ .compatible = "samsung,latona-gp2a-proximity" }, {}
};
MODULE_DEVICE_TABLE(of, latona_prox_match);
static struct i2c_driver latona_prox_driver = {
	.driver = { .name = "latona-gp2a-proximity", .of_match_table = latona_prox_match },
	.probe = latona_prox_probe,
	.shutdown = latona_prox_shutdown,
};
module_i2c_driver(latona_prox_driver);
MODULE_DESCRIPTION("Latona GP2AP002A00F on-demand proximity IIO");
MODULE_LICENSE("GPL");

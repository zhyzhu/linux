// SPDX-License-Identifier: GPL-2.0-only
/* Latona GP2A analog ALS: vendor ADC4 wiring and approximate board lux table. */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/iio/consumer.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
struct latona_als { struct iio_channel *adc; struct regulator_bulk_data supplies[2]; struct gpio_desc *enable; bool powered; };
static const int table[][2] = {
 {2,8},{5,10},{8,45},{10,69},{12,79},{22,102},{30,159},{50,196},
 {75,200},{110,245},{150,295},{170,307},{300,326},{460,378},
 {1000,463},{1500,500},{1700,515},{2000,526},{2850,561},{3450,570},
 {6000,603},{11000,648}
};
static int estimate_lux(int raw)
{
 int i;
 if(raw<=table[0][1])return table[0][0]*raw/table[0][1];
 for(i=1;i<ARRAY_SIZE(table);i++)if(raw<table[i][1])
  return table[i-1][0]+(raw-table[i-1][1])*(table[i][0]-table[i-1][0])/(table[i][1]-table[i-1][1]);
 return table[ARRAY_SIZE(table)-1][0];
}
static int latona_als_read(struct iio_dev *indio, const struct iio_chan_spec *chan,
 int *val,int *val2,long mask)
{
 struct latona_als *als=iio_priv(indio);int raw,ret;
 ret=iio_read_channel_raw(als->adc,&raw);if(ret<0)return ret;
 if(raw<0||raw>1023)return -ERANGE;
 if(mask==IIO_CHAN_INFO_RAW)*val=raw;
 else if(mask==IIO_CHAN_INFO_PROCESSED)*val=estimate_lux(raw);
 else return -EINVAL;
 return IIO_VAL_INT;
}
static const struct iio_info latona_als_info={.read_raw=latona_als_read};
static const struct iio_chan_spec latona_als_chan={
 .type=IIO_LIGHT,.info_mask_separate=BIT(IIO_CHAN_INFO_RAW)|BIT(IIO_CHAN_INFO_PROCESSED)
};
static void latona_als_off(void *ptr)
{
 struct latona_als *als=ptr;
 if(!als->powered)return;
 gpiod_set_value_cansleep(als->enable,0);
 regulator_bulk_disable(ARRAY_SIZE(als->supplies),als->supplies);als->powered=false;
}
static int latona_als_probe(struct platform_device *pdev)
{
 struct device *dev=&pdev->dev;struct iio_dev *indio;struct latona_als *als;enum iio_chan_type type;int ret;
 indio=devm_iio_device_alloc(dev,sizeof(*als));if(!indio)return -ENOMEM;
 als=iio_priv(indio);
 als->adc=devm_iio_channel_get(dev,"alsout");if(IS_ERR(als->adc))return dev_err_probe(dev,PTR_ERR(als->adc),"ADC4 unavailable\n");
 ret=iio_get_channel_type(als->adc,&type);if(ret<0)return ret;if(type!=IIO_VOLTAGE)return -EINVAL;
 /* Actual ZCKPL gp2a.ko pl_sensor_power_on uses VAUX1 and VAUX2. */
 als->supplies[0].supply="analog";als->supplies[1].supply="vio";
 ret=devm_regulator_bulk_get(dev,ARRAY_SIZE(als->supplies),als->supplies);
 if(ret)return dev_err_probe(dev,ret,"sensor rails unavailable\n");
 ret=regulator_get_voltage(als->supplies[0].consumer);
 if(ret!=3000000)return dev_err_probe(dev,ret<0?ret:-EINVAL,"expected VAUX1 3.0V\n");
 ret=regulator_get_voltage(als->supplies[1].consumer);
 if(ret!=2800000)return dev_err_probe(dev,ret<0?ret:-EINVAL,"expected VAUX2 2.8V\n");
 als->enable=devm_gpiod_get(dev,"enable",GPIOD_OUT_LOW);if(IS_ERR(als->enable))return dev_err_probe(dev,PTR_ERR(als->enable),"ALS enable unavailable\n");
 /* Match the vendor sequence; roll GPIO back if rail enable fails. */
 gpiod_set_value_cansleep(als->enable,1);
 ret=regulator_bulk_enable(ARRAY_SIZE(als->supplies),als->supplies);
 if(ret){gpiod_set_value_cansleep(als->enable,0);return ret;}
 als->powered=true;
 ret=devm_add_action_or_reset(dev,latona_als_off,als);if(ret)return ret;
 msleep(20);
 indio->name="latona-gp2a-als";indio->info=&latona_als_info;indio->modes=INDIO_DIRECT_MODE;indio->channels=&latona_als_chan;indio->num_channels=1;
 platform_set_drvdata(pdev,als);
 ret=devm_iio_device_register(dev,indio);if(ret)return ret;
 dev_info(dev,"Latona GP2A analog ALS ready: VAUX1/VAUX2 ADC4; lux is board-table estimate\n");return 0;
}
static void latona_als_shutdown(struct platform_device *pdev){latona_als_off(platform_get_drvdata(pdev));}
static const struct of_device_id latona_als_of_match[]={{.compatible="samsung,latona-gp2a-als"},{}};
MODULE_DEVICE_TABLE(of,latona_als_of_match);
static struct platform_driver latona_als_driver={.probe=latona_als_probe,.shutdown=latona_als_shutdown,.driver={.name="latona-gp2a-als",.of_match_table=latona_als_of_match}};
module_platform_driver(latona_als_driver);
MODULE_DESCRIPTION("Latona GP2A analog ambient light observer");
MODULE_LICENSE("GPL");

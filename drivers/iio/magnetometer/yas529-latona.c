// SPDX-License-Identifier: GPL-2.0-or-later
/* YAS529 one-shot IIO, protocol/compensation from Yamaha (c)2010 Latona driver. */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include "yas529-latona-math.h"
struct yas529_data {
 struct i2c_client *client;
 struct gpio_desc *reset;
 struct mutex lock;
 int matrix[9],coeff[3];
};
static int yas529_send(struct yas529_data *data,u8 cmd)
{
 int ret=i2c_master_send(data->client,&cmd,1);
 return ret<0?ret:ret==1?0:-EIO;
}
static int yas529_receive(struct yas529_data *data,u8 *buf,int len)
{
 int ret=i2c_master_recv(data->client,buf,len);
 return ret<0?ret:ret==len?0:-EIO;
}
static int yas529_init(struct yas529_data *data)
{
 static const u8 coil[]={0x90,0x81,0x91,0x82,0x92,0x83,0x93,0x84,0x94,0x85,0x95,0x86,0x96,0x87,0x97,0x80};
 static const u8 offset_addr[]={0x20,0x40,0x60};
 u8 cmd=0xd0,id[2],cal[9],rough[6],offset[3];
 struct i2c_msg msgs[]={{.addr=data->client->addr,.len=1,.buf=&cmd},
  {.addr=data->client->addr,.flags=I2C_M_RD,.len=2,.buf=id}};
 bool all_zero=true,all_ones=true;
 int ret,i,value;
 ret=i2c_transfer(data->client->adapter,msgs,ARRAY_SIZE(msgs));
 if(ret<0)return ret;
 if(ret!=ARRAY_SIZE(msgs))return -EIO;
 if(id[1]!=0x40)return -ENODEV;
 ret=yas529_send(data,1);if(ret)return ret;
 usleep_range(2000,3000);
 ret=yas529_send(data,0xc8);if(ret)return ret;
 ret=yas529_receive(data,cal,9);if(ret)return ret; /* Dummy CAL read. */
 ret=yas529_receive(data,cal,9);if(ret)return ret;
 for(i=0;i<9;i++){all_zero&=cal[i]==0;all_ones&=cal[i]==0xff;}
 if(all_zero||all_ones)return -ENODATA;
 yas529_decode_factory(cal,data->matrix,data->coeff);
 for(i=0;i<ARRAY_SIZE(coil);i++){ret=yas529_send(data,coil[i]);if(ret)return ret;}
 ret=yas529_send(data,0xc0);if(ret)return ret;
 ret=yas529_send(data,1);if(ret)return ret;
 usleep_range(2000,3000);
 ret=yas529_receive(data,rough,6);if(ret)return ret;
 if(rough[0]&0x80)return -EBUSY;
 for(i=0;i<3;i++){
  value=((rough[2*i]&7)<<8)|rough[2*i+1];
  if(value<1||value>32)return -ERANGE;
  offset[2-i]=value;
 }
 for(i=0;i<3;i++){
  ret=yas529_send(data,offset_addr[i]|(offset[i]<=5?0:offset[i]-5));
  if(ret)return ret;
 }
 dev_info(&data->client->dev,"YAS529 ID %02x,%02x; factory matrix loaded, rough offsets %u/%u/%u\n",id[0],id[1],offset[0],offset[1],offset[2]);
 return 0;
}
static int yas529_measure(struct yas529_data *data,int xyz[3])
{
 u8 bytes[8];
 int raw[3],temp,ret,i,j;
 ret=yas529_send(data,0xc0);if(ret)return ret;
 ret=yas529_send(data,2);if(ret)return ret;
 for(i=0;i<13;i++){
  usleep_range(1000,1500);
  ret=yas529_receive(data,bytes,8);if(ret)return ret;
  if(!(bytes[0]&0x80)){
   for(j=0;j<3;j++){
    raw[2-j]=((bytes[2*j]&7)<<8)|bytes[2*j+1];
    if(raw[2-j]<=1||raw[2-j]>=1024)return -ERANGE;
   }
   temp=((bytes[6]&7)<<8)|bytes[7];
   yas529_factory_xyz(raw,temp,data->matrix,data->coeff,xyz);
   return 0;
  }
 }
 return -ETIMEDOUT;
}
static int yas529_read_raw(struct iio_dev *indio,const struct iio_chan_spec *chan,
 int *val,int *val2,long mask)
{
 struct yas529_data *data=iio_priv(indio);
 int xyz[3],ret;
 if(mask==IIO_CHAN_INFO_SCALE){*val=0;*val2=4000;return IIO_VAL_INT_PLUS_MICRO;} /* 400nT = .004 gauss. */
 if(mask!=IIO_CHAN_INFO_RAW)return -EINVAL;
 mutex_lock(&data->lock);
 ret=yas529_measure(data,xyz);
 if(!ret)*val=xyz[chan->address];
 mutex_unlock(&data->lock);
 return ret?ret:IIO_VAL_INT;
}
static const struct iio_info yas529_info={.read_raw=yas529_read_raw};
#define YAS529_AXIS(_axis,_index) { .type=IIO_MAGN,.modified=1,.channel2=IIO_MOD_##_axis, \
 .address=_index,.info_mask_separate=BIT(IIO_CHAN_INFO_RAW),.info_mask_shared_by_type=BIT(IIO_CHAN_INFO_SCALE) }
static const struct iio_chan_spec yas529_channels[]={YAS529_AXIS(X,0),YAS529_AXIS(Y,1),YAS529_AXIS(Z,2)};
static int yas529_probe(struct i2c_client *client)
{
 struct iio_dev *indio;
 struct yas529_data *data;
 int ret;
 if(!i2c_check_functionality(client->adapter,I2C_FUNC_I2C))return -EOPNOTSUPP;
 indio=devm_iio_device_alloc(&client->dev,sizeof(*data));if(!indio)return -ENOMEM;
 data=iio_priv(indio);data->client=client;mutex_init(&data->lock);
 /* Active-low GPIO164, exactly the original board's two-millisecond reset. */
 data->reset=devm_gpiod_get(&client->dev,"reset",GPIOD_OUT_HIGH);
 if(IS_ERR(data->reset))return dev_err_probe(&client->dev,PTR_ERR(data->reset),"reset unavailable\n");
 usleep_range(2000,3000);gpiod_set_value_cansleep(data->reset,0);msleep(10);
 ret=yas529_init(data);if(ret)return dev_err_probe(&client->dev,ret,"initialization failed\n");
 indio->name="yas529";indio->info=&yas529_info;indio->modes=INDIO_DIRECT_MODE;
 indio->channels=yas529_channels;indio->num_channels=ARRAY_SIZE(yas529_channels);
 ret=devm_iio_device_register(&client->dev,indio);if(ret)return ret;
 dev_info(&client->dev,"YAS529 IIO ready; one-shot factory-compensated sensor axes\n");
 return 0;
}
static const struct of_device_id yas529_match[]={{.compatible="yamaha,yas529"},{}};
MODULE_DEVICE_TABLE(of,yas529_match);
static struct i2c_driver yas529_driver={.driver={.name="yas529-latona",.of_match_table=yas529_match},.probe=yas529_probe};
module_i2c_driver(yas529_driver);
MODULE_AUTHOR("Latona port; based on Yamaha Corporation 2010 driver");
MODULE_DESCRIPTION("YAS529 factory-compensated one-shot IIO magnetometer");
MODULE_LICENSE("GPL");

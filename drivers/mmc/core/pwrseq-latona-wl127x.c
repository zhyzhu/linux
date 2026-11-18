// SPDX-License-Identifier: GPL-2.0-only
/* Latona WL127x VIO bootstrap, matching the fixed legacy board sequence. */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/ktime.h>
#include <linux/gpio/consumer.h>
#include <linux/mmc/host.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/rfkill.h>
#include "pwrseq.h"
struct latona_wlan_pwrseq { struct mmc_pwrseq pwrseq; struct gpio_desc *bt_shutdown; struct gpio_desc *wlan_enable; unsigned int power_ons; };
/* GPIO63 stays exclusively owned here; rfkill does not touch WLAN_EN. */
static int latona_bt_set_block(void *data, bool blocked)
{
 struct latona_wlan_pwrseq *p=data;
 gpiod_set_value_cansleep(p->bt_shutdown,!blocked);
 return 0;
}
static const struct rfkill_ops latona_bt_rfkill_ops={.set_block=latona_bt_set_block};
static void latona_bt_rfkill_release(void *data)
{
 struct rfkill *rfk=data;
 rfkill_unregister(rfk);
 rfkill_destroy(rfk);
}
static void latona_wlan_pre_power_on(struct mmc_host *host)
{
 struct latona_wlan_pwrseq *p = container_of(host->pwrseq, struct latona_wlan_pwrseq, pwrseq);
 ktime_t t0, t1, t2;
 gpiod_set_value_cansleep(p->wlan_enable, 1);
 t0 = ktime_get();
 usleep_range(15000, 16000);
 gpiod_set_value_cansleep(p->wlan_enable, 0);
 t1 = ktime_get();
 usleep_range(1000, 1100);
 gpiod_set_value_cansleep(p->wlan_enable, 1);
 t2 = ktime_get();
 msleep(70);
 dev_info(p->pwrseq.dev, "Latona WLAN_EN stock pulse high15 low1 high70 complete\n");
 dev_dbg(p->pwrseq.dev, "Latona WLAN pulse high15/low1 uses usleep_range\n");
 dev_info(p->pwrseq.dev,
          "Latona WLAN pulse count=%u high_us=%lld low_us=%lld settle_us=%lld\n",
          ++p->power_ons, (long long)ktime_us_delta(t1, t0),
          (long long)ktime_us_delta(t2, t1),
          (long long)ktime_us_delta(ktime_get(), t2));
}
static void latona_wlan_power_off(struct mmc_host *host)
{
 struct latona_wlan_pwrseq *p = container_of(host->pwrseq, struct latona_wlan_pwrseq, pwrseq);
 gpiod_set_value_cansleep(p->wlan_enable, 0);
 msleep(10);
}
static const struct mmc_pwrseq_ops latona_wlan_ops = {
 .pre_power_on = latona_wlan_pre_power_on,
 .power_off = latona_wlan_power_off,
};
static void latona_vsim_test_disable(void *data)
{
 regulator_disable(data);
}
static int latona_wlan_pwrseq_probe(struct platform_device *pdev)
{
 struct regulator *vsim;
 struct rfkill *rfk;
 int ret;
 struct latona_wlan_pwrseq *p = devm_kzalloc(&pdev->dev, sizeof(*p), GFP_KERNEL);
 if (!p)
  return -ENOMEM;
 /* Single-rail diagnostic candidate from measured cold/warm difference.
  * Physical WLAN supply routing is not yet established. Never raise voltage.
  */
 vsim = devm_regulator_get(&pdev->dev, "vsim-test");
 if (IS_ERR(vsim))
  return dev_err_probe(&pdev->dev, PTR_ERR(vsim), "VSIM test supply unavailable\n");
 ret = regulator_get_voltage(vsim);
 if (ret != 1800000)
  return dev_err_probe(&pdev->dev, ret < 0 ? ret : -EINVAL,
                       "VSIM test requires existing 1800000uV\n");
 ret = regulator_enable(vsim);
 if (ret)
  return dev_err_probe(&pdev->dev, ret, "VSIM test enable failed\n");
 ret = devm_add_action_or_reset(&pdev->dev, latona_vsim_test_disable, vsim);
 if (ret)
  return ret;
 dev_info(&pdev->dev, "Latona candidate VSIM existing1.8V enabled before bootstrap\n");
 p->bt_shutdown = devm_gpiod_get(&pdev->dev, "bt-shutdown", GPIOD_OUT_LOW);
 if (IS_ERR(p->bt_shutdown))
  return dev_err_probe(&pdev->dev, PTR_ERR(p->bt_shutdown), "BT shutdown GPIO unavailable\n");
 p->wlan_enable = devm_gpiod_get(&pdev->dev, "wlan-enable", GPIOD_OUT_LOW);
 if (IS_ERR(p->wlan_enable))
  return dev_err_probe(&pdev->dev, PTR_ERR(p->wlan_enable), "WLAN enable GPIO unavailable\n");
 /* Board-level leakage workaround runs once per provider probe, not
  * every MMC power cycle. This is not a claim of identical legacy order.
  */
 gpiod_set_value_cansleep(p->bt_shutdown, 1);
 msleep(10);
 gpiod_set_value_cansleep(p->bt_shutdown, 0);
 usleep_range(64, 100);
 dev_info(&pdev->dev, "Latona WL127x bootstrap pulse complete; BT shutdown low\n");
 dev_info(&pdev->dev, "Latona BT bootstrap once at provider probe\n");
 /* b99 UART diagnostics: default blocked, same GPIO owner as bootstrap. */
 if(IS_ENABLED(CONFIG_RFKILL)) {
  rfk=rfkill_alloc("latona-bluetooth",&pdev->dev,RFKILL_TYPE_BLUETOOTH,
                   &latona_bt_rfkill_ops,p);
  if(!rfk)return -ENOMEM;
  rfkill_init_sw_state(rfk,true);
  ret=rfkill_register(rfk);
  if(ret){rfkill_destroy(rfk);return ret;}
  ret=devm_add_action_or_reset(&pdev->dev,latona_bt_rfkill_release,rfk);
  if(ret)return ret;
  dev_info(&pdev->dev,"Latona Bluetooth rfkill ready, default blocked\n");
 }
 p->pwrseq.dev = &pdev->dev;
 p->pwrseq.ops = &latona_wlan_ops;
 p->pwrseq.owner = THIS_MODULE;
 platform_set_drvdata(pdev, p);
 return mmc_pwrseq_register(&p->pwrseq);
}
static void latona_wlan_pwrseq_remove(struct platform_device *pdev)
{
 struct latona_wlan_pwrseq *p = platform_get_drvdata(pdev);
 mmc_pwrseq_unregister(&p->pwrseq);
 gpiod_set_value_cansleep(p->wlan_enable, 0);
 gpiod_set_value_cansleep(p->bt_shutdown, 0);
}
static const struct of_device_id latona_wlan_pwrseq_of_match[] = {
 { .compatible = "samsung,latona-wl127x-pwrseq" }, { }
};
MODULE_DEVICE_TABLE(of, latona_wlan_pwrseq_of_match);
static struct platform_driver latona_wlan_pwrseq_driver = {
 .probe = latona_wlan_pwrseq_probe,
 .remove_new = latona_wlan_pwrseq_remove,
 .driver = { .name = "pwrseq-latona-wl127x", .of_match_table = latona_wlan_pwrseq_of_match },
};
module_platform_driver(latona_wlan_pwrseq_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Latona WL127x legacy VIO bootstrap power sequence");

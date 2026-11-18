// SPDX-License-Identifier: GPL-2.0-only
/* Latona Sony panel: brightness-only SPI, preserving bootloader display setup. */
#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <drm/drm_panel.h>
#include <drm/drm_modes.h>
#include <drm/drm_connector.h>
#include <linux/media-bus-format.h>
struct latona_bl { struct drm_panel panel; struct spi_device *spi; struct gpio_desc *reset; struct gpio_desc *bl_enable; struct regulator_bulk_data rails[2]; struct pinctrl *pins; struct pinctrl_state *default_pins, *safe_pins; bool drm_owned, drm_enabled; bool power_ready; bool rail_owned[2]; int applied; };
static int latona_bl_word(struct spi_device *spi, u16 word)
{
 int ret = spi_write(spi, &word, sizeof(word));
 usleep_range(200, 300);
 return ret;
}
static int latona_bl_update(struct backlight_device *bd)
{
 struct latona_bl *bl = bl_get_data(bd);
 int level = backlight_get_brightness(bd), ret;
 if (bl->drm_owned && !bl->drm_enabled)
  level = 0;
 if (!bl->power_ready)
  return -EIO;
 ret = latona_bl_word(bl->spi, 0x51);
 if (!ret)
  ret = latona_bl_word(bl->spi, 0x100 | level);
 if (ret) {
  bl->applied = -ENODATA;
  return ret;
 }
 bl->applied = level;
 dev_info(&bl->spi->dev, "brightness command applied: %d\n", level);
 return 0;
}
static int latona_bl_get(struct backlight_device *bd)
{
 struct latona_bl *bl = bl_get_data(bd);
 return bl->applied;
}
static const struct backlight_ops latona_bl_ops = {
 .update_status = latona_bl_update,
 .get_brightness = latona_bl_get,
};
/* Caller holds backlight update_lock and controller runtime PM reference. */
static int latona_bl_cycle_rails(struct latona_bl *bl)
{
 bool vci_off = false, vddio_off = false;
 int ret, recover;
 gpiod_set_value_cansleep(bl->bl_enable, 0);
 gpiod_set_value_cansleep(bl->reset, 1);
 bl->power_ready = false;
 ret = pinctrl_select_state(bl->pins, bl->safe_pins);
 if (ret)
  goto restore;
 ret = regulator_disable(bl->rails[1].consumer);
 if (ret)
  goto restore;
 vci_off = true;
 bl->rail_owned[1] = false;
 usleep_range(1000, 1500);
 ret = regulator_disable(bl->rails[0].consumer);
 if (ret)
  goto restore;
 vddio_off = true;
 bl->rail_owned[0] = false;
 {
  int io = regulator_is_enabled(bl->rails[0].consumer);
  int ci = regulator_is_enabled(bl->rails[1].consumer);
  dev_info(&bl->spi->dev, "panel rails after disable: vddio=%d vci=%d\n", io, ci);
  if (io != 0 || ci != 0) {
   ret = -EBUSY;
   goto restore;
  }
 }
 msleep(20);
 ret = 0;
restore:
 /* Balance only the references actually dropped; restore IO rail first. */
 if (vddio_off) {
  recover = regulator_enable(bl->rails[0].consumer);
  if (recover) {
   dev_err(&bl->spi->dev, "vddio recovery failed: %d\n", recover);
   return recover;
  }
  bl->rail_owned[0] = true;
  usleep_range(1000, 1500);
 }
 if (vci_off) {
  recover = regulator_enable(bl->rails[1].consumer);
  if (recover) {
   dev_err(&bl->spi->dev, "vci recovery failed: %d\n", recover);
   return recover;
  }
  bl->rail_owned[1] = true;
  usleep_range(1000, 1500);
 }
 bl->power_ready = regulator_is_enabled(bl->rails[0].consumer) > 0 &&
                   regulator_is_enabled(bl->rails[1].consumer) > 0;
 if (!bl->power_ready)
  return -EIO;
 recover = pinctrl_select_state(bl->pins, bl->default_pins);
 if (recover) {
  bl->power_ready = false; /* Keep reset asserted until pin recovery. */
  return recover;
 }
 dev_info(&bl->spi->dev, "panel rails restored: vddio=1 vci=1\n");
 return ret;
}
/* Bring-up-only manual panel command replay; no reset or regulator changes. */
static ssize_t reinitialize_store(struct device *dev,
                                 struct device_attribute *attr,
                                 const char *buf, size_t count)
{
 struct backlight_device *bd = dev_get_drvdata(dev);
 struct latona_bl *bl = bl_get_data(bd);
 static const u16 setup[] = {
  0x03a, 0x177,
  0x03b, 0x107, 0x10a, 0x10e, 0x10a, 0x10a,
  0x02a, 0x100, 0x100, 0x101, 0x1df,
  0x02b, 0x100, 0x100, 0x103, 0x11f,
  0x036, 0x1d4,
 };
 static const u16 finish[] = {
  0x055, 0x101, 0x05e, 0x100, 0x053, 0x12c, 0x029,
 };
 int ret, level;
 bool cycle = !strcmp(attr->attr.name, "power_cycle");
 bool internal = !strcmp(attr->attr.name, "drm_prepare");
 bool hardware_reset = internal || cycle || !strcmp(attr->attr.name, "reset_reinitialize");
 bool pm_held = false, cycle_started = false;
 int enable_before = 1;
 unsigned int i;
 if (!sysfs_streq(buf, "1"))
  return -EINVAL;
 if (!bl->power_ready)
  return -EIO;
 if (hardware_reset && !bl->reset)
  return -ENODEV;
 if (cycle && (!bl->bl_enable || !bl->pins ||
               gpiod_get_direction(bl->bl_enable) != 0))
  return -EINVAL;
 mutex_lock(&bd->update_lock);
 if (bl->drm_owned && !internal) {
  ret = -EBUSY;
  goto out;
 }
 if (!bl->power_ready) {
  ret = -EIO;
  goto out;
 }
 if (cycle) {
  if (!bl->power_ready) {
   ret = -EIO;
   goto out;
  }
  bl->applied = -ENODATA;
  ret = pm_runtime_resume_and_get(bl->spi->controller->dev.parent);
  if (ret < 0)
   goto out;
  pm_held = true;
  enable_before = gpiod_get_raw_value_cansleep(bl->bl_enable);
  if (enable_before < 0) {
   ret = enable_before;
   goto out;
  }
  ret = latona_bl_word(bl->spi, 0x28);
  if (ret)
   goto out;
  msleep(25);
  ret = latona_bl_word(bl->spi, 0x10);
  if (ret)
   goto out;
  msleep(150);
  cycle_started = true;
  ret = latona_bl_cycle_rails(bl);
  if (ret)
   goto out;
 }
 level = backlight_get_brightness(bd);
 bl->applied = -ENODATA;
 dev_info(dev, "manual Sony panel replay begin, target brightness %d\n", level);
 if (hardware_reset) {
  ret = gpiod_direction_output(bl->reset, 0); /* inactive = physical high */
  if (ret)
   goto out;
  gpiod_set_value_cansleep(bl->reset, 1); /* active-low reset */
  usleep_range(1000, 1500);
  gpiod_set_value_cansleep(bl->reset, 0);
  msleep(5);
  dev_info(dev, "hardware panel reset pulse complete\n");
 } else {
  ret = latona_bl_word(bl->spi, 0x28);
  if (ret)
   goto out;
  msleep(25);
  ret = latona_bl_word(bl->spi, 0x10);
  if (ret)
   goto out;
  msleep(150);
 }
 msleep(145);
 for (i = 0; i < ARRAY_SIZE(setup); i++) {
  ret = latona_bl_word(bl->spi, setup[i]);
  if (ret)
   goto out;
 }
 msleep(25);
 ret = latona_bl_word(bl->spi, 0x11);
 if (ret)
  goto out;
 msleep(200);
 ret = latona_bl_word(bl->spi, 0x51);
 if (!ret)
  ret = latona_bl_word(bl->spi, 0x100); /* vendor sequence starts dark */
 if (ret)
  goto out;
 for (i = 0; i < ARRAY_SIZE(finish); i++) {
  ret = latona_bl_word(bl->spi, finish[i]);
  if (ret)
   goto out;
 }
 if (cycle)
  gpiod_set_value_cansleep(bl->bl_enable, enable_before);
 ret = latona_bl_update(bd);
out:
 if (cycle_started && bl->power_ready) {
  gpiod_set_value_cansleep(bl->reset, 0);
  gpiod_set_value_cansleep(bl->bl_enable, enable_before);
 }
 if (pm_held) {
  pm_runtime_mark_last_busy(bl->spi->controller->dev.parent);
  pm_runtime_put_autosuspend(bl->spi->controller->dev.parent);
 }
 dev_info(dev, "manual Sony panel replay finished: %d\n", ret);
 mutex_unlock(&bd->update_lock);
 return ret ? ret : count;
}
static DEVICE_ATTR_WO(reinitialize);
static ssize_t reset_reinitialize_store(struct device *dev,
                                      struct device_attribute *attr,
                                      const char *buf, size_t count)
{
 return reinitialize_store(dev, attr, buf, count);
}
static DEVICE_ATTR_WO(reset_reinitialize);
static ssize_t power_cycle_store(struct device *dev,
                                struct device_attribute *attr,
                                const char *buf, size_t count)
{
 return reinitialize_store(dev, attr, buf, count);
}
static DEVICE_ATTR_WO(power_cycle);
static struct attribute *latona_bl_attrs[] = { &dev_attr_reinitialize.attr, &dev_attr_reset_reinitialize.attr, &dev_attr_power_cycle.attr, NULL };
ATTRIBUTE_GROUPS(latona_bl);
static void latona_bl_release_rails(void *data)
{
 struct latona_bl *bl = data;
 if (bl->rail_owned[1])
  regulator_disable(bl->rails[1].consumer);
 if (bl->rail_owned[0])
  regulator_disable(bl->rails[0].consumer);
}
static const struct drm_display_mode latona_sony_mode = {
 .clock = 20571,
 .hdisplay = 480, .hsync_start = 490, .hsync_end = 500, .htotal = 510,
 .vdisplay = 800, .vsync_start = 804, .vsync_end = 806, .vtotal = 815,
 .flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
 .type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};
static int latona_panel_get_modes(struct drm_panel *panel,
                                 struct drm_connector *connector)
{
 struct drm_display_mode *mode = drm_mode_duplicate(connector->dev, &latona_sony_mode);
 if (!mode)
  return -ENOMEM;
 drm_mode_set_name(mode);
 drm_mode_probed_add(connector, mode);
 {
  static const u32 format = MEDIA_BUS_FMT_RGB888_1X24;
  int ret = drm_display_info_set_bus_formats(&connector->display_info, &format, 1);
  if (ret)
   return ret;
 }
 connector->display_info.bus_flags = DRM_BUS_FLAG_DE_LOW |
                                    DRM_BUS_FLAG_PIXDATA_DRIVE_NEGEDGE;
 connector->display_info.width_mm = 52;
 connector->display_info.height_mm = 86;
 return 1;
}
/* Caller owns update_lock and a controller runtime PM reference. */
static int latona_panel_restore_rails(struct latona_bl *bl)
{
 int ret;
 bl->power_ready = false;
 if (!bl->rail_owned[0]) {
  ret = regulator_enable(bl->rails[0].consumer);
  if (ret)
   return ret;
  bl->rail_owned[0] = true;
  usleep_range(1000, 1500);
 }
 if (!bl->rail_owned[1]) {
  ret = regulator_enable(bl->rails[1].consumer);
  if (ret)
   return ret;
  bl->rail_owned[1] = true;
  usleep_range(1000, 1500);
 }
 if (regulator_is_enabled(bl->rails[0].consumer) <= 0 ||
     regulator_is_enabled(bl->rails[1].consumer) <= 0)
  return -EIO;
 ret = pinctrl_select_state(bl->pins, bl->default_pins);
 if (ret)
  return ret;
 bl->power_ready = true;
 dev_info(&bl->spi->dev, "DRM panel rails restored: vddio=1 vci=1\n");
 return 0;
}
static int latona_panel_prepare(struct drm_panel *panel)
{
 struct latona_bl *bl = container_of(panel, struct latona_bl, panel);
 struct backlight_device *bd = spi_get_drvdata(bl->spi);
 static struct device_attribute internal = { .attr = { .name = "drm_prepare" } };
 ssize_t ret;
 ret = pm_runtime_resume_and_get(bl->spi->controller->dev.parent);
 if (ret < 0)
  return ret;
 mutex_lock(&bd->update_lock);
 bl->drm_owned = true;
 bl->drm_enabled = false;
 ret = latona_panel_restore_rails(bl);
 mutex_unlock(&bd->update_lock);
 if (!ret)
  ret = reinitialize_store(&bl->spi->dev, &internal, "1", 1);
 pm_runtime_mark_last_busy(bl->spi->controller->dev.parent);
 pm_runtime_put_autosuspend(bl->spi->controller->dev.parent);
 dev_info(panel->dev, "DRM panel prepare result: %zd\n", ret);
 return ret < 0 ? ret : 0;
}
static int latona_panel_enable(struct drm_panel *panel)
{
 struct latona_bl *bl = container_of(panel, struct latona_bl, panel);
 struct backlight_device *bd = spi_get_drvdata(bl->spi);
 int ret;
 mutex_lock(&bd->update_lock);
 bl->drm_enabled = true;
 gpiod_set_value_cansleep(bl->bl_enable, 1);
 ret = latona_bl_update(bd);
 mutex_unlock(&bd->update_lock);
 dev_info(panel->dev, "DRM panel enable result: %d\n", ret);
 return ret;
}
static int latona_panel_disable(struct drm_panel *panel)
{
 struct latona_bl *bl = container_of(panel, struct latona_bl, panel);
 struct backlight_device *bd = spi_get_drvdata(bl->spi);
 int ret;
 mutex_lock(&bd->update_lock);
 bl->drm_enabled = false;
 ret = latona_bl_update(bd);
 gpiod_set_value_cansleep(bl->bl_enable, 0);
 mutex_unlock(&bd->update_lock);
 return ret;
}
static int latona_panel_unprepare(struct drm_panel *panel)
{
 struct latona_bl *bl = container_of(panel, struct latona_bl, panel);
 struct backlight_device *bd = spi_get_drvdata(bl->spi);
 static struct device_attribute internal = { .attr = { .name = "drm_prepare" } };
 int ret, recovery, io, ci;
 ret = pm_runtime_resume_and_get(bl->spi->controller->dev.parent);
 if (ret < 0)
  return ret;
 mutex_lock(&bd->update_lock);
 ret = latona_bl_word(bl->spi, 0x28);
 if (ret)
  goto recover;
 msleep(25);
 ret = latona_bl_word(bl->spi, 0x10);
 if (ret)
  goto recover;
 msleep(150);
 gpiod_set_value_cansleep(bl->bl_enable, 0);
 gpiod_set_value_cansleep(bl->reset, 1);
 bl->applied = -ENODATA;
 bl->power_ready = false;
 ret = pinctrl_select_state(bl->pins, bl->safe_pins);
 if (ret)
  goto recover;
 if (bl->rail_owned[1]) {
  ret = regulator_disable(bl->rails[1].consumer);
  if (ret)
   goto recover;
  bl->rail_owned[1] = false;
  usleep_range(1000, 1500);
 }
 if (bl->rail_owned[0]) {
  ret = regulator_disable(bl->rails[0].consumer);
  if (ret)
   goto recover;
  bl->rail_owned[0] = false;
 }
 io = regulator_is_enabled(bl->rails[0].consumer);
 ci = regulator_is_enabled(bl->rails[1].consumer);
 dev_info(panel->dev, "DRM panel rails off: vddio=%d vci=%d\n", io, ci);
 if (io != 0 || ci != 0) {
  ret = -EBUSY;
  goto recover;
 }
 msleep(20);
 mutex_unlock(&bd->update_lock);
 goto put_pm;
recover:
 /* A failed unprepare leaves DRM prepared: restore a usable dark panel. */
 recovery = latona_panel_restore_rails(bl);
 mutex_unlock(&bd->update_lock);
 if (!recovery) {
  ssize_t replay = reinitialize_store(&bl->spi->dev, &internal, "1", 1);
  if (replay < 0)
   recovery = replay;
 }
 dev_err(panel->dev, "DRM panel unprepare failed: %d, recovery=%d\n", ret, recovery);
put_pm:
 pm_runtime_mark_last_busy(bl->spi->controller->dev.parent);
 pm_runtime_put_autosuspend(bl->spi->controller->dev.parent);
 return ret;
}
static const struct drm_panel_funcs latona_panel_funcs = {
 .get_modes = latona_panel_get_modes,
 .prepare = latona_panel_prepare,
 .enable = latona_panel_enable,
 .disable = latona_panel_disable,
 .unprepare = latona_panel_unprepare,
};
static void latona_panel_unregister(void *data)
{
 drm_panel_remove(data);
}
static int latona_bl_probe(struct spi_device *spi)
{
 struct backlight_properties props = { .type = BACKLIGHT_RAW,
  .max_brightness = 255, .brightness = 108 };
 struct latona_bl *bl;
 struct backlight_device *bd;
 int ret;
 spi->mode = SPI_MODE_0;
 spi->bits_per_word = 9;
 if (spi->max_speed_hz > 375000)
  return -EINVAL;
 ret = spi_setup(spi);
 if (ret)
  return ret;
 bl = devm_kzalloc(&spi->dev, sizeof(*bl), GFP_KERNEL);
 if (!bl)
  return -ENOMEM;
 bl->pins = devm_pinctrl_get(&spi->dev);
 if (IS_ERR(bl->pins))
  return dev_err_probe(&spi->dev, PTR_ERR(bl->pins), "panel pinctrl unavailable\n");
 bl->default_pins = pinctrl_lookup_state(bl->pins, "default");
 bl->safe_pins = pinctrl_lookup_state(bl->pins, "sleep");
 if (IS_ERR(bl->default_pins) || IS_ERR(bl->safe_pins))
  return -EINVAL;
 bl->power_ready = true;
 bl->rails[0].supply = "vddio";
 bl->rails[1].supply = "vci";
 ret = devm_regulator_bulk_get(&spi->dev, ARRAY_SIZE(bl->rails), bl->rails);
 if (ret)
  return dev_err_probe(&spi->dev, ret, "panel supplies unavailable\n");
 if (regulator_get_voltage(bl->rails[0].consumer) != 1800000 ||
     regulator_get_voltage(bl->rails[1].consumer) != 2800000 ||
     regulator_is_enabled(bl->rails[0].consumer) <= 0 ||
     regulator_is_enabled(bl->rails[1].consumer) <= 0)
  return dev_err_probe(&spi->dev, -EINVAL, "panel rail baseline mismatch\n");
 dev_info(&spi->dev, "panel rails observed: vddio=1800000 vci=2800000; no power toggle\n");
 bl->bl_enable = devm_gpiod_get_optional(&spi->dev, "backlight-enable", GPIOD_ASIS);
 if (IS_ERR(bl->bl_enable))
  return dev_err_probe(&spi->dev, PTR_ERR(bl->bl_enable), "backlight enable GPIO request failed\n");
 if (bl->bl_enable)
  dev_info(&spi->dev, "GPIO42 inherited direction=%d raw=%d; no output change\n",
           gpiod_get_direction(bl->bl_enable),
           gpiod_get_raw_value_cansleep(bl->bl_enable));
 /* b52 owns the enable references; no probe off/on cycle. */
 ret = regulator_bulk_enable(ARRAY_SIZE(bl->rails), bl->rails);
 if (ret)
  return dev_err_probe(&spi->dev, ret, "panel supply reference acquire failed\n");
 bl->rail_owned[0] = true;
 bl->rail_owned[1] = true;
 ret = devm_add_action_or_reset(&spi->dev, latona_bl_release_rails, bl);
 if (ret)
  return ret;
 dev_info(&spi->dev, "panel supply enable references acquired; managed rails\n");
 bl->reset = devm_gpiod_get_optional(&spi->dev, "reset", GPIOD_ASIS);
 if (IS_ERR(bl->reset))
  return dev_err_probe(&spi->dev, PTR_ERR(bl->reset), "reset GPIO request failed\n");
 if (bl->reset) {
  ret = gpiod_get_direction(bl->reset);
  if (ret != 0 || gpiod_get_value_cansleep(bl->reset) != 0)
   return dev_err_probe(&spi->dev, -EINVAL, "reset not inherited as inactive output\n");
  dev_info(&spi->dev, "reset inherited inactive output; no probe pulse\n");
 }
 bl->spi = spi;
 bl->applied = -ENODATA;
 bd = devm_backlight_device_register(&spi->dev, "latona-sony", &spi->dev,
          bl, &latona_bl_ops, &props);
 if (IS_ERR(bd))
  return PTR_ERR(bd);
 spi_set_drvdata(spi, bd);
 drm_panel_init(&bl->panel, &spi->dev, &latona_panel_funcs, DRM_MODE_CONNECTOR_DPI);
 drm_panel_add(&bl->panel);
 ret = devm_add_action_or_reset(&spi->dev, latona_panel_unregister, &bl->panel);
 if (ret)
  return ret;
 dev_info(&spi->dev, "DRM panel registered: 480x800 20571kHz; native lifecycle ready\n");
 /* Deliberately no update_status here: initial panel level is unknown. */
 dev_info(&spi->dev, "brightness interface ready; bootloader level preserved\n");
 return 0;
}
static const struct of_device_id latona_bl_of_match[] = {
 { .compatible = "samsung,latona-sony-backlight" }, { }
};
MODULE_DEVICE_TABLE(of, latona_bl_of_match);
static const struct spi_device_id latona_bl_ids[] = { { "latona-sony-backlight", 0 }, { } };
MODULE_DEVICE_TABLE(spi, latona_bl_ids);
static struct spi_driver latona_bl_driver = {
 .driver = { .name = "latona-sony-backlight", .of_match_table = latona_bl_of_match,
             .dev_groups = latona_bl_groups },
 .id_table = latona_bl_ids, .probe = latona_bl_probe,
};
module_spi_driver(latona_bl_driver);
MODULE_DESCRIPTION("Latona Sony brightness-only SPI backlight");
MODULE_LICENSE("GPL");

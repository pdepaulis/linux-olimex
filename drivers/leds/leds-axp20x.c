// SPDX-License-Identifier: GPL-2.0+
//
// Copyright 2019 Stefan Mavrodiev <stefan@olimex.com>

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/mfd/axp20x.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <uapi/linux/uleds.h>


#define AXP20X_CHGLED_CTRL_REG		AXP20X_OFF_CTRL
#define AXP20X_CHGLED_FUNC_MASK			GENMASK(5, 4)
#define AXP20X_CHGLED_FUNC_OFF			(0 << 4)
#define AXP20X_CHGLED_FUNC_1HZ			(1 << 4)
#define AXP20X_CHGLED_FUNC_4HZ			(2 << 4)
#define AXP20X_CHGLED_FUNC_FULL			(3 << 4)
#define AXP20X_CHGLED_CTRL_MASK			BIT(3)
#define AXP20X_CHGLED_CTRL_MANUAL		0
#define AXP20X_CHGLED_CTRL_CHARGER		1
#define AXP20X_CHGLED_CTRL(_ctrl)		(_ctrl << 3)

#define AXP20X_CHGLED_MODE_REG		AXP20X_CHRG_CTRL2
#define AXP20X_CHGLED_MODE_MASK			BIT(4)
#define AXP20X_CHGLED_MODE_A			0
#define AXP20X_CHGLED_MODE_B			1
#define AXP20X_CHGLED_MODE(_mode)		(_mode << 4)

struct axp20x_led {
	char			name[LED_MAX_NAME_SIZE];
	struct led_classdev	cdev;
	struct mutex		lock;
	u8			mode : 1;
	u8			ctrl : 1;
	u8			ctrl_inverted : 1;
	struct axp20x_dev	*axp20x;
};

static inline struct axp20x_led *to_axp20x_led(struct led_classdev *cdev)
{
	return container_of(cdev, struct axp20x_led, cdev);
}

static int axp20x_led_setup(struct axp20x_led *priv)
{
	int ret;
	u8 val;

	/* Invert the logic, if necessary */
	val = priv->ctrl ^ priv->ctrl_inverted;

	mutex_lock(&priv->lock);
	ret = regmap_update_bits(priv->axp20x->regmap, AXP20X_CHGLED_CTRL_REG,
				 AXP20X_CHGLED_CTRL_MASK,
				 AXP20X_CHGLED_CTRL(val));
	if (ret < 0)
		goto out;

	ret = regmap_update_bits(priv->axp20x->regmap, AXP20X_CHGLED_MODE_REG,
				 AXP20X_CHGLED_MODE_MASK,
				 AXP20X_CHGLED_MODE(priv->mode));
out:
	mutex_unlock(&priv->lock);
	return ret;
}

static ssize_t control_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct axp20x_led *priv = to_axp20x_led(cdev);

	return sprintf(buf, "%u\n", priv->ctrl);
}

static ssize_t control_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t size)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct axp20x_led *priv = to_axp20x_led(cdev);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;

	/**
	 * Supported values are:
	 *   - 0 : Manual control
	 *   - 1 : Charger control
	 */
	if (val > 1)
		return -EINVAL;

	priv->ctrl = val;

	return axp20x_led_setup(priv) ? : size;
}
static DEVICE_ATTR_RW(control);

static ssize_t mode_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct axp20x_led *priv = to_axp20x_led(cdev);

	return sprintf(buf, "%u\n", priv->mode);
}

static ssize_t mode_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t size)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct axp20x_led *priv = to_axp20x_led(cdev);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 0, &val);
	if (ret)
		return ret;
	/**
	 * Supported values are:
	 *   - 0 : Mode A
	 *   - 1 : Mode B
	 */
	if (val > 1)
		return -EINVAL;

	priv->mode = val;

	return axp20x_led_setup(priv) ? : size;
}
static DEVICE_ATTR_RW(mode);

static struct attribute *axp20x_led_attrs[] = {
	&dev_attr_control.attr,
	&dev_attr_mode.attr,
	NULL,
};
ATTRIBUTE_GROUPS(axp20x_led);

enum led_brightness axp20x_led_brightness_get(struct led_classdev *cdev)
{
	struct axp20x_led *priv = to_axp20x_led(cdev);
	u32 val;
	int ret;

	mutex_lock(&priv->lock);
	ret = regmap_read(priv->axp20x->regmap, AXP20X_CHGLED_CTRL_REG, &val);
	mutex_unlock(&priv->lock);
	if (ret < 0)
		return LED_OFF;

	return (val & AXP20X_CHGLED_FUNC_FULL) ? LED_FULL : LED_OFF;
}

static int axp20x_led_brightness_set_blocking(struct led_classdev *cdev,
					      enum led_brightness brightness)
{
	struct axp20x_led *priv = to_axp20x_led(cdev);
	int ret = 0;

	mutex_lock(&priv->lock);
	ret = regmap_update_bits(priv->axp20x->regmap,
				 AXP20X_CHGLED_CTRL_REG,
				 AXP20X_CHGLED_FUNC_MASK,
				 (brightness) ?
				 AXP20X_CHGLED_FUNC_FULL :
				 AXP20X_CHGLED_FUNC_OFF);
	mutex_unlock(&priv->lock);

	return ret;
}

static int axp20x_led_parse_dt(struct axp20x_led *priv, struct device_node *np)
{
	const char *str;
	u8 value;
	int ret = 0;

	str = of_get_property(np, "label", NULL);
	if (!str)
		snprintf(priv->name, sizeof(priv->name), "axp20x::");
	else
		snprintf(priv->name, sizeof(priv->name), "axp20x:%s", str);
	priv->cdev.name = priv->name;

	priv->cdev.default_trigger = of_get_property(np,
						     "linux,default-trigger",
						     NULL);

	if (!of_property_read_u8(np, "x-powers,charger-mode", &value)) {
		priv->ctrl = AXP20X_CHGLED_CTRL_CHARGER;
		priv->mode = (value < 2) ? value : 0;
	} else {
		priv->ctrl = AXP20X_CHGLED_CTRL_MANUAL;
	}

	str = of_get_property(np, "default-state", NULL);
	if (str) {
		if (!strcmp(str, "keep")) {
			ret = axp20x_led_brightness_get(&priv->cdev);
			if (ret < 0)
				return ret;
			priv->cdev.brightness = ret;
		} else if (!strcmp(str, "on")) {
			ret = axp20x_led_brightness_set_blocking(&priv->cdev,
								 LED_FULL);
		} else  {
			ret = axp20x_led_brightness_set_blocking(&priv->cdev,
								 LED_OFF);
		}
	}

	return ret;
}

static const struct of_device_id axp20x_led_of_match[] = {
	{ .compatible = "x-powers,axp20x-led" },
	{}
};
MODULE_DEVICE_TABLE(of, axp20x_led_of_match);

static int axp20x_led_probe(struct platform_device *pdev)
{
	struct axp20x_led *priv;
	int ret;

	if (!of_device_is_available(pdev->dev.of_node))
		return -ENODEV;

	priv = devm_kzalloc(&pdev->dev, sizeof(struct axp20x_led),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->axp20x = dev_get_drvdata(pdev->dev.parent);
	if (!priv->axp20x) {
		dev_err(&pdev->dev, "Failed to get parent data\n");
		return -ENXIO;
	}

	mutex_init(&priv->lock);

	priv->cdev.brightness_set_blocking = axp20x_led_brightness_set_blocking;
	priv->cdev.brightness_get = axp20x_led_brightness_get;
	priv->cdev.groups = axp20x_led_groups;

	ret = axp20x_led_parse_dt(priv, pdev->dev.of_node);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to set parameters\n");
		return ret;
	}

	/**
	 * For some reason in AXP209 the bit that controls CHGLED is with
	 * inverted logic compared to all other PMICs.
	 * If the PMIC is actually AXP209, set inverted flag and later use it
	 * when configuring the LED.
	 */
	if (priv->axp20x->variant == AXP209_ID)
		priv->ctrl_inverted = 1;

	ret =  axp20x_led_setup(priv);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to configure led");
		return ret;
	}

	return devm_led_classdev_register(&pdev->dev, &priv->cdev);
}

static struct platform_driver axp20x_led_driver = {
	.driver = {
		.name	= "axp20x-led",
		.of_match_table = of_match_ptr(axp20x_led_of_match),
	},
	.probe = axp20x_led_probe,
};

module_platform_driver(axp20x_led_driver);

MODULE_AUTHOR("Stefan Mavrodiev <stefan@olimex.com");
MODULE_DESCRIPTION("X-Powers PMIC CHGLED driver");
MODULE_LICENSE("GPL");

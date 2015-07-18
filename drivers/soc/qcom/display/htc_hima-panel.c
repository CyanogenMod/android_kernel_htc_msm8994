
#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/i2c.h>
#include <linux/debug_display.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/async.h>
#include <linux/htc_flags.h>

#include "../../../../drivers/video/msm/mdss/mdss_dsi.h"

extern void set_screen_status(bool onoff);

enum pane_id {
	PANEL_JDI_RENESAS  = 0,
	PANEL_TIA_RES63315 = 1,
	PANEL_TDI_RES69338 = 2,
};

struct dsi_power_data {
	uint32_t sysrev;         			
	struct regulator *tp_3v3;
	struct regulator *tp_1v8;
	struct regulator *vddio; 			
	struct regulator *vdda;  			
	struct regulator *vddpll;    			
	uint16_t lcmp5v;				
	uint16_t lcmn5v;				
	uint16_t lcmio;
	uint16_t lcmrst;
};

static struct i2c_client *tps_client = NULL;
struct i2c_dev_info {
	uint8_t	dev_addr;
	struct i2c_client *client;
};
#define TPS_I2C_ADDRESS 	(0x3E)	
#define TPS_VPOS_ADDRESS	(0x00)
#define TPS_VNEG_ADDRESS	(0x01)
#define I2C_DEV_INFO(addr) \
	{.dev_addr = addr >> 1, .client = NULL}

enum tps_voltage {
	VOL_5V0 = 0,
	VOL_5V4 = 1,
	VOL_5V5 = 2,
};
static uint8_t vol_cmd[6][2] = {
	{TPS_VPOS_ADDRESS, 0x0A}, 	
	{TPS_VNEG_ADDRESS, 0x0A},
	{TPS_VPOS_ADDRESS, 0x0E}, 	
	{TPS_VNEG_ADDRESS, 0x0E},
	{TPS_VPOS_ADDRESS, 0x0F}, 	
	{TPS_VNEG_ADDRESS, 0x0F},
};

int cont_splash_enabled = 0;

static struct i2c_dev_info device_addresses[] = {
	I2C_DEV_INFO(TPS_I2C_ADDRESS)
};
static int tps_i2c_txdata(struct i2c_client *i2c, uint8_t *txData, int length)
{
	int ret = 0, retry = 10, i;
	struct i2c_msg msg[] = {
		{
			.addr = i2c->addr,
			.flags = 0,
			.buf = txData,
			.len = length,
		},
	};

	for (i = 0; i < retry; i++) {
		ret = i2c_transfer(i2c->adapter, msg, ARRAY_SIZE(msg));
		if (ret == ARRAY_SIZE(msg))
			break;
		else if (ret < 0) {
			PR_DISP_ERR("[DISP]%s: transfer failed. retry:%d", __func__, i);
			usleep_range(2000, 4000);
		}
	}
	if (i == retry) {
		PR_DISP_ERR("i2c_write_block retry over %d times\n", i);
		return -EIO;
	}

	dev_vdbg(&i2c->dev, "TxData: len=%02x, addr=%02x data=%02x",
		length, txData[0], txData[1]);
	return 0;
}
static void tps_65132_boost_on(enum tps_voltage vol)
{
	int ret = 0;
	uint8_t cmd[2][2] = {};

	memcpy(cmd, vol_cmd[vol == VOL_5V0 ? 0 :
                            vol == VOL_5V4 ? 2 : 4], 4);

	ret = tps_i2c_txdata(tps_client, cmd[0] , 2);
	usleep_range(1500, 2000);
	ret = tps_i2c_txdata(tps_client, cmd[1] , 2);
	if (ret)
		PR_DISP_ERR("%s: Boost to Certain voltage FAIL\n", __func__);
}
static struct kobject *disp_boost_voltage = NULL;
static ssize_t set_voltage(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;
	char str[50] = {0};
	unsigned long voltage = 0;

	ret = strict_strtoul(buf, 10, &voltage);
	if (ret < 0) {
		snprintf(str, sizeof(str), "Wrong Parameter. echo # > set_vol, where # must be decimal\n");
		PR_DISP_ERR("%s\n", str);
		return count;
	}

	if (voltage > 0x15 ) {
		PR_DISP_ERR("Set Voltage lower than 0, voltage should during 0x00 ~ 0x14");
	} else {
		uint8_t cmd[2][2] = {
			{TPS_VPOS_ADDRESS, voltage},
			{TPS_VNEG_ADDRESS, voltage},
		};
		ret = tps_i2c_txdata(tps_client, cmd[0], 2);
		msleep(5);
		ret = tps_i2c_txdata(tps_client, cmd[1], 2);
		if (ret) {
			snprintf(str, sizeof(str), "Can't Boost to certain voltage due to I2C problem");
			PR_DISP_ERR("%s: %s\n", __func__, str);
		} else {
			snprintf(str, sizeof(str), "Set Vol to (%ldv%ld) Sucess",
						(40+voltage)/10, (40+voltage)%10);
			PR_DISP_INFO("%s \n", str);
		}
	}

	return count;
}
static DEVICE_ATTR(set_vol, (S_IWUSR|S_IRUGO), NULL, set_voltage);

static int tps_65132_sysfs_init(void)
{
	disp_boost_voltage = kobject_create_and_add("disp_boost", NULL);
	if (!disp_boost_voltage) {
		PR_DISP_ERR("tps_65132 boost sysfs register failed\n");
		return -ENOMEM;
	}

	if (sysfs_create_file(disp_boost_voltage, &dev_attr_set_vol.attr)) {
		PR_DISP_ERR("tps_65132 filenode register failed");
		return -ENOMEM;
	}
	return 0;
}

static int tps_65132_add_i2c(struct i2c_client *client)
{
	int idx = 0;
	tps_client = client;
	if (tps_client == NULL) {
		PR_DISP_ERR("%s() failed to get i2c adapter\n", __func__);
		return -ENODEV;
	}

	for (idx = 0; idx < ARRAY_SIZE(device_addresses); idx++) {
		if (idx == 0)
			device_addresses[idx].client = client;
		else {
			device_addresses[idx].client =
				i2c_new_dummy(client->adapter, device_addresses[idx].dev_addr);

			if (device_addresses[idx].client == NULL)
				return -ENODEV;
		}
	}

	return 0;
}
static int tps_65132_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int ret = 0;
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);

	if (!i2c_check_functionality(adapter, I2C_FUNC_I2C)) {
		PR_DISP_ERR("%s: Failed to i2c_check_functionality \n", __func__);
		return -EIO;
	}
	if (!client->dev.of_node) {
		PR_DISP_ERR("%s: client->dev.of_node = NULL\n", __func__);
		return -ENOMEM;
	}

	ret = tps_65132_add_i2c(client);
	if(ret < 0) {
		PR_DISP_ERR("%s: Failed to tps_65132_add_i2c, ret=%d\n", __func__,ret);
		return ret;
	}
	ret = tps_65132_sysfs_init();
	PR_DISP_INFO("== %s (DONE); %s(%s) ==\n", __func__, "sysfs_init",
						(ret == 0) ? "DONE" : "Fail");
	return 0;
}

static const struct i2c_device_id tps_65132_id[] = {
	{"tps_lcm_boost65132", 0}
};
static struct of_device_id tsp_match_table[] = {
	{.compatible = "disp-tps-65132",}
};
static struct i2c_driver tps_65132_i2c_driver = {
	.driver = {
		.owner          = THIS_MODULE,
		.name           = "tps_lcm_boost_65132",
		.of_match_table = tsp_match_table,
		},
	.id_table = tps_65132_id,
	.probe    = tps_65132_i2c_probe,
	.command  = NULL,
};
static int __init tps_65132_init(void)
{
	int ret = 0;
	ret = i2c_add_driver(&tps_65132_i2c_driver);
	if (ret < 0)
		PR_DISP_ERR("%s: Failed to Register I2C driver, boost_voltage. ret=%d\n",
				__func__, ret);
	return ret;
}
#if 0
static int __init tps_65132_init(void)
{
	async_schedule(tps_65132_init_async, NULL);
	return 0;
}
static void __exit tps_65132_exit(void)
{
	i2c_del_driver(&tps_65132_i2c_driver);
}
module_init(tps_65132_init);
module_exit(tps_65132_exit);

MODULE_DESCRIPTION("TPS_65132 driver");
MODULE_LICENSE("GPL");
#endif
static int htc_hima_regulator_init(struct platform_device *pdev);
static int htc_hima_regulator_deinit(struct platform_device *pdev);
void htc_hima_panel_reset(struct mdss_panel_data *pdata);
static int htc_hima_panel_power_on(struct mdss_panel_data *pdata);
static int htc_hima_panel_power_off(struct mdss_panel_data *pdata);

static struct kobject *android_disp_kobj;
static uint8_t panel_id;
const char bootstr[] = "offmode_charging";

static struct mdss_dsi_pwrctrl dsi_pwrctrl = {
	.dsi_regulator_init   = htc_hima_regulator_init,
	.dsi_regulator_deinit = htc_hima_regulator_deinit,
	.dsi_power_on         = htc_hima_panel_power_on,
	.dsi_power_off 	      = htc_hima_panel_power_off,
	.dsi_panel_reset      = htc_hima_panel_reset,
	.incell_touch_on      = NULL,
	.notify_touch_cont_splash = NULL,
};

struct lcd_vendor_info {
	int idx;
	char vendor[64];
};

struct lcd_vendor_info lcd_name[] = {
	{0x00, "JDI_RES-63417"},
	{0x01, "TIA_RES-63315"},
	{0x02, "TDI_RES-69338"},
};

static ssize_t disp_vendor_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	ssize_t ret = 0;
	ret = snprintf(buf, PAGE_SIZE, "%s\n", lcd_name[panel_id].vendor);
	return ret;
}
static DEVICE_ATTR(vendor, S_IRUGO, disp_vendor_show, NULL);

static int htc_display_sysfs_init(bool status)
{
	if (status) {
		android_disp_kobj = kobject_create_and_add("android_display", NULL);
		if (!android_disp_kobj) {
			PR_DISP_ERR("%s: subsystem register failed\n", __func__);
			return -ENOMEM;
		}
		if (sysfs_create_file(android_disp_kobj, &dev_attr_vendor.attr)) {
			PR_DISP_ERR("Fail to create sysfs file (vendor)\n");
			return -ENOMEM;
		}
	} else {
		sysfs_remove_file(android_disp_kobj, &dev_attr_vendor.attr);
		kobject_del(android_disp_kobj);
	}
	return 0;
}

static int htc_hima_regulator_init(struct platform_device *pdev)
{
	int ret = 0;
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = NULL;
	struct dsi_power_data *pwrdata = NULL;

	if (!pdev) {
		PR_DISP_ERR("%s: invalid input\n", __func__);
		return -EINVAL;
	}

	ctrl_pdata = platform_get_drvdata(pdev);
	if (!ctrl_pdata) {
		PR_DISP_ERR("%s: invalid driver data\n", __func__);
		return -EINVAL;
	}
	if (strcmp(htc_get_bootmode(), bootstr))
		tps_65132_init();
	else
		PR_DISP_ERR("Recovery mode, discard boost voltage driver");

	pwrdata = devm_kzalloc(&pdev->dev, sizeof(struct dsi_power_data), GFP_KERNEL);
	if (!pwrdata) {
		PR_DISP_ERR("%s: FAILED to alloc pwrdata\n", __func__);
		return -ENOMEM;
	}
	ctrl_pdata->dsi_pwrctrl_data = pwrdata;

	
	pwrdata->lcmp5v = of_get_named_gpio(pdev->dev.of_node, "htc,lcm_p5v-gpio", 0);
	if (!gpio_is_valid(pwrdata->lcmp5v)) {
		PR_DISP_ERR("p5v gpio is not valid");
		ret = -EIO;
	}
	pwrdata->lcmn5v = of_get_named_gpio(pdev->dev.of_node, "htc,lcm_n5v-gpio", 0);
	if (!gpio_is_valid(pwrdata->lcmn5v)) {
		PR_DISP_ERR("p5v gpio is not valid");
		ret = -EIO;
	}
	pwrdata->lcmio = of_get_named_gpio(pdev->dev.of_node, "htc,lcm_bl_en-gpio", 0);
	if (!gpio_is_valid(pwrdata->lcmio)) {
		PR_DISP_ERR("p5v gpio is not valid");
		ret = -EIO;
	}
	pwrdata->lcmrst = of_get_named_gpio(pdev->dev.of_node, "htc,lcm_rst-gpio", 0);
	if (!gpio_is_valid(pwrdata->lcmrst)) {
		PR_DISP_ERR("p5v gpio is not valid");
		ret = -EIO;
	}
	PR_DISP_INFO("p5v:%d n5v:%d io1v8:%d rst:%d", pwrdata->lcmp5v,
							pwrdata->lcmn5v, pwrdata->lcmio,  pwrdata->lcmrst);
	

	cont_splash_enabled = ctrl_pdata->panel_data.panel_info.cont_splash_enabled;
	panel_id = ctrl_pdata->panel_data.panel_info.htc_panel_id;

	if (strcmp(htc_get_bootmode(), bootstr))
		htc_display_sysfs_init(1);

	PR_DISP_INFO("== %s( DONE ) ==\n", __func__);
	return ret;
}

static int htc_hima_regulator_deinit(struct platform_device *pdev)
{
	PR_DISP_INFO("%s()\n", __func__);
	if (strcmp(htc_get_bootmode(), bootstr))
		htc_display_sysfs_init(0);

	return 0;
}

static inline int htc_hima_panel_check(int panel_id)
{
	int panel_used = -1;
	switch (panel_id) {
	case PANEL_JDI_RENESAS:
		panel_used = PANEL_JDI_RENESAS;
		break;
	case PANEL_TIA_RES63315:
		panel_used = PANEL_TIA_RES63315;
		break;
	case PANEL_TDI_RES69338:
		panel_used = PANEL_TDI_RES69338;
	break;
	default:
		PR_DISP_ERR("%s:Wrong panel_id[%d]\n", __func__, panel_id);
	}
	return panel_used;
}

void htc_hima_panel_reset(struct mdss_panel_data *pdata)
{
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = NULL;
	struct dsi_power_data *pwrdata = NULL;

	PR_DISP_INFO("++%s()++\n", __func__);
	if (!pdata) {
		PR_DISP_ERR("%s: Invalid input data pointer\n", __func__);
		return;
	}
	ctrl_pdata = container_of(pdata, struct mdss_dsi_ctrl_pdata, panel_data);
	pwrdata = ctrl_pdata->dsi_pwrctrl_data;

	if (!gpio_is_valid(pwrdata->lcmrst)) {
		PR_DISP_ERR("%s:reset pin not valid\n", __func__);
		return;
	}

	gpio_set_value((pwrdata->lcmrst), 1);
	usleep_range(1500, 2000);
	gpio_set_value((pwrdata->lcmrst), 0);
	usleep_range(1500, 2000);
	gpio_set_value((pwrdata->lcmrst), 1);
	usleep_range(1500, 2000);

	PR_DISP_INFO("--%s()--\n", __func__);
}

static int htc_hima_panel_power_on(struct mdss_panel_data *pdata)
{
        int ret = 0;
	uint8_t voltage = 0;
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = NULL;
	struct dsi_power_data *pwrdata = NULL;

	if (pdata == NULL) {
		PR_DISP_ERR("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	ctrl_pdata = container_of(pdata, struct mdss_dsi_ctrl_pdata, panel_data);
	pwrdata = ctrl_pdata->dsi_pwrctrl_data;
	if (!pwrdata) {
		PR_DISP_ERR("%s: pwrdata not initialized\n", __func__);
		return -EINVAL;
	}
	PR_DISP_INFO("++%s()++\n", __func__);

	if (gpio_is_valid(pwrdata->lcmio) && gpio_is_valid(pwrdata->lcmp5v) &&
	    gpio_is_valid(pwrdata->lcmn5v) && gpio_is_valid(pwrdata->lcmrst)) {
		if (pdata->panel_info.first_power_on == 1) {
			PR_DISP_INFO("panel on already\n");
			return 0;
		}
		if (pdata->panel_info.htc_panel_id == PANEL_TDI_RES69338) {
			gpio_set_value(pwrdata->lcmrst, 0);
			usleep_range(5000, 5500);

			gpio_set_value(pwrdata->lcmn5v, 0);
			usleep_range(1000, 1200);

			gpio_set_value(pwrdata->lcmp5v, 0);
			usleep_range(5000, 5500);
		}

		gpio_set_value(pwrdata->lcmio, 1);
		usleep_range(10000, 12000);

		gpio_set_value(pwrdata->lcmp5v, 1);
		usleep_range(10000, 12000);

		gpio_set_value(pwrdata->lcmn5v, 1);
		usleep_range(10000, 12000);

		if (gpio_is_valid(pwrdata->lcmrst))
			gpio_set_value(pwrdata->lcmrst, 1);

		switch (pdata->panel_info.htc_panel_id) {
		case PANEL_TIA_RES63315:
			voltage = VOL_5V5;
			break;
		case PANEL_TDI_RES69338:
			voltage = VOL_5V4;
			break;
		case PANEL_JDI_RENESAS:
		default:
			voltage = VOL_5V0;
			break;
		}
		if (strcmp(htc_get_bootmode(), bootstr))
			tps_65132_boost_on(voltage);	
		else
			PR_DISP_ERR("Recovery mode, discard send boost i2c command");
	} else {
		PR_DISP_ERR("%s:Gpio are not valid\n", __func__);
		ret = -EINVAL;
	}

	if (pdata->panel_info.htc_panel_id == PANEL_TDI_RES69338 &&	
		dsi_pwrctrl.incell_touch_on != NULL)
		dsi_pwrctrl.incell_touch_on(1);

	PR_DISP_INFO("--%s(%s)--\n", __func__, ((voltage == VOL_5V0) ? "5V" :
						(voltage == VOL_5V4) ? "5V4" : "5V5"));

#ifdef CONFIG_HTC_PNPMGR
	set_screen_status(true);
#endif

	return ret;
}

static int htc_hima_panel_power_off(struct mdss_panel_data *pdata)
{
	int ret = 0;
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = NULL;
	struct dsi_power_data *pwrdata = NULL;

	if (pdata == NULL) {
		PR_DISP_ERR("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	ctrl_pdata = container_of(pdata, struct mdss_dsi_ctrl_pdata, panel_data);
	pwrdata = ctrl_pdata->dsi_pwrctrl_data;
	if (!pwrdata) {
		PR_DISP_ERR("%s: pwrdata not initialized\n", __func__);
		return -EINVAL;
	}
	PR_DISP_INFO("++%s()++\n", __func__);

	if (pdata->panel_info.htc_panel_id == PANEL_TDI_RES69338 && 	
		dsi_pwrctrl.incell_touch_on != NULL)
		dsi_pwrctrl.incell_touch_on(0);

	if (gpio_is_valid(pwrdata->lcmio) && gpio_is_valid(pwrdata->lcmp5v) &&
	    gpio_is_valid(pwrdata->lcmn5v) && gpio_is_valid(pwrdata->lcmrst)) {
		gpio_set_value(pwrdata->lcmrst, 0);

		if (pdata->panel_info.htc_panel_id == PANEL_TDI_RES69338)
			usleep_range(5000, 5500);

		gpio_set_value(pwrdata->lcmn5v, 0);
		usleep_range(10000, 12000);

		gpio_set_value(pwrdata->lcmp5v, 0);

		if (pdata->panel_info.htc_panel_id != PANEL_TDI_RES69338) {
			gpio_set_value(pwrdata->lcmio, 0);
			usleep_range(10000, 12000);
		}

	} else {
		PR_DISP_ERR("%s:Gpio are not valid\n", __func__);
		ret = -EINVAL;
	}
	PR_DISP_INFO("--%s()--\n", __func__);

#ifdef CONFIG_HTC_PNPMGR
	set_screen_status(false);
#endif

	return ret;
}

void incell_driver_ready(void (*fn))
{
	PR_DISP_WARN("%s()",__func__);
	dsi_pwrctrl.notify_touch_cont_splash = fn;
	dsi_pwrctrl.notify_touch_cont_splash(cont_splash_enabled);
}
EXPORT_SYMBOL(incell_driver_ready);

static struct platform_device dsi_pwrctrl_device = {
	.name = "mdss_dsi_pwrctrl",
	.id   = -1,
	.dev.platform_data = &dsi_pwrctrl,
};

int __init htc_8994_dsi_panel_power_register(void)
{
	platform_device_register(&dsi_pwrctrl_device);
	return 0;
}

arch_initcall(htc_8994_dsi_panel_power_register);

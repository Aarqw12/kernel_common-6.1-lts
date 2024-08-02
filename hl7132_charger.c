// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the HL7132 battery charger.
 *
 * Copyright (C) 2024 Google, LLC.
 */

#include <linux/err.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/of_irq.h>
#include <linux/of_device.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/rtc.h>
#include <misc/gvotable.h>

#include "hl7132_regs.h"
#include "hl7132_charger.h"

#if defined(CONFIG_OF)
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#endif /* CONFIG_OF */

/* Timer definition */
#define HL7132_VBATMIN_CHECK_T	1000	/* 1000ms */
#define HL7132_CCMODE_CHECK1_T	5000	/* 10000ms -> 500ms */
#define HL7132_CCMODE_CHECK2_T	5000	/* 5000ms */
#define HL7132_CVMODE_CHECK_T	10000	/* 10000ms */
#define HL7132_ENABLE_DELAY_T	150	/* 150ms */
#define HL7132_CVMODE_CHECK2_T	1000	/* 1000ms */

/* Battery Threshold */
#define HL7132_DC_VBAT_MIN		3400000 /* uV */
/* Input Current Limit default value */
#define HL7132_IIN_CFG_DFT		2500000 /* uA*/
/* Charging vbat_reg default value */
#define HL7132_VBAT_REG_DFT		4350000	/* uV */
/* Charging vbat_reg max voltage TODO value */
#define HL7132_VBAT_REG_MAX		460000	/* uV */

/* Sense Resistance default value */
#define HL7132_SENSE_R_DFT		1	/* 10mOhm */
/* Switching Frequency default value */
#define HL7132_FSW_CFG_DFT		3	/* 980KHz */
/* NTC threshold voltage default value */
#define HL7132_NTC_TH_DFT		0	/* uV*/

/* Charging Done Condition */
#define HL7132_IIN_DONE_DFT	500000		/* uA */
/* parallel charging done conditoin */
#define HL7132_IIN_P_DONE	1000000		/* uA */
/* Parallel charging default threshold */
#define HL7132_IIN_P_TH_DFT	4000000		/* uA */
/* Single charging default threshold */
#define HL7132_IIN_S_TH_DFT	10000000	/* uA */

/* Maximum TA voltage threshold */
#define HL7132_TA_MAX_VOL		9800000 /* uV */
/* Maximum TA current threshold, set to max(cc_max) / 2 */
#define HL7132_TA_MAX_CUR		2600000	 /* uA */
/* Minimum TA current threshold */
#define HL7132_TA_MIN_CUR		1000000	/* uA - PPS minimum current */

/* Minimum TA voltage threshold in Preset mode */
#define HL7132_TA_MIN_VOL_PRESET	8000000	/* uV */
/* TA voltage threshold starting Adjust CC mode */
#define HL7132_TA_MIN_VOL_CCADJ	8500000	/* 8000000uV --> 8500000uV */

#define HL7132_TA_VOL_PRE_OFFSET	500000	 /* uV */
/* Adjust CC mode TA voltage step */
#define HL7132_TA_VOL_STEP_ADJ_CC	40000	/* uV */
/* Pre CV mode TA voltage step */
#define HL7132_TA_VOL_STEP_PRE_CV	20000	/* uV */

/* IIN_CC adc offset for accuracy */
#define HL7132_IIN_ADC_OFFSET		20000	/* uA */
/* IIN_CC compensation offset */
#define HL7132_IIN_CC_COMP_OFFSET	25000	/* uA */
/* IIN_CC compensation offset in Power Limit Mode(Constant Power) TA */
#define HL7132_IIN_CC_COMP_OFFSET_CP	20000	/* uA */
/* TA maximum voltage that can support CC in Constant Power Mode */
#define HL7132_TA_MAX_VOL_CP		9800000	/* 9760000uV --> 9800000uV */
/* Offset for cc_max / 2 */
#define HL7132_IIN_MAX_OFFSET		0
/* Offset for TA max current */
#define HL7132_TA_CUR_MAX_OFFSET	200000 /* uA */


/* maximum retry counter for restarting charging */
#define HL7132_MAX_RETRY_CNT		3	/* retries */
/* TA IIN tolerance */
#define HL7132_TA_IIN_OFFSET		100000	/* uA */
/* IIN_CC upper protection offset in Power Limit Mode TA */
#define HL7132_IIN_CC_UPPER_OFFSET	50000	/* 50mA */

/* PD Message Voltage and Current Step */
#define PD_MSG_TA_VOL_STEP		20000	/* uV */
#define PD_MSG_TA_CUR_STEP		50000	/* uA */

#define HL7132_OTV_MARGIN		12000	/* uV */

#define HL7132_TIER_SWITCH_DELTA	25000	/* uV */

/* INT1 Register Buffer */
enum {
	REG_INT1,
	REG_INT1_MSK,
	REG_INT1_STS,
	REG_INT1_MAX
};

/* STS Register Buffer */
enum {
	REG_STS_A,
	REG_STS_B,
	REG_STS_C,
	REG_STS_D,
	REG_STS_MAX
};

/* Status */
enum {
	STS_MODE_CHG_LOOP,	/* TODO: There is no such thing */
	STS_MODE_VFLT_LOOP,
	STS_MODE_IIN_LOOP,
	STS_MODE_IBAT_LOOP,
	STS_MODE_LOOP_INACTIVE,
	STS_MODE_TEMP_REG,
	STS_MODE_CHG_DONE,
	STS_MODE_VIN_UVLO,
	STS_MODE_UNKNOWN
};

/* Timer ID */
enum {
	TIMER_ID_NONE,
	TIMER_VBATMIN_CHECK,
	TIMER_PRESET_DC,
	TIMER_PRESET_CONFIG,
	TIMER_CHECK_ACTIVE,
	TIMER_ADJUST_CCMODE,
	TIMER_CHECK_CCMODE,
	TIMER_ENTER_CVMODE,
	TIMER_CHECK_CVMODE, /* 8 */
	TIMER_PDMSG_SEND,   /* 9 */
	TIMER_ADJUST_TAVOL,
	TIMER_ADJUST_TACUR,
};


/* TA increment Type */
enum {
	INC_NONE,	/* No increment */
	INC_TA_VOL,	/* TA voltage increment */
	INC_TA_CUR,	/* TA current increment */
};

/* BATT info Type */
enum {
	BATT_CURRENT,
	BATT_VOLTAGE,
};

/* ------------------------------------------------------------------------ */

static int hl7132_hw_ping(struct hl7132_charger *hl7132)
{
	unsigned int val = 0;
	int ret;

	/* Read Device info register to check the incomplete I2C operation */
	ret = regmap_read(hl7132->regmap, HL7132_REG_DEVICE_ID, &val);
	val = val & HL7132_BIT_DEV_ID;
	if ((ret < 0) || (val != HL7132_DEVICE_ID)) {
		ret = regmap_read(hl7132->regmap, HL7132_REG_DEVICE_ID, &val);
		val = val & HL7132_BIT_DEV_ID;
	}
	if ((ret < 0) || (val != HL7132_DEVICE_ID)) {
		dev_err(hl7132->dev, "reading DEVICE_ID failed, val=%#x ret=%d\n",
			val, ret);
		return -EINVAL;
	}

	return 0;
}

/* HW integration guide section 4
 * call holding mutex_lock(&hl7132->lock)
 */
static int hl7132_hw_init(struct hl7132_charger *hl7132)
{
	int ret = 0;

	unsigned int reg_value;
	unsigned int reg_ctrl_0, reg_ctrl_1, track_ov_uv, ctrl_0;
	unsigned int vbat_ovp_th, iin_ocp_th, iin_ucp_th;
	unsigned int track_ov, track_uv;

	/* HW integration guide section 4.1.1 */
	dev_info(hl7132->dev, "%s: Triggering soft reset\n", __func__);
	regmap_update_bits(hl7132->regmap, HL7132_REG_CTRL_2,
				HL7132_BITS_SFT_RST,
				HL7132_SFT_RESET << MASK2SHIFT(HL7132_BITS_SFT_RST));
	/* regmap_update_bits will always report a failure after soft reset,
	 * so confirm that it succeeded by making sure HL7132_REG_CTRL_2 is back
	 * to default after waiting for soft reset to complete - chip holds I2C
	 * BUS for ~6ms after reset is triggered. Due to the AP resetting the
	 * i2c after detecting the failure, two reads are needed.
	 * Wait 100ms as per HW
	 * integration guide.
	 */
	msleep(100);

	ret = regmap_read(hl7132->regmap, HL7132_REG_CTRL_2, &reg_value);
	msleep(20);
	ret = regmap_read(hl7132->regmap, HL7132_REG_CTRL_2, &reg_value);
	if (ret < 0) {
		dev_err(hl7132->dev, "%s: Failed to read after soft reset\n",
			__func__);
		return ret;
	}
	if (reg_value  != HL7132_CTRL_2_DFT) {
		dev_err(hl7132->dev, "%s: Failed to perform soft reset\n",
			__func__);
		return ret;
	}

	/* HW integration guide section 4.2.1 - check device ID */
	if (hl7132_hw_ping(hl7132))
		return ret;

	/* HW integration guide section 4.2.2 - Set TSBAT_EN_PIN - enable TS
	 * protection and set thresholds
	 */
	ret = regmap_update_bits(hl7132->regmap, HL7132_REG_CTRL_1,
				 HL7132_BIT_TS_PROT_EN, HL7132_BIT_TS_PROT_EN);
	if (ret < 0)
		return ret;
	ret = regmap_write(hl7132->regmap, HL7132_REG_TS0_TH_0,
			   HL7132_TS0_TH_0_INIT_DFT);
	if (ret < 0)
		return ret;
	ret = regmap_write(hl7132->regmap, HL7132_REG_TS0_TH_1,
			   HL7132_TS0_TH_1_INIT_DFT);
	if (ret < 0)
		return ret;

	/* HW integration guide section 4.2.3 - Disable IBAT OCP */
	ret = regmap_update_bits(hl7132->regmap, HL7132_REG_IBAT_REG,
				 HL7132_BIT_IBAT_OCP_DIS, 1);
	if (ret < 0)
		return ret;

	/* HW integration guide section 4.2.4 - Confirm default protection thresholds */
	ret = regmap_read(hl7132->regmap, HL7132_REG_REG_CTRL_0, &reg_ctrl_0);
	if (ret < 0) {
		dev_err(hl7132->dev, "%s: Failed to read REG_CTRL_0, ret=%d\n",
			__func__, ret);
		return ret;
	}

	ret = regmap_read(hl7132->regmap, HL7132_REG_REG_CTRL_1, &reg_ctrl_1);
	if (ret < 0) {
		dev_err(hl7132->dev, "%s: Failed to read REG_CTRL_1, ret=%d\n",
			__func__, ret);
		return ret;
	}

	ret = regmap_read(hl7132->regmap, HL7132_REG_TRACK_OV_UV, &track_ov_uv);
	if (ret < 0) {
		dev_err(hl7132->dev, "%s: Failed to read TRACK_OV, ret=%d\n",
			__func__, ret);
		return ret;
	}

	ret = regmap_read(hl7132->regmap, HL7132_REG_CTRL_0, &ctrl_0);
	if (ret < 0) {
		dev_err(hl7132->dev, "%s: Failed to read CTRL_0, ret=%d\n",
			__func__, ret);
		return ret;
	}

	vbat_ovp_th = (reg_ctrl_1 & HL7132_BITS_VBAT_OVP_TH) >> MASK2SHIFT(HL7132_BITS_VBAT_OVP_TH);
	if ((vbat_ovp_th) != HL7132_VBAT_OVP_TH_DFT) {
		dev_warn(hl7132->dev,
			"%s: Unexpected VBAT_OVP_TH value (0x%02x, expected 0x%02x)\n",
			__func__, vbat_ovp_th, HL7132_VBAT_OVP_TH_DFT);
		return -EINVAL;
	}

	iin_ocp_th = (reg_ctrl_0 & HL7132_BITS_IIN_OCP_TH) >> MASK2SHIFT(HL7132_BITS_IIN_OCP_TH);
	if (iin_ocp_th != HL7132_IIN_OCP_TH_DFT) {
		dev_warn(hl7132->dev,
			"%s: Unexpected IIN_OCP_TH value (0x%02x, expected 0x%02x)\n",
			__func__, iin_ocp_th, HL7132_IIN_OCP_TH_DFT);
		return -EINVAL;
	}

	iin_ucp_th = (ctrl_0 & HL7132_BITS_IIN_UCP_TH) >> MASK2SHIFT(HL7132_BITS_IIN_UCP_TH);
	if (iin_ucp_th != HL7132_IIN_UCP_TH_DFT) {
		dev_warn(hl7132->dev,
			"%s: Unexpected IIN_UCP_TH value (0x%02x, expected 0x%02x)\n",
			__func__, iin_ucp_th, HL7132_IIN_UCP_TH_DFT);

		return -EINVAL;
	}

	track_ov = (track_ov_uv & HL7132_BITS_TRACK_OV) >> MASK2SHIFT(HL7132_BITS_TRACK_OV);
	if (track_ov != HL7132_TRACK_OV_DFT) {
		dev_warn(hl7132->dev,
			"%s: Unexpected TRACK_OV value (0x%02x, expected 0x%02x)\n",
			__func__, track_ov, HL7132_TRACK_OV_DFT);
		return -EINVAL;
	}

	track_uv = (track_ov_uv & HL7132_BITS_TRACK_UV) >> MASK2SHIFT(HL7132_BITS_TRACK_UV);
	if (track_uv != HL7132_TRACK_UV_DFT) {
		dev_warn(hl7132->dev,
			"%s: Unexpected TRACK_UV value (0x%02x, expected 0x%02x)\n",
			__func__, track_uv, HL7132_TRACK_UV_DFT);
		return -EINVAL;
	}

	/* HW integration guide section 4.2.5 */
	/* Unmask TS_TEMP interrupt */
	ret = regmap_update_bits(hl7132->regmap, HL7132_REG_INT_MSK,
				 HL7132_BIT_TS_TEMP_M, 0);
	if (ret < 0)
		return ret;

	/* Clear interrupt flags (read to clear) */
	ret = regmap_read(hl7132->regmap, HL7132_REG_INT, &reg_value);
	if (ret < 0)
		return ret;

	/* HW integration guide section 4.2.6 */
	/* Disable unused ADC channels */
	ret = regmap_write(hl7132->regmap, HL7132_REG_ADC_CTRL_1, HL7132_ADC_CTRL_1_INIT_DFT);
	if (ret < 0)
		return ret;

	/* Enable ADC */
	ret = regmap_update_bits(hl7132->regmap, HL7132_REG_ADC_CTRL_0,
				 HL7132_BIT_ADC_EN, HL7132_BIT_ADC_EN);
	if (ret < 0)
		return ret;

	return 0;
}

enum {
	ENUM_INT,
	ENUM_INT_MASK,
	ENUM_INT_STS_A,
	ENUM_INT_STS_B,
	ENUM_INT_MAX,
};

/* Returns the input current limit programmed into the charger in uA. */
int hl7132_input_current_limit(struct hl7132_charger *hl7132)
{
	int ret, intval;
	unsigned int val;

	if (hl7132->mains_online)
		return -ENODATA;

	ret = regmap_read(hl7132->regmap, HL7132_REG_IIN_REG, &val);
	if (ret < 0)
		return ret;

	/* 50 mA/step * 1000 uA/mA = 50000 uA/step). */
	intval = (val & HL7132_BITS_IIN_REG_TH) * 50000;

	if (intval < 1000000) /* HL7132 min is 1A */
		intval = 1000000;

	return intval;
}

static int hl7132_mains_set_property(struct power_supply *psy,
				      enum power_supply_property prop,
				      const union power_supply_propval *val)
{
	struct hl7132_charger *hl7132 = power_supply_get_drvdata(psy);
	int ret = 0;

	dev_dbg(hl7132->dev, "%s: prop=%d, val=%d\n", __func__, prop,
		val->intval);

	if (!hl7132->init_done)
		return -EAGAIN;

	switch (prop) {

	default:
		ret = -EINVAL;
		break;
	}

	dev_dbg(hl7132->dev, "%s: End, ret=%d\n", __func__, ret);
	return ret;
}

static int hl7132_mains_get_property(struct power_supply *psy,
				     enum power_supply_property prop,
				     union power_supply_propval *val)
{
	struct hl7132_charger *hl7132 = power_supply_get_drvdata(psy);

	dev_dbg(hl7132->dev, "%s: prop=%d, val=%d\n", __func__, prop,
		val->intval);

	if (!hl7132->init_done)
		return -EAGAIN;

	switch (prop) {

	default:
		return -EINVAL;
	}

	return 0;
}

/*
 * GBMS not visible
 * POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
 * POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
 * POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
 * POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,
 */
static enum power_supply_property hl7132_mains_properties[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_TEMP,
	/* same as POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT */
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_STATUS,
};

static int hl7132_mains_is_writeable(struct power_supply *psy,
				      enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		return 1;
	default:
		break;
	}

	return 0;
}

static int hl7132_gbms_mains_set_property(struct power_supply *psy,
					   enum gbms_property prop,
					   const union gbms_propval *val)
{
	struct hl7132_charger *hl7132 = power_supply_get_drvdata(psy);
	int ret = 0;

	dev_dbg(hl7132->dev, "%s: prop=%d, val=%d\n", __func__, prop,
		val->prop.intval);
	if (!hl7132->init_done)
		return -EAGAIN;

	switch (prop) {

	default:
		dev_dbg(hl7132->dev,
			"%s: route to hl7132_mains_set_property, psp:%d\n",
			__func__, prop);
		return -ENODATA;
	}

	dev_dbg(hl7132->dev, "%s: End, ret=%d\n", __func__, ret);
	return ret;
}

static int hl7132_gbms_mains_get_property(struct power_supply *psy,
					   enum gbms_property prop,
					   union gbms_propval *val)
{
	struct hl7132_charger *hl7132 = power_supply_get_drvdata(psy);

	dev_dbg(hl7132->dev, "%s: prop=%d, val=%d\n", __func__, prop,
		val->prop.intval);
	if (!hl7132->init_done)
		return -EAGAIN;

	switch (prop) {

	default:
		dev_dbg(hl7132->dev,
			"%s: route to hl7132_mains_get_property, psp:%d\n",
			__func__, prop);
		return -ENODATA;
	}

	return 0;
}

static int hl7132_gbms_mains_is_writeable(struct power_supply *psy,
					   enum gbms_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
	case POWER_SUPPLY_PROP_CURRENT_MAX:
	case GBMS_PROP_CHARGING_ENABLED:
	case GBMS_PROP_CHARGE_DISABLE:
		return 1;
	default:
		break;
	}

	return 0;
}

static bool hl7132_is_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case HL7132_REG_DEVICE_ID ... HL7132_REG_ADC_TDIE_1:
		return true;
	default:
		break;
	}

	return false;
}

static struct regmap_config hl7132_regmap = {
	.name		= "dc-mains",
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= HL7132_MAX_REGISTER,
	.readable_reg = hl7132_is_reg,
	.volatile_reg = hl7132_is_reg,
};

static struct gbms_desc hl7132_mains_desc = {
	.psy_dsc.name		= "hl7132-mains",
	/* b/179246019 will not look online to Android */
	.psy_dsc.type		= POWER_SUPPLY_TYPE_UNKNOWN,
	.psy_dsc.get_property	= hl7132_mains_get_property,
	.psy_dsc.set_property	= hl7132_mains_set_property,
	.psy_dsc.property_is_writeable = hl7132_mains_is_writeable,
	.get_property		= hl7132_gbms_mains_get_property,
	.set_property		= hl7132_gbms_mains_set_property,
	.property_is_writeable = hl7132_gbms_mains_is_writeable,
	.psy_dsc.properties	= hl7132_mains_properties,
	.psy_dsc.num_properties	= ARRAY_SIZE(hl7132_mains_properties),
	.forward		= true,
};

#if defined(CONFIG_OF)
static int of_hl7132_dt(struct device *dev,
			 struct hl7132_platform_data *pdata)
{
	struct device_node *np_hl7132 = dev->of_node;
	int ret;

	if (!np_hl7132)
		return -EINVAL;

	/* input current limit */
	ret = of_property_read_u32(np_hl7132, "hl7132,input-current-limit",
				   &pdata->iin_cfg_max);
	if (ret) {
		dev_warn(dev, "%s: hl7132,input-current-limit is Empty\n",
			__func__);
		pdata->iin_cfg_max = HL7132_IIN_CFG_DFT;
	}
	pdata->iin_cfg = pdata->iin_cfg_max;
	dev_info(dev, "%s: hl7132,iin_cfg is %u\n", __func__, pdata->iin_cfg);

	/* TA max voltage limit */
	ret = of_property_read_u32(np_hl7132, "hl7132,ta-max-vol",
				   &pdata->ta_max_vol);
	if (ret) {
		dev_warn(dev, "%s: hl7132,ta-max-vol is Empty\n",
			__func__);
		pdata->ta_max_vol = HL7132_TA_MAX_VOL;
	}
	ret = of_property_read_u32(np_hl7132, "hl7132,ta-max-vol-cp",
				   &pdata->ta_max_vol_cp);
	if (ret) {
		dev_warn(dev, "%s: hl7132,ta-max-vol-cp is Empty\n",
			__func__);
		pdata->ta_max_vol_cp = pdata->ta_max_vol;
	}

	/* charging float voltage */
	ret = of_property_read_u32(np_hl7132, "hl7132,vbat_reg-voltage",
				   &pdata->vbat_reg_dt);
	if (ret) {
		dev_warn(dev, "%s: hl7132,vbat_reg-voltage is Empty\n",
			__func__);
		pdata->vbat_reg_dt = HL7132_VBAT_REG_DFT;
	}
	pdata->vbat_reg = pdata->vbat_reg_dt;
	dev_info(dev, "%s: hl7132,vbat_reg is %u\n", __func__, pdata->vbat_reg);

	/* input topoff current */
	ret = of_property_read_u32(np_hl7132, "hl7132,input-itopoff",
				   &pdata->iin_topoff);
	if (ret) {
		dev_warn(dev, "%s: hl7132,input-itopoff is Empty\n",
			__func__);
		pdata->iin_topoff = HL7132_IIN_DONE_DFT;
	}
	dev_info(dev, "%s: hl7132,iin_topoff is %u\n", __func__, pdata->iin_topoff);

	/* switching frequency */
	ret = of_property_read_u32(np_hl7132, "hl7132,switching-frequency",
				   &pdata->fsw_cfg);
	if (ret) {
		dev_warn(dev, "%s: hl7132,switching frequency is Empty\n",
			 __func__);
		pdata->fsw_cfg = HL7132_FSW_CFG_DFT;
	}
	dev_info(dev, "%s: hl7132,fsw_cfg is %u\n", __func__, pdata->fsw_cfg);

	/* iin offsets */
	ret = of_property_read_u32(np_hl7132, "hl7132,iin-max-offset",
				   &pdata->iin_max_offset);
	if (ret)
		pdata->iin_max_offset = HL7132_IIN_MAX_OFFSET;
	dev_info(dev, "%s: hl7132,iin_max_offset is %u\n", __func__, pdata->iin_max_offset);

	ret = of_property_read_u32(np_hl7132, "hl7132,iin-cc_comp-offset",
				   &pdata->iin_cc_comp_offset);
	if (ret)
		pdata->iin_cc_comp_offset = HL7132_IIN_CC_COMP_OFFSET;
	dev_info(dev, "%s: hl7132,iin_cc_comp_offset is %u\n", __func__, pdata->iin_cc_comp_offset);

	ret = of_property_read_u32(np_hl7132, "hl7132,ta-vol-offset",
				   &pdata->ta_vol_offset);
	if (ret)
		pdata->iin_cc_comp_offset = HL7132_TA_VOL_PRE_OFFSET;
	dev_info(dev, "%s: hl7132,ta-vol-offset is %u\n", __func__, pdata->ta_vol_offset);


#if IS_ENABLED(CONFIG_THERMAL)
	/* USBC thermal zone */
	ret = of_property_read_string(np_hl7132, "google,usb-port-tz-name",
				      &pdata->usb_tz_name);
	if (ret) {
		dev_info(dev, "%s: google,usb-port-tz-name is Empty\n", __func__);
		pdata->usb_tz_name = NULL;
	} else {
		dev_info(dev, "%s: google,usb-port-tz-name is %s\n", __func__,
			pdata->usb_tz_name);
	}
#endif

	ret = of_property_read_u32(np_hl7132, "hl7132,max-init-retry",
				   &pdata->max_init_retry);
	if (ret)
		pdata->max_init_retry = HL7132_MAX_INIT_RETRY_DFT;
	dev_info(dev, "%s: hl7132,max-init-retry is %u\n", __func__, pdata->max_init_retry);

	return 0;
}
#else
static int of_hl7132_dt(struct device *dev,
			 struct hl7132_platform_data *pdata)
{
	return 0;
}
#endif /* CONFIG_OF */

#ifdef CONFIG_THERMAL
static int hl7132_usb_tz_read_temp(struct thermal_zone_device *tzd, int *temp)
{
	struct hl7132_charger *hl7132 = tzd->devdata;

	if (hl7132)
		return -ENODEV;

	*temp = 0; /* TODO hl7132_read_adc(hl7132, ADCCH_TDIE); for v2 */

	return 0;
}

static struct thermal_zone_device_ops hl7132_usb_tzd_ops = {
	.get_temp = hl7132_usb_tz_read_temp,
};
#endif

static int read_reg(void *data, u64 *val)
{
	struct hl7132_charger *chip = data;
	int rc;
	unsigned int temp;

	rc = regmap_read(chip->regmap, chip->debug_address, &temp);
	if (rc) {
		dev_err(chip->dev, "Couldn't read reg %x rc = %d\n",
			chip->debug_address, rc);
		return -EAGAIN;
	}
	*val = temp;
	return 0;
}

static int write_reg(void *data, u64 val)
{
	struct hl7132_charger *chip = data;
	int rc;
	u8 temp;

	temp = (u8) val;
	rc = regmap_write(chip->regmap, chip->debug_address, temp);
	if (rc) {
		dev_err(chip->dev, "Couldn't write 0x%02x to 0x%02x rc = %d\n",
			temp, chip->debug_address, rc);
		return -EAGAIN;
	}
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(register_debug_ops, read_reg, write_reg, "0x%02llx\n");

static int debug_ftm_mode_get(void *data, u64 *val)
{
	struct hl7132_charger *hl7132 = data;
	*val = hl7132->ftm_mode;
	return 0;
}

static int debug_ftm_mode_set(void *data, u64 val)
{
	struct hl7132_charger *hl7132 = data;

	if (val) {
		hl7132->ftm_mode = true;
		hl7132->ta_type = TA_TYPE_USBPD;
		hl7132->chg_mode = CHG_2TO1_DC_MODE;
	} else {
		hl7132->ftm_mode = false;
	}

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(debug_ftm_mode_ops, debug_ftm_mode_get, debug_ftm_mode_set, "%llu\n");

static int debug_ta_max_vol_set(void *data, u64 val)
{
	struct hl7132_charger *hl7132 = data;

	hl7132->pdata->ta_max_vol = val;
	hl7132->pdata->ta_max_vol_cp = val;

	hl7132->ta_max_vol = val * hl7132->chg_mode;

	return 0;
}

static int debug_ta_max_vol_get(void *data, u64 *val)
{
	struct hl7132_charger *hl7132 = data;

	*val = hl7132->pdata->ta_max_vol;
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(debug_ta_max_vol_ops, debug_ta_max_vol_get,
			debug_ta_max_vol_set, "%llu\n");

static ssize_t sts_ab_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hl7132_charger *hl7132 = dev_get_drvdata(dev);
	u8 tmp[2];
	int ret;

	ret = regmap_bulk_read(hl7132->regmap, HL7132_REG_INT_STS_A, &tmp, sizeof(tmp));
	if (ret < 0)
		return ret;

	return scnprintf(buf, PAGE_SIZE, "%02x%02x\n", tmp[0], tmp[1]);
}

static DEVICE_ATTR_RO(sts_ab);

static ssize_t registers_dump_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct hl7132_charger *hl7132 = dev_get_drvdata(dev);
	u8 tmp[HL7132_MAX_REGISTER + 1];
	int ret, i;
	int len = 0;

	ret = regmap_bulk_read(hl7132->regmap, HL7132_REG_DEVICE_ID, &tmp,
			       HL7132_MAX_REGISTER + 1);
	if (ret < 0)
		return ret;

	for (i = 0; i <= HL7132_MAX_REGISTER; i++)
		len += scnprintf(&buf[len], PAGE_SIZE - len, "%02x: %02x\n", i, tmp[i]);

	return len;
}

static DEVICE_ATTR_RO(registers_dump);

static int hl7132_create_fs_entries(struct hl7132_charger *chip)
{

	device_create_file(chip->dev, &dev_attr_sts_ab);
	device_create_file(chip->dev, &dev_attr_registers_dump);

	chip->debug_root = debugfs_create_dir("charger-hl7132", NULL);
	if (IS_ERR_OR_NULL(chip->debug_root)) {
		dev_err(chip->dev, "Couldn't create debug dir\n");
		return -ENOENT;
	}

	debugfs_create_file("data", 0644, chip->debug_root, chip, &register_debug_ops);
	debugfs_create_x32("address", 0644, chip->debug_root, &chip->debug_address);

	debugfs_create_file("ta_vol_max", 0644, chip->debug_root, chip,
			   &debug_ta_max_vol_ops);

	debugfs_create_file("ftm_mode", 0644, chip->debug_root, chip,
			    &debug_ftm_mode_ops);

	return 0;
}


static int hl7132_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	static char *battery[] = { "hl7132-battery" };
	struct power_supply_config mains_cfg = {};
	struct hl7132_platform_data *pdata;
	struct hl7132_charger *hl7132_chg;
	struct device *dev = &client->dev;
	const char *psy_name = NULL;
	int ret;

	dev_info(dev, "starting hl7132 probe\n");

	hl7132_chg = devm_kzalloc(dev, sizeof(*hl7132_chg), GFP_KERNEL);
	if (!hl7132_chg)
		return -ENOMEM;

#if defined(CONFIG_OF)
	if (client->dev.of_node) {
		pdata = devm_kzalloc(&client->dev,
				     sizeof(struct hl7132_platform_data),
				     GFP_KERNEL);
		if (!pdata) {
			dev_err(dev, "Failed to allocate memory\n");
			return -ENOMEM;
		}

		ret = of_hl7132_dt(dev, pdata);
		if (ret < 0) {
			dev_err(dev, "Failed to get device of_node\n");
			return -ENOMEM;
		}

		client->dev.platform_data = pdata;
	} else {
		pdata = client->dev.platform_data;
	}
#else
	pdata = dev->platform_data;
#endif
	if (!pdata)
		return -EINVAL;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(dev, "%s: check_functionality failed.",
			__func__);
		ret = -ENODEV;
		goto error;
	}

	hl7132_chg->regmap = devm_regmap_init_i2c(client, &hl7132_regmap);
	if (IS_ERR(hl7132_chg->regmap)) {
		ret = PTR_ERR(hl7132_chg->regmap);
		dev_err(dev, "regmap init failed, err = %d\n", ret);
		goto error;
	}
	i2c_set_clientdata(client, hl7132_chg);

	mutex_init(&hl7132_chg->lock);
	hl7132_chg->dev = &client->dev;
	hl7132_chg->pdata = pdata;
	hl7132_chg->charging_state = DC_STATE_NO_CHARGING;

	/* Create a work queue for the direct charger */
	hl7132_chg->dc_wq = alloc_ordered_workqueue("hl7132_dc_wq", WQ_MEM_RECLAIM);
	if (hl7132_chg->dc_wq == NULL) {
		dev_err(dev, "failed to create work queue\n");
		return -ENOMEM;
	}

	hl7132_chg->monitor_wake_lock =
		wakeup_source_register(NULL, "hl7132-charger-monitor");
	if (!hl7132_chg->monitor_wake_lock) {
		dev_err(dev, "Failed to register wakeup source\n");
		return -ENODEV;
	}

	ret = of_property_read_string(dev->of_node,
				      "hl7132,psy_name", &psy_name);
	if ((ret == 0) && (strlen(psy_name) > 0))
		hl7132_regmap.name = hl7132_mains_desc.psy_dsc.name =
		    devm_kstrdup(dev, psy_name, GFP_KERNEL);

	ret = hl7132_hw_ping(hl7132_chg);
	if (ret)
		goto error;

	/* TODO: only enable ADC if usb_tz_name is defined */
	hl7132_chg->hw_init_done = false;
	ret = hl7132_hw_init(hl7132_chg);
	if (ret == 0)
		hl7132_chg->hw_init_done = true;
	else
		goto error;

	mains_cfg.supplied_to = battery;
	mains_cfg.num_supplicants = ARRAY_SIZE(battery);
	mains_cfg.drv_data = hl7132_chg;
	hl7132_chg->mains = devm_power_supply_register(dev,
							&hl7132_mains_desc.psy_dsc,
							&mains_cfg);
	if (IS_ERR(hl7132_chg->mains)) {
		ret = -ENODEV;
		goto error;
	}

	ret = hl7132_create_fs_entries(hl7132_chg);
	if (ret < 0)
		dev_err(dev, "error while registering debugfs %d\n", ret);

#if IS_ENABLED(CONFIG_THERMAL)
	if (pdata->usb_tz_name) {
		hl7132_chg->usb_tzd =
			thermal_zone_device_register(pdata->usb_tz_name, 0, 0,
						     hl7132_chg,
						     &hl7132_usb_tzd_ops,
						     NULL, 0, 0);
		if (IS_ERR(hl7132_chg->usb_tzd)) {
			hl7132_chg->usb_tzd = NULL;
			ret = PTR_ERR(hl7132_chg->usb_tzd);
			dev_err(dev,
				"Couldn't register usb connector thermal zone ret=%d\n",
				ret);
		}
	}
#endif

	hl7132_chg->dc_avail = NULL;

	hl7132_chg->init_done = true;
	dev_info(dev, "hl7132: probe_done\n");
	dev_dbg(dev, "%s: =========END=========\n", __func__);
	return 0;

error:
	destroy_workqueue(hl7132_chg->dc_wq);
	debugfs_remove(hl7132_chg->debug_root);
	mutex_destroy(&hl7132_chg->lock);
	wakeup_source_unregister(hl7132_chg->monitor_wake_lock);
	return ret;
}

static void hl7132_remove(struct i2c_client *client)
{
	struct hl7132_charger *hl7132_chg = i2c_get_clientdata(client);

	destroy_workqueue(hl7132_chg->dc_wq);
	debugfs_remove(hl7132_chg->debug_root);
	mutex_destroy(&hl7132_chg->lock);
	wakeup_source_unregister(hl7132_chg->monitor_wake_lock);

#if IS_ENABLED(CONFIG_THERMAL)
	if (hl7132_chg->usb_tzd)
		thermal_zone_device_unregister(hl7132_chg->usb_tzd);
#endif
	pps_free(&hl7132_chg->pps_data);
}

static const struct i2c_device_id hl7132_id[] = {
	{ "hl7132", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, hl7132_id);

#if defined(CONFIG_OF)
static struct of_device_id hl7132_i2c_dt_ids[] = {
	{ .compatible = "hl,hl7132" },
	{ },
};
MODULE_DEVICE_TABLE(of, hl7132_i2c_dt_ids);
#endif /* CONFIG_OF */

#if defined(CONFIG_PM)
#ifdef CONFIG_RTC_HCTOSYS
static int get_current_time(unsigned long *now_tm_sec)
{
	struct rtc_time tm;
	struct rtc_device *rtc;
	int rc;

	rtc = rtc_class_open(CONFIG_RTC_HCTOSYS_DEVICE);
	if (rtc == NULL) {
		pr_err("%s: unable to open rtc device (%s)\n",
			__FILE__, CONFIG_RTC_HCTOSYS_DEVICE);
		return -EINVAL;
	}

	rc = rtc_read_time(rtc, &tm);
	if (rc) {
		pr_err("Error reading rtc device (%s) : %d\n",
			CONFIG_RTC_HCTOSYS_DEVICE, rc);
		goto close_time;
	}

	rc = rtc_valid_tm(&tm);
	if (rc) {
		pr_err("Invalid RTC time (%s): %d\n",
			CONFIG_RTC_HCTOSYS_DEVICE, rc);
		goto close_time;
	}

	*now_tm_sec = rtc_tm_to_time64(&tm);

close_time:
	rtc_class_close(rtc);
	return rc;
}

static void
hl7132_check_and_update_charging_timer(struct hl7132_charger *hl7132)
{
	unsigned long current_time = 0;

	get_current_time(&current_time);

	hl7132->last_update_time = current_time;
}
#endif

static int hl7132_suspend(struct device *dev)
{
	return 0;
}

static int hl7132_resume(struct device *dev)
{
	struct hl7132_charger *hl7132 = dev_get_drvdata(dev);

	dev_dbg(hl7132->dev, "%s: update_timer\n", __func__);

	/* Update the current timer */
#ifdef CONFIG_RTC_HCTOSYS
	hl7132_check_and_update_charging_timer(hl7132);
#endif
	return 0;
}
#else
#define hl7132_suspend		NULL
#define hl7132_resume		NULL
#endif

const struct dev_pm_ops hl7132_pm_ops = {
	.suspend = hl7132_suspend,
	.resume = hl7132_resume,
};

static struct i2c_driver hl7132_driver = {
	.driver = {
		.name = "hl7132",
#if defined(CONFIG_OF)
		.of_match_table = hl7132_i2c_dt_ids,
#endif /* CONFIG_OF */
#if defined(CONFIG_PM)
		.pm = &hl7132_pm_ops,
#endif
	},
	.probe        = hl7132_probe,
	.remove       = hl7132_remove,
	.id_table     = hl7132_id,
};

module_i2c_driver(hl7132_driver);

MODULE_AUTHOR("Baltazar Ortiz <baltazarortiz@google.com>");
MODULE_DESCRIPTION("HL7132 gcharger driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("3.7.0");

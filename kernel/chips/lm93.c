/*
    lm93.c - Part of lm_sensors, Linux kernel modules for hardware monitoring

    Author/Maintainer: Mark M. Hoffman <mhoffman@lightlink.com>
	Copyright (c) 2004 Utilitek Systems, Inc.

    derived in part from lm78.c:
	Copyright (c) 1998, 1999  Frodo Looijaard <frodol@dds.nl> 

    derived in part from lm85.c:
	Copyright (c) 2002, 2003 Philip Pokorny <ppokorny@penguincomputing.com>
	Copyright (c) 2003       Margit Schubert-While <margitsw@t-online.de>

    derived in part from w83l785ts.c:
	Copyright (c) 2003-2004 Jean Delvare <khali@linux-fr.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#define DEBUG 1

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/ioport.h>
#include <linux/i2c.h>
#include <linux/i2c-proc.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <asm/io.h>
#include <asm/bitops.h>
#include "version.h"
#include "sensors_vid.h"

#ifndef I2C_DRIVERID_LM93
#define I2C_DRIVERID_LM93 1049
#endif

/* I2C addresses to scan */
static unsigned short normal_i2c[] = { SENSORS_I2C_END };
static unsigned short normal_i2c_range[] = { 0x2c, 0x2e, SENSORS_I2C_END };

/* ISA addresses to scan (none) */
static unsigned int normal_isa[] = { SENSORS_ISA_END };
static unsigned int normal_isa_range[] = { SENSORS_ISA_END };

/* module parameters */
SENSORS_INSMOD_1(lm93);

static int disable_block /* = 0 */ ;
MODULE_PARM(disable_block, "i");
MODULE_PARM_DESC(disable_block,
	"Set to non-zero to disable SMBus block data transactions.");

static int init = 1;
MODULE_PARM(init, "i");
MODULE_PARM_DESC(init, "Set to zero to bypass chip initialization");

static int vccp_limit_type[2] /* = {0,0} */ ;
MODULE_PARM(vccp_limit_type, "2-2i");
MODULE_PARM_DESC(vccp_limit_type, "Configures in7 and in8 limit modes");

/* SMBus capabilities */
#define LM93_SMBUS_FUNC_FULL (I2C_FUNC_SMBUS_BYTE_DATA | \
		I2C_FUNC_SMBUS_WORD_DATA | I2C_FUNC_SMBUS_BLOCK_DATA)
#define LM93_SMBUS_FUNC_MIN  (I2C_FUNC_SMBUS_BYTE_DATA | \
		I2C_FUNC_SMBUS_WORD_DATA)

/* LM93 REGISTER ADDRESSES */

/* miscellaneous */
#define LM93_REG_MFR_ID			0x3e
#define LM93_REG_VER			0x3f
#define LM93_REG_STATUS_CONTROL		0xe2
#define LM93_REG_CONFIG			0xe3
#define LM93_REG_SLEEP_CONTROL		0xe4

/* alarm values start here */
#define LM93_REG_HOST_ERROR_1		0x48

/* voltage inputs: in1-in16 (nr => 0-15) */
#define LM93_REG_IN(nr)			(0x56 + (nr))
#define LM93_REG_IN_MIN(nr)		(0x90 + (nr) * 2)
#define LM93_REG_IN_MAX(nr)		(0x91 + (nr) * 2)

/* temperature inputs: temp1-temp3 (nr => 0-2) */
#define LM93_REG_TEMP(nr)		(0x50 + (nr))
#define LM93_REG_TEMP_MIN(nr)		(0x78 + (nr) * 2)
#define LM93_REG_TEMP_MAX(nr)		(0x79 + (nr) * 2)

/* temp[1-4]_auto_boost (nr => 0-3) */
#define LM93_REG_TEMP_FAN_BOOST(nr)	(0x80 + (nr))

/* #PROCHOT inputs: prochot1-prochot2 (nr => 0-1) */
#define LM93_REG_PROCHOT_CUR(nr)	(0x67 + (nr) * 2)
#define LM93_REG_PROCHOT_AVG(nr)	(0x68 + (nr) * 2)
#define LM93_REG_PROCHOT_MAX(nr)	(0xb0 + (nr))

/* fan tach inputs: fan1-fan4 (nr => 0-3) */
#define LM93_REG_FAN(nr)		(0x6e + (nr) * 2)
#define LM93_REG_FAN_MIN(nr)		(0xb4 + (nr) * 2)

/* pwm outputs: pwm1-pwm2 (nr => 0-1, reg => 0-3) */
#define LM93_REG_PWM_CTL(nr,reg)	(0xc8 + (reg) + (nr) * 4)
#define LM93_PWM_CTL1	0
#define LM93_PWM_CTL2	1
#define LM93_PWM_CTL3	2
#define LM93_PWM_CTL4	3

/* GPIO input state */
#define LM93_REG_GPI			0x6b

/* vid inputs: vid1-vid2 (nr => 0-1) */
#define LM93_REG_VID(nr)		(0x6c + (nr))

/* vccp1 & vccp2: VID relative inputs (nr => 0-1) */
#define LM93_REG_VCCP_LIMIT_OFF(nr)	(0xb2 + (nr))

/* temp[1-4]_auto_boost_hyst */
#define LM93_REG_BOOST_HYST_12		0xc0
#define LM93_REG_BOOST_HYST_34		0xc1

/* temp[1-4]_auto_pwm_[min|hyst] */
#define LM93_REG_MIN_PWM_HYST_12	0xc3
#define LM93_REG_MIN_PWM_HYST_34	0xc4

/* prochot_override & prochot_interval */
#define LM93_REG_PROCHOT_OVERRIDE	0xc6
#define LM93_REG_PROCHOT_INTERVAL	0xc7

/* temp[1-4]_auto_base (nr => 0-3) */
#define LM93_REG_TEMP_BASE(nr)		(0xd0 + (nr))

/* temp[1-4]_auto_offsets (step => 0-11) */
#define LM93_REG_TEMP_OFFSET(step)	(0xd4 + (step))

/* #PROCHOT & #VRDHOT PWM ramp control */
#define LM93_REG_PWM_RAMP_CTL		0xbf

/* miscellaneous */
#define LM93_REG_SFC1		0xbc
#define LM93_REG_SFC2		0xbd
#define LM93_REG_SF_TACH_TO_PWM	0xe0

/* LM93 REGISTER VALUES */
#define LM93_MFR_ID		0x73
#define LM93_MFR_ID_PROTOTYPE	0x72

/* LM93 BLOCK READ COMMANDS */
static const struct { u8 cmd; u8 len; } lm93_block_read_cmds[12] = {
	{ 0xf2,  8 },
	{ 0xf3,  8 },
	{ 0xf4,  6 },
	{ 0xf5, 16 },
	{ 0xf6,  4 },
	{ 0xf7,  8 },
	{ 0xf8, 12 },
	{ 0xf9, 32 },
	{ 0xfa,  8 },
	{ 0xfb,  8 },
	{ 0xfc, 16 },
	{ 0xfd,  9 },
};

/* CONVERSIONS */

/* fan tach register to/from tach values */
static const u8 lm93_tach_reg_to_value[16] =
	{ 0x00, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xa0,
	  0xb0, 0xc0, 0xd0, 0xe0, 0xf0, 0xff, 0x00, 0x00 };

/* min, max, and nominal voltage readings, per channel (mV)*/
static const unsigned long lm93_vin_val_min[16] = {
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 3000,
};
static const unsigned long lm93_vin_val_max[16] = {
	1236, 1236, 1236, 1600, 2000, 2000, 1600, 1600,
	4400, 6667, 3333, 2625, 1312, 1312, 1236, 3600,
};
/*
static const unsigned long lm93_vin_val_nom[16] = {
	 927,  927,  927, 1200, 1500, 1500, 1200, 1200,
	3300, 5000, 2500, 1969,  984,  984,  309, 3300,
};
*/

/* min, max, and nominal register values, per channel (u8) */
static const u8 lm93_vin_reg_min[16] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xae, 
};
static const u8 lm93_vin_reg_max[16] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 
	0xff, 0xfa, 0xff, 0xff, 0xff, 0xff, 0xff, 0xd1, 
};
/*
static const u8 lm93_vin_reg_nom[16] = {
	0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0,
	0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0xc0, 0x40, 0xc0,
};
*/

/* IN: 1/100 V, limits determined by channel nr
   REG: scaling determined by channel nr */
static u8 LM93_IN_TO_REG(int nr, unsigned val)
{
	/* range limit */
	const long mV = SENSORS_LIMIT(val * 10, 
		lm93_vin_val_min[nr], lm93_vin_val_max[nr]);

	/* try not to lose too much precision here */
	const long uV = mV * 1000;
	const long uV_max = lm93_vin_val_max[nr] * 1000;
	const long uV_min = lm93_vin_val_min[nr] * 1000;

	/* convert */
	const long slope = (uV_max - uV_min) / 
		(lm93_vin_reg_max[nr] - lm93_vin_reg_min[nr]);
	const long intercept = uV_min - slope * lm93_vin_reg_min[nr];

	u8 result = ((uV - intercept + (slope/2)) / slope);
	result = SENSORS_LIMIT(result, 
			lm93_vin_reg_min[nr], lm93_vin_reg_max[nr]);
	return result;
}

static unsigned LM93_IN_FROM_REG(int nr, u8 reg)
{
	const long uV_max = lm93_vin_val_max[nr] * 1000;
	const long uV_min = lm93_vin_val_min[nr] * 1000;

	const long slope = (uV_max - uV_min) /
		(lm93_vin_reg_max[nr] - lm93_vin_reg_min[nr]);
	const long intercept = uV_min - slope * lm93_vin_reg_min[nr];

	return (slope * reg + intercept + 5000) / 10000;
}

/* vid in mV , upper == 0 indicates low limit, otherwise upper limit 
   upper also determines which nibble of the register is returned
   (the other nibble will be 0x0) */
static u8 LM93_IN_REL_TO_REG(unsigned val, int upper, int vid)
{
	long uV_offset = vid * 1000 - val * 10000;
	if (upper) {
		uV_offset = SENSORS_LIMIT(uV_offset, 12500, 200000);
		return (u8)((uV_offset /  12500 - 1) << 4);
	} else {
		uV_offset = SENSORS_LIMIT(uV_offset, -400000, -25000);
		return (u8)((uV_offset / -25000 - 1) << 0);
	}
}
	
/* vid in mV, upper == 0 indicates low limit, otherwise upper limit */
static unsigned LM93_IN_REL_FROM_REG(u8 reg, int upper, int vid)
{
	const long uV_offset = upper ? (((reg >> 4 & 0x0f) + 1) * 12500) :
				(((reg >> 0 & 0x0f) + 1) * -25000);
	const long uV_vid = vid * 1000;
	return (uV_vid + uV_offset + 5000) / 10000;
}

#define LM93_TEMP_MIN (-1280)
#define LM93_TEMP_MAX ( 1270)

/* TEMP: 1/10 degrees C (-128C to +127C)
   REG: 1C/bit, two's complement */
static u8 LM93_TEMP_TO_REG(int temp)
{
	int ntemp = SENSORS_LIMIT(temp, LM93_TEMP_MIN, LM93_TEMP_MAX);
	ntemp += (ntemp<0 ? -5 : 5);
	return (u8)(ntemp / 10);
}

static int LM93_TEMP_FROM_REG(u8 reg)
{
	return (s8)reg * 10;
}

/* RPM: (82.5 to 1350000)
   REG: 14-bits, LE, *left* justified */
static u16 LM93_FAN_TO_REG(long rpm)
{
	u16 count, regs;

	if (rpm == 0) {
		count = 0x3fff;
	} else {
		rpm = SENSORS_LIMIT(rpm, 1, 1000000);
		count = SENSORS_LIMIT((1350000 + rpm) / rpm, 1, 0x3ffe);
	}

	/* <TODO> is this byte-order correct? */
	regs = count << 2;
	return cpu_to_le16(regs);
}

static int LM93_FAN_FROM_REG(u16 regs)
{
	/* <TODO> is this byte-order correct? */
	const u16 count = le16_to_cpu(regs) >> 2;

	/* <TODO> do we need further divider of 2 here? */
	return count==0 ? -1 : count==0x3fff ? 0: 1350000 / count;
}

/* VID:	mV
   REG: 6-bits, right justified, *always* using Intel VRM/VRD 10 */
static int LM93_VID_FROM_REG(u8 reg)
{
	return vid_from_reg((reg & 0x3f), 100);
}

/* PROCHOT: 0-255, 0 => 0%, 255 => > 96.6%
 * REG: (same) */
static u8 LM93_PROCHOT_TO_REG(long prochot)
{
	prochot = SENSORS_LIMIT(prochot, 0, 255);
	return (u8)prochot;
}

/* PROCHOT-OVERRIDE; 0-15, 0 is 6.25%, 15 is 99.88%
 * REG: (same) */
static u8 LM93_PROCHOT_OVERRIDE_TO_REG(int force1, int force2, long prochot)
{
	u8 result = 0;

	result |= force1 ? 0x80 : 0x00;
	result |= force2 ? 0x40 : 0x00;
	prochot = SENSORS_LIMIT(prochot, 0, 15);
	result |= prochot;
	return result;
}

/* PROCHOT-INTERVAL: 73 - 37200 (1/100 seconds)
 * REG: 0-9 as mapped below */
static int lm93_interval_map[10] = {
	73, 146, 290, 580, 1170, 2330, 4660, 9320, 18600, 37200,
};

static int LM93_INTERVAL_FROM_REG(u8 reg)
{
	return lm93_interval_map[reg & 0x0f];
}

/* round up to nearest match */
static u8 LM93_INTERVAL_TO_REG(long interval)
{
        int i;
	for (i = 0; i < 9; i++)
		if (interval <= lm93_interval_map[i])
			break;

	/* can fall through with i==9 */
	return (u8)i;
}

/* PWM: 0-255 per sensors documentation
   REG: 0-13 as mapped below... *left* justified */
typedef enum { LM93_PWM_MAP_HI_FREQ, LM93_PWM_MAP_LO_FREQ } pwm_freq_t;
static int lm93_pwm_map[2][14] = {
	{
		0x00, /*   0.00% */ 0x40, /*  25.00% */
		0x50, /*  31.25% */ 0x60, /*  37.50% */
		0x70, /*  43.75% */ 0x80, /*  50.00% */
		0x90, /*  56.25% */ 0xa0, /*  62.50% */
		0xb0, /*  68.75% */ 0xc0, /*  75.00% */
		0xd0, /*  81.25% */ 0xe0, /*  87.50% */
		0xf0, /*  93.75% */ 0xff, /* 100.00% */
	},
	{
		0x00, /*   0.00% */ 0x40, /*  25.00% */
		0x49, /*  28.57% */ 0x52, /*  32.14% */
		0x5b, /*  35.71% */ 0x64, /*  39.29% */
		0x6d, /*  42.86% */ 0x76, /*  46.43% */
		0x80, /*  50.00% */ 0x89, /*  53.57% */
		0x92, /*  57.14% */ 0xb6, /*  71.43% */
		0xdb, /*  85.71% */ 0xff, /* 100.00% */
	},
};

static int LM93_PWM_FROM_REG(u8 reg, pwm_freq_t freq)
{
	return lm93_pwm_map[freq][reg >> 4 & 0x0f];
}

/* round up to nearest match */
static u8 LM93_PWM_TO_REG(int pwm, pwm_freq_t freq)
{
	int i;
	for (i = 0; i < 13; i++)
		if (pwm <= lm93_pwm_map[freq][i])
			break;

	/* can fall through with i==13 */
	return (u8)(i << 4);
}

/* PWM FREQ: HZ
   REG: 0-7 as mapped below */
static int lm93_pwm_freq_map[8] = {
	22500, 96, 84, 72, 60, 48, 36, 12
};

static int LM93_PWM_FREQ_FROM_REG(u8 reg)
{
	return lm93_pwm_freq_map[reg & 0x07];
}

/* round up to nearest match */
static u8 LM93_PWM_FREQ_TO_REG(int freq)
{
	int i;
	for (i = 7; i > 0; i--)
		if (freq <= lm93_pwm_freq_map[i])
			break;

	/* can fall through with i==0 */
	return (u8)i;
}

/* GPIO: 0-255, GPIO0 is LSB
 * REG: inverted */
static unsigned LM93_GPI_FROM_REG(u8 reg)
{
	return ~reg & 0xff;
}

/* ALARMS: SYSCTL format described further below
   REG: 64 bits in 8 registers, as immediately below */
struct block1_t {
	u8 host_status_1;
	u8 host_status_2;
	u8 host_status_3;
	u8 host_status_4;
	u8 p1_prochot_status;
	u8 p2_prochot_status;
	u8 gpi_status;
	u8 fan_status;
};

static unsigned LM93_ALARMS_FROM_REG(struct block1_t b1)
{
	unsigned result;
	result  = b1.host_status_2;
	result |= b1.host_status_3 << 8;
	result |= (b1.fan_status & 0x04) << 16;
	result |= (b1.p1_prochot_status & 0x80) << 13;
	result |= (b1.p2_prochot_status & 0x80) << 14;
	result |= (b1.host_status_4 & 0xfc) << 20;
	result |= (b1.host_status_1 & 0x07) << 28;
	return result;
}

/* For each registered client, we need to keep some data in memory. That
   data is pointed to by client->data. The structure itself is dynamically
   allocated, at the same time the client itself is allocated. */

struct lm93_data {
	struct i2c_client client;
	struct semaphore lock;
	int sysctl_id;
	enum chips type;

	struct semaphore update_lock;
	unsigned long last_updated;	/* In jiffies */

	/* client update function */
	void (*update)(struct lm93_data *, struct i2c_client *);

	char valid; /* !=0 if following fields are valid */

	/* register values, arranged by block read groups */
	struct block1_t block1;

	/* temp1 - temp4: unfiltered readings
	   temp1 - temp2: filtered readings */
	u8 block2[6];

	/* vin1 - vin16: readings */
	u8 block3[16];

	/* prochot1 - prochot2: readings */
	struct { u8 cur; u8 avg; } block4[2];

	/* fan counts 1-4 => 14-bits, LE, *left* justified */
	u16 block5[4];

	/* block6 has a lot of data we don't need */
	struct { u8 min; u8 max; } temp_lim[3];

	/* vin1 - vin16: low and high limits */
	struct { u8 min; u8 max; } block7[16];

	/* fan count limits 1-4 => same format as block5 */
	u16 block8[4];

	/* pwm control registers (2 pwms, 4 regs) */
	u8 block9[2][4];

	/* master config register */
	u8 config;

	/* VID1 & VID2 => register format, 6-bits, right justified */
	u8 vid[2];

	/* prochot1 - prochot2: limits */
	u8 prochot_max[2];

	/* vccp1 & vccp2 (in7 & in8): VID relative limits (register format) */
	u8 vccp_limits[2];

	/* GPIO input state (register format, i.e. inverted) */
	u8 gpi;

	/* #PROCHOT override (register format) */
	u8 prochot_override;

	/* #PROCHOT intervals (register format) */
	u8 prochot_interval;

	/* miscellaneous setup regs */
	u8 sfc1;
	u8 sfc2;
	u8 sf_tach_to_pwm;

	/* The two PWM CTL2  registers can read something other than what was
	   last written for the OVR_DC field (duty cycle override).  So, we
	   save the user-commanded value here. */
	u8 pwm_override[2];
};


static int lm93_attach_adapter(struct i2c_adapter *adapter);
static int lm93_detect(struct i2c_adapter *adapter, int address,
		unsigned short flags, int kind);
static int lm93_detach_client(struct i2c_client *client);

static u8 lm93_read_byte(struct i2c_client *client, u8 register);
static int lm93_write_byte(struct i2c_client *client, u8 register, u8 value);
static u16 lm93_read_word(struct i2c_client *client, u8 register);
static int lm93_write_word(struct i2c_client *client, u8 register, u16 value);
static void lm93_update_client(struct i2c_client *client);
static void lm93_update_client_full(struct lm93_data *data,
		struct i2c_client *client);
static void lm93_update_client_min (struct lm93_data *data,
		struct i2c_client *client);

static void lm93_init_client(struct i2c_client *client);


static void lm93_in(struct i2c_client *client, int operation, int ctl_name,
		int *nrels_mag, long *results);
static void lm93_temp(struct i2c_client *client, int operation,
		int ctl_name, int *nrels_mag, long *results);
static void lm93_fan(struct i2c_client *client, int operation,
		int ctl_name, int *nrels_mag, long *results);
static void lm93_pwm(struct i2c_client *client, int operation,
		int ctl_name, int *nrels_mag, long *results);
static void lm93_pwm_freq(struct i2c_client *client, int operation,
		int ctl_name, int *nrels_mag, long *results);
static void lm93_fan_smart_tach(struct i2c_client *client, int operation,
		int ctl_name, int *nrels_mag, long *results);
static void lm93_vid(struct i2c_client *client, int operation,
		int ctl_name, int *nrels_mag, long *results);
static void lm93_prochot(struct i2c_client *client, int operation,
		int ctl_name, int *nrels_mag, long *results);
static void lm93_prochot_short(struct i2c_client *client, int operation,
		int ctl_name, int *nrels_mag, long *results);
static void lm93_prochot_override(struct i2c_client *client, int operation,
		int ctl_name, int *nrels_mag, long *results);
static void lm93_prochot_interval(struct i2c_client *client, int operation,
		int ctl_name, int *nrels_mag, long *results);
static void lm93_vrdhot(struct i2c_client *client, int operation,
		int ctl_name, int *nrels_mag, long *results);
static void lm93_gpio(struct i2c_client *client, int operation,
		int ctl_name, int *nrels_mag, long *results);
static void lm93_alarms(struct i2c_client *client, int operation,
		int ctl_name, int *nrels_mag, long *results);

static struct i2c_driver lm93_driver = {
	.owner		= THIS_MODULE,
	.name		= "LM93 sensor driver",
	.id		= I2C_DRIVERID_LM93,
	.flags		= I2C_DF_NOTIFY,
	.attach_adapter	= lm93_attach_adapter,
	.detach_client	= lm93_detach_client,
};

/* unique ID for each LM93 detected */
static int lm93_id = 0;

/* -- SENSORS SYSCTL START -- */
/* volts * 100 */
#define LM93_SYSCTL_IN1		1001
#define LM93_SYSCTL_IN2 	1002
#define LM93_SYSCTL_IN3 	1003
#define LM93_SYSCTL_IN4 	1004
#define LM93_SYSCTL_IN5 	1005
#define LM93_SYSCTL_IN6 	1006
#define LM93_SYSCTL_IN7 	1007
#define LM93_SYSCTL_IN8 	1008
#define LM93_SYSCTL_IN9 	1009
#define LM93_SYSCTL_IN10 	1010
#define LM93_SYSCTL_IN11 	1011
#define LM93_SYSCTL_IN12 	1012
#define LM93_SYSCTL_IN13 	1013
#define LM93_SYSCTL_IN14 	1014
#define LM93_SYSCTL_IN15 	1015
#define LM93_SYSCTL_IN16 	1016

/* degrees celcius * 10 */
#define LM93_SYSCTL_TEMP1	1101
#define LM93_SYSCTL_TEMP2	1102
#define LM93_SYSCTL_TEMP3	1103

/* rotations/minute */
#define LM93_SYSCTL_FAN1	1201
#define LM93_SYSCTL_FAN2	1202
#define LM93_SYSCTL_FAN3	1203
#define LM93_SYSCTL_FAN4	1204

/* 1-2 => enable smart tach mode associated with this pwm #, or disable */
#define LM93_SYSCTL_FAN1_SMART_TACH	1205
#define LM93_SYSCTL_FAN2_SMART_TACH	1205
#define LM93_SYSCTL_FAN3_SMART_TACH	1207
#define LM93_SYSCTL_FAN4_SMART_TACH	1208

/* volts * 1000 */
#define LM93_SYSCTL_VID1	1301
#define LM93_SYSCTL_VID2	1302

/* 0 => off, 255 => 100% */
#define LM93_SYSCTL_PWM1	1401
#define LM93_SYSCTL_PWM2	1402

/* Hz */
#define LM93_SYSCTL_PWM1_FREQ	1403
#define LM93_SYSCTL_PWM2_FREQ	1404

/* 0 => 0%, 255 => > 99.6% */
#define LM93_SYSCTL_PROCHOT1	1501
#define LM93_SYSCTL_PROCHOT2	1502

/* !0 => enable #PROCHOT logical short */
#define LM93_SYSCTL_PROCHOT_SHORT 1503

/* 2 boolean enable/disable, 3rd value indicates duty cycle */
#define LM93_SYSCTL_PROCHOT_OVERRIDE	1504

/* 2 values, 0-9 */
#define LM93_SYSCTL_PROCHOT_INTERVAL	1505

/* GPIO input (bitmask) */
#define LM93_SYSCTL_GPIO	1601

/* #VRDHOT input (boolean) */
#define LM93_SYSCTL_VRDHOT1	1701
#define LM93_SYSCTL_VRDHOT2	1702

/* alarms (bitmask) */
#define LM93_SYSCTL_ALARMS	2001	/* bitvector */

/* alarm bitmask definitions
   The LM93 has nearly 64 bits of error status... I've pared that down to
   what I think is a useful subset in order to fit it into 32 bits.

   Especially note that the #VRD_HOT alarms are missing because we provide
   that information as values in another /proc file.

   If libsensors is extended to support 64 bit values, this could be revisited.
*/
#define LM93_ALARM_IN1		0x00000001
#define LM93_ALARM_IN2		0x00000002
#define LM93_ALARM_IN3		0x00000004
#define LM93_ALARM_IN4		0x00000008
#define LM93_ALARM_IN5		0x00000010
#define LM93_ALARM_IN6		0x00000020
#define LM93_ALARM_IN7		0x00000040
#define LM93_ALARM_IN8		0x00000080
#define LM93_ALARM_IN9		0x00000100
#define LM93_ALARM_IN10		0x00000200
#define LM93_ALARM_IN11		0x00000400
#define LM93_ALARM_IN12		0x00000800
#define LM93_ALARM_IN13		0x00001000
#define LM93_ALARM_IN14		0x00002000
#define LM93_ALARM_IN15		0x00004000
#define LM93_ALARM_IN16		0x00008000
#define LM93_ALARM_FAN1		0x00010000
#define LM93_ALARM_FAN2		0x00020000
#define LM93_ALARM_FAN3		0x00040000
#define LM93_ALARM_FAN4		0x00080000
#define LM93_ALARM_PH1_ERR	0x00100000
#define LM93_ALARM_PH2_ERR	0x00200000
#define LM93_ALARM_SCSI1_ERR	0x00400000
#define LM93_ALARM_SCSI2_ERR	0x00800000
#define LM93_ALARM_DVDDP1_ERR	0x01000000
#define LM93_ALARM_DVDDP2_ERR	0x02000000
#define LM93_ALARM_D1_ERR	0x04000000
#define LM93_ALARM_D2_ERR	0x08000000
#define LM93_ALARM_TEMP1	0x10000000
#define LM93_ALARM_TEMP2	0x20000000
#define LM93_ALARM_TEMP3	0x40000000

/* -- SENSORS SYSCTL END -- */

/* These files are created for each detected LM93. This is just a template;
   though at first sight, you might think we could use a statically
   allocated list, we need some way to get back to the parent - which
   is done through one of the 'extra' fields which are initialized 
   when a new copy is allocated. */

#define LM93_SYSCTL_IN(nr)   {LM93_SYSCTL_IN##nr, "in" #nr, NULL, 0, \
	0644, NULL, &i2c_proc_real, &i2c_sysctl_real, NULL, &lm93_in}
#define LM93_SYSCTL_TEMP(nr) {LM93_SYSCTL_TEMP##nr, "temp" #nr, NULL, 0, \
	0644, NULL, &i2c_proc_real, &i2c_sysctl_real, NULL, &lm93_temp}
#define LM93_SYSCTL_PWM(nr)  {LM93_SYSCTL_PWM##nr, "pwm" #nr, NULL, 0, \
	0644, NULL, &i2c_proc_real, &i2c_sysctl_real, NULL, &lm93_pwm}
#define LM93_SYSCTL_PWM_FREQ(nr)  {LM93_SYSCTL_PWM##nr##_FREQ, \
	"pwm" #nr "_freq", NULL, 0, 0644, NULL, &i2c_proc_real, \
	&i2c_sysctl_real, NULL, &lm93_pwm_freq}
#define LM93_SYSCTL_FAN(nr)  {LM93_SYSCTL_FAN##nr, "fan" #nr, NULL, 0, \
	0644, NULL, &i2c_proc_real, &i2c_sysctl_real, NULL, &lm93_fan}
#define LM93_SYSCTL_VID(nr)  {LM93_SYSCTL_VID##nr, "vid" #nr, NULL, 0, \
	0444, NULL, &i2c_proc_real, &i2c_sysctl_real, NULL, &lm93_vid}
#define LM93_SYSCTL_PROCHOT(nr) {LM93_SYSCTL_PROCHOT##nr, "prochot" #nr, NULL, \
	0, 0644, NULL, &i2c_proc_real, &i2c_sysctl_real, NULL, &lm93_prochot}
#define LM93_SYSCTL_VRDHOT(nr) {LM93_SYSCTL_VRDHOT##nr, "vrdhot" #nr, NULL, \
	0, 0444, NULL, &i2c_proc_real, &i2c_sysctl_real, NULL, &lm93_vrdhot}
#define LM93_SYSCTL_FAN_SMART_TACH(nr) {LM93_SYSCTL_FAN##nr##_SMART_TACH, \
	"fan" #nr "_smart_tach", NULL, 0, 0644, NULL, &i2c_proc_real, \
	&i2c_sysctl_real, NULL, &lm93_fan_smart_tach}
static ctl_table lm93_dir_table_template[] = {
	LM93_SYSCTL_IN(1),
	LM93_SYSCTL_IN(2),
	LM93_SYSCTL_IN(3),
	LM93_SYSCTL_IN(4),
	LM93_SYSCTL_IN(5),
	LM93_SYSCTL_IN(6),
	LM93_SYSCTL_IN(7),
	LM93_SYSCTL_IN(8),
	LM93_SYSCTL_IN(9),
	LM93_SYSCTL_IN(10),
	LM93_SYSCTL_IN(11),
	LM93_SYSCTL_IN(12),
	LM93_SYSCTL_IN(13),
	LM93_SYSCTL_IN(14),
	LM93_SYSCTL_IN(15),
	LM93_SYSCTL_IN(16),

	LM93_SYSCTL_TEMP(1),
	LM93_SYSCTL_TEMP(2),
	LM93_SYSCTL_TEMP(3),

	LM93_SYSCTL_FAN(1),
	LM93_SYSCTL_FAN(2),
	LM93_SYSCTL_FAN(3),
	LM93_SYSCTL_FAN(4),

	LM93_SYSCTL_FAN_SMART_TACH(1),
	LM93_SYSCTL_FAN_SMART_TACH(2),
	LM93_SYSCTL_FAN_SMART_TACH(3),
	LM93_SYSCTL_FAN_SMART_TACH(4),

	LM93_SYSCTL_PWM(1),
	LM93_SYSCTL_PWM(2),

	LM93_SYSCTL_PWM_FREQ(1),
	LM93_SYSCTL_PWM_FREQ(2),

	LM93_SYSCTL_VID(1),
	LM93_SYSCTL_VID(2),

	LM93_SYSCTL_PROCHOT(1),
	LM93_SYSCTL_PROCHOT(2),

	{LM93_SYSCTL_PROCHOT_SHORT, "prochot_short", NULL, 0, 0644, NULL,
	 &i2c_proc_real, &i2c_sysctl_real, NULL, &lm93_prochot_short},

	{LM93_SYSCTL_PROCHOT_OVERRIDE, "prochot_override", NULL, 0, 0644, NULL,
	 &i2c_proc_real, &i2c_sysctl_real, NULL, &lm93_prochot_override},

	{LM93_SYSCTL_PROCHOT_INTERVAL, "prochot_interval", NULL, 0, 0644, NULL,
	 &i2c_proc_real, &i2c_sysctl_real, NULL, &lm93_prochot_interval},

	LM93_SYSCTL_VRDHOT(1),
	LM93_SYSCTL_VRDHOT(2),

	{LM93_SYSCTL_GPIO, "gpio", NULL, 0, 0444, NULL,
	 &i2c_proc_real, &i2c_sysctl_real, NULL, &lm93_gpio},

	{LM93_SYSCTL_ALARMS, "alarms", NULL, 0, 0444, NULL, &i2c_proc_real,
	 &i2c_sysctl_real, NULL, &lm93_alarms},

	{0}
};


/* This function is called when:
     * lm93_driver is inserted (when this module is loaded), for each
       available adapter
     * when a new adapter is inserted (and lm93_driver is still present) */
static int lm93_attach_adapter(struct i2c_adapter *adapter)
{
	return i2c_detect(adapter, &addr_data, lm93_detect);
}

/* This function is called by i2c_detect */
int lm93_detect(struct i2c_adapter *adapter, int address,
		unsigned short flags, int kind)
{
	int err, func;
	struct lm93_data *data;
	struct i2c_client *client;
	void (*update)(struct lm93_data *, struct i2c_client *);

	/* lm93 is SMBus only */
	if (i2c_is_isa_adapter(adapter)) {
		pr_debug("lm93.o: detect failed, "
				"cannot attach to legacy adapter!\n");
		err = -ENODEV;
		goto ERROR0;
	}

	/* choose update routine based on bus capabilities */
	func = i2c_get_functionality(adapter);

	if ( ((LM93_SMBUS_FUNC_FULL & func) == LM93_SMBUS_FUNC_FULL) &&
			(!disable_block) ) {
		pr_debug("lm93.o: using SMBus block data transactions\n");
		update = lm93_update_client_full;
	} else if ((LM93_SMBUS_FUNC_MIN & func) == LM93_SMBUS_FUNC_MIN) {
		pr_debug("lm93.o: disabled SMBus block data transactions\n");
		update = lm93_update_client_min;
	} else {
		pr_debug("lm93.o: detect failed, "
				"smbus byte and/or word data not supported!\n");
		err = -ENODEV;
		goto ERROR0;
	}

	/* OK. For now, we presume we have a valid client. We now create the
	   client structure, even though we cannot fill it completely yet.
	   But it allows us to access lm78_{read,write}_value. */

	if (!(data = kmalloc(sizeof(struct lm93_data), GFP_KERNEL))) {
		pr_debug("lm93.o: detect failed, kmalloc failed!\n");
		err = -ENOMEM;
		goto ERROR0;
	}

	client = &data->client;
	client->addr = address;
	init_MUTEX(&data->lock);
	client->data = data;
	client->adapter = adapter;
	client->driver = &lm93_driver;
	client->flags = 0;

	/* detection */
	if (kind < 0) {
		int mfr = lm93_read_byte(client, LM93_REG_MFR_ID);

		if (mfr != 0x01) {
			pr_debug("lm93.o: detect failed, "
				"bad manufacturer id 0x%02x!\n", mfr);
			err = -ENODEV;
			goto ERROR1;
		}
	}

	if (kind <= 0) {
		int ver = lm93_read_byte(client, LM93_REG_VER);

		if ((ver == LM93_MFR_ID) || (ver == LM93_MFR_ID_PROTOTYPE)) {
			kind = lm93;
		} else {
			pr_debug("lm93.o: detect failed, "
				"bad version id 0x%02x!\n", ver);
			if (kind == 0)
				pr_debug("lm93.o: "
					"(ignored 'force' parameter)\n");
			err = -ENODEV;
			goto ERROR1;
		}
	}

	/* fill in remaining client fields */
	strcpy(client->name, "LM93 chip");
	client->id = lm93_id++;
	pr_debug("lm93.o: assigning ID %d to %s at %d,0x%02x\n", client->id,
		client->name, i2c_adapter_id(client->adapter), client->addr);

	/* housekeeping */
	data->type = kind;
	data->valid = 0;
	data->update = update;
	init_MUTEX(&data->update_lock);

	/* tell the I2C layer a new client has arrived */
	if ((err = i2c_attach_client(client)))
		goto ERROR1;

	/* initialize the chip */
	lm93_init_client(client);

	/* register a new directory entry with module sensors */
	if ((data->sysctl_id = i2c_register_entry(client, "lm93", 
			lm93_dir_table_template)) < 0) {
		err = data->sysctl_id;
		goto ERROR2;
	}

	return 0;

ERROR2:
	i2c_detach_client(client);
ERROR1:
	kfree(data);
ERROR0:
	return err;
}

static int lm93_detach_client(struct i2c_client *client)
{
	int err;
	struct lm93_data *data = client->data;

	i2c_deregister_entry(data->sysctl_id);

	if ((err = i2c_detach_client(client))) {
		printk (KERN_ERR "lm93.o: Client deregistration failed; "
			"client not detached.\n");
		return err;
	}

	return 0;
}

#define MAX_RETRIES 5

static u8 lm93_read_byte(struct i2c_client *client, u8 reg)
{
	int value, i;

	/* retry in case of read errors */
	for (i=1; i<=MAX_RETRIES; i++) {
		if ((value = i2c_smbus_read_byte_data(client, reg)) >= 0) {
			return value;
		} else {
			printk(KERN_WARNING "lm93.o: read byte data failed, "
				"address 0x%02x.\n", reg);
			mdelay(i);
		}

	}

	/* <TODO> what to return in case of error? */
	return 0;
}

static int lm93_write_byte(struct i2c_client *client, u8 reg, u8 value)
{
	int result;

	/* <TODO> how to handle write errors? */
	result = i2c_smbus_write_byte_data(client, reg, value);

	if (result < 0)
		printk(KERN_WARNING "lm93.o: write byte data failed, "
			"0x%02x at address 0x%02x.\n", value, reg);

	return result;
}

static u16 lm93_read_word(struct i2c_client *client, u8 reg)
{
	int value, i;

	/* retry in case of read errors */
	for (i=1; i<=MAX_RETRIES; i++) {
		if ((value = i2c_smbus_read_word_data(client, reg)) >= 0) {
			return value;
		} else {
			printk(KERN_WARNING "lm93.o: read word data failed, "
				"address 0x%02x.\n", reg);
			mdelay(i);
		}

	}

	/* <TODO> what to return in case of error? */
	return 0;
}

static int lm93_write_word(struct i2c_client *client, u8 reg, u16 value)
{
	int result;

	/* <TODO> how to handle write errors? */
	result = i2c_smbus_write_word_data(client, reg, value);

	if (result < 0)
		printk(KERN_WARNING "lm93.o: write word data failed, "
			"0x%04x at address 0x%02x.\n", value, reg);

	return result;
}

static u8 lm93_block_buffer[I2C_SMBUS_BLOCK_MAX];

/*
	read block data into values, retry if not expected length
	fbn => index to lm93_block_read_cmds table
		(Fixed Block Number - section 14.5.2 of LM93 datasheet)
*/
static void lm93_read_block(struct i2c_client *client, u8 fbn, u8 *values)
{
	int i, result;

	for (i = 1; i <= MAX_RETRIES; i++) {
		result = i2c_smbus_read_block_data(client, 
			lm93_block_read_cmds[fbn].cmd, lm93_block_buffer);

		if (result != lm93_block_read_cmds[fbn].len) {
			printk(KERN_WARNING "lm93.o: block read data failed, "
				"command 0x%02x.\n", 
				lm93_block_read_cmds[fbn].cmd);
			mdelay(1);
		}
	}

	if (result == lm93_block_read_cmds[fbn].len) {
		memcpy(values,lm93_block_buffer,lm93_block_read_cmds[fbn].len);
	} else {
		/* <TODO> what to do in case of error? */
	}
}

static void lm93_init_client(struct i2c_client *client)
{
	int i;
	u8 reg = lm93_read_byte(client, LM93_REG_CONFIG);

	/* start monitoring */
	lm93_write_byte(client, LM93_REG_CONFIG, reg | 0x01);

	if (init) {
		/* enable #ALERT pin */
		reg = lm93_read_byte(client, LM93_REG_CONFIG);
		lm93_write_byte(client, LM93_REG_CONFIG, reg | 0x08);

		/* enable ASF mode for BMC status registers */
		reg = lm93_read_byte(client, LM93_REG_STATUS_CONTROL);
		lm93_write_byte(client, LM93_REG_STATUS_CONTROL, reg | 0x02);

		/* set sleep state to S0 */
		lm93_write_byte(client, LM93_REG_SLEEP_CONTROL, 0);
	}
		
	/* spin until ready */
	for (i=0; i<20; i++) {
		mdelay(10);
		if ((lm93_read_byte(client, LM93_REG_CONFIG) & 0x80) == 0x80)	
			return;
	}

	printk(KERN_WARNING "lm93.o: timed out waiting for sensor "
			"chip to signal ready!\n");
}

static void lm93_update_client(struct i2c_client *client)
{
	struct lm93_data *data = client->data;

	down(&data->update_lock);

	if (time_after(jiffies - data->last_updated, HZ + HZ / 2) ||
		time_before(jiffies, data->last_updated) || !data->valid) {

		data->update(data, client);
		data->last_updated = jiffies;
		data->valid = 1;
	}

	up(&data->update_lock);
}

/* update routine for data that has no corresponding SMBus block data command */
static void lm93_update_client_common(struct lm93_data *data,
		struct i2c_client *client)
{
	int i;

	/* temp1 - temp3: limits */
	for (i = 0; i < 3; i++) {
		data->temp_lim[i].min =
			lm93_read_byte(client, LM93_REG_TEMP_MIN(i));
		data->temp_lim[i].max =
			lm93_read_byte(client, LM93_REG_TEMP_MAX(i));
	}

	/* config register */
	data->config = lm93_read_byte(client, LM93_REG_CONFIG);

	/* vid1 - vid2: values */
	for (i = 0; i < 2; i++)
		data->vid[i] = lm93_read_byte(client, LM93_REG_VID(i));

	/* prochot1 - prochot2: limits */
	for (i = 0; i < 2; i++)
		data->prochot_max[i] = lm93_read_byte(client,
				LM93_REG_PROCHOT_MAX(i));

	/* vccp1 - vccp2: VID relative limits */
	for (i = 0; i < 2; i++)
		data->vccp_limits[i] = lm93_read_byte(client,
				LM93_REG_VCCP_LIMIT_OFF(i));

	/* GPIO input state */
	data->gpi = lm93_read_byte(client, LM93_REG_GPI);

	/* #PROCHOT override state */
	data->prochot_override = lm93_read_byte(client,
			LM93_REG_PROCHOT_OVERRIDE);

	/* #PROCHOT intervals */
	data->prochot_interval = lm93_read_byte(client,
			LM93_REG_PROCHOT_INTERVAL);

	/* misc setup registers */
	data->sfc1 = lm93_read_byte(client, LM93_REG_SFC1);
	data->sfc2 = lm93_read_byte(client, LM93_REG_SFC2);
	data->sf_tach_to_pwm = lm93_read_byte(client,
			LM93_REG_SF_TACH_TO_PWM);
}

/* update routine which uses SMBus block data commands */
static void lm93_update_client_full(struct lm93_data *data,
		struct i2c_client *client)
{
	pr_debug("lm93.o: starting device update (block data enabled)\n");

	/* in1 - in16: values & limits */
	lm93_read_block(client, 3, (u8 *)(data->block3));
	lm93_read_block(client, 7, (u8 *)(data->block7));

	/* temp1 - temp3: values */
	lm93_read_block(client, 2, (u8 *)(data->block2));

	/* prochot1 - prochot2: values */
	lm93_read_block(client, 4, (u8 *)(data->block4));

	/* fan1 - fan4: values & limits */
	lm93_read_block(client, 5, (u8 *)(data->block5));
	lm93_read_block(client, 8, (u8 *)(data->block8));

	/* pmw control registers */
	lm93_read_block(client, 9, (u8 *)(data->block9));

	/* alarm values */
	lm93_read_block(client, 1, (u8 *)(&data->block1));

	lm93_update_client_common(data, client);
}

/* update routine which uses SMBus byte/word data commands only */
static void lm93_update_client_min(struct lm93_data *data,
		struct i2c_client *client)
{
	int i,j;
	u8 *ptr;

	pr_debug("lm93.o: starting device update (block data disabled)\n");

	/* in1 - in16: values & limits */
	for (i = 0; i < 16; i++) {
		data->block3[i] = 
			lm93_read_byte(client, LM93_REG_IN(i));
		data->block7[i].min =
			lm93_read_byte(client, LM93_REG_IN_MIN(i));
		data->block7[i].max =
			lm93_read_byte(client, LM93_REG_IN_MAX(i));
	}

	/* temp1 - temp3: values */
	for (i = 0; i < 4; i++) {
		data->block2[i] =
			lm93_read_byte(client, LM93_REG_TEMP(i));
	}

	/* prochot1 - prochot2: values */
	for (i = 0; i < 2; i++) {
		data->block4[i].cur =
			lm93_read_byte(client, LM93_REG_PROCHOT_CUR(i));
		data->block4[i].avg =
			lm93_read_byte(client, LM93_REG_PROCHOT_AVG(i));
	}

	/* fan1 - fan4: values & limits */
	for (i = 0; i < 4; i++) {
		data->block5[i] =
			lm93_read_word(client, LM93_REG_FAN(i));
		data->block8[i] =
			lm93_read_word(client, LM93_REG_FAN_MIN(i));
	}

	/* pwm control registers */
	for (i = 0; i < 2; i++) {
		for (j = 0; j < 4; j++) {
			data->block9[i][j] =
				lm93_read_word(client, LM93_REG_PWM_CTL(i,j));
		}
	}

	/* alarm values */
	ptr = &data->block1;
	for (i = 0, ptr = &data->block1; i < 8; i++) {
		*(ptr + i) =
			lm93_read_word(client, LM93_REG_HOST_ERROR_1 + i);
	}

	lm93_update_client_common(data, client);
}

/* The next few functions are the call-back functions of the /proc/sys and
   sysctl files. Which function is used is defined in the ctl_table in
   the extra1 field.
   Each function must return the magnitude (power of 10 to divide the date
   with) if it is called with operation==SENSORS_PROC_REAL_INFO. It must
   put a maximum of *nrels elements in results reflecting the data of this
   file, and set *nrels to the number it actually put in it, if operation==
   SENSORS_PROC_REAL_READ. Finally, it must get upto *nrels elements from
   results and write them to the chip, if operations==SENSORS_PROC_REAL_WRITE.
   Note that on SENSORS_PROC_REAL_READ, I do not check whether results is
   large enough (by checking the incoming value of *nrels). This is not very
   good practice, but as long as you put less than about 5 values in results,
   you can assume it is large enough. */

static void lm93_in(struct i2c_client *client, int operation, int ctl_name,
		int *nrels_mag, long *results)
{
	struct lm93_data *data = client->data;
	int nr = ctl_name - LM93_SYSCTL_IN1; /* 0 <= nr <= 15 */
	int vccp = ctl_name - LM93_SYSCTL_IN7; /* 0 <= vccp <= 1 if relevant */

	if (operation == SENSORS_PROC_REAL_INFO)
		*nrels_mag = 2;
	else if (operation == SENSORS_PROC_REAL_READ) {

		lm93_update_client(client);

		/* for limits, check in7 and in8 for VID relative mode */
		if ((ctl_name==LM93_SYSCTL_IN7 || ctl_name==LM93_SYSCTL_IN8) &&
				(vccp_limit_type[vccp])) {
			long vid = LM93_VID_FROM_REG(data->vid[vccp]);
			results[0] = LM93_IN_REL_FROM_REG(
				data->vccp_limits[vccp], 0, vid);
			results[1] = LM93_IN_REL_FROM_REG(
				data->vccp_limits[vccp], 1, vid);

		/* otherwise, use absolute limits */
		} else {
			results[0] = LM93_IN_FROM_REG(nr,
					data->block7[nr].min);
			results[1] = LM93_IN_FROM_REG(nr,
					data->block7[nr].max);
		}

		results[2] = LM93_IN_FROM_REG(nr, data->block3[nr]);
		*nrels_mag = 3;
	} else if (operation == SENSORS_PROC_REAL_WRITE) {
		down(&data->update_lock);

		/* for limits, check in7 and in8 for VID relative mode */
		if ((ctl_name==LM93_SYSCTL_IN7 || ctl_name==LM93_SYSCTL_IN8) &&
				(vccp_limit_type[vccp])) {

			long vid = LM93_VID_FROM_REG(data->vid[vccp]);
			if (*nrels_mag >= 2) {
				data->vccp_limits[vccp] =
					(data->vccp_limits[vccp] & 0x0f) |
					LM93_IN_REL_TO_REG(results[1], 1, vid);
			}
			if (*nrels_mag >= 1) {
				data->vccp_limits[vccp] = 
					(data->vccp_limits[vccp] & 0xf0) |
					LM93_IN_REL_TO_REG(results[0], 0, vid);
				lm93_write_byte(client,
						LM93_REG_VCCP_LIMIT_OFF(vccp),
						data->vccp_limits[vccp]);
			}

		/* otherwise, use absolute limits */
		} else {
			if (*nrels_mag >= 1) {
				data->block7[nr].min = LM93_IN_TO_REG(nr,
						results[0]);
				lm93_write_byte(client, LM93_REG_IN_MIN(nr),
						data->block7[nr].min);
			}
			if (*nrels_mag >= 2) {
				data->block7[nr].max = LM93_IN_TO_REG(nr,
						results[1]);
				lm93_write_byte(client, LM93_REG_IN_MAX(nr),
					 	data->block7[nr].max);
			}
		}
		up(&data->update_lock);
	}
}

void lm93_temp(struct i2c_client *client, int operation, int ctl_name,
		int *nrels_mag, long *results)
{
	struct lm93_data *data = client->data;
	int nr = ctl_name - LM93_SYSCTL_TEMP1;

	if (0 > nr || nr > 2)
		return; /* ERROR */

	if (operation == SENSORS_PROC_REAL_INFO)
		*nrels_mag = 1;
	else if (operation == SENSORS_PROC_REAL_READ) {
		lm93_update_client(client);
		results[0] = LM93_TEMP_FROM_REG(data->temp_lim[nr].max);
		results[1] = LM93_TEMP_FROM_REG(data->temp_lim[nr].min);
		results[2] = LM93_TEMP_FROM_REG(data->block2[nr]);
		*nrels_mag = 3;
	} else if (operation == SENSORS_PROC_REAL_WRITE) {
		down(&data->update_lock);
		if (*nrels_mag >= 1) {
			data->temp_lim[nr].max = LM93_TEMP_TO_REG(results[0]);
			lm93_write_byte(client, LM93_REG_TEMP_MAX(nr),
					 data->temp_lim[nr].max);
		}
		if (*nrels_mag >= 2) {
			data->temp_lim[nr].min = LM93_TEMP_TO_REG(results[1]);
			lm93_write_byte(client, LM93_REG_TEMP_MIN(nr),
					 data->temp_lim[nr].min);
		}
		up(&data->update_lock);
	}
}

void lm93_fan(struct i2c_client *client, int operation, int ctl_name,
		int *nrels_mag, long *results)
{
	struct lm93_data *data = client->data;
	int nr = ctl_name - LM93_SYSCTL_FAN1;

	if (0 > nr || nr > 3)
		return; /* ERROR */

	if (operation == SENSORS_PROC_REAL_INFO)
		*nrels_mag = 0;
	else if (operation == SENSORS_PROC_REAL_READ) {
		lm93_update_client(client);
		results[0] = LM93_FAN_FROM_REG(data->block8[nr]); /* min */
		results[1] = LM93_FAN_FROM_REG(data->block5[nr]); /* val */
		*nrels_mag = 2;
	} else if (operation == SENSORS_PROC_REAL_WRITE) {
		if (*nrels_mag >= 1) {
			down(&data->update_lock);
			data->block8[nr] = LM93_FAN_TO_REG(results[0]);
			lm93_write_word(client, LM93_REG_FAN_MIN(nr),
					data->block8[nr]);
			up(&data->update_lock);
		}
	}
}

void lm93_vid(struct i2c_client *client, int operation, int ctl_name,
	      int *nrels_mag, long *results)
{
	struct lm93_data *data = client->data;
	int nr = ctl_name - LM93_SYSCTL_VID1;

	if (0 > nr || nr > 1)
		return; /* ERROR */

	if (operation == SENSORS_PROC_REAL_INFO)
		*nrels_mag = 2;
	else if (operation == SENSORS_PROC_REAL_READ) {
		lm93_update_client(client);
		results[0] = LM93_VID_FROM_REG(data->vid[nr]);
		*nrels_mag = 1;
	}
}

/* some tedious bit-twiddling here to deal with the register format:

	data->sf_tach_to_pwm: (tach to pwm mapping bits)

		bit |  7  |  6  |  5  |  4  |  3  |  2  |  1  |  0
		     T4:P2 T4:P1 T3:P2 T3:P1 T2:P2 T2:P1 T1:P2 T1:P1

	data->sfc2: (enable bits)

		bit |  3  |  2  |  1  |  0
		       T4    T3    T2    T1
*/

/* helper function - must grab data->update_lock before calling
   fan is 0-3, indicating fan1-fan4 */
static void lm93_write_fan_smart_tach(struct i2c_client *client,
	struct lm93_data *data, int fan, long value)
{
	/* insert the new mapping and write it out */
	data->sf_tach_to_pwm = lm93_read_byte(client, LM93_REG_SF_TACH_TO_PWM);
	data->sf_tach_to_pwm &= ~0x3 << fan * 2;
	data->sf_tach_to_pwm |= value << fan * 2;
	lm93_write_byte(client, LM93_REG_SF_TACH_TO_PWM, data->sf_tach_to_pwm);

	/* insert the enable bit and write it out */
	data->sfc2 = lm93_read_byte(client, LM93_REG_SFC2);
	if (value)
		data->sfc2 |= 1 << fan;
	else
		data->sfc2 &= ~1 << fan;
	lm93_write_byte(client, LM93_REG_SFC2, data->sfc2);
}

/* helper function - must grab data->update_lock before calling
   pwm is 0-1, indicating pwm1-pwm2
   this disables smart tach for all tach channels bound to the given pwm */
static void lm93_disable_fan_smart_tach(struct i2c_client *client,
	struct lm93_data *data, int pwm)
{
	int mapping = lm93_read_byte(client, LM93_REG_SF_TACH_TO_PWM);
	int mask;

	/* collapse the mapping into a mask of enable bits */
	mapping = (mapping >> pwm) & 0x55;
	mask = mapping & 0x01;
	mask |= (mapping & 0x04) >> 1;
	mask |= (mapping & 0x10) >> 2;
	mask |= (mapping & 0x40) >> 3;

	/* disable smart tach according to the mask */
	data->sfc2 = lm93_read_byte(client, LM93_REG_SFC2);
	data->sfc2 &= ~mask;
	lm93_write_byte(client, LM93_REG_SFC2, data->sfc2);
}

void lm93_fan_smart_tach(struct i2c_client *client, int operation, int ctl_name,
	int *nrels_mag, long *results)
{
	struct lm93_data *data = client->data;
	int nr = ctl_name - LM93_SYSCTL_FAN1_SMART_TACH;

	if (0 > nr || nr > 3)
		return; /* ERROR */

	if (operation == SENSORS_PROC_REAL_INFO)
		*nrels_mag = 0;
	else if (operation == SENSORS_PROC_REAL_READ) {
		lm93_update_client(client);

		/* extract the relevant mapping */
		int mapping = (data->sf_tach_to_pwm >> (nr * 2)) & 0x03;

		/* if there's a mapping and it's enabled */
		if (mapping && ((data->sfc2 >> nr) & 0x01))
			results[0] = mapping;
		else
			results[0] = 0;

		*nrels_mag = 1;

	} else if (operation == SENSORS_PROC_REAL_WRITE) {
		if (*nrels_mag >= 1) {
			down(&data->update_lock);

			/* sanity test, ignore the write otherwise */
			if (0 <= results[0] && results[0] <= 2) {

				/* can't enable if pwm freq is 22.5KHz */
				if (results[0]) {
					u8 ctl4 = lm93_read_byte(client,
						LM93_REG_PWM_CTL(results[0]-1,
						LM93_PWM_CTL4));
					if ((ctl4 & 0x07) == 0)
						results[0] = 0;
				}

				lm93_write_fan_smart_tach(client, data, nr,
						results[0]);
			}
			up(&data->update_lock);
		}
	}
}

void lm93_prochot(struct i2c_client *client, int operation, int ctl_name,
		int *nrels_mag, long *results)
{
	struct lm93_data *data = client->data;
	int nr = ctl_name - LM93_SYSCTL_PROCHOT1;

	if (0 > nr || nr > 1)
		return; /* ERROR */

	if (operation == SENSORS_PROC_REAL_INFO)
		*nrels_mag = 0;
	else if (operation == SENSORS_PROC_REAL_READ) {
		lm93_update_client(client);
		results[0] = data->prochot_max[nr];
		results[1] = data->block4[nr].avg;
		results[2] = data->block4[nr].avg;
		*nrels_mag = 3;
	} else if (operation == SENSORS_PROC_REAL_WRITE) {
		down(&data->update_lock);
		if (*nrels_mag >= 1) {
			data->prochot_max[nr] = LM93_PROCHOT_TO_REG(results[0]);
			lm93_write_byte(client, LM93_REG_PROCHOT_MAX(nr),
				data->prochot_max[nr]);
		}
		up(&data->update_lock);
	}
}

void lm93_prochot_short(struct i2c_client *client, int operation, int ctl_name,
	int *nrels_mag, long *results)
{
	struct lm93_data *data = client->data;

	if (ctl_name != LM93_SYSCTL_PROCHOT_SHORT)
		return; /* ERROR */

	if (operation == SENSORS_PROC_REAL_INFO)
		*nrels_mag = 0;
	else if (operation == SENSORS_PROC_REAL_READ) {
		lm93_update_client(client);
		results[0] = (data->config & 0x10) ? 1 : 0;
		*nrels_mag = 1;
	} else if (operation == SENSORS_PROC_REAL_WRITE) {
		down(&data->update_lock);
		if (*nrels_mag >= 1) {
			if (results[0])
				data->config |= 0x10;
			else
				data->config &= ~0x10;
			lm93_write_byte(client, LM93_REG_CONFIG, data->config);
		}
		up(&data->update_lock);
	}
}

void lm93_prochot_override(struct i2c_client *client, int operation,
	int ctl_name, int *nrels_mag, long *results)
{
	struct lm93_data *data = client->data;

	if (ctl_name != LM93_SYSCTL_PROCHOT_OVERRIDE)
		return; /* ERROR */

	if (operation == SENSORS_PROC_REAL_INFO)
		*nrels_mag = 0;
	else if (operation == SENSORS_PROC_REAL_READ) {
		lm93_update_client(client);
		results[0] = (data->prochot_override & 0x80) ? 1 : 0;
		results[1] = (data->prochot_override & 0x40) ? 1 : 0;
		results[2] = data->prochot_override & 0x0f;
		*nrels_mag = 3;
	} else if (operation == SENSORS_PROC_REAL_WRITE) {

		/* grab old values */
		int force2 = (data->prochot_override & 0x40) ? 1 : 0;
		int prochot = data->prochot_override & 0x0f;

		down(&data->update_lock);
		if (*nrels_mag >= 3) {
			data->prochot_override = LM93_PROCHOT_OVERRIDE_TO_REG(
				results[0], results[1], results[2]);
		}
		if (*nrels_mag == 2) {
			data->prochot_override = LM93_PROCHOT_OVERRIDE_TO_REG(
				results[0], results[1], prochot);
		}
		if (*nrels_mag == 1) {
			data->prochot_override = LM93_PROCHOT_OVERRIDE_TO_REG(
				results[0], force2, prochot);
		}
		if (*nrels_mag >= 1) {
			lm93_write_byte(client, LM93_REG_PROCHOT_OVERRIDE,
				data->prochot_override);
		}
		up(&data->update_lock);
	}
}

void lm93_prochot_interval(struct i2c_client *client, int operation,
	int ctl_name, int *nrels_mag, long *results)
{
	struct lm93_data *data = client->data;

	if (ctl_name != LM93_SYSCTL_PROCHOT_INTERVAL)
		return; /* ERROR */

	if (operation == SENSORS_PROC_REAL_INFO)
		*nrels_mag = 2;
	else if (operation == SENSORS_PROC_REAL_READ) {
		lm93_update_client(client);
		results[0] = LM93_INTERVAL_FROM_REG(
			data->prochot_interval & 0x0f);
		results[1] = LM93_INTERVAL_FROM_REG(
			(data->prochot_interval & 0xf0) >> 4);
		*nrels_mag = 2;
	} else if (operation == SENSORS_PROC_REAL_WRITE) {
		down(&data->update_lock);
		if (*nrels_mag >= 2) {
			results[1] = SENSORS_LIMIT(results[1], 0, 9);
			data->prochot_interval =
				(LM93_INTERVAL_TO_REG(results[1] << 4)) |
				(data->prochot_interval & 0x0f);
		}
		if (*nrels_mag >= 1) {
			results[0] = SENSORS_LIMIT(results[0], 0, 9);
			data->prochot_interval =
				(data->prochot_interval & 0xf0) |
				LM93_INTERVAL_TO_REG(results[0]);
			lm93_write_byte(client, LM93_REG_PROCHOT_INTERVAL,
				data->prochot_interval);
		}
		up(&data->update_lock);
	}
}

void lm93_vrdhot(struct i2c_client *client, int operation, int ctl_name,
	int *nrels_mag, long *results)
{
	struct lm93_data *data = client->data;
	int nr = ctl_name - LM93_SYSCTL_VRDHOT1;

	if (0 > nr || nr > 1)
		return; /* ERROR */

	if (operation == SENSORS_PROC_REAL_INFO)
		*nrels_mag = 0;
	else if (operation == SENSORS_PROC_REAL_READ) {
		lm93_update_client(client);
		results[0] = data->block1.host_status_1 & (1 << (nr+4)) ? 1 : 0;
		*nrels_mag = 1;
	}
}

void lm93_gpio(struct i2c_client *client, int operation, int ctl_name,
	int *nrels_mag, long *results)
{
	struct lm93_data *data = client->data;

	if (ctl_name != LM93_SYSCTL_GPIO)
		return; /* ERROR */

	if (operation == SENSORS_PROC_REAL_INFO)
		*nrels_mag = 0;
	else if (operation == SENSORS_PROC_REAL_READ) {
		lm93_update_client(client);
		results[0] = LM93_GPI_FROM_REG(data->gpi);
		*nrels_mag = 1;
	}
}

void lm93_pwm(struct i2c_client *client, int operation, int ctl_name,
	int *nrels_mag, long *results)
{
	struct lm93_data *data = client->data;
	int nr = ctl_name - LM93_SYSCTL_PWM1;
	u8 ctl2, ctl4;

	if (0 > nr || nr > 1)
		return; /* ERROR */

	if (operation == SENSORS_PROC_REAL_INFO)
		*nrels_mag = 0;
	else if (operation == SENSORS_PROC_REAL_READ) {
		lm93_update_client(client);
		ctl2 = data->block9[nr][LM93_PWM_CTL2];
		ctl4 = data->block9[nr][LM93_PWM_CTL4];
		results[1] = (ctl2 & 0x01) ? 1 : 0;
		if (results[1]) /* show user commanded value if enabled */
			results[0] = data->pwm_override[nr];
		else /* show present h/w value if manual pwm disabled */
			results[0] = LM93_PWM_FROM_REG(ctl2, (ctl4 & 0x07) ?
				LM93_PWM_MAP_LO_FREQ : LM93_PWM_MAP_HI_FREQ);
		*nrels_mag = 2;
	}
	else if (operation == SENSORS_PROC_REAL_WRITE) {
		if (*nrels_mag >= 1) {
			down(&data->update_lock);
			ctl2 = lm93_read_byte(
				client, LM93_REG_PWM_CTL(nr,LM93_PWM_CTL2));
			ctl4 = lm93_read_byte(
				client, LM93_REG_PWM_CTL(nr,LM93_PWM_CTL4));
			ctl2 = (ctl2 & 0x0f) | 
				LM93_PWM_TO_REG(results[0], (ctl4 & 0x07) ?
					LM93_PWM_MAP_LO_FREQ :
					LM93_PWM_MAP_HI_FREQ);
			if (*nrels_mag >= 2) {
				if (results[1])
					ctl2 |= 0x01;
				else
					ctl2 &= ~0x01;
			}
			/* save user commanded value */
			data->pwm_override[nr] =
				LM93_PWM_FROM_REG(ctl2, (ctl4 & 0x07) ?
					LM93_PWM_MAP_LO_FREQ :
					LM93_PWM_MAP_HI_FREQ);
			lm93_write_byte(client,
				LM93_REG_PWM_CTL(nr,LM93_PWM_CTL2), ctl2);
			up(&data->update_lock);
		}
	}
}

void lm93_pwm_freq(struct i2c_client *client, int operation, int ctl_name,
	int *nrels_mag, long *results)
{
	struct lm93_data *data = client->data;
	int nr = ctl_name - LM93_SYSCTL_PWM1_FREQ;
	u8 ctl4;

	if (0 > nr || nr > 1)
		return; /* ERROR */

	if (operation == SENSORS_PROC_REAL_INFO)
		*nrels_mag = 0;
	else if (operation == SENSORS_PROC_REAL_READ) {
		lm93_update_client(client);
		ctl4 = data->block9[nr][LM93_PWM_CTL4];
		results[0] = LM93_PWM_FREQ_FROM_REG(ctl4);
		*nrels_mag = 1;
	}
	else if (operation == SENSORS_PROC_REAL_WRITE) {
		if (*nrels_mag >= 1) {
			down(&data->update_lock);
			ctl4 = lm93_read_byte( client,
				LM93_REG_PWM_CTL(nr,LM93_PWM_CTL4));
			ctl4 = (ctl4 & 0xf8) | LM93_PWM_FREQ_TO_REG(results[0]);
			data->block9[nr][LM93_PWM_CTL4] = ctl4;

			/* ctl4 == 0 -> 22.5KHz -> disable smart tach */
			if (!ctl4)
				lm93_disable_fan_smart_tach(client, data, nr);

			lm93_write_byte(client,
				LM93_REG_PWM_CTL(nr,LM93_PWM_CTL4), ctl4);
			up(&data->update_lock);
		}
	}
}

void lm93_alarms(struct i2c_client *client, int operation, int ctl_name,
	int *nrels_mag, long *results)
{
	struct lm93_data *data = client->data;
	if (operation == SENSORS_PROC_REAL_INFO)
		*nrels_mag = 0;
	else if (operation == SENSORS_PROC_REAL_READ) {
		lm93_update_client(client);
		results[0] = LM93_ALARMS_FROM_REG(data->block1);
		*nrels_mag = 1;
	}
}

static int __init lm93_init(void)
{
	printk(KERN_INFO "lm93.o version %s (%s)\n", LM_VERSION, LM_DATE);
	return i2c_add_driver(&lm93_driver);
}

static void __exit lm93_exit(void)
{
	i2c_del_driver(&lm93_driver);
}

MODULE_AUTHOR("Mark M. Hoffman <mhoffman@lightlink.com>");
MODULE_DESCRIPTION("LM93 driver");
MODULE_LICENSE("GPL");

module_init(lm93_init);
module_exit(lm93_exit);

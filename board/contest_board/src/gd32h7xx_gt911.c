/****************************************************************************
 * boards/arm/gd32h7xx/gd32h759imt6/src/gd32h7xx_gt911.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * GD32H759IMT6 GT911 capacitive touch controller (bit-bang I2C)
 *
 * Hardware (from vendor example "实验23 触摸屏实验（RGB屏）"):
 *   SCL = PF6, SDA = PF7  (bit-bang I2C)
 *   INT = PG3             (touch interrupt input)
 *   RST = PF8             (reset, active low)
 *   I2C addr: 0x28 (write) / 0x29 (read)   <- 7-bit addr 0x14
 *
 * Registers:
 *   GT9XXX_CTRL_REG   0x8040
 *   GT9XXX_CFGS_REG   0x8047
 *   GT9XXX_PID_REG    0x8140
 *   GT9XXX_GSTID_REG  0x814E
 *   GT9XXX_TP1_REG    0x8150  (each TP record is 8 bytes)
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>

#include "gd32h7xx_gpio.h"
#include "gd32h759imt6.h"

#ifdef CONFIG_GD32H759IMT6_GT911

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define GT911_I2C_ADDR_WR      0x28u
#define GT911_I2C_ADDR_RD      0x29u

#define GT9XXX_CTRL_REG        0x8040u
#define GT9XXX_PID_REG         0x8140u
#define GT9XXX_GSTID_REG       0x814eu
#define GT9XXX_TP1_REG         0x8150u

#define GT911_TOUCH_MAX        5

#define GT911_XRES             800
#define GT911_YRES             480

/* Pixel clock helper for the parallel RGB frame buffer in SDRAM */

#define GT911_FB_BASE          0xc0000000u
#define GT911_FB_STRIDE        (GT911_XRES * 2)  /* RGB565 */

/* GPIO configuration sets (SCL/SDA pulled up, matching the vendor example) */

#define GT911_SCL_CFG   (GPIO_CFG_PORT_F | GPIO_CFG_PIN_6  | GPIO_CFG_MODE_OUTPUT | \
                         GPIO_CFG_PP | GPIO_CFG_PUPD_PULLUP | GPIO_CFG_SPEED_60MHZ)
#define GT911_SDA_CFG   (GPIO_CFG_PORT_F | GPIO_CFG_PIN_7  | GPIO_CFG_MODE_OUTPUT | \
                         GPIO_CFG_OD | GPIO_CFG_PUPD_PULLUP | GPIO_CFG_SPEED_60MHZ)
#define GT911_SDA_INCFG (GPIO_CFG_PORT_F | GPIO_CFG_PIN_7  | GPIO_CFG_MODE_INPUT | \
                         GPIO_CFG_PUPD_PULLUP)
#define GT911_RST_CFG   (GPIO_CFG_PORT_F | GPIO_CFG_PIN_8  | GPIO_CFG_MODE_OUTPUT | \
                         GPIO_CFG_PP | GPIO_CFG_SPEED_60MHZ)
#define GT911_INT_CFG   (GPIO_CFG_PORT_G | GPIO_CFG_PIN_3  | GPIO_CFG_MODE_INPUT | \
                         GPIO_CFG_PUPD_PULLUP)
#define GT911_INT_NOCFG (GPIO_CFG_PORT_G | GPIO_CFG_PIN_3  | GPIO_CFG_MODE_INPUT | \
                         GPIO_CFG_PUPD_NONE)

#define GT911_SCL_LOW()   gd32_gpio_write(GT911_SCL_CFG, false)
#define GT911_SCL_HIGH()  gd32_gpio_write(GT911_SCL_CFG, true)
#define GT911_SDA_LOW()   gd32_gpio_write(GT911_SDA_CFG, false)
#define GT911_SDA_HIGH()  gd32_gpio_write(GT911_SDA_CFG, true)
#define GT911_RST_LOW()   gd32_gpio_write(GT911_RST_CFG, false)
#define GT911_RST_HIGH()  gd32_gpio_write(GT911_RST_CFG, true)

/* Bit-bang timing.  The vendor example uses a 2 us half-period delay
 * (SCL ~125 kHz, well within the GT911 fast-mode range) and is proven to
 * work on this exact panel.  Keep the same timing - a much slower clock
 * (e.g. 20 us -> ~15 kHz) puts the controller into an abnormal state and it
 * stops reporting touches.
 */

#define GT911_DELAY()     up_udelay(2)

/* Read SDA by first switching the pin to input (pull-up) mode so the input
 * buffer is definitely enabled, then back to open-drain output.  This avoids
 * relying on reading the ISTAT while the pin is in output mode.
 */

/* Read SDA by reading the ISTAT while the pin is in open-drain output mode.
 * On GD32H7 the input status register still reflects the actual pin level in
 * output mode (the vendor example relies on exactly this), so no mode switch
 * is needed - switching modes only adds transients.
 */

static bool gt911_sda_read_bit(void)
{
  return gd32_gpio_read(GT911_SDA_CFG);
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void gt911_i2c_start(void)
{
  GT911_SDA_HIGH();
  GT911_SCL_HIGH();
  GT911_DELAY();
  GT911_SDA_LOW();
  GT911_DELAY();
  GT911_SCL_LOW();
  GT911_DELAY();
}

static void gt911_i2c_stop(void)
{
  GT911_SDA_LOW();
  GT911_DELAY();
  GT911_SCL_HIGH();
  GT911_DELAY();
  GT911_SDA_HIGH();
  GT911_DELAY();
}

static void gt911_i2c_ack(void)
{
  GT911_SDA_LOW();
  GT911_DELAY();
  GT911_SCL_HIGH();
  GT911_DELAY();
  GT911_SCL_LOW();
  GT911_DELAY();
}

static void gt911_i2c_nack(void)
{
  GT911_SDA_HIGH();
  GT911_DELAY();
  GT911_SCL_HIGH();
  GT911_DELAY();
  GT911_SCL_LOW();
  GT911_DELAY();
}

static int gt911_i2c_wait_ack(void)
{
  int timeout = 250;

  GT911_SDA_HIGH();
  GT911_DELAY();
  GT911_SCL_HIGH();
  GT911_DELAY();

  while (gt911_sda_read_bit())
    {
      if (--timeout <= 0)
        {
          gt911_i2c_stop();
          return -EIO;
        }

      GT911_DELAY();
    }

  GT911_SCL_LOW();
  GT911_DELAY();
  return OK;
}

static void gt911_i2c_send_byte(uint8_t data)
{
  int i;

  for (i = 7; i >= 0; i--)
    {
      if (data & (1u << i))
        {
          GT911_SDA_HIGH();
        }
      else
        {
          GT911_SDA_LOW();
        }

      GT911_DELAY();
      GT911_SCL_HIGH();
      GT911_DELAY();
      GT911_SCL_LOW();
      GT911_DELAY();
    }
}

static uint8_t gt911_i2c_read_byte(bool last)
{
  uint8_t value = 0;
  int i;

  /* Release SDA so the slave can drive the line (open-drain high = high-Z).
   * Without this, the last value written to SDA (e.g. pulled low by the ACK
   * phase) keeps the line held low and every bit reads as 0.
   */

  GT911_SDA_HIGH();
  GT911_DELAY();

  for (i = 7; i >= 0; i--)
    {
      GT911_SCL_HIGH();
      GT911_DELAY();
      value <<= 1;
      if (gt911_sda_read_bit())
        {
          value |= 1;
        }

      GT911_SCL_LOW();
      GT911_DELAY();
    }

  if (last)
    {
      gt911_i2c_nack();
    }
  else
    {
      gt911_i2c_ack();
    }

  return value;
}

/* Write a register on the GT911 */

static int gt911_wr_reg(uint16_t reg, FAR const uint8_t *buf, uint8_t len)
{
  uint8_t i;

  gt911_i2c_start();
  gt911_i2c_send_byte(GT911_I2C_ADDR_WR);
  if (gt911_i2c_wait_ack() != OK)
    {
      return -EIO;
    }

  gt911_i2c_send_byte(reg >> 8);
  if (gt911_i2c_wait_ack() != OK)
    {
      return -EIO;
    }

  gt911_i2c_send_byte(reg & 0xff);
  if (gt911_i2c_wait_ack() != OK)
    {
      return -EIO;
    }

  for (i = 0; i < len; i++)
    {
      gt911_i2c_send_byte(buf[i]);
      if (gt911_i2c_wait_ack() != OK)
        {
          return -EIO;
        }
    }

  gt911_i2c_stop();
  return OK;
}

/* Read a register on the GT911 */

static int gt911_rd_reg(uint16_t reg, FAR uint8_t *buf, uint8_t len)
{
  uint8_t i;

  gt911_i2c_start();
  gt911_i2c_send_byte(GT911_I2C_ADDR_WR);
  if (gt911_i2c_wait_ack() != OK)
    {
      return -EIO;
    }

  gt911_i2c_send_byte(reg >> 8);
  if (gt911_i2c_wait_ack() != OK)
    {
      return -EIO;
    }

  gt911_i2c_send_byte(reg & 0xff);
  if (gt911_i2c_wait_ack() != OK)
    {
      return -EIO;
    }

  gt911_i2c_start();
  gt911_i2c_send_byte(GT911_I2C_ADDR_RD);
  if (gt911_i2c_wait_ack() != OK)
    {
      return -EIO;
    }

  for (i = 0; i < len; i++)
    {
      buf[i] = gt911_i2c_read_byte(i == (len - 1));
    }

  gt911_i2c_stop();
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
      gt911_i2c_stop();
    }
}

/****************************************************************************
 * Name: gt911_lower_init
 *
 * Description:
 *   Configure GPIOs, reset the GT911, verify the product ID and bring the
 *   controller into normal operation.
 *
 * Returned Value:
 *   OK on success, negative errno on failure.
 *
 ****************************************************************************/

int gt911_lower_init(void)
{
  uint8_t id[4];
  uint8_t temp;
  int ret;

  /* Configure the control pins */

  gd32_gpio_config(GT911_SCL_CFG);
  gd32_gpio_config(GT911_SDA_CFG);
  gd32_gpio_config(GT911_RST_CFG);
  gd32_gpio_config(GT911_INT_CFG);

  /* Reset sequence (vendor: RST low 30 ms, high 30 ms) */

  GT911_RST_LOW();
  up_mdelay(30);
  GT911_RST_HIGH();
  up_mdelay(30);

  /* After reset the INT line becomes the touch interrupt output; release the
   * host pull-up so the controller can drive it freely (vendor does the
   * same: input, no pull).
   */

  gd32_gpio_config(GT911_INT_NOCFG);

  /* Read product ID (4 chars) after boot */

  up_mdelay(100);

  memset(id, 0, sizeof(id));
  ret = gt911_rd_reg(GT9XXX_PID_REG, id, 4);
  if (ret != OK)
    {
      ierr("GT911: failed to read PID: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "GT911: product ID = %.3s\n", id);

  /* Verify a known controller family */

  if (strncmp((FAR const char *)id, "911", 3) != 0 &&
      strncmp((FAR const char *)id, "9147", 4) != 0 &&
      strncmp((FAR const char *)id, "1158", 4) != 0 &&
      strncmp((FAR const char *)id, "9271", 4) != 0 &&
      strncmp((FAR const char *)id, "967", 3) != 0)
    {
      ierr("GT911: unknown controller ID\n");
      return -ENODEV;
    }

  /* Soft reset and enter normal mode */

  temp = 0x02;
  gt911_wr_reg(GT9XXX_CTRL_REG, &temp, 1);
  up_mdelay(10);

  temp = 0x00;
  gt911_wr_reg(GT9XXX_CTRL_REG, &temp, 1);

  syslog(LOG_INFO, "GT911: initialized OK\n");
  return OK;
}

/****************************************************************************
 * Name: gt911_lower_scan
 *
 * Description:
 *   Poll the GT911 and report the first active touch point.
 *
 * Input Parameters:
 *   x, y    - (out) touch coordinates in pixels
 *   down    - (out) 1 if a finger is touching, 0 otherwise
 *
 * Returned Value:
 *   OK always.
 *
 ****************************************************************************/

int gt911_lower_scan(FAR int *x, FAR int *y, FAR int *down)
{
  uint8_t status;
  uint8_t buf[6];

  *down = 0;

  if (gt911_rd_reg(GT9XXX_GSTID_REG, &status, 1) != OK)
    {
      return -EIO;
    }

  /* Clear the buffer-ready flag whenever it is set, so the controller
   * resumes scanning (vendor logic: if bit7 set, write 0 to GSTID).
   * Without this the flag stays stuck at 0x80 and the controller stops
   * reporting new touches.
   */

  if (status & 0x80)
    {
      uint8_t z = 0;
      gt911_wr_reg(GT9XXX_GSTID_REG, &z, 1);
    }

  if ((status & 0x0f) == 0)
    {
      return OK;  /* no finger */
    }

  /* GT1151Q (PID 1158) touch-point record, per programming guide:
   *   0x8150 = point x LOW byte, 0x8151 = x HIGH byte,
   *   0x8152 = point y LOW byte, 0x8153 = y HIGH byte,
   *   0x8154 = size W, 0x8155 = size H.
   * Coordinates are little-endian 16-bit values.
   */

  if (gt911_rd_reg(GT9XXX_TP1_REG, buf, 6) != OK)
    {
      return -EIO;
    }

  *x = (buf[1] << 8) | buf[0];
  *y = (buf[3] << 8) | buf[2];
  *down = 1;

  return OK;
}

/****************************************************************************
 * Name: gt911_lower_rawscan
 *
 * Description:
 *   Diagnostic: like gt911_lower_scan but also returns the raw TP1 record
 *   bytes so the coordinate layout can be verified against the vendor
 *   example (which reads 4 bytes and computes buf[0]*257).
 *
 ****************************************************************************/

int gt911_lower_rawscan(FAR int *x, FAR int *y, FAR int *down,
                        FAR uint8_t *raw)
{
  uint8_t status;

  *down = 0;
  memset(raw, 0, 10);

  if (gt911_rd_reg(GT9XXX_GSTID_REG, &status, 1) != OK)
    {
      return -EIO;
    }

  if (status & 0x80)
    {
      uint8_t z = 0;
      gt911_wr_reg(GT9XXX_GSTID_REG, &z, 1);
    }

  if ((status & 0x0f) == 0)
    {
      return OK;  /* no finger */
    }

  /* Read the first touch-point record (6 bytes):
   *   0x8150 = X low, 0x8151 = X high, 0x8152 = Y low,
   *   0x8153 = Y high, 0x8154 = size W, 0x8155 = size H.
   */

  if (gt911_rd_reg(GT9XXX_TP1_REG, raw, 6) != OK)
    {
      return -EIO;
    }

  *x = (raw[1] << 8) | raw[0];
  *y = (raw[3] << 8) | raw[2];
  *down = 1;

  return OK;
}/****************************************************************************
 * Name: gt911_lower_draw
 *
 * Description:
 *   Draw a white pixel on the parallel RGB frame buffer in SDRAM.  This is
 *   a quick visual feedback for the touch demo.
 *
 ****************************************************************************/

void gt911_lower_draw(int x, int y)
{
  FAR volatile uint16_t *fb;
  int dx;
  int dy;

  fb = (FAR volatile uint16_t *)GT911_FB_BASE;

  /* Draw a 3x3 block so a single touch is clearly visible */

  for (dy = -1; dy <= 1; dy++)
    {
      for (dx = -1; dx <= 1; dx++)
        {
          int px = x + dx;
          int py = y + dy;

          if (px < 0 || px >= GT911_XRES || py < 0 || py >= GT911_YRES)
            {
              continue;
            }

          fb[py * GT911_XRES + px] = 0x0000;  /* black on white */
        }
    }
}

/****************************************************************************
 * Name: gt911_lower_diag
 *
 * Description:
 *   Dump a few key GT911 registers to help diagnose why touch is not
 *   detected.
 *
 ****************************************************************************/

void gt911_lower_diag(void)
{
  uint8_t b[8];

  memset(b, 0, sizeof(b));

  if (gt911_rd_reg(0x8040u, &b[0], 1) == OK)
    {
      syslog(LOG_INFO, "GT911: CTRL=0x%02x\n", b[0]);
    }

  if (gt911_rd_reg(0x8047u, &b[1], 1) == OK)
    {
      syslog(LOG_INFO, "GT911: CFGS=0x%02x\n", b[1]);
    }

  if (gt911_rd_reg(0x8140u, b, 4) == OK)
    {
      syslog(LOG_INFO, "GT911: PID=%c%c%c%c\n",
             b[0], b[1], b[2], b[3]);
    }

  if (gt911_rd_reg(0x814eu, &b[4], 1) == OK)
    {
      syslog(LOG_INFO, "GT911: GSTID=0x%02x\n", b[4]);
    }

  if (gt911_rd_reg(0x8100u, &b[5], 1) == OK)
    {
      syslog(LOG_INFO, "GT911: R8100=0x%02x\n", b[5]);
    }

  if (gt911_rd_reg(0x8150u, b, 6) == OK)
    {
      syslog(LOG_INFO, "GT911: TP1=%02x %02x %02x %02x %02x %02x\n",
             b[0], b[1], b[2], b[3], b[4], b[5]);
    }

  if (gt911_rd_reg(0x8047u, b, 24) == OK)
    {
      syslog(LOG_INFO,
             "GT911: CFG=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n"
             "GT911: CFG2=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
             b[0], b[1], b[2], b[3], b[4], b[5],
             b[6], b[7], b[8], b[9], b[10], b[11],
             b[12], b[13], b[14], b[15], b[16], b[17],
             b[18], b[19], b[20], b[21], b[22], b[23]);
    }
}

#endif /* CONFIG_GD32H759IMT6_GT911 */

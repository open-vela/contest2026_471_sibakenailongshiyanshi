/****************************************************************************
 * boards/arm/gd32h7xx/gd32h759imt6/src/gd32h7xx_bringup.c
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
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdio.h>
#include <debug.h>
#include <errno.h>

#include <nuttx/board.h>
#include <nuttx/clock.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mtd/mtd.h>
#include <nuttx/fs/fs.h>
#include <nuttx/fs/nxffs.h>
#include <nuttx/spi/spi_transfer.h>
#include <nuttx/rc/dummy.h>

#include "gd32h7xx.h"
#include "gd32h7xx_gpio.h"
#include "gd32h7xx_gpio.h"

#ifdef CONFIG_USERLED
#  include <nuttx/leds/userled.h>
#endif

#ifdef CONFIG_INPUT_BUTTONS
#  include <nuttx/input/buttons.h>
#endif

#ifdef CONFIG_VIDEO_FB
#  include <nuttx/video/fb.h>
#endif

#ifdef CONFIG_GD32H7_TLI
#  include "gd32h7xx_tli.h"
#endif

#ifdef CONFIG_GD32H7_ROMFS
#  include "gd32h7xx_romfs.h"
#endif

#include "gd32h759imt6.h"
#include "gd32h7xx_logo.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_lcd_enable
 *
 * Description:
 *   Enable the LCD on demand (runtime, from the NSH 'lcd' command).
 *   The TLI controller / SDRAM are NOT initialised at boot; this function
 *   performs the full TLI bring-up (SDRAM, pixel clock, GPIO, timing,
 *   layer 0, panel reset) and draws a simple test pattern, so the board
 *   can boot stably without touching the display until it is needed.
 *
 ****************************************************************************/

int board_lcd_enable(void)
{
#ifdef CONFIG_GD32H7_TLI
  int ret;
  static bool g_lcd_done;

  /* Idempotent: TLI/SDRAM already brought up by a previous call. */
  if (g_lcd_done)
    {
      return OK;
    }

  syslog(LOG_INFO, "LCD: enabling TLI on demand\n");

  ret = gd32_tli_initialize();
  if (ret != OK)
    {
      syslog(LOG_ERR, "LCD: TLI init failed: %d\n", ret);
      return ret;
    }

  /* v28: the application (e.g. 'gui') draws its own screen.
   * Just make sure the TLI scan is running.
   */
  gd32_tli_enable();

  g_lcd_done = true;
  syslog(LOG_INFO, "LCD: enabled, ready\n");
  return OK;
#else
  return -ENOSYS;
#endif
}

/****************************************************************************
 * Name: gd32_bringup
 *
 * Description:
 *   Perform architecture-specific initialization
 *
 *   CONFIG_BOARD_LATE_INITIALIZE=y :
 *     Called from board_late_initialize().
 *
 *   CONFIG_BOARD_LATE_INITIALIZE=n && CONFIG_BOARDCTL=y :
 *     Called from the NSH library via boardctl()
 *
 ****************************************************************************/

int gd32_bringup(void)
{
  static bool g_bringup_done = false;

  /* Prevent duplicate initialization (may be called from both
   * CONFIG_NSH_ARCHINIT and other init paths).
   */

  if (g_bringup_done)
    {
      return OK;
    }

  g_bringup_done = true;

#ifdef CONFIG_RAMMTD
  uint8_t *ramstart;
#endif

  int ret = OK;

#ifdef CONFIG_FS_PROCFS
  /* Mount the procfs file system */

  ret = nx_mount(NULL, GD32_PROCFS_MOUNTPOINT, "procfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to mount procfs at %s: %d\n",
             GD32_PROCFS_MOUNTPOINT, ret);
    }
#endif

#ifdef CONFIG_RAMMTD
  /* Create a RAM MTD device if configured */

  ramstart = kmm_malloc(64 * 1024);
  if (ramstart == NULL)
    {
      syslog(LOG_ERR, "ERROR: Allocation for RAM MTD failed\n");
    }
  else
    {
      /* Initialized the RAM MTD */

      struct mtd_dev_s *mtd = rammtd_initialize(ramstart, 64 * 1024);
      if (mtd == NULL)
        {
          syslog(LOG_ERR, "ERROR: rammtd_initialize failed\n");
          kmm_free(ramstart);
        }
      else
        {
          /* Erase the RAM MTD */

          ret = mtd->ioctl(mtd, MTDIOC_BULKERASE, 0);
          if (ret < 0)
            {
              syslog(LOG_ERR, "ERROR: IOCTL MTDIOC_BULKERASE failed\n");
            }

#if defined(CONFIG_MTD_SMART) && defined(CONFIG_FS_SMARTFS)
          /* Initialize a SMART Flash block device and bind it to the MTD
           * device.
           */

          smart_initialize(0, mtd, NULL);

#elif defined(CONFIG_FS_SPIFFS)
          /* Register the MTD driver so that it can be accessed from the
           * VFS.
           */

          ret = register_mtddriver("/dev/rammtd", mtd, 0755, NULL);
          if (ret < 0)
            {
              syslog(LOG_ERR, "ERROR: Failed to register MTD driver: %d\n",
                     ret);
            }

          /* Mount the SPIFFS file system */

          ret = nx_mount("/dev/rammtd", "/mnt/spiffs", "spiffs", 0, NULL);
          if (ret < 0)
            {
              syslog(LOG_ERR,
                     "ERROR: Failed to mount SPIFFS at /mnt/spiffs: %d\n",
                     ret);
            }

#elif defined(CONFIG_FS_LITTLEFS)
          /* Register the MTD driver so that it can be accessed from the
           * VFS.
           */

          ret = register_mtddriver("/dev/rammtd", mtd, 0755, NULL);
          if (ret < 0)
            {
              syslog(LOG_ERR, "ERROR: Failed to register MTD driver: %d\n",
                     ret);
            }

          /* Mount the LittleFS file system */

          ret = nx_mount("/dev/rammtd", "/mnt/lfs", "littlefs", 0,
                         "forceformat");
          if (ret < 0)
            {
              syslog(LOG_ERR,
                     "ERROR: Failed to mount LittleFS at /mnt/lfs: %d\n",
                     ret);
            }

#elif defined(CONFIG_FS_NXFFS)
          /* Initialize to provide NXFFS on the MTD interface */

          ret = nxffs_initialize(mtd);
          if (ret < 0)
            {
              syslog(LOG_ERR, "ERROR: NXFFS initialization failed: %d\n",
                     ret);
            }
#endif
        }
    }
#endif


#ifndef CONFIG_DISABLE_MOUNTPOINT

#  ifdef HAVE_GD25

      ret = gd32_gd25_automount(SPI_FLASH_CSNUM);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: Failed to mount the NXFFS \
                 volume on spi flash: %d\n", ret);
        }

#  endif

#  ifdef CONFIG_GD32H7_I2C
      gd32_i2c_initialize();
#  endif

#  ifdef HAVE_AT24
      ret = gd32_at24_wr_test(AT24_MINOR);
      if (ret < 0)
        {
          syslog(LOG_ERR,
                 "ERROR: I2C EEPROM wr test fail: %d\n",
                 ret);
        }

#    ifdef CONFIG_GD32H7_I2C_DMA
      ret = gd32_at24_multimsg_dma_test(AT24_MINOR);
      if (ret < 0)
        {
          syslog(LOG_ERR,
                 "ERROR: I2C DMA test fail: %d\n", ret);
        }
#    endif

#  endif

#endif /* CONFIG_FS_NXFFS */

#ifdef CONFIG_DEV_GPIO
      /* Register the GPIO driver */

      ret = gd32_gpio_initialize();
      if (ret < 0)
        {
          syslog(LOG_ERR, "Failed to initialize GPIO Driver: %d\n", ret);
          return ret;
        }
#endif

#ifdef CONFIG_INPUT_BUTTONS
#ifdef CONFIG_INPUT_BUTTONS_LOWER
  /* Register the BUTTON driver */

      ret = btn_lower_initialize("/dev/buttons");
      if (ret != OK && ret != -EEXIST)
        {
          syslog(LOG_ERR, "ERROR: btn_lower_initialize() failed: %d\n", ret);
          return ret;
        }
#else
  /* Enable BUTTON support for some other purpose */

  board_button_initialize();
#endif /* CONFIG_INPUT_BUTTONS_LOWER */
#endif /* CONFIG_INPUT_BUTTONS */

#if !defined(CONFIG_ARCH_LEDS) && defined(CONFIG_USERLED_LOWER)
      /* Register the LED driver */

      ret = userled_lower_initialize(LED_DRIVER_PATH);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: userled_lower_initialize() failed: %d\n",
                 ret);
        }
#endif

#ifdef CONFIG_VIDEO_FB
  /* Register the framebuffer device (/dev/fb0) */

  ret = fb_register(0, 0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: fb_register() failed: %d\n", ret);
    }
  else
    {
      /* Display bring-up verification: solid blue screen. */
      gd32_tli_clear(0x001f);
    }
#endif

  return ret;
}

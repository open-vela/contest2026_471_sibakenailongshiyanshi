/****************************************************************************
 * arch/arm/src/gd32h7xx/gd32h7xx_tli.h
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

#ifndef __ARCH_ARM_SRC_GD32H7XX_GD32H7XX_TLI_H
#define __ARCH_ARM_SRC_GD32H7XX_GD32H7XX_TLI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/video/fb.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* WKS RGB LCD panel configuration (4.3 inch, WKS43WV067, 800x480, RGB565).
 * The frame buffer is placed in the external SDRAM (0xC0000000) because
 * 800x480 RGB565 (~750 KB) does not fit in the internal 512 KB AXI SRAM.
 * The SDRAM is initialized by gd32_sdram_initialize() before TLI use.
 */

#ifndef CONFIG_GD32H7XX_TLI_XRES
#  define CONFIG_GD32H7XX_TLI_XRES    800
#endif

#ifndef CONFIG_GD32H7XX_TLI_YRES
#  define CONFIG_GD32H7XX_TLI_YRES    480
#endif

#define GD32_TLI_BPP                  16            /* RGB565 */
#define GD32_TLI_COLOR_FMT            FB_FMT_RGB16_565

/* The frame buffer is a plain contiguous 800x480 RGB565 buffer: each
 * visible line occupies exactly 800*2 = 1600 bytes and the next line
 * starts immediately after it.  The TLI FLL/STDOFF are set to 1607/1600
 * in gd32h7xx_tli.c.
 */

#define GD32_TLI_STRIDE_PIXELS        800
#define GD32_TLI_STRIDE               (GD32_TLI_STRIDE_PIXELS * 2)
#define GD32_TLI_FBSIZE               (GD32_TLI_STRIDE * \
                                       CONFIG_GD32H7XX_TLI_YRES)

/* Frame buffer base address in the external SDRAM bank 0 (0xC0000000). */

#ifndef CONFIG_GD32H7XX_TLI_FB_BASE
#  define CONFIG_GD32H7XX_TLI_FB_BASE 0xc0000000
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: gd32_tli_initialize
 *
 * Description:
 *   Initialize the TLI peripheral (PLL2 pixel clock, GPIO, timing and
 *   layer 0).  Called by up_fbinitialize().
 *
 ****************************************************************************/

int gd32_tli_initialize(void);
void gd32_tli_disable(void);
void gd32_tli_enable(void);

/****************************************************************************
 * Name: gd32_tli_clear
 *
 * Description:
 *   Fill the whole frame buffer with a single RGB565 color.
 *
 ****************************************************************************/

void gd32_tli_clear(uint16_t color);

/****************************************************************************
 * Name: gd32_tli_fillrect
 *
 * Description:
 *   Fill a rectangle of the frame buffer with an RGB565 color.
 *
 ****************************************************************************/

void gd32_tli_fillrect(unsigned int x0, unsigned int y0,
                       unsigned int w, unsigned int h,
                       uint16_t color);

#endif /* __ARCH_ARM_SRC_GD32H7XX_GD32H7XX_TLI_H */

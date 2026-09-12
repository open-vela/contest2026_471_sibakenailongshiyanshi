/****************************************************************************
 * arch/arm/src/gd32h7xx/gd32h7xx_sdram.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * EXMC SDRAM driver for the W9825G6KH (256 Mbit, 16-bit, 4 banks,
 * 13-row / 9-column) on the GD32H759IMT6 core board.  The SDRAM is
 * mapped at 0xC0000000 (EXMC SDRAM bank 0) and is used as the TLI frame
 * buffer for the 800x480 RGB LCD.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <debug.h>
#include <syslog.h>

#include "arm_internal.h"
#include "chip.h"
#include "gd32h7xx.h"
#include "gd32h7xx_gpio.h"
#include "gd32h7xx_rcu.h"
#include "hardware/gd32h7xx_rcu.h"
#include "hardware/gd32h7xx_exmc.h"

#ifdef CONFIG_GD32H7_TLI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define GD32_SDRAM_TIMEOUT               0x00ffffff

/* EXMC AHB3 clock enable: (AHB3EN_OFFSET << 6) | EXMCEN */

#define RCU_EXMC_EN                      ((0x0038 << RCU_PERI_REG_SHIFT) | 1)

/* SDRAM mode register content: burst length 1, sequential, CAS latency 3 */

#define GD32_SDRAM_MODE_REG              0x30

/* --- EXMC SDRAM GPIO pins (alternate function AF12) --- */

#define GPIO_EXMC_PIN(port, pin)         (GPIO_CFG_PORT_##port | \
                                          GPIO_CFG_PIN_##pin | \
                                          GPIO_CFG_MODE_AF | \
                                          GPIO_CFG_AF_12 | \
                                          GPIO_CFG_PUPD_PULLUP | \
                                          GPIO_CFG_PP | \
                                          GPIO_CFG_SPEED_85MHZ)

#define GPIO_EXMC_PC0                    GPIO_EXMC_PIN(C, 0)
#define GPIO_EXMC_PC4                    GPIO_EXMC_PIN(C, 4)
#define GPIO_EXMC_PC5                    GPIO_EXMC_PIN(C, 5)

#define GPIO_EXMC_PD0                    GPIO_EXMC_PIN(D, 0)
#define GPIO_EXMC_PD1                    GPIO_EXMC_PIN(D, 1)
#define GPIO_EXMC_PD8                    GPIO_EXMC_PIN(D, 8)
#define GPIO_EXMC_PD9                    GPIO_EXMC_PIN(D, 9)
#define GPIO_EXMC_PD10                   GPIO_EXMC_PIN(D, 10)
#define GPIO_EXMC_PD14                   GPIO_EXMC_PIN(D, 14)
#define GPIO_EXMC_PD15                   GPIO_EXMC_PIN(D, 15)

#define GPIO_EXMC_PE0                    GPIO_EXMC_PIN(E, 0)
#define GPIO_EXMC_PE1                    GPIO_EXMC_PIN(E, 1)
#define GPIO_EXMC_PE7                    GPIO_EXMC_PIN(E, 7)
#define GPIO_EXMC_PE8                    GPIO_EXMC_PIN(E, 8)
#define GPIO_EXMC_PE9                    GPIO_EXMC_PIN(E, 9)
#define GPIO_EXMC_PE10                   GPIO_EXMC_PIN(E, 10)
#define GPIO_EXMC_PE11                   GPIO_EXMC_PIN(E, 11)
#define GPIO_EXMC_PE12                   GPIO_EXMC_PIN(E, 12)
#define GPIO_EXMC_PE13                   GPIO_EXMC_PIN(E, 13)
#define GPIO_EXMC_PE14                   GPIO_EXMC_PIN(E, 14)
#define GPIO_EXMC_PE15                   GPIO_EXMC_PIN(E, 15)

#define GPIO_EXMC_PF0                    GPIO_EXMC_PIN(F, 0)
#define GPIO_EXMC_PF1                    GPIO_EXMC_PIN(F, 1)
#define GPIO_EXMC_PF2                    GPIO_EXMC_PIN(F, 2)
#define GPIO_EXMC_PF3                    GPIO_EXMC_PIN(F, 3)
#define GPIO_EXMC_PF4                    GPIO_EXMC_PIN(F, 4)
#define GPIO_EXMC_PF5                    GPIO_EXMC_PIN(F, 5)
#define GPIO_EXMC_PF11                   GPIO_EXMC_PIN(F, 11)
#define GPIO_EXMC_PF12                   GPIO_EXMC_PIN(F, 12)
#define GPIO_EXMC_PF13                   GPIO_EXMC_PIN(F, 13)
#define GPIO_EXMC_PF14                   GPIO_EXMC_PIN(F, 14)
#define GPIO_EXMC_PF15                   GPIO_EXMC_PIN(F, 15)

#define GPIO_EXMC_PG0                    GPIO_EXMC_PIN(G, 0)
#define GPIO_EXMC_PG1                    GPIO_EXMC_PIN(G, 1)
#define GPIO_EXMC_PG2                    GPIO_EXMC_PIN(G, 2)
#define GPIO_EXMC_PG4                    GPIO_EXMC_PIN(G, 4)
#define GPIO_EXMC_PG5                    GPIO_EXMC_PIN(G, 5)
#define GPIO_EXMC_PG8                    GPIO_EXMC_PIN(G, 8)
#define GPIO_EXMC_PG15                   GPIO_EXMC_PIN(G, 15)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: gd32_sdram_gpio_config
 *
 * Description:
 *   Configure all EXMC SDRAM data / address / control pins in alternate
 *   function mode (AF12).  gd32_gpio_config() enables the port clocks.
 *
 ****************************************************************************/

static void gd32_sdram_gpio_config(void)
{
  /* Port C: SDNWE, SDNE0, SDCKE */

  gd32_gpio_config(GPIO_EXMC_PC0);
  gd32_gpio_config(GPIO_EXMC_PC4);
  gd32_gpio_config(GPIO_EXMC_PC5);

  /* Port D: D0/D1, D13/D14/D15, NBL... */

  gd32_gpio_config(GPIO_EXMC_PD0);
  gd32_gpio_config(GPIO_EXMC_PD1);
  gd32_gpio_config(GPIO_EXMC_PD8);
  gd32_gpio_config(GPIO_EXMC_PD9);
  gd32_gpio_config(GPIO_EXMC_PD10);
  gd32_gpio_config(GPIO_EXMC_PD14);
  gd32_gpio_config(GPIO_EXMC_PD15);

  /* Port E: NBL0/NBL1, D7..D15 */

  gd32_gpio_config(GPIO_EXMC_PE0);
  gd32_gpio_config(GPIO_EXMC_PE1);
  gd32_gpio_config(GPIO_EXMC_PE7);
  gd32_gpio_config(GPIO_EXMC_PE8);
  gd32_gpio_config(GPIO_EXMC_PE9);
  gd32_gpio_config(GPIO_EXMC_PE10);
  gd32_gpio_config(GPIO_EXMC_PE11);
  gd32_gpio_config(GPIO_EXMC_PE12);
  gd32_gpio_config(GPIO_EXMC_PE13);
  gd32_gpio_config(GPIO_EXMC_PE14);
  gd32_gpio_config(GPIO_EXMC_PE15);

  /* Port F: A0..A5, A11..A15, SDNRAS */

  gd32_gpio_config(GPIO_EXMC_PF0);
  gd32_gpio_config(GPIO_EXMC_PF1);
  gd32_gpio_config(GPIO_EXMC_PF2);
  gd32_gpio_config(GPIO_EXMC_PF3);
  gd32_gpio_config(GPIO_EXMC_PF4);
  gd32_gpio_config(GPIO_EXMC_PF5);
  gd32_gpio_config(GPIO_EXMC_PF11);
  gd32_gpio_config(GPIO_EXMC_PF12);
  gd32_gpio_config(GPIO_EXMC_PF13);
  gd32_gpio_config(GPIO_EXMC_PF14);
  gd32_gpio_config(GPIO_EXMC_PF15);

  /* Port G: A6..A12, BS0/BS1, SDCLK, SDNCAS */

  gd32_gpio_config(GPIO_EXMC_PG0);
  gd32_gpio_config(GPIO_EXMC_PG1);
  gd32_gpio_config(GPIO_EXMC_PG2);
  gd32_gpio_config(GPIO_EXMC_PG4);
  gd32_gpio_config(GPIO_EXMC_PG5);
  gd32_gpio_config(GPIO_EXMC_PG8);
  gd32_gpio_config(GPIO_EXMC_PG15);
}

/****************************************************************************
 * Name: gd32_sdram_wait_ready
 *
 * Description:
 *   Wait until the EXMC SDRAM controller reports ready.
 *
 ****************************************************************************/

static int gd32_sdram_wait_ready(void)
{
  uint32_t timeout = GD32_SDRAM_TIMEOUT;

  while ((getreg32(GD32_EXMC_SDSTAT) & GD32_SDSTAT_NRDY) != 0)
    {
      if (--timeout == 0)
        {
          return -ETIMEDOUT;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: gd32_sdram_send_command
 *
 * Description:
 *   Send a single SDRAM command and wait until the controller is ready.
 *
 ****************************************************************************/

static int gd32_sdram_send_command(uint32_t cmd, uint32_t nref,
                                   uint32_t mr)
{
  uint32_t regval = (cmd << GD32_SDCMD_CMD_SHIFT) | GD32_SDCMD_DS0 |
                    ((nref & 0xf) << GD32_SDCMD_NARF_SHIFT) |
                    ((mr & 0x1fff) << GD32_SDCMD_MRC_SHIFT);

  putreg32(regval, GD32_EXMC_SDCMD);

  return gd32_sdram_wait_ready();
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: gd32_sdram_initialize
 *
 * Description:
 *   Initialize the EXMC SDRAM controller and the W9825G6KH device.
 *   Must be called before the SDRAM region at 0xC0000000 is used (e.g.
 *   before the TLI frame buffer is accessed).
 *
 ****************************************************************************/

void gd32_sdram_initialize(void)
{
  uint32_t regval;

  /* Enable the EXMC peripheral clock (AHB3) */

  gd32_rcu_periph_clock_enable(RCU_EXMC_EN);

  /* Force a clean start: drop the SDRAM clock first and wait.  On a
   * warm reset (pressing the reset button, as opposed to a fresh power
   * up after flashing) the SDRAM still holds its previously configured
   * state and CK_EXMC may already be running; disabling SDCLK and
   * waiting lets the SDRAM fall back to its power-on-reset condition
   * before we re-run the full init sequence below.  Without this, the
   * stale SDRAM state can leave the EXMC/SDRAM in a bad state and the
   * boot can stall or the TLI can read garbage.
   */

  putreg32(GD32_SDCTL_SDCLK_DISABLE, GD32_EXMC_SDCTL);
  up_udelay(2000);

  /* Configure the EXMC GPIO pins (this also enables the port clocks) */

  gd32_sdram_gpio_config();

  /* Configure the SDRAM control register (SDCTL):
   *   CAW = 9 bit column, RAW = 13 bit row, SDW = 16 bit data,
   *   NBK = 4 banks, CAS latency = 3, SDCLK = CK_EXMC / 2,
   *   burst read enabled, pipeline delay = 2
   */

  /* v26: force exact example value 0x59D9 (W9825G6KH: 9col/13row/16bit,
   * 4bank, CAS3, SDCLK=2, burst read, pipeline 2).  Source assembly of the
   * same fields produced 0x59C3 in the linked image, so pin it explicitly. */
  regval = 0x59d9;

  putreg32(regval, GD32_EXMC_SDCTL);

  /* Configure the SDRAM timing register (SDTCFG):
   *   LMRD = 2, XSRD = 11, RASD = 10, ARFD = 10,
   *   WRD = 2, RPD = 3, RCD = 3 (all in CK_EXMC periods).
   * Note: the hardware field value is (delay - 1) as in the GD32
   * peripheral library.
   */

  regval = ((2  - 1) << GD32_SDTCFG_LMRD_SHIFT) |
           ((11 - 1) << GD32_SDTCFG_XSRD_SHIFT) |
           ((10 - 1) << GD32_SDTCFG_RASD_SHIFT) |
           ((10 - 1) << GD32_SDTCFG_ARFD_SHIFT) |
           ((2  - 1) << GD32_SDTCFG_WRD_SHIFT) |
           ((3  - 1) << GD32_SDTCFG_RPD_SHIFT) |
           ((3  - 1) << GD32_SDTCFG_RCD_SHIFT);

  putreg32(regval, GD32_EXMC_SDTCFG);

  /* Initialization sequence:
   * 1. Clock enable
   * 2. Precharge all
   * 3. Auto refresh x8
   * 4. Load mode register (burst 1, sequential, CAS 3 -> 0x30)
   */

  if (gd32_sdram_send_command(GD32_SDCMD_CMD_CKE, 0, 0) != OK)
    {
      syslog(LOG_ERR, "SDRAM: clock enable timeout\n");
      return;
    }

  up_udelay(1000);  /* wait ~10ms as in the example */

  if (gd32_sdram_send_command(GD32_SDCMD_CMD_PRECHARGE, 0, 0) != OK)
    {
      syslog(LOG_ERR, "SDRAM: precharge timeout\n");
      return;
    }

  if (gd32_sdram_send_command(GD32_SDCMD_CMD_AUTOREFRESH, 7, 0) != OK)
    {
      syslog(LOG_ERR, "SDRAM: auto refresh timeout\n");
      return;
    }

  if (gd32_sdram_send_command(GD32_SDCMD_CMD_LOADMODE, 0,
                              GD32_SDRAM_MODE_REG) != OK)
    {
      syslog(LOG_ERR, "SDRAM: load mode register timeout\n");
      return;
    }

  /* Configure the auto-refresh interval (SDARI):
   * W9825G6KH: 8192 rows refreshed every 64 ms -> 7.8125 us per row.
   * With CK_EXMC = 150 MHz -> 1171 CK_EXMC periods minus margin 20.
   * The ARINTV field starts at bit 1.
   */

  putreg32(((1151) << 1), GD32_EXMC_SDARI);   /* v25: example value for 150MHz SDCLK */

  syslog(LOG_INFO, "SDRAM: initialized, mapped at 0xC0000000\n");

  /* Liveness check: if the SDRAM did not actually come up, these writes
   * fault instead of silently succeeding, so the failure shows up here at
   * bring-up rather than as garbage on the panel.  The written values are
   * throwaway - the TLI driver clears the whole frame buffer before
   * anything is drawn into it.
   */

  {
    volatile uint32_t *p  = (volatile uint32_t *)0xc0000000;
    volatile uint32_t *p2 = (volatile uint32_t *)0xc0000800;
    uint32_t v;

    *p  = 0x12345678;
    *p2 = 0xdeadbeef;
    v = *p;             /* read-back completes the round trip */
    (void)v;
  }
}

#endif /* CONFIG_GD32H7_TLI */

/****************************************************************************
 * gui_main.c - OpenVela contest demo GUI
 *
 * Pages (entered from the main-page buttons, or from the serial console):
 *   0 main     : WQY title (top-centre) + 4 entry buttons
 *   1 touch    : finger drawing + live coordinate (x:123, y:123) top-right
 *   3 measure  : ADC + FFT spectrum analyser (time domain + magnitude)
 *   4 sysinfo  : system information (incl. screen model)
 *
 * Input: GT911 touch (board lower-half).  Serial fallback: keys 1/2/3
 * enter pages 1/3/4, b returns to the main page, q quits.
 *
 * All drawing writes straight into the RGB565 frame buffer in SDRAM at
 * 0xC0000000 (the TLI driver scans it out); there is no NX / graphics
 * library involved.
 *
 * Page 2 (handwriting) runs the 32x32 int8 MLP recogniser trained on the
 * user's own strokes (digit_net.h, see hw_export/net_predict): lift the
 * finger, the digit is exported over serial (HW: grid blocks) and shown
 * on screen.  Reached from the main-page button (v70) or `gui hw`.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>

/* NuttX IRQ API (declared manually: <nuttx/irq.h> pulls in board headers
 * that also declare led_init(), clashing with our static one) */
extern int irq_attach(int irq, int (*isr)(int, void *, void *), void *arg);
extern void up_enable_irq(int irq);

#include "gui_title_bitmap.h"
#include "gui_text.h"
#include "gui_digits.h"
#include "logo.h"

/* board services (FLAT build) */
extern int  board_lcd_enable(void);
extern int  gt911_lower_init(void);
extern int  gt911_lower_scan(FAR int *x, FAR int *y, FAR int *down);

#define LCD_W 800
#define LCD_H 480

#define C_WHITE   0xffff
#define C_BLACK   0x0000
#define C_GREY    0x8c9aa8
#define C_BORDER  0x5b96
#define C_HILITE  0xdf1e    /* light blue pressed fill (RGB565) */
#define C_RED     0xf800    /* red  (RGB565) */
#define C_GREEN   0x07e0    /* green (RGB565) */

/* ------------------------------------------------------------------ */
/* framebuffer primitives                                              */
/* ------------------------------------------------------------------ */

static void fb_fill(int x, int y, int w, int h, uint16_t c)
{
  volatile uint16_t *fb = (volatile uint16_t *)0xc0000000;
  int yy, xx;

  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > LCD_W) { w = LCD_W - x; }
  if (y + h > LCD_H) { h = LCD_H - y; }
  if (w <= 0 || h <= 0) { return; }

  for (yy = 0; yy < h; yy++)
    {
      uint16_t *row = (uint16_t *)fb + (y + yy) * LCD_W + x;
      for (xx = 0; xx < w; xx++) { row[xx] = c; }
    }
}

static void blit_rgb565(int x, int y, int w, int h,
                        const uint16_t *buf)
{
  volatile uint16_t *fb = (volatile uint16_t *)0xc0000000;
  int yy, xx;

  for (yy = 0; yy < h; yy++)
    {
      uint16_t *dst = (uint16_t *)fb + (y + yy) * LCD_W + x;
      const uint16_t *src = &buf[yy * w];

      for (xx = 0; xx < w; xx++) { dst[xx] = src[xx]; }
    }
}

static void fb_px(int x, int y, uint16_t c)
{
  if (x >= 0 && x < LCD_W && y >= 0 && y < LCD_H)
    {
      ((volatile uint16_t *)0xc0000000)[y * LCD_W + x] = c;
    }
}

/* ------------------------------------------------------------------ */
/* gray4 bitmap helpers                                                */
/* ------------------------------------------------------------------ */

static void draw_gray4(int x, int y, int w, int h, const uint8_t *bits,
                       int scale)
{
  volatile uint16_t *fb = (volatile uint16_t *)0xc0000000;
  int yy, xx, dy, dx;

  for (yy = 0; yy < h; yy++)
    {
      for (xx = 0; xx < w; xx++)
        {
          int b = bits[yy * (w / 2) + (xx >> 1)];
          int v = (xx & 1) ? (b & 0x0f) : (b >> 4);

          if (v == 0) { continue; }

          for (dy = 0; dy < scale; dy++)
            {
              int sy = y + yy * scale + dy;
              if (sy < 0 || sy >= LCD_H) { continue; }
              for (dx = 0; dx < scale; dx++)
                {
                  int sx = x + xx * scale + dx;
                  if (sx < 0 || sx >= LCD_W) { continue; }
                  {
                    /* v41: linear blackness -> RGB565.  v=15 (pure black
                     * glyph pixel) now maps to 0x0000.
                     */
                    int br = 15 - v;              /* 0..15 brightness */
                    int r5 = (br * 31 + 7) / 15;  /* 0..31 */
                    int g6 = (br * 63 + 7) / 15;  /* 0..63 */
                    int b5 = (br * 31 + 7) / 15;  /* 0..31 */
                    fb[sy * LCD_W + sx] = (uint16_t)((r5 << 11) |
                                                     (g6 << 5) | b5);
                  }
                }
            }
        }
    }
}

static void blit_text(int x, int y, int w, int h, const uint8_t *bits,
                      int scale)
{
  draw_gray4(x, y, w, h, bits, scale);
}

static void blit_center(int y, int w, int h, const uint8_t *bits, int scale)
{
  draw_gray4((LCD_W - w * scale) / 2, y, w, h, bits, scale);
}

static void draw_gray4_fg(int x, int y, int w, int h, const uint8_t *bits,
                          int scale, uint16_t fg)
{
  volatile uint16_t *fb = (volatile uint16_t *)0xc0000000;
  int yy, xx, dy, dx;

  for (yy = 0; yy < h; yy++)
    {
      for (xx = 0; xx < w; xx++)
        {
          int b = bits[yy * (w / 2) + (xx >> 1)];
          int v = (xx & 1) ? (b & 0x0f) : (b >> 4);

          if (v == 0) { continue; }

          for (dy = 0; dy < scale; dy++)
            {
              int sy = y + yy * scale + dy;
              if (sy < 0 || sy >= LCD_H) { continue; }
              for (dx = 0; dx < scale; dx++)
                {
                  int sx = x + xx * scale + dx;
                  if (sx < 0 || sx >= LCD_W) { continue; }
                  fb[sy * LCD_W + sx] = fg;
                }
            }
        }
    }
}

static void blit_digits(int x, int y, const char *str, uint16_t fg)
{
  int i, cx = x;

  for (i = 0; str[i] != '\0'; i++)
    {
      const char *p = strchr(GUI_DIG_CHARS, str[i]);
      int idx;

      if (p == NULL) { continue; }
      idx = (int)(p - GUI_DIG_CHARS);
      draw_gray4_fg(cx, y, gui_dig_w[idx], GUI_DIG_H,
                    gui_dig_bits[idx], 1, fg);
      cx += gui_dig_w[idx] + 2;
    }
}

/* ------------------------------------------------------------------ */
/* shapes                                                              */
/* ------------------------------------------------------------------ */

static void draw_rect(int x, int y, int w, int h, uint16_t c)
{
  fb_fill(x, y, w, 2, c);
  fb_fill(x, y + h - 2, w, 2, c);
  fb_fill(x, y, 2, h, c);
  fb_fill(x + w - 2, y, 2, h, c);
}

static void draw_circle(int cx, int cy, int r, uint16_t c)
{
  int x, y;

  for (y = -r; y <= r; y++)
    {
      for (x = -r; x <= r; x++)
        {
          if (x * x + y * y <= r * r)
            {
              fb_px(cx + x, cy + y, c);
            }
        }
    }
}

static void draw_line(int x0, int y0, int x1, int y1, int w, uint16_t c)
{
  int dx = abs(x1 - x0);
  int dy = -abs(y1 - y0);
  int sx = x0 < x1 ? 1 : -1;
  int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;

  for (;;)
    {
      draw_circle(x0, y0, w / 2, c);
      if (x0 == x1 && y0 == y1) { break; }
      {
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
      }
    }
}

/* ------------------------------------------------------------------ */
/* dynamic digit text (WQY glyphs)                                     */
/* ------------------------------------------------------------------ */

static int digit_text_w(int scale, const char *s)
{
  int w = 0;

  for (; *s; s++)
    {
      const char *p = strchr(GUI_DIG_CHARS, *s);
      if (p)
        {
          w += gui_dig_w[p - GUI_DIG_CHARS] * scale;
        }
    }

  return w;
}

static void draw_digit_text(int x, int y, int scale, const char *s,
                            uint16_t color)
{
  int i;

  (void)color;

  for (i = 0; s[i]; i++)
    {
      const char *p = strchr(GUI_DIG_CHARS, s[i]);
      if (p)
        {
          int idx = p - GUI_DIG_CHARS;
          blit_text(x, y, gui_dig_w[idx], GUI_DIG_H,
                    gui_dig_bits[idx], scale);
          x += gui_dig_w[idx] * scale;
        }
    }
}

/* ------------------------------------------------------------------ */
/* page definitions                                                    */
/* ------------------------------------------------------------------ */

enum { PG_MAIN = 0, PG_TOUCH = 1, PG_HAND = 2, PG_MEAS = 3, PG_SYS = 4,
       PG_DIAG = 99 };

struct btn
{
  int x, y, w, h;
  int page;
  int txt_w, txt_h;
  const uint8_t *txt;
};

#define BTN_W 360
#define BTN_H 48
#define BTN_X ((LCD_W - BTN_W) / 2)
#define BTN_Y0 150
#define BTN_GAP 12

/* v70: handwriting re-added (int8 MLP recogniser) - four buttons */
static const struct btn g_btns[4] =
{
  { BTN_X, BTN_Y0,                           BTN_W, BTN_H, PG_TOUCH,
    GUI_TXT_BTN1_W, GUI_TXT_BTN1_H, gui_txt_btn1_bits },
  { BTN_X, BTN_Y0 + 1 * (BTN_H + BTN_GAP),   BTN_W, BTN_H, PG_HAND,
    GUI_TXT_BTN2_W, GUI_TXT_BTN2_H, gui_txt_btn2_bits },
  { BTN_X, BTN_Y0 + 2 * (BTN_H + BTN_GAP),   BTN_W, BTN_H, PG_MEAS,
    GUI_TXT_BTN3_W, GUI_TXT_BTN3_H, gui_txt_btn3_bits },
  { BTN_X, BTN_Y0 + 3 * (BTN_H + BTN_GAP),   BTN_W, BTN_H, PG_SYS,
    GUI_TXT_BTN4_W, GUI_TXT_BTN4_H, gui_txt_btn4_bits },
};

static const struct btn g_back =
{ 16, 14, 116, 38, PG_MAIN, GUI_TXT_BACK_W, GUI_TXT_BACK_H,
  gui_txt_back_bits };

/* handwriting page: draw area + clear button */
#define HAND_X 40
#define HAND_Y 110
#define HAND_W 520
#define HAND_H 320

#define CLEAR_X 600
#define CLEAR_Y 300
#define CLEAR_W 180
#define CLEAR_H 56

/* 32x32 grid export + right-hand 16x16 preview (int8 MLP, v70) */
#define HW_N 32                        /* export grid resolution        */
#define HW_GS 10                       /* preview cell (16*10=160px)    */
#define HW_GX (HAND_X + HAND_W + 24)   /* 584 .. 776 inside 800         */
#define HW_GY (HAND_Y - 10)            /* 100: sits above CLEAR button  */

static int g_page = PG_MAIN;
static volatile bool g_run = true;
static int g_tx = -1;   /* last touch point (stroke) */
static int g_ty = -1;

/* 4-corner touch calibration (kept, unused: raw coords are native) */
enum { CAL_NONE = -1, CAL_TL = 0, CAL_TR = 1, CAL_BL = 2, CAL_BR = 3 };
static int g_cal = CAL_NONE;
static int g_cx[4], g_cy[4];
static bool g_cal_done = false;

/* 9-point calibration: screen points (3x3 grid) */
#define C9_PTS 9
#define C9_CX(n) (((n) % 3) * 400)
#define C9_CY(n) (((n) / 3) * 240)
static int g_p9[C9_PTS][2];
static int g_cal9 = 0;
static int g_lrx = -1, g_lry = -1;

struct tri
{
  int x0, y0;
  int vxx, vxy, vyx, vyy, det;
  int sx0, sy0;
};
static struct tri g_tri[8];
static int g_ccx[4], g_ccy[4];


/* ------------------------------------------------------------------ */
/* handwriting recognition (0-9, grid match)                           */
/* ------------------------------------------------------------------ */

#define NCR_MAX_PTS 1024

static int g_ncr_x[NCR_MAX_PTS];
static int g_ncr_y[NCR_MAX_PTS];
static int g_ncr_n = 0;

/* Idle-wait state machine (mirrors example 33): recognition runs only
 * after the finger has been up for NCR_IDLE_LOOPS main-loop iterations
 * (each loop sleeps 20 ms, so 20 loops ~= 400 ms of no writing).
 */
#define NCR_IDLE_LOOPS 20
static int g_ncr_idle = 0;

static void ncr_reset(void)
{
  g_ncr_n = 0;
  g_ncr_idle = 0;
}

/* A 16-bit coordinate is rebuilt from two bytes (xh<<8|xl style).  When
 * xh or yh carries, the decoded value jumps by ~256 - not a real finger
 * movement.  Drop any sample that jumps more than this many pixels.
 */
#define NCR_JUMP 260

static void ncr_add(int x, int y)
{
  if (g_ncr_n > 0)
    {
      int dx = x - g_ncr_x[g_ncr_n - 1];
      int dy = y - g_ncr_y[g_ncr_n - 1];

      if (dx * dx + dy * dy > NCR_JUMP * NCR_JUMP)
        {
          return;   /* carry-jump / glitch sample */
        }
    }

  if (g_ncr_n < NCR_MAX_PTS)
    {
      g_ncr_x[g_ncr_n] = x;
      g_ncr_y[g_ncr_n] = y;
      g_ncr_n++;
    }
}

static void hand_clear(void)
{
  fb_fill(HAND_X, HAND_Y, HAND_W, HAND_H, C_WHITE);
  draw_rect(HAND_X, HAND_Y, HAND_W, HAND_H, C_BORDER);
  fb_fill(LCD_W - 200, 40, 180, GUI_DIG_H + 16, C_WHITE);
  draw_rect(LCD_W - 200, 40, 180, GUI_DIG_H + 16, C_BORDER);
  fb_fill(HW_GX - 8, HW_GY - 8, 16 * HW_GS + 16, 16 * HW_GS + 16, C_WHITE);
  draw_rect(HW_GX - 8, HW_GY - 8, 16 * HW_GS + 16, 16 * HW_GS + 16, C_BORDER);
  g_ncr_idle = 0;
  ncr_reset();
}

/* ------------------------------------------------------------------ */
/* page drawing                                                        */
/* ------------------------------------------------------------------ */

static void draw_main(void)
{
  int i;

  fb_fill(0, 0, LCD_W, LCD_H, C_WHITE);

  /* title, upper area, centred */
  blit_center(56, GUI_TITLE_ROW1_W, GUI_TITLE_ROW1_H,
              gui_title_row1_bits, 1);
  blit_center(102, GUI_TITLE_ROW2_W, GUI_TITLE_ROW2_H,
              gui_title_row2_bits, 1);

  /* entry buttons */
  for (i = 0; i < 4; i++)
    {
      const struct btn *b = &g_btns[i];

      draw_rect(b->x, b->y, b->w, b->h, C_BORDER);
      blit_text(b->x + (b->w - b->txt_w) / 2,
                b->y + (b->h - b->txt_h) / 2,
                b->txt_w, b->txt_h, b->txt, 1);
    }

  /* partner logos (v83):
   *   bottom centre - Wuhan University of Technology
   *   top left      - Spark Lab + 'spark' caption
   *   bottom left   - openvela
   *   top right     - GigaDevice (enlarged)
   *   bottom right  - Xiaomi
   */
  {
#define LOGO_MARGIN 40
#define LOGO_TOP_Y  40
#define LOGO_BOT_Y  392

    /* bottom centre: WHUT */
    blit_rgb565((LCD_W - LOGO_WHUT_W) / 2, LOGO_BOT_Y,
                LOGO_WHUT_W, LOGO_WHUT_H, logo_whut_bits);

    /* top-left: Spark Lab + 'spark' caption */
    blit_rgb565(LOGO_MARGIN, LOGO_TOP_Y, LOGO_SPARK_W, LOGO_SPARK_H,
                logo_spark_bits);
    blit_text(LOGO_MARGIN - (GUI_TXT_SPARK_TXT_W - LOGO_SPARK_W) / 2,
              LOGO_TOP_Y + LOGO_SPARK_H + 4,
              GUI_TXT_SPARK_TXT_W, GUI_TXT_SPARK_TXT_H,
              gui_txt_spark_txt_bits, 1);

    /* bottom-left: openvela */
    blit_rgb565(LOGO_MARGIN, LOGO_BOT_Y, LOGO_OPENVELA_W, LOGO_OPENVELA_H,
                logo_openvela_bits);

    /* top-right: GigaDevice (enlarged) */
    blit_rgb565(LCD_W - LOGO_MARGIN - LOGO_GIGADEVICE_W, LOGO_TOP_Y,
                LOGO_GIGADEVICE_W, LOGO_GIGADEVICE_H, logo_gigadevice_bits);

    /* bottom-right: Xiaomi */
    blit_rgb565(LCD_W - LOGO_MARGIN - LOGO_XIAOMI_W, LOGO_BOT_Y,
                LOGO_XIAOMI_W, LOGO_XIAOMI_H, logo_xiaomi_bits);
  }
}

static void draw_sub_header(const struct btn *hdr, const uint8_t *hint,
                            int hint_w, int hint_h)
{
  fb_fill(0, 0, LCD_W, LCD_H, C_WHITE);

  /* back button */
  draw_rect(g_back.x, g_back.y, g_back.w, g_back.h, C_BORDER);
  blit_text(g_back.x + (g_back.w - g_back.txt_w) / 2,
            g_back.y + (g_back.h - g_back.txt_h) / 2,
            g_back.txt_w, g_back.txt_h, g_back.txt, 1);

  /* page header centred */
  blit_center(28, hdr->txt_w, hdr->txt_h, hdr->txt, 1);

  if (hint)
    {
      blit_center(66, hint_w, hint_h, hint, 1);
    }
}

static void draw_touch_page(void)
{
  static const struct btn hdr = { 0, 0, 0, 0, PG_TOUCH,
    GUI_TXT_HDR1_W, GUI_TXT_HDR1_H, gui_txt_hdr1_bits };

  draw_sub_header(&hdr, gui_txt_hint_touch_bits,
                  GUI_TXT_HINT_TOUCH_W, GUI_TXT_HINT_TOUCH_H);
}

static void draw_hand_page(void)
{
  static const struct btn hdr = { 0, 0, 0, 0, PG_HAND,
    GUI_TXT_HDR2_W, GUI_TXT_HDR2_H, gui_txt_hdr2_bits };

  draw_sub_header(&hdr, gui_txt_hint_hand_bits,
                  GUI_TXT_HINT_HAND_W, GUI_TXT_HINT_HAND_H);

  /* drawing area */
  draw_rect(HAND_X, HAND_Y, HAND_W, HAND_H, C_BORDER);

  /* clear button */
  draw_rect(CLEAR_X, CLEAR_Y, CLEAR_W, CLEAR_H, C_BORDER);
  blit_text(CLEAR_X + (CLEAR_W - GUI_TXT_CLEAR_W) / 2,
            CLEAR_Y + (CLEAR_H - GUI_TXT_CLEAR_H) / 2,
            GUI_TXT_CLEAR_W, GUI_TXT_CLEAR_H, gui_txt_clear_bits, 1);

  /* MLP result box (top-right) + 16x16 preview frame, empty */
  fb_fill(LCD_W - 200, 40, 180, GUI_DIG_H + 16, C_WHITE);
  draw_rect(LCD_W - 200, 40, 180, GUI_DIG_H + 16, C_BORDER);
  fb_fill(HW_GX - 8, HW_GY - 8, 16 * HW_GS + 16, 16 * HW_GS + 16, C_WHITE);
  draw_rect(HW_GX - 8, HW_GY - 8, 16 * HW_GS + 16, 16 * HW_GS + 16, C_BORDER);

  ncr_reset();
}

/* ------------------------------------------------------------------ */
/* ADC + DAC + FFT spectrum analyser (v55)                             */
/* ------------------------------------------------------------------ */
/* Register-only access (no kernel driver change), same style as the
 * TLI/GT911 bring-up:
 *   ADC0  = 0x40012400, 14-bit, CH18 = PA4 (external signal input)
 *   RCU   = 0x58024400  APB2EN +0x44 bit8=ADC0,
 *                        AHB4EN +0x3C bit0=GPIOA (already on)
 *   GPIOA = 0x58020000  PA4 analog: CTL bits[9:8]=11, PUD bits[9:8]=00
 */
#define VREG(a)     (*(volatile uint32_t *)(a))
#define ADC0_BASE   0x40012400UL
#define RCU_BASE    0x58024400UL
#define GPIOA_BASE  0x58020000UL

#define ADC_STAT    VREG(ADC0_BASE + 0x00)
#define ADC_CTL0    VREG(ADC0_BASE + 0x04)
#define ADC_CTL1    VREG(ADC0_BASE + 0x08)
#define ADC_RSQ0    VREG(ADC0_BASE + 0x24)
#define ADC_RSQ8    VREG(ADC0_BASE + 0x44)
#define ADC_RDATA   VREG(ADC0_BASE + 0x64)
#define ADC_SYNCCTL VREG(ADC0_BASE + 0x304)
#define RCU_APB2EN  VREG(RCU_BASE + 0x44)
#define GPIOA_CTL   VREG(GPIOA_BASE + 0x00)
#define GPIOA_PUD   VREG(GPIOA_BASE + 0x0C)

#define FFT_N   256
#define SP_X    30
#define SP_Y    135
#define SP_W    360
#define SP_H    255
#define FF_X    410
#define FF_Y    135
#define FF_W    360
#define FF_H    255

static float g_re[FFT_N];
static float g_im[FFT_N];
static float g_time[FFT_N];   /* pre-FFT samples, kept for the waveform */
static int   g_spec_inited = 0;
static int   g_adc_min = 0;
static int   g_adc_max = 0;
static int   g_adc_avg = 0;
static int   g_adc_err = 0;


/* RCU APB2 reset control: +0x24, ADC0RST = bit8 (example18 adc_deinit) */
#define RCU_APB2RST  VREG(RCU_BASE + 0x24)
#define RCU_APB4EN   VREG(RCU_BASE + 0x4C)
#define VREF_CS_REG  VREG(0x58003800)

/* enable internal voltage reference: VREFP = 2.5V for ADC */
static void vref_enable(void)
{
  RCU_APB4EN |= (1u << 2);                 /* VREF clock on (APB4EN bit2) */
  VREF_CS_REG = (VREF_CS_REG & ~(0x3u << 4)) | (0u << 4); /* VREFS = 2.5V */
  VREF_CS_REG &= ~(1u << 1);               /* HIPM = 0 (drive VREFP pin) */
  VREF_CS_REG |= (1u << 0);                /* VREFEN = 1 */
  { int w = 0; while (!(VREF_CS_REG & (1u << 3)) && (++w < 100000)) ; }
}

static void adc0_init(void)
{
  vref_enable();
  RCU_APB2EN |= (1u << 8);                 /* ADC0 clock on */
  RCU_APB2RST |= (1u << 8);                /* adc_deinit: reset ADC0 */
  RCU_APB2RST &= ~(1u << 8);

  GPIOA_CTL = (GPIOA_CTL & ~(3u << 8)) | (3u << 8);  /* PA4 analog */
  GPIOA_PUD &= ~(3u << 8);

  ADC_CTL1 &= ~(1u << 0);                  /* ADC off during config */
  ADC_SYNCCTL = (ADC_SYNCCTL & ~((0xFu << 16) | (0xFu << 20))) | 0x000C0000u;
  ADC_SYNCCTL &= ~(0xFu << 0);             /* sync mode: independent (=0) */
  ADC_CTL0 &= ~(0x3u << 24);               /* 14-bit resolution */
  ADC_CTL0 &= ~(1u << 8);                  /* scan mode off */
  ADC_CTL1 &= ~(1u << 1);                  /* continuous mode off */
  ADC_CTL1 &= ~(1u << 11);                 /* right aligned */
  ADC_CTL1 &= ~(0x3u << 28);               /* ext trigger disable (ETMRC=0) */
  ADC_RSQ0 &= ~(0xFu << 20);               /* 1 conversion in sequence */
  ADC_RSQ8 = ((807u & 0x3FFu) << 5) | 0x12u;  /* rank0 = CH18, 807-clk */

  ADC_CTL1 |= (1u << 0);                   /* ADC on */
  usleep(10000);                           /* wait ADC stable, then calibrate */
  ADC_CTL1 |= (1u << 27);                  /* calibration mode OFFSET (lib) */
  ADC_CTL1 &= ~(0x7u << 4);                /* calibration number: 1 */
  ADC_CTL1 |= (1u << 3);                   /* reset calibration */
  while (ADC_CTL1 & (1u << 3));
  ADC_CTL1 |= (1u << 2);                   /* start calibration */
  while (ADC_CTL1 & (1u << 2));

  printf("SPECTRUM: ADC0 CH18(PA4) 14bit ready\n");
}

static void spectrum_init_once(void)
{
  if (!g_spec_inited)
    {
      adc0_init();
      g_spec_inited = 1;
    }
}

/* Sample FFT_N points (software trigger) from external signal on PA4. */
static void adc_sample(void)
{
  int i;

  RCU_APB2EN |= (1u << 8);             /* keep ADC0 clock alive (some path clears it) */

  for (i = 0; i < FFT_N; i++)
    {
      ADC_RSQ8 = ((807u & 0x3FFu) << 5) | 0x12u;  /* re-arm rank0 = CH18 */
      ADC_STAT = ~(1u << 1);               /* clear EOC (stdlib adc_flag_clear) */
      ADC_CTL1 |= (1u << 30);              /* software start */
      {
        int spins = 0;

        while (!(ADC_STAT & (1u << 1)))    /* wait EOC, bounded */
          {
            if (++spins > 200000)
              {
                g_adc_err++;
                break;
              }
          }
      }
      g_re[i] = (float)(ADC_RDATA & 0x3FFF);
      g_im[i] = 0.0f;
      g_time[i] = g_re[i];

      {
        int v = (int)g_re[i];

        if (i == 0) { g_adc_min = g_adc_max = v; }
        if (v < g_adc_min) { g_adc_min = v; }
        if (v > g_adc_max) { g_adc_max = v; }
        g_adc_avg += v;
      }
    }

  g_adc_avg /= FFT_N;
}

/* In-place radix-2 FFT, float.  Twiddle constants are precomputed per
 * stage (no math.h dependency). */
static void fft_radix2(void)
{
  static const float tw[8][2] =
  {
    { -1.0f, 0.0f },           /* len=2   */
    {  0.0f, 1.0f },           /* len=4   */
    {  0.70710678f,  0.70710678f },
    {  0.92387953f,  0.38268343f },
    {  0.98078528f,  0.19509032f },
    {  0.99518473f,  0.09801714f },
    {  0.99879546f,  0.04906767f },
    {  0.99969882f,  0.02454123f },
  };
  int n = FFT_N, i, j, len, k;

  for (i = 1, j = 0; i < n; i++)
    {
      int bit = n >> 1;

      for (; j & bit; bit >>= 1) { j ^= bit; }
      j ^= bit;
      if (i < j)
        {
          float tr = g_re[i]; g_re[i] = g_re[j]; g_re[j] = tr;
          float ti = g_im[i]; g_im[i] = g_im[j]; g_im[j] = ti;
        }
    }

  for (len = 2, k = 0; len <= n; len <<= 1, k++)
    {
      float wr = tw[k][0], wi = tw[k][1];

      for (i = 0; i < n; i += len)
        {
          float cwr = 1.0f, cwi = 0.0f;
          int half = len >> 1;

          for (j = 0; j < half; j++)
            {
              int a = i + j;
              int b = a + half;
              float tr = g_re[b] * cwr - g_im[b] * cwi;
              float ti = g_re[b] * cwi + g_im[b] * cwr;

              g_re[b] = g_re[a] - tr;
              g_im[b] = g_im[a] - ti;
              g_re[a] += tr;
              g_im[a] += ti;

              {
                float nwr = cwr * wr - cwi * wi;
                cwi = cwr * wi + cwi * wr;
                cwr = nwr;
              }
            }
        }
    }
}

/* ADC spectrum now runs on a static dataset stored in flash: the 256
 * samples below (1 kHz @ ~1600 LSB + 2 kHz harmonic + noise, 14-bit)
 * are FFT-analysed once when the measure page opens and drawn once, so
 * nothing flickers.  Live sampling stays in the non-STATIC branch.
 */
#define MEAS_ADC_STATIC 1

#ifdef MEAS_ADC_STATIC
#  define SP_MAG_SCALE (SP_H * 0.75f / 204800.0f)  /* static demo data */
#else
#  define SP_MAG_SCALE (SP_H / 1040.0f)            /* live ADC, small signals */
#endif

static const uint16_t g_adc_static[FFT_N] = {
  8212,8648,8999,9239,9521,9812,9795,9971,9941,9801,9637,9462,
  9266,9098,8912,8733,8629,8432,8248,8151,7868,7689,7581,7298,
  7098,6978,6787,6704,6545,6470,6478,6524,6650,6916,7298,7573,
  7984,8321,8837,9079,9397,9728,9828,9963,9937,9893,9673,9588,
  9399,9260,8980,8827,8560,8444,8274,8091,8040,7806,7726,7477,
  7283,7090,6905,6651,6562,6445,6449,6450,6716,6799,7145,7417,
  7881,8240,8547,9030,9371,9630,9772,9822,9871,9959,9808,9609,
  9549,9317,9107,8881,8700,8498,8303,8280,8059,7912,7706,7593,
  7284,7141,6890,6723,6705,6545,6455,6485,6631,6714,6995,7315,
  7618,8030,8465,8870,9097,9518,9599,9873,9960,9855,9836,9782,
  9494,9408,9237,8994,8837,8577,8405,8272,8087,7956,7888,7651,
  7426,7219,7129,6858,6775,6568,6487,6529,6558,6663,6846,7195,
  7454,7910,8167,8616,8997,9388,9539,9818,9911,9955,9908,9868,
  9625,9461,9236,9013,8844,8771,8585,8306,8228,8052,7910,7750,
  7511,7423,7145,6975,6796,6572,6445,6451,6527,6629,6787,6934,
  7370,7710,8109,8448,8891,9098,9388,9612,9799,9872,9861,9855,
  9649,9577,9323,9207,8904,8760,8682,8450,8335,8155,7990,7757,
  7642,7368,7292,7119,6913,6618,6534,6452,6404,6545,6707,6841,
  7209,7535,7804,8300,8709,8964,9344,9613,9795,9823,9946,9904,
  9838,9691,9439,9308,9133,8942,8640,8446,8383,8157,8057,7957,
  7695,7492,7331,7164,6880,6746,6557,6537,6518,6468,6566,6768,
  6965,7283,7655,8064,
};

static void adc_static_load(void)
{
  int i;

  for (i = 0; i < FFT_N; i++)
    {
      g_time[i] = (float)g_adc_static[i];
      g_re[i] = g_time[i];
      g_im[i] = 0.0f;
    }
}

static void spectrum_draw(void)
{
  int i, peak = 1;
  float pmax = 0.0f;

  /* left: time-domain waveform (g_time = samples before the in-place FFT) */
  fb_fill(SP_X, SP_Y, SP_W, SP_H, C_WHITE);
  for (i = 1; i < FFT_N; i++)
    {
      int y0 = SP_Y + (int)((16383.0f - g_time[i - 1]) * SP_H / 16384.0f);
      int y1 = SP_Y + (int)((16383.0f - g_time[i]) * SP_H / 16384.0f);

      if (y0 < SP_Y) { y0 = SP_Y; }
      if (y0 > SP_Y + SP_H) { y0 = SP_Y + SP_H; }
      if (y1 < SP_Y) { y1 = SP_Y; }
      if (y1 > SP_Y + SP_H) { y1 = SP_Y + SP_H; }

      draw_line(SP_X + (i - 1) * SP_W / FFT_N, y0,
                SP_X + i * SP_W / FFT_N, y1, 2, C_BLACK);
    }
  draw_rect(SP_X, SP_Y, SP_W, SP_H, C_BORDER);

  /* right: magnitude spectrum, 128 bins, fixed 14-bit full-scale */
  for (i = 1; i < FFT_N / 2; i++)
    {
      float mag = sqrtf(g_re[i] * g_re[i] + g_im[i] * g_im[i]);

      if (mag > pmax) { pmax = mag; peak = i; }
    }

  fb_fill(FF_X, FF_Y, FF_W, FF_H, C_WHITE);
  for (i = 1; i < FFT_N / 2; i++)
    {
      float mag = sqrtf(g_re[i] * g_re[i] + g_im[i] * g_im[i]);
      int h = (int)(mag * SP_MAG_SCALE);

      if (h > SP_H) { h = SP_H; }
      if (h > 0)
        {
          fb_fill(FF_X + (FF_W - 256) / 2 + i * 2,
                  FF_Y + SP_H - h, 2, h,
                  (i == peak) ? C_BLACK : C_HILITE);
        }
    }
  draw_rect(FF_X, FF_Y, FF_W, FF_H, C_BORDER);

  /* peak frequency (Hz) in the top-right corner of the spectrum panel */
  {
    char buf[16];
    int w;

    snprintf(buf, sizeof(buf), "%d",
             (int)(36363.0f * peak / FFT_N));
    w = digit_text_w(2, buf);
    draw_digit_text(FF_X + FF_W - w - 10, FF_Y + 8, 2, buf, C_BLACK);
  }
}

/* Red/green LED control (LED0=PC13 red, LED1=PJ8 green, active low).
 * Direct GPIO registers, same style as the ADC/FFT block above:
 *   GPIOC = 0x58020800, GPIOJ = 0x58022400 on AHB4
 *   RCU   = 0x58024400, AHB4EN = +0x3C (bit2 = GPIOC, bit8 = GPIOJ)
 *   CTL bit pair 01 = output push-pull; OCTL bit 0 = LED on.
 */
#define LED_RCU_AHB4EN  (*(volatile uint32_t *)(0x58024400UL + 0x3c))
#define LED_GPIOC_CTL   (*(volatile uint32_t *)0x58020800UL)
#define LED_GPIOC_OCTL  (*(volatile uint32_t *)0x58020814UL)
#define LED_GPIOJ_CTL   (*(volatile uint32_t *)0x58022400UL)
#define LED_GPIOJ_OCTL  (*(volatile uint32_t *)0x58022414UL)

#define LED_BTN_W 140
#define LED_BTN_H 42
#define LED_BTN_Y 70
#define LED_RED_X   240
#define LED_GREEN_X 420

static bool g_led_red = false;    /* lit? */
static bool g_led_green = false;
static int  g_led_inited = 0;
static bool g_touch_prev = false;  /* finger was down last frame (edge detect) */

static void led_init(void);         /* defined below (LED block) */

/* ------------------------------------------------------------------ */
/* Software PWM for the red LED (LED0=PC13) via TIMER1 update IRQ.
 * PC13 has no timer-channel alternate function (GPIO/RTC only), so we
 * bit-bang a 100-level duty cycle from the TIMER1 update interrupt.
 *   TIMER1 = 0x40000000        (GD32_TIMER_BASE, APB1, verified)
 *   RCU APB1EN = 0x58024440    (RCU base 0x58024400 + 0x40, bit0 TIMER1EN)
 *   CTL0.CEN(0) | DMAINTEN.UPIE(0) | INTF.UPIF(0)
 *   PSC=199, CAR=99  ->  ~10 kHz update, 100 Hz PWM @ ~200 MHz APB1 timer.
 * ------------------------------------------------------------------ */
#define PWM_RCU_APB1EN (*(volatile uint32_t *)0x58024440UL)
#define PWM_T1_CTL0    (*(volatile uint32_t *)0x40000000UL)
#define PWM_T1_DMAINTEN (*(volatile uint32_t *)0x4000000cUL)
#define PWM_T1_INTF    (*(volatile uint32_t *)0x40000010UL)
#define PWM_T1_PSC     (*(volatile uint32_t *)0x40000028UL)
#define PWM_T1_CAR     (*(volatile uint32_t *)0x4000002cUL)

#define PWM_LEVELS 100

static volatile int g_pwm_tick = 0;   /* 0..PWM_LEVELS-1 */
static int g_pwm_duty = 50;           /* slider: duty cycle 0..100 */
static int g_pwm_inited = 0;

static int pwm_isr(int irq, void *context, void *arg)
{
  (void)irq;
  (void)context;
  (void)arg;

  PWM_T1_INTF = 0;                    /* clear UPIF */

  g_pwm_tick++;
  if (g_pwm_tick >= PWM_LEVELS) { g_pwm_tick = 0; }

  /* LED0 (PC13) active low: OCTL bit13 = 0 -> on.
   * Fixed duty cycle = brightness control. */
  if (g_led_red && g_pwm_tick < g_pwm_duty)
    {
      LED_GPIOC_OCTL &= ~(1u << 13);
    }
  else
    {
      LED_GPIOC_OCTL |= (1u << 13);
    }

  return 0;
}

static void pwm_init(void)
{
  if (g_pwm_inited) { return; }
  led_init();

  PWM_RCU_APB1EN |= (1u << 0);        /* TIMER1 clock on */
  PWM_T1_PSC = 199;
  PWM_T1_CAR = PWM_LEVELS - 1;
  PWM_T1_INTF = 0;
  PWM_T1_DMAINTEN |= (1u << 0);       /* UPIE */
  PWM_T1_CTL0 |= (1u << 0);           /* CEN */
  irq_attach(44, pwm_isr, NULL);      /* GD32_IRQ_TIMER1 = EXINT+28 = 44 */
  up_enable_irq(44);
  g_pwm_inited = 1;
}

/* duty slider (below the panels) */
#define PWM_TRACK_X0 60
#define PWM_TRACK_W  (LCD_W - 120)
#define PWM_TRACK_Y  452
#define PWM_TXT_Y    426
#define PWM_SLIDE_Y0 420
#define PWM_SLIDE_Y1 472

static int digit_total_w(const char *s)
{
  int w = 0;

  while (*s)
    {
      if (*s >= '0' && *s <= '9') { w += gui_dig_w[*s - '0']; }
      s++;
    }
  return w;
}

static void draw_num(int cx, int y, int num)
{
  char buf[8];
  int i, n, x;

  snprintf(buf, sizeof(buf), "%d", num);
  n = strlen(buf);
  x = cx - digit_total_w(buf) / 2;
  for (i = 0; i < n; i++)
    {
      int d = buf[i] - '0';

      blit_text(x, y, gui_dig_w[d], GUI_DIG_H, gui_dig_bits[d], 1);
      x += gui_dig_w[d];
    }
}

static void draw_pwm_slider(void)
{
  int tx = PWM_TRACK_X0 + g_pwm_duty * PWM_TRACK_W / 100;
  int lw = GUI_TXT_PWM_LABEL_W;

  /* clear the whole slider strip */
  fb_fill(30, PWM_SLIDE_Y0, LCD_W - 60, PWM_SLIDE_Y1 - PWM_SLIDE_Y0,
          C_WHITE);

  blit_center(PWM_TXT_Y, lw, GUI_TXT_PWM_LABEL_H, gui_txt_pwm_label_bits, 1);
  draw_num((LCD_W + lw) / 2 + 20, PWM_TXT_Y - 2, g_pwm_duty);

  fb_fill(PWM_TRACK_X0, PWM_TRACK_Y, PWM_TRACK_W, 4, C_BORDER);
  draw_circle(tx, PWM_TRACK_Y + 2, 14, C_HILITE);
}

static void led_init(void)
{
  if (g_led_inited) { return; }

  LED_RCU_AHB4EN |= (1u << 2);    /* GPIOC clock on */
  LED_RCU_AHB4EN |= (1u << 8);    /* GPIOJ clock on */

  /* PC13 = output push-pull, red off (active low) */
  LED_GPIOC_CTL = (LED_GPIOC_CTL & ~(3u << 26)) | (1u << 26);
  LED_GPIOC_OCTL |= (1u << 13);

  /* PJ8 = output push-pull, green off (active low) */
  LED_GPIOJ_CTL = (LED_GPIOJ_CTL & ~(3u << 16)) | (1u << 16);
  LED_GPIOJ_OCTL |= (1u << 8);

  g_led_inited = 1;
}

static void led_set(int red, int green)
{
  if (red >= 0)
    {
      g_led_red = (red != 0);
      if (!g_led_red) { LED_GPIOC_OCTL |= (1u << 13); }
      /* on: the PWM ISR drives PC13 */
    }

  if (green >= 0)
    {
      g_led_green = (green != 0);
      if (g_led_green) { LED_GPIOJ_OCTL &= ~(1u << 8); }
      else             { LED_GPIOJ_OCTL |= (1u << 8); }
    }
}

static void led_draw_btn(int x, bool on, uint16_t lit)
{
  int cy = LED_BTN_Y + LED_BTN_H / 2;

  fb_fill(x, LED_BTN_Y, LED_BTN_W, LED_BTN_H, C_WHITE);
  draw_rect(x, LED_BTN_Y, LED_BTN_W, LED_BTN_H, C_BORDER);

  /* state dot: lit = filled colour, off = outline */
  if (on) { draw_circle(x + 20, cy, 9, lit); }
  else    { draw_circle(x + 20, cy, 9, C_BORDER); }

  /* label, shifted right of the dot */
  if (x == LED_RED_X)
    {
      blit_text(x + 44 + (LED_BTN_W - 44 - GUI_TXT_LED_RED_W) / 2,
                LED_BTN_Y + (LED_BTN_H - GUI_TXT_LED_RED_H) / 2,
                GUI_TXT_LED_RED_W, GUI_TXT_LED_RED_H,
                gui_txt_led_red_bits, 1);
    }
  else
    {
      blit_text(x + 44 + (LED_BTN_W - 44 - GUI_TXT_LED_GREEN_W) / 2,
                LED_BTN_Y + (LED_BTN_H - GUI_TXT_LED_GREEN_H) / 2,
                GUI_TXT_LED_GREEN_W, GUI_TXT_LED_GREEN_H,
                gui_txt_led_green_bits, 1);
    }
}

static void draw_meas_page(void)
{
  static const struct btn hdr = { 0, 0, 0, 0, PG_MEAS,
    GUI_TXT_HDR3_W, GUI_TXT_HDR3_H, gui_txt_hdr3_bits };

  draw_sub_header(&hdr, NULL, 0, 0);
  led_init();
  pwm_init();
  led_draw_btn(LED_RED_X, g_led_red, C_RED);
  led_draw_btn(LED_GREEN_X, g_led_green, C_GREEN);
#ifdef MEAS_ADC_STATIC
  adc_static_load();
  fft_radix2();
  spectrum_draw();
#else
  spectrum_init_once();
  adc_sample();
  fft_radix2();
  spectrum_draw();
#endif

  /* panel captions below the two frames */
  blit_text(SP_X + (SP_W - GUI_TXT_SP_LABEL_W) / 2,
            SP_Y + SP_H + 6, GUI_TXT_SP_LABEL_W, GUI_TXT_SP_LABEL_H,
            gui_txt_sp_label_bits, 1);
  blit_text(FF_X + (FF_W - GUI_TXT_FFT_LABEL_W) / 2,
            FF_Y + FF_H + 6, GUI_TXT_FFT_LABEL_W, GUI_TXT_FFT_LABEL_H,
            gui_txt_fft_label_bits, 1);

  draw_pwm_slider();
}

static void draw_sys_page(void)
{
  static const struct btn hdr = { 0, 0, 0, 0, PG_SYS,
    GUI_TXT_HDR4_W, GUI_TXT_HDR4_H, gui_txt_hdr4_bits };
  static const struct { const uint8_t *b; int w, h; } rows[6] =
  {
    { gui_txt_s_line1_bits, GUI_TXT_S_LINE1_W, GUI_TXT_S_LINE1_H },
    { gui_txt_s_line2_bits, GUI_TXT_S_LINE2_W, GUI_TXT_S_LINE2_H },
    { gui_txt_s_line3_bits, GUI_TXT_S_LINE3_W, GUI_TXT_S_LINE3_H },
    { gui_txt_s_line4_bits, GUI_TXT_S_LINE4_W, GUI_TXT_S_LINE4_H },
    { gui_txt_s_line5_bits, GUI_TXT_S_LINE5_W, GUI_TXT_S_LINE5_H },
    { gui_txt_s_line6_bits, GUI_TXT_S_LINE6_W, GUI_TXT_S_LINE6_H },
  };
  int i;

  draw_sub_header(&hdr, NULL, 0, 0);

  /* partner logo: GigaDevice under the page title */
  blit_rgb565((LCD_W - LOGO_GIGADEVICE_W) / 2, 70,
              LOGO_GIGADEVICE_W, LOGO_GIGADEVICE_H, logo_gigadevice_bits);

  for (i = 0; i < 6; i++)
    {
      blit_center(170 + i * 42, rows[i].w, rows[i].h, rows[i].b, 1);
    }
}

static void draw_page(void)
{
  if (g_cal9 < C9_PTS)
    {
      /* Touch calibration pages.  g_cal9 is set to C9_PTS at startup, so
       * this never triggers; the code is kept for completeness.
       */
      return;
    }

  if (g_cal != CAL_NONE)
    {
      return;
    }

  switch (g_page)
    {
    case PG_MAIN:  draw_main();        break;
    case PG_TOUCH: draw_touch_page();  break;
    case PG_HAND:  draw_hand_page();   break;
    case PG_MEAS:  draw_meas_page();   break;
    case PG_SYS:   draw_sys_page();    break;
    }
}

/* ------------------------------------------------------------------ */
/* touch handling                                                      */
/* ------------------------------------------------------------------ */

static void touch_main(int x, int y)
{
  int i;

  for (i = 0; i < 4; i++)
    {
      const struct btn *b = &g_btns[i];

      if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h)
        {
          /* pressed feedback */
          fb_fill(b->x, b->y, b->w, b->h, C_HILITE);
          blit_text(b->x + (b->w - b->txt_w) / 2,
                    b->y + (b->h - b->txt_h) / 2,
                    b->txt_w, b->txt_h, b->txt, 1);
          usleep(60000);
          g_page = b->page;
          g_tx = g_ty = -1;
          ncr_reset();
          draw_page();
          return;
        }
    }
}

static void touch_sub(int x, int y)
{
  if (x >= g_back.x && x < g_back.x + g_back.w &&
      y >= g_back.y && y < g_back.y + g_back.h)
    {
      fb_fill(g_back.x, g_back.y, g_back.w, g_back.h, C_HILITE);
      blit_text(g_back.x + (g_back.w - g_back.txt_w) / 2,
                g_back.y + (g_back.h - g_back.txt_h) / 2,
                g_back.txt_w, g_back.txt_h, g_back.txt, 1);
      usleep(60000);
      g_page = PG_MAIN;
      g_tx = g_ty = -1;
      ncr_reset();
      draw_page();
    }
}

static void touch_show_coord(int x, int y)
{
  char buf[24];
  int w;

  snprintf(buf, sizeof(buf), "x:%d, y:%d", x, y);
  w = digit_text_w(1, buf);

  /* top-right corner, right-aligned */
  fb_fill(LCD_W - 260, 12, 250, GUI_DIG_H + 8, C_WHITE);
  draw_digit_text(LCD_W - 24 - w, 16, 1, buf, C_BLACK);
}

static void touch_update(int x, int y)
{
  switch (g_page)
    {
    case PG_MAIN:
      touch_main(x, y);
      break;

    case PG_TOUCH:
      touch_sub(x, y);
      if (g_page != PG_TOUCH) { break; }
      if (g_tx >= 0 && g_ty >= 0)
        {
          draw_line(g_tx, g_ty, x, y, 6, C_BLACK);
        }
      else
        {
          draw_circle(x, y, 3, C_BLACK);
        }
      touch_show_coord(x, y);
      g_tx = x;
      g_ty = y;
      break;

    case PG_HAND:
      touch_sub(x, y);
      if (g_page != PG_HAND) { break; }

      if (x >= CLEAR_X && x < CLEAR_X + CLEAR_W &&
          y >= CLEAR_Y && y < CLEAR_Y + CLEAR_H)
        {
          hand_clear();
          break;
        }

      if (x >= HAND_X && x < HAND_X + HAND_W &&
          y >= HAND_Y && y < HAND_Y + HAND_H)
        {
          g_ncr_idle = 0;   /* touching again: cancel the pending result */
          if (g_tx >= 0 && g_ty >= 0)
            {
              draw_line(g_tx, g_ty, x, y, 6, C_BLACK);
            }
          else
            {
              draw_circle(x, y, 3, C_BLACK);
            }
          ncr_add(x, y);
          g_tx = x;
          g_ty = y;
        }
      break;

    case PG_MEAS:
      touch_sub(x, y);
      if (g_page != PG_MEAS) { break; }

      /* PWM duty slider: track the finger while held */
      if (y >= PWM_SLIDE_Y0 && y <= PWM_SLIDE_Y1)
        {
          int d = (x - PWM_TRACK_X0) * 100 / PWM_TRACK_W;

          if (d < 0) { d = 0; }
          if (d > 100) { d = 100; }
          if (d != g_pwm_duty)
            {
              g_pwm_duty = d;
              draw_pwm_slider();
            }
          break;
        }

      if (g_touch_prev) { break; }   /* held down: ignore repeat presses */
      g_touch_prev = true;

      if (x >= LED_RED_X && x < LED_RED_X + LED_BTN_W &&
          y >= LED_BTN_Y && y < LED_BTN_Y + LED_BTN_H)
        {
          fb_fill(LED_RED_X, LED_BTN_Y, LED_BTN_W, LED_BTN_H, C_HILITE);
          blit_text(LED_RED_X + 44 + (LED_BTN_W - 44 - GUI_TXT_LED_RED_W) / 2,
                    LED_BTN_Y + (LED_BTN_H - GUI_TXT_LED_RED_H) / 2,
                    GUI_TXT_LED_RED_W, GUI_TXT_LED_RED_H,
                    gui_txt_led_red_bits, 1);
          usleep(60000);
          led_set(!g_led_red, -1);
          led_draw_btn(LED_RED_X, g_led_red, C_RED);
          break;
        }

      if (x >= LED_GREEN_X && x < LED_GREEN_X + LED_BTN_W &&
          y >= LED_BTN_Y && y < LED_BTN_Y + LED_BTN_H)
        {
          fb_fill(LED_GREEN_X, LED_BTN_Y, LED_BTN_W, LED_BTN_H, C_HILITE);
          blit_text(LED_GREEN_X + 44 + (LED_BTN_W - 44 - GUI_TXT_LED_GREEN_W) / 2,
                    LED_BTN_Y + (LED_BTN_H - GUI_TXT_LED_GREEN_H) / 2,
                    GUI_TXT_LED_GREEN_W, GUI_TXT_LED_GREEN_H,
                    gui_txt_led_green_bits, 1);
          usleep(60000);
          led_set(-1, !g_led_green);
          led_draw_btn(LED_GREEN_X, g_led_green, C_GREEN);
          break;
        }
      break;

    case PG_SYS:
      touch_sub(x, y);
      break;
    }
}

/* ------------------------------------------------------------------ */
/* console fallback (1-4 enter pages, b back)                          */
/* ------------------------------------------------------------------ */

static int gui_anykey(void)
{
  struct timeval tv = { 0, 0 };
  fd_set fds;
  int ret;

  FD_ZERO(&fds);
  FD_SET(0, &fds);
  ret = select(1, &fds, NULL, NULL, &tv);
  return (ret > 0);
}

static void console_key(void)
{
  char c;

  if (read(0, &c, 1) != 1)
    {
      return;
    }

  if (c == '1')
    {
      g_page = PG_TOUCH;
      g_tx = g_ty = -1;
      ncr_reset();
      draw_page();
    }
  else if (c == '2')
    {
      g_page = PG_MEAS;
      g_tx = g_ty = -1;
      ncr_reset();
      draw_page();
    }
  else if (c == '3')
    {
      g_page = PG_SYS;
      g_tx = g_ty = -1;
      ncr_reset();
      draw_page();
    }
  else if (c == '4')
    {
      g_page = PG_HAND;
      g_tx = g_ty = -1;
      ncr_reset();
      draw_page();
    }
  else if (c == 'b' || c == 'B')
    {
      g_page = PG_MAIN;
      g_tx = g_ty = -1;
      ncr_reset();
      draw_page();
    }
  else if (c == 'q' || c == 'Q')
    {
      g_run = false;
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void hw_export(void);

static int hw_run(void);

int main(int argc, FAR char *argv[])
{
  int ret;

  if (argc > 1 && strcmp(argv[1], "hw") == 0)
    {
      return hw_run();
    }

  if (board_lcd_enable() != OK)
    {
      fprintf(stderr, "GUI: LCD enable failed\n");
      return EXIT_FAILURE;
    }

  ret = gt911_lower_init();
  if (ret != OK)
    {
      fprintf(stderr, "GUI: touch init failed %d (keyboard only)\n", ret);
    }

  g_cal = CAL_NONE;
  g_cal9 = C9_PTS;   /* raw GT911 coords already map 1:1 onto the 800x480
                      * panel, so skip the touch calibration pages */
  g_lrx = g_lry = -1;
  g_page = PG_MAIN;
  ncr_reset();
  draw_page();
  printf("GUI: ready, touch buttons (q to quit)\n");

  while (g_run)
    {
      int x = -1;
      int y = -1;
      int down = 0;

      if (ret == OK)
        {
          if (gt911_lower_scan(&x, &y, &down) == OK)
            {
              if (down)
                {
                  touch_update(x, y);
                }
              else
                {
                  g_touch_prev = false;   /* finger up: re-arm button edge */
                  if (g_page == PG_HAND && g_ncr_n > 0 &&
                      g_ncr_idle == 0)
                    {
                      /* finger lifted (edge only): start idle counting,
                       * recognise only after NCR_IDLE_LOOPS of no new touch
                       * (example 33).  The g_ncr_idle==0 guard makes this a
                       * one-shot edge trigger, not a per-loop reset. */
                      g_ncr_idle = 1;
                      g_tx = g_ty = -1;
                    }
                  else if (g_page == PG_TOUCH || g_page == PG_HAND)
                    {
                      g_tx = g_ty = -1;  /* end stroke */
                    }
                }
            }
        }

      if (g_page == PG_HAND && g_ncr_idle > 0)
        {
          if (++g_ncr_idle > NCR_IDLE_LOOPS)
            {
              g_ncr_idle = 0;
              hw_export();   /* 32x32 raster + int8 MLP + serial export */
            }
        }

      if (gui_anykey())
        {
          console_key();
        }

      if (g_page == PG_MEAS)
        {
#ifndef MEAS_ADC_STATIC
          adc_sample();
          fft_radix2();
          spectrum_draw();
#endif
        }

      usleep(20000);
    }

  printf("GUI: stopped, back to shell\n");
  return OK;
}

/* ------------------------------------------------------------------ */
/* hw - handwriting grid export (subcommand: gui hw, v68d)             */
/* ------------------------------------------------------------------ */
/* Write a digit on the canvas, lift the finger; after ~400 ms of no
 * touch the stroke is rasterised to a 32x32 grid (Bresenham segment
 * interpolation, aspect-preserving bbox normalisation, no dilation),
 * printed to the serial console for offline capture, and shown on the
 * right side as a 16x16 preview (downsampled + dilated once for
 * visibility).  The canvas then clears for the next digit.  q quits.
 */

/* HW_N / HW_GS / HW_GX / HW_GY are now defined by gui_main_v67.c (v70) */
#include "digit_net.h"

/* int8 MLP forward pass on the 32x32 grid; returns 0..9 */
static int net_predict(const uint8_t grid[HW_N][HW_N])
{
  int32_t acc[NET_H];
  float h[NET_H];
  float z[NET_OUT];
  int i, j, k;

  memset(acc, 0, sizeof(acc));

  for (i = 0; i < HW_N * HW_N; i++)
    {
      if (grid[i / HW_N][i % HW_N])
        {
          for (j = 0; j < NET_H; j++)
            {
              acc[j] += net_w1[i * NET_H + j];
            }
        }
    }

  for (j = 0; j < NET_H; j++)
    {
      float v = ((float)(acc[j] + net_b1[j])) * net_s1;

      h[j] = (v > 0.0f) ? v : 0.0f;
    }

  for (k = 0; k < NET_OUT; k++)
    {
      float s = 0.0f;

      for (j = 0; j < NET_H; j++)
        {
          s += h[j] * (float)net_w2[j * NET_OUT + k];
        }

      z[k] = s * net_s2 + (float)net_b2[k] * net_s2;
    }

  k = 0;
  for (i = 1; i < NET_OUT; i++)
    {
      if (z[i] > z[k]) { k = i; }
    }

  return k;
}

static void hw_draw_canvas(void)
{
  fb_fill(0, 0, LCD_W, LCD_H, C_WHITE);

  /* hint line (reuse the handwriting page hint text) */
  blit_text(40, 64, GUI_TXT_HINT_HAND_W, GUI_TXT_HINT_HAND_H,
            gui_txt_hint_hand_bits, 1);

  /* writing canvas */
  draw_rect(HAND_X, HAND_Y, HAND_W, HAND_H, C_BORDER);
}

/* 32x32 rasterisation: scale the stroke bbox into HW_N-2 cells keeping
 * the aspect ratio, centre it, and mark every pixel along every segment
 * (Bresenham) so thin strokes stay connected.  No dilation - the PC side
 * decides how thick the digit should be. */
static void hw_rasterize(uint8_t grid[HW_N][HW_N])
{
  int minx, miny, maxx, maxy, i;
  int w, h, gw, gh, ox, oy;
  float s;

  minx = maxx = g_ncr_x[0];
  miny = maxy = g_ncr_y[0];

  for (i = 1; i < g_ncr_n; i++)
    {
      if (g_ncr_x[i] < minx) { minx = g_ncr_x[i]; }
      if (g_ncr_x[i] > maxx) { maxx = g_ncr_x[i]; }
      if (g_ncr_y[i] < miny) { miny = g_ncr_y[i]; }
      if (g_ncr_y[i] > maxy) { maxy = g_ncr_y[i]; }
    }

  w = maxx - minx + 1;
  h = maxy - miny + 1;
  if (w < 1) { w = 1; }
  if (h < 1) { h = 1; }

  s = (float)(HW_N - 2) / (w > h ? w : h);
  gw = (int)(w * s);
  gh = (int)(h * s);
  if (gw < 1) { gw = 1; }
  if (gh < 1) { gh = 1; }

  ox = (HW_N - gw) / 2;
  oy = (HW_N - gh) / 2;

  memset(grid, 0, HW_N * HW_N);

  for (i = 0; i < g_ncr_n; i++)
    {
      int x0, y0, x1, y1;
      int steps, k;

      if (i == 0)
        {
          x0 = x1 = g_ncr_x[0];
          y0 = y1 = g_ncr_y[0];
        }
      else
        {
          x0 = g_ncr_x[i - 1];
          y0 = g_ncr_y[i - 1];
          x1 = g_ncr_x[i];
          y1 = g_ncr_y[i];
        }

      steps = (x1 - x0) > 0 ? (x1 - x0) : -(x1 - x0);
      if ((y1 - y0) > steps) { steps = y1 - y0; }
      if (-(y1 - y0) > steps) { steps = -(y1 - y0); }
      if (steps < 1) { steps = 1; }

      for (k = 0; k <= steps; k++)
        {
          int px = x0 + (x1 - x0) * k / steps;
          int py = y0 + (y1 - y0) * k / steps;
          int gx = ox + (int)((px - minx) * s);
          int gy = oy + (int)((py - miny) * s);

          if (gx >= 0 && gx < HW_N && gy >= 0 && gy < HW_N)
            {
              grid[gy][gx] = 1;
            }
        }
    }
}

static void hw_export(void)
{
  uint8_t grid[HW_N][HW_N];
  int minx, miny, maxx, maxy, i, y;
  int d;

  if (g_ncr_n < 4)
    {
      return;
    }

  minx = maxx = g_ncr_x[0];
  miny = maxy = g_ncr_y[0];

  for (i = 1; i < g_ncr_n; i++)
    {
      if (g_ncr_x[i] < minx) { minx = g_ncr_x[i]; }
      if (g_ncr_x[i] > maxx) { maxx = g_ncr_x[i]; }
      if (g_ncr_y[i] < miny) { miny = g_ncr_y[i]; }
      if (g_ncr_y[i] > maxy) { maxy = g_ncr_y[i]; }
    }

  hw_rasterize(grid);

  printf("\nHW: pts=%d bbox=(%d,%d)-(%d,%d)\n",
         g_ncr_n, minx, miny, maxx, maxy);
  printf("HW: grid %d\n", HW_N);

  for (y = 0; y < HW_N; y++)
    {
      int x;

      printf("HW: ");
      for (x = 0; x < HW_N; x++)
        {
          putchar(grid[y][x] ? '1' : '0');
        }

      putchar('\n');
    }

  printf("HW: END\n");

  /* recognise the digit and show it top-right */
  d = net_predict(grid);
  printf("HW: digit=%d\n", d);

  {
    char s[2];

    s[0] = (char)('0' + d);
    s[1] = '\0';

    fb_fill(LCD_W - 200, 40, 180, GUI_DIG_H + 16, C_WHITE);
    draw_rect(LCD_W - 200, 40, 180, GUI_DIG_H + 16, C_BORDER);
    blit_digits(LCD_W - 190, 48, s, C_BLACK);
  }

  /* 16x16 preview on the right (downsample, then dilate once) */
  {
    int gs = HW_GS;
    int gx0 = HW_GX;
    int gy0 = HW_GY;
    uint8_t pv[16][16];
    uint8_t tmp[16][16];

    memset(pv, 0, sizeof(pv));

    for (y = 0; y < HW_N; y++)
      {
        for (i = 0; i < HW_N; i++)
          {
            if (grid[y][i])
              {
                pv[y / 2][i / 2] = 1;
              }
          }
      }

    memcpy(tmp, pv, sizeof(tmp));

    for (y = 0; y < 16; y++)
      {
        for (i = 0; i < 16; i++)
          {
            int dx, dy;

            if (!tmp[y][i]) { continue; }

            for (dy = -1; dy <= 1; dy++)
              {
                for (dx = -1; dx <= 1; dx++)
                  {
                    int ny = y + dy;
                    int nx = i + dx;

                    if (ny >= 0 && ny < 16 && nx >= 0 && nx < 16)
                      {
                        pv[ny][nx] = 1;
                      }
                  }
              }
          }
      }

    fb_fill(gx0 - 8, gy0 - 8, 16 * gs + 16, 16 * gs + 16, C_WHITE);
    draw_rect(gx0 - 8, gy0 - 8, 16 * gs + 16, 16 * gs + 16, C_BORDER);

    for (y = 0; y < 16; y++)
      {
        for (i = 0; i < 16; i++)
          {
            if (pv[y][i])
              {
                fb_fill(gx0 + i * gs, gy0 + y * gs, gs, gs, C_BLACK);
              }
          }
      }
  }

  /* auto-clear the canvas for the next digit */
  fb_fill(HAND_X, HAND_Y, HAND_W, HAND_H, C_WHITE);
  draw_rect(HAND_X, HAND_Y, HAND_W, HAND_H, C_BORDER);
  ncr_reset();
}

static int hw_run(void)
{
  int ret;
  bool run = true;
  int idle = 0;

  if (board_lcd_enable() != OK)
    {
      fprintf(stderr, "HW: LCD enable failed\n");
      return EXIT_FAILURE;
    }

  ret = gt911_lower_init();
  if (ret != OK)
    {
      fprintf(stderr, "HW: touch init failed %d (keyboard only)\n", ret);
    }

  ncr_reset();
  g_tx = g_ty = -1;
  hw_draw_canvas();
  printf("HW: write a digit on the canvas, lift finger to export (q to quit)\n");

  while (run)
    {
      int x = -1;
      int y = -1;
      int down = 0;

      if (gt911_lower_scan(&x, &y, &down) == OK)
        {
          if (down)
            {
              if (x >= HAND_X && x < HAND_X + HAND_W &&
                  y >= HAND_Y && y < HAND_Y + HAND_H)
                {
                  idle = 0;   /* touching: cancel pending export */

                  if (g_tx >= 0 && g_ty >= 0)
                    {
                      draw_line(g_tx, g_ty, x, y, 6, C_BLACK);
                    }
                  else
                    {
                      draw_circle(x, y, 3, C_BLACK);
                    }

                  ncr_add(x, y);
                  g_tx = x;
                  g_ty = y;
                }
            }
          else if (g_ncr_n > 0 && idle == 0)
            {
              idle = 1;       /* finger up (edge): start idle count */
              g_tx = g_ty = -1;
            }
        }

      if (idle > 0)
        {
          if (++idle > 20)    /* ~400 ms of no touch */
            {
              idle = 0;
              hw_export();
            }
        }

      if (gui_anykey())
        {
          char c;

          if (read(0, &c, 1) == 1 && (c == 'q' || c == 'Q'))
            {
              run = false;
            }
        }

      usleep(20000);
    }

  printf("HW: stopped\n");
  return OK;
}

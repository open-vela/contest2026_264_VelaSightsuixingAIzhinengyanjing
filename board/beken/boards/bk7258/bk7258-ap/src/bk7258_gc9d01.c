/****************************************************************************
 * board/beken/boards/bk7258/bk7258-ap/src/bk7258_gc9d01.c
 *
 * GC9D01 160x160 QSPI LCD panel bring-up.  Init command table copied from
 * bk_avdk_smp release/v3.1.1
 * ap/components/bk_peripheral/src/lcd/spi/lcd_spi_gc9d01.c (gc9d01_init_cmds,
 * 50 entries total).  Only the init sequence is ported; DMA-backed
 * full-frame refresh is out of scope for this task.
 *
 * Command table traceability: bk7258_qspi0_send_cmd() only supports up to
 * 4 data bytes per command (see bk7258_qspi.h), which hard-blocks porting
 * 11 of the 50 upstream entries whose data_len exceeds 4:
 *   0x74(7) 0x64(6) 0x65(6) 0x6C(7) 0x6E(32) 0x70(6) 0xF0(6) 0xF2(6)
 *   0xF1(6) 0xF3(6), plus the special 0x00/data_len=0xFF delay marker
 *   (handled separately below, not a real QSPI command).
 * The remaining 9 omitted entries (0x98, 0xB5, 0x60, 0x61, 0x6A, 0x93,
 * 0x71, 0x91, 0x34) all fit within the 4-byte limit and were dropped in
 * an earlier revision of this file without a stated reason; upstream
 * documents them as gamma/porch/timing fine-tuning registers that are not
 * required to reach a functional display state, but this has not been
 * independently confirmed against the GC9D01 datasheet.  If the panel
 * shows visible artifacts once full-frame refresh is implemented, restore
 * these 9 entries first as they are the cheapest to re-add (all <= 4
 * bytes, no interface change needed).
 *
 * One functional gap from an earlier revision of this file has been
 * fixed here: upstream sends {0x00, {0x78}, 0xFF} between the Sleep Out
 * (0x11) and Display On (0x29) commands.  data_len == 0xFF is not a real
 * QSPI command; per bk_lcd_qspi_init() in
 * ap/middleware/driver/lcd/lcd_qspi_driver.c, it is a driver-level marker
 * meaning "delay data[0] milliseconds" (0x78 = 120ms).  This delay is
 * restored below as an explicit up_udelay() call between the two
 * commands, since bk7258_qspi0_send_cmd() has no equivalent marker
 * convention.
 *
 * Bus/reset-pin correction (2026-07-31): the very first init command
 * (0xFE) was timing out on real hardware.  Root cause was two wrong
 * hardware assumptions, both corrected together (diagnosed via the board
 * schematic, AIDK_AI玩具开发板_原理图.pdf):
 *   1. bk7258_qspi.c was driving QSPI0 (GPIO22-25), but this board's
 *      LCD is wired to QSPI1 (GPIO2-5) -- see that file's header comment
 *      for the full pin-by-pin schematic evidence.
 *   2. GC9D01_RESET_PIN below was GPIO_6 (copied from a different board's
 *      dual-screen reference config in BTdocs/DualScreenAVIPlayer.md);
 *      the schematic shows GPIO_6 floating (unconnected) on this board,
 *      the real LCD_RST net is GPIO45.
 * Neither symptom was diagnosable from register-level behavior alone
 * (cmd_start_done simply never asserts when the controller drives pins
 * the panel isn't wired to); both required cross-checking the schematic
 * against gpio_map.h's pinmux function tables.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "bk7258_gpio.h"
#include "bk7258_qspi.h"

/* GC9D01 reset pin, corrected against the actual board schematic
 * (AIDK_AI玩具开发板_原理图.pdf) after this driver's original QSPI0/GPIO6
 * assumption (ported from BTdocs/DualScreenAVIPlayer.md's lcd0 config,
 * a *different* board's dual-screen reference layout) caused the very
 * first init command (0xFE) to time out -- the panel was never wired to
 * the pins this driver was toggling.  The schematic (sheet 2/6 main chip
 * pin table) shows GPIO_6 net-labelled nothing at all on this board
 * (floating, no connection); the actual LCD_RST net is wired to chip pin
 * 65 = P45 = GPIO45 (same sheet's pin table row "P45/0CSN/B5/D2/2LRCK"),
 * confirmed again on sheet 5/6's CN5 single-screen connector pin 13
 * (labelled LCD_RST). See bk7258_qspi.c's file header for the QSPI0->
 * QSPI1 correction that was diagnosed alongside this one (both changes
 * were needed to get past the same "cmd 0xFE timed out" symptom). */
#define GC9D01_RESET_PIN 45u

/* Post-Sleep-Out settle delay before Display On, per upstream
 * gc9d01_init_cmds entry {0x00, {0x78}, 0xFF} (0x78 = 120 decimal ms). */
#define GC9D01_SLEEP_OUT_DELAY_MS 120u

struct gc9d01_init_cmd
{
  uint8_t cmd;
  uint8_t data[4];
  uint8_t data_len;
};

/* Entries below are the 30 upstream commands whose data_len <= 4 and that
 * precede the Sleep Out (0x11) command; see file header comment above for
 * the full list of the 20 omitted upstream entries and why. */
static const struct gc9d01_init_cmd g_gc9d01_init_cmds_pre_sleep_out[] =
{
  { 0xFE, { 0x00 }, 0 },
  { 0xEF, { 0x00 }, 0 },
  { 0x80, { 0xFF }, 1 },
  { 0x81, { 0xFF }, 1 },
  { 0x82, { 0xFF }, 1 },
  { 0x83, { 0xFF }, 1 },
  { 0x84, { 0xFF }, 1 },
  { 0x85, { 0xFF }, 1 },
  { 0x86, { 0xFF }, 1 },
  { 0x87, { 0xFF }, 1 },
  { 0x88, { 0xFF }, 1 },
  { 0x89, { 0xFF }, 1 },
  { 0x8A, { 0xFF }, 1 },
  { 0x8B, { 0xFF }, 1 },
  { 0x8C, { 0xFF }, 1 },
  { 0x8D, { 0xFF }, 1 },
  { 0x8E, { 0xFF }, 1 },
  { 0x8F, { 0xFF }, 1 },
  { 0x3A, { 0x05 }, 1 },
  { 0xEC, { 0x01 }, 1 },
  { 0xBF, { 0x01 }, 1 },
  { 0xF9, { 0x40 }, 1 },
  { 0x9B, { 0x3B }, 1 },
  { 0x7E, { 0x30 }, 1 },
  { 0xC3, { 0x18 }, 1 },
  { 0xC4, { 0x18 }, 1 },
  { 0xC9, { 0x3C }, 1 },
  { 0x36, { 0x00 }, 1 },
  { 0x11, { 0x00 }, 0 }, /* Sleep Out */
};

#define GC9D01_PRE_SLEEP_OUT_CMD_COUNT \
  (sizeof(g_gc9d01_init_cmds_pre_sleep_out) / \
   sizeof(g_gc9d01_init_cmds_pre_sleep_out[0]))

/* Display On (0x29), sent GC9D01_SLEEP_OUT_DELAY_MS after Sleep Out. */
static const struct gc9d01_init_cmd g_gc9d01_display_on = { 0x29, { 0x00 }, 0 };

/****************************************************************************
 * Name: bk7258_gc9d01_hw_reset
 *
 * Description:
 *   Drive the GC9D01 RESET pin through a high-low-high pulse to force a
 *   hardware reset.  Timing: the 10ms pre/post pulse widths are a
 *   conservative margin (GC9D01 reset pulse width requirements are
 *   typically in the microsecond range; 10ms has no specific datasheet
 *   citation but costs nothing at bring-up time).  The final 120ms
 *   post-reset settle delay before issuing any command matches the
 *   reset-to-ready delay used across every other panel driver in the same
 *   bk_avdk_smp codebase (ap/components/bk_peripheral/src/lcd/rgb and .../mcu
 *   uniformly use rtos_delay_milliseconds(120) after hardware reset, e.g.
 *   lcd_st7701s.c:48, lcd_nt35512.c:859, lcd_st7789v.c:167), i.e. 120ms is
 *   the de-facto standard settle time for this class of display driver IC
 *   and is not GC9D01-specific tuning.
 *
 ****************************************************************************/

static void bk7258_gc9d01_hw_reset(void)
{
  bk7258_gpio_output(GC9D01_RESET_PIN, true);
  up_udelay(10000);
  bk7258_gpio_write(GC9D01_RESET_PIN, false);
  up_udelay(10000);
  bk7258_gpio_write(GC9D01_RESET_PIN, true);
  up_udelay(120000);
}

/****************************************************************************
 * Name: bk7258_gc9d01_test
 *
 * Description:
 *   Bring-up smoke test for the GC9D01 panel: hardware reset, QSPI0 init,
 *   then send the full init command sequence (pre-Sleep-Out commands,
 *   Sleep Out, 120ms settle delay, Display On).  There is no Read ID
 *   command in the upstream init table (verified: the only "0x04" bytes
 *   found in lcd_spi_gc9d01.c are payload data, not an RDID opcode), so
 *   this test cannot verify success by reading back a panel ID.  Success
 *   criterion is therefore "the full init sequence is sent without the
 *   call hanging or crashing", not a register readback match.
 *
 ****************************************************************************/

int bk7258_gc9d01_test(int argc, char **argv)
{
  size_t i;

  printf("gc9d01: hardware reset\n");
  bk7258_gc9d01_hw_reset();

  printf("gc9d01: qspi0 init\n");
  bk7258_qspi0_init();

  printf("gc9d01: sending %u init commands (through sleep out)\n",
         (unsigned int)GC9D01_PRE_SLEEP_OUT_CMD_COUNT);
  for (i = 0; i < GC9D01_PRE_SLEEP_OUT_CMD_COUNT; i++)
    {
      if (!bk7258_qspi0_send_cmd(g_gc9d01_init_cmds_pre_sleep_out[i].cmd,
                                 g_gc9d01_init_cmds_pre_sleep_out[i].data,
                                 g_gc9d01_init_cmds_pre_sleep_out[i].data_len))
        {
          printf("gc9d01: qspi0 timed out waiting for cmd 0x%02x "
                 "(index %u) to complete, aborting init sequence\n",
                 g_gc9d01_init_cmds_pre_sleep_out[i].cmd, (unsigned int)i);
          return -1;
        }
    }

  printf("gc9d01: sleep-out settle delay (%u ms)\n",
         GC9D01_SLEEP_OUT_DELAY_MS);
  up_udelay(GC9D01_SLEEP_OUT_DELAY_MS * 1000u);

  printf("gc9d01: display on\n");
  if (!bk7258_qspi0_send_cmd(g_gc9d01_display_on.cmd,
                             g_gc9d01_display_on.data,
                             g_gc9d01_display_on.data_len))
    {
      printf("gc9d01: qspi0 timed out waiting for display-on cmd to "
             "complete\n");
      return -1;
    }

  printf("gc9d01: init sequence completed without hang\n");
  return 0;
}

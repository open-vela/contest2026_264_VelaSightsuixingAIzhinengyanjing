/****************************************************************************
 * board/beken/chips/bk7258/bk7258_i2c_sim.c
 *
 * Software bit-banged I2C master.  START/STOP/ACK/byte sequencing ported
 * from bk_avdk_smp release/v3.1.1
 * ap/middleware/driver/i2c/sim_i2c_driver.c (non-CONFIG_SIM_I2C_HW_BOARD_V3
 * branch: I2cStart/I2cStop/I2cSend/I2cReceive/I2cWaitForAck/I2cAck/
 * I2cNoAck/I2cMemWrite/I2cMemRead).
 *
 * Timing note: the reference implementation uses a raw busy-loop
 * (`I2cDelay(count)` = `for(i=0;i<count;i++);`) calibrated for a specific
 * CPU frequency ("RISCV 120Mhz" per that file's header comment), with
 * SCL_DELAY=25 loop iterations for 100kHz mode.  A raw iteration count is
 * not portable across CPU frequencies/compiler optimization levels, so
 * this port uses up_udelay() with a fixed microsecond value instead:
 * standard I2C 100kHz timing requires each SCL half-period to be at least
 * ~5us (4.7us minimum per the I2C-bus specification's t_LOW/t_HIGH at
 * Standard-mode), so 5us is used here as a conservative, frequency-
 * independent equivalent of the reference's SCL_DELAY, matching the same
 * "at least one Standard-mode half-period" intent as the original
 * ap/middleware/driver/i2c/sim_i2c_driver.c CLK_DELAY_100K constant.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <stdbool.h>
#include <stdint.h>

#include "bk7258_gpio.h"
#include "bk7258_i2c_sim.h"

#define I2C_SIM_SCL_PIN   0u
#define I2C_SIM_SDA_PIN   1u

#define I2C_SIM_SCL_DELAY_US   5u
#define I2C_SIM_PIN_DELAY_US   1u

static void i2c_sim_scl_delay(void)
{
  up_udelay(I2C_SIM_SCL_DELAY_US);
}

static void i2c_sim_pin_delay(void)
{
  up_udelay(I2C_SIM_PIN_DELAY_US);
}

/* --- Low-level pin helpers, mirroring sim_i2c_driver.c's I2cSetSda /
 * I2cSetScl naming --- */

static void i2c_sim_sda_output(void)
{
  bk7258_gpio_output(I2C_SIM_SDA_PIN, true);
}

static void i2c_sim_sda_input(void)
{
  /* NOTE: sim_i2c_driver.c's I2cSetSdaInput() only switches direction
   * (bk_gpio_disable_output + bk_gpio_enable_input), it does not touch
   * pull configuration -- the reference driver's init function
   * (sim_i2c_init()) instead calls bk_gpio_pull_down() once at startup, on
   * the theory that the I2C bus has an external pull-up resistor on the
   * PCB (standard I2C hardware requirement) and the pin only needs a
   * defined idle state, not an active pull direction, while acting as
   * input.  bk7258_gpio.c does not currently expose a "switch to input,
   * leave pull setting alone" primitive, only bk7258_gpio_input_pullup()
   * (which actively enables an internal pull-up).  Using an internal
   * pull-up here instead of leaving pull unconfigured is a deliberate,
   * minor deviation from the reference driver: it is protocol-safe (an
   * internal pull-up can only help the line return to idle-high faster,
   * never violate I2C timing) and avoids introducing a new GPIO primitive
   * for this one caller.  If GC2145 fails to ACK, this is not the
   * expected root cause (no external pull-up on the bus would be a board
   * wiring issue, not something this software choice can fix or break).
   */
  bk7258_gpio_input_pullup(I2C_SIM_SDA_PIN);
}

static void i2c_sim_sda_high(void)
{
  bk7258_gpio_write(I2C_SIM_SDA_PIN, true);
}

static void i2c_sim_sda_low(void)
{
  bk7258_gpio_write(I2C_SIM_SDA_PIN, false);
}

static void i2c_sim_scl_output(void)
{
  bk7258_gpio_output(I2C_SIM_SCL_PIN, true);
}

static void i2c_sim_scl_high(void)
{
  bk7258_gpio_write(I2C_SIM_SCL_PIN, true);
}

static void i2c_sim_scl_low(void)
{
  bk7258_gpio_write(I2C_SIM_SCL_PIN, false);
}

static bool i2c_sim_sda_read(void)
{
  return bk7258_gpio_read(I2C_SIM_SDA_PIN);
}

/* --- START/STOP/ACK sequencing, ported from sim_i2c_driver.c --- */

static void i2c_sim_scl_pulse(void)
{
  i2c_sim_scl_high();
  i2c_sim_scl_delay();

  i2c_sim_scl_low();
  i2c_sim_scl_delay();
}

/* Matches I2cSclPulse_ack(): after the 9th (ACK) clock's high phase,
 * switch SDA to input while pulling SCL low, so the next
 * i2c_sim_wait_for_ack()/byte-receive can read the slave's response. */
static void i2c_sim_scl_pulse_ack(void)
{
  i2c_sim_scl_high();
  i2c_sim_scl_delay();

  i2c_sim_scl_low();
  i2c_sim_sda_input();
  i2c_sim_scl_delay();
}

static void i2c_sim_start(void)
{
  i2c_sim_sda_output();
  i2c_sim_pin_delay();

  i2c_sim_sda_high();
  i2c_sim_pin_delay();

  i2c_sim_scl_high();
  i2c_sim_scl_delay();

  /* HIGH-to-LOW transition on SDA while SCL is HIGH: START condition. */
  i2c_sim_sda_low();
  i2c_sim_scl_delay();

  i2c_sim_scl_low();
  i2c_sim_scl_delay();
}

static void i2c_sim_stop(void)
{
  i2c_sim_scl_low();
  i2c_sim_scl_delay();

  i2c_sim_sda_low();
  i2c_sim_pin_delay();

  i2c_sim_scl_high();
  i2c_sim_scl_delay();

  /* LOW-to-HIGH transition on SDA while SCL is HIGH: STOP condition. */
  i2c_sim_sda_high();
  i2c_sim_scl_delay();
}

static void i2c_sim_ack(void)
{
  i2c_sim_sda_low();
  i2c_sim_pin_delay();

  i2c_sim_scl_pulse();
}

static void i2c_sim_noack(void)
{
  i2c_sim_sda_high();
  i2c_sim_pin_delay();

  i2c_sim_scl_pulse();
}

static bool i2c_sim_wait_for_ack(void)
{
  bool ack;

  i2c_sim_sda_input();
  i2c_sim_pin_delay();

  i2c_sim_scl_high();
  i2c_sim_scl_delay();

  ack = !i2c_sim_sda_read();

  i2c_sim_pin_delay();

  i2c_sim_scl_low();
  i2c_sim_pin_delay();
  i2c_sim_scl_delay();

  return ack;
}

static bool i2c_sim_send_byte(uint8_t data)
{
  uint8_t mask;

  i2c_sim_sda_output();

  for (mask = 128; mask != 0; mask >>= 1)
    {
      if ((data & mask) != 0)
        {
          i2c_sim_sda_high();
        }
      else
        {
          i2c_sim_sda_low();
        }

      i2c_sim_pin_delay();

      if (mask == 1)
        {
          i2c_sim_scl_pulse_ack();
        }
      else
        {
          i2c_sim_scl_pulse();
        }
    }

  return i2c_sim_wait_for_ack();
}

static uint8_t i2c_sim_receive_byte(bool last_one)
{
  uint8_t mask;
  uint8_t data = 0;

  i2c_sim_sda_input();
  i2c_sim_pin_delay();

  for (mask = 128; mask != 0; mask >>= 1)
    {
      i2c_sim_scl_high();
      i2c_sim_scl_delay();

      if (i2c_sim_sda_read())
        {
          data |= mask;
        }

      i2c_sim_pin_delay();

      i2c_sim_scl_low();
      i2c_sim_scl_delay();
    }

  i2c_sim_sda_output();
  i2c_sim_pin_delay();

  if (last_one)
    {
      i2c_sim_noack();
    }
  else
    {
      i2c_sim_ack();
    }

  return data;
}

void bk7258_i2c_sim_init(void)
{
  i2c_sim_sda_output();
  i2c_sim_scl_output();
  i2c_sim_stop();
}

/* Matches I2cMemWrite(): dev_addr is 7-bit, shifted <<1 with the R/W bit
 * cleared for the write direction.  reg_addr is sent as a single 8-bit
 * memory address byte (GC2145's registers are all 8-bit addressed; this
 * driver does not implement the 16-bit memory address variant that
 * sim_i2c_driver.c supports via I2C_MEM_ADDR_SIZE_16BIT, since it is not
 * needed for GC2145). */
bool bk7258_i2c_sim_write_reg(uint8_t dev_addr, uint8_t reg_addr,
                               uint8_t value)
{
  uint8_t addr_byte = (uint8_t)(dev_addr << 1);
  bool ok = false;

  i2c_sim_start();

  if (!i2c_sim_send_byte(addr_byte))
    {
      goto out;
    }

  if (!i2c_sim_send_byte(reg_addr))
    {
      goto out;
    }

  if (!i2c_sim_send_byte(value))
    {
      goto out;
    }

  ok = true;

out:
  i2c_sim_stop();
  return ok;
}

/* Matches I2cMemRead(): write phase (address + register address) followed
 * by a repeated START and a read phase (address | 0x01), per
 * sim_i2c_driver.c's I2cMemRead() sequencing (the non-CONFIG_AIRPLAY
 * branch, without the 40ms inter-phase STOP+delay that is only required
 * for a specific MFI chip workaround not applicable to GC2145). */
bool bk7258_i2c_sim_read_reg(uint8_t dev_addr, uint8_t reg_addr,
                              uint8_t *value)
{
  uint8_t addr_byte = (uint8_t)(dev_addr << 1);
  bool ok = false;

  i2c_sim_start();

  if (!i2c_sim_send_byte(addr_byte))
    {
      goto out;
    }

  if (!i2c_sim_send_byte(reg_addr))
    {
      goto out;
    }

  i2c_sim_start();

  if (!i2c_sim_send_byte((uint8_t)(addr_byte | 0x01u)))
    {
      goto out;
    }

  *value = i2c_sim_receive_byte(true);
  ok = true;

out:
  i2c_sim_stop();
  return ok;
}

/****************************************************************************
 * board/beken/chips/bk7258/include/bk7258_i2c1.h
 *
 * Interrupt-driven hardware I2C1 master driver.
 *
 * This replaces the earlier software-simulated (bit-banged) I2C driver
 * (bk7258_i2c_sim.c/.h) for GC2145 camera register access.  The switch
 * was forced by the board schematic (AIDK_AI玩具开发板_原理图.pdf, sheet
 * 4/6 "DVP/SENSOR/NAND/MOTOR/KEY"), which shows GC2145's SCL/SDA wired to
 * the chip's *hardware* I2C1 peripheral (network labels IIC1_SCL/
 * IIC1_SDA, with 4.7K pull-ups to VDDGPIO) -- not to a pair of plain
 * GPIOs that firmware could bit-bang arbitrarily.  The previous driver's
 * choice of GPIO0/GPIO1 for bit-banging was based on a reference
 * project's default config (bk_avdk_smp dvp_example) that does not match
 * this board: GPIO0/GPIO1 are actually wired to UART1_TXD/UART1_RXD here
 * (schematic sheet 2/6 pin table), which explains why every prior
 * hardware bring-up attempt saw the GC2145 never ACK the first I2C byte
 * -- the bit-banged signal was toggling the wrong physical pins.
 *
 * The real IIC1_SCL/IIC1_SDA nets are routed by the chip's pinmux to
 * GPIO42/GPIO43 (bk_avdk_smp ap/middleware/soc/bk7258_ap/soc/gpio_map.h
 * GPIO_I2C1_MAP_TABLE's second entry, BIT64(42)|BIT64(43); the first
 * entry, GPIO0/GPIO1, is I2C1's *other* selectable pin group used by a
 * different board, not this one).  This driver hard-codes GPIO42/43 as
 * this board's only wiring; it is not a general-purpose pinmux-selectable
 * I2C1 driver.
 *
 * Architecture: this is a from-scratch reimplementation of the interrupt
 * + semaphore master state machine in bk_avdk_smp release/v3.1.1
 * ap/middleware/driver/i2c/i2c_driver.c (i2c_hardware_memory_write_impl(),
 * i2c_master_isr_common(), i2c1_isr_common()), adapted to NuttX's
 * irq_attach()/up_enable_irq()/nxsem_wait() primitives in place of
 * Beken RTOS's bk_int_isr_register()/rtos_get_semaphore().  Only the
 * master-write path is implemented (I2C_MASTER_WRITE state sequence:
 * START -> TX_DEV_ADDR -> TX_MEM_ADDR -> TX_DATA -> STOP); master-read,
 * slave mode, 10-bit addressing, and low-power-sleep register
 * backup/restore are intentionally omitted, since GC2145 bring-up (585 +
 * 40 register writes, all 8-bit register address / 8-bit data, no
 * readback) is this driver's only consumer.
 */

#ifndef __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_I2C1_H
#define __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_I2C1_H

#include <stdbool.h>
#include <stdint.h>

/* Initializes the I2C1 peripheral: configures GPIO42/GPIO43 pinmux to
 * the I2C1_SCL/I2C1_SDA hardware function (with pull-ups, matching the
 * board's 4.7K external pull-up resistors being merely a bus-idle-state
 * backstop, not a substitute for internal pull enable), sets the bus
 * clock divider for 100kHz standard-mode operation, and attaches/enables
 * the I2C1 interrupt.  Must be called once before any
 * bk7258_i2c1_write_reg() call. */
void bk7258_i2c1_init(void);

/* Writes a single 8-bit value to an 8-bit register address on the 7-bit
 * I2C device address i2c_addr, via I2C1.  Blocks (via semaphore wait,
 * bounded by an internal timeout) until the hardware state machine
 * completes the START/address/register-address/data/STOP sequence and
 * reports the transaction's ACK/NACK outcome.  Returns true if every
 * byte (device address, register address, data) was ACKed by the slave;
 * false on any NACK or SCL-timeout condition (mirroring
 * bk7258_i2c_sim_write_reg()'s return convention so callers such as
 * bk7258_gc2145_write_reg_table() do not need to change). */
bool bk7258_i2c1_write_reg(uint8_t i2c_addr, uint8_t reg, uint8_t value);

#endif /* __VENDOR_BEKEN_CHIPS_BK7258_INCLUDE_BK7258_I2C1_H */

#ifndef SI5351_H
#define SI5351_H

#include "hardware/i2c.h"
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// 2026 board hardware constants (verified from schematic)
// ---------------------------------------------------------------------------

// Si5351a I2C address: A0 pin is pulled low on the 2026 board
#define SI5351_I2C_ADDR     0x60

// Reference crystal on the 2026 board: 24.576 MHz.
// CLK0 passes this frequency directly to the PCM1808 SCKI master clock pin.
// This is exact: 24.576 MHz = 512 × 48000 Hz = 256 × 96000 Hz.
#ifndef SI5351_XTAL_FREQ
#define SI5351_XTAL_FREQ    24576000UL
#endif

// Si5351a VCO operating range (PLLA / PLLB internal oscillator)
#define SI5351_VCO_MIN      600000000UL
#define SI5351_VCO_MAX      900000000UL

// ---------------------------------------------------------------------------
// Register addresses
// ---------------------------------------------------------------------------
#define SI5351_REG_DEV_STATUS   0
#define SI5351_REG_OEB          3    // Output Enable Control (1 = output disabled)
#define SI5351_REG_CLK0_CTRL    16   // CLK0 driver + source configuration
#define SI5351_REG_CLK1_CTRL    17   // CLK1 driver + source configuration
#define SI5351_REG_CLK2_CTRL    18   // CLK2 driver + source configuration
#define SI5351_REG_PLLA_BASE    26   // PLL A multisynth params (regs 26-33)
#define SI5351_REG_MS0_BASE     42   // MS0 multisynth params (regs 42-49) -- CLK0 / I channel
#define SI5351_REG_MS1_BASE     50   // MS1 multisynth params (regs 50-57) -- CLK1 / Q channel
#define SI5351_REG_MS2_BASE     58   // MS2 multisynth params (regs 58-65) -- CLK2 / Johnson Counter
#define SI5351_REG_CLK0_PHOFF   165  // CLK0 phase offset (units: Tvco/4)
#define SI5351_REG_CLK1_PHOFF   166  // CLK1 phase offset
#define SI5351_REG_CLK2_PHOFF   167  // CLK2 phase offset
#define SI5351_REG_PLL_RESET    177  // PLL soft reset (bit 5 = PLLA, bit 7 = PLLB)
#define SI5351_REG_XTAL_LOAD    183  // Crystal load capacitance

// Internal load capacitance values for reg 183 (bits 7:6)
#define SI5351_XTAL_LOAD_6PF    (1 << 6)
#define SI5351_XTAL_LOAD_8PF    (2 << 6)
#define SI5351_XTAL_LOAD_10PF   (3 << 6)  // 2026 board: 10 pF load caps fitted

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Initialise the Si5351a: disable all outputs, set crystal load, wait for
// SYS_INIT (register 0 bit 7) to clear.  Returns true if device is ready.
bool    si5351_init(i2c_inst_t *i2c);

// Write / read a single Si5351a register.
void    si5351_write_reg(i2c_inst_t *i2c, uint8_t reg, uint8_t val);
uint8_t si5351_read_reg (i2c_inst_t *i2c, uint8_t reg);

// Configure CLK0 (I channel) and CLK1 (Q channel) to the nearest frequency
// achievable with INTEGER PLL multiplier N and even MS divider M.
//
// Architecture (verified from 2026 board schematic):
//   CLK0 -> I_CLK (I mixer of QSD)
//   CLK1 -> Q_CLK (Q mixer of QSD), phase-offset 90 deg via CLK1_PHOFF = M
//
// The 90 deg offset is exact: PHOFF units are Tvco/4, and
//   M * Tvco/4 = Tout/4 = exactly one quarter output period => 90 deg.
// No fractional remainder, no phase dithering.
//
// Constraints (AN619 section 4.1):
//   - N: any integer 25-36 (VCO = N * 24.576 MHz in range 614-885 MHz)
//   - M: must be EVEN; odd M gives non-50% duty cycle on both I and Q
//     outputs, causing unequal charge injection and degraded image rejection
//   - PHOFF must fit in 7 bits: M <= 127 (satisfied for all 40 m entries)
//
// Returns the actual frequency achieved (Hz), or 0 if no valid (N, M) pair.
uint32_t si5351_set_freq_integer(i2c_inst_t *i2c, uint32_t freq_hz, bool direct_mode);

// Program CLK0 (I channel) and CLK1 (Q channel) from pre-computed register
// values.  Quisk sends N, P1, P2, P3 over USB CDC and the firmware programs
// them directly -- no integer search needed.
//
//   N        : even Multisynth output divider for MS0 and MS1
//   P1/P2/P3 : PLLA register parameters
//
// direct_mode: true  = DIRECT topology: CLK1_PHOFF = N for exact 90 deg.
//              false = JOHNSON topology: CLK1_PHOFF not written.
void si5351_set_freq_regs(i2c_inst_t *i2c, uint32_t N,
                          uint32_t P1, uint32_t P2, uint32_t P3,
                          bool direct_mode);

#endif // SI5351_H

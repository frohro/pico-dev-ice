#ifndef FPGA_VNA_H
#define FPGA_VNA_H

#include <stdbool.h>
#include <stdint.h>

/*
 * ============================================================================
 * RP2040 <-> Lattice iCE40UP5K-SG48 GPIO Assignments (Pico-Dev-iCE)
 * ============================================================================
 *
 * Pinout verified against Pico-Dev-iCE README hardware table:
 *   GPIO 0  -> FPGA_INT  (FPGA hardware interrupt / meas_done, active-HIGH)
 *   GPIO 4  -> SPI0_RX   (MISO: data entering Pico from FPGA)
 *   GPIO 5  -> ICE_SSN   (Manual GPIO CS, active-low, shared with FPGA boot)
 *   GPIO 6  -> SPI0_CLK  (SCK)
 *   GPIO 7  -> SPI0_TX   (MOSI: data leaving Pico to FPGA)
 *   GPIO 21 -> ICE_DONE  (FPGA CDONE status input, active-HIGH)
 *   GPIO 22 -> ~ICE_RST  (FPGA ~CRESET reset output, active-LOW)
 *   GPIO 26 -> REF       (Front-end RF mux: 0 = SDR ANT, 1 = VNA Input)
 *   GPIO 28 -> ~T/R      (T/R switch: 1 = RX Mode, 0 = TX Mode)
 *   GPIO 8..11 -> PGA0..3 (0 = default max gain, 1 = attenuate/bypass)
 *
 * Note: RP2040 GPIOs 13-16 are routed to the FPGA I2S audio bus and are left
 * untouched by SPI0.
 */
#define PIN_FPGA_INT    0u
#define PIN_SPI_MISO    4u
#define PIN_SPI_CS      5u
#define PIN_SPI_SCK     6u
#define PIN_SPI_MOSI    7u

#define PIN_PGA0        8u
#define PIN_PGA1        9u
#define PIN_PGA2        10u
#define PIN_PGA3        11u
#define PIN_FPGA_CDONE  21u
#define PIN_FPGA_CRESET 22u
#define PIN_VNA_REF     26u
#define PIN_TR_SW       28u

/* SPI bus configuration */
#define FPGA_SPI_PORT           spi0
#define FPGA_SPI_BAUD_HZ        (10u * 1000u * 1000u)

/* 8-byte SPI frame header constants */
#define FPGA_SPI_SYNC_BYTE      0xD5u
#define FPGA_SPI_PROTO_VER      0x01u
#define FPGA_SPI_PAYLOAD_LEN    0x04u

/* FPGA Register Map (Write commands < 0x80, Read commands >= 0x80) */
#define FPGA_REG_TX_FTW         0x01u  /* W: 32-bit unsigned TX NCO tuning word */
#define FPGA_REG_RX_FTW         0x02u  /* W: 32-bit unsigned RX NCO tuning word */
#define FPGA_REG_INTEG_CYCLES   0x03u  /* W: [15:0] 4-sample (12 kHz IF) cycles */
#define FPGA_REG_CLEAR_OTR      0x04u  /* W: bit[0] = 1 clears sticky ADC OTR */
#define FPGA_REG_CTRL           0x05u  /* W: [0]=start, [1]=rx_conj, [2]=clr_otr */

#define FPGA_REG_STATUS         0x80u  /* R: [0]=meas_done, [1]=busy, [2]=otr_sticky */
#define FPGA_REG_I_RESULT       0x81u  /* R: int32_t in-phase lock-in result */
#define FPGA_REG_Q_RESULT       0x82u  /* R: int32_t quadrature lock-in result */
#define FPGA_REG_RD_TX_FTW      0x83u  /* R: current TX_FTW readback */
#define FPGA_REG_RD_RX_FTW      0x84u  /* R: current RX_FTW readback */

/* CTRL register bits (0x05) */
#define FPGA_CTRL_START_MEAS    (1u << 0)
#define FPGA_CTRL_RX_CONJ       (1u << 1)
#define FPGA_CTRL_CLEAR_OTR     (1u << 2)

/* STATUS register bits (0x80) */
#define FPGA_STATUS_MEAS_DONE   (1u << 0)
#define FPGA_STATUS_BUSY        (1u << 1)
#define FPGA_STATUS_OTR_STICKY  (1u << 2)

/* ADC, IF, and DAC clock constants (Hz) */
#define VNA_F_ADC_HZ                30720000.0
#define VNA_F_IF_HZ                 12000.0
#define VNA_DAC_CLK_PRESENT_HZ      30720000.0
#define VNA_DAC_CLK_V015_HZ         122880000.0
#define VNA_BLIND_GUARD_HZ          25000.0
#define VNA_CIC_FULL_SCALE_PER_CYC  23550000.0
#define VNA_DEFAULT_INTEG_CYCLES    32u

typedef enum {
    VNA_DAC_30M72 = 0,  /* Present PCB: F_DAC = 30.72 MHz */
    VNA_DAC_122M88 = 1  /* v0.15 PCB:   F_DAC = 122.88 MHz */
} vna_dac_clk_t;

typedef struct {
    float re;
    float im;
} vna_complex_t;

void fpga_vna_init(void);
bool fpga_vna_load_stored_bitstream(void);
bool fpga_vna_is_configured(void);

void fpga_vna_write_reg(uint8_t cmd, uint32_t value);
uint32_t fpga_vna_read_reg(uint8_t cmd);

void fpga_vna_set_dac_clk(vna_dac_clk_t clk);
vna_dac_clk_t fpga_vna_get_dac_clk(void);
double fpga_vna_get_dac_clk_hz(void);

void fpga_vna_compute_tuning(uint64_t f_tx_hz,
                             uint32_t *tx_ftw_out,
                             uint32_t *rx_ftw_out,
                             uint8_t *rx_conj_out);

bool fpga_vna_measure(uint64_t f_tx_hz,
                      uint16_t integ_cycles,
                      vna_complex_t *out_iq,
                      uint32_t *out_status);

#endif /* FPGA_VNA_H */

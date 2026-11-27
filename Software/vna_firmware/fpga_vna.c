#include "fpga_vna.h"

#include <math.h>
#include <stddef.h>
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

#ifdef VNA_HAS_STORED_BITSTREAM
#include "vna_bitstream.h"
#endif

static vna_dac_clk_t g_dac_clk_mode = VNA_DAC_30M72;

static inline void cs_select(void) {
    gpio_put(PIN_SPI_CS, 0);
}

static inline void cs_deselect(void) {
    gpio_put(PIN_SPI_CS, 1);
    /* Wait >= 1 us between frames for FPGA CDC settling (CS shared with ICE_SSN). */
    busy_wait_us(2);
}

bool fpga_vna_is_configured(void) {
    return gpio_get(PIN_FPGA_CDONE) != 0;
}

static bool fpga_load_cram(const uint8_t *data, size_t len) {
    if (!data || len == 0u) {
        return false;
    }

    /* Drive ~CRESET LOW and ICE_SSN LOW to select iCE40 Slave-SPI configuration mode. */
    gpio_put(PIN_FPGA_CRESET, 0);
    gpio_put(PIN_SPI_CS, 0);
    busy_wait_us(50);

    /* Release ~CRESET HIGH while holding ICE_SSN LOW, wait >= 1200 us for CRAM clear. */
    gpio_put(PIN_FPGA_CRESET, 1);
    busy_wait_us(1500);

    /* Send 8 dummy clocks with ICE_SSN HIGH per Lattice iCE40 TN-02001. */
    gpio_put(PIN_SPI_CS, 1);
    const uint8_t dummy = 0x00u;
    spi_write_blocking(FPGA_SPI_PORT, &dummy, 1);

    /* Drive ICE_SSN LOW and stream the packed .bin bitstream MSB-first. */
    gpio_put(PIN_SPI_CS, 0);
    spi_write_blocking(FPGA_SPI_PORT, data, len);

    /* Drive ICE_SSN HIGH and supply >= 100 clocks until CDONE rises. */
    gpio_put(PIN_SPI_CS, 1);
    const uint8_t zeros[16] = {0};
    for (int attempt = 0; attempt < 10; attempt++) {
        spi_write_blocking(FPGA_SPI_PORT, zeros, sizeof(zeros));
        if (gpio_get(PIN_FPGA_CDONE)) {
            break;
        }
    }

    /* Supply >= 49 additional clocks after CDONE HIGH so user I/Os release cleanly. */
    spi_write_blocking(FPGA_SPI_PORT, zeros, 8);
    busy_wait_us(10);

    return gpio_get(PIN_FPGA_CDONE) != 0;
}

bool fpga_vna_load_stored_bitstream(void) {
#ifdef VNA_HAS_STORED_BITSTREAM
    bool ok = fpga_load_cram(vna_fpga_bitstream, vna_fpga_bitstream_len);
    if (ok) {
        fpga_vna_write_reg(FPGA_REG_CLEAR_OTR, 0x00000001u);
    }
    return ok;
#else
    return false;
#endif
}

void fpga_vna_init(void) {
    /* Route analog front-end mux to VNA Input (REF=1), RX path (~T/R=1), max PGA gain (0). */
    const uint8_t pga_pins[4] = {PIN_PGA0, PIN_PGA1, PIN_PGA2, PIN_PGA3};
    for (int i = 0; i < 4; i++) {
        gpio_init(pga_pins[i]);
        gpio_put(pga_pins[i], 0);
        gpio_set_dir(pga_pins[i], GPIO_OUT);
    }
    gpio_init(PIN_VNA_REF);
    gpio_put(PIN_VNA_REF, 1);
    gpio_set_dir(PIN_VNA_REF, GPIO_OUT);

    gpio_init(PIN_TR_SW);
    gpio_put(PIN_TR_SW, 1);
    gpio_set_dir(PIN_TR_SW, GPIO_OUT);

    /* Configure FPGA CDONE (input) and ~CRESET (output, default HIGH). */
    gpio_init(PIN_FPGA_CDONE);
    gpio_set_dir(PIN_FPGA_CDONE, GPIO_IN);

    gpio_init(PIN_FPGA_CRESET);
    gpio_put(PIN_FPGA_CRESET, 1);
    gpio_set_dir(PIN_FPGA_CRESET, GPIO_OUT);

    /* Drive CS HIGH before enabling output so ICE_SSN never glitches LOW. */
    gpio_init(PIN_SPI_CS);
    gpio_put(PIN_SPI_CS, 1);
    gpio_set_dir(PIN_SPI_CS, GPIO_OUT);
    gpio_put(PIN_SPI_CS, 1);

    /* Configure FPGA measurement-done interrupt input. */
    gpio_init(PIN_FPGA_INT);
    gpio_set_dir(PIN_FPGA_INT, GPIO_IN);
    gpio_pull_down(PIN_FPGA_INT);

    /* Configure SPI0: Mode 0 (CPOL=0, CPHA=0), 8-bit, MSB-first, 10 MHz. */
    spi_init(FPGA_SPI_PORT, FPGA_SPI_BAUD_HZ);
    spi_set_format(FPGA_SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    gpio_set_function(PIN_SPI_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_MISO, GPIO_FUNC_SPI);

    /* Program the iCE40UP5K volatile CRAM with embedded vna_top.bin before VNA startup. */
    fpga_vna_load_stored_bitstream();

    /* Clear any sticky OTR state from power-up. */
    fpga_vna_write_reg(FPGA_REG_CLEAR_OTR, 0x00000001u);
}

void fpga_vna_write_reg(uint8_t cmd, uint32_t value) {
    uint8_t tx[8] = {
        FPGA_SPI_SYNC_BYTE,
        FPGA_SPI_PROTO_VER,
        cmd,
        FPGA_SPI_PAYLOAD_LEN,
        (uint8_t)(value & 0xFFu),
        (uint8_t)((value >> 8) & 0xFFu),
        (uint8_t)((value >> 16) & 0xFFu),
        (uint8_t)((value >> 24) & 0xFFu)
    };

    cs_select();
    spi_write_blocking(FPGA_SPI_PORT, tx, sizeof(tx));
    cs_deselect();
}

uint32_t fpga_vna_read_reg(uint8_t cmd) {
    uint8_t tx[8] = {
        FPGA_SPI_SYNC_BYTE,
        FPGA_SPI_PROTO_VER,
        cmd,
        FPGA_SPI_PAYLOAD_LEN,
        0x00u, 0x00u, 0x00u, 0x00u
    };
    uint8_t rx[8] = {0};

    cs_select();
    spi_write_read_blocking(FPGA_SPI_PORT, tx, rx, sizeof(tx));
    cs_deselect();

    return ((uint32_t)rx[4]) |
           ((uint32_t)rx[5] << 8) |
           ((uint32_t)rx[6] << 16) |
           ((uint32_t)rx[7] << 24);
}

void fpga_vna_set_dac_clk(vna_dac_clk_t clk) {
    g_dac_clk_mode = clk;
}

vna_dac_clk_t fpga_vna_get_dac_clk(void) {
    return g_dac_clk_mode;
}

double fpga_vna_get_dac_clk_hz(void) {
    return (g_dac_clk_mode == VNA_DAC_122M88) ? VNA_DAC_CLK_V015_HZ
                                              : VNA_DAC_CLK_PRESENT_HZ;
}

void fpga_vna_compute_tuning(uint64_t f_tx_hz,
                             uint32_t *tx_ftw_out,
                             uint32_t *rx_ftw_out,
                             uint8_t *rx_conj_out) {
    double f_dac = fpga_vna_get_dac_clk_hz();
    if (f_dac <= 0.0) {
        f_dac = VNA_DAC_CLK_PRESENT_HZ;
    }

    double f_tx = (double)f_tx_hz;
    int64_t n = (int64_t)llround(f_tx / VNA_F_ADC_HZ);
    double zone_center = (double)n * VNA_F_ADC_HZ;
    double f_alias_raw = fabs(f_tx - zone_center);

    double f_eff = f_tx;
    if (f_alias_raw < VNA_BLIND_GUARD_HZ) {
        if (n <= 0 && f_tx < zone_center + VNA_BLIND_GUARD_HZ) {
            f_eff = VNA_BLIND_GUARD_HZ;
        } else if (f_tx >= zone_center) {
            f_eff = zone_center + VNA_BLIND_GUARD_HZ;
        } else {
            f_eff = zone_center - VNA_BLIND_GUARD_HZ;
        }
    } else if (fabs(f_alias_raw - (0.5 * VNA_F_ADC_HZ)) < VNA_BLIND_GUARD_HZ) {
        if (f_tx >= zone_center) {
            f_eff = zone_center + (0.5 * VNA_F_ADC_HZ) - VNA_BLIND_GUARD_HZ;
        } else {
            f_eff = zone_center - (0.5 * VNA_F_ADC_HZ) + VNA_BLIND_GUARD_HZ;
        }
    }

    double tx_norm = fmod(f_eff / f_dac, 1.0);
    if (tx_norm < 0.0) {
        tx_norm += 1.0;
    }
    uint32_t tx_ftw = (uint32_t)(uint64_t)llround(tx_norm * 4294967296.0);

    n = (int64_t)llround(f_eff / VNA_F_ADC_HZ);
    zone_center = (double)n * VNA_F_ADC_HZ;

    double f_rx = 0.0;
    uint8_t rx_conj = 0u;

    if (f_eff >= zone_center) {
        double f_alias = f_eff - zone_center;
        if (f_alias < 0.0) {
            f_alias = 0.0;
        }
        if (f_alias >= VNA_F_IF_HZ) {
            f_rx = f_alias - VNA_F_IF_HZ;
            rx_conj = 0u;
        } else {
            f_rx = VNA_F_IF_HZ - f_alias;
            rx_conj = 1u;
        }
    } else {
        double f_alias = zone_center - f_eff;
        if (f_alias < 0.0) {
            f_alias = 0.0;
        }
        rx_conj = 1u;
        f_rx = f_alias + VNA_F_IF_HZ;
    }

    f_rx = fmod(f_rx, VNA_F_ADC_HZ);
    if (f_rx < 0.0) {
        f_rx += VNA_F_ADC_HZ;
    }
    uint32_t rx_ftw = (uint32_t)(uint64_t)llround((f_rx / VNA_F_ADC_HZ) * 4294967296.0);

    if (tx_ftw_out) {
        *tx_ftw_out = tx_ftw;
    }
    if (rx_ftw_out) {
        *rx_ftw_out = rx_ftw;
    }
    if (rx_conj_out) {
        *rx_conj_out = rx_conj;
    }
}

bool fpga_vna_measure(uint64_t f_tx_hz,
                      uint16_t integ_cycles,
                      vna_complex_t *out_iq,
                      uint32_t *out_status) {
    if (integ_cycles == 0u) {
        integ_cycles = 1u;
    }

    uint32_t tx_ftw = 0u;
    uint32_t rx_ftw = 0u;
    uint8_t rx_conj = 0u;
    fpga_vna_compute_tuning(f_tx_hz, &tx_ftw, &rx_ftw, &rx_conj);

    fpga_vna_write_reg(FPGA_REG_TX_FTW, tx_ftw);
    fpga_vna_write_reg(FPGA_REG_RX_FTW, rx_ftw);
    fpga_vna_write_reg(FPGA_REG_INTEG_CYCLES, (uint32_t)integ_cycles);

    uint32_t ctrl = (((uint32_t)rx_conj & 0x01u) << 1) | FPGA_CTRL_START_MEAS;
    fpga_vna_write_reg(FPGA_REG_CTRL, ctrl);

    busy_wait_us(10);

    uint64_t expected_us = (uint64_t)integ_cycles * 84ULL;
    absolute_time_t int_deadline = make_timeout_time_us(expected_us + 50000ULL);

    bool irq_done = false;
    while (!time_reached(int_deadline)) {
        if (gpio_get(PIN_FPGA_INT)) {
            irq_done = true;
            break;
        }
        tight_loop_contents();
    }

    uint32_t status = fpga_vna_read_reg(FPGA_REG_STATUS);
    if (!irq_done && !(status & FPGA_STATUS_MEAS_DONE)) {
        absolute_time_t poll_deadline = make_timeout_time_us(expected_us + 100000ULL);
        while (!time_reached(poll_deadline)) {
            status = fpga_vna_read_reg(FPGA_REG_STATUS);
            if ((status & FPGA_STATUS_MEAS_DONE) || !(status & FPGA_STATUS_BUSY)) {
                break;
            }
            sleep_us(50);
        }
    }

    int32_t i_res = (int32_t)fpga_vna_read_reg(FPGA_REG_I_RESULT);
    int32_t q_res = (int32_t)fpga_vna_read_reg(FPGA_REG_Q_RESULT);

    if (status & FPGA_STATUS_OTR_STICKY) {
        fpga_vna_write_reg(FPGA_REG_CLEAR_OTR, 0x00000001u);
    }

    if (out_status) {
        *out_status = status;
    }

    double full_scale = VNA_CIC_FULL_SCALE_PER_CYC * (double)integ_cycles;
    if (full_scale <= 0.0) {
        full_scale = VNA_CIC_FULL_SCALE_PER_CYC;
    }

    double re = (double)i_res / full_scale;
    double im = (double)q_res / full_scale;

    if (rx_conj) {
        im = -im;
    }

    if (out_iq) {
        out_iq->re = (float)re;
        out_iq->im = (float)im;
    }

    return (status & FPGA_STATUS_MEAS_DONE) != 0u || !(status & FPGA_STATUS_BUSY);
}

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/spi.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"

#include "pico/cyw43_arch.h"
#include "cyw43_internal.h"
#include "openhpsdr.h"
#include "wifi_config.h"
#include "lwip/dhcp.h"

#include "boards.h"
#include "agc_control.h"
#include "ddc_protocol.h"
#include "ice_cram.h"
#include "ice_fpga.h"
#include "ice_fpga_data.h"
#include "ice_spi.h"
#include "i2s_rx.pio.h"
#ifdef DDC_HAS_STORED_RX
#include "fpga_bitstream_rx.h"
#endif

#define DDC_I2S_DATA_PIN 14
#define DDC_I2S_BCK_PIN 15
#define DDC_I2S_WS_PIN 16
#define DDC_TR_PIN 28
#define DDC_REF_PIN 26
#define DDC_FPGA_INT_PIN 0
#define DDC_PGA_GPIO_BASE 8
#define DDC_PGA_GPIO_COUNT 4
#define DDC_DEFAULT_SAMPLE_RATE 48000u
#define DDC_RUNTIME_SPI_BAUD_HZ 10000000u
#define DDC_MAX_WORDS_PER_BUFFER 256u

#define DDC_AUDIO_RING_COUNT 64u
static uint32_t audio_ring[DDC_AUDIO_RING_COUNT][DDC_MAX_WORDS_PER_BUFFER];
static volatile uint8_t ring_write_idx = 0;
static volatile uint8_t ring_read_idx = 0;
static volatile uint32_t ring_overruns = 0;

static uint g_pio_offset;
static int dma_channel_a = -1;
static int dma_channel_b = -1;
static const uint32_t words_per_buffer = HPSDR_WORDS_PER_PACKET;

static bool i2s_running = false;
static bool runtime_spi_ready = false;
static bool fpga_ready = false;
static uint32_t sample_rate = DDC_DEFAULT_SAMPLE_RATE;
static volatile bool fpga_interrupt_pending = false;
static ddc_agc_state_t agc_state;
static uint32_t last_frequency_hz = 7050000u;
static uint32_t freq_cmd_count = 0;

static const char *s_current_ssid = DEFAULT_WIFI_SSID_PRIMARY;
static bool s_ip_configured = false;
static uint32_t s_noip_since = 0;

static bool fpga_write_command(uint8_t command, uint32_t value);
static bool fpga_set_frequency(uint32_t frequency_hz);
static void pga_set_code(uint8_t code);
static bool apply_sample_rate(uint32_t rate);
static void i2s_start(void);
static void i2s_stop(void);

static volatile uint32_t s_pending_freq = 0;
static volatile uint32_t s_pending_rate = 0;
static volatile int8_t s_pending_gain = -1;

static void on_hpsdr_freq_change(uint32_t freq_hz) {
    s_pending_freq = freq_hz;
}

static void on_hpsdr_rate_change(uint32_t rate_hz) {
    s_pending_rate = rate_hz;
}

static void on_hpsdr_gain_change(uint8_t pga_code) {
    s_pending_gain = (int8_t)pga_code;
}

static void tr_set_receive(bool receive)
{
    gpio_put(DDC_TR_PIN, receive ? 1u : 0u);
}

static void pga_set_code(uint8_t code)
{
    code &= DDC_PGA_MAX_CODE;
    for (uint gpio = DDC_PGA_GPIO_BASE;
         gpio < DDC_PGA_GPIO_BASE + DDC_PGA_GPIO_COUNT;
         gpio++) {
        gpio_put(gpio, (code >> (gpio - DDC_PGA_GPIO_BASE)) & 1u);
    }
}

static void fpga_interrupt_handler(uint gpio, uint32_t events)
{
    (void)events;
    if (gpio == DDC_FPGA_INT_PIN) {
        fpga_interrupt_pending = true;
    }
}

static void handle_fpga_interrupt(void)
{
    if (!fpga_interrupt_pending || !fpga_ready || !runtime_spi_ready) {
        return;
    }

    fpga_interrupt_pending = false;
    if (ddc_agc_on_otr(&agc_state))
        pga_set_code(agc_state.pga_code);
    fpga_write_command(DDC_FPGA_CMD_CLEAR_OTR, 1u);
}

static void agc_task(void)
{
    bool fpga_int_high;
    uint64_t now_ms;

    if (!fpga_ready)
        return;

    fpga_int_high = gpio_get(DDC_FPGA_INT_PIN);
    now_ms = time_us_64() / 1000u;
    if (ddc_agc_tick(&agc_state, fpga_int_high, now_ms))
        pga_set_code(agc_state.pga_code);
}

static volatile uint32_t dma_a_irq_count = 0;
static volatile uint32_t dma_b_irq_count = 0;

static void dma_handler(void)
{
    uint32_t status = dma_hw->ints0;

    if (status & (1u << dma_channel_a)) {
        dma_hw->ints0 = 1u << dma_channel_a;
        dma_a_irq_count++;
        uint8_t next_write = (ring_write_idx + 1u) % DDC_AUDIO_RING_COUNT;
        if (next_write == ring_read_idx) {
            ring_overruns++;
            ring_read_idx = (ring_read_idx + 1u) % DDC_AUDIO_RING_COUNT;
        }
        ring_write_idx = next_write;

        // Channel A runs on even slots (0, 2, 4, 6...): arm next even slot
        uint8_t next_a = (ring_write_idx + 1u) % DDC_AUDIO_RING_COUNT;
        dma_channel_set_write_addr(dma_channel_a, audio_ring[next_a], false);
        dma_channel_set_trans_count(dma_channel_a, words_per_buffer, false);
    }
    if (status & (1u << dma_channel_b)) {
        dma_hw->ints0 = 1u << dma_channel_b;
        dma_b_irq_count++;
        uint8_t next_write = (ring_write_idx + 1u) % DDC_AUDIO_RING_COUNT;
        if (next_write == ring_read_idx) {
            ring_overruns++;
            ring_read_idx = (ring_read_idx + 1u) % DDC_AUDIO_RING_COUNT;
        }
        ring_write_idx = next_write;

        // Channel B runs on odd slots (1, 3, 5, 7...): arm next odd slot
        uint8_t next_b = (ring_write_idx + 1u) % DDC_AUDIO_RING_COUNT;
        dma_channel_set_write_addr(dma_channel_b, audio_ring[next_b], false);
        dma_channel_set_trans_count(dma_channel_b, words_per_buffer, false);
    }
}

static void i2s_configure(void)
{
    pio_gpio_init(pio0, DDC_I2S_DATA_PIN);
    pio_gpio_init(pio0, DDC_I2S_BCK_PIN);
    pio_gpio_init(pio0, DDC_I2S_WS_PIN);
    gpio_pull_up(DDC_I2S_DATA_PIN);
    gpio_pull_up(DDC_I2S_BCK_PIN);
    gpio_pull_up(DDC_I2S_WS_PIN);
    pio_sm_set_consecutive_pindirs(pio0, 0, DDC_I2S_DATA_PIN, 3, false);

    pio_sm_config config = i2s_rx_program_get_default_config(g_pio_offset);
    sm_config_set_in_pins(&config, DDC_I2S_DATA_PIN);
    sm_config_set_in_shift(&config, false, true, 32);
    sm_config_set_clkdiv(&config, 1.0f);
    pio_sm_init(pio0, 0, g_pio_offset, &config);
}

static void i2s_stop(void)
{
    if (dma_channel_a < 0 || dma_channel_b < 0) {
        return;
    }

    dma_channel_abort(dma_channel_a);
    dma_channel_abort(dma_channel_b);
    dma_hw->ints0 = (1u << dma_channel_a) | (1u << dma_channel_b);
    pio_sm_set_enabled(pio0, 0, false);
    pio_sm_clear_fifos(pio0, 0);
    i2s_running = false;
    irq_set_enabled(DMA_IRQ_0, false);
}

static void i2s_start(void)
{
    if (i2s_running || !fpga_ready || !openhpsdr_is_active()) {
        return;
    }

    // Stop and clear PIO SM and FIFOs completely first
    pio_sm_set_enabled(pio0, 0, false);
    pio_sm_clear_fifos(pio0, 0);

    i2s_configure();

    // Reset PIO execution state to sync anchor at start of program
    pio_sm_restart(pio0, 0);
    pio_sm_exec(pio0, 0, pio_encode_jmp(g_pio_offset));
    pio_sm_clear_fifos(pio0, 0);

    // Initialize ring buffer pointers and arm slot 0 on channel A, slot 1 on channel B
    ring_write_idx = 0;
    ring_read_idx = 0;
    dma_channel_set_write_addr(dma_channel_a, audio_ring[0], false);
    dma_channel_set_trans_count(dma_channel_a, words_per_buffer, false);
    dma_channel_set_write_addr(dma_channel_b, audio_ring[1], false);
    dma_channel_set_trans_count(dma_channel_b, words_per_buffer, false);
    dma_hw->ints0 = (1u << dma_channel_a) | (1u << dma_channel_b);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_start(dma_channel_a);

    // NOW enable PIO state machine with guaranteed empty FIFO.
    // The PIO begins with wait 1 pin 2; wait 0 pin 2 (WS falling edge),
    // guaranteeing that word 0 received by DMA is ALWAYS Left (I).
    pio_sm_set_enabled(pio0, 0, true);
    i2s_running = true;
}

static void runtime_spi_stop(void)
{
    if (!runtime_spi_ready) {
        return;
    }

    ice_spi_chip_deselect(FPGA_DATA.bus.CS_cram);
    ice_spi_deinit();
    runtime_spi_ready = false;
}

static bool fpga_runtime_init(void)
{
    if (runtime_spi_ready) {
        return true;
    }

    ice_spi_init_cs_pin(FPGA_DATA.bus.CS_cram, false);
    runtime_spi_ready = ice_spi_init(FPGA_DATA.bus);
    if (runtime_spi_ready)
        spi_set_baudrate(FPGA_DATA.bus.peripheral, DDC_RUNTIME_SPI_BAUD_HZ);
    return runtime_spi_ready;
}

static bool fpga_write_command(uint8_t command, uint32_t value)
{
    uint8_t frame[DDC_FPGA_FRAME_HEADER_LEN + 4u];

    if (!runtime_spi_ready) {
        return false;
    }

    ddc_make_u32_command(frame, command, value);
    ice_spi_chip_select(FPGA_DATA.bus.CS_cram);
    ice_spi_write_blocking(frame, sizeof(frame));
    ice_spi_chip_deselect(FPGA_DATA.bus.CS_cram);
    return true;
}

static bool fpga_set_sample_rate(uint32_t rate)
{
    return fpga_write_command(DDC_FPGA_CMD_SET_SAMPLE_RATE, rate);
}

static bool fpga_set_frequency(uint32_t frequency_hz)
{
    if (frequency_hz == 0 || frequency_hz > DDC_FPGA_MAX_FREQUENCY_HZ) {
        return false;
    }
    return fpga_write_command(DDC_FPGA_CMD_SET_FREQUENCY,
                              ddc_frequency_to_fcw(frequency_hz));
}

static void pga_configure(void)
{
    ddc_agc_init(&agc_state);
    for (uint gpio = DDC_PGA_GPIO_BASE;
         gpio < DDC_PGA_GPIO_BASE + DDC_PGA_GPIO_COUNT;
         gpio++) {
        gpio_init(gpio);
        gpio_set_dir(gpio, GPIO_OUT);
    }
    pga_set_code(agc_state.pga_code);
}

static void fpga_interrupt_configure(void)
{
    gpio_init(DDC_FPGA_INT_PIN);
    gpio_set_dir(DDC_FPGA_INT_PIN, GPIO_IN);
    gpio_pull_down(DDC_FPGA_INT_PIN);
    gpio_set_irq_enabled_with_callback(DDC_FPGA_INT_PIN,
                                        GPIO_IRQ_EDGE_RISE,
                                        true,
                                        fpga_interrupt_handler);
}

static bool configure_fpga_bitstream(const uint8_t *bitstream, size_t size)
{
    ice_fpga_init(FPGA_DATA, 48);
    ice_fpga_stop(FPGA_DATA);
    if (!ice_cram_open(FPGA_DATA)) {
        return false;
    }
    if (ice_cram_write(bitstream, size) < 0) {
        ice_cram_close();
        return false;
    }
    return ice_cram_close();
}

static bool configure_stored_fpga(void)
{
#ifdef DDC_HAS_STORED_RX
    return configure_fpga_bitstream(ddc_fpga_rx_bitstream, DDC_FPGA_RX_BITSTREAM_SIZE);
#else
    return false;
#endif
}

static bool restore_fpga_runtime(void)
{
    if (!fpga_runtime_init()) {
        return false;
    }

    fpga_ready = true;
    fpga_set_sample_rate(sample_rate);
    fpga_set_frequency(last_frequency_hz);
    i2s_start();
    return true;
}

static bool apply_sample_rate(uint32_t rate)
{
    bool was_running;

    if (rate != 48000u && rate != 96000u) {
        return false;
    }
    if (!fpga_ready) {
        return false;
    }
    if (rate == sample_rate) {
        return true;
    }

    was_running = i2s_running;
    if (was_running) {
        i2s_stop();
    }
    if (!fpga_set_sample_rate(rate)) {
        if (was_running) {
            i2s_start();
        }
        return false;
    }

    sample_rate = rate;
    if (was_running) {
        i2s_start();
    }
    return true;
}

static void audio_task(void)
{
    if (!i2s_running || !openhpsdr_is_active()) {
        return;
    }

    int budget = 8;
    while (ring_read_idx != ring_write_idx && budget-- > 0) {
        if (!openhpsdr_can_send()) {
            break;
        }
        const uint32_t *source = audio_ring[ring_read_idx];
        openhpsdr_push_samples(source, words_per_buffer);
        ring_read_idx = (ring_read_idx + 1u) % DDC_AUDIO_RING_COUNT;
    }
}

uint32_t get_ring_overruns(void) {
    return ring_overruns;
}

uint32_t get_dma_irq_count(void) {
    return dma_a_irq_count + dma_b_irq_count;
}

int main(void)
{
    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);
    gpio_put(25, 1);

    gpio_init(DDC_TR_PIN);
    gpio_set_dir(DDC_TR_PIN, GPIO_OUT);
    tr_set_receive(true);

    gpio_init(DDC_REF_PIN);
    gpio_set_dir(DDC_REF_PIN, GPIO_OUT);
    gpio_put(DDC_REF_PIN, 0);

    // Hardware FPGA Reset: hold CRESET_B (GPIO 22) low to allow power rails to settle
    gpio_init(22);
    gpio_set_dir(22, GPIO_OUT);
    gpio_put(22, 0);
    sleep_ms(30);
    gpio_put(22, 1);
    sleep_ms(15);

    // CDONE pin (GPIO 21)
    gpio_init(21);
    gpio_set_dir(21, GPIO_IN);
    gpio_pull_up(21);

    vreg_set_voltage(VREG_VOLTAGE_1_15);
    set_sys_clock_khz(150000, true);

    g_pio_offset = pio_add_program(pio0, &i2s_rx_program);
    pga_configure();
    fpga_interrupt_configure();
    dma_channel_a = dma_claim_unused_channel(true);
    dma_channel_b = dma_claim_unused_channel(true);

    dma_channel_config config_a = dma_channel_get_default_config(dma_channel_a);
    channel_config_set_transfer_data_size(&config_a, DMA_SIZE_32);
    channel_config_set_read_increment(&config_a, false);
    channel_config_set_write_increment(&config_a, true);
    channel_config_set_dreq(&config_a, pio_get_dreq(pio0, 0, false));
    channel_config_set_chain_to(&config_a, dma_channel_b);
    dma_channel_configure(dma_channel_a, &config_a, audio_ring[0],
                          &pio0->rxf[0], words_per_buffer, false);

    dma_channel_config config_b = dma_channel_get_default_config(dma_channel_b);
    channel_config_set_transfer_data_size(&config_b, DMA_SIZE_32);
    channel_config_set_read_increment(&config_b, false);
    channel_config_set_write_increment(&config_b, true);
    channel_config_set_dreq(&config_b, pio_get_dreq(pio0, 0, false));
    channel_config_set_chain_to(&config_b, dma_channel_a);
    dma_channel_configure(dma_channel_b, &config_b, audio_ring[1],
                          &pio0->rxf[0], words_per_buffer, false);

    dma_channel_set_irq0_enabled(dma_channel_a, true);
    dma_channel_set_irq0_enabled(dma_channel_b, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);

#ifdef DDC_FPGA_BOOT_FROM_STORED
    fpga_ready = false;
    for (int retry = 0; retry < 5; retry++) {
        gpio_put(22, 0);
        sleep_ms(10);
        gpio_put(22, 1);
        sleep_ms(10);

        if (configure_stored_fpga()) {
            if (restore_fpga_runtime()) {
                fpga_ready = true;
                break;
            }
        }
        sleep_ms(50);
    }
#else
    fpga_ready = false;
#endif

    bool wifi_ok = false;
    if (cyw43_arch_init() == 0) {
        wifi_ok = true;
        cyw43_arch_enable_sta_mode();
        cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
        uint32_t auth = (DEFAULT_WIFI_PASSWORD[0] == '\0') ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK;
        const char *pass_param = (DEFAULT_WIFI_PASSWORD[0] == '\0') ? NULL : DEFAULT_WIFI_PASSWORD;
        cyw43_arch_wifi_connect_async(s_current_ssid, pass_param, auth);
        openhpsdr_init(on_hpsdr_freq_change, on_hpsdr_rate_change, on_hpsdr_gain_change);
    }
    uint32_t last_led_poll = 0;
    uint32_t last_reconnect_ms = to_ms_since_boot(get_absolute_time());

    while (true) {
        cyw43_arch_poll();
        handle_fpga_interrupt();
        agc_task();
        audio_task();

        if (s_pending_freq != 0) {
            uint32_t freq = s_pending_freq;
            s_pending_freq = 0;
            if (fpga_ready) {
                fpga_set_frequency(freq);
                last_frequency_hz = freq;
                freq_cmd_count++;
            }
        }
        if (s_pending_gain >= 0) {
            uint8_t code = (uint8_t)s_pending_gain;
            s_pending_gain = -1;
            agc_state.pga_code = code;
            pga_set_code(code);
        }
        if (s_pending_rate != 0) {
            uint32_t rate = s_pending_rate;
            s_pending_rate = 0;
            if (rate == 48000u || rate == 96000u) {
                apply_sample_rate(rate);
            }
        }

        if (wifi_ok) {
            openhpsdr_task();
            static bool s_last_hpsdr = false;
            bool hpsdr_now = openhpsdr_is_active();
            if (hpsdr_now != s_last_hpsdr) {
                s_last_hpsdr = hpsdr_now;
                if (hpsdr_now) {
                    i2s_start();
                    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
                    cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
                } else {
                    i2s_stop();
                    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
                }
            } else {
                if (hpsdr_now && !i2s_running) {
                    i2s_start();
                } else if (!hpsdr_now && i2s_running) {
                    i2s_stop();
                }
            }

            uint32_t now_ms = to_ms_since_boot(get_absolute_time());
            bool active = openhpsdr_is_active();

            // Pause status and LED polling during active streaming to eliminate SPI bus contention
            if (!active && (now_ms - last_led_poll >= 500)) {
                last_led_poll = now_ms;
                cyw43_arch_lwip_begin();
                int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
                static int s_last_led = -1;
                int led_val = 0;
                if (st == CYW43_LINK_UP) {
                    s_noip_since = 0;
                    static bool s_pm_disabled = false;
                    if (!s_pm_disabled) {
                        s_pm_disabled = true;
                        cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
                    }
                    last_reconnect_ms = now_ms;
                    led_val = (now_ms / 500) % 2;
                } else if (st == CYW43_LINK_JOIN || st == CYW43_LINK_NOIP) {
                    last_reconnect_ms = now_ms;
                    led_val = (now_ms / 250) % 2;
#ifdef STATIC_FALLBACK_IP
                    if (!s_ip_configured) {
                        if (s_noip_since == 0) s_noip_since = now_ms;
                        if (now_ms - s_noip_since >= 8000) {
                            s_ip_configured = true;
                            dhcp_stop(&cyw43_state.netif[CYW43_ITF_STA]);
                            ip4_addr_t ip, nm, gw;
                            ip4addr_aton(STATIC_FALLBACK_IP, &ip);
                            ip4addr_aton(STATIC_FALLBACK_NETMASK, &nm);
                            ip4addr_aton(STATIC_FALLBACK_GATEWAY, &gw);
                            netif_set_addr(&cyw43_state.netif[CYW43_ITF_STA], &ip, &nm, &gw);
                            netif_set_link_up(&cyw43_state.netif[CYW43_ITF_STA]);
                            netif_set_up(&cyw43_state.netif[CYW43_ITF_STA]);
                        }
                    }
#endif
                } else {
                    s_noip_since = 0;
                    s_ip_configured = false;
                    led_val = (now_ms / 1000) % 2;
                    if (now_ms - last_reconnect_ms >= 15000) {
                        last_reconnect_ms = now_ms;
                        uint32_t auth = (DEFAULT_WIFI_PASSWORD[0] == 0) ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK;
                        const char *pass_param = (DEFAULT_WIFI_PASSWORD[0] == 0) ? NULL : DEFAULT_WIFI_PASSWORD;
                        cyw43_arch_wifi_connect_async(s_current_ssid, pass_param, auth);
                    }
                }
                if (led_val != s_last_led) {
                    s_last_led = led_val;
                    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_val);
                }
                cyw43_arch_lwip_end();
            }
        }
    }
}

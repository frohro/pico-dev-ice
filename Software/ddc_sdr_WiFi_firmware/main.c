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
#include "lwip/etharp.h"

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

// INCREASED TO 128 (Provides 168ms of shock absorption at 96kHz!)
#define DDC_AUDIO_RING_COUNT 128u
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

// Cross-core communication variables
static volatile uint32_t s_pending_freq = 0;
static volatile uint32_t s_pending_rate = 0;
static volatile int8_t s_pending_gain = -1;
static volatile bool s_stream_active = false;

static void on_hpsdr_freq_change(uint32_t freq_hz) { s_pending_freq = freq_hz; }
static void on_hpsdr_rate_change(uint32_t rate_hz) { s_pending_rate = rate_hz; }
static void on_hpsdr_gain_change(uint8_t pga_code) { s_pending_gain = (int8_t)pga_code; }

static void pga_set_code(uint8_t code) {
    code &= DDC_PGA_MAX_CODE;
    for (uint gpio = DDC_PGA_GPIO_BASE; gpio < DDC_PGA_GPIO_BASE + DDC_PGA_GPIO_COUNT; gpio++) {
        gpio_put(gpio, (code >> (gpio - DDC_PGA_GPIO_BASE)) & 1u);
    }
}

static void fpga_interrupt_handler(uint gpio, uint32_t events) {
    (void)gpio;
    (void)events;
    fpga_interrupt_pending = true;
}

static bool fpga_write_command(uint8_t command, uint32_t value) {
    uint8_t frame[DDC_FPGA_FRAME_HEADER_LEN + 4u];
    if (!runtime_spi_ready) return false;
    ddc_make_u32_command(frame, command, value);
    ice_spi_chip_select(FPGA_DATA.bus.CS_cram);
    ice_spi_write_blocking(frame, sizeof(frame));
    ice_spi_chip_deselect(FPGA_DATA.bus.CS_cram);
    return true;
}

static void handle_fpga_interrupt(void) {
    if (!fpga_interrupt_pending || !fpga_ready || !runtime_spi_ready) return;
    fpga_interrupt_pending = false;
    if (ddc_agc_on_otr(&agc_state)) pga_set_code(agc_state.pga_code);
    fpga_write_command(DDC_FPGA_CMD_CLEAR_OTR, 1u);
}

static void agc_task(void) {
    if (!fpga_ready) return;
    bool fpga_int_high = gpio_get(DDC_FPGA_INT_PIN);
    uint64_t now_ms = time_us_64() / 1000u;
    if (ddc_agc_tick(&agc_state, fpga_int_high, now_ms)) pga_set_code(agc_state.pga_code);
}

static volatile uint32_t dma_a_irq_count = 0;
static volatile uint32_t dma_b_irq_count = 0;
static volatile uint8_t dma_a_slot = 0;
static volatile uint8_t dma_b_slot = 1;

static void dma_handler(void) {
    uint32_t status = dma_hw->ints0;
    
    // Core 1 DMA interrupt. Writes into ring buffer.
    if (status & (1u << dma_channel_a)) {
        dma_hw->ints0 = 1u << dma_channel_a;
        dma_a_irq_count++;
        uint8_t next_write = (dma_a_slot + 1u) % DDC_AUDIO_RING_COUNT;
        if (next_write == ring_read_idx) {
            ring_overruns++;
            ring_read_idx = (ring_read_idx + 1u) % DDC_AUDIO_RING_COUNT;
        }
        ring_write_idx = next_write;
        dma_a_slot = (dma_a_slot + 2u) % DDC_AUDIO_RING_COUNT;
        dma_channel_set_write_addr(dma_channel_a, audio_ring[dma_a_slot], false);
        dma_channel_set_trans_count(dma_channel_a, words_per_buffer, false);
    }
    if (status & (1u << dma_channel_b)) {
        dma_hw->ints0 = 1u << dma_channel_b;
        dma_b_irq_count++;
        uint8_t next_write = (dma_b_slot + 1u) % DDC_AUDIO_RING_COUNT;
        if (next_write == ring_read_idx) {
            ring_overruns++;
            ring_read_idx = (ring_read_idx + 1u) % DDC_AUDIO_RING_COUNT;
        }
        ring_write_idx = next_write;
        dma_b_slot = (dma_b_slot + 2u) % DDC_AUDIO_RING_COUNT;
        dma_channel_set_write_addr(dma_channel_b, audio_ring[dma_b_slot], false);
        dma_channel_set_trans_count(dma_channel_b, words_per_buffer, false);
    }

    if (!dma_channel_is_busy(dma_channel_a) && !dma_channel_is_busy(dma_channel_b)) {
        dma_channel_start(dma_channel_a);
    }
}

static void i2s_stop(void) {
    if (dma_channel_a < 0) return;
    dma_channel_abort(dma_channel_a);
    dma_channel_abort(dma_channel_b);
    dma_hw->ints0 = (1u << dma_channel_a) | (1u << dma_channel_b);
    pio_sm_set_enabled(pio0, 0, false);
    pio_sm_clear_fifos(pio0, 0);
    i2s_running = false;
    irq_set_enabled(DMA_IRQ_0, false);
}

static void i2s_start(void) {
    if (i2s_running || !fpga_ready) return;
    pio_sm_set_enabled(pio0, 0, false);
    pio_sm_clear_fifos(pio0, 0);
    
    pio_gpio_init(pio0, DDC_I2S_DATA_PIN); pio_gpio_init(pio0, DDC_I2S_BCK_PIN); pio_gpio_init(pio0, DDC_I2S_WS_PIN);
    gpio_pull_up(DDC_I2S_DATA_PIN); gpio_pull_up(DDC_I2S_BCK_PIN); gpio_pull_up(DDC_I2S_WS_PIN);
    pio_sm_set_consecutive_pindirs(pio0, 0, DDC_I2S_DATA_PIN, 3, false);
    
    pio_sm_config config = i2s_rx_program_get_default_config(g_pio_offset);
    sm_config_set_in_pins(&config, DDC_I2S_DATA_PIN);
    sm_config_set_in_shift(&config, false, true, 32);
    sm_config_set_clkdiv(&config, 1.0f);
    pio_sm_init(pio0, 0, g_pio_offset, &config);

    pio_sm_restart(pio0, 0);
    pio_sm_exec(pio0, 0, pio_encode_jmp(g_pio_offset));
    pio_sm_clear_fifos(pio0, 0);

    dma_a_slot = 0;
    dma_b_slot = 1;
    ring_write_idx = 0;
    ring_read_idx = 0;
    dma_channel_set_write_addr(dma_channel_a, audio_ring[0], false);
    dma_channel_set_trans_count(dma_channel_a, words_per_buffer, false);
    dma_channel_set_write_addr(dma_channel_b, audio_ring[1], false);
    dma_channel_set_trans_count(dma_channel_b, words_per_buffer, false);
    dma_hw->ints0 = (1u << dma_channel_a) | (1u << dma_channel_b);
    
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_start(dma_channel_a);
    pio_sm_set_enabled(pio0, 0, true);
    i2s_running = true;
}

uint32_t get_ring_overruns(void) { return ring_overruns; }
uint32_t get_dma_irq_count(void) { return dma_a_irq_count + dma_b_irq_count; }

// ==============================================================================
// CORE 1: DEDICATED HARD REAL-TIME SDR ENGINE (PIO, DMA, FPGA, AGC)
// ==============================================================================
void core1_sdr_entry(void) {
    // 1. Hardware Pin Init
    gpio_init(DDC_TR_PIN); gpio_set_dir(DDC_TR_PIN, GPIO_OUT); gpio_put(DDC_TR_PIN, 1);
    gpio_init(DDC_REF_PIN); gpio_set_dir(DDC_REF_PIN, GPIO_OUT); gpio_put(DDC_REF_PIN, 0);

    // FPGA CRESET (GPIO 22)
    gpio_init(22); gpio_set_dir(22, GPIO_OUT); gpio_put(22, 0);
    sleep_ms(30); gpio_put(22, 1); sleep_ms(15);

    g_pio_offset = pio_add_program(pio0, &i2s_rx_program);
    
    // Init PGA and Interrupts
    ddc_agc_init(&agc_state);
    for (uint gpio = DDC_PGA_GPIO_BASE; gpio < DDC_PGA_GPIO_BASE + DDC_PGA_GPIO_COUNT; gpio++) {
        gpio_init(gpio); gpio_set_dir(gpio, GPIO_OUT);
    }
    pga_set_code(agc_state.pga_code);

    gpio_init(DDC_FPGA_INT_PIN); gpio_set_dir(DDC_FPGA_INT_PIN, GPIO_IN); gpio_pull_down(DDC_FPGA_INT_PIN);
    gpio_set_irq_enabled_with_callback(DDC_FPGA_INT_PIN, GPIO_IRQ_EDGE_RISE, true, fpga_interrupt_handler);

    // Setup DMA on Core 1
    dma_channel_a = dma_claim_unused_channel(true);
    dma_channel_b = dma_claim_unused_channel(true);

    dma_channel_config config_a = dma_channel_get_default_config(dma_channel_a);
    channel_config_set_transfer_data_size(&config_a, DMA_SIZE_32);
    channel_config_set_read_increment(&config_a, false);
    channel_config_set_write_increment(&config_a, true);
    channel_config_set_dreq(&config_a, pio_get_dreq(pio0, 0, false));
    channel_config_set_chain_to(&config_a, dma_channel_b);
    dma_channel_configure(dma_channel_a, &config_a, audio_ring[0], &pio0->rxf[0], words_per_buffer, false);

    dma_channel_config config_b = dma_channel_get_default_config(dma_channel_b);
    channel_config_set_transfer_data_size(&config_b, DMA_SIZE_32);
    channel_config_set_read_increment(&config_b, false);
    channel_config_set_write_increment(&config_b, true);
    channel_config_set_dreq(&config_b, pio_get_dreq(pio0, 0, false));
    channel_config_set_chain_to(&config_b, dma_channel_a);
    dma_channel_configure(dma_channel_b, &config_b, audio_ring[1], &pio0->rxf[0], words_per_buffer, false);

    dma_channel_set_irq0_enabled(dma_channel_a, true);
    dma_channel_set_irq0_enabled(dma_channel_b, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_priority(DMA_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);

    // Boot FPGA
#ifdef DDC_FPGA_BOOT_FROM_STORED
    fpga_ready = false;
    for (int retry = 0; retry < 5; retry++) {
        gpio_put(22, 0); sleep_ms(10); gpio_put(22, 1); sleep_ms(10);
        ice_fpga_init(FPGA_DATA, 48); ice_fpga_stop(FPGA_DATA);
        if (ice_cram_open(FPGA_DATA)) {
            if (ice_cram_write(ddc_fpga_rx_bitstream, DDC_FPGA_RX_BITSTREAM_SIZE) >= 0) {
                if (ice_cram_close()) {
                    ice_spi_init_cs_pin(FPGA_DATA.bus.CS_cram, false);
                    runtime_spi_ready = ice_spi_init(FPGA_DATA.bus);
                    if (runtime_spi_ready) spi_set_baudrate(FPGA_DATA.bus.peripheral, DDC_RUNTIME_SPI_BAUD_HZ);
                    fpga_ready = true;
                    fpga_write_command(DDC_FPGA_CMD_SET_SAMPLE_RATE, sample_rate);
                    fpga_write_command(DDC_FPGA_CMD_SET_FREQUENCY, ddc_frequency_to_fcw(last_frequency_hz));
                    break;
                }
            }
        }
        sleep_ms(50);
    }
#endif

    // Core 1 Hard Real-Time SDR Loop
    bool s_last_active = false;
    while (true) {
        handle_fpga_interrupt();
        agc_task();

        // Handle Cross-Core Commands from Core 0
        if (s_pending_freq != 0) {
            uint32_t freq = s_pending_freq;
            s_pending_freq = 0;
            if (fpga_ready) fpga_write_command(DDC_FPGA_CMD_SET_FREQUENCY, ddc_frequency_to_fcw(freq));
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
            // The gateware currently only supports 48 kHz
            if (rate == 48000u && fpga_ready) {
                sample_rate = 48000u;
            }
        }

        // Manage I2S state based on network active status
        bool active_now = s_stream_active;
        if (active_now != s_last_active) {
            s_last_active = active_now;
            if (active_now) i2s_start();
            else i2s_stop();
        }
    }
}

// ==============================================================================
// CORE 0: DEDICATED WI-FI & LWIP TASK LOOP (POLLED MODE)
// ==============================================================================
int main(void) {
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    set_sys_clock_khz(125000, true);

    stdio_init_all();

    // 1. Launch Core 1 (Hard Real-Time SDR Engine)
    multicore_launch_core1(core1_sdr_entry);

    // 2. Initialize Wi-Fi on Core 0 (Polled Mode)
    if (cyw43_arch_init() != 0) {
        while (1) tight_loop_contents();
    }
    cyw43_arch_enable_sta_mode();
    cyw43_wifi_pm(&cyw43_state, CYW43_PERFORMANCE_PM);

    uint32_t auth = (DEFAULT_WIFI_PASSWORD[0] == '\0') ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK;
    const char *pass_param = (DEFAULT_WIFI_PASSWORD[0] == '\0') ? NULL : DEFAULT_WIFI_PASSWORD;
    printf("[WiFi] Connecting to %s...\n", DEFAULT_WIFI_SSID_PRIMARY);
    cyw43_arch_wifi_connect_async(DEFAULT_WIFI_SSID_PRIMARY, pass_param, auth);

    openhpsdr_init(on_hpsdr_freq_change, on_hpsdr_rate_change, on_hpsdr_gain_change);

    bool s_announced = false;
#ifdef STATIC_FALLBACK_IP
    bool s_ip_configured = false;
    uint32_t s_noip_since = 0;
#endif
    uint32_t last_reconnect_ms = to_ms_since_boot(get_absolute_time());
    uint32_t last_status_ms = 0;
    while (true) {
        // A. Service Wi-Fi events (Polled Mode)
        cyw43_arch_poll();
        openhpsdr_task();

        // B. Update cross-core streaming flag
        s_stream_active = openhpsdr_is_active();

        // C. Audio Streaming Task (Reads from Ring Buffer with Backpressure)
        if (s_stream_active) {
            int max_packets_per_loop = 8;
            while (ring_read_idx != ring_write_idx && max_packets_per_loop-- > 0) {
                if (!openhpsdr_can_send()) {
                    break;
                }
                const uint32_t *source = audio_ring[ring_read_idx];
                if (openhpsdr_push_samples(source, words_per_buffer)) {
                    ring_read_idx = (ring_read_idx + 1u) % DDC_AUDIO_RING_COUNT;
                    cyw43_arch_poll();
                } else {
                    break;
                }
            }
        }

        // D. Background Wi-Fi health & LED (only when idle to preserve streaming line-rate)
        static bool s_last_active_led = false;
        if (s_stream_active != s_last_active_led) {
            s_last_active_led = s_stream_active;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, s_stream_active ? 1 : 0);
        }

        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (!s_stream_active && (now_ms - last_status_ms >= 500)) {
            last_status_ms = now_ms;
            int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
            if (st == CYW43_LINK_UP) {
#ifdef STATIC_FALLBACK_IP
                s_noip_since = 0;
#endif
                last_reconnect_ms = now_ms;
                if (!s_announced) {
                    s_announced = true;
                    etharp_gratuitous(&cyw43_state.netif[CYW43_ITF_STA]);
                    printf("[WiFi] Connected! IP: %s\n",
                           ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA])));
                }
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, (now_ms / 500) % 2); // 1 Hz heartbeat
            } else if (st == CYW43_LINK_JOIN || st == CYW43_LINK_NOIP) {
                last_reconnect_ms = now_ms;
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, (now_ms / 250) % 2); // 2 Hz blink
#ifdef STATIC_FALLBACK_IP
                if (!s_ip_configured) {
                    if (s_noip_since == 0) s_noip_since = now_ms;
                    if (now_ms - s_noip_since >= 6000) {
                        s_ip_configured = true;
                        dhcp_stop(&cyw43_state.netif[CYW43_ITF_STA]);
                        ip4_addr_t ip, nm, gw;
                        ip4addr_aton(STATIC_FALLBACK_IP, &ip);
                        ip4addr_aton(STATIC_FALLBACK_NETMASK, &nm);
                        ip4addr_aton(STATIC_FALLBACK_GATEWAY, &gw);
                        netif_set_addr(&cyw43_state.netif[CYW43_ITF_STA], &ip, &nm, &gw);
                        netif_set_link_up(&cyw43_state.netif[CYW43_ITF_STA]);
                        netif_set_up(&cyw43_state.netif[CYW43_ITF_STA]);
                        etharp_gratuitous(&cyw43_state.netif[CYW43_ITF_STA]);
                        printf("[WiFi] Static fallback IP configured: %s\n", STATIC_FALLBACK_IP);
                    }
                }
#endif
            } else if (st < 0) {
                s_announced = false;
#ifdef STATIC_FALLBACK_IP
                s_noip_since = 0;
                s_ip_configured = false;
#endif
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, (now_ms / 1000) % 2); // 0.5 Hz blink
                if (now_ms - last_reconnect_ms >= 10000) {
                    last_reconnect_ms = now_ms;
                    printf("[WiFi] Link failure (%d), reconnecting to %s...\n", st, DEFAULT_WIFI_SSID_PRIMARY);
                    uint32_t a = (DEFAULT_WIFI_PASSWORD[0] == '\0') ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK;
                    const char *p = (DEFAULT_WIFI_PASSWORD[0] == '\0') ? NULL : DEFAULT_WIFI_PASSWORD;
                    cyw43_arch_wifi_connect_async(DEFAULT_WIFI_SSID_PRIMARY, p, a);
                }
            }
        }
    }
}

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/board_api.h"
#include "tusb.h"

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

#ifdef PICO_CYW43_SUPPORTED
#include "pico/cyw43_arch.h"
#include "cyw43_internal.h"
#include "openhpsdr.h"
#include "wifi_config.h"
#include "lwip/dhcp.h"
#endif

#include "boards.h"
#include "agc_control.h"
#include "ddc_protocol.h"
#include "ice_cram.h"
#include "ice_fpga.h"
#include "ice_fpga_data.h"
#include "ice_spi.h"
#include "i2s_rx.pio.h"
#include "i2s_tx.pio.h"
#ifdef DDC_HAS_STORED_RX
#include "fpga_bitstream_rx.h"
#endif
#ifdef DDC_HAS_STORED_TX
#include "fpga_bitstream_tx.h"
#endif

#define DDC_I2S_DATA_PIN 14
#define DDC_I2S_TX_PIN 13
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
#define DDC_TX_BUFFER_COUNT 4u
#define DDC_AUDIO_CAPTURE_INTERFACE 3u
#define DDC_AUDIO_PLAYBACK_INTERFACE 4u
#define DDC_LINE_BUFFER_SIZE 80u

typedef enum {
    DDC_FPGA_IMAGE_RX,
    DDC_FPGA_IMAGE_TX,
    DDC_FPGA_IMAGE_DFU,
} ddc_fpga_image_t;

typedef struct {
    const uint8_t *data;
    size_t size;
} ddc_fpga_bitstream_t;

#define DDC_AUDIO_RING_COUNT 32u
static uint32_t audio_ring[DDC_AUDIO_RING_COUNT][DDC_MAX_WORDS_PER_BUFFER];
static volatile uint8_t ring_write_idx = 0;
static volatile uint8_t ring_read_idx = 0;
static volatile uint32_t ring_overruns = 0;
static uint32_t tx_audio_buffers[DDC_TX_BUFFER_COUNT][DDC_MAX_WORDS_PER_BUFFER];
static uint32_t tx_silence_buffer[DDC_MAX_WORDS_PER_BUFFER];
static uint g_pio_offset;
static uint g_tx_pio_offset;
static int dma_channel_a = -1;
static int dma_channel_b = -1;
static int tx_dma_channel = -1;
static dma_channel_config_t tx_dma_config;
static volatile uint32_t words_per_buffer = 96u;
static volatile uint32_t ready_buffers;
static volatile uint32_t tx_ready_mask;
static volatile bool tx_dma_active;
static volatile bool tx_dma_silence;
static volatile uint8_t tx_dma_buffer;
static bool i2s_running;
static bool tx_running;
static bool runtime_spi_ready;
static bool fpga_ready;
static bool update_prepared;
static ddc_fpga_image_t active_fpga_image = DDC_FPGA_IMAGE_DFU;
static uint32_t sample_rate = DDC_DEFAULT_SAMPLE_RATE;
static volatile bool fpga_interrupt_pending;
static ddc_agc_state_t agc_state;

static uint8_t capture_alt;
static uint8_t playback_alt;
static uint32_t audio_requested_rate = DDC_DEFAULT_SAMPLE_RATE;
static uint8_t audio_mute[3];
static uint8_t tx_fill_buffer;
static uint32_t tx_fill_words;

static char line_buffer[DDC_LINE_BUFFER_SIZE];
static uint8_t line_length;
static bool ready_message_sent;
static uint32_t last_frequency_hz = 7050000u;
static uint32_t freq_cmd_count;
static volatile uint32_t s_main_loop_counter = 0;
static volatile uint32_t prof_tud = 0, prof_cdc = 0, prof_fpga = 0, prof_agc = 0, prof_audio = 0, prof_task = 0, prof_led = 0, prof_wifi = 0;

static bool fpga_write_command(uint8_t command, uint32_t value);
static bool fpga_set_frequency(uint32_t frequency_hz);
static void pga_set_code(uint8_t code);
static bool apply_sample_rate(uint32_t rate);
static void i2s_start(void);
static void i2s_stop(void);
static void cdc_write(const char *text);

#ifdef PICO_CYW43_SUPPORTED
static const char *s_wifi_ssids[] = {
    DEFAULT_WIFI_SSID_PRIMARY,
#ifdef DEFAULT_WIFI_SSID_SECONDARY
    DEFAULT_WIFI_SSID_SECONDARY,
#endif
};
static int s_wifi_ssid_idx = 0;
static const char *s_current_ssid = DEFAULT_WIFI_SSID_PRIMARY;
static bool s_ip_configured = false;
static uint32_t s_noip_since = 0;

static void on_hpsdr_freq_change(uint32_t freq_hz) {
    if (fpga_ready) {
        fpga_set_frequency(freq_hz);
        last_frequency_hz = freq_hz;
        freq_cmd_count++;
    }
}

static void on_hpsdr_rate_change(uint32_t rate_hz) {
    apply_sample_rate(rate_hz);
}

static void on_hpsdr_gain_change(uint8_t pga_code) {
    agc_state.pga_code = pga_code;
    pga_set_code(pga_code);
}

static int wifi_scan_result_cb(void *env, const cyw43_ev_scan_result_t *r) {
    (void)env;
    if (r) {
        char buf[128];
        snprintf(buf, sizeof(buf), "SCAN: SSID='%s' RSSI=%d CH=%d AUTH=0x%lx\r\n",
                 r->ssid, (int)r->rssi, (int)r->channel, (unsigned long)r->auth_mode);
        cdc_write(buf);
    }
    return 0;
}
#endif

static bool fpga_get_stored_image(ddc_fpga_image_t image,
                                  ddc_fpga_bitstream_t *bitstream)
{
    if (bitstream == NULL) {
        return false;
    }

    switch (image) {
    case DDC_FPGA_IMAGE_RX:
#ifdef DDC_HAS_STORED_RX
        bitstream->data = ddc_fpga_rx_bitstream;
        bitstream->size = DDC_FPGA_RX_BITSTREAM_SIZE;
        return true;
#else
        return false;
#endif
    case DDC_FPGA_IMAGE_TX:
#ifdef DDC_HAS_STORED_TX
        bitstream->data = ddc_fpga_tx_bitstream;
        bitstream->size = DDC_FPGA_TX_BITSTREAM_SIZE;
        return true;
#else
        return false;
#endif
    default:
        return false;
    }
}

static const char *fpga_image_name(ddc_fpga_image_t image)
{
    switch (image) {
    case DDC_FPGA_IMAGE_RX:
        return "RX";
    case DDC_FPGA_IMAGE_TX:
        return "TX";
    default:
        return "DFU";
    }
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

    if (!fpga_ready || update_prepared)
        return;

    fpga_int_high = gpio_get(DDC_FPGA_INT_PIN);
    now_ms = time_us_64() / 1000u;
    if (ddc_agc_tick(&agc_state, fpga_int_high, now_ms))
        pga_set_code(agc_state.pga_code);
}

static void cdc_write(const char *text)
{
    tud_cdc_write_str(text);
    tud_cdc_write_flush();
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
    if (tx_dma_channel >= 0 && (status & (1u << tx_dma_channel))) {
        uint32_t buffer_bit;

        dma_hw->ints0 = 1u << tx_dma_channel;
        buffer_bit = tx_dma_silence ? 0u : 1u << tx_dma_buffer;
        tx_dma_active = false;
        if (buffer_bit != 0u) {
            tx_ready_mask &= ~buffer_bit;
        }
        tx_dma_silence = false;
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
    ready_buffers = 0;
    i2s_running = false;
    if (!tx_running) {
        irq_set_enabled(DMA_IRQ_0, false);
    }
}

static void i2s_start(void)
{
    bool needed = (capture_alt > 0);
#ifdef PICO_CYW43_SUPPORTED
    needed = needed || openhpsdr_is_active();
#endif
    if (i2s_running || !fpga_ready || !needed) {
        return;
    }

#ifdef PICO_CYW43_SUPPORTED
    if (openhpsdr_is_active()) {
        words_per_buffer = HPSDR_WORDS_PER_PACKET;
    } else {
        words_per_buffer = (sample_rate == 96000u) ? 192u : 96u;
    }
#else
    words_per_buffer = (sample_rate == 96000u) ? 192u : 96u;
#endif

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
    ready_buffers = 0;
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_start(dma_channel_a);

    // NOW enable PIO state machine with guaranteed empty FIFO.
    // The PIO begins with 'wait 1 pin 2; wait 0 pin 2' (WS falling edge),
    // guaranteeing that word 0 received by DMA is ALWAYS Left (I).
    pio_sm_set_enabled(pio0, 0, true);
    i2s_running = true;
}

static void tx_configure(void)
{
    pio_gpio_init(pio0, DDC_I2S_TX_PIN);
    pio_gpio_init(pio0, DDC_I2S_DATA_PIN);
    pio_gpio_init(pio0, DDC_I2S_BCK_PIN);
    pio_gpio_init(pio0, DDC_I2S_WS_PIN);
    gpio_pull_down(DDC_I2S_BCK_PIN);
    gpio_pull_down(DDC_I2S_WS_PIN);
    pio_sm_set_consecutive_pindirs(pio0, 1, DDC_I2S_DATA_PIN, 3, false);
    pio_sm_set_consecutive_pindirs(pio0, 1, DDC_I2S_TX_PIN, 1, true);

    pio_sm_config config = i2s_tx_program_get_default_config(g_tx_pio_offset);
    sm_config_set_in_pins(&config, DDC_I2S_DATA_PIN);
    sm_config_set_out_pins(&config, DDC_I2S_TX_PIN, 1);
    sm_config_set_out_shift(&config, false, true, 32);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&config, 1.0f);
    pio_sm_init(pio0, 1, g_tx_pio_offset, &config);
}

static void tx_stop(void)
{
    uint32_t saved_interrupts = save_and_disable_interrupts();

    if (tx_dma_channel >= 0) {
        dma_channel_abort(tx_dma_channel);
        dma_hw->ints0 = 1u << tx_dma_channel;
    }
    pio_sm_set_enabled(pio0, 1, false);
    pio_sm_clear_fifos(pio0, 1);
    tx_ready_mask = 0;
    tx_dma_active = false;
    tx_dma_silence = false;
    tx_fill_buffer = 0;
    tx_fill_words = 0;
    tx_running = false;
    tr_set_receive(true);
    if (!i2s_running) {
        irq_set_enabled(DMA_IRQ_0, false);
    }
    restore_interrupts(saved_interrupts);
}

static void tx_start_next_dma(void)
{
    uint32_t ready;
    uint32_t saved_interrupts;
    uint8_t index;

    saved_interrupts = save_and_disable_interrupts();
    if (!tx_running || tx_dma_active || tx_dma_channel < 0) {
        restore_interrupts(saved_interrupts);
        return;
    }

    ready = tx_ready_mask;
    for (index = 0; index < DDC_TX_BUFFER_COUNT; index++) {
        if (ready & (1u << index)) {
            tx_dma_buffer = index;
            tx_dma_silence = false;
            tx_dma_active = true;
            dma_channel_configure(tx_dma_channel, &tx_dma_config,
                                  &pio0->txf[1],
                                  tx_audio_buffers[index],
                                  words_per_buffer,
                                  true);
            restore_interrupts(saved_interrupts);
            return;
        }
    }

    tx_dma_silence = true;
    tx_dma_active = true;
    dma_channel_configure(tx_dma_channel, &tx_dma_config,
                          &pio0->txf[1], tx_silence_buffer,
                          words_per_buffer, true);
    restore_interrupts(saved_interrupts);
}

static void tx_start(void)
{
    if (tx_running || !fpga_ready || playback_alt == 0) {
        return;
    }

    tx_configure();
    tx_running = true;
    irq_set_enabled(DMA_IRQ_0, true);
    tx_start_next_dma();
    pio_sm_restart(pio0, 1);
    pio_sm_set_enabled(pio0, 1, true);
    tr_set_receive(false);
}

static bool tx_select_fill_buffer(void)
{
    uint32_t occupied;
    uint32_t saved_interrupts;
    uint8_t offset;

    if (tx_fill_words != 0) {
        return true;
    }

    saved_interrupts = save_and_disable_interrupts();
    occupied = tx_ready_mask;
    if (tx_dma_active && !tx_dma_silence) {
        occupied |= 1u << tx_dma_buffer;
    }

    for (offset = 0; offset < DDC_TX_BUFFER_COUNT; offset++) {
        uint8_t index = (uint8_t)((tx_fill_buffer + offset) % DDC_TX_BUFFER_COUNT);
        if ((occupied & (1u << index)) == 0) {
            tx_fill_buffer = index;
            restore_interrupts(saved_interrupts);
            return true;
        }
    }
    restore_interrupts(saved_interrupts);
    return false;
}

static void tx_task(void)
{
    uint8_t packet[582];

    if (!tx_running || !tud_audio_mounted()) {
        return;
    }

    while (tud_audio_available() >= 6u && tx_select_fill_buffer()) {
        uint32_t available_frames = tud_audio_available() / 6u;
        uint32_t capacity_frames = (words_per_buffer - tx_fill_words) / 2u;
        uint32_t frames = available_frames < capacity_frames
                        ? available_frames : capacity_frames;
        uint16_t bytes;
        uint16_t received;

        if (frames == 0) {
            tx_ready_mask |= 1u << tx_fill_buffer;
            tx_fill_words = 0;
            tx_fill_buffer = (uint8_t)((tx_fill_buffer + 1u) % DDC_TX_BUFFER_COUNT);
            continue;
        }

        bytes = (uint16_t)(frames * 6u);
        received = tud_audio_read(packet, bytes);
        received -= (uint16_t)(received % 6u);
        if (received == 0) {
            break;
        }
        for (uint32_t offset = 0; offset < received; offset += 6u) {
            uint32_t left = (uint32_t)packet[offset]
                          | ((uint32_t)packet[offset + 1u] << 8)
                          | ((uint32_t)packet[offset + 2u] << 16);
            uint32_t right = (uint32_t)packet[offset + 3u]
                           | ((uint32_t)packet[offset + 4u] << 8)
                           | ((uint32_t)packet[offset + 5u] << 16);
            if (audio_mute[0]) {
                left = 0;
                right = 0;
            }
            tx_audio_buffers[tx_fill_buffer][tx_fill_words++] = left << 8;
            tx_audio_buffers[tx_fill_buffer][tx_fill_words++] = right << 8;
        }

        if (tx_fill_words == words_per_buffer) {
            uint32_t saved_interrupts = save_and_disable_interrupts();
            tx_ready_mask |= 1u << tx_fill_buffer;
            restore_interrupts(saved_interrupts);
            tx_fill_words = 0;
            tx_fill_buffer = (uint8_t)((tx_fill_buffer + 1u) % DDC_TX_BUFFER_COUNT);
        }
    }

    tx_start_next_dma();
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

static bool configure_stored_fpga(ddc_fpga_image_t image)
{
    ddc_fpga_bitstream_t bitstream;

    if (!fpga_get_stored_image(image, &bitstream)) {
        return false;
    }
    return configure_fpga_bitstream(bitstream.data, bitstream.size);
}

static bool restore_fpga_runtime(void)
{
    if (!fpga_runtime_init()) {
        return false;
    }

    fpga_ready = true;
    fpga_set_sample_rate(sample_rate);
    i2s_start();
    tx_start();
    return true;
}

static bool apply_sample_rate(uint32_t rate)
{
    bool was_running;
    bool was_tx_running;

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
    was_tx_running = tx_running;
    if (was_running) {
        i2s_stop();
    }
    if (was_tx_running) {
        tx_stop();
    }
    if (!fpga_set_sample_rate(rate)) {
        if (was_running) {
            i2s_start();
        }
        if (was_tx_running) {
            tx_start();
        }
        return false;
    }

    sample_rate = rate;
    words_per_buffer = rate == 96000u ? 192u : 96u;
    if (was_running) {
        i2s_start();
    }
    if (was_tx_running) {
        tx_start();
    }
    return true;
}

static bool prepare_fpga_update(void)
{
    if (update_prepared) {
        return true;
    }

    i2s_stop();
    tx_stop();
    runtime_spi_stop();
    fpga_ready = false;
    update_prepared = true;
    return true;
}

static void complete_fpga_update(bool success)
{
    update_prepared = false;
    fpga_ready = false;

    if (!success || !restore_fpga_runtime()) {
        active_fpga_image = DDC_FPGA_IMAGE_DFU;
        return;
    }
    active_fpga_image = DDC_FPGA_IMAGE_DFU;
}

static bool cancel_fpga_update(void)
{
    if (!update_prepared) {
        return true;
    }

    update_prepared = false;
    if (!restore_fpga_runtime()) {
        fpga_ready = false;
        active_fpga_image = DDC_FPGA_IMAGE_DFU;
        return false;
    }

    return true;
}

static bool switch_stored_fpga(ddc_fpga_image_t image)
{
    bool success;
    ddc_fpga_bitstream_t bitstream;

    if (image == DDC_FPGA_IMAGE_DFU ||
        !fpga_get_stored_image(image, &bitstream)) {
        return false;
    }
    if (!prepare_fpga_update()) {
        return false;
    }

    success = configure_stored_fpga(image);
    complete_fpga_update(success);
    if (success && fpga_ready) {
        active_fpga_image = image;
    }
    return success && fpga_ready;
}

#ifdef PICO_CYW43_SUPPORTED
static inline bool cyw43_can_send(void) {
    cyw43_int_t *ci = (cyw43_int_t *)&cyw43_state;
    if (ci->wlan_flow_control) {
        return false;
    }
    if (ci->wwd_sdpcm_last_bus_data_credit == ci->wwd_sdpcm_packet_transmit_sequence_number) {
        cyw43_arch_lwip_begin();
        cyw43_poll();
        cyw43_arch_lwip_end();
        if (ci->wwd_sdpcm_last_bus_data_credit == ci->wwd_sdpcm_packet_transmit_sequence_number) {
            return false;
        }
    }
    return true;
}
#endif

static void audio_task(void)
{
    if (!i2s_running) {
        return;
    }

    uint32_t words = words_per_buffer;
    int budget = 4;

    while (ring_read_idx != ring_write_idx && budget-- > 0) {
        const uint32_t *source = audio_ring[ring_read_idx];

#ifdef PICO_CYW43_SUPPORTED
        if (openhpsdr_is_active()) {
            openhpsdr_push_samples(source, words);
        } else
#endif
        if (tud_audio_mounted() && capture_alt > 0) {
            tu_fifo_t *fifo = tud_audio_get_ep_in_ff();
            if (fifo && tu_fifo_remaining(fifo) >= words * 3u) {
                static uint8_t packed[DDC_MAX_WORDS_PER_BUFFER * 3u];
                for (uint32_t index = 0; index < words; index++) {
                    uint32_t word = source[index];
                    packed[3u * index] = (uint8_t)(word >> 8);
                    packed[3u * index + 1u] = (uint8_t)(word >> 16);
                    packed[3u * index + 2u] = (uint8_t)(word >> 24);
                }
                tud_audio_write(packed, (uint16_t)(words * 3u));
            } else {
                return;
            }
        }

        ring_read_idx = (ring_read_idx + 1u) % DDC_AUDIO_RING_COUNT;
    }
}

static void handle_line(const char *line, uint8_t length)
{
    char reply[96];

    if (length == 0) {
        return;
    }

    if (strncmp(line, "VER", 3) == 0) {
        cdc_write("VER,DDC SDR 0.2\r\nOK\r\n");
        return;
    }
    if (strncmp(line, "XTAL", 4) == 0) {
        cdc_write("XTAL,30720000\r\nOK\r\n");
        return;
    }
    if (strncmp(line, "MODE", 4) == 0) {
        cdc_write("MODE,DDC\r\nOK\r\n");
        return;
    }
    if (strcmp(line, "FPGA,STATUS") == 0) {
        snprintf(reply, sizeof(reply), "FPGA,%s\r\nOK\r\n",
                 fpga_image_name(active_fpga_image));
        cdc_write(reply);
        return;
    }
    if (strcmp(line, "FPGA,LOAD,RX") == 0) {
        cdc_write(switch_stored_fpga(DDC_FPGA_IMAGE_RX)
                      ? "FPGA,RX\r\nOK\r\n"
                      : "ERROR,RX image unavailable\r\n");
        return;
    }
    if (strcmp(line, "FPGA,LOAD,TX") == 0) {
        cdc_write(switch_stored_fpga(DDC_FPGA_IMAGE_TX)
                      ? "FPGA,TX\r\nOK\r\n"
                      : "ERROR,TX image unavailable\r\n");
        return;
    }
    if (strcmp(line, "FREQ,") == 0 || strcmp(line, "FREQ") == 0) {
        snprintf(reply, sizeof(reply), "%lu\r\nOK\r\n",
                 (unsigned long)last_frequency_hz);
        cdc_write(reply);
        return;
    }
    if (strncmp(line, "FREQ,", 5) == 0) {
        uint32_t frequency_hz = (uint32_t)strtoul(line + 5, NULL, 10);
        if (fpga_ready && fpga_set_frequency(frequency_hz)) {
            last_frequency_hz = frequency_hz;
            freq_cmd_count++;
            snprintf(reply, sizeof(reply), "%lu\r\nOK\r\n",
                     (unsigned long)frequency_hz);
            cdc_write(reply);
        } else {
            cdc_write("ERROR,frequency rejected\r\n");
        }
        return;
    }
    if (strncmp(line, "RATE,", 5) == 0) {
        uint32_t rate = (uint32_t)strtoul(line + 5, NULL, 10);
        if (apply_sample_rate(rate)) {
            freq_cmd_count++;
            snprintf(reply, sizeof(reply), "RATE,%lu\r\nOK\r\n",
                     (unsigned long)rate);
            cdc_write(reply);
        } else {
            cdc_write("ERROR,sample rate rejected\r\n");
        }
        return;
    }
    if (strcmp(line, "DFU,PREPARE") == 0) {
        if (prepare_fpga_update()) {
            cdc_write("DFU,READY\r\nOK\r\n");
        } else {
            cdc_write("ERROR,DFU busy\r\n");
        }
        return;
    }
    if (strcmp(line, "DFU,CANCEL") == 0) {
        cdc_write(cancel_fpga_update() ? "DFU,CANCELLED\r\nOK\r\n"
                                       : "ERROR,FPGA unavailable\r\n");
        return;
    }
    if (strcmp(line, "DFU,STATUS") == 0) {
        cdc_write(update_prepared ? "DFU,READY\r\nOK\r\n"
                                  : (fpga_ready ? "DFU,RUNNING\r\nOK\r\n"
                                                : "DFU,WAITING\r\nOK\r\n"));
        return;
    }
    if (strcmp(line, "DEBUG") == 0) {
        snprintf(reply, sizeof(reply),
                 "DEBUG: ready=%d, cap_alt=%d, freq_cnt=%lu, last_freq=%lu, g14=%d, g15=%d, g16=%d\r\nOK\r\n",
                 fpga_ready, capture_alt,
                 (unsigned long)freq_cmd_count, (unsigned long)last_frequency_hz,
                 gpio_get(14), gpio_get(15), gpio_get(16));
        cdc_write(reply);
        return;
    }
#ifdef PICO_CYW43_SUPPORTED
    if (strcmp(line, "HPSDR") == 0) {
        uint32_t push = 0, sent = 0, pfail = 0, uerr = 0, max_us = 0, last_us = 0;
        uint32_t stall_seq = 0, stall_dt = 0;
        openhpsdr_get_stats(&push, &sent, &pfail, &uerr, &max_us, &last_us, &stall_seq, &stall_dt);
        cyw43_int_t *ci = (cyw43_int_t *)&cyw43_state;
        snprintf(reply, sizeof(reply),
                 "HPSDR: act=%d, words=%lu, dmaA=%lu, push=%lu, sent=%lu, ovr=%lu, fc=%d, cred=%d, max_us=%lu, last_us=%lu, stall_seq=%lu\r\nOK\r\n",
                 openhpsdr_is_active(), (unsigned long)words_per_buffer,
                 (unsigned long)dma_a_irq_count,
                 (unsigned long)push, (unsigned long)sent, (unsigned long)ring_overruns,
                 (int)ci->wlan_flow_control,
                 (int)(uint8_t)(ci->wwd_sdpcm_last_bus_data_credit - ci->wwd_sdpcm_packet_transmit_sequence_number),
                 (unsigned long)max_us, (unsigned long)last_us, (unsigned long)stall_seq);
        cdc_write(reply);
        return;
    }
    if (strcmp(line, "PROF") == 0) {
        snprintf(reply, sizeof(reply),
                 "PROF: loops=%lu tud=%lu cdc=%lu fpga=%lu agc=%lu audio=%lu wifi=%lu\r\nOK\r\n",
                 (unsigned long)s_main_loop_counter,
                 (unsigned long)prof_tud, (unsigned long)prof_cdc,
                 (unsigned long)prof_fpga, (unsigned long)prof_agc,
                 (unsigned long)prof_audio, (unsigned long)prof_wifi);
        prof_tud = prof_cdc = prof_fpga = prof_agc = prof_audio = prof_wifi = 0;
        cdc_write(reply);
        return;
    }
    if (strncmp(line, "UDPBENCH,", 9) == 0 || strcmp(line, "UDPBENCH") == 0) {
        const char *target_ip_str = (strncmp(line, "UDPBENCH,", 9) == 0) ? (line + 9) : "192.168.1.205";
        ip_addr_t tip;
        ip4addr_aton(target_ip_str, &tip);
        uint32_t total_us = 0;
        uint32_t max_p_us = 0;
        int sent_count = 0;
        int err_count = 0;
        uint8_t bench_buf[1032];
        memset(bench_buf, 0x55, sizeof(bench_buf));
        for (int i = 0; i < 50; i++) {
            uint32_t t0 = time_us_32();
            cyw43_arch_lwip_begin();
            struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 1032, PBUF_POOL);
            if (p) {
                pbuf_take(p, bench_buf, 1032);
                err_t err = udp_sendto(openhpsdr_get_pcb(), p, &tip, 1024);
                if (err == ERR_OK) sent_count++;
                else err_count++;
                pbuf_free(p);
            }
            cyw43_arch_lwip_end();
            uint32_t dt = time_us_32() - t0;
            total_us += dt;
            if (dt > max_p_us) max_p_us = dt;
            busy_wait_us(2500); // 2.5 ms pacing (400 pkts/s)
            cyw43_arch_poll();
        }
        snprintf(reply, sizeof(reply), "UDPBENCH: sent=%d err=%d avg_us=%lu max_us=%lu\r\nOK\r\n",
                 sent_count, err_count, (unsigned long)(total_us / 50), (unsigned long)max_p_us);
        cdc_write(reply);
        return;
    }
#endif
    if (strcmp(line, "REF") == 0) {
        snprintf(reply, sizeof(reply), "REF,%d\r\nOK\r\n", gpio_get(DDC_REF_PIN));
        cdc_write(reply);
        return;
    }
    if (strncmp(line, "REF,", 4) == 0) {
        uint8_t ref_val = (uint8_t)strtoul(line + 4, NULL, 10);
        gpio_put(DDC_REF_PIN, ref_val ? 1u : 0u);
        snprintf(reply, sizeof(reply), "REF,%d\r\nOK\r\n", gpio_get(DDC_REF_PIN));
        cdc_write(reply);
        return;
    }
    if (strcmp(line, "PGA") == 0) {
        snprintf(reply, sizeof(reply), "PGA,%u\r\nOK\r\n", agc_state.pga_code);
        cdc_write(reply);
        return;
    }
    if (strncmp(line, "PGA,", 4) == 0) {
        uint8_t code = (uint8_t)strtoul(line + 4, NULL, 10);
        agc_state.pga_code = code;
        pga_set_code(code);
        snprintf(reply, sizeof(reply), "PGA,%u\r\nOK\r\n", code);
        cdc_write(reply);
        return;
    }
#ifdef PICO_CYW43_SUPPORTED
    if (strcmp(line, "SCAN") == 0) {
        static cyw43_wifi_scan_options_t scan_opts;
        memset(&scan_opts, 0, sizeof(scan_opts));
        cyw43_arch_lwip_begin();
        int r = cyw43_wifi_scan(&cyw43_state, &scan_opts, NULL, wifi_scan_result_cb);
        cyw43_arch_lwip_end();
        snprintf(reply, sizeof(reply), "SCAN_START,%d\r\nOK\r\n", r);
        cdc_write(reply);
        return;
    }
    if (strncmp(line, "WIFI,JOIN,", 10) == 0 || strncmp(line, "JOIN,", 5) == 0) {
        const char *target_ssid = strncmp(line, "WIFI,JOIN,", 10) == 0 ? line + 10 : line + 5;
        s_current_ssid = target_ssid;
        cyw43_arch_lwip_begin();
        int r = cyw43_arch_wifi_connect_async(target_ssid, NULL, CYW43_AUTH_OPEN);
        cyw43_arch_lwip_end();
        snprintf(reply, sizeof(reply), "JOIN_START,%d,SSID,%s\r\nOK\r\n", r, target_ssid);
        cdc_write(reply);
        return;
    }
    if (strcmp(line, "WIFI") == 0 || strcmp(line, "WIFI,STATUS") == 0 || strcmp(line, "IP") == 0) {
        cyw43_arch_lwip_begin();
        int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        const char *st_str = "DOWN";
        if (st == CYW43_LINK_UP) st_str = "UP";
        else if (st == CYW43_LINK_JOIN) st_str = "JOIN";
        else if (st == CYW43_LINK_NOIP) st_str = "NO_IP";
        else if (st == CYW43_LINK_BADAUTH) st_str = "BAD_AUTH";
        else if (st == CYW43_LINK_NONET) st_str = "NO_NET";
        else if (st == CYW43_LINK_FAIL) st_str = "FAIL";

        char ip_str[32] = "0.0.0.0";
        if (st == CYW43_LINK_UP) {
            snprintf(ip_str, sizeof(ip_str), "%s",
                     ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA])));
        }
        cyw43_arch_lwip_end();
        snprintf(reply, sizeof(reply), "WIFI,%s,IP,%s,SSID,%s\r\nOK\r\n",
                 st_str, ip_str, s_current_ssid ? s_current_ssid : "NONE");
        cdc_write(reply);
        return;
    }
#endif
    if (strcmp(line, "BOOTSEL") == 0 || strcmp(line, "RESET,BOOTSEL") == 0) {
        cdc_write("REBOOTING_BOOTSEL\r\nOK\r\n");
        reset_usb_boot(0, 0);
        return;
    }
    if (strcmp(line, "HELP") == 0 || strcmp(line, "?") == 0) {
        cdc_write("Commands:\r\n"
                  "  VER          - Show firmware version\r\n"
                  "  MODE         - Show SDR mode (DDC)\r\n"
                  "  XTAL         - Show master clock (30.72 MHz)\r\n"
                  "  FPGA,STATUS  - Show FPGA gateware status\r\n"
                  "  FREQ,<hz>    - Set NCO tuning frequency in Hz\r\n"
                  "  RATE,<hz>    - Set audio sample rate in Hz\r\n"
                  "  REF,<0|1>    - Set REF mux (0=SDR RF RX, 1=VNA)\r\n"
                  "  PGA,<0..15>  - Set attenuator code (0=max gain)\r\n"
                  "  DEBUG        - Show DMA / clock / toggle diagnostics\r\n"
                  "  BOOTSEL      - Reboot Pico to BOOTSEL flash mode\r\n"
                  "OK\r\n");
        return;
    }
    cdc_write("ERR\r\n");
}

static void cdc_task(void)
{
    while (tud_cdc_available()) {
        uint8_t byte;
        tud_cdc_read(&byte, 1);
        if (byte == 0x03) {
            line_length = 0;
            cdc_write("^C\r\n");
        } else if (byte == 0x04) {
            line_length = 0;
            cdc_write("SDR ready\r\n");
        } else if (byte == '\r' || byte == '\n') {
            if (line_length > 0) {
                line_buffer[line_length] = '\0';
                handle_line(line_buffer, line_length);
                line_length = 0;
            }
        } else if (byte == 0x08 || byte == 0x7F) {
            if (line_length > 0) {
                line_length--;
            }
        } else if (line_length < DDC_LINE_BUFFER_SIZE - 1u) {
            line_buffer[line_length++] = (char)byte;
        }
    }
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    (void)rts;
    if (dtr && !ready_message_sent) {
        cdc_write("SDR ready\r\n");
        ready_message_sent = true;
    }
    if (!dtr) {
        ready_message_sent = false;
        line_length = 0;
    }
}

void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const* p_line_coding)
{
    (void)itf;
    if (p_line_coding->bit_rate == 1200) {
        reset_usb_boot(0, 0);
    }
}

void tud_cdc_rx_cb(uint8_t itf)
{
    (void)itf;
}

bool tud_audio_set_itf_cb(uint8_t rhport,
                          tusb_control_request_t const *request)
{
    static const uint32_t rates[] = {0u, 48000u, 96000u};
    uint8_t alt = (uint8_t)(request->wValue & 0xffu);
    uint8_t interface = (uint8_t)(request->wIndex & 0xffu);

    (void)rhport;
    if (alt > 2u || (interface != DDC_AUDIO_CAPTURE_INTERFACE &&
                     interface != DDC_AUDIO_PLAYBACK_INTERFACE)) {
        return false;
    }
    if (alt != 0u &&
        ((interface == DDC_AUDIO_CAPTURE_INTERFACE && playback_alt != 0u) ||
         (interface == DDC_AUDIO_PLAYBACK_INTERFACE && capture_alt != 0u)) &&
        rates[alt] != sample_rate) {
        return false;
    }

    if (interface == DDC_AUDIO_CAPTURE_INTERFACE) {
        capture_alt = alt;
        if (alt == 0u) {
            i2s_stop();
            return true;
        }
        audio_requested_rate = rates[alt];
        if (!apply_sample_rate(audio_requested_rate)) {
            capture_alt = 0;
            i2s_stop();
            return false;
        }
        i2s_start();
    } else {
        playback_alt = alt;
        if (alt == 0u) {
            tx_stop();
            return true;
        }
        audio_requested_rate = rates[alt];
        if (!apply_sample_rate(audio_requested_rate)) {
            playback_alt = 0;
            tx_stop();
            return false;
        }
        tx_start();
    }
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport,
                                   tusb_control_request_t const *request)
{
    (void)rhport;
    if ((request->wIndex & 0xffu) == DDC_AUDIO_CAPTURE_INTERFACE) {
        i2s_stop();
    } else if ((request->wIndex & 0xffu) == DDC_AUDIO_PLAYBACK_INTERFACE) {
        tx_stop();
    }
    return true;
}

bool tud_audio_set_req_ep_cb(uint8_t rhport,
                             tusb_control_request_t const *request,
                             uint8_t *buffer)
{
    (void)rhport;
    if ((request->wValue >> 8) == AUDIO10_EP_CTRL_SAMPLING_FREQ) {
        audio_requested_rate = (uint32_t)buffer[0]
                             | ((uint32_t)buffer[1] << 8)
                             | ((uint32_t)buffer[2] << 16);
    }
    return true;
}

bool tud_audio_get_req_ep_cb(uint8_t rhport,
                             tusb_control_request_t const *request)
{
    uint8_t frequency[3];

    if ((request->wValue >> 8) != AUDIO10_EP_CTRL_SAMPLING_FREQ) {
        return false;
    }
    frequency[0] = (uint8_t)audio_requested_rate;
    frequency[1] = (uint8_t)(audio_requested_rate >> 8);
    frequency[2] = (uint8_t)(audio_requested_rate >> 16);
    return tud_audio_buffer_and_schedule_control_xfer(rhport, request,
                                                        frequency, sizeof(frequency));
}

bool tud_audio_set_req_entity_cb(uint8_t rhport,
                                 tusb_control_request_t const *request,
                                 uint8_t *buffer)
{
    uint8_t channel = (uint8_t)(request->wValue & 0xffu);
    uint8_t control = (uint8_t)(request->wValue >> 8);

    (void)rhport;
    if (control == AUDIO10_FU_CTRL_MUTE && channel < 3u) {
        audio_mute[channel] = buffer[0];
    }
    return true;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport,
                                 tusb_control_request_t const *request)
{
    uint8_t channel = (uint8_t)(request->wValue & 0xffu);
    uint8_t control = (uint8_t)(request->wValue >> 8);
    uint8_t value;

    if (control != AUDIO10_FU_CTRL_MUTE) {
        return false;
    }
    value = channel < 3u ? audio_mute[channel] : 0u;
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &value, 1);
}



int main(void)
{
    board_init();
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

    vreg_set_voltage(VREG_VOLTAGE_1_10);
    set_sys_clock_khz(125000, true);

    g_pio_offset = pio_add_program(pio0, &i2s_rx_program);
    g_tx_pio_offset = pio_add_program(pio0, &i2s_tx_program);
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
    tx_dma_channel = dma_claim_unused_channel(true);
    tx_dma_config = dma_channel_get_default_config(tx_dma_channel);
    channel_config_set_transfer_data_size(&tx_dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&tx_dma_config, true);
    channel_config_set_write_increment(&tx_dma_config, false);
    channel_config_set_dreq(&tx_dma_config, pio_get_dreq(pio0, 1, true));
    dma_channel_configure(tx_dma_channel, &tx_dma_config, &pio0->txf[1],
                          tx_audio_buffers[0], words_per_buffer, false);
    dma_channel_set_irq0_enabled(tx_dma_channel, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);

#ifdef DDC_FPGA_BOOT_FROM_STORED
#ifdef DDC_DEFAULT_FPGA_TX
    active_fpga_image = DDC_FPGA_IMAGE_TX;
#else
    active_fpga_image = DDC_FPGA_IMAGE_RX;
#endif
    fpga_ready = false;
    for (int retry = 0; retry < 5; retry++) {
        gpio_put(22, 0);
        sleep_ms(10);
        gpio_put(22, 1);
        sleep_ms(10);

        if (configure_stored_fpga(active_fpga_image)) {
            if (restore_fpga_runtime()) {
                fpga_ready = true;
                break;
            }
        }
        sleep_ms(50);
    }
    if (!fpga_ready) {
        active_fpga_image = DDC_FPGA_IMAGE_DFU;
    }
#else
    fpga_ready = false;
    active_fpga_image = DDC_FPGA_IMAGE_DFU;
#endif

    tusb_init();

#ifdef PICO_CYW43_SUPPORTED
    bool wifi_ok = false;
    if (cyw43_arch_init() == 0) {
        wifi_ok = true;
        cyw43_arch_enable_sta_mode();
        uint32_t auth = (DEFAULT_WIFI_PASSWORD[0] == '\0') ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK;
        const char *pass_param = (DEFAULT_WIFI_PASSWORD[0] == '\0') ? NULL : DEFAULT_WIFI_PASSWORD;
        cyw43_arch_wifi_connect_async(s_current_ssid, pass_param, auth);
        openhpsdr_init(on_hpsdr_freq_change, on_hpsdr_rate_change, on_hpsdr_gain_change);
    }
    uint32_t last_led_poll = 0;
    uint32_t last_reconnect_ms = to_ms_since_boot(get_absolute_time());
    uint32_t s_join_start_ms = 0;
#endif

    while (true) {
        s_main_loop_counter++;
        uint32_t t = time_us_32();
        tud_task();
        uint32_t d = time_us_32() - t; if (d > prof_tud) prof_tud = d;

        t = time_us_32();
        cdc_task();
        d = time_us_32() - t; if (d > prof_cdc) prof_cdc = d;

        t = time_us_32();
        handle_fpga_interrupt();
        d = time_us_32() - t; if (d > prof_fpga) prof_fpga = d;

        t = time_us_32();
        agc_task();
        d = time_us_32() - t; if (d > prof_agc) prof_agc = d;

        t = time_us_32();
        audio_task();
        d = time_us_32() - t; if (d > prof_audio) prof_audio = d;

        tx_task();

#ifdef PICO_CYW43_SUPPORTED
        t = time_us_32();
        if (wifi_ok) {
            openhpsdr_task();
            static bool s_last_hpsdr = false;
            bool hpsdr_now = openhpsdr_is_active();
            if (hpsdr_now != s_last_hpsdr) {
                s_last_hpsdr = hpsdr_now;
                if (i2s_running) {
                    i2s_stop();
                }
                if (hpsdr_now || capture_alt > 0) {
                    i2s_start();
                }
            } else {
                if (hpsdr_now && !i2s_running) {
                    i2s_start();
                } else if (!hpsdr_now && capture_alt == 0 && i2s_running) {
                    i2s_stop();
                }
            }
            uint32_t now_ms = to_ms_since_boot(get_absolute_time());
            bool active = openhpsdr_is_active();
            static bool s_was_active = false;
            if (active != s_was_active) {
                s_was_active = active;
                if (active) {
                    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
                }
            }

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
                        uint32_t auth = (DEFAULT_WIFI_PASSWORD[0] == '\0') ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK;
                        const char *pass_param = (DEFAULT_WIFI_PASSWORD[0] == '\0') ? NULL : DEFAULT_WIFI_PASSWORD;
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
        d = time_us_32() - t; if (d > prof_wifi) prof_wifi = d;
#endif
    }
}

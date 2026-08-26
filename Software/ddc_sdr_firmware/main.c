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

#if defined(PICO_CYW43_SUPPORTED) && (PICO_CYW43_SUPPORTED != 0)
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "openhpsdr.h"
#include "wifi_config.h"
#endif

#include "boards.h"
#include "agc_control.h"
#include "ddc_protocol.h"
#include "ice_cram.h"
#include "ice_fpga.h"
#include "ice_fpga_data.h"
#include "ice_spi.h"
#include "ice_usb.h"
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
#define DDC_MAX_WORDS_PER_BUFFER 192u
#define DDC_TX_BUFFER_COUNT 4u
#define DDC_AUDIO_CAPTURE_INTERFACE 3u
#define DDC_AUDIO_PLAYBACK_INTERFACE 4u
#define DDC_LINE_BUFFER_SIZE 128u

typedef enum {
    DDC_FPGA_IMAGE_RX,
    DDC_FPGA_IMAGE_TX,
    DDC_FPGA_IMAGE_DFU,
} ddc_fpga_image_t;

typedef struct {
    const uint8_t *data;
    size_t size;
} ddc_fpga_bitstream_t;

static uint32_t audio_buffer_a[DDC_MAX_WORDS_PER_BUFFER];
static uint32_t audio_buffer_b[DDC_MAX_WORDS_PER_BUFFER];
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

#if defined(PICO_CYW43_SUPPORTED) && (PICO_CYW43_SUPPORTED != 0)
static char s_wifi_ssid[64] = DEFAULT_WIFI_SSID_PRIMARY;
static char s_wifi_pass[64] = DEFAULT_WIFI_PASSWORD;
static struct tcp_pcb *s_tcp_server_pcb = NULL;
static struct tcp_pcb *s_active_tcp_client = NULL;
static volatile uint32_t s_pending_hpsdr_freq = 0;
static volatile uint32_t s_pending_hpsdr_rate = 0;
static volatile uint8_t s_pending_hpsdr_gain = 0xFF;
#endif

/* Forward declarations */
static bool fpga_write_command(uint8_t command, uint32_t value);
static bool fpga_set_frequency(uint32_t frequency_hz);
static bool apply_sample_rate(uint32_t rate);
static void pga_set_code(uint8_t pga_code);
static void cdc_write(const char *text);
static void handle_line(const char *line, uint8_t length, void (*reply_fn)(const char *));

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

static void fpga_interrupt_handler(uint gpio, uint32_t events)
{
    (void)gpio;
    (void)events;
    fpga_interrupt_pending = true;
}

static void runtime_spi_init(void)
{
    if (runtime_spi_ready) {
        return;
    }

    ice_spi_init_cs_pin(FPGA_DATA.bus.CS_cram, true);
    ice_spi_init(FPGA_DATA.bus);
    spi_set_baudrate(FPGA_DATA.bus.peripheral, DDC_RUNTIME_SPI_BAUD_HZ);
    runtime_spi_ready = true;
}

static void runtime_spi_deinit(void)
{
    if (!runtime_spi_ready) {
        return;
    }

    ice_spi_deinit();
    runtime_spi_ready = false;
}

static bool restore_fpga_runtime(void)
{
    runtime_spi_init();
    if (!fpga_set_frequency(last_frequency_hz)) {
        return false;
    }
    if (!fpga_write_command(DDC_FPGA_CMD_SET_SAMPLE_RATE, sample_rate)) {
        return false;
    }
    return true;
}

static bool configure_stored_fpga(ddc_fpga_image_t image)
{
    ddc_fpga_bitstream_t bitstream;

    if (!fpga_get_stored_image(image, &bitstream)) {
        return false;
    }

    runtime_spi_deinit();
    fpga_ready = false;
    ice_cram_open(FPGA_DATA);
    ice_cram_write(bitstream.data, bitstream.size);
    return ice_cram_close();
}

static bool prepare_fpga_update(void)
{
    if (update_prepared) {
        return true;
    }

    tr_set_receive(true);
    if (i2s_running) {
        pio_sm_set_enabled(pio0, 0, false);
    }
    if (tx_running) {
        pio_sm_set_enabled(pio0, 1, false);
    }
    runtime_spi_deinit();
    fpga_ready = false;
    update_prepared = true;
    return true;
}

static void complete_fpga_update(bool success)
{
    (void)success;
    update_prepared = false;
    fpga_ready = true;
    active_fpga_image = DDC_FPGA_IMAGE_DFU;

    if (!restore_fpga_runtime()) {
        fpga_ready = false;
        return;
    }

    if (i2s_running) {
        pio_sm_restart(pio0, 0);
        pio_sm_set_enabled(pio0, 0, true);
    }
    if (tx_running) {
        pio_sm_restart(pio0, 1);
        pio_sm_set_enabled(pio0, 1, true);
        tr_set_receive(false);
    }
}

static bool cancel_fpga_update(void)
{
    if (!update_prepared) {
        return true;
    }

    update_prepared = false;
    if (active_fpga_image != DDC_FPGA_IMAGE_DFU && configure_stored_fpga(active_fpga_image)) {
        complete_fpga_update(true);
        return true;
    }
    return false;
}

static void dma_handler(void)
{
    uint32_t status = dma_hw->ints0;

    if (status & (1u << dma_channel_a)) {
        dma_hw->ints0 = 1u << dma_channel_a;
        dma_channel_set_write_addr(dma_channel_a, audio_buffer_a, false);
        dma_channel_set_trans_count(dma_channel_a, words_per_buffer, false);
        ready_buffers |= 1u;
    }
    if (status & (1u << dma_channel_b)) {
        dma_hw->ints0 = 1u << dma_channel_b;
        dma_channel_set_write_addr(dma_channel_b, audio_buffer_b, false);
        dma_channel_set_trans_count(dma_channel_b, words_per_buffer, false);
        ready_buffers |= 2u;
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
    if (!i2s_running) {
        return;
    }

    dma_channel_abort(dma_channel_a);
    dma_channel_abort(dma_channel_b);
    pio_sm_set_enabled(pio0, 0, false);
    pio_sm_clear_fifos(pio0, 0);
    ready_buffers = 0;
    i2s_running = false;
}

static void i2s_start(void)
{
    if (i2s_running || !fpga_ready) {
        return;
    }

    i2s_configure();
    ready_buffers = 0;
    i2s_running = true;
    dma_channel_set_write_addr(dma_channel_a, audio_buffer_a, false);
    dma_channel_set_trans_count(dma_channel_a, words_per_buffer, false);
    dma_channel_set_write_addr(dma_channel_b, audio_buffer_b, false);
    dma_channel_set_trans_count(dma_channel_b, words_per_buffer, false);
    dma_channel_start(dma_channel_a);
    pio_sm_restart(pio0, 0);
    pio_sm_set_enabled(pio0, 0, true);
}

static void tx_configure(void)
{
    pio_gpio_init(pio0, DDC_I2S_TX_PIN);
    pio_sm_set_consecutive_pindirs(pio0, 1, DDC_I2S_TX_PIN, 1, true);

    pio_sm_config config = i2s_tx_program_get_default_config(g_tx_pio_offset);
    sm_config_set_out_pins(&config, DDC_I2S_TX_PIN, 1);
    sm_config_set_out_shift(&config, false, true, 32);
    sm_config_set_clkdiv(&config, 1.0f);
    pio_sm_init(pio0, 1, g_tx_pio_offset, &config);
}

static void tx_stop(void)
{
    if (!tx_running) {
        return;
    }

    if (tx_dma_channel >= 0) {
        dma_channel_abort(tx_dma_channel);
    }
    pio_sm_set_enabled(pio0, 1, false);
    pio_sm_clear_fifos(pio0, 1);
    tx_ready_mask = 0;
    tx_dma_active = false;
    tx_dma_silence = false;
    tx_running = false;
    tr_set_receive(true);
}

static void tx_start_next_dma(void)
{
    uint32_t saved_interrupts;
    uint32_t mask;
    const uint32_t *source;
    uint8_t buffer_index;

    saved_interrupts = save_and_disable_interrupts();
    if (tx_dma_active || !tx_running) {
        restore_interrupts(saved_interrupts);
        return;
    }

    mask = tx_ready_mask;
    if (mask != 0) {
        for (buffer_index = 0; buffer_index < DDC_TX_BUFFER_COUNT; buffer_index++) {
            if (mask & (1u << buffer_index)) {
                break;
            }
        }
        source = tx_audio_buffers[buffer_index];
        tx_dma_buffer = buffer_index;
        tx_dma_silence = false;
    } else {
        memset(tx_silence_buffer, 0, words_per_buffer * sizeof(uint32_t));
        source = tx_silence_buffer;
        tx_dma_silence = true;
    }

    tx_dma_active = true;
    dma_channel_configure(tx_dma_channel, &tx_dma_config,
                          &pio0->txf[1], source,
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
    uint8_t packet[DDC_MAX_WORDS_PER_BUFFER * 3u];
    uint16_t bytes_read;
    uint32_t words_read;
    uint32_t index;

    if (!tx_running || playback_alt == 0) {
        return;
    }

    tx_start_next_dma();

    while (tud_audio_available() >= 6u && tx_select_fill_buffer()) {
        bytes_read = tud_audio_read(packet, sizeof(packet));
        if (bytes_read == 0) {
            break;
        }

        words_read = bytes_read / 3u;
        for (index = 0; index < words_read && tx_fill_words < words_per_buffer; index++) {
            uint32_t word = ((uint32_t)packet[3u * index])
                          | ((uint32_t)packet[3u * index + 1u] << 8)
                          | ((uint32_t)packet[3u * index + 2u] << 16);
            tx_audio_buffers[tx_fill_buffer][tx_fill_words++] = word << 8;
        }

        if (tx_fill_words >= words_per_buffer) {
            uint32_t saved_interrupts = save_and_disable_interrupts();
            tx_ready_mask |= 1u << tx_fill_buffer;
            tx_fill_buffer = (uint8_t)((tx_fill_buffer + 1u) % DDC_TX_BUFFER_COUNT);
            tx_fill_words = 0;
            restore_interrupts(saved_interrupts);
            tx_start_next_dma();
        }
    }
}

static void pga_set_code(uint8_t pga_code)
{
    for (uint offset = 0; offset < DDC_PGA_GPIO_COUNT; offset++) {
        gpio_put(DDC_PGA_GPIO_BASE + offset, (pga_code >> offset) & 1u);
    }
}

static void handle_fpga_interrupt(void)
{
    if (!fpga_interrupt_pending) {
        return;
    }

    fpga_interrupt_pending = false;
    if (ddc_agc_on_otr(&agc_state)) {
        pga_set_code(agc_state.pga_code);
    }
    (void)fpga_write_command(DDC_FPGA_CMD_CLEAR_OTR, 1u);
}

static void agc_task(void)
{
    uint64_t now_ms = to_ms_since_boot(get_absolute_time());
    if (ddc_agc_tick(&agc_state, gpio_get(DDC_FPGA_INT_PIN), now_ms)) {
        pga_set_code(agc_state.pga_code);
    }
}

static bool fpga_write_command(uint8_t command, uint32_t value)
{
    uint8_t frame[DDC_FPGA_FRAME_HEADER_LEN + 4u];

    if (!runtime_spi_ready) {
        return false;
    }

    (void)ddc_make_u32_command(frame, command, value);
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
    if (frequency_hz > DDC_FPGA_MAX_FREQUENCY_HZ) {
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

static bool switch_stored_fpga(ddc_fpga_image_t image)
{
    bool was_running = i2s_running;
    bool was_tx_running = tx_running;
    bool success;

    if (image == active_fpga_image) {
        return true;
    }

    i2s_stop();
    tx_stop();
    tr_set_receive(true);

    success = configure_stored_fpga(image);
    if (success && restore_fpga_runtime()) {
        fpga_ready = true;
        if (was_running) {
            i2s_start();
        }
        if (was_tx_running) {
            tx_start();
        }
    } else {
        fpga_ready = false;
    }

    if (success && fpga_ready) {
        active_fpga_image = image;
    }
    return success && fpga_ready;
}

static void audio_task(void)
{
    uint32_t mask;
    const uint32_t *source;
    uint32_t words;
    uint32_t saved_interrupts;
    tu_fifo_t *fifo;
    static uint8_t packed[DDC_MAX_WORDS_PER_BUFFER * 3u];

    if (!i2s_running) {
        return;
    }

    saved_interrupts = save_and_disable_interrupts();
    mask = ready_buffers;
    if (mask != 0) {
        ready_buffers &= ~(mask & 1u ? 1u : 2u);
    }
    restore_interrupts(saved_interrupts);
    if (mask == 0) {
        return;
    }

    source = (mask & 1u) ? audio_buffer_a : audio_buffer_b;
    words = words_per_buffer;

#if defined(PICO_CYW43_SUPPORTED) && (PICO_CYW43_SUPPORTED != 0)
    // 1. Stream via OpenHPSDR Protocol 1 over Wi-Fi
    if (openhpsdr_is_active()) {
        openhpsdr_push_samples(source, words);
    }
#endif

    // 2. Stream via USB Audio Class 1.0 if USB host is listening
    if (tud_audio_mounted() && capture_alt > 0) {
        fifo = tud_audio_get_ep_in_ff();
        if (fifo != NULL && tu_fifo_remaining(fifo) >= words * 3u) {
            for (uint32_t index = 0; index < words; index++) {
                uint32_t word = source[index];
                packed[3u * index] = (uint8_t)(word >> 8);
                packed[3u * index + 1u] = (uint8_t)(word >> 16);
                packed[3u * index + 2u] = (uint8_t)(word >> 24);
            }
            tud_audio_write(packed, (uint16_t)(words * 3u));
        }
    }
}

#if defined(PICO_CYW43_SUPPORTED) && (PICO_CYW43_SUPPORTED != 0)
static void on_hpsdr_freq_change(uint32_t freq_hz) {
    s_pending_hpsdr_freq = freq_hz;
}

static void on_hpsdr_rate_change(uint32_t rate_hz) {
    s_pending_hpsdr_rate = rate_hz;
}

static void on_hpsdr_gain_change(uint8_t pga_code) {
    s_pending_hpsdr_gain = pga_code;
}

static void service_hpsdr_pending_tuning(void) {
    if (s_pending_hpsdr_rate != 0) {
        uint32_t r = s_pending_hpsdr_rate;
        s_pending_hpsdr_rate = 0;
        apply_sample_rate(r);
    }
    if (s_pending_hpsdr_freq != 0) {
        uint32_t f = s_pending_hpsdr_freq;
        s_pending_hpsdr_freq = 0;
        if (fpga_ready && fpga_set_frequency(f)) {
            last_frequency_hz = f;
            freq_cmd_count++;
        }
    }
    if (s_pending_hpsdr_gain != 0xFF) {
        uint8_t g = s_pending_hpsdr_gain;
        s_pending_hpsdr_gain = 0xFF;
        agc_state.pga_code = g;
        pga_set_code(g);
    }
}

static void tcp_write_str(const char *s) {
    if (s_active_tcp_client) {
        tcp_write(s_active_tcp_client, s, (u16_t)strlen(s), TCP_WRITE_FLAG_COPY);
        tcp_output(s_active_tcp_client);
    }
}

static err_t tcp_client_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (!p) {
        tcp_close(tpcb);
        s_active_tcp_client = NULL;
        return ERR_OK;
    }
    s_active_tcp_client = tpcb;
    char line_buf[DDC_LINE_BUFFER_SIZE];
    u16_t len = (p->len < DDC_LINE_BUFFER_SIZE - 1) ? p->len : DDC_LINE_BUFFER_SIZE - 1;
    pbuf_copy_partial(p, line_buf, len, 0);
    line_buf[len] = '\0';

    while (len > 0 && (line_buf[len-1] == '\r' || line_buf[len-1] == '\n')) {
        line_buf[--len] = '\0';
    }

    handle_line(line_buf, (uint8_t)len, tcp_write_str);
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *client_pcb, err_t err) {
    (void)arg; (void)err;
    s_active_tcp_client = client_pcb;
    tcp_recv(client_pcb, tcp_client_recv);
    tcp_write_str("SDR ready (WiFi TCP)\r\n");
    return ERR_OK;
}

static void start_tcp_control_server(void) {
    s_tcp_server_pcb = tcp_new();
    if (s_tcp_server_pcb) {
        tcp_bind(s_tcp_server_pcb, IP_ADDR_ANY, TCP_CONTROL_PORT);
        s_tcp_server_pcb = tcp_listen(s_tcp_server_pcb);
        tcp_accept(s_tcp_server_pcb, tcp_server_accept);
    }
}
#endif

static void cdc_write(const char *text)
{
    tud_cdc_write_str(text);
    tud_cdc_write_flush();
}

static void handle_line(const char *line, uint8_t length, void (*reply_fn)(const char *))
{
    char reply[128];

    if (length == 0) {
        return;
    }
    if (strcmp(line, "VER") == 0) {
        reply_fn("VER,DDC SDR 0.2\r\nOK\r\n");
        return;
    }
    if (strcmp(line, "XTAL") == 0) {
        reply_fn("XTAL,30720000\r\nOK\r\n");
        return;
    }
    if (strcmp(line, "MODE") == 0) {
        reply_fn("MODE,DDC\r\nOK\r\n");
        return;
    }
#if defined(PICO_CYW43_SUPPORTED) && (PICO_CYW43_SUPPORTED != 0)
    if (strcmp(line, "WIFI?") == 0 || strcmp(line, "WIFI") == 0) {
        int status = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
        const char *st_str = "DOWN";
        if (status == CYW43_LINK_JOIN) st_str = "JOINING";
        else if (status == CYW43_LINK_NOIP) st_str = "NO_IP";
        else if (status == CYW43_LINK_UP) st_str = "CONNECTED";
        else if (status == CYW43_LINK_FAIL) st_str = "AUTH_FAILED";
        else if (status == CYW43_LINK_NONET) st_str = "NO_NETWORK";
        else if (status == CYW43_LINK_BADAUTH) st_str = "BAD_AUTH";

        uint8_t *ip = (uint8_t*)&(cyw43_state.netif[CYW43_ITF_STA].ip_addr.addr);
        snprintf(reply, sizeof(reply), "WIFI,%s,IP:%u.%u.%u.%u,SSID:%s\r\nOK\r\n",
                 st_str, ip[0], ip[1], ip[2], ip[3], s_wifi_ssid);
        reply_fn(reply);
        return;
    }
    if (strncmp(line, "WIFI,", 5) == 0) {
        char buf[DDC_LINE_BUFFER_SIZE];
        strncpy(buf, line + 5, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *ssid = strtok(buf, ",");
        char *pass = strtok(NULL, ",");
        if (ssid) {
            strncpy(s_wifi_ssid, ssid, sizeof(s_wifi_ssid) - 1);
            s_wifi_ssid[sizeof(s_wifi_ssid) - 1] = '\0';
            if (pass) {
                strncpy(s_wifi_pass, pass, sizeof(s_wifi_pass) - 1);
                s_wifi_pass[sizeof(s_wifi_pass) - 1] = '\0';
            } else {
                s_wifi_pass[0] = '\0';
            }
            uint32_t auth = (s_wifi_pass[0] == '\0') ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK;
            const char *pass_param = (s_wifi_pass[0] == '\0') ? NULL : s_wifi_pass;
            cyw43_arch_wifi_connect_async(s_wifi_ssid, pass_param, auth);
            snprintf(reply, sizeof(reply), "WIFI,CONNECTING,%s\r\nOK\r\n", s_wifi_ssid);
            reply_fn(reply);
        } else {
            reply_fn("ERROR,invalid wifi format\r\n");
        }
        return;
    }
#endif
    if (strcmp(line, "FPGA,STATUS") == 0) {
        snprintf(reply, sizeof(reply), "FPGA,%s\r\nOK\r\n",
                 fpga_image_name(active_fpga_image));
        reply_fn(reply);
        return;
    }
    if (strcmp(line, "FPGA,LOAD,RX") == 0) {
        reply_fn(switch_stored_fpga(DDC_FPGA_IMAGE_RX)
                      ? "FPGA,RX\r\nOK\r\n"
                      : "ERROR,RX image unavailable\r\n");
        return;
    }
    if (strcmp(line, "FPGA,LOAD,TX") == 0) {
        reply_fn(switch_stored_fpga(DDC_FPGA_IMAGE_TX)
                      ? "FPGA,TX\r\nOK\r\n"
                      : "ERROR,TX image unavailable\r\n");
        return;
    }
    if (strcmp(line, "FREQ,") == 0 || strcmp(line, "FREQ") == 0) {
        snprintf(reply, sizeof(reply), "%lu\r\nOK\r\n",
                 (unsigned long)last_frequency_hz);
        reply_fn(reply);
        return;
    }
    if (strncmp(line, "FREQ,", 5) == 0) {
        uint32_t frequency_hz = (uint32_t)strtoul(line + 5, NULL, 10);
        if (fpga_ready && fpga_set_frequency(frequency_hz)) {
            last_frequency_hz = frequency_hz;
            freq_cmd_count++;
            snprintf(reply, sizeof(reply), "%lu\r\nOK\r\n",
                     (unsigned long)frequency_hz);
            reply_fn(reply);
        } else {
            reply_fn("ERROR,frequency rejected\r\n");
        }
        return;
    }
    if (strncmp(line, "RATE,", 5) == 0) {
        uint32_t rate = (uint32_t)strtoul(line + 5, NULL, 10);
        if (apply_sample_rate(rate)) {
            freq_cmd_count++;
            snprintf(reply, sizeof(reply), "RATE,%lu OK\r\n",
                     (unsigned long)rate);
            reply_fn(reply);
        } else {
            reply_fn("ERROR,sample rate rejected\r\n");
        }
        return;
    }
    if (strcmp(line, "DFU,PREPARE") == 0) {
        if (prepare_fpga_update()) {
            reply_fn("DFU,READY\r\nOK\r\n");
        } else {
            reply_fn("ERROR,DFU busy\r\n");
        }
        return;
    }
    if (strcmp(line, "DFU,CANCEL") == 0) {
        reply_fn(cancel_fpga_update() ? "DFU,CANCELLED\r\nOK\r\n"
                                       : "ERROR,FPGA unavailable\r\n");
        return;
    }
    if (strcmp(line, "DFU,STATUS") == 0) {
        reply_fn(update_prepared ? "DFU,READY\r\nOK\r\n"
                                   : (fpga_ready ? "DFU,RUNNING\r\nOK\r\n"
                                                 : "DFU,WAITING\r\nOK\r\n"));
        return;
    }
    if (strcmp(line, "DEBUG") == 0) {
        uint32_t bck_toggles = 0, ws_toggles = 0;
        bool last_bck = gpio_get(DDC_I2S_BCK_PIN), last_ws = gpio_get(DDC_I2S_WS_PIN);
        absolute_time_t end = make_timeout_time_ms(10);
        while (absolute_time_diff_us(get_absolute_time(), end) > 0) {
            bool cur_bck = gpio_get(DDC_I2S_BCK_PIN);
            bool cur_ws = gpio_get(DDC_I2S_WS_PIN);
            if (cur_bck != last_bck) { bck_toggles++; last_bck = cur_bck; }
            if (cur_ws != last_ws) { ws_toggles++; last_ws = cur_ws; }
        }
        snprintf(reply, sizeof(reply),
                 "DEBUG: ready=%d, cap_alt=%d, bck_10ms=%lu, ws_10ms=%lu, freq_cnt=%lu, last_freq=%lu, g14=%d, g15=%d, g16=%d\r\nOK\r\n",
                 fpga_ready, capture_alt, (unsigned long)bck_toggles, (unsigned long)ws_toggles,
                 (unsigned long)freq_cmd_count, (unsigned long)last_frequency_hz,
                 gpio_get(14), gpio_get(15), gpio_get(16));
        reply_fn(reply);
        return;
    }
    if (strcmp(line, "REF") == 0) {
        snprintf(reply, sizeof(reply), "REF,%d\r\nOK\r\n", gpio_get(DDC_REF_PIN));
        reply_fn(reply);
        return;
    }
    if (strncmp(line, "REF,", 4) == 0) {
        uint8_t ref_val = (uint8_t)strtoul(line + 4, NULL, 10);
        gpio_put(DDC_REF_PIN, ref_val ? 1u : 0u);
        snprintf(reply, sizeof(reply), "REF,%d\r\nOK\r\n", gpio_get(DDC_REF_PIN));
        reply_fn(reply);
        return;
    }
    if (strcmp(line, "PGA") == 0) {
        snprintf(reply, sizeof(reply), "PGA,%u\r\nOK\r\n", agc_state.pga_code);
        reply_fn(reply);
        return;
    }
    if (strncmp(line, "PGA,", 4) == 0) {
        uint8_t code = (uint8_t)strtoul(line + 4, NULL, 10);
        agc_state.pga_code = code;
        pga_set_code(code);
        snprintf(reply, sizeof(reply), "PGA,%u\r\nOK\r\n", code);
        reply_fn(reply);
        return;
    }
    if (strcmp(line, "BOOTSEL") == 0 || strcmp(line, "RESET,BOOTSEL") == 0) {
        reply_fn("REBOOTING_BOOTSEL\r\nOK\r\n");
        reset_usb_boot(0, 0);
        return;
    }
    if (strcmp(line, "HELP") == 0 || strcmp(line, "?") == 0) {
        reply_fn("Commands:\r\n"
                  "  VER          - Show firmware version\r\n"
                  "  MODE         - Show SDR mode (DDC)\r\n"
                  "  XTAL         - Show master clock (30.72 MHz)\r\n"
                  "  WIFI?        - Show Wi-Fi status & IP\r\n"
                  "  WIFI,SSID,PW - Connect to Wi-Fi AP\r\n"
                  "  FPGA,STATUS  - Show FPGA gateware status\r\n"
                  "  FREQ,<hz>    - Set NCO tuning frequency in Hz\r\n"
                  "  RATE,<hz>    - Set audio sample rate in Hz\r\n"
                  "  REF,<0|1>    - Set REF mux (0=SDR RF RX, 1=VNA)\r\n"
                  "  PGA,<0..15>  - Set digital step attenuator code\r\n"
                  "  DEBUG        - Show hardware toggle rates\r\n"
                  "  BOOTSEL      - Reboot to USB BOOTSEL mode\r\n"
                  "OK\r\n");
        return;
    }
    reply_fn("ERR\r\n");
}

static void cdc_task(void)
{
    while (tud_cdc_available()) {
        uint8_t byte;

        tud_cdc_read(&byte, 1);
        if (byte == 0x03) {
            line_length = 0;
            continue;
        }
        if (byte == 0x04) {
            line_length = 0;
            if (!ready_message_sent) {
                cdc_write("SDR ready\r\n");
                ready_message_sent = true;
            }
            continue;
        }
        if (byte == '\r' || byte == '\n') {
            if (line_length > 0) {
                line_buffer[line_length] = '\0';
                handle_line(line_buffer, line_length, cdc_write);
                line_length = 0;
            }
            continue;
        }
        if (byte == '\b' || byte == 0x7f) {
            if (line_length > 0) {
                line_length--;
            }
            continue;
        }
        if (byte >= 32 && byte <= 126) {
            if (line_length < sizeof(line_buffer) - 1u) {
                line_buffer[line_length++] = (char)byte;
            }
        }
    }
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    uint8_t interface_number = (uint8_t)(request->wIndex & 0xffu);
    uint8_t alt_setting = (uint8_t)(request->wValue & 0xffu);

    (void)rhport;
    if (interface_number == DDC_AUDIO_CAPTURE_INTERFACE) {
        capture_alt = alt_setting;
        if (capture_alt > 0) {
            apply_sample_rate(capture_alt == 2 ? 96000u : 48000u);
            i2s_start();
        } else {
            i2s_stop();
        }
    } else if (interface_number == DDC_AUDIO_PLAYBACK_INTERFACE) {
        playback_alt = alt_setting;
        if (playback_alt > 0) {
            apply_sample_rate(playback_alt == 2 ? 96000u : 48000u);
            tx_start();
        } else {
            tx_stop();
        }
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
    if ((uint8_t)(request->wValue >> 8) == AUDIO10_EP_CTRL_SAMPLING_FREQ) {
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

    if ((uint8_t)(request->wValue >> 8) != AUDIO10_EP_CTRL_SAMPLING_FREQ) {
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
    gpio_init(DDC_TR_PIN);
    gpio_set_dir(DDC_TR_PIN, GPIO_OUT);
    tr_set_receive(true);
    gpio_init(DDC_REF_PIN);
    gpio_set_dir(DDC_REF_PIN, GPIO_OUT);
    gpio_put(DDC_REF_PIN, 0);
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    set_sys_clock_khz(250000, true);

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
    dma_channel_configure(dma_channel_a, &config_a, audio_buffer_a,
                          &pio0->rxf[0], words_per_buffer, false);

    dma_channel_config config_b = dma_channel_get_default_config(dma_channel_b);
    channel_config_set_transfer_data_size(&config_b, DMA_SIZE_32);
    channel_config_set_read_increment(&config_b, false);
    channel_config_set_write_increment(&config_b, true);
    channel_config_set_dreq(&config_b, pio_get_dreq(pio0, 0, false));
    channel_config_set_chain_to(&config_b, dma_channel_a);
    dma_channel_configure(dma_channel_b, &config_b, audio_buffer_b,
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
    fpga_ready = configure_stored_fpga(active_fpga_image);
    if (fpga_ready && !restore_fpga_runtime()) {
        fpga_ready = false;
    }
    if (!fpga_ready) {
        active_fpga_image = DDC_FPGA_IMAGE_DFU;
    }
#else
    fpga_ready = false;
    active_fpga_image = DDC_FPGA_IMAGE_DFU;
#endif

    ice_usb_set_dfu_callbacks(prepare_fpga_update, complete_fpga_update);
    ice_usb_init();

#if defined(PICO_CYW43_SUPPORTED) && (PICO_CYW43_SUPPORTED != 0)
    // Initialize Wi-Fi on Pico W
    if (cyw43_arch_init() == 0) {
        cyw43_arch_enable_sta_mode();
        
        // Connect to Primary AP ("Frohro-2.4GHz") or Secondary AP ("Frohro-Shop-2.4GHz")
        int wifi_err = cyw43_arch_wifi_connect_timeout_ms(
            DEFAULT_WIFI_SSID_PRIMARY, NULL, CYW43_AUTH_OPEN, 5000);
        if (wifi_err != 0) {
            // Try secondary AP
            strncpy(s_wifi_ssid, DEFAULT_WIFI_SSID_SECONDARY, sizeof(s_wifi_ssid) - 1);
            cyw43_arch_wifi_connect_timeout_ms(
                DEFAULT_WIFI_SSID_SECONDARY, NULL, CYW43_AUTH_OPEN, 5000);
        }

        // Initialize OpenHPSDR Protocol 1 UDP Server on Port 1024
        openhpsdr_init(on_hpsdr_freq_change, on_hpsdr_rate_change, on_hpsdr_gain_change);

        // Start TCP Control Server on Port 5000
        start_tcp_control_server();

        // Turn on Pico W onboard LED when Wi-Fi is active
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    }
#endif

    // Start I2S streaming if FPGA is ready
    if (fpga_ready) {
        i2s_start();
    }

    while (true) {
        tud_task();
        cdc_task();
#if defined(PICO_CYW43_SUPPORTED) && (PICO_CYW43_SUPPORTED != 0)
        cyw43_arch_poll();
        openhpsdr_task();
        service_hpsdr_pending_tuning();
#endif
        handle_fpga_interrupt();
        agc_task();
        audio_task();
        tx_task();
    }
}

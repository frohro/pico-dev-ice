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
#define DDC_FPGA_INT_PIN 0
#define DDC_PGA_GPIO_BASE 8
#define DDC_PGA_GPIO_COUNT 4
#define DDC_DEFAULT_SAMPLE_RATE 48000u
#define DDC_RUNTIME_SPI_BAUD_HZ 10000000u
#define DDC_MAX_WORDS_PER_BUFFER 192u
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
static uint32_t last_frequency_hz;

static bool fpga_write_command(uint8_t command, uint32_t value);

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
    (void)gpio;
    (void)events;
    fpga_interrupt_pending = true;
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
    if (i2s_running || !fpga_ready || capture_alt == 0) {
        return;
    }

    i2s_configure();
    dma_channel_set_write_addr(dma_channel_a, audio_buffer_a, false);
    dma_channel_set_trans_count(dma_channel_a, words_per_buffer, false);
    dma_channel_set_write_addr(dma_channel_b, audio_buffer_b, false);
    dma_channel_set_trans_count(dma_channel_b, words_per_buffer, false);
    dma_hw->ints0 = (1u << dma_channel_a) | (1u << dma_channel_b);
    pio_sm_restart(pio0, 0);
    pio_sm_exec(pio0, 0, pio_encode_jmp(g_pio_offset));
    pio_sm_set_enabled(pio0, 0, true);
    ready_buffers = 0;
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_start(dma_channel_a);
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

static void audio_task(void)
{
    uint32_t mask;
    const uint32_t *source;
    uint32_t words;
    uint32_t saved_interrupts;
    tu_fifo_t *fifo;
    static uint8_t packed[DDC_MAX_WORDS_PER_BUFFER * 3u];

    if (!i2s_running || !tud_audio_mounted()) {
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
    fifo = tud_audio_get_ep_in_ff();
    if (fifo == NULL || tu_fifo_remaining(fifo) < words * 3u) {
        return;
    }

    for (uint32_t index = 0; index < words; index++) {
        uint32_t word = source[index] << 1;
        packed[3u * index] = (uint8_t)(word >> 8);
        packed[3u * index + 1u] = (uint8_t)(word >> 16);
        packed[3u * index + 2u] = (uint8_t)(word >> 24);
    }
    tud_audio_write(packed, (uint16_t)(words * 3u));
}

static void handle_line(const char *line, uint8_t length)
{
    char reply[96];

    if (length == 0) {
        return;
    }
    if (strcmp(line, "VER") == 0) {
        cdc_write("VER,DDC SDR 0.1\r\nOK\r\n");
        return;
    }
    if (strcmp(line, "XTAL") == 0) {
        cdc_write("XTAL,30720000\r\nOK\r\n");
        return;
    }
    if (strcmp(line, "MODE") == 0) {
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
    if (strcmp(line, "FREQ,") == 0) {
        snprintf(reply, sizeof(reply), "%lu\r\nOK\r\n",
                 (unsigned long)last_frequency_hz);
        cdc_write(reply);
        return;
    }
    if (strncmp(line, "FREQ,", 5) == 0) {
        uint32_t frequency_hz = (uint32_t)strtoul(line + 5, NULL, 10);
        if (fpga_ready && fpga_set_frequency(frequency_hz)) {
            last_frequency_hz = frequency_hz;
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
            audio_requested_rate = rate;
            snprintf(reply, sizeof(reply), "RATE,%lu\r\nOK\r\n",
                     (unsigned long)rate);
            cdc_write(reply);
        } else {
            cdc_write("ERROR,unsupported rate\r\n");
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
    cdc_write("ERR\r\n");
}

static void cdc_task(void)
{
    if (!tud_cdc_connected()) {
        line_length = 0;
        return;
    }

    while (tud_cdc_available()) {
        uint8_t byte;
        tud_cdc_read(&byte, 1);
        if (byte == 0x03) {
            line_length = 0;
        } else if (byte == 0x04) {
            line_length = 0;
            cdc_write("SDR ready\r\n");
        } else if (byte == '\r') {
            continue;
        } else if (byte == '\n') {
            line_buffer[line_length] = '\0';
            handle_line(line_buffer, line_length);
            line_length = 0;
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

    while (true) {
        tud_task();
        cdc_task();
        handle_fpga_interrupt();
        agc_task();
        audio_task();
        tx_task();
    }
}

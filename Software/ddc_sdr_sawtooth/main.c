/*
 * RP2040 firmware for the Dev-iCE DDC SDR.
 *
 * The FPGA is the I2S clock master. RP2040 PIO state machines handle the
 * bit-level I2S timing, while DMA moves whole 32-bit channel words between
 * PIO and RAM. TinyUSB exposes the samples as UAC1 capture/playback streams
 * and also provides a CDC serial command port.
 *
 * The main loop services USB, CDC commands, and audio playback. Capture is
 * advanced by the USB audio transfer callback; the PIO/DMA hardware continues
 * receiving I2S independently.
 *
 * FPGA SDR RX/TX names the RF mode or gateware image. USB capture is
 * device-to-host; USB playback is host-to-device. Pico I2S capture receives
 * samples from the FPGA, while Pico I2S playback sends samples to it.
 */
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

#include "boards.h"
#include "agc_control.h"
#include "audio_rate_ctrl.h"
#include "ddc_protocol.h"
#include "ice_cram.h"
#include "ice_fpga.h"
#include "ice_fpga_data.h"
#include "ice_spi.h"
#include "ice_usb.h"
#include "i2s_rx.pio.h"
#include "i2s_tx.pio.h"

/*
 * CMake defines DDC_HAS_STORED_RX when FPGA_RX_BITSTREAM_BIN is configured.
 * The build then runs tools/bin2header.py to generate fpga_bitstream_rx.h in
 * the build directory and adds that directory to this target's include path.
 * The generated header contains the binary as a const byte array.
 */
#ifdef DDC_HAS_STORED_RX
#include "fpga_bitstream_rx.h"
#endif
#ifdef DDC_HAS_STORED_TX
#include "fpga_bitstream_tx.h"
#endif

/* GPIO 14 carries I2S data from FPGA to Pico capture; GPIO 13 carries
 * playback data from Pico to FPGA. The FPGA supplies BCK and WS on 15/16. */
#define DDC_I2S_FPGA_TO_PICO_PIN 14
#define DDC_I2S_PICO_TO_FPGA_PIN 13
#define DDC_I2S_BCK_PIN 15
#define DDC_I2S_WS_PIN 16
#define DDC_TR_PIN 28
#define DDC_REF_PIN 26
#define DDC_FPGA_INT_PIN 0
#define DDC_PGA_GPIO_BASE 8
#define DDC_PGA_GPIO_COUNT 4
#define DDC_DEFAULT_SAMPLE_RATE 48000u
#define DDC_RUNTIME_SPI_BAUD_HZ 10000000u
/* One word is one 32-bit I2S channel sample. At 96 kHz there are 192 words
 * per millisecond; at the default 48 kHz there are 96. */
#define DDC_MAX_I2S_WORDS_PER_MILLISECOND 192u
#define DDC_PLAYBACK_BUFFER_COUNT 4u
/* Four one-millisecond capture slots form a four-millisecond circular ring. */
#define DDC_CAPTURE_RING_SLOTS 4u
#define DDC_AUDIO_CAPTURE_INTERFACE 3u
#define DDC_AUDIO_PLAYBACK_INTERFACE 4u
#define DDC_LINE_BUFFER_SIZE 80u

typedef enum {
    DDC_FPGA_IMAGE_SDR_RX,
    DDC_FPGA_IMAGE_SDR_TX,
    DDC_FPGA_IMAGE_DFU,
} ddc_fpga_image_t;

typedef struct {
    const uint8_t *data;
    size_t size;
} ddc_fpga_bitstream_t;

/* Capture state is independent of USB interface state: PRIMING sends silence
 * until the DMA ring has enough samples, then RUNNING consumes ring data. */
typedef enum {
    CAPTURE_IDLE,
    CAPTURE_PRIMING,
    CAPTURE_RUNNING,
} capture_state_t;

/* One flat ring of DDC_CAPTURE_RING_SLOTS slots of i2s_words_per_millisecond words (1 ms each).
 * A data DMA channel fills one slot and chains to a control channel that
 * loads the next slot address from capture_slot_table. */
static uint32_t capture_ring[DDC_CAPTURE_RING_SLOTS * DDC_MAX_I2S_WORDS_PER_MILLISECOND];
/* The RP2040 DMA read ring wraps on a power-of-two boundary, so this
 * four-entry address table must start on a 16-byte boundary. */
static uint32_t capture_slot_table[DDC_CAPTURE_RING_SLOTS] __attribute__((aligned(16)));
/* Incremented by the capture data-DMA completion interrupt, once per slot.
 * The DMA write address supplies the current position within the slot. */
static volatile uint32_t capture_slot_irqs;
static uint32_t capture_tail_words;
static capture_state_t capture_state = CAPTURE_IDLE;
/* Stream-health counters exposed by the AUDIO,STATS CDC command. */
static uint32_t capture_underruns;
static uint32_t capture_overruns;
static uint32_t capture_packets_short;
static uint32_t capture_packets_long;
static uint32_t capture_fifo_stale;
static uint32_t capture_min_occupancy = UINT32_MAX;
static uint32_t capture_max_occupancy;
/* Playback buffers are filled from USB OUT data and consumed by playback
 * I2S DMA. The ready mask and active-buffer fields are shared with the DMA IRQ. */
static uint32_t playback_audio_buffers[DDC_PLAYBACK_BUFFER_COUNT][DDC_MAX_I2S_WORDS_PER_MILLISECOND];
static uint32_t playback_silence_buffer[DDC_MAX_I2S_WORDS_PER_MILLISECOND];
static uint g_capture_pio_offset;
static uint g_playback_pio_offset;
static int capture_data_dma_channel = -1;
static int capture_ctrl_dma_channel = -1;
static int playback_dma_channel = -1;
static dma_channel_config_t playback_dma_config;
static volatile uint32_t i2s_words_per_millisecond = 96u;
static volatile uint32_t playback_ready_mask;
static volatile bool playback_dma_active;
static volatile bool playback_dma_silence;
static volatile uint8_t playback_dma_buffer;
static bool capture_i2s_running;
static bool playback_running;
static bool runtime_spi_ready;
static bool fpga_ready;
static bool update_prepared;
static ddc_fpga_image_t active_fpga_image = DDC_FPGA_IMAGE_DFU;
static uint32_t sample_rate = DDC_DEFAULT_SAMPLE_RATE;
static volatile bool fpga_interrupt_pending;
static ddc_agc_state_t agc_state;

/* USB audio alternate settings: 0 is stopped, 1 is 48 kHz, 2 is 96 kHz. */
static uint8_t capture_alt;
static uint8_t playback_alt;
static uint32_t audio_requested_rate = DDC_DEFAULT_SAMPLE_RATE;
static uint8_t audio_mute[3];
static uint8_t playback_fill_buffer;
static uint32_t playback_fill_words;

/* CDC commands arrive a byte at a time; these variables hold the unfinished
 * line and the last values returned by diagnostic commands. */
static char line_buffer[DDC_LINE_BUFFER_SIZE];
static uint8_t line_length;
static bool ready_message_sent;
static uint32_t last_frequency_hz;
static uint32_t freq_cmd_count;

static bool fpga_write_command(uint8_t command, uint32_t value);

/* Return the generated bitstream array for the requested image. The
 * DDC_HAS_STORED_* macros are build options, not Pico SDK definitions. */
static bool fpga_get_stored_image(ddc_fpga_image_t image,
                                  ddc_fpga_bitstream_t *bitstream)
{
    if (bitstream == NULL) {
        return false;
    }

    switch (image) {
    case DDC_FPGA_IMAGE_SDR_RX:
#ifdef DDC_HAS_STORED_RX
        bitstream->data = ddc_fpga_rx_bitstream;
        bitstream->size = DDC_FPGA_RX_BITSTREAM_SIZE;
        return true;
#else
        return false;
#endif
    case DDC_FPGA_IMAGE_SDR_TX:
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
    /* Preserve the CDC protocol's short names for FPGA SDR RF images. */
    switch (image) {
    case DDC_FPGA_IMAGE_SDR_RX:
        return "RX";
    case DDC_FPGA_IMAGE_SDR_TX:
        return "TX";
    default:
        return "DFU";
    }
}

/* Control the board's TR switch: receive selects the SDR input path. */
static void set_rf_path_receive(bool receive)
{
    gpio_put(DDC_TR_PIN, receive ? 1u : 0u);
}

/* Drive the four parallel GPIO bits that select the RF attenuator setting. */
static void pga_set_code(uint8_t code)
{
    code &= DDC_PGA_MAX_CODE;
    for (uint gpio = DDC_PGA_GPIO_BASE;
         gpio < DDC_PGA_GPIO_BASE + DDC_PGA_GPIO_COUNT;
         gpio++) {
        gpio_put(gpio, (code >> (gpio - DDC_PGA_GPIO_BASE)) & 1u);
    }
}

/* GPIO callbacks run in interrupt context. Defer SPI/AGC work to the main
 * loop instead of doing it inside the short hardware interrupt. */
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

/* Periodic AGC service. Currently disabled in the main loop below. */
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

/* Queue a string for the USB CDC endpoint and ask TinyUSB to transmit it. */
static void cdc_write(const char *text)
{
    tud_cdc_write_str(text);
    tud_cdc_write_flush();
}

/* DMA IRQ 0 is shared by capture and playback. Capture only counts completed
 * slots; the control DMA channel chains the next slot without CPU help.
 * Playback completion releases the finished buffer for reuse. */
static void dma_handler(void)
{
    uint32_t status = dma_hw->ints0;

    if (capture_data_dma_channel >= 0 && (status & (1u << capture_data_dma_channel))) {
        dma_hw->ints0 = 1u << capture_data_dma_channel;
        capture_slot_irqs++;
    }
    if (playback_dma_channel >= 0 && (status & (1u << playback_dma_channel))) {
        uint32_t buffer_bit;

        dma_hw->ints0 = 1u << playback_dma_channel;
        buffer_bit = playback_dma_silence ? 0u : 1u << playback_dma_buffer;
        playback_dma_active = false;
        if (buffer_bit != 0u) {
            playback_ready_mask &= ~buffer_bit;
        }
        playback_dma_silence = false;
    }
}

/* Configure PIO state machine 0 as the FPGA-to-Pico I2S capture input. The
 * PIO program waits on the FPGA's BCK/WS pins and pushes each channel word
 * into its RX FIFO.
 * Joining the two PIO FIFOs gives DMA more time to service the stream. */
static void capture_i2s_configure(void)
{
    pio_gpio_init(pio0, DDC_I2S_FPGA_TO_PICO_PIN);
    pio_gpio_init(pio0, DDC_I2S_BCK_PIN);
    pio_gpio_init(pio0, DDC_I2S_WS_PIN);
    gpio_pull_up(DDC_I2S_FPGA_TO_PICO_PIN);
    gpio_pull_up(DDC_I2S_BCK_PIN);
    gpio_pull_up(DDC_I2S_WS_PIN);
    pio_sm_set_consecutive_pindirs(pio0, 0, DDC_I2S_FPGA_TO_PICO_PIN, 3, false);

    pio_sm_config config = i2s_rx_program_get_default_config(g_capture_pio_offset);
    sm_config_set_in_pins(&config, DDC_I2S_FPGA_TO_PICO_PIN);
    sm_config_set_in_shift(&config, false, true, 32);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&config, 1.0f);
    pio_sm_init(pio0, 0, g_capture_pio_offset, &config);
}

static void capture_i2s_stop(void)
{
    uint32_t saved_interrupts;

    if (capture_data_dma_channel < 0 || capture_ctrl_dma_channel < 0) {
        return;
    }

    /* Abort the control channel first so it cannot re-trigger the data
     * channel, then the data channel, then the control channel again in case
     * the data channel chained to it meanwhile. */
    dma_channel_abort(capture_ctrl_dma_channel);
    dma_channel_abort(capture_data_dma_channel);
    dma_channel_abort(capture_ctrl_dma_channel);
    dma_channel_set_irq0_enabled(capture_data_dma_channel, false);
    dma_hw->ints0 = 1u << capture_data_dma_channel;
    pio_sm_set_enabled(pio0, 0, false);
    pio_sm_clear_fifos(pio0, 0);
    saved_interrupts = save_and_disable_interrupts();
    capture_state = CAPTURE_IDLE;
    capture_i2s_running = false;
    restore_interrupts(saved_interrupts);
    if (!playback_running) {
        irq_set_enabled(DMA_IRQ_0, false);
    }
}

/* Start continuous FPGA-to-RAM capture. One data-DMA transfer fills one
 * 1-ms slot. Its chain starts the control channel, which writes the next slot
 * address to the data channel's trigger alias. The four slot addresses wrap
 * via the control channel's 16-byte read ring. */
static void capture_i2s_start(void)
{
    uint32_t slot_words;
    uint32_t saved_interrupts;
    dma_channel_config data_config;
    dma_channel_config ctrl_config;

    if (capture_i2s_running || !fpga_ready || capture_alt == 0) {
        return;
    }

    capture_i2s_configure();
    slot_words = i2s_words_per_millisecond;
    for (uint32_t slot = 0; slot < DDC_CAPTURE_RING_SLOTS; slot++) {
        capture_slot_table[slot] = (uint32_t)&capture_ring[slot * slot_words];
    }

    /* The capture channel reads the same PIO RX FIFO register every time, but
     * increments its RAM destination. The PIO DREQ pauses DMA until a word is
     * available. Completing a slot chains to the address-loader channel. */
    data_config = dma_channel_get_default_config(capture_data_dma_channel);
    channel_config_set_transfer_data_size(&data_config, DMA_SIZE_32);
    channel_config_set_read_increment(&data_config, false);
    channel_config_set_write_increment(&data_config, true);
    channel_config_set_dreq(&data_config, pio_get_dreq(pio0, 0, false));
    channel_config_set_chain_to(&data_config, capture_ctrl_dma_channel);
    dma_channel_configure(capture_data_dma_channel, &data_config, capture_ring,
                          &pio0->rxf[0], slot_words, false);

    /* Each slot completion writes the next slot address into the data
     * channel's trigger alias; the RP2040 reloads the live transfer count
     * from the RELOAD value written above, so only the address is needed. */
    ctrl_config = dma_channel_get_default_config(capture_ctrl_dma_channel);
    channel_config_set_transfer_data_size(&ctrl_config, DMA_SIZE_32);
    channel_config_set_read_increment(&ctrl_config, true);
    channel_config_set_write_increment(&ctrl_config, false);
    channel_config_set_ring(&ctrl_config, false, 4);
    dma_channel_configure(capture_ctrl_dma_channel, &ctrl_config,
                          &dma_hw->ch[capture_data_dma_channel].al2_write_addr_trig,
                          &capture_slot_table[1], 1, false);

    saved_interrupts = save_and_disable_interrupts();
    capture_slot_irqs = 0;
    capture_tail_words = 0;
    capture_state = CAPTURE_PRIMING;
    restore_interrupts(saved_interrupts);
    dma_hw->ints0 = 1u << capture_data_dma_channel;
    dma_channel_set_irq0_enabled(capture_data_dma_channel, true);
    pio_sm_restart(pio0, 0);
    pio_sm_exec(pio0, 0, pio_encode_jmp(g_capture_pio_offset));
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_start(capture_data_dma_channel);
    pio_sm_set_enabled(pio0, 0, true);
    capture_i2s_running = true;
}

/* Configure PIO state machine 1 to shift playback words out to the FPGA.
 * The FPGA still provides BCK and WS; the PIO program synchronizes to them. */
static void playback_i2s_configure(void)
{
    pio_gpio_init(pio0, DDC_I2S_PICO_TO_FPGA_PIN);
    pio_gpio_init(pio0, DDC_I2S_FPGA_TO_PICO_PIN);
    pio_gpio_init(pio0, DDC_I2S_BCK_PIN);
    pio_gpio_init(pio0, DDC_I2S_WS_PIN);
    gpio_pull_down(DDC_I2S_BCK_PIN);
    gpio_pull_down(DDC_I2S_WS_PIN);
    pio_sm_set_consecutive_pindirs(pio0, 1, DDC_I2S_FPGA_TO_PICO_PIN, 3, false);
    pio_sm_set_consecutive_pindirs(pio0, 1, DDC_I2S_PICO_TO_FPGA_PIN, 1, true);

    pio_sm_config config = i2s_tx_program_get_default_config(g_playback_pio_offset);
    sm_config_set_in_pins(&config, DDC_I2S_FPGA_TO_PICO_PIN);
    sm_config_set_out_pins(&config, DDC_I2S_PICO_TO_FPGA_PIN, 1);
    sm_config_set_out_shift(&config, false, true, 32);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&config, 1.0f);
    pio_sm_init(pio0, 1, g_playback_pio_offset, &config);
}

/* Stop playback, discard queued buffers, and return the RF path to receive. */
static void playback_i2s_stop(void)
{
    uint32_t saved_interrupts = save_and_disable_interrupts();

    if (playback_dma_channel >= 0) {
        dma_channel_abort(playback_dma_channel);
        dma_hw->ints0 = 1u << playback_dma_channel;
    }
    pio_sm_set_enabled(pio0, 1, false);
    pio_sm_clear_fifos(pio0, 1);
    playback_ready_mask = 0;
    playback_dma_active = false;
    playback_dma_silence = false;
    playback_fill_buffer = 0;
    playback_fill_words = 0;
    playback_running = false;
    set_rf_path_receive(true);
    if (!capture_i2s_running) {
        irq_set_enabled(DMA_IRQ_0, false);
    }
    restore_interrupts(saved_interrupts);
}

/* Start DMA for the next complete playback buffer. If the USB host has not
 * supplied one yet, transmit a zero-filled buffer to keep I2S clocks flowing. */
static void playback_start_next_dma(void)
{
    uint32_t ready;
    uint32_t saved_interrupts;
    uint8_t index;

    saved_interrupts = save_and_disable_interrupts();
    if (!playback_running || playback_dma_active || playback_dma_channel < 0) {
        restore_interrupts(saved_interrupts);
        return;
    }

    ready = playback_ready_mask;
    for (index = 0; index < DDC_PLAYBACK_BUFFER_COUNT; index++) {
        if (ready & (1u << index)) {
            playback_dma_buffer = index;
            playback_dma_silence = false;
            playback_dma_active = true;
            dma_channel_configure(playback_dma_channel, &playback_dma_config,
                                  &pio0->txf[1],
                                  playback_audio_buffers[index],
                                  i2s_words_per_millisecond,
                                  true);
            restore_interrupts(saved_interrupts);
            return;
        }
    }

    playback_dma_silence = true;
    playback_dma_active = true;
    dma_channel_configure(playback_dma_channel, &playback_dma_config,
                          &pio0->txf[1], playback_silence_buffer,
                          i2s_words_per_millisecond, true);
    restore_interrupts(saved_interrupts);
}

static void playback_i2s_start(void)
{
    if (playback_running || !fpga_ready || playback_alt == 0) {
        return;
    }

    playback_i2s_configure();
    playback_running = true;
    irq_set_enabled(DMA_IRQ_0, true);
    playback_start_next_dma();
    pio_sm_restart(pio0, 1);
    pio_sm_set_enabled(pio0, 1, true);
    set_rf_path_receive(false);
}

/* Reserve an unused playback buffer for the USB-to-I2S task. The short
 * interrupt-disabled section prevents the DMA IRQ from freeing/reusing it
 * while the main loop checks the ready/active masks. */
static bool playback_select_fill_buffer(void)
{
    uint32_t occupied;
    uint32_t saved_interrupts;
    uint8_t offset;

    if (playback_fill_words != 0) {
        return true;
    }

    saved_interrupts = save_and_disable_interrupts();
    occupied = playback_ready_mask;
    if (playback_dma_active && !playback_dma_silence) {
        occupied |= 1u << playback_dma_buffer;
    }

    for (offset = 0; offset < DDC_PLAYBACK_BUFFER_COUNT; offset++) {
        uint8_t index = (uint8_t)((playback_fill_buffer + offset) % DDC_PLAYBACK_BUFFER_COUNT);
        if ((occupied & (1u << index)) == 0) {
            playback_fill_buffer = index;
            restore_interrupts(saved_interrupts);
            return true;
        }
    }
    restore_interrupts(saved_interrupts);
    return false;
}

/* Move complete S24_3LE USB frames into 32-bit I2S words. The 24 sample bits
 * occupy the most significant bits of each I2S word, matching the FPGA data
 * format. A finished RAM buffer is handed to the playback DMA path. */
static void usb_playback_task(void)
{
    uint8_t packet[582];

    if (!playback_running || !tud_audio_mounted()) {
        return;
    }

    while (tud_audio_available() >= 6u && playback_select_fill_buffer()) {
        uint32_t available_frames = tud_audio_available() / 6u;
        uint32_t capacity_frames = (i2s_words_per_millisecond - playback_fill_words) / 2u;
        uint32_t frames = available_frames < capacity_frames
                        ? available_frames : capacity_frames;
        uint16_t bytes;
        uint16_t received;

        if (frames == 0) {
            playback_ready_mask |= 1u << playback_fill_buffer;
            playback_fill_words = 0;
            playback_fill_buffer = (uint8_t)((playback_fill_buffer + 1u) % DDC_PLAYBACK_BUFFER_COUNT);
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
            playback_audio_buffers[playback_fill_buffer][playback_fill_words++] = left << 8;
            playback_audio_buffers[playback_fill_buffer][playback_fill_words++] = right << 8;
        }

        if (playback_fill_words == i2s_words_per_millisecond) {
            uint32_t saved_interrupts = save_and_disable_interrupts();
            playback_ready_mask |= 1u << playback_fill_buffer;
            restore_interrupts(saved_interrupts);
            playback_fill_words = 0;
            playback_fill_buffer = (uint8_t)((playback_fill_buffer + 1u) % DDC_PLAYBACK_BUFFER_COUNT);
        }
    }

    playback_start_next_dma();
}

/* Release the runtime SPI bus used for FPGA commands. This is separate from
 * the CRAM programming path used when loading a complete bitstream. */
static void runtime_spi_stop(void)
{
    if (!runtime_spi_ready) {
        return;
    }

    ice_spi_chip_deselect(FPGA_DATA.bus.CS_cram);
    ice_spi_deinit();
    runtime_spi_ready = false;
}

/* Initialize the pico-ice-sdk SPI bus described by FPGA_DATA. The project
 * sends runtime commands over this bus after the FPGA has been configured. */
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

/* Encode and send one DDC protocol command. The SPI helpers here come from
 * pico-ice-sdk; ddc_make_u32_command() defines this project's wire format. */
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

/* The FPGA uses this command to change its generated I2S sample rate. */
static bool fpga_set_sample_rate(uint32_t rate)
{
    return fpga_write_command(DDC_FPGA_CMD_SET_SAMPLE_RATE, rate);
}

/* Convert hertz to the FPGA's NCO tuning word, then send it over SPI. */
static bool fpga_set_frequency(uint32_t frequency_hz)
{
    if (frequency_hz == 0 || frequency_hz > DDC_FPGA_MAX_FREQUENCY_HZ) {
        return false;
    }
    return fpga_write_command(DDC_FPGA_CMD_SET_FREQUENCY,
                              ddc_frequency_to_fcw(frequency_hz));
}

/* Initialize the attenuator GPIOs and reset the AGC state machine. */
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

/* Configure the FPGA interrupt pin. The callback only sets a pending flag;
 * handle_fpga_interrupt() performs the slower SPI work later. */
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

/* Program the FPGA's configuration RAM (CRAM) from a byte array. ice_fpga_*
 * and ice_cram_* are pico-ice-sdk helpers; false means opening, writing, or
 * closing the programming interface failed. */
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

/* Load one image embedded by CMake. This avoids a separate file transfer at
 * boot; DFU remains available for replacing or reloading the FPGA image. */
static bool configure_stored_fpga(ddc_fpga_image_t image)
{
    ddc_fpga_bitstream_t bitstream;

    if (!fpga_get_stored_image(image, &bitstream)) {
        return false;
    }
    return configure_fpga_bitstream(bitstream.data, bitstream.size);
}

/* Reinitialize runtime SPI after FPGA programming, mark the FPGA usable, and
 * restore whichever I2S streams are currently selected by USB. */
static bool restore_fpga_runtime(void)
{
    if (!fpga_runtime_init()) {
        return false;
    }

    fpga_ready = true;
    fpga_set_sample_rate(sample_rate);
    capture_i2s_start();
    playback_i2s_start();
    return true;
}

/* Apply a supported USB-selected rate to the FPGA and update DMA slot sizes.
 * Active streams are stopped before the rate change and restarted afterward
 * so their PIO/DMA transfer counts match the new rate. */
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

    was_running = capture_i2s_running;
    was_tx_running = playback_running;
    if (was_running) {
        capture_i2s_stop();
    }
    if (was_tx_running) {
        playback_i2s_stop();
    }
    if (!fpga_set_sample_rate(rate)) {
        if (was_running) {
            capture_i2s_start();
        }
        if (was_tx_running) {
            playback_i2s_start();
        }
        return false;
    }

    sample_rate = rate;
    i2s_words_per_millisecond = rate == 96000u ? 192u : 96u;
    if (was_running) {
        capture_i2s_start();
    }
    if (was_tx_running) {
        playback_i2s_start();
    }
    return true;
}

/* pico-ice-sdk calls this before DFU changes the FPGA image. Stop all users of
 * the FPGA and SPI bus while its configuration is being replaced. */
static bool prepare_fpga_update(void)
{
    if (update_prepared) {
        return true;
    }

    capture_i2s_stop();
    playback_i2s_stop();
    runtime_spi_stop();
    fpga_ready = false;
    update_prepared = true;
    return true;
}

/* DFU completion callback: restart runtime access after an update attempt. */
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

/* Resume the current FPGA image if a prepared DFU operation is cancelled. */
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

/* CDC helper for switching to one of the bitstreams embedded in the firmware. */
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

/* Return the number of words DMA has written since capture started.
 *
 * A DMA address alone wraps every time the ring wraps, so capture_slot_irqs counts
 * completed slots while the hardware write_addr identifies the current word.
 * The second IRQ-counter read detects a slot interrupt arriving during this
 * snapshot; retrying avoids combining an address from one lap with a count
 * from another. ring_pos is the corresponding physical word index in capture_ring.
 */
static uint32_t capture_head_words(uint32_t *ring_pos)
{
    uint32_t slots;
    uint32_t slots_again;
    uint32_t address;
    uint32_t offset;
    uint32_t slot_words = i2s_words_per_millisecond;
    uint32_t ring_words = DDC_CAPTURE_RING_SLOTS * slot_words;

    do {
        slots = capture_slot_irqs;
        address = dma_hw->ch[capture_data_dma_channel].write_addr;
        slots_again = capture_slot_irqs;
    } while (slots != slots_again);

    offset = (address - (uint32_t)capture_ring) >> 2;
    *ring_pos = offset % ring_words;
    return slots * slot_words +
           (offset + ring_words - (slots % DDC_CAPTURE_RING_SLOTS) * slot_words) %
               ring_words;
}

static void capture_note_occupancy(uint32_t frames)
{
    if (frames < capture_min_occupancy) {
        capture_min_occupancy = frames;
    }
    if (frames > capture_max_occupancy) {
        capture_max_occupancy = frames;
    }
}

/* Prepare one USB capture packet from the FPGA-to-Pico I2S ring.
 *
 * TinyUSB sends the bytes currently in its audio IN FIFO. This function
 * therefore writes exactly one packet (47/48/49 stereo frames at 48 kHz),
 * which TinyUSB will consume on the next endpoint transfer. Samples arrive
 * from PIO as 32-bit words with the 24-bit value in bits 31..8; the loop below
 * strips the padding and writes the three sample bytes in little-endian order.
 */
static void capture_fill_usb_in_packet(void)
{
    /* At 96 kHz the largest packet is 97 frames; each stereo frame is 6 bytes. */
    static uint8_t packed[(DDC_MAX_I2S_WORDS_PER_MILLISECOND / 2u + 1u) * 6u];
    uint32_t nominal = i2s_words_per_millisecond / 2u;
    uint32_t target = audio_rate_target_frames(nominal);
    uint32_t frames = nominal;
    bool silence = true;
    tu_fifo_t *fifo;

    /* This FIFO is TinyUSB's software queue for endpoint 0x81. If an earlier
     * packet was not fully consumed, discard it rather than combine two
     * independently sized packets into one USB frame. */
    fifo = tud_audio_get_ep_in_ff();
    if (fifo == NULL) {
        return;
    }
    if (tu_fifo_count(fifo) != 0) {
        tu_fifo_clear(fifo);
        capture_fifo_stale++;
    }

    if (capture_i2s_running && capture_state != CAPTURE_IDLE) {
        uint32_t ring_pos;
        uint32_t head = capture_head_words(&ring_pos) & ~1u;
        /* Convert word counts to stereo-frame counts; both channel words must
         * be present before a frame can be sent. */
        uint32_t occupancy = (head - capture_tail_words) >> 1;
        uint32_t ring_words = DDC_CAPTURE_RING_SLOTS * i2s_words_per_millisecond;

        ring_pos &= ~1u;
        /* Begin at half-full. If the consumer lagged badly, drop the oldest
         * buffered audio and restore the target while preserving channel
         * alignment. */
        if (capture_state == CAPTURE_PRIMING && occupancy >= target) {
            capture_tail_words = head - 2u * target;
            occupancy = target;
            capture_state = CAPTURE_RUNNING;
        } else if (capture_state == CAPTURE_RUNNING && occupancy > 3u * nominal) {
            capture_tail_words = head - 2u * target;
            occupancy = target;
            capture_overruns++;
        }

        if (capture_state == CAPTURE_RUNNING) {
            frames = audio_rate_pick(occupancy, nominal);
            if (occupancy < frames) {
                /* Do not read incomplete DMA data. Send silence and wait for
                 * the ring to refill to the priming level. */
                capture_underruns++;
                capture_state = CAPTURE_PRIMING;
                frames = nominal;
            } else {
                /* tail is an absolute word count; subtracting it from head
                 * gives the unread region's length. Apply the same distance
                 * to the physical ring position to find its oldest frame. */
                uint32_t position =
                    (ring_pos + ring_words - (head - capture_tail_words) % ring_words) %
                    ring_words;

                capture_note_occupancy(occupancy);
                if (frames > nominal) {
                    capture_packets_long++;
                } else if (frames < nominal) {
                    capture_packets_short++;
                }
                for (uint32_t index = 0; index < frames * 2u; index++) {
                    uint32_t word = capture_ring[position];

                    packed[3u * index] = (uint8_t)(word >> 8);
                    packed[3u * index + 1u] = (uint8_t)(word >> 16);
                    packed[3u * index + 2u] = (uint8_t)(word >> 24);
                    if (++position == ring_words) {
                        position = 0;
                    }
                }
                capture_tail_words += frames * 2u;
                silence = false;
            }
        }
    }

    if (silence) {
        memset(packed, 0, frames * 6u);
    }
    tud_audio_write(packed, (uint16_t)(frames * 6u));
}

bool tud_audio_tx_done_isr(uint8_t rhport, uint16_t n_bytes_sent,
                           uint8_t func_id, uint8_t ep_in,
                           uint8_t cur_alt_setting)
{
    /* TinyUSB's "tx" here means sending USB IN data to the host: this is
     * the USB capture stream, not FPGA SDR transmit or USB playback. */
    /* TinyUSB calls this from its isochronous IN completion path. It has
     * already queued the next packet, so filling now prepares the following
     * frame. The first packet is primed in tud_audio_set_itf_cb() below. */
    (void)rhport;
    (void)n_bytes_sent;
    (void)func_id;
    (void)ep_in;
    (void)cur_alt_setting;
    capture_fill_usb_in_packet();
    return true;
}

/* TinyUSB schedules the first IN transfer before invoking set_itf_cb. Fill
 * the FIFO here so that the first transfer completion can schedule audio
 * instead of a second zero-length packet. */
bool tud_audio_set_itf_cb(uint8_t rhport,
                          tusb_control_request_t const *request)
{
    static const uint32_t rates[] = {0u, 48000u, 96000u};
    /* UAC Set Interface selects alt 0 (off), 1 (48 kHz), or 2 (96 kHz). */
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
            capture_i2s_stop();
            return true;
        }
        audio_requested_rate = rates[alt];
        if (!apply_sample_rate(audio_requested_rate)) {
            capture_alt = 0;
            capture_i2s_stop();
            return false;
        }
        capture_i2s_start();
        /* TinyUSB queues an initial zero-length transfer before this callback.
         * Preload the FIFO so its completion schedules an audio packet rather
         * than another empty packet. */
        capture_fill_usb_in_packet();
    } else {
        playback_alt = alt;
        if (alt == 0u) {
            playback_i2s_stop();
            return true;
        }
        audio_requested_rate = rates[alt];
        if (!apply_sample_rate(audio_requested_rate)) {
            playback_alt = 0;
            playback_i2s_stop();
            return false;
        }
        playback_i2s_start();
    }
    return true;
}

/* Dispatch one complete CDC command line. Replies use cdc_write(), which
 * queues text on TinyUSB's CDC interface. */
static void handle_line(const char *line, uint8_t length)
{
    char reply[160];

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
    if (strcmp(line, "AUDIO,STATS") == 0) {
        snprintf(reply, sizeof(reply),
                 "AUDIO,under=%lu,over=%lu,short=%lu,long=%lu,stale=%lu,"
                 "occ=%lu..%lu\r\nOK\r\n",
                 (unsigned long)capture_underruns, (unsigned long)capture_overruns,
                 (unsigned long)capture_packets_short,
                 (unsigned long)capture_packets_long,
                 (unsigned long)capture_fifo_stale,
                 (unsigned long)(capture_min_occupancy == UINT32_MAX
                                     ? 0u : capture_min_occupancy),
                 (unsigned long)capture_max_occupancy);
        cdc_write(reply);
        return;
    }
    if (strcmp(line, "FPGA,LOAD,RX") == 0) {
        cdc_write(switch_stored_fpga(DDC_FPGA_IMAGE_SDR_RX)
                      ? "FPGA,RX\r\nOK\r\n"
                      : "ERROR,RX image unavailable\r\n");
        return;
    }
    if (strcmp(line, "FPGA,LOAD,TX") == 0) {
        cdc_write(switch_stored_fpga(DDC_FPGA_IMAGE_SDR_TX)
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
                  "  AUDIO,STATS  - Show capture buffer statistics\r\n"
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

/* Poll the CDC receive FIFO and assemble CR- or LF-terminated commands. This
 * is called from the main loop, not from a per-byte USB interrupt callback. */
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

/* TinyUSB calls this when the host opens/closes the virtual serial port.
 * DTR is the host's "terminal is connected" signal. */
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
    /* A 1200-baud open is the common host convention for requesting BOOTSEL. */
    if (p_line_coding->bit_rate == 1200) {
        reset_usb_boot(0, 0);
    }
}

/* CDC bytes are polled by cdc_task(); this notification needs no work. */
void tud_cdc_rx_cb(uint8_t itf)
{
    (void)itf;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport,
                                   tusb_control_request_t const *request)
{
    (void)rhport;
    /* TinyUSB calls this when it closes the old alternate setting's endpoint. */
    if ((request->wIndex & 0xffu) == DDC_AUDIO_CAPTURE_INTERFACE) {
        capture_i2s_stop();
    } else if ((request->wIndex & 0xffu) == DDC_AUDIO_PLAYBACK_INTERFACE) {
        playback_i2s_stop();
    }
    return true;
}

bool tud_audio_set_req_ep_cb(uint8_t rhport,
                             tusb_control_request_t const *request,
                             uint8_t *buffer)
{
    (void)rhport;
    /* The host sends sample frequency as three little-endian bytes. Save it;
     * the Set Interface callback applies the actual supported rate. */
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
    /* TinyUSB requires control replies to be copied into a scheduled EP0
     * transfer; this helper owns that buffer until the control transfer ends. */
    return tud_audio_buffer_and_schedule_control_xfer(rhport, request,
                                                        frequency, sizeof(frequency));
}

/* UAC1 feature-unit controls carry a channel number and control selector in
 * wValue. Channel 0 is master; channels 1 and 2 are left and right. */
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
    /* Initialize the Pico SDK board support, then set the board's RF routing
     * and status GPIOs to safe defaults. */
    board_init();
    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);
    gpio_put(25, 1);
    gpio_init(DDC_TR_PIN);
    gpio_set_dir(DDC_TR_PIN, GPIO_OUT);
    set_rf_path_receive(true);
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

    /* Set the RP2040 core voltage before raising its system clock to 125 MHz. */
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    set_sys_clock_khz(125000, true);

    /* Load the PIO programs into PIO0 instruction memory. The returned offsets
     * identify where their instructions start; state machines use them below. */
    g_capture_pio_offset = pio_add_program(pio0, &i2s_rx_program);
    g_playback_pio_offset = pio_add_program(pio0, &i2s_tx_program);
    pga_configure();
    fpga_interrupt_configure();
    /* Claim DMA channels from the Pico SDK so they cannot collide with other
     * users: capture data, capture address reload, and playback data. */
    capture_data_dma_channel = dma_claim_unused_channel(true);
    capture_ctrl_dma_channel = dma_claim_unused_channel(true);
    playback_dma_channel = dma_claim_unused_channel(true);
    playback_dma_config = dma_channel_get_default_config(playback_dma_channel);
    channel_config_set_transfer_data_size(&playback_dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&playback_dma_config, true);
    channel_config_set_write_increment(&playback_dma_config, false);
    channel_config_set_dreq(&playback_dma_config, pio_get_dreq(pio0, 1, true));
    dma_channel_configure(playback_dma_channel, &playback_dma_config, &pio0->txf[1],
                          playback_audio_buffers[0], i2s_words_per_millisecond, false);
    /* The common handler acknowledges capture and playback DMA completion. */
    dma_channel_set_irq0_enabled(playback_dma_channel, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);

#ifdef DDC_FPGA_BOOT_FROM_STORED
#ifdef DDC_DEFAULT_FPGA_TX
    active_fpga_image = DDC_FPGA_IMAGE_SDR_TX;
#else
    active_fpga_image = DDC_FPGA_IMAGE_SDR_RX;
#endif
    fpga_ready = false;
    /* Configure the FPGA before USB starts. Retry because CRAM programming
     * can fail transiently during board power-up. */
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

    /* Register pico-ice-sdk hooks so its TinyUSB DFU interface can stop and
     * restart the FPGA runtime around an image update. */
    ice_usb_set_dfu_callbacks(prepare_fpga_update, complete_fpga_update);
    ice_usb_init();

    /* TinyUSB's device stack is cooperative: tud_task() processes queued USB
     * events; the following calls handle application data and commands. */
    while (true) {
        tud_task();  /* Handle USB control and endpoint events. */
        cdc_task();  /* Parse commands received on the virtual serial port. */
        // handle_fpga_interrupt();
        // agc_task();
        usb_playback_task();   /* Move USB playback samples to Pico-to-FPGA I2S. */
    }
}

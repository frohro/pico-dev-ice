//
// Standalone Left-Justified (LJ) I2S Sawtooth Audio Generator
// Designed for Dev-iCE iCE40UP5K-SG48
//
module i2s_tx_demo (
    // 30.720 MHz Master Oscillator (Pin 37 in PCF)
    input  logic clk,

    // I2S Audio Bus to Pico (Pins 12, 9, 11 in PCF)
    output logic i2s_bck,       // Pin 12 -> Pico GPIO 15
    output logic i2s_ws,        // Pin  9 -> Pico GPIO 16
    output logic i2s_rx_data,   // Pin 11 -> Pico GPIO 14 (Baseband data to Pico)
    input  logic i2s_tx_data,   // Pin 10 -> Unused input from Pico

    // SPI Runtime pins (Pins 16, 15, 17, 14 in PCF)
    input  logic spi_cs,        // Pin 16
    input  logic spi_sck,       // Pin 15
    input  logic spi_mosi,      // Pin 17
    output logic spi_miso,      // Pin 14 (High-Z to avoid SPI bus contention)

    // FPGA Interrupt to Pico (Pin 13 in PCF)
    output logic fpga_int,

    // Tri-color Status LEDs (Pins 39, 40, 41 in PCF, Active-Low)
    output logic led_red,
    output logic led_yellow,
    output logic led_green
);

    // Keep unused/shared buses safe
    assign spi_miso = 1'bz;
    assign fpga_int = 1'b0;     // No OTR interrupts to Pico
    assign led_red  = 1'b1;     // Red OFF
    assign led_yellow = 1'b1;   // Yellow OFF
    // led_green is driven below to indicate when reset is released!

    // -------------------------------------------------------------
    // Timing parameters for 48 kHz stereo (64 bits per frame)
    // 30.72 MHz / (2 * 5) = 3.072 MHz BCK
    // 3.072 MHz / 64 bits = 48.000 kHz sample rate
    // -------------------------------------------------------------
    localparam integer BCK_HALF_DIV = 5;
    localparam logic [23:0] SAW_START = 24'h800000; // -1.0 in signed 24-bit
    localparam logic [23:0] SAW_STEP  = 24'h010000; // 187.5 Hz audio tone

    // -------------------------------------------------------------
    // Internal Power-On Reset (Runs automatically after CDONE)
    // -------------------------------------------------------------
    logic [7:0] por_count = 8'd0;
    logic reset_n = 1'b0;

    always_ff @(posedge clk) begin
        if (por_count != 8'hFF) begin
            por_count <= por_count + 1'b1;
            reset_n   <= 1'b0;
            led_green <= 1'b1;   // Green LED OFF while in reset
        end else begin
            reset_n   <= 1'b1;
            led_green <= 1'b0;   // Green LED ON when actively transmitting!
        end
    end

    // -------------------------------------------------------------
    // Left-Justified I2S Generator
    // -------------------------------------------------------------
    logic [2:0]  bck_divider = '0;
    logic [5:0]  frame_bit   = '0;
    logic [23:0] sawtooth    = SAW_START;
    logic [31:0] left_word   = {SAW_START, 8'b0};
    logic [31:0] right_word  = {SAW_START, 8'b0};

    logic [23:0] next_sawtooth;
    assign next_sawtooth = sawtooth + SAW_STEP;

    always_ff @(posedge clk) begin
        if (!reset_n) begin
            bck_divider <= '0;
            frame_bit   <= 6'd0;
            sawtooth    <= SAW_START;
            left_word   <= {SAW_START, 8'b0};
            right_word  <= {SAW_START, 8'b0};
            i2s_bck     <= 1'b0;
            i2s_ws      <= 1'b1;             // LJ: Left channel is WS = 1
            i2s_rx_data <= SAW_START[23];    // MSB ready for 1st rising edge
        end else if (bck_divider == BCK_HALF_DIV - 1) begin
            bck_divider <= '0;

            if (i2s_bck) begin
                // BCK Falling Edge: Update Data & WS
                i2s_bck <= 1'b0;

                if (frame_bit < 6'd31) begin
                    // Left Channel bits 30 down to 0
                    frame_bit   <= frame_bit + 1'b1;
                    i2s_rx_data <= left_word[30 - frame_bit];
                end else if (frame_bit == 6'd31) begin
                    // Switch to Right Channel (WS drops to 0, MSB immediately presented)
                    frame_bit   <= 6'd32;
                    i2s_ws      <= 1'b0;         // LJ: Right channel is WS = 0
                    i2s_rx_data <= right_word[31];
                end else if (frame_bit < 6'd63) begin
                    // Right Channel bits 30 down to 0
                    frame_bit   <= frame_bit + 1'b1;
                    i2s_rx_data <= right_word[62 - frame_bit];
                end else begin
                    // Switch to Left Channel (WS rises to 1, next sample MSB presented)
                    frame_bit   <= 6'd0;
                    i2s_ws      <= 1'b1;         // LJ: Left channel is WS = 1
                    sawtooth    <= next_sawtooth;
                    left_word   <= {next_sawtooth, 8'b0};
                    right_word  <= {next_sawtooth, 8'b0};
                    i2s_rx_data <= next_sawtooth[23];
                end
            end else begin
                // BCK Rising Edge: Receiver samples data
                i2s_bck <= 1'b1;
            end
        end else begin
            bck_divider <= bck_divider + 1'b1;
        end
    end

endmodule

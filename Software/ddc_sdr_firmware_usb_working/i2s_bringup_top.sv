module i2s_bringup_top (
    input  wire clk_30m,     // Pin 37: 30.720 MHz Master Clock
    output wire i2s_bck,     // Pin 11: 3.072 MHz I2S Bit Clock (Pico GPIO 15)
    output wire i2s_ws,      // Pin 12: 48.000 kHz I2S Word Select (Pico GPIO 16)
    output wire i2s_rx_data, // Pin 10: I2S Audio Data Out (Pico GPIO 14)
    output wire led_r,       // Pin 39: Red LED (1 = OFF)
    output wire led_y,       // Pin 40: Yellow LED (1 = OFF)
    output wire led_g        // Pin 41: Green LED (Blinks)
);

    // Power-on reset generator (auto-releases after 256 clock cycles)
    reg [7:0] rst_cnt = 8'd0;
    wire reset_n = (rst_cnt == 8'hFF);
    always @(posedge clk_30m) begin
        if (!reset_n)
            rst_cnt <= rst_cnt + 1'b1;
    end

    // Heartbeat LED counter (~0.9 Hz blink from 30.720 MHz)
    reg [24:0] hb_cnt = 25'd0;
    always @(posedge clk_30m) begin
        hb_cnt <= hb_cnt + 1'b1;
    end

    assign led_r = 1'b1;        // OFF
    assign led_y = 1'b1;        // OFF
    assign led_g = hb_cnt[24];  // Blinking green

    // Instantiate I2S transmitter
    i2s_tx_demo #(
        .BCK_HALF_DIV(5),           // 30.720 MHz / 10 = 3.072 MHz BCK
        .SAW_START(24'h800000),
        .SAW_STEP(24'h010000)
    ) u_i2s (
        .clk_30m(clk_30m),
        .reset_n(reset_n),
        .i2s_bck(i2s_bck),
        .i2s_ws(i2s_ws),
        .i2s_rx_data(i2s_rx_data)
    );

endmodule

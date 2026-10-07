module i2s_tx_demo #(
    parameter integer BCK_HALF_DIV = 5,
    parameter logic [23:0] SAW_START = 24'h800000,
    parameter logic [23:0] SAW_STEP = 24'h010000
) (
    input logic clk_30m,
    input logic reset_n,
    output logic i2s_bck,
    output logic i2s_ws,
    output logic i2s_rx_data
);

    localparam integer DIVIDER_WIDTH = (BCK_HALF_DIV < 2) ? 1 : $clog2(BCK_HALF_DIV);

    logic [DIVIDER_WIDTH - 1:0] bck_divider;
    logic [5:0] frame_bit;
    logic [23:0] sawtooth_value;
    logic [31:0] left_word;
    logic [31:0] right_word;
    logic [23:0] next_sawtooth;

    assign next_sawtooth = sawtooth_value + SAW_STEP;

    always_ff @(posedge clk_30m or negedge reset_n) begin
        if (!reset_n) begin
            bck_divider <= '0;
            frame_bit <= 6'd0;
            sawtooth_value <= SAW_START;
            left_word <= {SAW_START, 8'b0};
            right_word <= {SAW_START, 8'b0};
            i2s_bck <= 1'b0;
            i2s_ws <= 1'b0;
            i2s_rx_data <= SAW_START[23];
        end else if (bck_divider == BCK_HALF_DIV - 1) begin
            bck_divider <= '0;
            if (i2s_bck) begin
                i2s_bck <= 1'b0;
                if (frame_bit == 6'd0)
                    i2s_rx_data <= left_word[31];
                else if (frame_bit <= 6'd31)
                    i2s_rx_data <= left_word[31 - frame_bit];
                else if (frame_bit == 6'd32)
                    i2s_rx_data <= right_word[31];
                else begin
                    i2s_rx_data <= right_word[63 - frame_bit];
                    if (frame_bit == 6'd63) begin
                        sawtooth_value <= next_sawtooth;
                        left_word <= {next_sawtooth, 8'b0};
                        right_word <= {next_sawtooth, 8'b0};
                    end
                end
            end else begin
                i2s_bck <= 1'b1;
                if (frame_bit == 6'd31) begin
                    i2s_ws <= 1'b1;
                    frame_bit <= 6'd32;
                end else if (frame_bit == 6'd63) begin
                    i2s_ws <= 1'b0;
                    frame_bit <= 6'd0;
                end else begin
                    frame_bit <= frame_bit + 1'b1;
                end
            end
        end else begin
            bck_divider <= bck_divider + 1'b1;
        end
    end

endmodule
module i2s_transmitter #(
    parameter integer BCK_HALF_DIV = 5
) (
    input logic clk_30m,
    input logic reset_n,
    input logic signed [23:0] sample_i,
    input logic signed [23:0] sample_q,
    input logic sample_valid,
    output logic i2s_bck,
    output logic i2s_ws,
    output logic i2s_rx_data
);

    localparam integer DIVIDER_WIDTH = (BCK_HALF_DIV < 2) ? 1 : $clog2(BCK_HALF_DIV);

    logic [DIVIDER_WIDTH - 1:0] bck_divider;
    logic [5:0] frame_bit;
    logic signed [23:0] pending_i;
    logic signed [23:0] pending_q;
    logic [31:0] left_word;
    logic [31:0] right_word;

    wire signed [23:0] pending_i_for_frame = sample_valid ? sample_i : pending_i;
    wire signed [23:0] pending_q_for_frame = sample_valid ? sample_q : pending_q;

    always_ff @(posedge clk_30m or negedge reset_n) begin
        if (!reset_n) begin
            bck_divider <= '0;
            frame_bit <= 6'd0;
            pending_i <= 24'sd0;
            pending_q <= 24'sd0;
            left_word <= 32'd0;
            right_word <= 32'd0;
            i2s_bck <= 1'b0;
            i2s_ws <= 1'b0;
            i2s_rx_data <= 1'b0;
        end else begin
            if (sample_valid) begin
                pending_i <= sample_i;
                pending_q <= sample_q;
            end

            if (bck_divider == BCK_HALF_DIV - 1) begin
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
                            left_word <= {pending_i_for_frame, 8'b0};
                            right_word <= {pending_q_for_frame, 8'b0};
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
    end

endmodule
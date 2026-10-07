module i2s_clock_master (
    input  logic clk_30m,
    input  logic reset_n,
    input  logic rate_96,
    output logic i2s_bck,
    output logic i2s_ws
);

    logic [2:0] phase_acc;
    logic [5:0] frame_bit;
    logic [2:0] phase_increment;

    always_comb begin
        phase_increment = rate_96 ? 3'd2 : 3'd1;
    end

    always_ff @(posedge clk_30m or negedge reset_n) begin
        if (!reset_n) begin
            phase_acc <= 3'd0;
            frame_bit <= 6'd0;
            i2s_bck <= 1'b0;
            i2s_ws <= 1'b0;
        end else if (phase_acc >= 3'd5 - phase_increment) begin
            phase_acc <= phase_acc + phase_increment - 3'd5;
            i2s_bck <= ~i2s_bck;

            // Change WS one complete BCK period before the new MSB is
            // sampled, as required by Philips I2S.
            if (!i2s_bck) begin
                if (frame_bit == 6'd31)
                    i2s_ws <= 1'b1;
                else if (frame_bit == 6'd63)
                    i2s_ws <= 1'b0;
            end else if (frame_bit == 6'd63) begin
                frame_bit <= 6'd0;
            end else begin
                frame_bit <= frame_bit + 1'b1;
            end
        end else begin
            phase_acc <= phase_acc + phase_increment;
        end
    end

endmodule

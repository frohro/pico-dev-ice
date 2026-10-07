module i2s_receiver (
    input  logic clk_30m,
    input  logic reset_n,
    input  logic i2s_bck,
    input  logic i2s_ws,
    input  logic i2s_data,
    output logic signed [23:0] sample_i,
    output logic signed [23:0] sample_q,
    output logic sample_valid
);

    logic bck_delayed;
    logic active_ws;
    logic [5:0] bit_count;
    logic [31:0] shift_reg;
    logic signed [23:0] left_sample;

    wire bck_rising = i2s_bck && !bck_delayed;
    wire [31:0] completed_word = {shift_reg[30:0], i2s_data};

    always_ff @(posedge clk_30m or negedge reset_n) begin
        if (!reset_n) begin
            bck_delayed <= 1'b0;
            active_ws <= 1'b0;
            bit_count <= 6'd0;
            shift_reg <= 32'd0;
            left_sample <= 24'sd0;
            sample_i <= 24'sd0;
            sample_q <= 24'sd0;
            sample_valid <= 1'b0;
        end else begin
            bck_delayed <= i2s_bck;
            sample_valid <= 1'b0;

            if (bck_rising) begin
                if (i2s_ws != active_ws) begin
                    // This edge is the final bit of the old slot. WS has
                    // already changed, so do not treat it as the new slot's
                    // first bit.
                    if (bit_count == 6'd31) begin
                        if (!active_ws) begin
                            left_sample <= $signed(completed_word[31:8]);
                        end else begin
                            sample_i <= left_sample;
                            sample_q <= $signed(completed_word[31:8]);
                            sample_valid <= 1'b1;
                        end
                    end
                    active_ws <= i2s_ws;
                    bit_count <= 6'd0;
                    shift_reg <= 32'd0;
                end else begin
                    shift_reg <= {shift_reg[30:0], i2s_data};
                    bit_count <= bit_count + 1'b1;
                end
            end
        end
    end

endmodule

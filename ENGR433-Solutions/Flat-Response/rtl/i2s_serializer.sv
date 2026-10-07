module i2s_serializer (
    input  logic clk_30m,
    input  logic reset_n,
    input  logic i2s_bck,
    input  logic i2s_ws,
    input  logic signed [23:0] sample_i,
    input  logic signed [23:0] sample_q,
    input  logic sample_valid,
    output logic i2s_data
);

    logic bck_delayed;
    logic slot_ws;
    logic [4:0] bit_index;
    logic [31:0] left_word;
    logic [31:0] right_word;

    wire bck_falling = !i2s_bck && bck_delayed;

    always_ff @(posedge clk_30m or negedge reset_n) begin
        if (!reset_n) begin
            bck_delayed <= 1'b0;
            slot_ws <= 1'b0;
            bit_index <= 5'd30;
            left_word <= 32'd0;
            right_word <= 32'd0;
            i2s_data <= 1'b0;
        end else begin
            bck_delayed <= i2s_bck;
            if (sample_valid) begin
                left_word <= {sample_i, 8'd0};
                right_word <= {sample_q, 8'd0};
            end

            if (bck_falling) begin
                if (i2s_ws != slot_ws) begin
                    slot_ws <= i2s_ws;
                    i2s_data <= i2s_ws ? right_word[31] : left_word[31];
                    bit_index <= 5'd30;
                end else if (i2s_ws) begin
                    i2s_data <= right_word[bit_index];
                    if (bit_index != 0)
                        bit_index <= bit_index - 1'b1;
                end else begin
                    i2s_data <= left_word[bit_index];
                    if (bit_index != 0)
                        bit_index <= bit_index - 1'b1;
                end
            end
        end
    end

endmodule

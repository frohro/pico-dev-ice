`timescale 1ns/1ps

module i2s_tb;

    localparam integer BCK_HALF_DIV = 2;
    localparam integer SLOT_COUNT = 6;

    logic clk_30m = 1'b0;
    logic reset_n = 1'b0;
    logic i2s_bck;
    logic i2s_ws;
    logic i2s_rx_data;
    logic [31:0] expected_words [0:SLOT_COUNT - 1];
    logic [31:0] captured_word;
    logic previous_ws = 1'b0;
    integer bit_count = 0;
    integer word_count = 0;

    i2s_tx_demo #(
        .BCK_HALF_DIV(BCK_HALF_DIV)
    ) dut (
        .clk_30m(clk_30m),
        .reset_n(reset_n),
        .i2s_bck(i2s_bck),
        .i2s_ws(i2s_ws),
        .i2s_rx_data(i2s_rx_data)
    );

    always #1 clk_30m = ~clk_30m;

    initial begin
        $readmemh("expected_words.mem", expected_words);
        #5 reset_n = 1'b1;
        #3000;
        $display("FAIL: timeout after %0d channel slots", word_count);
        $fatal(1);
    end

    always @(posedge i2s_bck) begin
        if (reset_n) begin
            captured_word = {captured_word[30:0], i2s_rx_data};
            if (i2s_ws != previous_ws) begin
                if (i2s_ws !== ((word_count % 2) == 0)) begin
                    $display("FAIL: unexpected WS=%b at slot %0d", i2s_ws, word_count);
                    $fatal(1);
                end
                if (captured_word !== expected_words[word_count]) begin
                    $display("FAIL: slot %0d expected %h got %h",
                             word_count, expected_words[word_count], captured_word);
                    $fatal(1);
                end
                word_count = word_count + 1;
                bit_count = 0;
                captured_word = '0;
                if (word_count == SLOT_COUNT) begin
                    $display("PASS: checked %0d I2S channel slots", SLOT_COUNT);
                    $finish;
                end
            end else begin
                if (bit_count == 31) begin
                    $display("FAIL: slot %0d did not receive a Philips WS transition", word_count);
                    $fatal(1);
                end
                bit_count = bit_count + 1;
            end
            previous_ws = i2s_ws;
        end
    end

endmodule
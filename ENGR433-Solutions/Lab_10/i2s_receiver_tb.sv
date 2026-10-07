`timescale 1ns/1ps

module i2s_receiver_tb;
    logic clk = 1'b0;
    logic reset_n = 1'b0;
    logic i2s_bck = 1'b0;
    logic i2s_ws = 1'b0;
    logic i2s_data = 1'b0;
    logic signed [23:0] sample_i;
    logic signed [23:0] sample_q;
    logic sample_valid;
    integer failures = 0;

    i2s_receiver dut (
        .clk_30m(clk),
        .reset_n(reset_n),
        .i2s_bck(i2s_bck),
        .i2s_ws(i2s_ws),
        .i2s_data(i2s_data),
        .sample_i(sample_i),
        .sample_q(sample_q),
        .sample_valid(sample_valid)
    );

    always #1 clk = ~clk;

    task send_slot;
        input [31:0] word;
        input logic slot_ws;
        input logic next_ws;
        integer bit_index;
        begin
            while (i2s_ws !== slot_ws)
                @(negedge clk);
            for (bit_index = 31; bit_index >= 0; bit_index = bit_index - 1) begin
                i2s_data = word[bit_index];
                if (bit_index == 0)
                    i2s_ws = next_ws;
                #3 i2s_bck = 1'b1;
                #3 i2s_bck = 1'b0;
            end
        end
    endtask

    initial begin
        #3 reset_n = 1'b1;
        i2s_ws = 1'b0;
        send_slot(32'h81234500, 1'b0, 1'b1);
        send_slot(32'hFEDCBA00, 1'b1, 1'b0);
        #4;
        if (sample_i !== 24'sh812345 || sample_q !== -24'sh012346) begin
            $display("FAIL: received I/Q %h/%h", sample_i, sample_q);
            failures = failures + 1;
        end
        if (failures == 0)
            $display("PASS: I2S receiver captured signed 24-bit stereo slots");
        else
            $fatal(1, "I2S receiver had %0d failures", failures);
        $finish;
    end
endmodule

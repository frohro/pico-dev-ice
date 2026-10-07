`timescale 1ns/1ps

module i2s_transmitter_dual_rate_tb;

    reg clk_30m;
    reg reset_n;
    reg rate_select;
    reg signed [23:0] sample_i;
    reg signed [23:0] sample_q;
    reg sample_valid;
    wire i2s_bck;
    wire i2s_ws;
    wire i2s_rx_data;

    // Clock generator (30.72 MHz -> 32.552 ns period)
    always #16.276 clk_30m = ~clk_30m;

    i2s_transmitter_dual_rate u_dut (
        .clk_30m      (clk_30m),
        .reset_n      (reset_n),
        .rate_select  (rate_select),
        .sample_i     (sample_i),
        .sample_q     (sample_q),
        .sample_valid (sample_valid),
        .i2s_bck      (i2s_bck),
        .i2s_ws       (i2s_ws),
        .i2s_rx_data  (i2s_rx_data)
    );

    // Bitstream deserializer to reconstruct Left and Right audio words
    reg [31:0] rx_shift;
    reg [31:0] rx_left;
    reg [31:0] rx_right;
    reg prev_ws;
    integer bit_count = 0;
    integer frames_captured = 0;
    integer error_cnt = 0;

    // Sample data on BCK rising edge (standard I2S receiver behavior)
    always @(posedge i2s_bck or negedge reset_n) begin
        if (!reset_n) begin
            rx_shift <= 32'd0;
            rx_left  <= 32'd0;
            rx_right <= 32'd0;
            prev_ws  <= 1'b0;
            bit_count <= 0;
        end else begin
            prev_ws  <= i2s_ws;

            if (i2s_ws != prev_ws) begin
                // Philips I2S changes WS on the final sampling edge of the
                // old slot. Include that edge in the old word, then begin
                // the new slot on the following rising edge.
                if (i2s_ws == 1'b1) begin
                    rx_left <= {rx_shift[30:0], i2s_rx_data};
                end else begin
                    rx_right <= {rx_shift[30:0], i2s_rx_data};
                    frames_captured <= frames_captured + 1;
                end
                rx_shift <= 32'd0;
                bit_count <= 0;
            end else begin
                rx_shift <= {rx_shift[30:0], i2s_rx_data};
                bit_count <= bit_count + 1;
            end
        end
    end

    initial begin
        clk_30m = 0;
        reset_n = 0;
        rate_select = 0; // 48 kHz mode
        sample_i = 24'h123456;
        sample_q = 24'h789ABC;
        sample_valid = 0;

        #100;
        @(posedge clk_30m);
        reset_n = 1;
        #100;

        $display("================================================================");
        $display(" Starting Dual-Rate I2S Transmitter Testbench (48 kHz Mode)");
        $display("================================================================");

        // Send sample strobe
        @(posedge clk_30m);
        sample_i = 24'sh123456;
        sample_q = -24'sh123456;
        sample_valid = 1'b1;
        @(posedge clk_30m);
        sample_valid = 1'b0;

        // Wait for 3 complete I2S frames at 48 kHz
        wait(frames_captured >= 2);
        #100;

        $display("Reconstructed Frame @ 48 kHz -> Left: 0x%06x, Right: 0x%06x", rx_left[31:8], rx_right[31:8]);
        if (rx_left[31:8] !== 24'h123456 || rx_right[31:8] !== (-24'sh123456 & 24'hFFFFFF)) begin
            $display("ERROR: 48 kHz I2S frame mismatch! Expected (0x123456, 0x%06x), Got (0x%06x, 0x%06x)",
                     (-24'sh123456 & 24'hFFFFFF), rx_left[31:8], rx_right[31:8]);
            error_cnt = error_cnt + 1;
        end else begin
            $display("SUCCESS: 48 kHz I2S frame deserialized with exact bit alignment!");
        end

        // Switch to 96 kHz mode
        $display("\n================================================================");
        $display(" Switching to 96 kHz Mode (Rate Select = 1)");
        $display("================================================================");
        @(posedge clk_30m);
        rate_select = 1;
        sample_i = 24'sh55AA33;
        sample_q = 24'shAACC55;
        sample_valid = 1'b1;
        @(posedge clk_30m);
        sample_valid = 1'b0;

        frames_captured = 0;
        wait(frames_captured >= 2);
        #100;

        $display("Reconstructed Frame @ 96 kHz -> Left: 0x%06x, Right: 0x%06x", rx_left[31:8], rx_right[31:8]);
        if (rx_left[31:8] !== 24'h55AA33 || rx_right[31:8] !== 24'hAACC55) begin
            $display("ERROR: 96 kHz I2S frame mismatch! Expected (0x55AA33, 0xAACC55), Got (0x%06x, 0x%06x)",
                     rx_left[31:8], rx_right[31:8]);
            error_cnt = error_cnt + 1;
        end else begin
            $display("SUCCESS: 96 kHz I2S frame deserialized with exact bit alignment!");
        end

        #200;
        if (error_cnt == 0) begin
            $display("\n================================================================");
            $display(" ALL DUAL-RATE I2S TESTS PASSED (48k & 96k MODES VERIFIED)!");
            $display("================================================================");
        end else begin
            $display("\n================================================================");
            $display(" I2S TRANSMITTER TEST FAILED with %0d errors.", error_cnt);
            $display("================================================================");
            $fatal(1);
        end
        $finish;
    end

endmodule

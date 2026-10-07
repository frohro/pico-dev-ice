`timescale 1ns/1ps

module ddc_sdr_top_tb;

    localparam [31:0] TEST_FCW = 32'h08555555;
    localparam integer CIC_RATIO = 640;

    logic clk = 1'b0;
    logic [7:0] adc_data = 8'h81;
    logic adc_otr = 1'b0;
    logic spi_mosi = 1'b0;
    logic spi_sck = 1'b0;
    logic spi_cs = 1'b1;
    logic [3:0] pmod = 4'b0000;
    logic encoder_a = 1'b0;
    logic encoder_b = 1'b0;
    logic encoder_button = 1'b0;
    wire adc_clk;
    wire dac_clk;
    wire [7:0] dac_data;
    wire spi_miso;
    wire i2s_bck;
    wire i2s_ws;
    wire i2s_rx_data;
    wire i2s_tx_data;
    wire fpga_int;
    wire led_red;
    wire led_yellow;
    wire led_green;

    integer failures = 0;
    integer cycle_count = 0;
    integer valid_count = 0;
    integer last_valid_cycle = -1;
    integer frequency_valid_count = 0;
    integer i2s_bit_count = 0;
    integer i2s_slot_count = 0;
    integer stable_audio_checks = 0;
    integer reset_generation = 0;
    integer seen_reset_generation = -1;
    logic [31:0] i2s_captured_word = 32'd0;
    logic i2s_previous_ws = 1'b0;
    logic check_stable_audio = 1'b0;
    logic check_zero_audio = 1'b1;

    ddc_sdr_top dut (
        .clk(clk),
        .adc_clk(adc_clk),
        .dac_clk(dac_clk),
        .adc_data(adc_data),
        .adc_otr(adc_otr),
        .dac_data(dac_data),
        .spi_miso(spi_miso),
        .spi_mosi(spi_mosi),
        .spi_sck(spi_sck),
        .spi_cs(spi_cs),
        .i2s_bck(i2s_bck),
        .i2s_ws(i2s_ws),
        .i2s_rx_data(i2s_rx_data),
        .i2s_tx_data(i2s_tx_data),
        .fpga_int(fpga_int),
        .encoder_a(encoder_a),
        .encoder_b(encoder_b),
        .encoder_button(encoder_button),
        .pmod(pmod),
        .led_red(led_red),
        .led_yellow(led_yellow),
        .led_green(led_green)
    );

    always #1 clk = ~clk;

    task send_byte;
        input [7:0] value;
        integer bit_index;
        begin
            for (bit_index = 7; bit_index >= 0; bit_index = bit_index - 1) begin
                spi_mosi = value[bit_index];
                #0.5 spi_sck = 1'b1;
                #0.5 spi_sck = 1'b0;
            end
        end
    endtask

    task send_frame;
        input [7:0] command;
        input [31:0] value;
        begin
            spi_cs = 1'b0;
            send_byte(8'hD5);
            send_byte(8'h01);
            send_byte(command);
            send_byte(8'h04);
            send_byte(value[7:0]);
            send_byte(value[15:8]);
            send_byte(value[23:16]);
            send_byte(value[31:24]);
            spi_cs = 1'b1;
            spi_mosi = 1'b0;
        end
    endtask

    task fail;
        input [8*120-1:0] message;
        begin
            $display("FAIL: %0s", message);
            failures = failures + 1;
        end
    endtask

    always @(negedge pmod[0]) begin
        reset_generation = reset_generation + 1;
    end

    always @(posedge clk) begin
        if (!pmod[0]) begin
            cycle_count = 0;
            valid_count = 0;
            last_valid_cycle = -1;
            frequency_valid_count = 0;
        end else begin
            cycle_count = cycle_count + 1;

            if (dut.cic_i_valid !== dut.cic_q_valid)
                fail("I and Q CIC valid strobes are not simultaneous");

            if (dut.cic_i_valid) begin
                valid_count = valid_count + 1;
                if (last_valid_cycle >= 0 &&
                    cycle_count - last_valid_cycle != CIC_RATIO)
                    fail("CIC valid interval changed during integration");
                last_valid_cycle = cycle_count;
            end

            if (dut.cmd_freq_valid)
                frequency_valid_count = frequency_valid_count + 1;
        end
    end

    always @(posedge i2s_bck) begin
        if (!pmod[0]) begin
            i2s_bit_count = 0;
            i2s_slot_count = 0;
            i2s_captured_word = 32'd0;
            i2s_previous_ws = 1'b0;
            seen_reset_generation = reset_generation;
        end else begin
            if (seen_reset_generation != reset_generation) begin
                i2s_bit_count = 0;
                i2s_slot_count = 0;
                i2s_captured_word = 32'd0;
                i2s_previous_ws = 1'b0;
                seen_reset_generation = reset_generation;
            end

            i2s_captured_word = {i2s_captured_word[30:0], i2s_rx_data};
            if (i2s_ws != i2s_previous_ws) begin
                if (i2s_ws !== ((i2s_slot_count % 2) == 0))
                    fail("I2S WS changed at the wrong Philips boundary");

                if (check_zero_audio && i2s_previous_ws == 1'b0 &&
                    i2s_captured_word !== 32'h00000000)
                    fail("zero-frequency I channel is not zero");

                if (check_stable_audio && i2s_previous_ws == 1'b1) begin
                    if (i2s_captured_word !== 32'h007C0600)
                        fail("steady-state Q sample does not match CIC gain");
                    stable_audio_checks = stable_audio_checks + 1;
                end

                i2s_slot_count = i2s_slot_count + 1;
                i2s_bit_count = 0;
                i2s_captured_word = 32'd0;
            end else begin
                if (i2s_ws !== ((i2s_slot_count % 2) != 0))
                    fail("I2S WS changed within a Philips slot");
                if (i2s_bit_count == 31)
                    fail("I2S slot did not receive a Philips WS transition");
                i2s_bit_count = i2s_bit_count + 1;
            end
            i2s_previous_ws = i2s_ws;
        end
    end

    initial begin
        #5;
        pmod[0] = 1'b1;
        wait (dut.internal_rst_n);

        send_frame(8'h99, 32'h00000001);
        repeat (5) @(posedge clk);
        if (frequency_valid_count != 0)
            fail("invalid SPI command generated a frequency update");

        spi_cs = 1'b0;
        send_byte(8'hD5);
        send_byte(8'h01);
        send_byte(8'h01);
        pmod[0] = 1'b0;
        #2;
        pmod[0] = 1'b1;
        spi_cs = 1'b1;
        repeat (5) @(posedge clk);
        if (frequency_valid_count != 0)
            fail("reset during SPI traffic committed a partial command");

        adc_otr = 1'b1;
        @(posedge clk);
        #0.1;
        adc_otr = 1'b0;
        if (fpga_int !== 1'b1)
            fail("OTR pulse was not latched");

        send_frame(8'h04, 32'h00000001);
        repeat (5) @(posedge clk);
        if (fpga_int !== 1'b0)
            fail("clear-OTR command did not clear the interrupt");

        while (valid_count < 8)
            @(posedge clk);
        check_stable_audio = 1'b1;
        while (stable_audio_checks < 1)
            @(posedge clk);
        check_stable_audio = 1'b0;

        while (dut.decimator_i.decimation_count != 10'd638)
            @(posedge clk);
        send_frame(8'h01, TEST_FCW);
        repeat (5) @(posedge clk);
        if (frequency_valid_count != 1 || dut.cmd_freq_val !== TEST_FCW)
            fail("frequency update near a CIC boundary was not committed");

        while (valid_count < 10)
            @(posedge clk);

        if (dac_clk !== 1'b0 || dac_data !== 8'h00 || i2s_tx_data !== 1'b0)
            fail("unused transmit interfaces are not tied to known values");

        if (failures == 0)
            $display("PASS: Lab 09 integration, I2S, SPI, OTR, reset, and CIC checks passed");
        else
            $fatal(1, "Lab 09 integration had %0d failures", failures);
        $finish;
    end

    initial begin
        #20000;
        $fatal(1, "Lab 09 integration timeout");
    end

endmodule
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

    logic [23:0] audio_signal;
    logic sample_req;

    sawtooth_gen #(
        .SAW_START(SAW_START),
        .SAW_STEP(SAW_STEP)
    ) sample_generator (
        .clk_30m(clk_30m),
        .reset_n(reset_n),
        .sample_req(sample_req),
        .out_data(audio_signal)
    );

    i2s_master_tx #(
        .BCK_HALF_DIV(BCK_HALF_DIV)
    ) transmitter (
        .clk_30m(clk_30m),
        .reset_n(reset_n),
        .left_data(audio_signal),
        .right_data(audio_signal),
        .sample_req(sample_req),
        .i2s_bck(i2s_bck),
        .i2s_ws(i2s_ws),
        .i2s_tx_data(i2s_rx_data)
    );

endmodule
`default_nettype none

// 1. Simulation/Linting stub for Verilator
// Yosys ignores this completely, but Verilator uses it to know the port directions.
`ifdef VERILATOR
module SB_RGBA_DRV #(
    parameter CURRENT_MODE = "0b0",
    parameter RGB0_CURRENT = "0b000000",
    parameter RGB1_CURRENT = "0b000000",
    parameter RGB2_CURRENT = "0b000000"
)(
    input  logic CURREN,
    input  logic RGBLEDEN,
    input  logic RGB0PWM,
    input  logic RGB1PWM,
    input  logic RGB2PWM,
    output logic RGB0,
    output logic RGB1,
    output logic RGB2
);
    assign RGB0 = RGB0PWM;
    assign RGB1 = RGB1PWM;
    assign RGB2 = RGB2PWM;
endmodule
`endif

module top (
    input  logic clk,       // 30.72 MHz Hardware Oscillator
    output logic led_blue,
    output logic led_green,
    output logic led_red
);

    // Tell Verilator that this inline initialization is intentional for FPGA power-on
    /* verilator lint_off PROCASSINIT */
    logic [24:0] counter = 25'd0;
    /* verilator lint_on PROCASSINIT */

    always_ff @(posedge clk) begin
        counter <= counter + 1'b1;
    end

    // Dedicated iCE40 Hard IP RGB Current-Sink Driver
    SB_RGBA_DRV #(
        .CURRENT_MODE("0b1"),       // Half-current mode
        .RGB0_CURRENT("0b000001"),  // ~4 mA current
        .RGB1_CURRENT("0b000001"),
        .RGB2_CURRENT("0b000001")
    ) rgb_driver_inst (
        .CURREN(1'b1),
        .RGBLEDEN(1'b1),
        .RGB0PWM(counter[24]),      // Blue
        .RGB1PWM(counter[23]),      // Green
        .RGB2PWM(~counter[24]),     // Red
        .RGB0(led_blue),
        .RGB1(led_green),
        .RGB2(led_red)
    );

endmodule

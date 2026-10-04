`default_nettype none

// 1. Simulation/Linting stub for Verilator
// Yosys ignores this completely, but Verilator uses it to understand port directions.
`ifdef VERILATOR
/* verilator lint_off DECLFILENAME */
/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off UNUSEDSIGNAL */
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
/* verilator lint_on DECLFILENAME */
/* verilator lint_on UNUSEDPARAM */
/* verilator lint_on UNUSEDSIGNAL */
`endif

module top (
    input  logic clk,          // 30.72 MHz Hardware Oscillator (Pin 37)
    output logic led_red,      // Pin 39 (RGB0)
    output logic led_yellow,   // Pin 40 (RGB1)
    output logic led_green     // Pin 41 (RGB2)
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
        .CURREN(1'b1),              // Master driver enable
        .RGBLEDEN(1'b1),            // LED enable
        .RGB0PWM(~counter[24]),     // Red    (Pin 39 / RGB0)
        .RGB1PWM(counter[24]),      // Yellow (Pin 40 / RGB1)
        .RGB2PWM(counter[23]),      // Green  (Pin 41 / RGB2 - toggles twice as fast)
        .RGB0(led_red),
        .RGB1(led_yellow),
        .RGB2(led_green)
    );

endmodule

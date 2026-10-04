#include <stdio.h>

#include "../agc_control.h"

static int failures;

static void expect_code(const ddc_agc_state_t *state, uint8_t expected)
{
    if (state->pga_code != expected) {
        printf("FAIL: expected PGA code 0x%02x got 0x%02x\n",
               expected, state->pga_code);
        failures++;
    }
}

static void expect_bool(bool actual, bool expected, const char *name)
{
    if (actual != expected) {
        printf("FAIL: %s expected %d got %d\n", name, expected, actual);
        failures++;
    }
}

int main(void)
{
    ddc_agc_state_t state;

    ddc_agc_init(&state);
    expect_code(&state, 0x0u);
    expect_bool(ddc_agc_on_otr(&state), true, "first OTR transition");
    expect_code(&state, 0x1u);
    expect_bool(ddc_agc_on_otr(&state), true, "second OTR transition");
    expect_code(&state, 0x3u);
    expect_bool(ddc_agc_on_otr(&state), true, "third OTR transition");
    expect_code(&state, 0xfu);
    expect_bool(ddc_agc_on_otr(&state), false, "maximum-state OTR transition");
    expect_code(&state, 0xfu);

    expect_bool(ddc_agc_tick(&state, true, 1000), false, "active interrupt tick");
    expect_bool(ddc_agc_tick(&state, false, 2000), false, "quiet timer start");
    expect_bool(ddc_agc_tick(&state, false, 3999), false, "pre-decay boundary");
    expect_bool(ddc_agc_tick(&state, false, 4000), true, "first decay boundary");
    expect_code(&state, 0x3u);
    expect_bool(ddc_agc_tick(&state, false, 5999), false, "second pre-decay boundary");
    expect_bool(ddc_agc_tick(&state, false, 6000), true, "second decay boundary");
    expect_code(&state, 0x1u);
    expect_bool(ddc_agc_tick(&state, false, 8000), true, "final decay boundary");
    expect_code(&state, 0x0u);
    expect_bool(ddc_agc_tick(&state, false, 10000), false, "zero-state decay");

    ddc_agc_on_otr(&state);
    expect_bool(ddc_agc_tick(&state, false, 12000), false, "timer restart");
    expect_bool(ddc_agc_on_otr(&state), true, "timer reset OTR");
    expect_code(&state, 0x3u);
    expect_bool(ddc_agc_tick(&state, false, 13000), false, "reset timer start");
    expect_bool(ddc_agc_tick(&state, false, 14999), false, "reset timer pre-boundary");
    expect_bool(ddc_agc_tick(&state, false, 15000), true, "reset timer boundary");
    expect_code(&state, 0x1u);

    if (failures == 0)
        printf("PASS: AGC escalation, saturation, decay, and timer-reset checks passed\n");
    return failures != 0;
}
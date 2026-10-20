#ifndef DDC_AGC_CONTROL_H_
#define DDC_AGC_CONTROL_H_

#include <stdbool.h>
#include <stdint.h>

#include "ddc_protocol.h"

#define DDC_AGC_DECAY_MS 2000u

typedef struct {
    uint8_t pga_code;       /* code currently driven onto the PGA pins */
    uint8_t manual_code;    /* host (gain slider) setpoint; AGC decay stops here */
    uint64_t quiet_since_ms;
    bool quiet_timer_active;
} ddc_agc_state_t;

static inline void ddc_agc_init(ddc_agc_state_t *state)
{
    state->pga_code = 0x0u;
    state->manual_code = 0x0u;
    state->quiet_since_ms = 0;
    state->quiet_timer_active = false;
}

/* The user moved the gain slider: this becomes both the current gain and the
 * level the AGC relaxes back to after an overload. */
static inline void ddc_agc_set_manual(ddc_agc_state_t *state, uint8_t code)
{
    code &= DDC_PGA_MAX_CODE;
    state->manual_code = code;
    state->pga_code = code;
    state->quiet_timer_active = false;
}

static inline bool ddc_agc_on_otr(ddc_agc_state_t *state)
{
    uint8_t next_code = ddc_pga_next_otr_code(state->pga_code);
    bool changed = next_code != state->pga_code;

    state->pga_code = next_code;
    state->quiet_timer_active = false;
    return changed;
}

static inline bool ddc_agc_tick(ddc_agc_state_t *state,
                                bool fpga_int_high,
                                uint64_t now_ms)
{
    if (fpga_int_high || state->pga_code == state->manual_code) {
        state->quiet_timer_active = false;
        return false;
    }

    if (!state->quiet_timer_active) {
        state->quiet_since_ms = now_ms;
        state->quiet_timer_active = true;
        return false;
    }

    if (now_ms - state->quiet_since_ms < DDC_AGC_DECAY_MS)
        return false;

    uint8_t next_code = ddc_pga_previous_otr_code(state->pga_code);

    /* Never relax past the manual setpoint (codes are ordered by attenuation). */
    if (next_code <= state->manual_code)
        next_code = state->manual_code;
    state->pga_code = next_code;
    state->quiet_since_ms = now_ms;
    state->quiet_timer_active = state->pga_code != state->manual_code;
    return true;
}

#endif
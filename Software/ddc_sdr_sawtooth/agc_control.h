#ifndef DDC_AGC_CONTROL_H_
#define DDC_AGC_CONTROL_H_

#include <stdbool.h>
#include <stdint.h>

#include "ddc_protocol.h"

#define DDC_AGC_DECAY_MS 2000u

typedef struct {
    uint8_t pga_code;
    uint64_t quiet_since_ms;
    bool quiet_timer_active;
} ddc_agc_state_t;

static inline void ddc_agc_init(ddc_agc_state_t *state)
{
    state->pga_code = 0x0u;
    state->quiet_since_ms = 0;
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
    if (fpga_int_high || state->pga_code == 0x0u) {
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

    state->pga_code = ddc_pga_previous_otr_code(state->pga_code);
    state->quiet_since_ms = now_ms;
    state->quiet_timer_active = state->pga_code != 0x0u;
    return true;
}

#endif
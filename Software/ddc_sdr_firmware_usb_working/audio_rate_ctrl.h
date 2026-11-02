#ifndef AUDIO_RATE_CTRL_H
#define AUDIO_RATE_CTRL_H

#include <stdint.h>

/* Occupancy (in stereo frames) is regulated around two nominal packets (50 %
 * of the 4-packet ring). Inside +/- DEADBAND the nominal packet size is sent,
 * outside it the packet is one frame larger or smaller. */
#define AUDIO_RATE_DEADBAND_FRAMES 10u

static inline uint32_t audio_rate_target_frames(uint32_t nominal)
{
    return 2u * nominal;
}

static inline uint32_t audio_rate_pick(uint32_t occupancy, uint32_t nominal)
{
    uint32_t target = audio_rate_target_frames(nominal);

    if (occupancy > target + AUDIO_RATE_DEADBAND_FRAMES) {
        return nominal + 1u;
    }
    if (occupancy + AUDIO_RATE_DEADBAND_FRAMES < target) {
        return nominal - 1u;
    }
    return nominal;
}

#endif

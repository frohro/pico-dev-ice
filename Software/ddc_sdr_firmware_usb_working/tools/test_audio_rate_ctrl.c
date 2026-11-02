#include <stdio.h>

#include "../audio_rate_ctrl.h"

static int failures;

static void expect(uint32_t occupancy, uint32_t nominal, uint32_t expected)
{
    uint32_t actual = audio_rate_pick(occupancy, nominal);

    if (actual != expected) {
        printf("FAIL: occ %u nom %u expected %u got %u\n",
               occupancy, nominal, expected, actual);
        failures++;
    }
}

int main(void)
{
    expect(0, 48, 47);
    expect(85, 48, 47);
    expect(86, 48, 48);
    expect(96, 48, 48);
    expect(106, 48, 48);
    expect(107, 48, 49);
    expect(144, 48, 49);
    expect(182, 96, 96);
    expect(171, 96, 95);
    expect(203, 96, 97);

    if (failures == 0) {
        printf("audio rate control tests passed\n");
    }
    return failures != 0;
}

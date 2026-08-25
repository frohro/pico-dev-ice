// AudioImpl.hpp — miniaudio context + device wrapper.
//
// This header must be included ONLY in Streaming.cpp (which also defines
// MINIAUDIO_IMPLEMENTATION before including miniaudio.h).
//
// Settings.cpp stores a void* _audio pointer and casts it to AudioImpl*
// only in Streaming.cpp where the full type is known.

#pragma once
#include "miniaudio.h"

struct AudioImpl {
    ma_context context;
    ma_device  device;
    bool       contextOk = false;
    bool       deviceOk  = false;
};

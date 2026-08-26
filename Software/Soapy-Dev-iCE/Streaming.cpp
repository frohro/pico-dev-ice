// Streaming.cpp — miniaudio I/Q capture + SoapySDR stream API for Pico-Dev-iCE and WWU 2026 SDR.
//
// miniaudio is the audio backend (single-header, cross-platform).
// #define MINIAUDIO_IMPLEMENTATION must appear in exactly ONE translation unit.
//
// Data flow:
//   FPGA/PCM1808 (stereo 24-bit S24_3LE) → USB Audio → miniaudio callback
//   → ring buffer [I,Q interleaved float32]
//   → readStream() copies into caller's CF32/CS16 buffer.

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "AudioImpl.hpp"
#include "Soapy2026SDR.hpp"

#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Formats.hpp>
#include <chrono>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>

// ─── miniaudio data callback (called on audio thread) ────────────────────────

static void maDataCallback(ma_device *dev, void *output,
                            const void *input, ma_uint32 frameCount)
{
    (void)output;
    if (!input || !dev->pUserData) return;
    auto *self = static_cast<Soapy2026SDR *>(dev->pUserData);
    self->_audioCallback(input, frameCount);
}

// Public: called from maDataCallback on the audio thread
void Soapy2026SDR::_audioCallback(const void *input, unsigned int frameCount)
{
    // input is interleaved S24_3LE (3 bytes per sample, little-endian)
    // Channel 0 = I (left), Channel 1 = Q (right)
    // Each frame = 6 bytes: [I_b0 I_b1 I_b2 Q_b0 Q_b1 Q_b2]
    const uint8_t *src = static_cast<const uint8_t *>(input);

    std::unique_lock<std::mutex> lock(_ringMutex);

    for (unsigned int i = 0; i < frameCount; i++) {
        if (_ringCount >= RING_FRAMES) {
            // Overflow: drop oldest frame
            _ringHead = (_ringHead + 2) % _ring.size();
            _ringCount--;
            _ringOverflow = true;
        }
        // Unpack two S24_3LE samples and sign-extend
        const uint8_t *p = src + i * 6;
        int32_t rawI = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
        if (rawI & 0x800000) rawI |= 0xFF000000;  // sign-extend bit 23
        int32_t rawQ = (int32_t)((uint32_t)p[3] | ((uint32_t)p[4] << 8) | ((uint32_t)p[5] << 16));
        if (rawQ & 0x800000) rawQ |= 0xFF000000;

        // Normalize 24-bit ALSA S24_3LE sample to [-1.0, +1.0] by 8388608.0f (2^23)
        float I = static_cast<float>(rawI) / 8388608.0f;
        float Q = static_cast<float>(rawQ) / 8388608.0f;
        _ring[_ringTail]     = I;
        _ring[_ringTail + 1] = Q;
        _ringTail = (_ringTail + 2) % _ring.size();
        _ringCount++;
    }

    lock.unlock();
    _ringCond.notify_one();
}

// ─── Audio device setup / teardown ───────────────────────────────────────────

// Scan /proc/asound/cards to find the ALSA short card name for a device
// whose description contains the given search string (case-insensitive).
static std::string _findAlsaCardName(const std::string &needle)
{
    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s;
    };
    std::string lcNeedle = toLower(needle);

    FILE *f = fopen("/proc/asound/cards", "r");
    if (!f) return "";

    char line[256];
    std::string shortName;
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        // Lines look like: " 3 [D2026          ]: USB-Audio - DDC SDR 2026"
        // Short name is between '[' and ']'
        auto lb = l.find('[');
        auto rb = l.find(']');
        if (lb == std::string::npos || rb == std::string::npos) continue;
        std::string candidate = l.substr(lb + 1, rb - lb - 1);
        while (!candidate.empty() && candidate.back() == ' ') candidate.pop_back();
        while (!candidate.empty() && candidate.front() == ' ') candidate.erase(candidate.begin());
        if (toLower(l).find(lcNeedle) != std::string::npos) {
            shortName = candidate;
            break;
        }
    }
    fclose(f);
    return shortName;
}

void Soapy2026SDR::_setupAudio()
{
    AudioImpl *ai = new AudioImpl();

    // Direct ALSA backend bypasses PipeWire / PulseAudio resampling
    ma_backend backends[] = { ma_backend_alsa };
    ma_context_config ctxCfg = ma_context_config_init();
    if (ma_context_init(backends, 1, &ctxCfg, &ai->context) != MA_SUCCESS) {
        delete ai;
        throw std::runtime_error("2026SDR: failed to initialize miniaudio ALSA context");
    }
    ai->contextOk = true;

    // Search /proc/asound/cards using prioritize needles
    std::vector<std::string> searchNeedles;
    if (!_audioDeviceName.empty()) {
        searchNeedles.push_back(_audioDeviceName);
    }
    searchNeedles.push_back("DDC SDR 2026");
    searchNeedles.push_back("DDC SDR");
    searchNeedles.push_back("D2026");
    searchNeedles.push_back("WWU SDR");
    searchNeedles.push_back("sdr");

    std::string hwDeviceStr;
    for (const auto &needle : searchNeedles) {
        std::string cardName = _findAlsaCardName(needle);
        if (!cardName.empty()) {
            hwDeviceStr = "hw:CARD=" + cardName + ",DEV=0";
            SoapySDR_logf(SOAPY_SDR_INFO,
                "2026SDR: matched ALSA card '%s' for '%s' → device '%s'",
                cardName.c_str(), needle.c_str(), hwDeviceStr.c_str());
            break;
        }
    }

    if (hwDeviceStr.empty()) {
        SoapySDR_logf(SOAPY_SDR_WARNING,
            "2026SDR: could not find matching ALSA card in /proc/asound/cards; using default device");
    }

    ma_device_id hwId;
    ma_device_id *pDeviceId = nullptr;
    if (!hwDeviceStr.empty()) {
        MA_ZERO_OBJECT(&hwId);
        strncpy(hwId.alsa, hwDeviceStr.c_str(), sizeof(hwId.alsa) - 1);
        pDeviceId = &hwId;
    }

    ma_device_config devCfg = ma_device_config_init(ma_device_type_capture);
    devCfg.capture.pDeviceID      = pDeviceId;
    devCfg.capture.format         = ma_format_s24;   // S24_3LE
    devCfg.capture.channels       = 2;               // stereo: L=I, R=Q
    devCfg.sampleRate             = (ma_uint32)_sampleRate;
    devCfg.dataCallback           = maDataCallback;
    devCfg.pUserData              = this;
    devCfg.alsa.noAutoResample    = MA_TRUE;

    if (ma_device_init(&ai->context, &devCfg, &ai->device) != MA_SUCCESS) {
        ma_context_uninit(&ai->context);
        delete ai;
        throw std::runtime_error("2026SDR: failed to open audio capture device '"
                                 + hwDeviceStr + "'");
    }
    ai->deviceOk = true;

    _audio = static_cast<void *>(ai);

    SoapySDR_logf(SOAPY_SDR_INFO, "2026SDR: audio capture initialized at %.0f Hz on %s",
                  _sampleRate, hwDeviceStr.empty() ? "default" : hwDeviceStr.c_str());
}

void Soapy2026SDR::_teardownAudio()
{
    if (!_audio) return;
    AudioImpl *ai = static_cast<AudioImpl *>(_audio);

    if (ai->deviceOk) {
        ma_device_stop(&ai->device);
        ma_device_uninit(&ai->device);
    }
    if (ai->contextOk) {
        ma_context_uninit(&ai->context);
    }
    delete ai;
    _audio = nullptr;
}

// ─── SoapySDR Stream API ─────────────────────────────────────────────────────

static int STREAM_TOKEN = 0xA026;

std::vector<std::string> Soapy2026SDR::getStreamFormats(int, size_t) const
{
    return {SOAPY_SDR_CF32, SOAPY_SDR_CS16};
}

std::string Soapy2026SDR::getNativeStreamFormat(int, size_t,
                                                  double &fullScale) const
{
    fullScale = 1.0;
    return SOAPY_SDR_CF32;
}

SoapySDR::Stream *Soapy2026SDR::setupStream(int dir,
                                              const std::string &format,
                                              const std::vector<size_t> &,
                                              const SoapySDR::Kwargs &)
{
    if (dir != SOAPY_SDR_RX)
        throw std::runtime_error("2026SDR: only RX streaming supported");
    if (format != SOAPY_SDR_CF32 && format != SOAPY_SDR_CS16)
        throw std::runtime_error("2026SDR: unsupported format " + format);

    _streamFormat = format;

    // Reset ring buffer
    {
        std::lock_guard<std::mutex> lk(_ringMutex);
        _ringHead = _ringTail = _ringCount = 0;
        _ringOverflow = false;
    }

    return reinterpret_cast<SoapySDR::Stream *>(&STREAM_TOKEN);
}

void Soapy2026SDR::closeStream(SoapySDR::Stream *) {}

size_t Soapy2026SDR::getStreamMTU(SoapySDR::Stream *) const
{
    return 4096;
}

int Soapy2026SDR::activateStream(SoapySDR::Stream *, int, long long, size_t)
{
    AudioImpl *ai = static_cast<AudioImpl *>(_audio);
    if (!ai || !ai->deviceOk) return SOAPY_SDR_NOT_SUPPORTED;
    if (ma_device_start(&ai->device) != MA_SUCCESS) return SOAPY_SDR_STREAM_ERROR;
    return 0;
}

int Soapy2026SDR::deactivateStream(SoapySDR::Stream *, int, long long)
{
    AudioImpl *ai = static_cast<AudioImpl *>(_audio);
    if (ai && ai->deviceOk) ma_device_stop(&ai->device);
    _ringCond.notify_all();
    return 0;
}

int Soapy2026SDR::readStream(SoapySDR::Stream *stream,
                              void *const *buffs,
                              size_t numElems,
                              int   &flags,
                              long long &timeNs,
                              long  timeoutUs)
{
    flags  = 0;
    timeNs = 0;

    std::unique_lock<std::mutex> lock(_ringMutex);

    auto waitUntil = std::chrono::steady_clock::now()
                   + std::chrono::microseconds(timeoutUs);
    while (_ringCount < numElems) {
        if (_ringCond.wait_until(lock, waitUntil) == std::cv_status::timeout) {
            if (_ringCount == 0)
                return SOAPY_SDR_TIMEOUT;
            break;
        }
    }

    if (_ringOverflow) {
        flags |= SOAPY_SDR_OVERFLOW;
        _ringOverflow = false;
    }

    size_t toRead = std::min(numElems, _ringCount);

    if (_streamFormat == SOAPY_SDR_CS16) {
        auto *dst = static_cast<int16_t *>(buffs[0]);
        for (size_t i = 0; i < toRead; i++) {
            float I = _ring[_ringHead];
            float Q = _ring[_ringHead + 1];
            dst[i * 2 + 0] = static_cast<int16_t>(std::max(-32768.0f, std::min(32767.0f, I * 32767.0f)));
            dst[i * 2 + 1] = static_cast<int16_t>(std::max(-32768.0f, std::min(32767.0f, Q * 32767.0f)));
            _ringHead = (_ringHead + 2) % _ring.size();
            _ringCount--;
        }
    } else {
        auto *dst = static_cast<float *>(buffs[0]);
        for (size_t i = 0; i < toRead; i++) {
            dst[i * 2 + 0] = _ring[_ringHead];          // I
            dst[i * 2 + 1] = _ring[_ringHead + 1];      // Q
            _ringHead = (_ringHead + 2) % _ring.size();
            _ringCount--;
        }
    }

    return static_cast<int>(toRead);
}

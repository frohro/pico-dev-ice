// Soapy2026SDR.hpp — SoapySDR device class for Pico-Dev-iCE DDC SDR and WWU 2026 SDR.
//
// Hardware:
//   1. Pico-Dev-iCE DDC SDR: RP2040 + iCE40UP5K FPGA (DDC NCO) + UAC1 24-bit Audio (USB CDC/Audio)
//   2. WWU 2026 SDR: RP2040 + PCM1808 stereo ADC + Si5351a LO (USB CDC/Audio)
//
// Audio backend: miniaudio (single-header, cross-platform).
// Serial backend: SerialPort.hpp (termios/Win32).

#pragma once
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Formats.hpp>

#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>

#include "SerialPort.hpp"

// ─── Helper structs ───────────────────────────────────────────────────────────

struct Si5351Params {
    int64_t si5351_hz;       // frequency actually sent to the chip
    int     n;               // output divider
    int64_t a, b, c;         // MSNA multiplier = a + b/c
    int64_t p1, p2, p3;      // register values
};

// ─── Main driver class ────────────────────────────────────────────────────────

class Soapy2026SDR : public SoapySDR::Device
{
public:
    explicit Soapy2026SDR(const SoapySDR::Kwargs &args);
    ~Soapy2026SDR() override;

    /* ── Identification ─────────────────────────────────────────────────── */
    std::string getDriverKey()   const override;
    std::string getHardwareKey() const override;
    SoapySDR::Kwargs getHardwareInfo() const override;

    /* ── Channels ───────────────────────────────────────────────────────── */
    size_t getNumChannels(int direction) const override;

    /* ── Stream ─────────────────────────────────────────────────────────── */
    std::vector<std::string> getStreamFormats(int dir, size_t ch) const override;
    std::string getNativeStreamFormat(int dir, size_t ch,
                                      double &fullScale) const override;
    SoapySDR::Stream *setupStream(int dir,
                                  const std::string &format,
                                  const std::vector<size_t> &channels,
                                  const SoapySDR::Kwargs &args) override;
    void closeStream(SoapySDR::Stream *stream) override;
    size_t getStreamMTU(SoapySDR::Stream *stream) const override;
    int activateStream(SoapySDR::Stream *stream, int flags,
                       long long timeNs, size_t numElems) override;
    int deactivateStream(SoapySDR::Stream *stream, int flags,
                         long long timeNs) override;
    int readStream(SoapySDR::Stream *stream, void *const *buffs,
                   size_t numElems, int &flags, long long &timeNs,
                   long timeoutUs) override;

    /* ── Antenna ────────────────────────────────────────────────────────── */
    std::vector<std::string> listAntennas(int dir, size_t ch) const override;
    void setAntenna(int dir, size_t ch, const std::string &name) override;
    std::string getAntenna(int dir, size_t ch) const override;

    /* ── Gain ───────────────────────────────────────────────────────────── */
    std::vector<std::string> listGains(int dir, size_t ch) const override;
    void setGain(int dir, size_t ch, const std::string &name, double value) override;
    void setGain(int dir, size_t ch, double value) override;
    double getGain(int dir, size_t ch, const std::string &name) const override;
    double getGain(int dir, size_t ch) const override;
    SoapySDR::Range getGainRange(int dir, size_t ch, const std::string &name) const override;
    SoapySDR::Range getGainRange(int dir, size_t ch) const override;

    /* ── Frequency ──────────────────────────────────────────────────────── */
    void setFrequency(int dir, size_t ch, const std::string &name,
                      double freq, const SoapySDR::Kwargs &args) override;
    double getFrequency(int dir, size_t ch,
                        const std::string &name) const override;
    std::vector<std::string> listFrequencies(int dir, size_t ch) const override;
    SoapySDR::RangeList getFrequencyRange(int dir, size_t ch,
                                          const std::string &name) const override;

    /* ── Sample Rate ────────────────────────────────────────────────────── */
    void setSampleRate(int dir, size_t ch, double rate) override;
    double getSampleRate(int dir, size_t ch) const override;
    std::vector<double> listSampleRates(int dir, size_t ch) const override;
    SoapySDR::RangeList getSampleRateRange(int dir, size_t ch) const override;

    /* ── Settings / sensors ─────────────────────────────────────────────── */
    SoapySDR::ArgInfoList getSettingInfo() const override;
    std::string readSetting(const std::string &key) const override;
    void writeSetting(const std::string &key, const std::string &value) override;

    // Called from the miniaudio audio thread (must be public for C callback).
    void _audioCallback(const void *input, unsigned int frameCount);

private:
    /* ── Serial helpers ─────────────────────────────────────────────────── */
    SerialPort   _serial;
    std::string  _serialPath;

    void        _serialWrite(const std::string &line);
    std::string _serialReadLine(int timeoutMs = 500);
    std::string _getParam(const std::string &cmd);
    void        _setParam(const std::string &cmd, const std::string &val);

    /* ── Hardware configuration ─────────────────────────────────────────── */
    bool        _isDDC;             // true = Pico-Dev-iCE FPGA DDC; false = Si5351 Tayloe
    bool        _johnsonMode;       // true = chip outputs 4× RF (Tayloe mode)
    bool        _johnsonAvailable;  // true if v0.2 firmware (MODE,JOHNSON capable)
    double      _crystalFreq;       // Master clock / crystal freq in Hz
    double      _centerFreq;        // Current tuned center frequency in Hz
    std::string _firmwareVersion;   // Firmware version string
    std::string _sdrMode;           // DDC, DIRECT, or JOHNSON
    std::string _pllStatus;         // Return status of tuning command
    uint8_t     _pgaCode;           // PGA attenuator code (0..15)
    int         _refMux;            // REF Multiplexer (0 = RX Antenna, 1 = VNA)

    /* ── DDC / Si5351a Tuning ────────────────────────────────────────────── */
    void         _programFrequencyDDC(double rfHz);
    Si5351Params _computeSi5351(double rfHz) const;
    void         _programSi5351(double rfHz);
    static std::pair<int64_t,int64_t> _limitDenominator(
        int64_t num, int64_t den, int64_t maxDen);

    /* ── miniaudio capture ──────────────────────────────────────────────── */
    void       *_audio;          // AudioImpl*; cast in Streaming.cpp
    double      _sampleRate;
    std::string _audioDeviceName; // e.g. "DDC SDR 2026", "WWU SDR"
    std::string _streamFormat;

    /* ── Ring buffer (audio thread → readStream) ────────────────────────── */
    static constexpr size_t RING_FRAMES = 131072; // ~2.7 s at 48 kHz
    std::vector<float>  _ring;       // interleaved float32 [I,Q,I,Q,...]
    size_t              _ringHead;
    size_t              _ringTail;
    size_t              _ringCount;
    bool                _ringOverflow;
    std::mutex              _ringMutex;
    std::condition_variable _ringCond;

    void _setupAudio();
    void _teardownAudio();
};

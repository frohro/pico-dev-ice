// Settings.cpp — Constructor, DDC & Si5351a frequency control, sample rate, PGA gain, and settings.
//
// Supports both:
//   - Pico-Dev-iCE FPGA DDC SDR (FREQ,<hz>, PGA,<code>, REF,<0|1>, 0-30 MHz)
//   - WWU 2026 Tayloe SDR (Si5351 multi-synth math, 0.5-30 MHz)

#include "Soapy2026SDR.hpp"
#include <SoapySDR/Logger.hpp>
#include <stdexcept>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <sstream>

// ─── Constructor ─────────────────────────────────────────────────────────────

Soapy2026SDR::Soapy2026SDR(const SoapySDR::Kwargs &args)
    : _isDDC(false)
    , _johnsonMode(false)
    , _johnsonAvailable(false)
    , _crystalFreq(30720000.0)
    , _centerFreq(7100000.0)
    , _sdrMode("DDC")
    , _pgaCode(0)
    , _refMux(0)
    , _audio(nullptr)
    , _sampleRate(48000.0)
    , _audioDeviceName("DDC SDR 2026")
    , _ringHead(0), _ringTail(0), _ringCount(0), _ringOverflow(false)
{
    // Audio device name can be overridden via args
    if (args.count("audio_label"))
        _audioDeviceName = args.at("audio_label");

    // ── Open serial port ──────────────────────────────────────────────────
    _serialPath = args.count("serial_port") ? args.at("serial_port") : "";
    if (_serialPath.empty())
        throw std::runtime_error("Soapy2026SDR: no serial_port specified");

    if (!_serial.open(_serialPath, 115200))
        throw std::runtime_error("Soapy2026SDR: cannot open " + _serialPath);

    SoapySDR_logf(SOAPY_SDR_INFO, "2026SDR: opened serial %s", _serialPath.c_str());

    // ── Board reset + ready handshake ─────────────────────────────────────
    char ctrlcd[] = {0x03, 0x04, 0};
    _serial.writeLine(std::string(ctrlcd));
    _serial.flushInput();

    // Wait for "SDR ready"
    for (int i = 0; i < 30; i++) {
        std::string line = _serialReadLine(200);
        if (line.find("SDR ready") != std::string::npos) break;
    }

    // ── Query firmware parameters ─────────────────────────────────────────
    _firmwareVersion = _getParam("VER");
    SoapySDR_logf(SOAPY_SDR_INFO, "2026SDR: firmware %s", _firmwareVersion.c_str());

    std::string xtalStr = _getParam("XTAL");
    double xtal = std::atof(xtalStr.c_str());
    if (xtal > 0.0) _crystalFreq = xtal;
    SoapySDR_logf(SOAPY_SDR_INFO, "2026SDR: master clock %.3f Hz", _crystalFreq);

    std::string modeStr = _getParam("MODE");
    _sdrMode = modeStr.empty() ? "DIRECT" : modeStr;

    if (_sdrMode == "DDC" || _firmwareVersion.find("DDC") != std::string::npos) {
        _isDDC = true;
        if (!args.count("audio_label")) {
            _audioDeviceName = "DDC SDR 2026";
        }
        SoapySDR_logf(SOAPY_SDR_INFO, "2026SDR: detected Pico-Dev-iCE DDC architecture");
    } else {
        _isDDC = false;
        _johnsonMode      = (_sdrMode == "JOHNSON");
        _johnsonAvailable = (_sdrMode == "JOHNSON");
        if (!args.count("audio_label")) {
            _audioDeviceName = "WWU SDR";
        }
        SoapySDR_logf(SOAPY_SDR_INFO, "2026SDR: detected Tayloe architecture, mode %s",
                      _johnsonMode ? "JOHNSON" : "DIRECT");
    }

    // ── Set initial sample rate ───────────────────────────────────────────
    _setParam("RATE", std::to_string((int)_sampleRate));

    // ── Set initial gain & antenna if DDC ──────────────────────────────────
    if (_isDDC) {
        _pgaCode = 0; // 0 = straight path (+40 dB nominal gain)
        _setParam("PGA", "0");
        _refMux = 0;  // 0 = SDR RF Antenna RX
        _setParam("REF", "0");
    }

    // ── Pre-allocate ring buffer ──────────────────────────────────────────
    _ring.assign(RING_FRAMES * 2, 0.0f); // *2 for I+Q per frame

    // ── Initialize miniaudio (sets up _audio) ────────────────────────────
    _setupAudio(); // defined in Streaming.cpp
}

Soapy2026SDR::~Soapy2026SDR()
{
    _teardownAudio(); // defined in Streaming.cpp
    _serial.close();
}

// ─── Identification ───────────────────────────────────────────────────────────

std::string Soapy2026SDR::getDriverKey() const { return "2026SDR"; }

std::string Soapy2026SDR::getHardwareKey() const
{
    if (_isDDC) return "Pico-Dev-iCE-DDC";
    return _johnsonAvailable ? "WWU-2026-v0.2" : "WWU-2026-v0.1";
}

SoapySDR::Kwargs Soapy2026SDR::getHardwareInfo() const
{
    SoapySDR::Kwargs info;
    info["firmware"]    = _firmwareVersion;
    info["crystal_hz"]  = std::to_string(_crystalFreq);
    info["mixer_mode"]  = _sdrMode;
    info["is_ddc"]      = _isDDC ? "yes" : "no";
    info["serial_port"] = _serialPath;
    info["audio_label"] = _audioDeviceName;
    return info;
}

// ─── Channels ────────────────────────────────────────────────────────────────

size_t Soapy2026SDR::getNumChannels(int direction) const
{
    return (direction == SOAPY_SDR_RX) ? 1 : 0;
}

// ─── Antenna ─────────────────────────────────────────────────────────────────

std::vector<std::string> Soapy2026SDR::listAntennas(int, size_t) const
{
    if (_isDDC) {
        return {"RX", "VNA"};
    }
    return {"RX"};
}

void Soapy2026SDR::setAntenna(int, size_t, const std::string &name)
{
    if (!_isDDC) return;
    if (name == "VNA" || name == "vna" || name == "1") {
        _refMux = 1;
        _setParam("REF", "1");
        SoapySDR_logf(SOAPY_SDR_INFO, "2026SDR: REF mux set to VNA (1)");
    } else {
        _refMux = 0;
        _setParam("REF", "0");
        SoapySDR_logf(SOAPY_SDR_INFO, "2026SDR: REF mux set to SDR RF RX (0)");
    }
}

std::string Soapy2026SDR::getAntenna(int, size_t) const
{
    if (_isDDC && _refMux == 1) return "VNA";
    return "RX";
}

// ─── Gain (PGA Digital Step Attenuator for DDC) ──────────────────────────────

std::vector<std::string> Soapy2026SDR::listGains(int, size_t) const
{
    if (_isDDC) {
        return {"PGA"};
    }
    return {};
}

SoapySDR::Range Soapy2026SDR::getGainRange(int, size_t, const std::string &) const
{
    if (_isDDC) {
        return SoapySDR::Range(-15.0, 40.0, 5.0);
    }
    return SoapySDR::Range(0.0, 0.0);
}

SoapySDR::Range Soapy2026SDR::getGainRange(int dir, size_t ch) const
{
    return getGainRange(dir, ch, "PGA");
}

void Soapy2026SDR::setGain(int dir, size_t ch, const std::string &name, double value)
{
    if (!_isDDC || dir != SOAPY_SDR_RX || ch != 0) return;

    // Map nominal dB gain to PGA codes:
    // +40 dB -> 0x0 (straight path, 0 dB attenuation)
    // +35 dB -> 0x1 (5 dB attenuation)
    // +25 dB -> 0x3 (15 dB attenuation)
    // -15 dB -> 0xF (55 dB attenuation)
    uint8_t code = 0x0;
    if (value >= 37.5) {
        code = 0x0; // +40 dB
    } else if (value >= 30.0) {
        code = 0x1; // +35 dB
    } else if (value >= 5.0) {
        code = 0x3; // +25 dB
    } else {
        code = 0xF; // -15 dB
    }

    _pgaCode = code;
    _setParam("PGA", std::to_string((int)code));
    SoapySDR_logf(SOAPY_SDR_INFO, "2026SDR: PGA gain set to %.1f dB (mask 0x%X)",
                  getGain(dir, ch, name), code);
}

void Soapy2026SDR::setGain(int dir, size_t ch, double value)
{
    setGain(dir, ch, "PGA", value);
}

double Soapy2026SDR::getGain(int, size_t, const std::string &) const
{
    if (!_isDDC) return 0.0;
    switch (_pgaCode & 0xF) {
        case 0x0: return 40.0;
        case 0x1: return 35.0;
        case 0x3: return 25.0;
        case 0xF: return -15.0;
        default:  return 40.0 - (double)_pgaCode * 3.5;
    }
}

double Soapy2026SDR::getGain(int dir, size_t ch) const
{
    return getGain(dir, ch, "PGA");
}

// ─── Sample Rate ─────────────────────────────────────────────────────────────

std::vector<double> Soapy2026SDR::listSampleRates(int, size_t) const
{
    return {48000.0, 96000.0};
}

SoapySDR::RangeList Soapy2026SDR::getSampleRateRange(int, size_t) const
{
    return {
        SoapySDR::Range(48000.0, 48000.0),
        SoapySDR::Range(96000.0, 96000.0)
    };
}

double Soapy2026SDR::getSampleRate(int, size_t) const { return _sampleRate; }

void Soapy2026SDR::setSampleRate(int, size_t, double rate)
{
    if (rate != 48000.0 && rate != 96000.0)
        throw std::runtime_error("2026SDR: unsupported sample rate (use 48000 or 96000)");
    if (rate == _sampleRate) return;
    _sampleRate = rate;
    _setParam("RATE", std::to_string((int)rate));
    // Restart audio device at new rate
    _teardownAudio();
    _setupAudio();
    SoapySDR_logf(SOAPY_SDR_INFO, "2026SDR: sample rate set to %.0f Hz", rate);
}

// ─── Frequency ───────────────────────────────────────────────────────────────

std::vector<std::string> Soapy2026SDR::listFrequencies(int, size_t) const
{ return {"RF"}; }

SoapySDR::RangeList Soapy2026SDR::getFrequencyRange(int, size_t,
                                                      const std::string &) const
{
    if (_isDDC) {
        return {SoapySDR::Range(0.0, 30e6)};
    }
    double lo = _johnsonAvailable ? 500e3 : 3.8e6;
    return {SoapySDR::Range(lo, 30e6)};
}

double Soapy2026SDR::getFrequency(int, size_t, const std::string &) const
{ return _centerFreq; }

void Soapy2026SDR::setFrequency(int dir, size_t ch, const std::string &name,
                                 double freq, const SoapySDR::Kwargs &)
{
    if (dir != SOAPY_SDR_RX || ch != 0 || name != "RF") return;

    if (_isDDC) {
        freq = std::max(0.0, std::min(30e6, freq));
        _programFrequencyDDC(freq);
        _centerFreq = freq;
    } else {
        // Clamp to supported Tayloe range
        double lo = _johnsonAvailable ? 500e3 : 3.8e6;
        freq = std::max(lo, std::min(30e6, freq));

        // Auto-switch DIRECT ↔ JOHNSON on v0.2 boards
        if (_johnsonAvailable) {
            bool needJohnson = (freq < 3.8e6);
            if (needJohnson != _johnsonMode) {
                _johnsonMode = needJohnson;
                _setParam("MODE", _johnsonMode ? "JOHNSON" : "DIRECT");
                SoapySDR_logf(SOAPY_SDR_INFO, "2026SDR: switched to %s mode",
                              _johnsonMode ? "JOHNSON" : "DIRECT");
            }
        }

        _programSi5351(freq);
        _centerFreq = freq;
    }
}

// ─── DDC Frequency Programming ───────────────────────────────────────────────

void Soapy2026SDR::_programFrequencyDDC(double rfHz)
{
    uint32_t freqHz = (uint32_t)std::round(rfHz);

    std::ostringstream cmd;
    cmd << "FREQ," << freqHz;
    _serialWrite(cmd.str());

    // Read responses (e.g. "<freqHz>\r\nOK\r\n")
    std::string ok;
    for (int attempt = 0; attempt < 5; attempt++) {
        std::string line = _serialReadLine(300);
        if (line.empty()) continue;
        ok = line;
        if (ok.find("OK") != std::string::npos || ok.find("ERROR") != std::string::npos) break;
    }

    _pllStatus = ok;
    SoapySDR_logf(SOAPY_SDR_INFO, "2026SDR [DDC]: tuned to %u Hz (status: %s)",
                  freqHz, ok.c_str());
}

// ─── Settings / sensors ──────────────────────────────────────────────────────

SoapySDR::ArgInfoList Soapy2026SDR::getSettingInfo() const
{
    SoapySDR::ArgInfoList infos;
    auto make = [](const std::string &k, const std::string &desc, SoapySDR::ArgInfo::Type type) {
        SoapySDR::ArgInfo i;
        i.key = k; i.name = k; i.description = desc;
        i.type = type;
        return i;
    };
    infos.push_back(make("firmware_version", "Firmware version string", SoapySDR::ArgInfo::STRING));
    infos.push_back(make("crystal_freq_hz",  "Master clock frequency (Hz)", SoapySDR::ArgInfo::FLOAT));
    infos.push_back(make("mixer_mode",        "Current SDR mode: DDC, DIRECT, or JOHNSON", SoapySDR::ArgInfo::STRING));
    infos.push_back(make("pga_code",          "PGA digital step attenuator code (0..15)", SoapySDR::ArgInfo::INT));
    infos.push_back(make("ref_mux",           "RF Multiplexer (0=Antenna RX, 1=VNA)", SoapySDR::ArgInfo::INT));
    infos.push_back(make("fpga_status",       "Active FPGA gateware image (RX/TX/DFU)", SoapySDR::ArgInfo::STRING));
    infos.push_back(make("fpga_load",         "Load FPGA image: RX or TX", SoapySDR::ArgInfo::STRING));
    infos.push_back(make("pll_status",        "Last tuning command status from firmware", SoapySDR::ArgInfo::STRING));
    return infos;
}

std::string Soapy2026SDR::readSetting(const std::string &key) const
{
    if (key == "firmware_version") return _firmwareVersion;
    if (key == "crystal_freq_hz")  return std::to_string(_crystalFreq);
    if (key == "mixer_mode")       return _sdrMode;
    if (key == "pga_code")         return std::to_string((int)_pgaCode);
    if (key == "ref_mux")          return std::to_string(_refMux);
    if (key == "fpga_status") {
        if (_isDDC) {
            Soapy2026SDR *self = const_cast<Soapy2026SDR *>(this);
            return self->_getParam("FPGA,STATUS");
        }
        return "N/A";
    }
    if (key == "pll_status")       return _pllStatus;
    return "";
}

void Soapy2026SDR::writeSetting(const std::string &key, const std::string &value)
{
    if (key == "pga_code" && _isDDC) {
        uint8_t code = (uint8_t)std::strtoul(value.c_str(), nullptr, 10);
        _pgaCode = code;
        _setParam("PGA", std::to_string((int)code));
    } else if (key == "ref_mux" && _isDDC) {
        int mux = std::atoi(value.c_str());
        _refMux = mux ? 1 : 0;
        _setParam("REF", std::to_string(_refMux));
    } else if (key == "fpga_load" && _isDDC) {
        _setParam("FPGA,LOAD", value);
    }
}

// ─── Serial helpers ───────────────────────────────────────────────────────────

void Soapy2026SDR::_serialWrite(const std::string &line)
{
    _serial.flushInput();
    _serial.writeLine(line);
}

std::string Soapy2026SDR::_serialReadLine(int timeoutMs)
{
    return _serial.readLine(timeoutMs);
}

std::string Soapy2026SDR::_getParam(const std::string &cmd)
{
    _serialWrite(cmd);
    std::string result;
    // Read lines until we find the value line and terminal "OK"/"ERR"
    for (int attempt = 0; attempt < 10; attempt++) {
        std::string line = _serialReadLine(500);
        if (line.empty()) continue;
        if (line == "OK") {
            if (!result.empty()) return result;
            return "OK";
        }
        if (line == "ERR" || line.find("ERROR") != std::string::npos) {
            return line;
        }
        auto comma = line.find(',');
        if (comma != std::string::npos) {
            result = line.substr(comma + 1);
        } else {
            result = line;
        }
    }
    return result;
}

void Soapy2026SDR::_setParam(const std::string &cmd, const std::string &val)
{
    _serialWrite(cmd + "," + val);
    // Read until OK or ERR
    for (int attempt = 0; attempt < 10; attempt++) {
        std::string line = _serialReadLine(500);
        if (line.empty()) continue;
        if (line.find("OK") != std::string::npos ||
            line.find("ERR") != std::string::npos ||
            line.find("ERROR") != std::string::npos)
            return;
    }
}

// ─── Si5351a math (for Tayloe board) ──────────────────────────────────────────

std::pair<int64_t,int64_t>
Soapy2026SDR::_limitDenominator(int64_t num, int64_t den, int64_t maxDen)
{
    auto gcd = [](int64_t a, int64_t b) -> int64_t {
        if (a < 0) a = -a;
        if (b < 0) b = -b;
        while (b) { a %= b; std::swap(a, b); } return a;
    };
    int64_t g = gcd(num, den);
    if (g > 0) { num /= g; den /= g; }
    if (den <= maxDen) return {num, den};

    int64_t origNum = num, origDen = den;
    int64_t p0=0, q0=1, p1=1, q1=0;
    int64_t n = num, d = den;
    while (true) {
        int64_t a  = n / d;
        int64_t q2 = q0 + a * q1;
        if (q2 > maxDen) break;
        int64_t p2 = p0 + a * p1;
        p0 = p1; q0 = q1;
        p1 = p2; q1 = q2;
        int64_t newN = d; d = n - a * d; n = newN;
    }
    int64_t k  = (maxDen - q0) / q1;
    int64_t pa = p0 + k * p1, qa = q0 + k * q1;
    int64_t pb = p1,          qb = q1;

    double diff_a = std::abs((double)pa * origDen - (double)origNum * qa) * qb;
    double diff_b = std::abs((double)pb * origDen - (double)origNum * qb) * qa;
    return (diff_b <= diff_a) ? std::make_pair(pb, qb)
                              : std::make_pair(pa, qa);
}

Si5351Params Soapy2026SDR::_computeSi5351(double rfHz) const
{
    double si5351d = rfHz * (_johnsonMode ? 4.0 : 1.0);
    int64_t si5351_hz = (int64_t)std::round(si5351d);

    int nStep    = _johnsonMode ? 1 : 2;
    int nMinBase = _johnsonMode ? 4 : 6;
    int nMaxBase = _johnsonMode ? 127 : 126;

    int nMin = std::max(nMinBase, (int)std::ceil(600000000.0 / si5351d));
    int nMax = std::min(nMaxBase, (int)(900000000.0 / si5351d));
    if (!_johnsonMode && nMin % 2 != 0) nMin++;

    if (nMin > nMax) {
        return {si5351_hz, nMin, 0, 0, 1, 0, 0, 1};
    }

    int     bestN = -1;
    int64_t exactM = 0;
    for (int n = nMin; n <= nMax; n += nStep) {
        double mf = si5351d * n / _crystalFreq;
        if (mf <= 14.0 || mf >= 91.0) continue;
        int64_t mi = (int64_t)std::round(mf);
        if (std::abs(mf - (double)mi) < 1e-4) {
            bestN = n; exactM = mi; break;
        }
    }

    int64_t a, b, c;
    int     nVal;
    if (bestN >= 0) {
        nVal = bestN; a = exactM; b = 0; c = 1;
    } else {
        nVal = nMin;
        int64_t num = si5351_hz * (int64_t)nVal;
        int64_t den = (int64_t)std::round(_crystalFreq);
        std::pair<int64_t,int64_t> frac = _limitDenominator(num, den, 1048575LL);
        a = frac.first / frac.second;
        b = frac.first - a * frac.second;
        c = frac.second;
    }

    int64_t floorTerm = (int64_t)std::floor(128.0 * b / c);
    int64_t p1 = 128 * a + floorTerm - 512;
    int64_t p2 = 128 * b - c * floorTerm;
    int64_t p3 = c;

    return {si5351_hz, nVal, a, b, c, p1, p2, p3};
}

void Soapy2026SDR::_programSi5351(double rfHz)
{
    Si5351Params p = _computeSi5351(rfHz);

    std::ostringstream cmd;
    cmd << "FREQ,"
        << p.si5351_hz << ","
        << p.n << ","
        << p.a << ","
        << p.b << ","
        << p.c << ","
        << p.p1 << ","
        << p.p2 << ","
        << p.p3;

    _serialWrite(cmd.str());

    std::string ok;
    for (int attempt = 0; attempt < 3; attempt++) {
        std::string line = _serialReadLine(300);
        if (line.empty()) continue;
        ok = line;
        if (ok.rfind("OK", 0) == 0) break;
    }

    _pllStatus = ok;
    if (ok.rfind("OK", 0) == 0) {
        std::vector<std::string> parts;
        std::istringstream ss(ok);
        std::string tok;
        while (std::getline(ss, tok, ',')) parts.push_back(tok);

        std::string ptype = (parts.size() >= 2) ? parts[1] : "?";
        int64_t offset = (parts.size() >= 3) ? std::atoll(parts[2].c_str()) : 0;

        SoapySDR_logf(SOAPY_SDR_INFO,
            "2026SDR: LO=%.0f Hz  type=%s  offset=%+lld Hz  N=%d  M=%lld+%lld/%lld",
            rfHz, ptype.c_str(), (long long)offset,
            p.n, (long long)p.a, (long long)p.b, (long long)p.c);
    } else {
        SoapySDR_logf(SOAPY_SDR_WARNING,
            "2026SDR: unexpected FREQ reply: '%s'", ok.c_str());
    }
}

// Registration.cpp — SoapySDR discovery + factory for Pico-Dev-iCE DDC SDR and WWU 2026 SDR boards.
//
// Discovery strategy:
//   1. Scan serial ports for:
//      - 0x1209:0xB1C0 (Pico-Dev-iCE DDC SDR)
//      - 0xCAFE:0x4011 / 0xCAFE:0x4010 (RP2040 Pico CDC)
//      Falls back to scanning common ACM/usbmodem/COM paths if enumeration fails.
//   2. Queries firmware version via VER and SDR architecture via MODE.
//   3. Supports both 2026sdr and devicesdr driver keys for maximum compatibility.

#include "Soapy2026SDR.hpp"
#include "SerialPort.hpp"
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Logger.hpp>
#include <vector>
#include <string>
#include <cstdlib>

// Known USB VID:PIDs
static const uint16_t DEVICE_VID   = 0x1209;
static const uint16_t DEVICE_PID   = 0xB1C0; // Pico-Dev-iCE DDC SDR
static const uint16_t PICO_VID     = 0xCAFE;
static const uint16_t PICO_PID1    = 0x4011; // 2026 board standard firmware
static const uint16_t PICO_PID2    = 0x4010; // 2026 board alternate PID

// ─── Device discovery ────────────────────────────────────────────────────────

static std::vector<SoapySDR::Kwargs> find2026SDR(const SoapySDR::Kwargs &args)
{
    std::vector<SoapySDR::Kwargs> results;

    // 1. Try VID:PID enumeration
    std::vector<std::string> ports;
    {
        auto p0 = SerialPort::findPorts(DEVICE_VID, DEVICE_PID);
        auto p1 = SerialPort::findPorts(PICO_VID, PICO_PID1);
        auto p2 = SerialPort::findPorts(PICO_VID, PICO_PID2);
        ports.insert(ports.end(), p0.begin(), p0.end());
        ports.insert(ports.end(), p1.begin(), p1.end());
        ports.insert(ports.end(), p2.begin(), p2.end());
    }

    // 2. If VID/PID enumeration returned nothing, try common device paths
    if (ports.empty()) {
#ifdef _WIN32
        for (int i = 1; i <= 20; i++) {
            ports.push_back("COM" + std::to_string(i));
        }
#elif defined(__APPLE__)
        for (int i = 0; i < 8; i++) {
            ports.push_back("/dev/cu.usbmodem" + std::to_string(i));
        }
#else
        for (int i = 0; i < 8; i++) {
            ports.push_back("/dev/ttyACM" + std::to_string(i));
        }
#endif
    }

    // 3. If caller specified a serial_port override, use only that.
    if (args.count("serial_port")) {
        ports.clear();
        ports.push_back(args.at("serial_port"));
    }

    // 4. For each candidate port, probe and query the firmware.
    for (const auto &path : ports) {
        SerialPort sp;
        if (!sp.open(path, 115200)) continue;

        // Send Ctrl-C / Ctrl-D to abort any partial command, then ask for VER.
        char reset[] = {0x03, 0x04, 0};
        sp.flushInput();
        sp.writeLine(std::string(reset));
        sp.flushInput();

        sp.writeLine("VER");
        std::string reply;
        for (int attempt = 0; attempt < 6; attempt++) {
            std::string line = sp.readLine(400);
            if (line.rfind("VER,", 0) == 0) {
                reply = line;
            }
            if (line == "OK" && !reply.empty()) break;
        }

        if (reply.empty()) {
            sp.close();
            continue; // not our board
        }

        // Query MODE
        sp.flushInput();
        sp.writeLine("MODE");
        std::string modeReply;
        for (int attempt = 0; attempt < 6; attempt++) {
            std::string line = sp.readLine(400);
            if (line.rfind("MODE,", 0) == 0) {
                modeReply = line;
            }
            if (line == "OK" && !modeReply.empty()) break;
        }
        sp.close();

        // Extract version string and mode
        std::string ver = reply.substr(4);
        std::string mode = modeReply.empty() ? "DIRECT" : modeReply.substr(5);

        bool isDDC = (mode == "DDC" || ver.find("DDC") != std::string::npos);

        SoapySDR::Kwargs info;
        info["driver"]      = "2026sdr";
        info["serial_port"] = path;
        info["version"]     = ver;
        info["mode"]        = mode;

        if (isDDC) {
            info["device"] = "DDC SDR 2026";
            info["label"]  = "WWU Dev-iCE DDC SDR (" + path + ")";
            SoapySDR_logf(SOAPY_SDR_INFO, "Found WWU Dev-iCE DDC SDR on %s (fw: %s, mode: %s)",
                          path.c_str(), ver.c_str(), mode.c_str());
        } else {
            info["device"] = "WWU 2026 SDR";
            info["label"]  = "WWU 2026 SDR (" + path + ")";
            SoapySDR_logf(SOAPY_SDR_INFO, "Found WWU 2026 SDR on %s (fw: %s, mode: %s)",
                          path.c_str(), ver.c_str(), mode.c_str());
        }

        // Apply user filters
        if (args.count("label") &&
            info["label"].find(args.at("label")) == std::string::npos)
            continue;
        if (args.count("device") &&
            info["device"].find(args.at("device")) == std::string::npos)
            continue;

        results.push_back(info);
    }

    return results;
}

// ─── Device factory ──────────────────────────────────────────────────────────

static SoapySDR::Device *make2026SDR(const SoapySDR::Kwargs &args)
{
    return new Soapy2026SDR(args);
}

// ─── Registration ────────────────────────────────────────────────────────────

static SoapySDR::Registry register2026SDR(
    "2026sdr", &find2026SDR, &make2026SDR, SOAPY_SDR_ABI_VERSION);

static SoapySDR::Registry registerDeviCESDR(
    "devicesdr", &find2026SDR, &make2026SDR, SOAPY_SDR_ABI_VERSION);

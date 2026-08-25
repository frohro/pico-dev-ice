// SerialPort.hpp — Thin cross-platform serial wrapper for CDC control of the
// Pico-Dev-iCE / WWU 2026 SDR board (RP2040 Pico at 115200 8N1).
//
// Supports: Linux (/dev/ttyACM*), macOS (/dev/cu.usbmodem*), Windows (COM*)
//
// MIT-licensed.

#pragma once
#include <string>
#include <vector>
#include <cstdint>

class SerialPort {
public:
    SerialPort();
    ~SerialPort();

    // Open the port at 115200 8N1 with a 500 ms read timeout.
    bool open(const std::string &path, int baud = 115200);
    void close();
    bool isOpen() const;

    // Write a text line followed by '\n' (or '\r\n').
    void writeLine(const std::string &line);

    // Read one line, stripping CR/LF.
    // Returns "" on timeout or error.
    std::string readLine(int timeoutMs = 500);

    // Discard any pending bytes in the receive buffer.
    void flushInput();

    // Scan the system for serial ports whose USB VID:PID matches.
    // Returns a list of device paths (e.g. "/dev/ttyACM0").
    static std::vector<std::string> findPorts(uint16_t vid, uint16_t pid);

private:
#ifdef _WIN32
    void *_handle; // HANDLE — kept as void* to avoid pulling in windows.h
#else
    int   _fd;
#endif
};

// SerialPort.cpp — Cross-platform implementation
// POSIX (Linux/macOS): termios + select
// Windows: Win32 COMMAPI
//
// Port-enumeration strategy:
//   Linux  — scan /sys/class/tty/*/device/../idVendor|idProduct via sysfs
//   macOS  — IOKit service tree (IOSerialBSDClient + USBDevice)
//   Windows— SetupAPI + DEVPKEY_Device_HardwareIds VID/PID match

#include "SerialPort.hpp"
#include <stdexcept>
#include <cstring>
#include <algorithm>

// ═══════════════════════════════════════════════════════════════════════════════
// POSIX (Linux + macOS)
// ═══════════════════════════════════════════════════════════════════════════════
#ifndef _WIN32

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/time.h>
#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <fstream>

SerialPort::SerialPort() : _fd(-1) {}

SerialPort::~SerialPort() { close(); }

bool SerialPort::open(const std::string &path, int baud)
{
    _fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (_fd < 0) return false;

    // Switch to blocking mode (we use select() for timeout)
    int flags = fcntl(_fd, F_GETFL, 0);
    fcntl(_fd, F_SETFL, flags & ~O_NONBLOCK);

    struct termios tty;
    ::memset(&tty, 0, sizeof tty);
    if (tcgetattr(_fd, &tty) != 0) { ::close(_fd); _fd = -1; return false; }

    speed_t speed = B115200;
    switch (baud) {
        case 9600:   speed = B9600;   break;
        case 57600:  speed = B57600;  break;
        case 115200: speed = B115200; break;
        default:     speed = B115200; break;
    }
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
    tty.c_cflag |= (CREAD | CLOCAL);
    tty.c_lflag  = 0;
    tty.c_oflag  = 0;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL | INLCR);
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(_fd, TCSANOW, &tty) != 0) { ::close(_fd); _fd = -1; return false; }
    tcflush(_fd, TCIFLUSH);
    return true;
}

void SerialPort::close()
{
    if (_fd >= 0) { ::close(_fd); _fd = -1; }
}

bool SerialPort::isOpen() const { return _fd >= 0; }

void SerialPort::writeLine(const std::string &line)
{
    std::string buf = line + "\r\n";
    ssize_t ret = ::write(_fd, buf.c_str(), buf.size());
    (void)ret;
}

std::string SerialPort::readLine(int timeoutMs)
{
    std::string result;
    auto deadline = [&]() -> struct timeval {
        struct timeval tv;
        tv.tv_sec  = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        return tv;
    };

    while (true) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(_fd, &fds);
        struct timeval tv = deadline();
        int ret = select(_fd + 1, &fds, nullptr, nullptr, &tv);
        if (ret <= 0) break;          // timeout or error

        char c;
        if (::read(_fd, &c, 1) != 1) break;
        if (c == '\n') break;
        if (c != '\r') result += c;
    }
    return result;
}

void SerialPort::flushInput()
{
    if (_fd >= 0) tcflush(_fd, TCIFLUSH);
}

// ─── Linux port enumeration via sysfs ────────────────────────────────────────
#ifdef __linux__

static std::string sysfsRead(const std::string &path)
{
    std::ifstream f(path);
    if (!f) return "";
    std::string s;
    std::getline(f, s);
    // strip whitespace
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

std::vector<std::string> SerialPort::findPorts(uint16_t vid, uint16_t pid)
{
    std::vector<std::string> result;
    const std::string ttyDir = "/sys/class/tty";
    DIR *dir = opendir(ttyDir.c_str());
    if (!dir) return result;

    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name.rfind("ttyACM", 0) != 0 && name.rfind("ttyUSB", 0) != 0)
            continue;

        // Walk up from /sys/class/tty/<name>/device to find the USB device
        std::string devLink = ttyDir + "/" + name + "/device";
        // Traverse up to find idVendor/idProduct
        char realPath[4096] = {};
        if (!realpath(devLink.c_str(), realPath)) continue;
        std::string base = realPath;

        // Walk up parent directories looking for idVendor
        bool found = false;
        for (int depth = 0; depth < 6 && !found; depth++) {
            std::string vPath = base + "/idVendor";
            std::string pPath = base + "/idProduct";
            std::string vStr = sysfsRead(vPath);
            std::string pStr = sysfsRead(pPath);
            if (!vStr.empty() && !pStr.empty()) {
                uint16_t v = (uint16_t)strtol(vStr.c_str(), nullptr, 16);
                uint16_t p = (uint16_t)strtol(pStr.c_str(), nullptr, 16);
                if (v == vid && p == pid) {
                    result.push_back("/dev/" + name);
                    found = true;
                }
            }
            // Go up one directory
            size_t slash = base.rfind('/');
            if (slash == std::string::npos) break;
            base = base.substr(0, slash);
        }
    }
    closedir(dir);
    std::sort(result.begin(), result.end());
    return result;
}

// ─── macOS port enumeration via IOKit ────────────────────────────────────────
#elif defined(__APPLE__)
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>
#include <IOKit/usb/IOUSBLib.h>
#include <CoreFoundation/CoreFoundation.h>

std::vector<std::string> SerialPort::findPorts(uint16_t vid, uint16_t pid)
{
    std::vector<std::string> result;

    CFMutableDictionaryRef matchDict =
        IOServiceMatching(kIOSerialBSDServiceValue);
    if (!matchDict) return result;
    CFDictionarySetValue(matchDict,
        CFSTR(kIOSerialBSDTypeKey), CFSTR(kIOSerialBSDModemType));

    io_iterator_t iter = 0;
    if (IOServiceGetMatchingServices(kIOMasterPortDefault, matchDict, &iter)
            != KERN_SUCCESS)
        return result;

    io_object_t service;
    while ((service = IOIteratorNext(iter))) {
        // Get the device path
        CFStringRef pathCF = (CFStringRef)IORegistryEntryCreateCFProperty(
            service, CFSTR(kIODialinDeviceKey), kCFAllocatorDefault, 0);
        if (!pathCF) { IOObjectRelease(service); continue; }
        char path[256] = {};
        CFStringGetCString(pathCF, path, sizeof path, kCFStringEncodingUTF8);
        CFRelease(pathCF);

        // Walk up IORegistry to find USB device with matching VID/PID
        io_registry_entry_t parent = service;
        IOObjectRetain(parent);
        bool found = false;
        for (int depth = 0; depth < 8 && !found; depth++) {
            io_registry_entry_t next = 0;
            if (IORegistryEntryGetParentEntry(parent, kIOServicePlane, &next)
                    != KERN_SUCCESS) break;
            IOObjectRelease(parent);
            parent = next;

            CFNumberRef vRef = (CFNumberRef)IORegistryEntryCreateCFProperty(
                parent, CFSTR(kUSBVendorID), kCFAllocatorDefault, 0);
            CFNumberRef pRef = (CFNumberRef)IORegistryEntryCreateCFProperty(
                parent, CFSTR(kUSBProductID), kCFAllocatorDefault, 0);
            if (vRef && pRef) {
                int v = 0, p = 0;
                CFNumberGetValue(vRef, kCFNumberIntType, &v);
                CFNumberGetValue(pRef, kCFNumberIntType, &p);
                if ((uint16_t)v == vid && (uint16_t)p == pid) {
                    result.push_back(path);
                    found = true;
                }
            }
            if (vRef) CFRelease(vRef);
            if (pRef) CFRelease(pRef);
        }
        IOObjectRelease(parent);
        IOObjectRelease(service);
    }
    IOObjectRelease(iter);
    std::sort(result.begin(), result.end());
    return result;
}
#else
// Fallback: no enumeration
std::vector<std::string> SerialPort::findPorts(uint16_t, uint16_t)
{ return {}; }
#endif // platform

// ═══════════════════════════════════════════════════════════════════════════════
// Windows
// ═══════════════════════════════════════════════════════════════════════════════
#else // _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <regstr.h>
#include <string>
#include <sstream>

SerialPort::SerialPort() : _handle(INVALID_HANDLE_VALUE) {}

SerialPort::~SerialPort() { close(); }

bool SerialPort::open(const std::string &path, int /*baud*/)
{
    std::string devPath = "\\\\.\\" + path;
    HANDLE h = CreateFileA(devPath.c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) { CloseHandle(h); return false; }
    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity   = NOPARITY;
    dcb.fBinary  = TRUE;
    dcb.fParity  = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl  = DTR_CONTROL_DISABLE;
    dcb.fRtsControl  = RTS_CONTROL_DISABLE;
    if (!SetCommState(h, &dcb)) { CloseHandle(h); return false; }

    COMMTIMEOUTS ct = {};
    ct.ReadIntervalTimeout         = 1;
    ct.ReadTotalTimeoutMultiplier  = 1;
    ct.ReadTotalTimeoutConstant    = 500; // ms default
    SetCommTimeouts(h, &ct);

    _handle = h;
    return true;
}

void SerialPort::close()
{
    if (_handle != INVALID_HANDLE_VALUE) {
        CloseHandle((HANDLE)_handle);
        _handle = INVALID_HANDLE_VALUE;
    }
}

bool SerialPort::isOpen() const
{
    return _handle != INVALID_HANDLE_VALUE;
}

void SerialPort::writeLine(const std::string &line)
{
    std::string buf = line + "\r\n";
    DWORD written = 0;
    WriteFile((HANDLE)_handle, buf.c_str(), (DWORD)buf.size(), &written, nullptr);
}

std::string SerialPort::readLine(int timeoutMs)
{
    COMMTIMEOUTS ct = {};
    ct.ReadIntervalTimeout        = 1;
    ct.ReadTotalTimeoutMultiplier = 1;
    ct.ReadTotalTimeoutConstant   = (DWORD)timeoutMs;
    SetCommTimeouts((HANDLE)_handle, &ct);

    std::string result;
    DWORD read = 0;
    char c = 0;
    while (true) {
        if (!ReadFile((HANDLE)_handle, &c, 1, &read, nullptr) || read == 0) break;
        if (c == '\n') break;
        if (c != '\r') result += c;
    }
    return result;
}

void SerialPort::flushInput()
{
    PurgeComm((HANDLE)_handle, PURGE_RXCLEAR);
}

std::vector<std::string> SerialPort::findPorts(uint16_t vid, uint16_t pid)
{
    std::vector<std::string> result;
    char hwIdFilter[64];
    snprintf(hwIdFilter, sizeof hwIdFilter, "VID_%04X&PID_%04X", vid, pid);

    HDEVINFO devInfo = SetupDiGetClassDevsA(
        &GUID_DEVCLASS_PORTS, nullptr, nullptr,
        DIGCF_PRESENT);
    if (devInfo == INVALID_HANDLE_VALUE) return result;

    SP_DEVINFO_DATA devData = {};
    devData.cbSize = sizeof(devData);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &devData); i++) {
        char hwId[512] = {};
        if (!SetupDiGetDeviceRegistryPropertyA(devInfo, &devData,
                SPDRP_HARDWAREID, nullptr,
                (PBYTE)hwId, sizeof hwId, nullptr))
            continue;
        std::string hw(hwId);
        for (auto &c : hw) c = toupper(c);
        char upper[64]; for (size_t k=0;k<strlen(hwIdFilter);k++) upper[k]=toupper(hwIdFilter[k]); upper[strlen(hwIdFilter)]=0;
        if (hw.find(upper) == std::string::npos) continue;

        HKEY hKey = SetupDiOpenDevRegKey(devInfo, &devData,
            DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (hKey == INVALID_HANDLE_VALUE) continue;
        char portName[32] = {};
        DWORD sz = sizeof portName;
        RegQueryValueExA(hKey, "PortName", nullptr, nullptr,
            (LPBYTE)portName, &sz);
        RegCloseKey(hKey);
        if (portName[0]) result.push_back(portName);
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    std::sort(result.begin(), result.end());
    return result;
}

#endif // _WIN32

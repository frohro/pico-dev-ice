#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#define DEFAULT_WIFI_SSID_PRIMARY   "Frohne-Shop-2.4GHz"
#define DEFAULT_WIFI_SSID_SECONDARY "Frohne-2.4GHz"

// Keep the password out of git: create wifi_secrets.h next to this file
// (and add it to .gitignore) containing:
//     #define DEFAULT_WIFI_PASSWORD "your-password"
// If it is absent, or the password is empty, the firmware joins OPEN networks only.
#if defined(__has_include)
#  if __has_include("wifi_secrets.h")
#    include "wifi_secrets.h"
#  endif
#endif
#ifndef DEFAULT_WIFI_PASSWORD
#define DEFAULT_WIFI_PASSWORD       ""
#endif

#define STATIC_FALLBACK_IP          "192.168.1.191"
#define STATIC_FALLBACK_NETMASK     "255.255.255.0"
#define STATIC_FALLBACK_GATEWAY     "192.168.1.1"

#define TCP_CONTROL_PORT            5000

#endif // WIFI_CONFIG_H

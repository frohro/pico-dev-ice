#ifndef NANOVNA_SHELL_H
#define NANOVNA_SHELL_H

#include <stdint.h>

#define VNA_MAX_POINTS          401u
#define VNA_DEFAULT_POINTS      101u
#define VNA_DEFAULT_START_HZ    50000ULL
#define VNA_DEFAULT_STOP_HZ     100000000ULL

#define NANOVNA_VERSION_STR     "0.4.5"

typedef enum {
    VNA_PORT_S11 = 0,
    VNA_PORT_S21 = 1,
    VNA_PORT_BOTH = 2
} vna_port_mode_t;

void nanovna_shell_init(void);
void nanovna_shell_process_line(const char *line);

#endif /* NANOVNA_SHELL_H */

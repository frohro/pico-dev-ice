#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pico/stdio.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#include "fpga_vna.h"
#include "nanovna_shell.h"

#define RX_LINE_MAX 256u

int main(void) {
    stdio_init_all();

    /* Disable CR/LF auto-translation so \r, \n, and \r\n framing stays exact. */
    stdio_set_translate_crlf(&stdio_usb, false);

    fpga_vna_init();
    nanovna_shell_init();

    char line_buf[RX_LINE_MAX];
    size_t line_len = 0;
    bool last_was_cr = false;

    while (true) {
        int ch = getchar_timeout_us(0);
        if (ch == PICO_ERROR_TIMEOUT) {
            tight_loop_contents();
            continue;
        }

        uint8_t c = (uint8_t)ch;

        if (c == '\n' && last_was_cr) {
            last_was_cr = false;
            continue;
        }

        if (c == '\r' || c == '\n') {
            last_was_cr = (c == '\r');
            line_buf[line_len] = '\0';
            nanovna_shell_process_line(line_buf);
            line_len = 0;
            continue;
        }

        last_was_cr = false;

        if (c == '\b' || c == 0x7Fu) {
            if (line_len > 0) {
                line_len--;
            }
            continue;
        }

        if (c >= 0x20u && c < 0x7Fu) {
            if (line_len < (RX_LINE_MAX - 1u)) {
                line_buf[line_len++] = (char)c;
            }
        }
    }

    return 0;
}

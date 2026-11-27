#include "nanovna_shell.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdio.h"

#include "fpga_vna.h"

static uint64_t g_sweep_start_hz = VNA_DEFAULT_START_HZ;
static uint64_t g_sweep_stop_hz  = VNA_DEFAULT_STOP_HZ;
static uint32_t g_sweep_points   = VNA_DEFAULT_POINTS;

static uint64_t g_freqs[VNA_MAX_POINTS];
static vna_complex_t g_s11[VNA_MAX_POINTS];
static vna_complex_t g_s21[VNA_MAX_POINTS];

static vna_port_mode_t g_port_mode = VNA_PORT_BOTH;
static uint16_t g_integ_cycles     = VNA_DEFAULT_INTEG_CYCLES;
static bool g_paused               = false;

static int str_casecmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) {
            return ca - cb;
        }
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

static bool parse_u64(const char *s, uint64_t *out) {
    if (!s || *s == '\0' || *s == '-') {
        return false;
    }
    char *end = NULL;
    unsigned long long val = strtoull(s, &end, 10);
    if (end == s || *end != '\0') {
        return false;
    }
    *out = (uint64_t)val;
    return true;
}

static bool parse_u32_auto(const char *s, uint32_t *out) {
    if (!s || *s == '\0' || *s == '-') {
        return false;
    }
    char *end = NULL;
    unsigned long val = 0;

    if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        if (s[2] == '\0') {
            return false;
        }
        val = strtoul(s + 2, &end, 2);
        if (end == s + 2 || *end != '\0') {
            return false;
        }
    } else {
        val = strtoul(s, &end, 0);
        if (end == s || *end != '\0') {
            return false;
        }
    }

    *out = (uint32_t)val;
    return true;
}

static uint32_t clamp_points(uint32_t pts) {
    if (pts < 1u) {
        return 1u;
    }
    if (pts > VNA_MAX_POINTS) {
        return VNA_MAX_POINTS;
    }
    return pts;
}

static void populate_freq_grid(uint64_t start_hz, uint64_t stop_hz, uint32_t points) {
    points = clamp_points(points);
    if (points == 1u) {
        g_freqs[0] = start_hz;
        return;
    }
    for (uint32_t i = 0; i < points; i++) {
        if (i == points - 1u) {
            g_freqs[i] = stop_hz;
        } else if (stop_hz >= start_hz) {
            double step = ((double)(stop_hz - start_hz) * (double)i) / (double)(points - 1u);
            g_freqs[i] = start_hz + (uint64_t)llround(step);
        } else {
            double step = ((double)(start_hz - stop_hz) * (double)i) / (double)(points - 1u);
            g_freqs[i] = start_hz - (uint64_t)llround(step);
        }
    }
}

static void run_sweep(uint64_t start_hz, uint64_t stop_hz, uint32_t points) {
    points = clamp_points(points);
    g_sweep_start_hz = start_hz;
    g_sweep_stop_hz  = stop_hz;
    g_sweep_points   = points;

    populate_freq_grid(start_hz, stop_hz, points);

    for (uint32_t i = 0; i < points; i++) {
        vna_complex_t iq = {0.0f, 0.0f};
        fpga_vna_measure(g_freqs[i], g_integ_cycles, &iq, NULL);

        switch (g_port_mode) {
            case VNA_PORT_S11:
                g_s11[i] = iq;
                g_s21[i].re = 0.0f;
                g_s21[i].im = 0.0f;
                break;
            case VNA_PORT_S21:
                g_s11[i].re = 0.0f;
                g_s11[i].im = 0.0f;
                g_s21[i] = iq;
                break;
            case VNA_PORT_BOTH:
            default:
                g_s11[i] = iq;
                g_s21[i] = iq;
                break;
        }
    }
}

static const char *port_mode_str(vna_port_mode_t mode) {
    switch (mode) {
        case VNA_PORT_S11:  return "s11";
        case VNA_PORT_S21:  return "s21";
        case VNA_PORT_BOTH:
        default:            return "both";
    }
}

static void cmd_version(void) {
    printf("%s\r\n", NANOVNA_VERSION_STR);
}

static void cmd_info(void) {
    printf("Board: NanoVNA-H (Pico-Dev-iCE VNA)\r\n");
    printf("Version: %s\r\n", NANOVNA_VERSION_STR);
    printf("ADC: 30.72M, IF: 12k\r\n");
    printf("DAC: %.2fMHz\r\n", fpga_vna_get_dac_clk_hz() / 1.0e6);
    printf("Integ: %u cycles\r\n", (unsigned)g_integ_cycles);
    printf("Port: %s\r\n", port_mode_str(g_port_mode));
}

static void cmd_help(void) {
    printf("Commands: scan data frequencies freq sweep pause resume info version port integ dacclk\r\n");
}

static void cmd_scan(int argc, char *argv[]) {
    if (argc < 3) {
        printf("usage: scan <start_hz> <stop_hz> [points] [outmask]\r\n");
        return;
    }

    uint64_t start_hz = 0;
    uint64_t stop_hz = 0;
    if (!parse_u64(argv[1], &start_hz) || !parse_u64(argv[2], &stop_hz)) {
        printf("error: invalid frequency\r\n");
        return;
    }

    uint32_t points = g_sweep_points;
    if (argc >= 4) {
        uint32_t parsed_pts = 0;
        if (!parse_u32_auto(argv[3], &parsed_pts)) {
            printf("error: invalid points\r\n");
            return;
        }
        points = clamp_points(parsed_pts);
    }

    bool has_outmask = false;
    uint32_t outmask = 0;
    if (argc >= 5) {
        if (!parse_u32_auto(argv[4], &outmask)) {
            printf("error: invalid outmask\r\n");
            return;
        }
        has_outmask = true;
    }

    run_sweep(start_hz, stop_hz, points);

    if (!has_outmask || (outmask & 0x07u) == 0u) {
        return;
    }

    for (uint32_t i = 0; i < g_sweep_points; i++) {
        bool need_space = false;

        if (outmask & 0x01u) {
            printf("%llu", (unsigned long long)g_freqs[i]);
            need_space = true;
        }
        if (outmask & 0x02u) {
            if (need_space) {
                putchar(' ');
            }
            printf("%.6f %.6f", (double)g_s11[i].re, (double)g_s11[i].im);
            need_space = true;
        }
        if (outmask & 0x04u) {
            if (need_space) {
                putchar(' ');
            }
            printf("%.6f %.6f", (double)g_s21[i].re, (double)g_s21[i].im);
        }
        printf("\r\n");
    }
}

static void cmd_sweep(int argc, char *argv[]) {
    if (argc == 1) {
        printf("%llu %llu %lu\r\n",
               (unsigned long long)g_sweep_start_hz,
               (unsigned long long)g_sweep_stop_hz,
               (unsigned long)g_sweep_points);
        return;
    }

    if (str_casecmp(argv[1], "start") == 0 && argc >= 3) {
        uint64_t v = 0;
        if (parse_u64(argv[2], &v)) {
            g_sweep_start_hz = v;
            populate_freq_grid(g_sweep_start_hz, g_sweep_stop_hz, g_sweep_points);
        }
        return;
    }
    if (str_casecmp(argv[1], "stop") == 0 && argc >= 3) {
        uint64_t v = 0;
        if (parse_u64(argv[2], &v)) {
            g_sweep_stop_hz = v;
            populate_freq_grid(g_sweep_start_hz, g_sweep_stop_hz, g_sweep_points);
        }
        return;
    }
    if (str_casecmp(argv[1], "points") == 0 && argc >= 3) {
        uint32_t pts = 0;
        if (parse_u32_auto(argv[2], &pts)) {
            g_sweep_points = clamp_points(pts);
            populate_freq_grid(g_sweep_start_hz, g_sweep_stop_hz, g_sweep_points);
        }
        return;
    }
    if (str_casecmp(argv[1], "center") == 0 && argc >= 3) {
        uint64_t center = 0;
        if (parse_u64(argv[2], &center)) {
            uint64_t span = (g_sweep_stop_hz > g_sweep_start_hz)
                                ? (g_sweep_stop_hz - g_sweep_start_hz)
                                : 0ULL;
            g_sweep_start_hz = (center > span / 2ULL) ? (center - span / 2ULL) : 0ULL;
            g_sweep_stop_hz  = center + span / 2ULL;
            populate_freq_grid(g_sweep_start_hz, g_sweep_stop_hz, g_sweep_points);
        }
        return;
    }
    if (str_casecmp(argv[1], "span") == 0 && argc >= 3) {
        uint64_t span = 0;
        if (parse_u64(argv[2], &span)) {
            uint64_t center = (g_sweep_start_hz + g_sweep_stop_hz) / 2ULL;
            g_sweep_start_hz = (center > span / 2ULL) ? (center - span / 2ULL) : 0ULL;
            g_sweep_stop_hz  = center + span / 2ULL;
            populate_freq_grid(g_sweep_start_hz, g_sweep_stop_hz, g_sweep_points);
        }
        return;
    }

    uint64_t start_hz = g_sweep_start_hz;
    uint64_t stop_hz  = g_sweep_stop_hz;
    uint32_t points   = g_sweep_points;

    if (argc >= 2 && parse_u64(argv[1], &start_hz)) {
        g_sweep_start_hz = start_hz;
    }
    if (argc >= 3 && parse_u64(argv[2], &stop_hz)) {
        g_sweep_stop_hz = stop_hz;
    }
    if (argc >= 4 && parse_u32_auto(argv[3], &points)) {
        g_sweep_points = clamp_points(points);
    }
    populate_freq_grid(g_sweep_start_hz, g_sweep_stop_hz, g_sweep_points);
}

static void cmd_frequencies(void) {
    for (uint32_t i = 0; i < g_sweep_points; i++) {
        printf("%llu\r\n", (unsigned long long)g_freqs[i]);
    }
}

static void cmd_data(int argc, char *argv[]) {
    uint32_t ch = 0u;
    if (argc >= 2) {
        if (!parse_u32_auto(argv[1], &ch)) {
            ch = 0u;
        }
    }

    const vna_complex_t *buf = (ch == 1u) ? g_s21 : g_s11;
    for (uint32_t i = 0; i < g_sweep_points; i++) {
        printf("%.6f %.6f\r\n", (double)buf[i].re, (double)buf[i].im);
    }
}

static void cmd_freq(int argc, char *argv[]) {
    if (argc < 2) {
        printf("%llu\r\n", (unsigned long long)g_sweep_start_hz);
        return;
    }
    uint64_t f_hz = 0;
    if (!parse_u64(argv[1], &f_hz)) {
        printf("error: invalid frequency\r\n");
        return;
    }
    g_sweep_start_hz = f_hz;
    g_sweep_stop_hz  = f_hz;
    vna_complex_t iq = {0.0f, 0.0f};
    fpga_vna_measure(f_hz, g_integ_cycles, &iq, NULL);
}

static void cmd_port(int argc, char *argv[]) {
    if (argc < 2) {
        printf("%s\r\n", port_mode_str(g_port_mode));
        return;
    }
    if (str_casecmp(argv[1], "s11") == 0) {
        g_port_mode = VNA_PORT_S11;
    } else if (str_casecmp(argv[1], "s21") == 0) {
        g_port_mode = VNA_PORT_S21;
    } else if (str_casecmp(argv[1], "both") == 0) {
        g_port_mode = VNA_PORT_BOTH;
    } else {
        printf("usage: port [s11|s21|both]\r\n");
    }
}

static void cmd_integ(int argc, char *argv[]) {
    if (argc < 2) {
        printf("%u\r\n", (unsigned)g_integ_cycles);
        return;
    }
    uint32_t cycles = 0u;
    if (!parse_u32_auto(argv[1], &cycles) || cycles < 1u || cycles > 65535u) {
        printf("error: integ must be 1..65535\r\n");
        return;
    }
    g_integ_cycles = (uint16_t)cycles;
}

static void cmd_dacclk(int argc, char *argv[]) {
    if (argc < 2) {
        printf("%.2fMHz\r\n", fpga_vna_get_dac_clk_hz() / 1.0e6);
        return;
    }
    if (strcmp(argv[1], "30") == 0 || strcmp(argv[1], "30.72") == 0) {
        fpga_vna_set_dac_clk(VNA_DAC_30M72);
    } else if (strcmp(argv[1], "122") == 0 || strcmp(argv[1], "122.88") == 0) {
        fpga_vna_set_dac_clk(VNA_DAC_122M88);
    } else {
        printf("usage: dacclk [30|122]\r\n");
    }
}

void nanovna_shell_init(void) {
    g_sweep_start_hz = VNA_DEFAULT_START_HZ;
    g_sweep_stop_hz  = VNA_DEFAULT_STOP_HZ;
    g_sweep_points   = VNA_DEFAULT_POINTS;
    g_port_mode      = VNA_PORT_BOTH;
    g_integ_cycles   = VNA_DEFAULT_INTEG_CYCLES;
    g_paused         = false;

    populate_freq_grid(g_sweep_start_hz, g_sweep_stop_hz, g_sweep_points);
    memset(g_s11, 0, sizeof(g_s11));
    memset(g_s21, 0, sizeof(g_s21));
}

void nanovna_shell_process_line(const char *line) {
    if (!line || line[0] == '\0') {
        printf("ch> ");
        stdio_flush();
        return;
    }

    printf("%s\r\n", line);

    char work[256];
    strncpy(work, line, sizeof(work) - 1u);
    work[sizeof(work) - 1u] = '\0';

    char *argv[12];
    int argc = 0;
    char *p = work;

    while (*p != '\0' && argc < (int)(sizeof(argv) / sizeof(argv[0]))) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
        if (*p != '\0') {
            *p++ = '\0';
        }
    }

    if (argc > 0) {
        const char *cmd = argv[0];

        if (str_casecmp(cmd, "version") == 0) {
            cmd_version();
        } else if (str_casecmp(cmd, "info") == 0) {
            cmd_info();
        } else if (str_casecmp(cmd, "help") == 0) {
            cmd_help();
        } else if (str_casecmp(cmd, "scan") == 0) {
            cmd_scan(argc, argv);
        } else if (str_casecmp(cmd, "sweep") == 0) {
            cmd_sweep(argc, argv);
        } else if (str_casecmp(cmd, "frequencies") == 0) {
            cmd_frequencies();
        } else if (str_casecmp(cmd, "data") == 0) {
            cmd_data(argc, argv);
        } else if (str_casecmp(cmd, "freq") == 0) {
            cmd_freq(argc, argv);
        } else if (str_casecmp(cmd, "pause") == 0) {
            g_paused = true;
        } else if (str_casecmp(cmd, "resume") == 0) {
            g_paused = false;
        } else if (str_casecmp(cmd, "cal") == 0 ||
                   str_casecmp(cmd, "save") == 0 ||
                   str_casecmp(cmd, "recall") == 0 ||
                   str_casecmp(cmd, "clearconfig") == 0 ||
                   str_casecmp(cmd, "edelay") == 0) {
            (void)g_paused;
        } else if (str_casecmp(cmd, "port") == 0) {
            cmd_port(argc, argv);
        } else if (str_casecmp(cmd, "integ") == 0) {
            cmd_integ(argc, argv);
        } else if (str_casecmp(cmd, "dacclk") == 0) {
            cmd_dacclk(argc, argv);
        } else {
            printf("%s?\r\n", cmd);
        }
    }

    printf("ch> ");
    stdio_flush();
}

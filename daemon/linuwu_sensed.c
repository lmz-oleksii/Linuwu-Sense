/*
 * linuwu_sensed.c — Fan curve daemon for Linuwu-Sense kernel module
 *
 * Reads a YAML config with CPU/GPU fan curves and temperature thresholds,
 * polls hwmon sensors, interpolates the curve with hysteresis, and writes
 * the target fan speed to the linuwu_sense sysfs interface.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <stdarg.h>
#include <errno.h>
#include <dirent.h>
#include <getopt.h>
#include <syslog.h>
#include <sys/stat.h>
#include <yaml.h>

/* ─── compile-time defaults ─────────────────────────────────────────────── */

#define DEFAULT_CONFIG      "/etc/linuwu-sense-daemon.yaml"
#define MAX_CURVE_POINTS    16
#define SYSFS_BASE          "/sys/module/linuwu_sense/drivers/platform:acer-wmi/acer-wmi"
#define HWMON_BASE          "/sys/class/hwmon"

/*
 * fan: -1 in a curve means "hands-off" (hand-controlled mode/STOP mode):
 * the daemon will not write anything for that channel and lets
 * the user or BIOS control it directly, or forces the driver into STOP mode.
 */
#define FAN_HANDS_OFF  (-1)

/*
 * linuwu_sense registers its hwmon device with chip name "acer".
 * Channel mapping (matches acer_wmi_temp_channel_to_sensor_id in the module):
 *   temp1_input -> CPU temperature
 *   temp2_input -> GPU temperature
 */
#define HWMON_CPU_TEMP_IDX  1
#define HWMON_GPU_TEMP_IDX  2

/* ─── data structures ────────────────────────────────────────────────────── */

typedef struct {
    int temp; /* °C  */
    int fan;  /* 0-100 (0 = auto) */
} curve_point_t;

typedef struct {
    curve_point_t points[MAX_CURVE_POINTS];
    int           count;
} curve_t;

typedef struct {
    int     polling_interval;   /* seconds                   */
    int     hysteresis;         /* °C drop needed to go down */
    curve_t cpu;
    curve_t gpu;
} config_t;

/* ─── globals ────────────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_running = 1;
static bool                  g_use_syslog = false;
static bool                  g_dry_run    = false;

/* ─── logging helpers ────────────────────────────────────────────────────── */

#define LS_INFO(fmt, ...)  log_msg(LOG_INFO,    fmt, ##__VA_ARGS__)
#define LS_WARN(fmt, ...)  log_msg(LOG_WARNING, fmt, ##__VA_ARGS__)
#define LS_ERR(fmt, ...)   log_msg(LOG_ERR,     fmt, ##__VA_ARGS__)

static void log_msg(int priority, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_use_syslog) {
        vsyslog(priority, fmt, ap);
    } else {
        FILE *out = (priority <= LOG_WARNING) ? stderr : stdout;
        vfprintf(out, fmt, ap);
        fprintf(out, "\n");
    }
    va_end(ap);
}

/* ─── signal handler ─────────────────────────────────────────────────────── */

static void handle_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ─── sysfs / hwmon helpers ──────────────────────────────────────────────── */

/*
 * Read a single integer from a sysfs file.
 * Returns 0 on success, -1 on error.
 */
static int sysfs_read_int(const char *path, int *out)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int r = fscanf(f, "%d", out);
    fclose(f);
    return (r == 1) ? 0 : -1;
}

/*
 * Write a formatted string to a sysfs file.
 */
static int sysfs_write(const char *path, const char *fmt, ...)
{
    if (g_dry_run) {
        char buf[256];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        LS_INFO("[dry-run] would write '%s' -> %s", buf, path);
        return 0;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        LS_ERR("Cannot open %s for writing: %s", path, strerror(errno));
        return -1;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
    return 0;
}

/*
 * Find hwmon directory that matches one of the given chip names.
 * Finds the hwmon directory for the 'acer' chip.
 * Writes the result into `result` (must be at least PATH_MAX bytes).
 * Returns 0 on success, -1 if not found.
 */
static int find_acer_hwmon_dir(char *result, size_t result_len)
{
    DIR *d = opendir(HWMON_BASE);
    if (!d) {
        LS_ERR("Cannot open %s: %s", HWMON_BASE, strerror(errno));
        return -1;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "hwmon", 5) != 0)
            continue;

        char name_path[512];
        snprintf(name_path, sizeof(name_path),
                 HWMON_BASE "/%s/name", ent->d_name);

        FILE *f = fopen(name_path, "r");
        if (!f)
            continue;

        char chip_name[64] = {0};
        if (fscanf(f, "%63s", chip_name) != 1)
                chip_name[0] = '\0';
        fclose(f);

        if (strcmp(chip_name, "acer") == 0) {
            snprintf(result, result_len,
                     HWMON_BASE "/%s", ent->d_name);
            closedir(d);
            return 0;
        }
    }

    closedir(d);
    return -1;
}

/*
 * Read the temperature from a specific hwmon channel (e.g., temp1_input).
 * Returns the value in °C, or -1 if the channel cannot be read.
 */
static int read_hwmon_temp_channel(const char *hwmon_dir, int channel_idx)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/temp%d_input", hwmon_dir, channel_idx);

    int millideg = 0;
    if (sysfs_read_int(path, &millideg) == 0) {
        return millideg / 1000;
    }

    return -1;
}

/*
 * Locate the fan_speed sysfs file exposed by linuwu_sense.
 * Tries predator_sense first, then nitro_sense.
 */
static int find_fan_speed_path(char *out, size_t out_len)
{
    const char *suffixes[] = { "predator_sense", "nitro_sense", NULL };

    for (int i = 0; suffixes[i]; i++) {
        snprintf(out, out_len, "%s/%s/fan_speed", SYSFS_BASE, suffixes[i]);
        if (access(out, W_OK) == 0)
            return 0;
    }

    return -1;
}

/* ─── curve interpolation ────────────────────────────────────────────────── */

/*
 * Determine fan speed for a given temperature using a step curve.
 * If temp < first point  → return first point fan value.
 * If temp >= last point  → return last  point fan value.
 * For temperatures between two points, the lower point's fan value is used
 * (i.e. fan speed only increases when the next temperature threshold is reached).
 * If a lower breakpoint has fan == FAN_HANDS_OFF, the whole range up to
 * the next real point is also FAN_HANDS_OFF.
 */
static int interpolate_curve(const curve_t *curve, int temp)
{
    if (curve->count == 0)
        return 0;

    /* Below first point */
    if (temp < curve->points[0].temp)
        return curve->points[0].fan;

    /* Above or exactly at last point */
    if (temp >= curve->points[curve->count - 1].temp)
        return curve->points[curve->count - 1].fan;

    /* Find the active step */
    for (int i = 0; i < curve->count - 1; i++) {
        const curve_point_t *lo = &curve->points[i];
        const curve_point_t *hi = &curve->points[i + 1];

        if (temp >= lo->temp && temp < hi->temp) {
            /* If the lower bound is hands-off, stay hands-off */
            if (lo->fan == FAN_HANDS_OFF)
                return FAN_HANDS_OFF;

            return lo->fan;
        }
    }

    return curve->points[curve->count - 1].fan;
}

/*
 * Apply hysteresis. Returns FAN_HANDS_OFF unchanged.
 * When exiting hands-off (current==-1, new>=0), take control immediately.
 */
static int apply_hysteresis(int current_fan, int new_fan,
                            int temp, int *last_set_temp,
                            int hysteresis)
{
    /* Hands-off: daemon stays silent for this channel */
    if (new_fan == FAN_HANDS_OFF) {
        /* Reset tracking so we take control immediately when we exit */
        *last_set_temp = 0;
        return FAN_HANDS_OFF;
    }

    /* Exiting hands-off: take control immediately */
    if (current_fan == FAN_HANDS_OFF) {
        *last_set_temp = temp;
        return new_fan;
    }

    if (new_fan > current_fan) {
        /* Always increase immediately */
        *last_set_temp = temp;
        return new_fan;
    }

    if (new_fan < current_fan) {
        /* Decrease only if we dropped far enough below the last set point */
        if (temp <= *last_set_temp - hysteresis) {
            *last_set_temp = temp;
            return new_fan;
        }
        return current_fan; /* hold the current speed */
    }

    return current_fan; /* unchanged */
}

/* ─── YAML config parser ─────────────────────────────────────────────────── */

typedef enum {
    PARSE_ROOT,
    PARSE_CPU,
    PARSE_GPU,
    PARSE_CPU_CURVE,
    PARSE_GPU_CURVE,
    PARSE_CURVE_ENTRY,
} parse_state_t;

static int parse_config(const char *path, config_t *cfg)
{
    /* Sane defaults */
    cfg->polling_interval = 3;
    cfg->hysteresis       = 5;
    cfg->cpu.count        = 0;
    cfg->gpu.count        = 0;

    FILE *f = fopen(path, "r");
    if (!f) {
        LS_ERR("Cannot open config %s: %s", path, strerror(errno));
        return -1;
    }

    yaml_parser_t parser;
    yaml_event_t  event;

    if (!yaml_parser_initialize(&parser)) {
        LS_ERR("Failed to initialize YAML parser");
        fclose(f);
        return -1;
    }
    yaml_parser_set_input_file(&parser, f);

    parse_state_t state      = PARSE_ROOT;
    char          last_key[64] = "";
    curve_t      *active_curve = NULL;
    int           entry_temp   = -1;
    int           entry_fan    = -1;
    bool          in_entry     = false;
    bool          done         = false;
    int           ret          = 0;

#define NEXT_EVENT() \
    do { if (!yaml_parser_parse(&parser, &event)) { \
        LS_ERR("YAML parse error at line %zu: %s", \
                parser.problem_mark.line + 1, parser.problem ? parser.problem : "?"); \
        ret = -1; goto cleanup; } } while (0)

    while (!done) {
        NEXT_EVENT();

        switch (event.type) {
        case YAML_STREAM_END_EVENT:
        case YAML_DOCUMENT_END_EVENT:
            done = true;
            break;

        case YAML_SCALAR_EVENT: {
            const char *val = (const char *)event.data.scalar.value;

            if (state == PARSE_ROOT) {
                if (strcmp(val, "cpu") == 0) {
                    state = PARSE_CPU;
                } else if (strcmp(val, "gpu") == 0) {
                    state = PARSE_GPU;
                } else if (strcmp(val, "polling_interval") == 0) {
                    strncpy(last_key, val, sizeof(last_key) - 1);
                } else if (strcmp(val, "hysteresis") == 0) {
                    strncpy(last_key, val, sizeof(last_key) - 1);
                } else if (last_key[0]) {
                    if (strcmp(last_key, "polling_interval") == 0)
                        cfg->polling_interval = atoi(val);
                    else if (strcmp(last_key, "hysteresis") == 0)
                        cfg->hysteresis = atoi(val);
                    last_key[0] = '\0';
                }
            } else if (state == PARSE_CPU || state == PARSE_CPU_CURVE ||
                       state == PARSE_GPU || state == PARSE_GPU_CURVE) {
                if (strcmp(val, "curve") == 0) {
                    active_curve = (state == PARSE_CPU || state == PARSE_CPU_CURVE)
                                   ? &cfg->cpu : &cfg->gpu;
                    state = (active_curve == &cfg->cpu)
                            ? PARSE_CPU_CURVE : PARSE_GPU_CURVE;
                } else {
                    strncpy(last_key, val, sizeof(last_key) - 1);
                }
            } else if (state == PARSE_CURVE_ENTRY) {
                if (strcmp(val, "temp") == 0 || strcmp(val, "fan") == 0) {
                    strncpy(last_key, val, sizeof(last_key) - 1);
                } else if (last_key[0]) {
                    if (strcmp(last_key, "temp") == 0)
                        entry_temp = atoi(val);
                    else if (strcmp(last_key, "fan") == 0)
                        entry_fan = atoi(val);
                    last_key[0] = '\0';
                }
            }
            break;
        }

        case YAML_MAPPING_START_EVENT:
            if (state == PARSE_CPU_CURVE || state == PARSE_GPU_CURVE) {
                state     = PARSE_CURVE_ENTRY;
                in_entry  = true;
                entry_temp = -1;
                entry_fan  = -1;
            }
            break;

        case YAML_MAPPING_END_EVENT:
            if (state == PARSE_CURVE_ENTRY && in_entry) {
                if (entry_temp >= 0 && entry_fan >= -1 && active_curve &&
                    active_curve->count < MAX_CURVE_POINTS) {
                    active_curve->points[active_curve->count].temp = entry_temp;
                    active_curve->points[active_curve->count].fan  = entry_fan;
                    active_curve->count++;
                }
                in_entry = false;
                /* back to curve-list state */
                state = (active_curve == &cfg->cpu)
                        ? PARSE_CPU_CURVE : PARSE_GPU_CURVE;
            } else if (state == PARSE_CPU || state == PARSE_CPU_CURVE) {
                state = PARSE_ROOT;
                active_curve = NULL;
            } else if (state == PARSE_GPU || state == PARSE_GPU_CURVE) {
                state = PARSE_ROOT;
                active_curve = NULL;
            }
            break;

        case YAML_SEQUENCE_END_EVENT:
            if (state == PARSE_CPU_CURVE || state == PARSE_GPU_CURVE) {
                state = (active_curve == &cfg->cpu) ? PARSE_CPU : PARSE_GPU;
            }
            break;

        default:
            break;
        }

        yaml_event_delete(&event);
    }

cleanup:
    yaml_parser_delete(&parser);
    fclose(f);
    return ret;
}

/* ─── daemon main loop ───────────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "Options:\n"
        "  -c, --config PATH    Config file path (default: " DEFAULT_CONFIG ")\n"
        "  -n, --dry-run        Print actions but do not write to sysfs\n"
        "  -s, --syslog         Log to syslog instead of stderr\n"
        "  -h, --help           Show this help\n",
        prog);
}

int main(int argc, char **argv)
{
    const char *config_path = DEFAULT_CONFIG;

    static const struct option long_opts[] = {
        { "config",  required_argument, NULL, 'c' },
        { "dry-run", no_argument,       NULL, 'n' },
        { "syslog",  no_argument,       NULL, 's' },
        { "help",    no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:nsh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'c': config_path = optarg;  break;
        case 'n': g_dry_run    = true;   break;
        case 's': g_use_syslog = true;   break;
        case 'h': print_usage(argv[0]);  return 0;
        default:  print_usage(argv[0]);  return 1;
        }
    }

    if (g_use_syslog)
        openlog("linuwu-sensed", LOG_PID | LOG_NDELAY, LOG_DAEMON);

    signal(SIGTERM, handle_signal);
    signal(SIGINT,  handle_signal);
    signal(SIGHUP,  handle_signal); /* re-read config on SIGHUP in future */

    /* ── load config ─────────────────────────────────────────────────────── */
    config_t cfg;
    if (parse_config(config_path, &cfg) != 0) {
        LS_ERR("Failed to load config from %s", config_path);
        return 1;
    }

    LS_INFO("linuwu-sensed started (config=%s, interval=%ds, hysteresis=%d C)",
             config_path, cfg.polling_interval, cfg.hysteresis);

    if (cfg.cpu.count == 0)
        LS_WARN("CPU fan curve has no points - CPU fan will not be controlled");
    if (cfg.gpu.count == 0)
        LS_WARN("GPU fan curve has no points - GPU fan will not be controlled");

    /* ── discover hardware paths ─────────────────────────────────────────── */
    char acer_hwmon[512] = {0};
    char fan_speed_path[512] = {0};

    if (find_acer_hwmon_dir(acer_hwmon, sizeof(acer_hwmon)) == 0) {
        LS_INFO("acer hwmon (linuwu_sense): %s", acer_hwmon);
        LS_INFO("  temp1_input = CPU, temp2_input = GPU");
    } else {
        LS_WARN("linuwu_sense hwmon (chip 'acer') not found.");
        LS_WARN("Is the module loaded? Temperature control will be disabled.");
    }

    if (find_fan_speed_path(fan_speed_path, sizeof(fan_speed_path)) == 0)
        LS_INFO("fan_speed sysfs: %s", fan_speed_path);
    else {
        LS_ERR("linuwu_sense fan_speed sysfs not found. Is the module loaded?");
        return 1;
    }

    /* ── state for hysteresis ────────────────────────────────────────────── */
    int cpu_fan_current   = 0;
    int gpu_fan_current   = 0;
    int cpu_last_set_temp = 0;
    int gpu_last_set_temp = 0;

    /* ── main poll loop ──────────────────────────────────────────────────── */
    while (g_running) {
        int cpu_temp = acer_hwmon[0]
                       ? read_hwmon_temp_channel(acer_hwmon, HWMON_CPU_TEMP_IDX) : -1;
        int gpu_temp = acer_hwmon[0]
                       ? read_hwmon_temp_channel(acer_hwmon, HWMON_GPU_TEMP_IDX) : -1;

        int cpu_fan_target = cpu_fan_current;
        int gpu_fan_target = gpu_fan_current;

        if (cpu_temp > 0 && cfg.cpu.count > 0) {
            int raw = interpolate_curve(&cfg.cpu, cpu_temp);
            cpu_fan_target = apply_hysteresis(cpu_fan_current, raw,
                                              cpu_temp, &cpu_last_set_temp,
                                              cfg.hysteresis);
        }

        if (gpu_temp > 0 && cfg.gpu.count > 0) {
            int raw = interpolate_curve(&cfg.gpu, gpu_temp);
            gpu_fan_target = apply_hysteresis(gpu_fan_current, raw,
                                              gpu_temp, &gpu_last_set_temp,
                                              cfg.hysteresis);
        }

        /* ── decide what to write ────────────────────────────────────────── */
        bool cpu_changed = (cpu_fan_target != cpu_fan_current);
        bool gpu_changed = (gpu_fan_target != gpu_fan_current);

        if (cpu_changed || gpu_changed) {
            /*
             * Driver constraint: -1 (STOP) must be applied to both fans at
             * the same time. If either channel wants STOP, force both to STOP.
             * When exiting STOP, both return to their independently computed
             * target values.
             */
            if (cpu_fan_target == FAN_HANDS_OFF || gpu_fan_target == FAN_HANDS_OFF) {
                cpu_fan_target = FAN_HANDS_OFF;
                gpu_fan_target = FAN_HANDS_OFF;
            }

            int write_cpu = cpu_fan_target;
            int write_gpu = gpu_fan_target;

            if (write_cpu == FAN_HANDS_OFF) {
                LS_INFO("CPU %d C / GPU %d C -> STOP (-1,-1): forcing fans off",
                        cpu_temp, gpu_temp);
            } else {
                LS_INFO("CPU %d C -> fan %d%%   GPU %d C -> fan %d%%",
                        cpu_temp, write_cpu, gpu_temp, write_gpu);
            }

            sysfs_write(fan_speed_path, "%d,%d\n", write_cpu, write_gpu);

            cpu_fan_current = cpu_fan_target;
            gpu_fan_current = gpu_fan_target;
        }

        sleep((unsigned int)cfg.polling_interval);
    }

    /* ── restore auto mode on exit ───────────────────────────────────────── */
    LS_INFO("linuwu-sensed stopping - restoring auto fan mode (0,0)");
    sysfs_write(fan_speed_path, "0,0\n");

    if (g_use_syslog)
        closelog();

    return 0;
}

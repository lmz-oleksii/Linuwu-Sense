#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <dirent.h>
#include <fcntl.h>
#include <ctype.h>

#define MAX_CURVE_POINTS 16

typedef struct {
    int temp;
    int fan;
} curve_point_t;

typedef struct {
    int write_interval_ms;
    int poll_interval_sec;
    float ema_alpha;
    int hysteresis;
    
    curve_point_t cpu_curve[MAX_CURVE_POINTS];
    int cpu_curve_len;
    
    curve_point_t gpu_curve[MAX_CURVE_POINTS];
    int gpu_curve_len;
} config_t;

// Default configuration
config_t g_config = {
    .write_interval_ms = 200,
    .poll_interval_sec = 2,
    .ema_alpha = 0.1f,
    .hysteresis = 5,
    .cpu_curve_len = 0,
    .gpu_curve_len = 0
};

volatile sig_atomic_t g_running = 1;

void handle_signal(int sig) {
    g_running = 0;
}

// Function to trim whitespace from the beginning and end of a string
char* trim_whitespace(char* str) {
    char *end;
    while(isspace((unsigned char)*str)) str++;
    if(*str == 0) return str;
    end = str + strlen(str) - 1;
    while(end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

bool load_config(const char* filepath) {
    FILE* file = fopen(filepath, "r");
    if (!file) return false;

    char line[256];
    char current_section[64] = "";

    while (fgets(line, sizeof(line), file)) {
        char* trimmed = trim_whitespace(line);
        if (trimmed[0] == '\0' || trimmed[0] == '#' || trimmed[0] == ';') continue;

        if (trimmed[0] == '[' && trimmed[strlen(trimmed)-1] == ']') {
            strncpy(current_section, trimmed + 1, strlen(trimmed) - 2);
            current_section[strlen(trimmed) - 2] = '\0';
            continue;
        }

        char* eq_sign = strchr(trimmed, '=');
        if (!eq_sign) continue;

        *eq_sign = '\0';
        char* key = trim_whitespace(trimmed);
        char* val = trim_whitespace(eq_sign + 1);

        if (strcmp(current_section, "settings") == 0) {
            if (strcmp(key, "write_interval_ms") == 0) {
                g_config.write_interval_ms = atoi(val);
            } else if (strcmp(key, "poll_interval_sec") == 0) {
                g_config.poll_interval_sec = atoi(val);
            } else if (strcmp(key, "ema_alpha") == 0) {
                g_config.ema_alpha = atof(val);
            } else if (strcmp(key, "hysteresis") == 0) {
                g_config.hysteresis = atoi(val);
            }
        } else if (strcmp(current_section, "cpu_curve") == 0) {
            if (g_config.cpu_curve_len < MAX_CURVE_POINTS) {
                g_config.cpu_curve[g_config.cpu_curve_len].temp = atoi(key);
                g_config.cpu_curve[g_config.cpu_curve_len].fan = atoi(val);
                g_config.cpu_curve_len++;
            }
        } else if (strcmp(current_section, "gpu_curve") == 0) {
            if (g_config.gpu_curve_len < MAX_CURVE_POINTS) {
                g_config.gpu_curve[g_config.gpu_curve_len].temp = atoi(key);
                g_config.gpu_curve[g_config.gpu_curve_len].fan = atoi(val);
                g_config.gpu_curve_len++;
            }
        }
    }
    fclose(file);
    return true;
}

int get_fan_speed(int temp, const curve_point_t* curve, int len) {
    if (len == 0) return 15; // default fallback
    int target_fan = curve[0].fan;
    for (int i = 0; i < len; i++) {
        if (temp >= curve[i].temp) {
            target_fan = curve[i].fan;
        } else {
            break;
        }
    }
    return target_fan;
}

int apply_hysteresis(int current_fan, int new_fan, int temp, int *last_set_temp) {
    if (new_fan > current_fan) {
        *last_set_temp = temp;
        return new_fan;
    }
    if (new_fan < current_fan) {
        if (temp <= *last_set_temp - g_config.hysteresis) {
            *last_set_temp = temp;
            return new_fan;
        }
        return current_fan;
    }
    return current_fan;
}

bool find_sysfs_dir(char* result) {
    const char* base = "/sys/module/linuwu_sense/drivers/platform:acer-wmi/acer-wmi";
    char path[512];
    
    snprintf(path, sizeof(path), "%s/predator_sense", base);
    if (access(path, F_OK) == 0) {
        strcpy(result, path);
        return true;
    }
    
    snprintf(path, sizeof(path), "%s/nitro_sense", base);
    if (access(path, F_OK) == 0) {
        strcpy(result, path);
        return true;
    }
    
    return false;
}

bool find_hwmon_dir(char* result) {
    DIR *d = opendir("/sys/class/hwmon");
    if (!d) return false;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "hwmon", 5) != 0) continue;

        char name_path[512];
        snprintf(name_path, sizeof(name_path), "/sys/class/hwmon/%s/name", ent->d_name);

        FILE *f = fopen(name_path, "r");
        if (!f) continue;

        char chip_name[64] = {0};
        if (fscanf(f, "%63s", chip_name) == 1) {
            if (strcmp(chip_name, "acer") == 0) {
                snprintf(result, 512, "/sys/class/hwmon/%s", ent->d_name);
                fclose(f);
                closedir(d);
                return true;
            }
        }
        fclose(f);
    }
    closedir(d);
    return false;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    const char* config_path = "/etc/linuwu-sense.conf";
    if (access("linuwu-sense.conf", F_OK) == 0) {
        config_path = "linuwu-sense.conf";
    }

    if (argc > 1) {
        config_path = argv[1];
    }

    if (load_config(config_path)) {
        printf("Configuration loaded successfully from %s\n", config_path);
    } else {
        printf("Warning: Configuration file %s not found, using default settings.\n", config_path);
    }

    char sysfs_dir[512];
    char hwmon_dir[512];

    if (!find_sysfs_dir(sysfs_dir)) {
        printf("Error: Linuwu-Sense sysfs directory not found!\n");
        return 1;
    }

    if (!find_hwmon_dir(hwmon_dir)) {
        printf("Error: acer hwmon directory not found!\n");
        return 1;
    }

    char cpu_temp_path[512];
    char gpu_temp_path[512];
    char fan_speed_path[512];

    snprintf(cpu_temp_path, sizeof(cpu_temp_path), "%s/temp1_input", hwmon_dir);
    snprintf(gpu_temp_path, sizeof(gpu_temp_path), "%s/temp2_input", hwmon_dir);
    snprintf(fan_speed_path, sizeof(fan_speed_path), "%s/fan_speed", sysfs_dir);

    int fd_cpu = open(cpu_temp_path, O_RDONLY);
    int fd_gpu = open(gpu_temp_path, O_RDONLY);
    int fd_fan = open(fan_speed_path, O_WRONLY);

    if (fd_cpu < 0 || fd_gpu < 0 || fd_fan < 0) {
        printf("Error: Failed to open sysfs files!\n");
        if (fd_cpu >= 0) close(fd_cpu);
        if (fd_gpu >= 0) close(fd_gpu);
        if (fd_fan >= 0) close(fd_fan);
        return 1;
    }

    printf("CPU_TEMP(Raw), CPU_TEMP(EMA), CPU_FAN, GPU_TEMP(Raw), GPU_TEMP(EMA), GPU_FAN\n");

    float cpu_ema = -1.0f;
    float gpu_ema = -1.0f;
    
    int cpu_fan = 0;
    int gpu_fan = 0;
    int cpu_last_set = 0;
    int gpu_last_set = 0;
    char buf[32];
    
    int ticks = 0;
    int cpu_raw = 0, gpu_raw = 0;
    int cpu_temp = 0, gpu_temp = 0;

    int write_interval_us = g_config.write_interval_ms * 1000;
    int poll_ticks = (g_config.poll_interval_sec * 1000000) / write_interval_us;
    if (poll_ticks <= 0) poll_ticks = 1;

    while (g_running) {
        if (ticks % poll_ticks == 0) {
            int cpu_temp_mC = 0, gpu_temp_mC = 0;
            
            lseek(fd_cpu, 0, SEEK_SET);
            int n = read(fd_cpu, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                cpu_temp_mC = atoi(buf);
            }

            lseek(fd_gpu, 0, SEEK_SET);
            n = read(fd_gpu, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                gpu_temp_mC = atoi(buf);
            }

            cpu_raw = cpu_temp_mC / 1000;
            gpu_raw = gpu_temp_mC / 1000;

            if (cpu_ema < 0.0f) {
                cpu_ema = cpu_raw;
                gpu_ema = gpu_raw;
            } else {
                cpu_ema = (g_config.ema_alpha * cpu_raw) + ((1.0f - g_config.ema_alpha) * cpu_ema);
                gpu_ema = (g_config.ema_alpha * gpu_raw) + ((1.0f - g_config.ema_alpha) * gpu_ema);
            }

            cpu_temp = (int)(cpu_ema + 0.5f);
            gpu_temp = (int)(gpu_ema + 0.5f);

            int raw_cpu_fan = get_fan_speed(cpu_temp, g_config.cpu_curve, g_config.cpu_curve_len);
            int raw_gpu_fan = get_fan_speed(gpu_temp, g_config.gpu_curve, g_config.gpu_curve_len);

            cpu_fan = apply_hysteresis(cpu_fan, raw_cpu_fan, cpu_temp, &cpu_last_set);
            gpu_fan = apply_hysteresis(gpu_fan, raw_gpu_fan, gpu_temp, &gpu_last_set);

            printf("\r%d, %d, %d, %d, %d, %d                ", cpu_raw, cpu_temp, cpu_fan, gpu_raw, gpu_temp, gpu_fan);
            fflush(stdout);
        }

        int out_len = snprintf(buf, sizeof(buf), "%d,%d\n", cpu_fan, gpu_fan);
        lseek(fd_fan, 0, SEEK_SET);
        if (write(fd_fan, buf, out_len) < 0) {}

        usleep(write_interval_us);
        ticks++;
    }

    printf("\nStopping... Restoring AUTO mode (-1,-1)\n");
    int out_len = snprintf(buf, sizeof(buf), "-1,-1\n");
    lseek(fd_fan, 0, SEEK_SET);
    if (write(fd_fan, buf, out_len) < 0) {}

    close(fd_cpu);
    close(fd_gpu);
    close(fd_fan);

    return 0;
}

/**
 *  mbpfan.c - automatically control fan for MacBook Pro
 *  Copyright (C) 2010  Allan McRae <allan@archlinux.org>
 *  Modifications by Rafael Vega <rvega@elsoftwarehamuerto.org>
 *  Modifications (2012) by Ismail Khatib <ikhatib@gmail.com>
 *  Modifications (2012-present) by Daniel Graziotin <daniel@ineed.coffee> [CURRENT MAINTAINER]
 *  Modifications (2017-present) by Robert Musial <rmusial@fastmail.com>
 *  Modifications (2018-present) by Ati Sharma <ati.sharma@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *
 *  Notes:
 *    Assumes any number of processors, cores, sensors and fans
 *    (as defined in NUM_PROCESSORS, NUM_HWMONS, NUM_TEMP_INPUTS and NUM_FANS)
 *    It uses only the temperatures from the processors as input.
 *    Requires coretemp and applesmc kernel modules to be loaded.
 *    Requires root use
 *
 *  Tested models: see README.md
 */

#include <linux/limits.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <syslog.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/syslog.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/errno.h>
#include "daemon.h"
#include "mbpfan.h"
#include "global.h"
#include "settings.h"
#include "util.h"

/* lazy min/max... */
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

#define CORETEMP_PATH "/sys/devices/platform/coretemp.0"
#define APPLESMC_PATH "/sys/devices/platform/applesmc.768"
#define ALT_APPLESMC_PATH "/sys/bus/acpi/drivers/applesmc"
#ifndef DRM_CLASS_PATH
#define DRM_CLASS_PATH "/sys/class/drm"
#endif

/* seconds between GPU sensor scans while none have been found yet
 * (the GPU driver may load after mbpfan starts) */
#define GPU_RESCAN_INTERVAL 30

/* temperature thresholds
 * low_temp - temperature below which fan speed will be at minimum
 * high_temp - fan will increase speed when higher than this temperature
 * max_temp - fan will run at full speed above this temperature */
int low_temp = 63;  // try ranges 55-63
int high_temp = 66; // try ranges 58-66
int max_temp = 86;  // do not set it > 90

/* same thresholds for the GPU(s). Starting points for a 2013 Mac Pro
 * (FirePro D300/D500/D700); tune them for your machine. */
int gpu_low_temp = 65;
int gpu_high_temp = 72;
int gpu_max_temp = 92;

// maximum number of processors etc supported
#define NUM_PROCESSORS 6
#define NUM_HWMONS 12
#define NUM_TEMP_INPUTS 64
#define NUM_FANS 10
// sane defaults when user provides unexpected values
#define MIN_FAN_SPEED_DEFAULT 500
#define MAX_FAN_SPEED_DEFAULT 6500

int polling_interval = 1;

t_sensors *sensors = NULL;
t_fans *fans = NULL;
char applesmc_path[PATH_MAX];
char applesmc_fan_path[PATH_MAX];


char *smprintf(const char *fmt, ...)
{
    char *buf;
    int cnt;
    va_list ap;

    // find buffer length
    va_start(ap, fmt);
    cnt = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (cnt < 0) {
        return NULL;
    }

    // create and write to buffer
    buf = malloc(cnt + 1);
    va_start(ap, fmt);
    vsnprintf(buf, cnt + 1, fmt, ap);
    va_end(ap);
    return buf;
}

bool is_modern_sensors_path()
{
    struct utsname kernel;
    uname(&kernel);

    char *str_kernel_version;
    str_kernel_version = strtok(kernel.release, ".");

    if (atoi(str_kernel_version) < 3) {
        mbp_log(LOG_ERR, "mbpfan detected a pre-3.x.x linux kernel. Detected version: %s. Exiting.\n", kernel.release);
        exit(EXIT_FAILURE);
    }

    int counter;

    for (counter = 0; counter < NUM_HWMONS; counter++) {
        int temp;
        for (temp = 1; temp < NUM_TEMP_INPUTS; ++temp) {
            char *path = smprintf("/sys/devices/platform/coretemp.0/hwmon/hwmon%d/temp%d_input", counter, temp);
            int res = access(path, R_OK);
            free(path);
            if (res == 0) {
                return 1;
            }
        }
    }

    return 0;
}

t_sensors *retrieve_sensors()
{
    t_sensors *sensors_head = NULL;
    t_sensors *s = NULL;

    char *path = NULL;
    char *path_begin = NULL;

    const char *path_end = "_input";
    int sensors_found = 0;

    if (!is_modern_sensors_path()) {
        if (verbose) {
            mbp_log(LOG_INFO, "Using legacy path for kernel < 3.15.0");
        }

        path_begin = strdup("/sys/devices/platform/coretemp.0/temp");

    } else {

        if (verbose) {
            mbp_log(LOG_INFO, "Using new sensor path for kernel >= 3.15.0 or some CentOS versions with kernel 3.10.0 ");
        }

        // loop over up to 6 processors
        int processor;
        for (processor = 0; processor < NUM_PROCESSORS; processor++) {

            if (path_begin != NULL) {
                free(path_begin);
            }
            path_begin = smprintf("/sys/devices/platform/coretemp.%d/hwmon/hwmon", processor);

            int counter;
            for (counter = 0; counter < NUM_HWMONS; counter++) {

                char *hwmon_path = smprintf("%s%d", path_begin, counter);

                int res = access(hwmon_path, R_OK);
                if (res == 0) {

                    free(path_begin);
                    path_begin = smprintf("%s/temp", hwmon_path);

                    if (verbose) {
                        mbp_log(LOG_INFO, "Found hwmon path at %s", path_begin);
                    }

                    free(hwmon_path);
                    break;
                }

                free(hwmon_path);
            }

            int core = 0;
            for (core = 0; core < NUM_TEMP_INPUTS; core++) {
                path = smprintf("%s%d%s", path_begin, core, path_end);

                FILE *file = fopen(path, "r");

                if (file != NULL) {
                    s = (t_sensors *)malloc(sizeof(t_sensors));
                    s->path = strdup(path);
                    s->is_gpu = 0;
                    fscanf(file, "%u", &s->temperature);

                    if (sensors_head == NULL) {
                        sensors_head = s;
                        sensors_head->next = NULL;

                    } else {
                        t_sensors *tmp = sensors_head;

                        while (tmp->next != NULL) {
                            tmp = tmp->next;
                        }

                        tmp->next = s;
                        tmp->next->next = NULL;
                    }

                    s->file = file;
                    sensors_found++;
                }

                free(path);
                path = NULL;
            }
        }
    }

    if (verbose) {
        mbp_log(LOG_INFO, "Found %d sensors", sensors_found);
    }

    if (sensors_found == 0) {
        mbp_log(LOG_CRIT, "mbpfan could not detect any temp sensor. Please contact the developer.");
        exit(EXIT_FAILURE);
    }

    free(path_begin);
    path_begin = NULL;

    return sensors_head;
}

static int read_value(const char *path)
{
    int value = -1;
    FILE *file = fopen(path, "r");
    if (file != NULL) {
        fscanf(file, "%d", &value);
        fclose(file);
    }
    return value;
}

static void read_value_str(const char *path, char *str, size_t len)
{
    str[0] = '\0';
    FILE *file = fopen(path, "r");
    if (file != NULL) {
        if (fgets(str, len, file) == NULL) {
            str[0] = '\0';
        }
        fclose(file);
    }
}

static void trim_trailing_whitespace(char *str)
{
    for (ssize_t i = strlen(str) - 1; i >= 0; --i) {
        if (!isspace((unsigned char)str[i])) {
            break;
        }
        str[i] = '\0';
    }
}

/* Append a sensor read from `path` to the list. Returns NULL (and adds
 * nothing) if the file can't be opened. */
static t_sensors *append_sensor(t_sensors **head, const char *path, int is_gpu)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return NULL;
    }

    t_sensors *s = (t_sensors *)malloc(sizeof(t_sensors));
    if (s == NULL) {
        fclose(file);
        return NULL;
    }

    s->path = strdup(path);
    s->file = file;
    s->is_gpu = is_gpu;
    s->next = NULL;
    if (fscanf(file, "%u", &s->temperature) != 1) {
        s->temperature = 0;
    }

    if (*head == NULL) {
        *head = s;
    } else {
        t_sensors *tmp = *head;
        while (tmp->next != NULL) {
            tmp = tmp->next;
        }
        tmp->next = s;
    }

    return s;
}

int retrieve_gpu_sensors(t_sensors **head)
{
    int added = 0;

    DIR *drm_dir = opendir(DRM_CLASS_PATH);
    if (drm_dir == NULL) {
        return 0;
    }

    struct dirent *ent;
    while ((ent = readdir(drm_dir)) != NULL) {
        /* only cardN; skip renderD*, and connectors like card0-DP-1 */
        if (strncmp(ent->d_name, "card", 4) != 0) continue;
        if (strchr(ent->d_name, '-') != NULL) continue;

        char *hwmon_base = smprintf("%s/%s/device/hwmon", DRM_CLASS_PATH, ent->d_name);
        if (hwmon_base == NULL) continue;

        DIR *hw_dir = opendir(hwmon_base);
        if (hw_dir == NULL) {
            free(hwmon_base);
            continue;
        }

        struct dirent *hent;
        while ((hent = readdir(hw_dir)) != NULL) {
            if (strncmp(hent->d_name, "hwmon", 5) != 0) continue;

            /* only accept the AMD drivers; anything else is not a sensor
             * we know how to interpret */
            char *name_path = smprintf("%s/%s/name", hwmon_base, hent->d_name);
            char name[32];
            name[0] = '\0';
            if (name_path != NULL) {
                read_value_str(name_path, name, sizeof(name));
                free(name_path);
            }
            trim_trailing_whitespace(name);
            if (strcmp(name, "amdgpu") != 0 && strcmp(name, "radeon") != 0) continue;

            /* temp1 is the edge/core temperature on the GCN cards */
            char *temp_path = smprintf("%s/%s/temp1_input", hwmon_base, hent->d_name);
            if (temp_path != NULL) {
                if (append_sensor(head, temp_path, 1) != NULL) {
                    added++;
                    if (verbose) {
                        mbp_log(LOG_INFO, "Found GPU sensor (%s) at %s", name, temp_path);
                    }
                }
                free(temp_path);
            }
        }

        closedir(hw_dir);
        free(hwmon_base);
    }

    closedir(drm_dir);
    return added;
}

t_fans *retrieve_fans()
{
    t_fans *fans_head = NULL;
    t_fans *fan = NULL;

    char *path_output = NULL;
    char *path_label = NULL;
    char *path_manual = NULL;
    char *path_fan_max = NULL;
    char *path_fan_min = NULL;

    const char *path_begin = (const char *) &applesmc_fan_path;
    const char *path_output_end = "_output";
    const char *path_label_end = "_label";
    const char *path_man_end = "_manual";
    const char *path_max_speed = "_max";
    const char *path_min_speed = "_min";

    int counter = 0;
    int fans_found = 0;

    for (counter = 0; counter < NUM_FANS; counter++) {

        path_output = smprintf("%s%d%s", path_begin, counter, path_output_end);
        path_label = smprintf("%s%d%s", path_begin, counter, path_label_end);
        path_manual = smprintf("%s%d%s", path_begin, counter, path_man_end);
        path_fan_min = smprintf("%s%d%s", path_begin, counter, path_min_speed);
        path_fan_max = smprintf("%s%d%s", path_begin, counter, path_max_speed);

        FILE *file = fopen(path_output, "w");

        if (file != NULL) {
            fan = (t_fans *)malloc(sizeof(t_fans));
            fan->fan_output_path = strdup(path_output);
            fan->fan_manual_path = strdup(path_manual);
            fan->fan_id = counter;

            int fan_speed = read_value(path_fan_min);
            if (fan_speed == -1 || fan_speed < MIN_FAN_SPEED_DEFAULT)
                fan->fan_min_speed = MIN_FAN_SPEED_DEFAULT;
            else
                fan->fan_min_speed = fan_speed;

            fan_speed = read_value(path_fan_max);
            if (fan_speed == -1 || fan_speed > MAX_FAN_SPEED_DEFAULT)
                fan->fan_max_speed = MAX_FAN_SPEED_DEFAULT;
            else
                fan->fan_max_speed = fan_speed;

            size_t max_label_len = 64;
            fan->label = malloc(max_label_len);
            read_value_str(path_label, fan->label, max_label_len);
            trim_trailing_whitespace(fan->label);

            fan->old_speed = 0;

            if (fans_head == NULL) {
                fans_head = fan;
                fans_head->next = NULL;

            } else {
                t_fans *tmp = fans_head;

                while (tmp->next != NULL) {
                    tmp = tmp->next;
                }

                tmp->next = fan;
                tmp->next->next = NULL;
            }

            fan->file = file;
            fans_found++;
        }
        free(path_fan_min);
        path_fan_min = NULL;
        free(path_label);
        path_label = NULL;
        free(path_fan_max);
        path_fan_max = NULL;
        free(path_output);
        path_output = NULL;
        free(path_manual);
        path_manual = NULL;
    }

    if (verbose) {
        mbp_log(LOG_INFO, "Found %d fans", fans_found);
    }

    if (fans_found == 0) {
        mbp_log(LOG_CRIT, "mbpfan could not detect any fan. Please contact the developer.");
        exit(EXIT_FAILURE);
    }

    return fans_head;
}

static void set_fans_mode(t_fans *fans, int mode)
{
    t_fans *tmp = fans;
    FILE *file;

    while (tmp != NULL) {
        file = fopen(tmp->fan_manual_path, "rw+");

        if (file != NULL) {
            fprintf(file, "%d", mode);
            fclose(file);
        }

        tmp = tmp->next;
    }
}

/* If something (the SMC, another tool, a manual `echo 0`) put this fan back
 * in automatic mode, our speed writes are silently ignored. Detect that and
 * retake manual control. Returns 1 if it had to. */
static int ensure_manual_mode(t_fans *fan)
{
    if (read_value(fan->fan_manual_path) != 0) {
        return 0;
    }

    FILE *file = fopen(fan->fan_manual_path, "w");
    if (file != NULL) {
        fprintf(file, "1");
        fclose(file);
    }

    /* force the next set_fan_speed() to actually write */
    fan->old_speed = -1;
    return 1;
}

void set_fans_man(t_fans *fans)
{

    set_fans_mode(fans, 1);
}

void set_fans_auto(t_fans *fans)
{

    set_fans_mode(fans, 0);
}

t_sensors *refresh_sensors(t_sensors *sensors)
{
    t_sensors *tmp = sensors;

    while (tmp != NULL) {
        if (tmp->file != NULL) {
            char buf[16];
            ssize_t len = pread(fileno(tmp->file), buf, sizeof(buf) - 1, /*offset=*/0);
            if (len >= 0) {
                buf[len] = '\0';
                tmp->temperature = strtod(buf, NULL);
            }
        }

        tmp = tmp->next;
    }

    return sensors;
}

/* Controls the speed of a fan */
void set_fan_speed(t_fans *fan, int speed)
{
    if (fan != NULL && fan->file != NULL && fan->old_speed != speed) {
        char buf[16];
        int len = snprintf(buf, sizeof(buf), "%d", speed);
        int res = pwrite(fileno(fan->file), buf, len, /*offset=*/0);
        if (res == -1) {
            perror("Could not set fan speed");
        }
        fan->old_speed = speed;
    }
}

void set_fan_minimum_speed(t_fans *fans)
{
    t_fans *tmp = fans;

    while (tmp != NULL) {
        set_fan_speed(tmp, tmp->fan_min_speed);
        tmp = tmp->next;
    }
}
/* Highest temperature (degrees C) among sensors of one kind, from their
 * last refresh. -1 if there are none. */
static int group_temp(t_sensors *sensors, int gpu)
{
    int found = 0;
    unsigned int temp = 0;

    for (t_sensors *tmp = sensors; tmp != NULL; tmp = tmp->next) {
        if (tmp->is_gpu != gpu) continue;
        found = 1;
        temp = max(temp, tmp->temperature);
    }

    return found ? (int)(temp / 1000) : -1;
}

unsigned short get_temp(t_sensors *sensors)
{
    sensors = refresh_sensors(sensors);
    int temp = group_temp(sensors, 0);
    return temp < 0 ? 0 : temp;
}

int get_gpu_temp(t_sensors *sensors)
{
    sensors = refresh_sensors(sensors);
    return group_temp(sensors, 1);
}

/* One independent controller (CPU or GPU).
 * Given the speed that controller last asked for and its old/new
 * temperature, return the speed it asks for now. This is the original
 * mbpfan ramp: jump to max at/above max_t, min at/below low_t, ramp up
 * quadratically while heating above high_t, ramp down while cooling
 * above low_t, otherwise hold. */
static int ramp_speed(const t_fans *fan, int speed, int old_temp, int new_temp,
                      int low_t, int high_t, int max_t)
{
    int span = fan->fan_max_speed - fan->fan_min_speed;

    if (new_temp >= max_t) {
        speed = fan->fan_max_speed;
    }

    if (new_temp <= low_t) {
        speed = fan->fan_min_speed;
    }

    int temp_change = new_temp - old_temp;

    if (temp_change > 0 && new_temp > high_t && new_temp < max_t) {
        int steps = (new_temp - high_t) * (new_temp - high_t + 1) / 2;
        double step_up = span / ((max_t - high_t) * (max_t - high_t + 1) / 2.0);
        speed = max(speed, (int)ceil(fan->fan_min_speed + steps * step_up));
    }

    if (temp_change < 0 && new_temp > low_t && new_temp < max_t) {
        int steps = (max_t - new_temp) * (max_t - new_temp + 1) / 2;
        double step_down = span / ((max_t - low_t) * (max_t - low_t + 1) / 2.0);
        speed = min(speed, (int)floor(fan->fan_max_speed - steps * step_down));
    }

    return speed;
}

void retrieve_settings(const char *settings_path, t_fans *fans)
{
    Settings *settings = NULL;
    int result = 0;
    FILE *f = NULL;

    if (settings_path == NULL) {
        f = fopen("/etc/mbpfan.conf", "r");

    } else {
        f = fopen(settings_path, "r");
    }

    if (f == NULL) {
        /* Could not open configfile */
        if (verbose) {
            mbp_log(LOG_INFO, "Couldn't open configfile, using defaults");
        }

    } else {
        settings = settings_open(f);
        fclose(f);

        if (settings == NULL) {
            /* Could not read configfile */
            if (verbose) {
                mbp_log(LOG_WARNING, "Couldn't read configfile");
            }

        } else {

            t_fans *fan = fans;

            while (fan != NULL) {

                char *config_key;
                config_key = smprintf("min_fan%d_speed", fan->fan_id);
                /* Read configfile values */
                result = settings_get_int(settings, "general", config_key);
                if (result != 0) {
                    fan->fan_min_speed = result;
                }
                free(config_key);

                config_key = smprintf("max_fan%d_speed", fan->fan_id);
                result = settings_get_int(settings, "general", config_key);

                if (result != 0) {
                    fan->fan_max_speed = result;
                }
                free(config_key);
                fan = fan->next;
            }
            result = settings_get_int(settings, "general", "low_temp");

            if (result != 0) {
                low_temp = result;
            }

            result = settings_get_int(settings, "general", "high_temp");

            if (result != 0) {
                high_temp = result;
            }

            result = settings_get_int(settings, "general", "max_temp");

            if (result != 0) {
                max_temp = result;
            }

            result = settings_get_int(settings, "general", "gpu_low_temp");

            if (result != 0) {
                gpu_low_temp = result;
            }

            result = settings_get_int(settings, "general", "gpu_high_temp");

            if (result != 0) {
                gpu_high_temp = result;
            }

            result = settings_get_int(settings, "general", "gpu_max_temp");

            if (result != 0) {
                gpu_max_temp = result;
            }

            result = settings_get_int(settings, "general", "polling_interval");

            if (result != 0) {
                polling_interval = result;
            }

            /* Destroy the settings object */
            settings_delete(settings);
        }
    }
}

void check_requirements(const char *program_path)
{

    /**
     * Check for root
     */

    uid_t uid = getuid(), euid = geteuid();

    if (uid != 0 || euid != 0) {
        mbp_log(LOG_ERR, "%s needs root privileges. Please run %s as root. Exiting.", program_path, program_path);
        exit(EXIT_FAILURE);
    }

    /**
     * Check for coretemp and applesmc modules
     */
    DIR *dir = opendir(CORETEMP_PATH);

    if (dir == NULL) {
        mbp_log(LOG_ERR, "%s needs coretemp support. Please either load it or build it into the kernel. Exiting.", program_path);
        exit(EXIT_FAILURE);
    }

    closedir(dir);
    memset(&applesmc_path, 0, PATH_MAX);
    memset(&applesmc_fan_path, 0, PATH_MAX);

    dir = opendir(APPLESMC_PATH);

    if (dir != NULL) {
        strncpy((char *) &applesmc_path, APPLESMC_PATH, PATH_MAX);
        closedir(dir);
    } else {
        /**
        * Check for alternate ACPI device path for newer macbooks
        */
        dir = opendir(ALT_APPLESMC_PATH);
        if (dir != NULL) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                if (strncmp("APP", (const char *) &ent->d_name, 3) == 0) {
                    snprintf((char *) &applesmc_path, PATH_MAX, "%s/%s", ALT_APPLESMC_PATH, (char *) &ent->d_name);
                    break;
                }
            }
            closedir(dir);
        }
    }

    if (strlen(applesmc_path) != 0) {
        strncpy((char *) &applesmc_fan_path, (char *) &applesmc_path, PATH_MAX);
        strcat((char *) &applesmc_fan_path, "/fan");
        if (verbose) mbp_log(LOG_INFO, "applesmc device path: %s", (char *) &applesmc_path);

    } else {
        mbp_log(LOG_ERR, "%s needs applesmc support. Please either load it or build it into the kernel. Exiting.", program_path);
        exit(EXIT_FAILURE);
    }
}

int get_max_mhz(void)
{
    int max_mhz = -1;
    DIR *dir = opendir("/sys/devices/system/cpu");
    if (dir == NULL) {
        return -1;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "cpu", 3) != 0 || strcmp(ent->d_name, "cpufreq") == 0 || strcmp(ent->d_name, "cpuidle") == 0) {
            continue;
        }
        char *path = smprintf("/sys/devices/system/cpu/%s/cpufreq/scaling_cur_freq", ent->d_name);
        max_mhz = max(max_mhz, read_value(path) / 1000);
        free(path);
    }
    closedir(dir);
    return max_mhz;
}

void mbpfan()
{
    int old_cpu, new_cpu, old_gpu, new_gpu, fan_speed;

    /* The CPU and the GPU each get their own controller (their own
     * thresholds and their own "speed I last asked for"). The fan runs at
     * the higher of the two requests, so neither can hold the other down
     * and a change in which sensor is hottest doesn't confuse the
     * heating/cooling trend logic. */
    enum { CPU_CTRL = 0, GPU_CTRL = 1, NUM_CTRL = 2 };
    int ctrl_speed[NUM_CTRL][NUM_FANS];

    sensors = retrieve_sensors();      /* CPU sensors; exits if none */
    int gpu_sensors = retrieve_gpu_sensors(&sensors);
    time_t last_gpu_scan = time(NULL);
    fans = retrieve_fans();

    retrieve_settings(NULL, fans);

    t_fans *fan = fans;
    while (fan != NULL) {

        if (fan->fan_min_speed > fan->fan_max_speed) {
            mbp_log(LOG_ERR, "Invalid fan speeds: %d %d", fan->fan_min_speed, fan->fan_max_speed);
            exit(EXIT_FAILURE);
        }
        fan = fan->next;
    }

    if (low_temp > high_temp || high_temp > max_temp) {
        mbp_log(LOG_ERR, "Invalid temperatures: %d %d %d", low_temp, high_temp, max_temp);
        exit(EXIT_FAILURE);
    }

    if (gpu_low_temp > gpu_high_temp || gpu_high_temp > gpu_max_temp) {
        mbp_log(LOG_ERR, "Invalid GPU temperatures: %d %d %d", gpu_low_temp, gpu_high_temp, gpu_max_temp);
        exit(EXIT_FAILURE);
    }

    if (gpu_sensors == 0) {
        mbp_log(LOG_WARNING, "No GPU sensors found yet; controlling on CPU temperature only (will keep looking)");
    }

    set_fans_man(fans);

    refresh_sensors(sensors);
    new_cpu = group_temp(sensors, 0);
    new_gpu = group_temp(sensors, 1);
    set_fan_minimum_speed(fans);

    for (fan = fans; fan != NULL; fan = fan->next) {
        ctrl_speed[CPU_CTRL][fan->fan_id] = fan->fan_min_speed;
        ctrl_speed[GPU_CTRL][fan->fan_id] = fan->fan_min_speed;
    }

recalibrate:
    if (verbose) {
        mbp_log(LOG_INFO, "Sleeping for 2 seconds to get first temp delta");
    }
    sleep(2);

    while (1) {
        if (do_exit) {
            syslog(LOG_WARNING, "Received SIGTERM, SIGQUIT, or SIGINT signal.");
            cleanup_and_exit(EXIT_SUCCESS);
        }

        if (do_reload) {
            syslog(LOG_WARNING, "Received SIGHUP signal.");
            retrieve_settings(NULL, fans);
            if (gpu_low_temp > gpu_high_temp || gpu_high_temp > gpu_max_temp) {
                mbp_log(LOG_ERR, "Invalid GPU temperatures after reload: %d %d %d", gpu_low_temp, gpu_high_temp, gpu_max_temp);
                exit(EXIT_FAILURE);
            }
            do_reload = 0;
        }

        /* GPU driver may have loaded after we started */
        if (gpu_sensors == 0 && time(NULL) - last_gpu_scan >= GPU_RESCAN_INTERVAL) {
            last_gpu_scan = time(NULL);
            gpu_sensors = retrieve_gpu_sensors(&sensors);
            if (gpu_sensors > 0) {
                mbp_log(LOG_INFO, "Found %d GPU sensor(s); now controlling on CPU and GPU", gpu_sensors);
            }
        }

        old_cpu = new_cpu;
        old_gpu = new_gpu;
        refresh_sensors(sensors);
        new_cpu = group_temp(sensors, 0);
        new_gpu = group_temp(sensors, 1);

        if (old_gpu < 0) {
            /* GPU just appeared: no trend yet, and it starts at minimum */
            old_gpu = new_gpu;
            for (fan = fans; fan != NULL; fan = fan->next) {
                ctrl_speed[GPU_CTRL][fan->fan_id] = fan->fan_min_speed;
            }
        }

        fan = fans;

        while (fan != NULL) {
            int id = fan->fan_id;

            if (ensure_manual_mode(fan)) {
                mbp_log(LOG_WARNING, "Fan %s was back in automatic mode; retook manual control", fan->label);
            }

            ctrl_speed[CPU_CTRL][id] = ramp_speed(fan, ctrl_speed[CPU_CTRL][id],
                                                  old_cpu, new_cpu,
                                                  low_temp, high_temp, max_temp);
            fan_speed = ctrl_speed[CPU_CTRL][id];

            if (new_gpu >= 0) {
                ctrl_speed[GPU_CTRL][id] = ramp_speed(fan, ctrl_speed[GPU_CTRL][id],
                                                      old_gpu, new_gpu,
                                                      gpu_low_temp, gpu_high_temp, gpu_max_temp);
                fan_speed = max(fan_speed, ctrl_speed[GPU_CTRL][id]);
            }

            if (verbose) {
                mbp_log(LOG_INFO, "CPU: %d->%d GPU: %d->%d Fan: %s Speed: %d (cpu wants %d, gpu wants %d) Max MHz: %d",
                        old_cpu, new_cpu, old_gpu, new_gpu, fan->label, fan_speed,
                        ctrl_speed[CPU_CTRL][id],
                        new_gpu >= 0 ? ctrl_speed[GPU_CTRL][id] : 0,
                        get_max_mhz());
            }

            set_fan_speed(fan, fan_speed);
            fan = fan->next;
        }

        if (verbose) {
            mbp_log(LOG_INFO, "Sleeping for %d seconds", polling_interval);
        }

        time_t before_sleep = time(NULL);

        // call nanosleep instead of sleep to avoid rt_sigprocmask and
        // rt_sigaction
        struct timespec ts;
        ts.tv_sec = polling_interval;
        ts.tv_nsec = 0;
        nanosleep(&ts, NULL);

        time_t after_sleep = time(NULL);
        if (after_sleep - before_sleep > 2 * polling_interval) {
            mbp_log(LOG_INFO, "Clock skew detected - slept for %ld seconds but expected %d", after_sleep - before_sleep, polling_interval);
            set_fans_man(fans);
            goto recalibrate;
        }
    }
}

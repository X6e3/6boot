/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The watchdog tests' clock, on the J713's Linux. heartbeat.service starts it early in boot when the
 * kernel command line has x6e3.hb=1. It writes a header, then every half second one line, each
 * fsynced, to /var/lib/6boot-heartbeat/<boot id>.log:
 *
 *   t <CNTVCT_EL0> <CLOCK_BOOTTIME ns> <CLOCK_REALTIME s.ns>
 *
 * CNTVCT_EL0 is the 24 MHz counter 6boot read when it armed the watchdog (xboot,watchdog in /chosen);
 * Linux at EL2 sets CNTVOFF_EL2 to 0, so the two count alike, and the header checks it against
 * CLOCK_BOOTTIME. A boot that a reset ends has its last line within half a second of the reset; one
 * that is shut down ends with "stopped".
 *
 *   heartbeat [DIR]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static void on_term(int s)
{
    (void)s;
    stop = 1;
}

static unsigned long long cntvct(void)
{
    unsigned long long v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static long long ns(clockid_t c)
{
    struct timespec t;
    clock_gettime(c, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void slurp(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n = 0;
    out[0] = 0;
    if (!f) {
        snprintf(out, cap, "(none)");
        return;
    }
    n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = 0;
    for (size_t i = 0; i < n; i++)
        if (out[i] == '\n' || out[i] == 0)
            out[i] = i + 1 < n ? ' ' : 0;
}

static void put(int fd, const char *s)
{
    size_t n = strlen(s);
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
    fsync(fd);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/var/lib/6boot-heartbeat";
    char id[64], path[256], line[4096], buf[3800];
    static const char *const chosen[] = {"asahi,m1n1-stage2-version", "xboot,path", "xboot,trial",
                                         "xboot,trial-report", "xboot,watchdog"};

    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    mkdir(dir, 0755);
    slurp("/proc/sys/kernel/random/boot_id", id, sizeof(id));
    snprintf(path, sizeof(path), "%s/%s.log", dir, id);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        perror(path);
        return 1;
    }
    snprintf(line, sizeof(line), "boot %s\n", id);
    put(fd, line);
    slurp("/proc/cmdline", buf, sizeof(buf));
    snprintf(line, sizeof(line), "cmdline %s\n", buf);
    put(fd, line);
    for (size_t i = 0; i < sizeof(chosen) / sizeof(chosen[0]); i++) {
        char p[128];
        snprintf(p, sizeof(p), "/proc/device-tree/chosen/%s", chosen[i]);
        slurp(p, buf, sizeof(buf));
        snprintf(line, sizeof(line), "chosen %s = %s\n", chosen[i], buf);
        put(fd, line);
    }
    snprintf(line, sizeof(line), "watchdog dev: %s, apple-watchdog driver: %s\n",
             access("/dev/watchdog0", F_OK) ? "none" : "/dev/watchdog0",
             access("/sys/bus/platform/drivers/apple-watchdog", F_OK) ? "not registered" : "registered");
    put(fd, line);
    /* the counter against CLOCK_BOOTTIME: where boottime 0 falls on the counter */
    unsigned long long c = cntvct();
    long long b = ns(CLOCK_BOOTTIME);
    snprintf(line, sizeof(line), "clock cntvct=%llu boottime_ns=%lld boottime0_cnt=%lld\n", c, b,
             (long long)c - b / 1000 * 24); /* 24 ticks a microsecond */
    put(fd, line);

    while (!stop) {
        struct timespec r;
        clock_gettime(CLOCK_REALTIME, &r);
        snprintf(line, sizeof(line), "t %llu %lld %lld.%09ld\n", cntvct(), ns(CLOCK_BOOTTIME), (long long)r.tv_sec,
                 r.tv_nsec);
        put(fd, line);
        struct timespec half = {0, 500000000L};
        while (nanosleep(&half, &half) && errno == EINTR && !stop)
            ;
    }
    snprintf(line, sizeof(line), "stopped %llu\n", cntvct());
    put(fd, line);
    close(fd);
    return 0;
}

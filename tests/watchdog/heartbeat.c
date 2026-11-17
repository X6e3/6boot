/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The watchdog tests' clock, on the J713's Linux. heartbeat.service starts it early in boot when the
 * kernel command line has x6e3.hb=1. It writes a header, then every half second one line, each
 * fsynced, to /var/lib/6boot-heartbeat/<boot id>.log:
 *
 *   t <CNTVCT_EL0> <CLOCK_BOOTTIME ns> <CLOCK_REALTIME s.ns>
 *
 * CNTVCT_EL0 is the counter 6boot read (as CNTPCT_EL0) when it armed the watchdog (xboot,watchdog in
 * /chosen). Linux at EL2 clears CNTVOFF_EL2 (arch/arm64/include/asm/el2_setup.h), so the two are the
 * same count. Its rate is CNTFRQ_EL0, which the header logs: 1 GHz on the J713's M4 (24 MHz in the
 * build VM), not the watchdog's 24 MHz. The header also says where CLOCK_BOOTTIME 0 falls on the
 * counter. The last line of a boot that a reset ends is the last heartbeat that reached the disk; the
 * reset came after it. A boot that is shut down ends with "stopped".
 *
 * Who has the watchdog open: every half second it looks through every process's open files (/proc/<pid>/fd)
 * for a path starting with /dev/watchdog, and logs what it finds at the start and whenever it changes:
 *
 *   w <CNTVCT_EL0> <CLOCK_BOOTTIME ns> none | pid=<pid> comm=<comm> fd=<fd> <path> ...
 *
 * The kernel's log (/dev/kmsg, as root): the first record still in it, every record about a watchdog
 * (from the start of the boot, then as they come), and marks that tie the kernel log's own clock to the
 * counter. A mark is a line the heartbeat writes to /dev/kmsg between two counter reads; the kernel stamps
 * it with its own clock, which the heartbeat reads back:
 *
 *   kfirst <seq> <ts us> <message>     the oldest record still in the kernel log (seq 0: nothing lost)
 *   k <seq> <ts us> <message>          a record whose text has "watchdog" or "wdt"
 *   klost                              the kernel log overwrote records before they were read
 *   m <ts us> <CNTVCT_EL0 before> <CNTVCT_EL0 after>   a mark, at the start and then every 10 s
 *
 *   heartbeat [DIR [WATCHDOG_PATH_PREFIX]]
 *
 * (the prefix, /dev/watchdog by default, is there for the host test)
 */
#define _GNU_SOURCE
#include <dirent.h>
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

static unsigned long long cntfrq(void)
{
    unsigned long long v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

/* the counter value when CLOCK_BOOTTIME was 0: cnt minus boottime in counter ticks, without overflow */
static long long boottime0(unsigned long long cnt, long long boot_ns, unsigned long long frq)
{
    return (long long)cnt - (long long)((__int128)boot_ns * frq / 1000000000);
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

/* write without fsync; put() also fsyncs */
static void emit(int fd, const char *s)
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
}

static void put(int fd, const char *s)
{
    emit(fd, s);
    fsync(fd);
}

/* append to out[cap] at *n, never past the end */
static void add(char *out, size_t cap, size_t *n, const char *fmt, const char *a, const char *b, const char *c,
                const char *d)
{
    if (*n + 1 >= cap)
        return;
    int k = snprintf(out + *n, cap - *n, fmt, a, b, c, d);
    if (k > 0)
        *n = *n + (size_t)k < cap ? *n + (size_t)k : cap - 1;
}

/* every open file under prefix, in every process: " pid=… comm=… fd=… path" each, or " none" */
static void openers(const char *prefix, char *out, size_t cap)
{
    size_t n = 0, plen = strlen(prefix);
    DIR *p = opendir("/proc");
    out[0] = 0;
    if (!p) {
        snprintf(out, cap, " (no /proc)");
        return;
    }
    for (struct dirent *e; (e = readdir(p));) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        char fdd[300], comm[64];
        snprintf(fdd, sizeof(fdd), "/proc/%s/fd", e->d_name);
        DIR *f = opendir(fdd);
        if (!f)
            continue;
        comm[0] = 0;
        for (struct dirent *g; (g = readdir(f));) {
            char link[600], target[512];
            if (g->d_name[0] == '.')
                continue;
            snprintf(link, sizeof(link), "%s/%s", fdd, g->d_name);
            ssize_t k = readlink(link, target, sizeof(target) - 1);
            if (k <= 0)
                continue;
            target[k] = 0;
            if (strncmp(target, prefix, plen))
                continue;
            if (!comm[0]) {
                char cp[300];
                snprintf(cp, sizeof(cp), "/proc/%s/comm", e->d_name);
                slurp(cp, comm, sizeof(comm));
            }
            add(out, cap, &n, " pid=%s comm=%s fd=%s %s", e->d_name, comm, g->d_name, target);
        }
        closedir(f);
    }
    closedir(p);
    if (!n)
        snprintf(out, cap, " none");
}

/* the next unsigned decimal at *s, then past one separator; -1 if there is none (no sscanf: it would tie the
 * binary to the build machine's glibc 2.38 and later) */
static int num(const char **s, unsigned long long *v)
{
    const char *p = *s;
    if (*p < '0' || *p > '9')
        return -1;
    for (*v = 0; *p >= '0' && *p <= '9'; p++)
        *v = *v * 10 + (unsigned long long)(*p - '0');
    *s = *p ? p + 1 : p;
    return 0;
}

/*
 * Read every kernel log record not yet read (kmsg is non-blocking) and log the ones about a watchdog, and
 * the first record of all. A record that is the text mark (when mark is not NULL) sets *mark_us to its
 * timestamp. Records the kernel overwrote before they were read show as one klost line.
 */
static void kmsg_drain(int kmsg, int fd, const char *mark, long long *mark_us)
{
    static int first = 1;
    static char rec[8193], line[8400];
    for (;;) {
        ssize_t k = read(kmsg, rec, sizeof(rec) - 1);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EPIPE) {
                emit(fd, "klost\n");
                continue;
            }
            break; /* EAGAIN: nothing more, or an error */
        }
        rec[k] = 0;
        /* "<prio>,<seq>,<ts us>,<flags>[,...];<text>\n" then " KEY=value" lines */
        char *text = strchr(rec, ';');
        const char *h = rec;
        unsigned long long prio, seq, ts;
        if (!text || num(&h, &prio) || num(&h, &seq) || num(&h, &ts))
            continue;
        text++;
        char *nl = strchr(text, '\n');
        if (nl)
            *nl = 0;
        if (first) {
            first = 0;
            snprintf(line, sizeof(line), "kfirst %llu %llu %.200s\n", seq, ts, text);
            emit(fd, line);
        }
        if (mark && mark_us && !strcmp(text, mark)) {
            *mark_us = (long long)ts;
            continue;
        }
        if (strcasestr(text, "watchdog") || strcasestr(text, "wdt")) {
            snprintf(line, sizeof(line), "k %llu %llu %s\n", seq, ts, text);
            emit(fd, line);
        }
    }
    fsync(fd);
}

/* one mark: the kernel's timestamp of a line written between two counter reads */
static void kmsg_mark(int kmsg, int fd, int n)
{
    char text[64], msg[80], line[160];
    long long us = -1;
    snprintf(text, sizeof(text), "6boot-heartbeat: mark %d", n);
    snprintf(msg, sizeof(msg), "<14>%s\n", text);
    kmsg_drain(kmsg, fd, NULL, NULL); /* what came before the mark */
    unsigned long long c1 = cntvct();
    ssize_t w = write(kmsg, msg, strlen(msg));
    unsigned long long c2 = cntvct();
    if (w < 0) {
        snprintf(line, sizeof(line), "m - %llu %llu write: %s\n", c1, c2, strerror(errno));
        put(fd, line);
        return;
    }
    kmsg_drain(kmsg, fd, text, &us);
    if (us < 0)
        snprintf(line, sizeof(line), "m - %llu %llu not read back\n", c1, c2);
    else
        snprintf(line, sizeof(line), "m %lld %llu %llu\n", us, c1, c2);
    put(fd, line);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/var/lib/6boot-heartbeat";
    const char *wdpath = argc > 2 ? argv[2] : "/dev/watchdog";
    char id[64], path[256], line[4096], buf[3800];
    static char seen[4096], now[4096];
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
    /* the counter against CLOCK_BOOTTIME: its rate, and where boottime 0 falls on it */
    unsigned long long f = cntfrq(), c = cntvct();
    long long b = ns(CLOCK_BOOTTIME);
    snprintf(line, sizeof(line), "clock cntfrq=%llu cntvct=%llu boottime_ns=%lld boottime0_cnt=%lld\n", f, c, b,
             boottime0(c, b, f));
    put(fd, line);
    snprintf(line, sizeof(line), "watchdog path prefix: %s\n", wdpath);
    put(fd, line);
    int kmsg = open("/dev/kmsg", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (kmsg < 0) {
        snprintf(line, sizeof(line), "kmsg: %s\n", strerror(errno));
        put(fd, line);
    } else {
        kmsg_mark(kmsg, fd, 0);
    }

    for (unsigned long tick = 0; !stop; tick++) {
        struct timespec r;
        unsigned long long cv = cntvct();
        long long bt = ns(CLOCK_BOOTTIME);
        clock_gettime(CLOCK_REALTIME, &r);
        snprintf(line, sizeof(line), "t %llu %lld %lld.%09ld\n", cv, bt, (long long)r.tv_sec, r.tv_nsec);
        put(fd, line);
        openers(wdpath, now, sizeof(now));
        if (!tick || strcmp(now, seen)) {
            snprintf(line, sizeof(line), "w %llu %lld%s\n", cv, bt, now);
            put(fd, line);
            memcpy(seen, now, sizeof(seen));
        }
        if (kmsg >= 0) {
            if (tick && tick % 20 == 0)
                kmsg_mark(kmsg, fd, (int)(tick / 20));
            else
                kmsg_drain(kmsg, fd, NULL, NULL);
        }
        struct timespec half = {0, 500000000L};
        while (nanosleep(&half, &half) && errno == EINTR && !stop)
            ;
    }
    snprintf(line, sizeof(line), "stopped %llu\n", cntvct());
    put(fd, line);
    close(fd);
    return 0;
}

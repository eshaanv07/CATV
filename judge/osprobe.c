/*
 * osprobe.c - Problem 4 (Cat-V Minimalist Stack) primitive conformance probe.
 * SAMPLE VERSION. The judges' copy is the same program with a different
 * random seed for every run; verify.sh already rewrites OSPROBE_SEED each
 * time it builds the assets disk, exactly like the judges do.
 *
 *   osprobe all               run the six L2 checks
 *   osprobe <check>           proc | pipe | signal | mmap | thread | clock
 *   osprobe smp <iters>       L4: one busy worker, then two in parallel
 *   osprobe burn <iters>      one busy loop between markers (the harness
 *                             uses it to size the L4 work on the host)
 *   (osprobe --child <x>      internal, used by the proc check)
 *
 * Each check prints "PASS <check> <value>" where <value> is 16 hex digits
 * that depend on the seed and can only be right if the primitive works,
 * or "FAIL <check> <reason>". The host computes the expected values; the
 * guest never sees them.
 *
 * Portable POSIX C (C99 + pthreads). Builds with gcc, clang or tcc:
 *     cc -O2 -o osprobe osprobe.c -lpthread
 * Optional: -DOSPROBE_TMP=\"/some/writable/dir\" (default "/tmp").
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define OSPROBE_SEED 0x5eed5eed5eed5eedULL /* replaced by verify.sh */
#ifndef OSPROBE_TMP
#define OSPROBE_TMP "/tmp"
#endif

typedef uint64_t u64;
static char *self_path;

static u64 mix(u64 x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static u64 token(int check) { return mix(OSPROBE_SEED ^ ((u64)(check + 1) << 40)); }

/* xorshift64* stream: 8 bytes per step, little-endian */
static void stream(u64 seed, unsigned char *out, size_t n)
{
    u64 x = seed | 1;
    for (size_t i = 0; i < n; i += 8) {
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        u64 v = x * 0x2545F4914F6CDD1DULL;
        for (int b = 0; b < 8 && i + (size_t)b < n; b++) out[i + (size_t)b] = (unsigned char)(v >> (8 * b));
    }
}

static u64 fnv(const unsigned char *p, size_t n)
{
    u64 h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

static int xwrite(int fd, const void *b, size_t n)
{
    const char *p = b;
    while (n) { ssize_t r = write(fd, p, n); if (r < 0) { if (errno == EINTR) continue; return -1; } p += r; n -= (size_t)r; }
    return 0;
}

static int xread(int fd, void *b, size_t n)
{
    char *p = b;
    while (n) { ssize_t r = read(fd, p, n); if (r < 0) { if (errno == EINTR) continue; return -1; } if (r == 0) return -1; p += r; n -= (size_t)r; }
    return 0;
}

static void pass(const char *c, u64 v) { printf("PASS %s %016llx\n", c, (unsigned long long)v); fflush(stdout); }
static int failc(const char *c, const char *why) { printf("FAIL %s %s (%s)\n", c, why, strerror(errno)); fflush(stdout); return 1; }

/* 0: fork / exec / wait */
static int c_proc(void)
{
    u64 t = token(0);
    char arg[32];
    snprintf(arg, sizeof arg, "%llu", (unsigned long long)t);
    pid_t p = fork();
    if (p < 0) return failc("proc", "fork");
    if (p == 0) {
        execl(self_path, self_path, "--child", arg, (char *)NULL);
        _exit(255);
    }
    int st;
    if (waitpid(p, &st, 0) != p) return failc("proc", "waitpid");
    if (!WIFEXITED(st)) return failc("proc", "child did not exit normally");
    pass("proc", (mix(t) & ~0xffULL) | (u64)(WEXITSTATUS(st) & 0xff));
    return 0;
}

/* 1: pipes, in both directions between two processes */
#define PIPE_BYTES 65536
static int c_pipe(void)
{
    static unsigned char buf[PIPE_BYTES];
    int to[2], back[2];
    if (pipe(to) || pipe(back)) return failc("pipe", "pipe");
    pid_t p = fork();
    if (p < 0) return failc("pipe", "fork");
    if (p == 0) {
        close(to[1]); close(back[0]);
        static unsigned char in[PIPE_BYTES];
        if (xread(to[0], in, sizeof in)) _exit(1);
        u64 h = fnv(in, sizeof in);
        if (xwrite(back[1], &h, sizeof h)) _exit(1);
        _exit(0);
    }
    close(to[0]); close(back[1]);
    stream(token(1), buf, sizeof buf);
    if (xwrite(to[1], buf, sizeof buf)) return failc("pipe", "write");
    close(to[1]);
    u64 h = 0;
    if (xread(back[0], &h, sizeof h)) return failc("pipe", "read");
    waitpid(p, NULL, 0);
    pass("pipe", h);
    return 0;
}

/* 2: signals sent by another process, one at a time */
static volatile sig_atomic_t sig_count;
static volatile u64 sig_acc;
static void on_usr1(int s) { (void)s; sig_count++; sig_acc = mix(sig_acc + (u64)sig_count); }
static int c_signal(void)
{
    u64 t = token(2);
    int n = 3 + (int)(t % 5);
    sig_acc = t; sig_count = 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL)) return failc("signal", "sigaction");
    int ack[2];
    if (pipe(ack)) return failc("signal", "pipe");
    pid_t parent = getpid(), p = fork();
    if (p < 0) return failc("signal", "fork");
    if (p == 0) {
        close(ack[1]);
        char c;
        for (int i = 0; i < n; i++) { kill(parent, SIGUSR1); if (xread(ack[0], &c, 1)) _exit(1); }
        _exit(0);
    }
    close(ack[0]);
    for (int i = 0; i < n; i++) {
        time_t start = time(NULL);
        while (sig_count <= i) { if (time(NULL) - start > 10) return failc("signal", "signal not delivered"); usleep(1000); }
        if (xwrite(ack[1], "a", 1)) return failc("signal", "ack");
    }
    waitpid(p, NULL, 0);
    pass("signal", sig_acc);
    return 0;
}

/* 3: anonymous mmap, shared anonymous mmap across fork, and file mmap */
#define MAP_BYTES (1u << 20)
static int c_mmap(void)
{
    u64 t = token(3);
    unsigned char *a = mmap(NULL, MAP_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (a == MAP_FAILED) return failc("mmap", "anonymous mmap");
    stream(t, a, MAP_BYTES);
    u64 ha = fnv(a, MAP_BYTES);

    char path[256];
    snprintf(path, sizeof path, "%s/osprobe.%d.dat", OSPROBE_TMP, (int)getpid());
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return failc("mmap", "open " OSPROBE_TMP);
    if (xwrite(fd, a, MAP_BYTES)) return failc("mmap", "write file");
    unsigned char *f = mmap(NULL, MAP_BYTES, PROT_READ, MAP_SHARED, fd, 0);
    if (f == MAP_FAILED) return failc("mmap", "file mmap");
    u64 hf = fnv(f, MAP_BYTES);
    munmap(f, MAP_BYTES); close(fd); unlink(path);

    volatile u64 *sh = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if ((void *)sh == MAP_FAILED) return failc("mmap", "shared anonymous mmap");
    sh[0] = 0;
    pid_t p = fork();
    if (p == 0) { sh[0] = mix(t + 1); _exit(0); }
    waitpid(p, NULL, 0);
    pass("mmap", ha + 3 * hf + sh[0]);
    munmap(a, MAP_BYTES);
    return 0;
}

/* 4: threads */
#define THR 4
#define THR_BYTES (1u << 18)
static unsigned char thr_buf[THR_BYTES];
static u64 thr_out[THR];
static void *thr_main(void *arg)
{
    long i = (long)arg;
    thr_out[i] = fnv(thr_buf + (size_t)i * (THR_BYTES / THR), THR_BYTES / THR);
    return NULL;
}
static int c_thread(void)
{
    stream(token(4), thr_buf, THR_BYTES);
    pthread_t th[THR];
    for (long i = 0; i < THR; i++) if (pthread_create(&th[i], NULL, thr_main, (void *)i)) return failc("thread", "pthread_create");
    u64 v = 0;
    for (int i = 0; i < THR; i++) { pthread_join(th[i], NULL); v += thr_out[i] * (u64)(i + 1); }
    pass("thread", v);
    return 0;
}

/* 5: monotonic clock */
static int c_clock(void)
{
    struct timespec a, b, s = { 0, 50000000 };
    long long prev = -1;
    for (int i = 0; i < 2000; i++) {
        if (clock_gettime(CLOCK_MONOTONIC, &a)) return failc("clock", "clock_gettime");
        long long ns = (long long)a.tv_sec * 1000000000LL + a.tv_nsec;
        if (ns < prev) { errno = 0; return failc("clock", "went backwards"); }
        prev = ns;
    }
    clock_gettime(CLOCK_MONOTONIC, &a);
    nanosleep(&s, NULL);
    clock_gettime(CLOCK_MONOTONIC, &b);
    long long d = ((long long)b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec);
    if (d < 50000000LL || d > 10000000000LL) { errno = 0; return failc("clock", "nanosleep(50 ms) measured wrongly"); }
    pass("clock", mix(token(5)));
    return 0;
}

/* L4: busy workers. Timing is measured on the HOST between the markers. */
static volatile u64 sink;
static void burn(u64 iters)
{
    u64 x = 88172645463325252ULL;
    for (u64 i = 0; i < iters; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; }
    sink = x;
}
static int c_burn(u64 iters)
{
    printf("BURN-BEGIN\n"); fflush(stdout);
    burn(iters);
    printf("BURN-END\n"); fflush(stdout);
    return 0;
}
static int c_smp(u64 iters)
{
    if (!iters) iters = 200000000ULL;
    printf("SMP iters=%llu\n", (unsigned long long)iters); fflush(stdout);
    printf("SMP-ONE-BEGIN\n"); fflush(stdout);
    pid_t p = fork();
    if (p == 0) { burn(iters); _exit(0); }
    waitpid(p, NULL, 0);
    printf("SMP-ONE-END\n"); fflush(stdout);
    printf("SMP-TWO-BEGIN\n"); fflush(stdout);
    pid_t q[2];
    for (int i = 0; i < 2; i++) { q[i] = fork(); if (q[i] == 0) { burn(iters); _exit(0); } }
    for (int i = 0; i < 2; i++) waitpid(q[i], NULL, 0);
    printf("SMP-TWO-END\n"); fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    self_path = argv[0];
    if (argc == 3 && strcmp(argv[1], "--child") == 0) {
        u64 t = strtoull(argv[2], NULL, 10);
        return (int)(mix(t) & 0xff);
    }
    const char *what = argc > 1 ? argv[1] : "all";
    if (strcmp(what, "smp") == 0) return c_smp(argc > 2 ? strtoull(argv[2], NULL, 10) : 0);
    if (strcmp(what, "burn") == 0) return c_burn(argc > 2 ? strtoull(argv[2], NULL, 10) : 10000000ULL);
    int all = strcmp(what, "all") == 0, bad = 0, ran = 0;
    if (all || !strcmp(what, "proc"))   { bad += c_proc(); ran++; }
    if (all || !strcmp(what, "pipe"))   { bad += c_pipe(); ran++; }
    if (all || !strcmp(what, "signal")) { bad += c_signal(); ran++; }
    if (all || !strcmp(what, "mmap"))   { bad += c_mmap(); ran++; }
    if (all || !strcmp(what, "thread")) { bad += c_thread(); ran++; }
    if (all || !strcmp(what, "clock"))  { bad += c_clock(); ran++; }
    if (!ran) { fprintf(stderr, "usage: osprobe all|proc|pipe|signal|mmap|thread|clock|smp <iters>|burn <iters>\n"); return 2; }
    printf("OSPROBE-DONE %d\n", bad);
    fflush(stdout);
    return bad ? 1 : 0;
}

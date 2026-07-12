/*
 * TLB shootdown generator for validating TLBanalyser: reader threads pinned
 * to several CPUs keep the mm active there, while the main thread writes and
 * MADV_DONTNEEDs the mapping - every cycle forces remote shootdown IPIs.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

#define SZ (16UL << 20)
static char *buf;
static volatile int stop;
static volatile unsigned long sink;

static void *toucher(void *arg)
{
    int cpu = (int)(long)arg;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);
    unsigned long s = 0;
    while (!stop)
        for (size_t i = 0; i < SZ && !stop; i += 4096) s += buf[i];
    sink = s;
    return NULL;
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 2) {
        fprintf(stderr, "shootgen: needs >=2 CPUs for remote shootdowns\n");
        return 1;
    }
    printf("shootgen pid %d starting (%d s)\n", getpid(), secs);
    fflush(stdout);
    buf = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    memset(buf, 1, SZ);
    int nth = ncpu - 1;
    if (nth > 4) nth = 4;
    pthread_t th[4];
    for (long i = 0; i < nth; i++) {
        int cpu = (int)((i * 2 + 2) % ncpu);
        if (cpu == 0) cpu = 1;
        pthread_create(&th[i], NULL, toucher, (void *)(long)cpu);
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    sched_setaffinity(0, sizeof set, &set);
    unsigned long iters = 0;
    for (int t = 0; t < secs * 10; t++) {
        memset(buf, 1, SZ);                 /* repopulate PTEs   */
        madvise(buf, SZ, MADV_DONTNEED);    /* zap -> IPI flush  */
        iters++;
        usleep(100 * 1000);
    }
    stop = 1;
    for (int i = 0; i < nth; i++) pthread_join(th[i], NULL);
    printf("shootgen pid %d: %lu madvise cycles of %lu MB done\n",
           getpid(), iters, SZ >> 20);
    return 0;
}

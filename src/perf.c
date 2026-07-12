#include "tlba.h"
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>

static int perf_open(struct perf_event_attr *a, pid_t pid, int cpu, int gfd, unsigned long flags)
{
    return (int)syscall(SYS_perf_event_open, a, pid, cpu, gfd, flags);
}

struct rd { uint64_t value, enabled, running; };

struct pmu_state {
    int ncpu;
    int fd[EV__N][MAX_CPUS];
    struct rd prev[EV__N][MAX_CPUS];
    bool avail[EV__N];
    char l2_desc[96];
};

static int sysfs_pmu_type(const char *name)
{
    char p[128], buf[32];
    snprintf(p, sizeof p, "/sys/bus/event_source/devices/%s/type", name);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    int t = fgets(buf, sizeof buf, f) ? atoi(buf) : -1;
    fclose(f);
    return t;
}

static void attr_common(struct perf_event_attr *a)
{
    memset(a, 0, sizeof *a);
    a->size = sizeof *a;
    a->read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    a->inherit = 0;
    a->exclude_guest = 1;
}

#define HWC(cache, op, res) ((cache) | ((op) << 8) | ((res) << 16))

/* open one event type on every CPU; returns number of CPUs where it opened */
static int open_all(struct pmu_state *p, enum pmu_ev ev, struct perf_event_attr *a)
{
    int ok = 0;
    for (int c = 0; c < p->ncpu; c++) {
        p->fd[ev][c] = perf_open(a, -1, c, -1, PERF_FLAG_FD_CLOEXEC);
        if (p->fd[ev][c] >= 0) ok++;
    }
    return ok;
}

static void close_all(struct pmu_state *p, enum pmu_ev ev)
{
    for (int c = 0; c < p->ncpu; c++)
        if (p->fd[ev][c] >= 0) { close(p->fd[ev][c]); p->fd[ev][c] = -1; }
}

struct pmu_state *pmu_open(const struct topology *t, char *errbuf, size_t errlen)
{
    struct pmu_state *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->ncpu = t->ncpu;
    for (int e = 0; e < EV__N; e++)
        for (int c = 0; c < MAX_CPUS; c++) p->fd[e][c] = -1;

    struct perf_event_attr a;

    /* generic hardware + hw-cache events (portable Intel/AMD) */
    static const struct { enum pmu_ev ev; uint32_t type; uint64_t config; } gen[] = {
        { EV_INSTR,     PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS },
        { EV_CYCLES,    PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES },
        { EV_L1D_ACC,   PERF_TYPE_HW_CACHE, HWC(PERF_COUNT_HW_CACHE_L1D,  PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_ACCESS) },
        { EV_L1D_MISS,  PERF_TYPE_HW_CACHE, HWC(PERF_COUNT_HW_CACHE_L1D,  PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS) },
        { EV_L1I_MISS,  PERF_TYPE_HW_CACHE, HWC(PERF_COUNT_HW_CACHE_L1I,  PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS) },
        { EV_DTLB_MISS, PERF_TYPE_HW_CACHE, HWC(PERF_COUNT_HW_CACHE_DTLB, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS) },
        { EV_ITLB_MISS, PERF_TYPE_HW_CACHE, HWC(PERF_COUNT_HW_CACHE_ITLB, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS) },
    };
    int opened_any = 0;
    for (size_t i = 0; i < sizeof gen / sizeof gen[0]; i++) {
        attr_common(&a);
        a.type = gen[i].type;
        a.config = gen[i].config;
        int n = open_all(p, gen[i].ev, &a);
        p->avail[gen[i].ev] = n > 0;
        opened_any += n;
    }
    if (!opened_any) {
        snprintf(errbuf, errlen,
                 "perf_event_open failed (%s) - run as root / CAP_PERFMON, "
                 "check /proc/sys/kernel/perf_event_paranoid", strerror(errno));
        pmu_close(p);
        return NULL;
    }

    /*
     * L2 cache: no generic perf event exists, use vendor raw encodings.
     *  Intel Xeon (SKX..GNR):  L2_RQSTS.REFERENCES  event=0x24 umask=0xff
     *                          L2_RQSTS.MISS        event=0x24 umask=0x3f
     *  AMD EPYC (Zen1..Zen5):  L2RequestG1 (all)    event=0x60 umask=0xff
     *                          L2CacheReqStat.IcDcMissInL2 event=0x64 umask=0x09
     */
    int pmu_type = sysfs_pmu_type("cpu");            /* Xeon / EPYC */
    bool hybrid_fallback = false;
    if (pmu_type < 0) {                              /* hybrid dev box: P-cores only */
        pmu_type = sysfs_pmu_type("cpu_core");
        hybrid_fallback = pmu_type >= 0;
    }
    uint64_t l2_acc = 0, l2_miss = 0;
    const char *desc = NULL;
    if (t->vendor == VENDOR_INTEL) {
        l2_acc  = 0x24 | (0xffULL << 8);
        l2_miss = 0x24 | (0x3fULL << 8);
        desc = "Intel L2_RQSTS.REFERENCES / L2_RQSTS.MISS";
    } else if (t->vendor == VENDOR_AMD) {
        l2_acc  = 0x60 | (0xffULL << 8);
        l2_miss = 0x64 | (0x09ULL << 8);
        desc = "AMD L2RequestG1.All / L2CacheReqStat.IcDcMissInL2";
    }
    if (pmu_type >= 0 && desc) {
        attr_common(&a);
        a.type = (uint32_t)pmu_type;
        a.config = l2_acc;
        int n1 = open_all(p, EV_L2_ACC, &a);
        a.config = l2_miss;
        int n2 = open_all(p, EV_L2_MISS, &a);
        if (n1 > 0 && n2 > 0) {
            p->avail[EV_L2_ACC] = p->avail[EV_L2_MISS] = true;
            snprintf(p->l2_desc, sizeof p->l2_desc, "%s%s", desc,
                     hybrid_fallback ? " (P-cores only)" : "");
        } else {
            close_all(p, EV_L2_ACC);
            close_all(p, EV_L2_MISS);
        }
    }
    if (!p->avail[EV_L2_ACC]) {
        /* last resort: generic last-level-cache events */
        attr_common(&a);
        a.type = PERF_TYPE_HW_CACHE;
        a.config = HWC(PERF_COUNT_HW_CACHE_LL, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_ACCESS);
        int n1 = open_all(p, EV_L2_ACC, &a);
        a.config = HWC(PERF_COUNT_HW_CACHE_LL, PERF_COUNT_HW_CACHE_OP_READ, PERF_COUNT_HW_CACHE_RESULT_MISS);
        int n2 = open_all(p, EV_L2_MISS, &a);
        if (n1 > 0 && n2 > 0) {
            p->avail[EV_L2_ACC] = p->avail[EV_L2_MISS] = true;
            snprintf(p->l2_desc, sizeof p->l2_desc, "generic LLC events (vendor L2 PMU unavailable)");
        }
    }

    /* prime the prev readings */
    struct pmu_cpu_rates dummy[MAX_CPUS];
    pmu_read(p, dummy, 1.0);
    return p;
}

void pmu_read(struct pmu_state *p, struct pmu_cpu_rates *out, double dt)
{
    if (dt <= 0) dt = 1;
    for (int c = 0; c < p->ncpu; c++) {
        memset(&out[c], 0, sizeof out[c]);
        for (int e = 0; e < EV__N; e++) {
            int fd = p->fd[e][c];
            if (fd < 0) continue;
            struct rd r;
            if (read(fd, &r, sizeof r) != sizeof r) continue;
            struct rd *pv = &p->prev[e][c];
            uint64_t dv = r.value - pv->value;
            uint64_t de = r.enabled - pv->enabled;
            uint64_t dr = r.running - pv->running;
            double scale = 1.0;
            if (dr > 0 && de > dr) { scale = (double)de / (double)dr; out[c].scaled = true; }
            else if (dr == 0 && de > 0) { dv = 0; } /* not scheduled at all this interval */
            out[c].v[e] = (double)dv * scale / dt;
            out[c].ok[e] = true;
            *pv = r;
        }
    }
}

bool pmu_event_available(struct pmu_state *p, enum pmu_ev ev) { return p->avail[ev]; }
const char *pmu_l2_source(struct pmu_state *p) { return p->l2_desc[0] ? p->l2_desc : "unavailable"; }

void pmu_close(struct pmu_state *p)
{
    if (!p) return;
    for (int e = 0; e < EV__N; e++) close_all(p, e);
    free(p);
}

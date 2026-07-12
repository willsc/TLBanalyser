/*
 * TLBanalyser - htop-style TLB shootdown / IPI / cache visibility tool
 * Targeted at Intel Xeon and AMD EPYC class processors.
 */
#ifndef TLBA_H
#define TLBA_H

#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TLBA_VERSION "1.0.0"

#define MAX_CPUS      1024
#define COMM_LEN      16
#define REASON_MAX    8   /* kernel currently defines 0..5 */

enum cpu_vendor { VENDOR_UNKNOWN = 0, VENDOR_INTEL, VENDOR_AMD };

/* tlb_flush_reason (arch/x86, include/trace/events/tlb.h) */
enum tlb_reason {
    R_TASK_SWITCH = 0,   /* flush on task switch (lazy TLB)          */
    R_REMOTE_RECV = 1,   /* remote shootdown - receiver side of IPI  */
    R_LOCAL       = 2,   /* local shootdown                          */
    R_LOCAL_MM    = 3,   /* local MM shootdown                       */
    R_REMOTE_SEND = 4,   /* remote shootdown - IPI sender (culprit!) */
    R_WRONG_CPU   = 5,   /* remote wrong CPU                         */
};

/* ---------------- topology ---------------- */

struct cache_desc {
    int  level;
    char type;          /* 'D' data, 'I' instruction, 'U' unified */
    long size_kb;
    int  sharing;       /* number of CPUs sharing one instance */
    int  line_size;
    int  ways;
};

struct topology {
    int  ncpu;                 /* online CPUs (0..ncpu-1 assumed) */
    int  nsocket;
    int  nnode;
    int  ncore;                /* physical cores */
    enum cpu_vendor vendor;
    int  family, model;
    char model_name[128];
    int  pkg_of[MAX_CPUS];
    int  node_of[MAX_CPUS];
    int  core_of[MAX_CPUS];
    struct cache_desc l1d, l1i, l2, l3;
    bool has_l3;
};

int topology_read(struct topology *t);

/* ---------------- /proc counters ---------------- */

struct cpu_ticks { uint64_t user, nice, system, idle, iowait, irq, softirq, steal, guest; };

struct proc_sample {
    struct cpu_ticks ticks[MAX_CPUS];
    uint64_t irq_tlb[MAX_CPUS];   /* TLB shootdowns received (exact, /proc/interrupts) */
    uint64_t irq_res[MAX_CPUS];   /* rescheduling IPIs   */
    uint64_t irq_cal[MAX_CPUS];   /* function-call IPIs  */
    bool     have_tlb, have_res, have_cal;
    /* vmstat: known drivers of TLB shootdowns */
    uint64_t thp_collapse, thp_split_pmd, thp_deferred_split, thp_migration;
    uint64_t compact_stall, compact_daemon_wake, pgmigrate, numa_pte_updates,
             numa_pages_migrated, pgsteal, pgscan, ksm_cow; /* pgsteal/pgscan: reclaim */
    uint64_t unmap_pgs; /* not available; kept 0 */
};

struct cpu_load { float user, nice, system, irq, softirq, steal, iowait, busy; };

struct proc_rates {
    struct cpu_load load[MAX_CPUS];
    double tlb_recv[MAX_CPUS], res[MAX_CPUS], cal[MAX_CPUS];   /* per second */
    double tlb_recv_tot, res_tot, cal_tot;
    double thp_collapse, thp_split_pmd, thp_deferred_split, thp_migration,
           compact_stall, compact_daemon_wake, pgmigrate, numa_pte_updates,
           numa_pages_migrated, pgsteal, pgscan;
};

void proc_sample_read(struct proc_sample *s, int ncpu);
void proc_rates_calc(struct proc_rates *r, const struct proc_sample *prev,
                     const struct proc_sample *cur, int ncpu, double dt);

/* ---------------- perf PMU counters ---------------- */

enum pmu_ev {
    EV_INSTR = 0,
    EV_CYCLES,
    EV_L1D_ACC, EV_L1D_MISS,
    EV_L1I_MISS,
    EV_L2_ACC,  EV_L2_MISS,
    EV_DTLB_MISS, EV_ITLB_MISS,
    EV__N
};

struct pmu_cpu_rates {
    double v[EV__N];        /* events per second, multiplex-scaled */
    bool   ok[EV__N];
    bool   scaled;          /* true if any event was multiplexed   */
};

struct pmu_state;
struct pmu_state *pmu_open(const struct topology *t, char *errbuf, size_t errlen);
void pmu_read(struct pmu_state *p, struct pmu_cpu_rates *out, double dt);
bool pmu_event_available(struct pmu_state *p, enum pmu_ev ev);
const char *pmu_l2_source(struct pmu_state *p); /* description of L2 event used */
void pmu_close(struct pmu_state *p);

/* ---------------- tlb_flush tracepoint attribution ---------------- */

struct pid_stat {
    int      pid;
    char     comm[COMM_LEN + 1];
    bool     kthread;
    uint64_t cnt[REASON_MAX];      /* events this interval          */
    uint64_t cum[REASON_MAX];      /* cumulative since start/reset  */
    uint64_t pages, pages_cum;     /* pages flushed by send events  */
    uint32_t origin_sym[4];        /* top kernel origin functions   */
    uint64_t origin_cnt[4];
    uint64_t last_seen;
};

struct origin_stat { uint32_t sym; uint64_t cnt, cum; };

struct trace_state;
struct trace_state *trace_open(int ncpu, char *errbuf, size_t errlen);
/* drain ring buffers; cheap, call frequently */
void trace_drain(struct trace_state *ts);
/* close out an interval: fills sorted views. returns #pids */
int  trace_interval(struct trace_state *ts, struct pid_stat **pids_out,
                    struct origin_stat **origins_out, int *norigin,
                    uint64_t reason_tot[REASON_MAX],
                    uint64_t reason_cum[REASON_MAX],
                    uint64_t *pages_tot, uint64_t *lost, uint64_t *lost_cum);
const char *trace_symname(struct trace_state *ts, uint32_t sym);
/* per-sending-CPU IPI-send event counts for the last closed interval */
const uint64_t *trace_cpu_sends(struct trace_state *ts);
/* context-channel (task-switch/IPI-recv) drops; cosmetic, recv totals stay
 * exact via /proc/interrupts */
void trace_lost_ctx(struct trace_state *ts, uint64_t *cur, uint64_t *cum);
void trace_reset_cum(struct trace_state *ts);
void trace_close(struct trace_state *ts);

/* ---------------- UI ---------------- */

struct snapshot {
    double dt;
    struct topology     *topo;
    struct proc_rates    proc;
    struct pmu_cpu_rates pmu[MAX_CPUS];
    bool   pmu_ok;
    char   pmu_err[256];
    const char *l2_desc;
    /* attribution */
    bool   trace_ok;
    char   trace_err[256];
    struct pid_stat    *pids;   int npid;
    struct origin_stat *origins; int norigin;
    uint64_t reason_tot[REASON_MAX], reason_cum[REASON_MAX];
    uint64_t pages_tot, lost, lost_cum, lost_ctx, lost_ctx_cum;
    struct trace_state *ts;     /* for symbol lookup */
    double uptime;
};

extern const char *tlb_reason_name[REASON_MAX];
extern const char *tlb_reason_short[REASON_MAX];

void ui_init(void);
void ui_done(void);
int  ui_key(int ch);          /* 0 none, 1 quit, 2 reset cumulative */
bool ui_paused(void);
void ui_draw(struct snapshot *s);

#endif

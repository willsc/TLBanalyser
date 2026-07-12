#include "tlba.h"
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <time.h>
#include <ncurses.h>

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void usage(const char *argv0)
{
    printf("TLBanalyser %s - htop-style TLB shootdown / IPI / L1+L2 cache analyser\n"
           "for Intel Xeon and AMD EPYC systems.\n\n"
           "usage: %s [options]\n"
           "  -d SEC    refresh interval in seconds (default 1.5)\n"
           "  -b        batch mode: plain-text output, no TUI (for logging)\n"
           "  -n N      batch mode: exit after N intervals\n"
           "  -t ROWS   batch mode: top-N processes to print (default 10)\n"
           "  -V        version\n"
           "  -h        this help\n\n"
           "Needs root (or CAP_PERFMON + CAP_SYS_ADMIN) for PMU counters and the\n"
           "tlb:tlb_flush tracepoint. /proc counters degrade gracefully without.\n",
           TLBA_VERSION, argv0);
}

static void batch_print(struct snapshot *s, int top)
{
    char tbuf[32];
    time_t now = time(NULL);
    strftime(tbuf, sizeof tbuf, "%H:%M:%S", localtime(&now));

    printf("=== %s  dt=%.2fs ===\n", tbuf, s->dt);
    printf("tlb_recv_ipi/s %.0f  resched_ipi/s %.0f  call_ipi/s %.0f",
           s->proc.tlb_recv_tot, s->proc.res_tot, s->proc.cal_tot);
    if (s->trace_ok) {
        printf("  send_ev/s %.0f  pages/s %.0f  lost %lu (ctx %lu)",
               (double)s->reason_tot[R_REMOTE_SEND] / s->dt,
               (double)s->pages_tot / s->dt, (unsigned long)s->lost,
               (unsigned long)s->lost_ctx);
    }
    printf("\n");
    if (s->pmu_ok) {
        double in = 0, cy = 0, l1a = 0, l1m = 0, l2a = 0, l2m = 0, dt_ = 0, it = 0;
        for (int i = 0; i < s->topo->ncpu; i++) {
            in += s->pmu[i].v[EV_INSTR];  cy += s->pmu[i].v[EV_CYCLES];
            l1a += s->pmu[i].v[EV_L1D_ACC]; l1m += s->pmu[i].v[EV_L1D_MISS];
            l2a += s->pmu[i].v[EV_L2_ACC];  l2m += s->pmu[i].v[EV_L2_MISS];
            dt_ += s->pmu[i].v[EV_DTLB_MISS]; it += s->pmu[i].v[EV_ITLB_MISS];
        }
        printf("ipc %.2f  l1d_miss%% %.2f  l2_miss%% %.2f  dtlb_mpki %.3f  itlb_mpki %.3f\n",
               cy > 0 ? in / cy : 0,
               l1a > 0 ? 100 * l1m / l1a : 0,
               l2a > 0 ? 100 * l2m / l2a : 0,
               in > 0 ? dt_ / (in / 1000) : 0,
               in > 0 ? it / (in / 1000) : 0);
    }
    if (s->trace_ok) {
        printf("reasons/s:");
        for (int r = 0; r < 6; r++)
            printf(" %s=%.0f", tlb_reason_short[r], (double)s->reason_tot[r] / s->dt);
        printf("\norigins:");
        for (int i = 0; i < s->norigin && i < 6; i++) {
            if (!s->origins[i].cnt) break;
            printf(" %s=%lu", trace_symname(s->ts, s->origins[i].sym),
                   (unsigned long)s->origins[i].cnt);
        }
        printf("\n%7s %-16s %8s %9s %7s %7s %7s %7s  %s\n",
               "PID", "COMM", "SEND/s", "PAGES/s", "LOCAL", "LOCMM", "SWTCH", "RECV", "ORIGIN");
        int shown = 0;
        for (int i = 0; i < s->npid && shown < top; i++) {
            struct pid_stat *p = &s->pids[i];
            uint64_t tot = 0;
            for (int r = 0; r < REASON_MAX; r++) tot += p->cnt[r];
            if (!tot) continue;
            uint32_t best = 0;
            uint64_t bc = 0;
            for (int k = 0; k < 4; k++)
                if (p->origin_cnt[k] > bc) { bc = p->origin_cnt[k]; best = p->origin_sym[k]; }
            printf("%7d %-16.16s %8.0f %9.0f %7.0f %7.0f %7.0f %7.0f  %s\n",
                   p->pid, p->comm,
                   p->cnt[R_REMOTE_SEND] / s->dt, p->pages / s->dt,
                   p->cnt[R_LOCAL] / s->dt, p->cnt[R_LOCAL_MM] / s->dt,
                   p->cnt[R_TASK_SWITCH] / s->dt, p->cnt[R_REMOTE_RECV] / s->dt,
                   best ? trace_symname(s->ts, best) : "-");
            shown++;
        }
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    double delay = 1.5;
    bool batch = false;
    int iterations = -1, top = 10, opt;

    while ((opt = getopt(argc, argv, "d:bn:t:Vh")) != -1) {
        switch (opt) {
        case 'd': delay = atof(optarg); if (delay < 0.2) delay = 0.2; break;
        case 'b': batch = true; break;
        case 'n': iterations = atoi(optarg); break;
        case 't': top = atoi(optarg); break;
        case 'V': printf("TLBanalyser %s\n", TLBA_VERSION); return 0;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    static struct topology topo;
    topology_read(&topo);

    static struct snapshot snap;
    snap.topo = &topo;

    struct pmu_state *pmu = pmu_open(&topo, snap.pmu_err, sizeof snap.pmu_err);
    snap.pmu_ok = pmu != NULL;
    snap.l2_desc = pmu ? pmu_l2_source(pmu) : "";

    struct trace_state *ts = trace_open(topo.ncpu, snap.trace_err, sizeof snap.trace_err);
    snap.trace_ok = ts != NULL;
    snap.ts = ts;

    static struct proc_sample ps[2];
    int cur = 0;
    proc_sample_read(&ps[cur], topo.ncpu);

    if (!batch) ui_init();

    double t_start = now_s(), t_last = t_start;
    bool have_frame = false;
    int done = 0;

    while (!g_stop) {                      /* rings drained by trace.c thread */
        double t = now_s();
        if (t - t_last >= delay) {
            double dt = t - t_last;
            t_last = t;
            cur ^= 1;
            proc_sample_read(&ps[cur], topo.ncpu);
            if (!batch && ui_paused()) continue;

            snap.dt = dt;
            snap.uptime = t - t_start;
            proc_rates_calc(&snap.proc, &ps[cur ^ 1], &ps[cur], topo.ncpu, dt);
            if (pmu) pmu_read(pmu, snap.pmu, dt);
            if (ts) {
                snap.npid = trace_interval(ts, &snap.pids, &snap.origins,
                                           &snap.norigin, snap.reason_tot,
                                           snap.reason_cum, &snap.pages_tot,
                                           &snap.lost, &snap.lost_cum);
                trace_lost_ctx(ts, &snap.lost_ctx, &snap.lost_ctx_cum);
            }
            have_frame = true;
            if (batch) {
                batch_print(&snap, top);
                if (iterations > 0 && ++done >= iterations) break;
            } else {
                ui_draw(&snap);
            }
        }

        if (batch) {
            usleep(50 * 1000);
        } else {
            int ch = getch();               /* 50ms timeout = drain cadence */
            if (ch == ERR) continue;
            int act = ui_key(ch);
            if (act == 1) break;
            if (act == 2 && ts) trace_reset_cum(ts);
            if (have_frame) ui_draw(&snap);
        }
    }

    if (!batch) ui_done();
    if (ts) trace_close(ts);
    if (pmu) pmu_close(pmu);
    return 0;
}

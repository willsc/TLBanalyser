#include "tlba.h"

/* ---- /proc/stat ---- */

static void read_stat(struct proc_sample *s, int ncpu)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "cpu", 3) || line[3] == ' ') continue;
        int c = atoi(line + 3);
        if (c < 0 || c >= ncpu) continue;
        struct cpu_ticks *t = &s->ticks[c];
        sscanf(line, "cpu%*d %lu %lu %lu %lu %lu %lu %lu %lu %lu",
               &t->user, &t->nice, &t->system, &t->idle, &t->iowait,
               &t->irq, &t->softirq, &t->steal, &t->guest);
    }
    fclose(f);
}

/* ---- /proc/interrupts (TLB / RES / CAL rows, per-CPU, exact) ---- */

static void read_interrupts(struct proc_sample *s, int ncpu)
{
    FILE *f = fopen("/proc/interrupts", "r");
    if (!f) return;
    char *line = NULL;
    size_t cap = 0;
    static int colmap[MAX_CPUS];
    int ncol = 0;

    if (getline(&line, &cap, f) > 0) {           /* header: CPU ids per column */
        char *p = line;
        while (*p && ncol < MAX_CPUS) {
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "CPU", 3)) break;
            colmap[ncol++] = atoi(p + 3);
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        }
    }
    while (getline(&line, &cap, f) > 0) {
        char *p = line;
        while (*p == ' ') p++;
        uint64_t *dst = NULL;
        if (!strncmp(p, "TLB:", 4)) { dst = s->irq_tlb; s->have_tlb = true; }
        else if (!strncmp(p, "RES:", 4)) { dst = s->irq_res; s->have_res = true; }
        else if (!strncmp(p, "CAL:", 4)) { dst = s->irq_cal; s->have_cal = true; }
        if (!dst) continue;
        p = strchr(p, ':') + 1;
        for (int i = 0; i < ncol; i++) {
            char *end;
            uint64_t v = strtoull(p, &end, 10);
            if (end == p) break;
            if (colmap[i] < ncpu) dst[colmap[i]] = v;
            p = end;
        }
    }
    free(line);
    fclose(f);
}

/* ---- /proc/vmstat: known drivers of TLB shootdowns ---- */

static void read_vmstat(struct proc_sample *s)
{
    FILE *f = fopen("/proc/vmstat", "r");
    if (!f) return;
    char key[64];
    uint64_t val;
    uint64_t pgsteal = 0, pgscan = 0;
    while (fscanf(f, "%63s %lu", key, &val) == 2) {
        if      (!strcmp(key, "thp_collapse_alloc"))      s->thp_collapse = val;
        else if (!strcmp(key, "thp_split_pmd"))           s->thp_split_pmd = val;
        else if (!strcmp(key, "thp_deferred_split_page")) s->thp_deferred_split = val;
        else if (!strcmp(key, "thp_migration_success"))   s->thp_migration = val;
        else if (!strcmp(key, "compact_stall"))           s->compact_stall = val;
        else if (!strcmp(key, "compact_daemon_wake"))     s->compact_daemon_wake = val;
        else if (!strcmp(key, "pgmigrate_success"))       s->pgmigrate = val;
        else if (!strcmp(key, "numa_pte_updates"))        s->numa_pte_updates = val;
        else if (!strcmp(key, "numa_pages_migrated"))     s->numa_pages_migrated = val;
        else if (!strncmp(key, "pgsteal_", 8))            pgsteal += val;
        else if (!strncmp(key, "pgscan_", 7))             pgscan += val;
    }
    s->pgsteal = pgsteal;
    s->pgscan = pgscan;
    fclose(f);
}

void proc_sample_read(struct proc_sample *s, int ncpu)
{
    memset(s, 0, sizeof *s);
    read_stat(s, ncpu);
    read_interrupts(s, ncpu);
    read_vmstat(s);
}

static double d64(uint64_t a, uint64_t b) { return b >= a ? (double)(b - a) : 0.0; }

void proc_rates_calc(struct proc_rates *r, const struct proc_sample *prev,
                     const struct proc_sample *cur, int ncpu, double dt)
{
    memset(r, 0, sizeof *r);
    if (dt <= 0) dt = 1;
    for (int i = 0; i < ncpu; i++) {
        const struct cpu_ticks *a = &prev->ticks[i], *b = &cur->ticks[i];
        double tot = d64(a->user, b->user) + d64(a->nice, b->nice) +
                     d64(a->system, b->system) + d64(a->idle, b->idle) +
                     d64(a->iowait, b->iowait) + d64(a->irq, b->irq) +
                     d64(a->softirq, b->softirq) + d64(a->steal, b->steal);
        if (tot <= 0) tot = 1;
        struct cpu_load *l = &r->load[i];
        l->user    = (float)(d64(a->user, b->user)       / tot);
        l->nice    = (float)(d64(a->nice, b->nice)       / tot);
        l->system  = (float)(d64(a->system, b->system)   / tot);
        l->irq     = (float)(d64(a->irq, b->irq)         / tot);
        l->softirq = (float)(d64(a->softirq, b->softirq) / tot);
        l->steal   = (float)(d64(a->steal, b->steal)     / tot);
        l->iowait  = (float)(d64(a->iowait, b->iowait)   / tot);
        l->busy = l->user + l->nice + l->system + l->irq + l->softirq + l->steal;

        r->tlb_recv[i] = d64(prev->irq_tlb[i], cur->irq_tlb[i]) / dt;
        r->res[i]      = d64(prev->irq_res[i], cur->irq_res[i]) / dt;
        r->cal[i]      = d64(prev->irq_cal[i], cur->irq_cal[i]) / dt;
        r->tlb_recv_tot += r->tlb_recv[i];
        r->res_tot      += r->res[i];
        r->cal_tot      += r->cal[i];
    }
    r->thp_collapse        = d64(prev->thp_collapse, cur->thp_collapse) / dt;
    r->thp_split_pmd       = d64(prev->thp_split_pmd, cur->thp_split_pmd) / dt;
    r->thp_deferred_split  = d64(prev->thp_deferred_split, cur->thp_deferred_split) / dt;
    r->thp_migration       = d64(prev->thp_migration, cur->thp_migration) / dt;
    r->compact_stall       = d64(prev->compact_stall, cur->compact_stall) / dt;
    r->compact_daemon_wake = d64(prev->compact_daemon_wake, cur->compact_daemon_wake) / dt;
    r->pgmigrate           = d64(prev->pgmigrate, cur->pgmigrate) / dt;
    r->numa_pte_updates    = d64(prev->numa_pte_updates, cur->numa_pte_updates) / dt;
    r->numa_pages_migrated = d64(prev->numa_pages_migrated, cur->numa_pages_migrated) / dt;
    r->pgsteal             = d64(prev->pgsteal, cur->pgsteal) / dt;
    r->pgscan              = d64(prev->pgscan, cur->pgscan) / dt;
}

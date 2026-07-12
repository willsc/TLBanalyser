/*
 * tlb:tlb_flush tracepoint capture via perf_event_open, one event per CPU,
 * sample_period=1 (every event recorded - no statistical sampling).
 * Attributes shootdowns to the sending PID and the kernel code path that
 * triggered the flush (munmap / madvise / THP / reclaim / migration / ...).
 */
#include "tlba.h"
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <pthread.h>
#include <linux/perf_event.h>

const char *tlb_reason_name[REASON_MAX] = {
    "flush on task switch", "remote shootdown (recv)", "local shootdown",
    "local MM shootdown", "remote IPI send", "remote wrong CPU", "?", "?",
};
const char *tlb_reason_short[REASON_MAX] = {
    "switch", "recv", "local", "localmm", "send", "wrongcpu", "?", "?",
};

#define PID_TABLE     8192
#define OUT_PIDS      512
#define OUT_ORIGINS   64
#define MAX_STACK     24

/* ---------------- kallsyms ---------------- */

struct ksym { uint64_t addr; uint32_t name_off; };

struct kallsyms {
    struct ksym *sym;
    uint32_t n;
    char *names;
    size_t names_len;
};

static int ksym_cmp(const void *a, const void *b)
{
    uint64_t x = ((const struct ksym *)a)->addr, y = ((const struct ksym *)b)->addr;
    return x < y ? -1 : x > y ? 1 : 0;
}

static bool kallsyms_load(struct kallsyms *ks)
{
    FILE *f = fopen("/proc/kallsyms", "r");
    if (!f) return false;
    size_t cap = 1 << 16, names_cap = 1 << 20;
    ks->sym = malloc(cap * sizeof *ks->sym);
    ks->names = malloc(names_cap);
    ks->n = 0;
    ks->names_len = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        uint64_t addr;
        char type, name[256];
        if (sscanf(line, "%lx %c %255s", &addr, &type, name) != 3) continue;
        if (!strchr("TtWw", type) || addr == 0) continue;
        size_t len = strlen(name) + 1;
        if (ks->n == cap) {
            cap *= 2;
            ks->sym = realloc(ks->sym, cap * sizeof *ks->sym);
        }
        if (ks->names_len + len > names_cap) {
            names_cap *= 2;
            ks->names = realloc(ks->names, names_cap);
        }
        ks->sym[ks->n].addr = addr;
        ks->sym[ks->n].name_off = (uint32_t)ks->names_len;
        memcpy(ks->names + ks->names_len, name, len);
        ks->names_len += len;
        ks->n++;
    }
    fclose(f);
    if (!ks->n) return false;
    qsort(ks->sym, ks->n, sizeof *ks->sym, ksym_cmp);
    return true;
}

/* returns symbol index+1, or 0 if not found */
static uint32_t kallsyms_lookup(const struct kallsyms *ks, uint64_t ip)
{
    if (!ks->n || ip < ks->sym[0].addr) return 0;
    uint32_t lo = 0, hi = ks->n - 1;
    while (lo < hi) {
        uint32_t mid = (lo + hi + 1) / 2;
        if (ks->sym[mid].addr <= ip) lo = mid;
        else hi = mid - 1;
    }
    return lo + 1;
}

/* kernel plumbing between the flush trigger and the tracepoint - skipped when
 * choosing the "origin" frame so we surface the meaningful caller instead */
static const char *skip_prefix[] = {
    "__traceiter_", "trace_event_", "perf_trace_", "perf_tp_", "perf_swevent",
    "native_flush_tlb", "flush_tlb_", "__flush_tlb", "do_flush_tlb",
    "arch_tlbbatch", "try_to_unmap_flush", "tlb_flush_mmu", "tlb_finish_mmu",
    "tlb_batch_", "tlb_remove_", "ptep_clear_flush", "pmdp_", "pudp_",
    "smp_call_function", "on_each_cpu", "generic_exec_single",
    "__sysvec_", "sysvec_", "asm_sysvec_", "common_interrupt", "irqentry_",
    "_raw_spin", "rcu_", "srcu_",
};

static bool is_plumbing(const char *name)
{
    for (size_t i = 0; i < sizeof skip_prefix / sizeof skip_prefix[0]; i++)
        if (!strncmp(name, skip_prefix[i], strlen(skip_prefix[i]))) return true;
    return false;
}

/* ---------------- per-CPU ring buffers ---------------- */

struct ring {
    int fd;
    void *base;         /* meta page + data */
    size_t data_size;
    uint64_t tail;
};

/*
 * Two capture channels per CPU (when the kernel supports tracepoint filters):
 *   CH_ATTR: reasons 2,3,4,5 (local / IPI-send) WITH kernel callchains.
 *            Low rate; any loss here breaks attribution -> reported loudly.
 *   CH_CTX:  reasons 0,1 (task-switch / IPI-receive), no callchain.  These
 *            can storm; loss is cosmetic because exact receive counts come
 *            from /proc/interrupts.
 */
enum { CH_ATTR = 0, CH_CTX = 1, CH__N };

/* ---------------- state ---------------- */

struct trace_state {
    int ncpu;
    struct ring ring[CH__N][MAX_CPUS];
    bool split;                        /* filters worked; CH_CTX active */
    struct kallsyms ks;
    int reason_off, pages_off;

    struct pid_stat tab[PID_TABLE];
    bool used[PID_TABLE];

    struct origin_stat org[1024];
    int norg;

    uint64_t reason_cnt[REASON_MAX], reason_cum[REASON_MAX];
    uint64_t cpu_send[MAX_CPUS];       /* IPI-send events per sending CPU */
    uint64_t pages_cnt;
    uint64_t lost, lost_cum;           /* attribution channel (critical)  */
    uint64_t lost_ctx, lost_ctx_cum;   /* context channel (cosmetic)      */
    uint64_t tick;

    struct pid_stat out_pids[OUT_PIDS];
    struct origin_stat out_orgs[OUT_ORIGINS];
    uint64_t out_cpu_send[MAX_CPUS];

    pthread_t drainer;
    pthread_mutex_t lock;
    volatile bool stop;
    bool thread_running;
};

const uint64_t *trace_cpu_sends(struct trace_state *ts) { return ts->out_cpu_send; }

static int tp_read_int(const char *file, int dflt)
{
    char p[256];
    static const char *bases[] = { "/sys/kernel/tracing", "/sys/kernel/debug/tracing" };
    for (int i = 0; i < 2; i++) {
        snprintf(p, sizeof p, "%s/events/tlb/tlb_flush/%s", bases[i], file);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        int v = dflt;
        if (!strcmp(file, "id")) {
            if (fscanf(f, "%d", &v) != 1) v = dflt;
        }
        fclose(f);
        return v;
    }
    return dflt;
}

/* parse the tracepoint format file for a field's byte offset */
static int tp_field_offset(const char *field, int dflt)
{
    static const char *bases[] = { "/sys/kernel/tracing", "/sys/kernel/debug/tracing" };
    char p[256], line[256], want[64];
    snprintf(want, sizeof want, " %s;", field);
    for (int i = 0; i < 2; i++) {
        snprintf(p, sizeof p, "%s/events/tlb/tlb_flush/format", bases[i]);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        while (fgets(line, sizeof line, f)) {
            if (!strstr(line, want)) continue;
            char *o = strstr(line, "offset:");
            if (o) {
                fclose(f);
                return atoi(o + 7);
            }
        }
        fclose(f);
    }
    return dflt;
}

static void *drain_thread(void *arg);

/*
 * Per-CPU ring data pages (power of 2).  The attribution ring must absorb
 * bursts (e.g. one munmap/madvise can trigger thousands of flush events with
 * callchains within milliseconds on one CPU), so it is deep; smaller on very
 * large machines to bound memory (pages * 4K * ncpu per channel).
 */
static int ring_pages(int ncpu, int ch)
{
    if (ch == CH_ATTR)                                  /* 0.5-2 MB */
        return ncpu <= 64 ? 512 : ncpu <= 128 ? 256 : 128;
    return ncpu > 128 ? 32 : 64;                        /* 128-256 KB */
}

static int open_channel(struct trace_state *ts, int ch, struct perf_event_attr *a,
                        const char *filter)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    int pages = ring_pages(ts->ncpu, ch);
    int opened = 0;
    for (int c = 0; c < ts->ncpu; c++) {
        struct ring *r = &ts->ring[ch][c];
        r->fd = (int)syscall(SYS_perf_event_open, a, -1, c, -1, PERF_FLAG_FD_CLOEXEC);
        if (r->fd < 0) continue;
        if (filter && ioctl(r->fd, PERF_EVENT_IOC_SET_FILTER, filter) < 0) {
            close(r->fd);
            r->fd = -1;
            return -1;                          /* filters unsupported */
        }
        r->base = mmap(NULL, (1 + (size_t)pages) * page, PROT_READ | PROT_WRITE,
                       MAP_SHARED, r->fd, 0);
        if (r->base == MAP_FAILED) {
            close(r->fd);
            r->fd = -1;
            r->base = NULL;
            continue;
        }
        r->data_size = (size_t)pages * page;
        opened++;
    }
    return opened;
}

static void close_channel(struct trace_state *ts, int ch)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    int pages = ring_pages(ts->ncpu, ch);
    for (int c = 0; c < ts->ncpu; c++) {
        struct ring *r = &ts->ring[ch][c];
        if (r->base) munmap(r->base, (1 + (size_t)pages) * page);
        if (r->fd >= 0) close(r->fd);
        r->base = NULL;
        r->fd = -1;
        r->tail = 0;
    }
}

struct trace_state *trace_open(int ncpu, char *errbuf, size_t errlen)
{
    /* minimal images often boot without tracefs mounted; fix it ourselves */
    if (access("/sys/kernel/tracing/events", F_OK) != 0 &&
        access("/sys/kernel/debug/tracing/events", F_OK) != 0 &&
        geteuid() == 0)
        mount("tracefs", "/sys/kernel/tracing", "tracefs", 0, NULL);

    int tp_id = tp_read_int("id", -1);
    if (tp_id < 0) {
        snprintf(errbuf, errlen, "tlb:tlb_flush tracepoint not found - "
                 "tracefs missing or inaccessible (container?); try: "
                 "mount -t tracefs tracefs /sys/kernel/tracing");
        return NULL;
    }
    struct trace_state *ts = calloc(1, sizeof *ts);
    if (!ts) return NULL;
    ts->ncpu = ncpu;
    for (int ch = 0; ch < CH__N; ch++)
        for (int c = 0; c < MAX_CPUS; c++) ts->ring[ch][c].fd = -1;
    ts->reason_off = tp_field_offset("reason", 8);
    ts->pages_off  = tp_field_offset("pages", 16);
    if (!kallsyms_load(&ts->ks))
        /* not fatal: attribution still works per-PID, origins become "?" */
        ts->ks.n = 0;

    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.size = sizeof a;
    a.type = PERF_TYPE_TRACEPOINT;
    a.config = (uint64_t)tp_id;
    a.sample_period = 1;                       /* capture EVERY event */
    a.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_CPU |
                    PERF_SAMPLE_CALLCHAIN | PERF_SAMPLE_RAW;
    a.exclude_callchain_user = 1;              /* kernel stacks only */
    a.sample_max_stack = MAX_STACK;
    a.exclude_guest = 1;

    /* attribution channel: local + remote-send flushes, with callchains */
    int n_attr = open_channel(ts, CH_ATTR, &a, "reason != 0 && reason != 1");
    if (n_attr > 0) {
        /* context channel: task-switch + IPI-receive, tiny records */
        a.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW;
        a.sample_max_stack = 0;
        int n_ctx = open_channel(ts, CH_CTX, &a, "reason == 0 || reason == 1");
        if (n_ctx <= 0) close_channel(ts, CH_CTX);
        ts->split = n_ctx > 0;
    } else if (n_attr < 0) {
        /* kernel lacks tracepoint filters: capture everything on one channel */
        close_channel(ts, CH_ATTR);
        n_attr = open_channel(ts, CH_ATTR, &a, NULL);
    }
    if (n_attr <= 0) {
        snprintf(errbuf, errlen, "cannot attach to tlb:tlb_flush (%s) - "
                 "run as root / CAP_PERFMON+CAP_SYS_ADMIN", strerror(errno));
        trace_close(ts);
        return NULL;
    }
    pthread_mutex_init(&ts->lock, NULL);
    ts->thread_running = pthread_create(&ts->drainer, NULL, drain_thread, ts) == 0;
    return ts;
}

/* ---------------- pid table ---------------- */

static struct pid_stat *pid_slot(struct trace_state *ts, int pid)
{
    uint32_t h = ((uint32_t)pid * 2654435761u) & (PID_TABLE - 1);
    for (int i = 0; i < PID_TABLE; i++) {
        uint32_t s = (h + i) & (PID_TABLE - 1);
        if (!ts->used[s]) {
            ts->used[s] = true;
            memset(&ts->tab[s], 0, sizeof ts->tab[s]);
            ts->tab[s].pid = pid;
            return &ts->tab[s];
        }
        if (ts->tab[s].pid == pid) return &ts->tab[s];
    }
    return NULL; /* table full: drop */
}

static void pid_add_origin(struct pid_stat *p, uint32_t sym)
{
    if (!sym) return;
    int min = 0;
    for (int i = 0; i < 4; i++) {
        if (p->origin_sym[i] == sym) { p->origin_cnt[i]++; return; }
        if (p->origin_cnt[i] < p->origin_cnt[min]) min = i;
    }
    if (p->origin_cnt[min] == 0) {
        p->origin_sym[min] = sym;
        p->origin_cnt[min] = 1;
    } else if (p->origin_cnt[min] == 1) {  /* cheap replacement policy */
        p->origin_sym[min] = sym;
    }
}

static void global_add_origin(struct trace_state *ts, uint32_t sym)
{
    if (!sym) return;
    for (int i = 0; i < ts->norg; i++)
        if (ts->org[i].sym == sym) { ts->org[i].cnt++; ts->org[i].cum++; return; }
    if (ts->norg < (int)(sizeof ts->org / sizeof ts->org[0])) {
        ts->org[ts->norg].sym = sym;
        ts->org[ts->norg].cnt = 1;
        ts->org[ts->norg].cum = 1;
        ts->norg++;
    }
}

/* ---------------- sample parsing ---------------- */

#define CTX_MARKER 0xffffffffffffe000ULL   /* PERF_CONTEXT_* sentinels */

static void handle_sample(struct trace_state *ts, const uint8_t *d, size_t len,
                          bool has_callchain)
{
    /* layout for our sample_type: TID, CPU, [CALLCHAIN,] RAW */
    size_t off = 0;
    if (off + 16 > len) return;
    uint32_t pid = *(const uint32_t *)(d + off);
    off += 8;                     /* pid, tid */
    uint32_t cpu = *(const uint32_t *)(d + off);
    off += 8;                     /* cpu, res */
    uint64_t nr = 0;
    const uint64_t *ips = NULL;
    if (has_callchain) {
        if (off + 8 > len) return;
        nr = *(const uint64_t *)(d + off);
        off += 8;
        ips = (const uint64_t *)(d + off);
        if (nr > MAX_STACK + 4) nr = 0;
        off += nr * 8;
    }
    if (off + 4 > len) return;
    uint32_t raw_size = *(const uint32_t *)(d + off);
    off += 4;
    if (off + raw_size > len) return;
    const uint8_t *raw = d + off;

    int reason = 0;
    uint64_t pages = 0;
    if ((size_t)ts->reason_off + 4 <= raw_size)
        reason = *(const int32_t *)(raw + ts->reason_off);
    if ((size_t)ts->pages_off + 8 <= raw_size)
        pages = *(const uint64_t *)(raw + ts->pages_off);
    /* TLB_FLUSH_ALL (-1UL) marks a full flush, not a page count */
    if (pages >= (1ULL << 32)) pages = 0;
    if (reason < 0 || reason >= REASON_MAX) return;

    ts->reason_cnt[reason]++;
    ts->reason_cum[reason]++;
    if (reason == R_REMOTE_SEND) {
        ts->pages_cnt += pages;
        if (cpu < MAX_CPUS) ts->cpu_send[cpu]++;
    }

    struct pid_stat *p = pid_slot(ts, (int)pid);
    if (!p) return;
    p->cnt[reason]++;
    p->cum[reason]++;
    p->last_seen = ts->tick;
    if (reason == R_REMOTE_SEND) {
        p->pages += pages;
        p->pages_cum += pages;
    }

    /* origin only matters where the sampled task is the cause */
    if (reason == R_REMOTE_SEND || reason == R_LOCAL || reason == R_LOCAL_MM) {
        uint32_t sym = 0;
        for (uint64_t i = 0; i < nr; i++) {
            if (ips[i] >= CTX_MARKER) continue;
            uint32_t s = kallsyms_lookup(&ts->ks, ips[i]);
            if (!s) continue;
            const char *name = ts->ks.names + ts->ks.sym[s - 1].name_off;
            if (is_plumbing(name)) continue;
            sym = s;
            break;
        }
        pid_add_origin(p, sym);
        global_add_origin(ts, sym);
    }
}

static void drain_channel(struct trace_state *ts, int ch)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    bool has_cc = ch == CH_ATTR || !ts->split;
    for (int c = 0; c < ts->ncpu; c++) {
        struct ring *r = &ts->ring[ch][c];
        if (!r->base) continue;
        struct perf_event_mmap_page *mp = r->base;
        uint64_t head = __atomic_load_n(&mp->data_head, __ATOMIC_ACQUIRE);
        uint64_t tail = r->tail;
        uint8_t *data = (uint8_t *)r->base + page;
        static uint8_t tmp[64 * 1024];

        while (tail < head) {
            struct perf_event_header *h =
                (struct perf_event_header *)(data + (tail & (r->data_size - 1)));
            size_t size = h->size;
            if (size == 0 || size > sizeof tmp) { tail = head; break; }
            const uint8_t *rec;
            uint64_t start = tail & (r->data_size - 1);
            if (start + size > r->data_size) {          /* wrapped record */
                size_t first = r->data_size - start;
                memcpy(tmp, data + start, first);
                memcpy(tmp + first, data, size - first);
                rec = tmp;
            } else {
                rec = data + start;
            }
            const struct perf_event_header *rh = (const struct perf_event_header *)rec;
            if (rh->type == PERF_RECORD_SAMPLE) {
                handle_sample(ts, rec + sizeof *rh, size - sizeof *rh, has_cc);
            } else if (rh->type == PERF_RECORD_LOST) {
                uint64_t lost = *(const uint64_t *)(rec + sizeof *rh + 8);
                if (ch == CH_CTX) {
                    ts->lost_ctx += lost;
                    ts->lost_ctx_cum += lost;
                } else {
                    ts->lost += lost;
                    ts->lost_cum += lost;
                }
            }
            tail += size;
        }
        r->tail = tail;
        __atomic_store_n(&mp->data_tail, tail, __ATOMIC_RELEASE);
    }
}

void trace_drain(struct trace_state *ts)
{
    pthread_mutex_lock(&ts->lock);
    drain_channel(ts, CH_ATTR);
    if (ts->split) drain_channel(ts, CH_CTX);
    pthread_mutex_unlock(&ts->lock);
}

/* dedicated drainer: 5ms cadence keeps rings empty regardless of UI pace */
static void *drain_thread(void *arg)
{
    struct trace_state *ts = arg;
    while (!ts->stop) {
        trace_drain(ts);
        usleep(5 * 1000);
    }
    return NULL;
}

void trace_lost_ctx(struct trace_state *ts, uint64_t *cur, uint64_t *cum)
{
    *cur = ts->lost_ctx;
    *cum = ts->lost_ctx_cum;
}

/* ---------------- interval close-out ---------------- */

static uint64_t pid_interval_total(const struct pid_stat *p)
{
    uint64_t t = 0;
    for (int i = 0; i < REASON_MAX; i++) t += p->cnt[i];
    return t;
}

static uint64_t pid_cum_total(const struct pid_stat *p)
{
    uint64_t t = 0;
    for (int i = 0; i < REASON_MAX; i++) t += p->cum[i];
    return t;
}

static int pid_cmp(const void *a, const void *b)
{
    const struct pid_stat *x = *(struct pid_stat *const *)a;
    const struct pid_stat *y = *(struct pid_stat *const *)b;
    if (x->cnt[R_REMOTE_SEND] != y->cnt[R_REMOTE_SEND])
        return x->cnt[R_REMOTE_SEND] > y->cnt[R_REMOTE_SEND] ? -1 : 1;
    uint64_t xt = pid_interval_total(x), yt = pid_interval_total(y);
    if (xt != yt) return xt > yt ? -1 : 1;
    uint64_t xc = x->cum[R_REMOTE_SEND], yc = y->cum[R_REMOTE_SEND];
    if (xc != yc) return xc > yc ? -1 : 1;
    xc = pid_cum_total(x);
    yc = pid_cum_total(y);
    return xc > yc ? -1 : xc < yc ? 1 : 0;
}

static int org_cmp(const void *a, const void *b)
{
    const struct origin_stat *x = a, *y = b;
    if (x->cnt != y->cnt) return x->cnt > y->cnt ? -1 : 1;
    return x->cum > y->cum ? -1 : x->cum < y->cum ? 1 : 0;
}

static void resolve_comm(struct pid_stat *p)
{
    if (p->pid == 0) {
        snprintf(p->comm, sizeof p->comm, "<kernel/idle>");
        p->kthread = true;
        return;
    }
    char path[64], buf[COMM_LEN + 2];
    snprintf(path, sizeof path, "/proc/%d/comm", p->pid);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(buf, sizeof buf, f)) {
            buf[strcspn(buf, "\n")] = 0;
            snprintf(p->comm, sizeof p->comm, "%.*s", COMM_LEN, buf);
        }
        fclose(f);
        /* kernel thread: empty cmdline */
        snprintf(path, sizeof path, "/proc/%d/cmdline", p->pid);
        f = fopen(path, "r");
        if (f) {
            p->kthread = fgetc(f) == EOF;
            fclose(f);
        }
    } else if (!p->comm[0]) {
        snprintf(p->comm, sizeof p->comm, "<exited>");
    }
}

int trace_interval(struct trace_state *ts, struct pid_stat **pids_out,
                   struct origin_stat **origins_out, int *norigin,
                   uint64_t reason_tot[REASON_MAX],
                   uint64_t reason_cum[REASON_MAX],
                   uint64_t *pages_tot, uint64_t *lost, uint64_t *lost_cum)
{
    pthread_mutex_lock(&ts->lock);
    drain_channel(ts, CH_ATTR);
    if (ts->split) drain_channel(ts, CH_CTX);
    ts->tick++;

    static struct pid_stat *ptrs[PID_TABLE];
    int n = 0;
    for (int i = 0; i < PID_TABLE; i++) {
        if (!ts->used[i]) continue;
        /* age out long-idle entries so the table stays useful */
        if (pid_interval_total(&ts->tab[i]) == 0 &&
            ts->tick - ts->tab[i].last_seen > 120) {
            ts->used[i] = false;
            continue;
        }
        ptrs[n++] = &ts->tab[i];
    }
    qsort(ptrs, n, sizeof *ptrs, pid_cmp);

    int out = n < OUT_PIDS ? n : OUT_PIDS;
    for (int i = 0; i < out; i++) {
        if (pid_interval_total(ptrs[i]) > 0 || !ptrs[i]->comm[0])
            resolve_comm(ptrs[i]);
        ts->out_pids[i] = *ptrs[i];
    }

    qsort(ts->org, ts->norg, sizeof ts->org[0], org_cmp);
    int no = ts->norg < OUT_ORIGINS ? ts->norg : OUT_ORIGINS;
    memcpy(ts->out_orgs, ts->org, no * sizeof ts->org[0]);

    memcpy(ts->out_cpu_send, ts->cpu_send, sizeof ts->cpu_send);
    memset(ts->cpu_send, 0, sizeof ts->cpu_send);

    memcpy(reason_tot, ts->reason_cnt, sizeof ts->reason_cnt);
    memcpy(reason_cum, ts->reason_cum, sizeof ts->reason_cum);
    *pages_tot = ts->pages_cnt;
    *lost = ts->lost;
    *lost_cum = ts->lost_cum;

    /* reset interval accumulators */
    memset(ts->reason_cnt, 0, sizeof ts->reason_cnt);
    ts->pages_cnt = 0;
    ts->lost = 0;
    ts->lost_ctx = 0;
    for (int i = 0; i < n; i++) {
        memset(ptrs[i]->cnt, 0, sizeof ptrs[i]->cnt);
        ptrs[i]->pages = 0;
    }
    for (int i = 0; i < ts->norg; i++) ts->org[i].cnt = 0;

    pthread_mutex_unlock(&ts->lock);
    *pids_out = ts->out_pids;
    *origins_out = ts->out_orgs;
    *norigin = no;
    return out;
}

const char *trace_symname(struct trace_state *ts, uint32_t sym)
{
    if (!sym || sym > ts->ks.n) return "?";
    return ts->ks.names + ts->ks.sym[sym - 1].name_off;
}

void trace_reset_cum(struct trace_state *ts)
{
    pthread_mutex_lock(&ts->lock);
    memset(ts->used, 0, sizeof ts->used);
    ts->norg = 0;
    memset(ts->reason_cum, 0, sizeof ts->reason_cum);
    memset(ts->reason_cnt, 0, sizeof ts->reason_cnt);
    ts->lost_cum = ts->lost = 0;
    ts->lost_ctx_cum = ts->lost_ctx = 0;
    ts->pages_cnt = 0;
    pthread_mutex_unlock(&ts->lock);
}

void trace_close(struct trace_state *ts)
{
    if (!ts) return;
    if (ts->thread_running) {
        ts->stop = true;
        pthread_join(ts->drainer, NULL);
    }
    for (int ch = 0; ch < CH__N; ch++) close_channel(ts, ch);
    free(ts->ks.sym);
    free(ts->ks.names);
    free(ts);
}

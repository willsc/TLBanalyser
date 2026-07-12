/*
 * Heuristic fallback when no tracepoint/kprobe attribution is available:
 * scan /proc for the processes with the highest page-fault / RSS churn.
 * Heavy mmap/munmap/madvise users - the usual TLB shootdown senders - show
 * up here even on kernels where the flush events themselves are hidden.
 */
#include "tlba.h"
#include <dirent.h>
#include <ctype.h>

#define SUSP_TABLE 4096

struct prev { int pid; uint64_t minflt, majflt; bool used; };
static struct prev tab[SUSP_TABLE];

static struct prev *slot(int pid)
{
    uint32_t h = ((uint32_t)pid * 2654435761u) & (SUSP_TABLE - 1);
    for (int i = 0; i < SUSP_TABLE; i++) {
        uint32_t s = (h + i) & (SUSP_TABLE - 1);
        if (!tab[s].used) {
            tab[s].used = true;
            tab[s].pid = pid;
            tab[s].minflt = tab[s].majflt = 0;
            return &tab[s];
        }
        if (tab[s].pid == pid) return &tab[s];
    }
    return NULL;
}

static int cmp_susp(const void *a, const void *b)
{
    const struct suspect *x = a, *y = b;
    double xa = x->minflt + 10 * x->majflt, ya = y->minflt + 10 * y->majflt;
    return xa > ya ? -1 : xa < ya ? 1 : 0;
}

int suspects_scan(struct suspect *out, int max, double dt)
{
    if (dt <= 0) dt = 1;
    static struct suspect all[1024];
    int n = 0;

    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < (int)(sizeof all / sizeof all[0])) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        int pid = atoi(e->d_name);
        char path[64], buf[512];
        snprintf(path, sizeof path, "/proc/%d/stat", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t len = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[len] = 0;

        /* comm is field 2 in parens and may contain spaces; skip past it */
        char *close = strrchr(buf, ')');
        if (!close) continue;
        char comm[COMM_LEN + 1] = "";
        char *open = strchr(buf, '(');
        if (open && close > open) {
            size_t cl = (size_t)(close - open - 1);
            if (cl > COMM_LEN) cl = COMM_LEN;
            memcpy(comm, open + 1, cl);
            comm[cl] = 0;
        }
        /* fields after ')': state(3) ... minflt=10 ... majflt=12 ... rss=24 */
        uint64_t minflt = 0, majflt = 0;
        long rss_pages = 0;
        char *p = close + 2;
        int field = 3;
        while (*p && field <= 24) {
            if (field == 10) minflt = strtoull(p, NULL, 10);
            else if (field == 12) majflt = strtoull(p, NULL, 10);
            else if (field == 24) rss_pages = atol(p);
            while (*p && *p != ' ') p++;
            while (*p == ' ') p++;
            field++;
        }

        struct prev *pv = slot(pid);
        if (!pv) continue;
        double dmin = pv->minflt && minflt >= pv->minflt
                        ? (double)(minflt - pv->minflt) / dt : 0;
        double dmaj = pv->majflt && majflt >= pv->majflt
                        ? (double)(majflt - pv->majflt) / dt : 0;
        bool first = pv->minflt == 0 && pv->majflt == 0;
        pv->minflt = minflt ? minflt : 1;
        pv->majflt = majflt ? majflt : 1;
        if (first || (dmin <= 0 && dmaj <= 0)) continue;

        struct suspect *s = &all[n++];
        s->pid = pid;
        snprintf(s->comm, sizeof s->comm, "%s", comm);
        s->minflt = dmin;
        s->majflt = dmaj;
        s->rss_kb = rss_pages * 4;
    }
    closedir(d);

    qsort(all, n, sizeof all[0], cmp_susp);
    if (n > max) n = max;
    memcpy(out, all, (size_t)n * sizeof all[0]);
    return n;
}

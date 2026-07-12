/* htop-style ncurses front-end */
#include "tlba.h"
#include <ncurses.h>
#include <math.h>

enum { CP_LABEL = 1, CP_USER, CP_SYS, CP_IRQ, CP_SOFTIRQ, CP_NICE,
       CP_FKEY, CP_TABHDR, CP_SEL, CP_HEAT_LO, CP_HEAT_MID, CP_HEAT_HI,
       CP_TITLE, CP_WARN };

enum sort_mode { SORT_SEND = 0, SORT_TOTAL, SORT_RECV, SORT_CUMSEND, SORT__N };
static const char *sort_name[SORT__N] = { "SEND/s", "TOTAL/s", "RECV/s", "ΣSEND" };

static struct {
    int  sort;
    int  scroll;
    bool paused;
    bool help;
    bool compact_cpu;   /* force compact CPU grid */
    bool cumulative;    /* show cumulative counts in table */
} st;

void ui_init(void)
{
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    timeout(50);            /* getch timeout doubles as ring drain cadence */
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(CP_LABEL,   COLOR_CYAN,    -1);
        init_pair(CP_USER,    COLOR_GREEN,   -1);
        init_pair(CP_SYS,     COLOR_RED,     -1);
        init_pair(CP_IRQ,     COLOR_YELLOW,  -1);
        init_pair(CP_SOFTIRQ, COLOR_MAGENTA, -1);
        init_pair(CP_NICE,    COLOR_BLUE,    -1);
        init_pair(CP_FKEY,    COLOR_BLACK,   COLOR_CYAN);
        init_pair(CP_TABHDR,  COLOR_BLACK,   COLOR_GREEN);
        init_pair(CP_SEL,     COLOR_BLACK,   COLOR_CYAN);
        init_pair(CP_HEAT_LO, COLOR_GREEN,   -1);
        init_pair(CP_HEAT_MID,COLOR_YELLOW,  -1);
        init_pair(CP_HEAT_HI, COLOR_RED,     -1);
        init_pair(CP_TITLE,   COLOR_BLACK,   COLOR_CYAN);
        init_pair(CP_WARN,    COLOR_RED,     -1);
    }
}

void ui_done(void) { endwin(); }

/* returns: 0 none, 1 quit, 2 reset-cumulative */
int ui_key(int ch)
{
    switch (ch) {
    case 'q': case 'Q': case KEY_F(10): return 1;
    case 'r': case 'R': return 2;
    case 'p': case ' ': st.paused = !st.paused; break;
    case 'h': case '?': case KEY_F(1): st.help = !st.help; break;
    case 'm': st.compact_cpu = !st.compact_cpu; break;
    case 'c': st.cumulative = !st.cumulative; break;
    case 's': case KEY_F(5): st.sort = (st.sort + 1) % SORT__N; break;
    case KEY_UP:    if (st.scroll > 0) st.scroll--; break;
    case KEY_DOWN:  st.scroll++; break;
    case KEY_PPAGE: st.scroll = st.scroll > 10 ? st.scroll - 10 : 0; break;
    case KEY_NPAGE: st.scroll += 10; break;
    case KEY_HOME:  st.scroll = 0; break;
    }
    return 0;
}

bool ui_paused(void) { return st.paused; }

/* ---------- formatting helpers ---------- */

static void fmt_rate(double v, char *buf, size_t len)
{
    if (v >= 1e9)      snprintf(buf, len, "%.1fG", v / 1e9);
    else if (v >= 1e6) snprintf(buf, len, "%.1fM", v / 1e6);
    else if (v >= 1e5) snprintf(buf, len, "%.0fk", v / 1e3);
    else if (v >= 1e3) snprintf(buf, len, "%.1fk", v / 1e3);
    else if (v >= 10 || v == 0.0 || fmod(v, 1.0) < 0.05) snprintf(buf, len, "%.0f", v);
    else               snprintf(buf, len, "%.1f", v);
}

static void fmt_count(uint64_t v, char *buf, size_t len)
{
    fmt_rate((double)v, buf, len);
}

static void fmt_kb(long kb, char *buf, size_t len)
{
    if (kb >= 1024 && kb % 1024 == 0) snprintf(buf, len, "%ldM", kb / 1024);
    else snprintf(buf, len, "%ldK", kb);
}

/* ---------- widgets ---------- */

/* htop-style CPU bar:  NN[|||||||        45.3%] */
static void draw_cpu_meter(int y, int x, int w, int cpu, const struct cpu_load *l)
{
    int inner = w - 8;                        /* label(3) + [ ] */
    if (inner < 6) return;
    attron(COLOR_PAIR(CP_LABEL));
    mvprintw(y, x, "%3d", cpu);
    attroff(COLOR_PAIR(CP_LABEL));
    attron(A_BOLD);
    mvaddch(y, x + 3, '[');
    mvaddch(y, x + 3 + inner + 1, ']');
    attroff(A_BOLD);

    struct { float v; int cp; } seg[] = {
        { l->nice, CP_NICE }, { l->user, CP_USER }, { l->system, CP_SYS },
        { l->irq, CP_IRQ }, { l->softirq, CP_SOFTIRQ }, { l->steal, CP_IRQ },
    };
    int pos = 0;
    for (size_t i = 0; i < sizeof seg / sizeof seg[0]; i++) {
        int n = (int)lroundf(seg[i].v * inner);
        if (pos + n > inner) n = inner - pos;
        if (n <= 0) continue;
        attron(COLOR_PAIR(seg[i].cp));
        for (int k = 0; k < n; k++) mvaddch(y, x + 4 + pos + k, '|');
        attroff(COLOR_PAIR(seg[i].cp));
        pos += n;
    }
    char pct[8];
    snprintf(pct, sizeof pct, "%.1f%%", l->busy * 100.0);
    int px = x + 4 + inner - (int)strlen(pct);
    attron(A_DIM);
    mvprintw(y, px, "%s", pct);
    attroff(A_DIM);
}

/* heat strip: one cell per CPU (or aggregated), colored by value/max */
static void draw_heat(int y, int x, int w, const double *val, int n, double lo, double hi)
{
    attron(A_BOLD);
    mvaddch(y, x, '[');
    attroff(A_BOLD);
    int cells = w - 2;
    if (cells < 1) return;
    int per = (n + cells - 1) / cells;        /* CPUs per cell */
    if (per < 1) per = 1;
    int used = (n + per - 1) / per;
    static const char glyph[] = " .:-=+*#%@";
    for (int c = 0; c < used; c++) {
        double m = 0;
        for (int i = c * per; i < (c + 1) * per && i < n; i++)
            if (val[i] > m) m = val[i];
        double f = hi > lo ? (m - lo) / (hi - lo) : 0;
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        int g = (int)(f * 9.0);
        int cp = f < 0.4 ? CP_HEAT_LO : f < 0.75 ? CP_HEAT_MID : CP_HEAT_HI;
        attron(COLOR_PAIR(cp) | (g >= 7 ? A_BOLD : 0));
        mvaddch(y, x + 1 + c, (chtype)glyph[g]);
        attroff(COLOR_PAIR(cp) | A_BOLD);
    }
    for (int c = used; c < cells; c++) mvaddch(y, x + 1 + c, ' ');
    attron(A_BOLD);
    mvaddch(y, x + 1 + cells, ']');
    attroff(A_BOLD);
}

static void section(int y, const char *title)
{
    attron(COLOR_PAIR(CP_LABEL) | A_BOLD);
    mvprintw(y, 0, "%s", title);
    attroff(COLOR_PAIR(CP_LABEL) | A_BOLD);
    attron(A_DIM);
    int x = (int)strlen(title) + 1;
    for (int i = x; i < COLS; i++) mvaddch(y, i, ACS_HLINE);
    attroff(A_DIM);
}

/* ---------- table sorting ---------- */

static const struct snapshot *g_snap;

static double row_key(const struct pid_stat *p)
{
    uint64_t t = 0;
    switch (st.sort) {
    case SORT_SEND:   return (double)p->cnt[R_REMOTE_SEND];
    case SORT_RECV:   return (double)p->cnt[R_REMOTE_RECV];
    case SORT_CUMSEND:return (double)p->cum[R_REMOTE_SEND];
    default:
        for (int i = 0; i < REASON_MAX; i++) t += p->cnt[i];
        return (double)t;
    }
}

static int row_cmp(const void *a, const void *b)
{
    double x = row_key(a), y = row_key(b);
    if (x != y) return x > y ? -1 : 1;
    uint64_t xc = ((const struct pid_stat *)a)->cum[R_REMOTE_SEND];
    uint64_t yc = ((const struct pid_stat *)b)->cum[R_REMOTE_SEND];
    if (xc != yc) return xc > yc ? -1 : 1;
    uint64_t xt = 0, yt = 0;
    for (int i = 0; i < REASON_MAX; i++) {
        xt += ((const struct pid_stat *)a)->cum[i];
        yt += ((const struct pid_stat *)b)->cum[i];
    }
    return xt > yt ? -1 : xt < yt ? 1 : 0;
}

/* ---------- help overlay ---------- */

static void draw_help(void)
{
    static const char *txt[] = {
        "TLBanalyser " TLBA_VERSION " - TLB shootdown / IPI / cache analyser",
        "",
        "SOURCES OF TRUTH",
        "  recv IPIs   /proc/interrupts TLB row - exact kernel counters",
        "  send events tlb:tlb_flush tracepoint, period=1: EVERY event is",
        "              captured (watch 'lost' - 0 means nothing was missed)",
        "  L1/L2       per-CPU PMU counters; L2 uses vendor raw events",
        "",
        "TABLE COLUMNS (per second unless cumulative view, key 'c')",
        "  SEND    remote shootdown IPIs this task initiated  <- culprit",
        "  PAGES   pages invalidated by those sends (0 pages = full flush)",
        "  LOCAL/LOCMM  local-only flushes (no IPI to other CPUs)",
        "  SWTCH   lazy-TLB flushes at context switch (benign)",
        "  RECV    IPIs that interrupted this task (victim, not cause)",
        "  ORIGIN  kernel function that triggered the flush, e.g.:",
        "          unmap_region=munmap  madvise_*=allocator MADV_DONTNEED/FREE",
        "          shrink_folio_list=memory reclaim  migrate_pages*=migration",
        "          collapse_huge_page/khugepaged=THP  zap_page_range=exit/free",
        "",
        "KEYS  s/F5 sort   c cumulative   p pause   m compact CPU grid",
        "      r reset cumulative   arrows/PgUp/PgDn scroll   q quit",
        "",
        "                      press h to close",
    };
    int n = (int)(sizeof txt / sizeof txt[0]);
    int w = 74, h = n + 2;
    int y0 = (LINES - h) / 2, x0 = (COLS - w) / 2;
    if (y0 < 0) y0 = 0;
    if (x0 < 0) x0 = 0;
    for (int y = 0; y < h && y0 + y < LINES; y++) {
        move(y0 + y, x0);
        for (int x = 0; x < w && x0 + x < COLS; x++) addch(' ');
    }
    attron(A_BOLD);
    for (int i = 0; i < n && y0 + 1 + i < LINES; i++) {
        if (i > 0) attroff(A_BOLD);
        mvprintw(y0 + 1 + i, x0 + 2, "%.*s", w - 4, txt[i]);
    }
    /* border around the overlay */
    attron(COLOR_PAIR(CP_LABEL));
    mvhline(y0, x0, ACS_HLINE, w);
    mvhline(y0 + h - 1, x0, ACS_HLINE, w);
    mvvline(y0, x0, ACS_VLINE, h);
    mvvline(y0, x0 + w - 1, ACS_VLINE, h);
    mvaddch(y0, x0, ACS_ULCORNER);
    mvaddch(y0, x0 + w - 1, ACS_URCORNER);
    mvaddch(y0 + h - 1, x0, ACS_LLCORNER);
    mvaddch(y0 + h - 1, x0 + w - 1, ACS_LRCORNER);
    attroff(COLOR_PAIR(CP_LABEL));
}

/* ---------- main draw ---------- */

void ui_draw(struct snapshot *s)
{
    g_snap = s;
    erase();
    int W = COLS, H = LINES, y = 0;
    char b1[32], b2[32], b3[32], b4[32];
    const struct topology *t = s->topo;

    /* title bar */
    attron(COLOR_PAIR(CP_TITLE));
    move(0, 0);
    for (int i = 0; i < W; i++) addch(' ');
    fmt_kb(t->l1d.size_kb, b1, sizeof b1);
    fmt_kb(t->l2.size_kb, b2, sizeof b2);
    fmt_kb(t->has_l3 ? t->l3.size_kb : 0, b3, sizeof b3);
    mvprintw(0, 1, "TLBanalyser %s %s  %dC/%dT %dskt %dnode  L1d %s L2 %s/%dc%s%s%s",
             TLBA_VERSION,
             t->model_name[0] ? t->model_name : "unknown CPU",
             t->ncore, t->ncpu, t->nsocket, t->nnode,
             b1, b2, t->l2.sharing,
             t->has_l3 ? " L3 " : "", t->has_l3 ? b3 : "",
             st.paused ? "  [PAUSED]" : "");
    attroff(COLOR_PAIR(CP_TITLE));
    y = 1;

    /* ---- CPU meters ---- */
    int n = t->ncpu;
    bool compact = st.compact_cpu || n > 64;
    if (!compact) {
        int cols = W / 34;
        if (cols < 1) cols = 1;
        if (cols > 4) cols = 4;
        int rows = (n + cols - 1) / cols;
        int mw = W / cols;
        for (int i = 0; i < n; i++)
            draw_cpu_meter(y + i % rows, (i / rows) * mw, mw - 1, i, &s->proc.load[i]);
        y += rows;
    } else {
        static double busy[MAX_CPUS];
        for (int i = 0; i < n; i++) busy[i] = s->proc.load[i].busy;
        attron(COLOR_PAIR(CP_LABEL));
        mvprintw(y, 0, "CPU busy ");
        attroff(COLOR_PAIR(CP_LABEL));
        double avg = 0;
        for (int i = 0; i < n; i++) avg += busy[i];
        avg /= n > 0 ? n : 1;
        draw_heat(y, 10, W - 20, busy, n, 0, 1);
        mvprintw(y, W - 8, "%5.1f%%", avg * 100);
        y++;
    }

    /* ---- CACHE section ---- */
    if (y < H - 3) {
        char hdr[128];
        snprintf(hdr, sizeof hdr, "CACHE / TLB MISSES  (per-CPU PMU%s)",
                 s->pmu_ok ? "" : " - UNAVAILABLE");
        section(y++, hdr);
    }
    if (!s->pmu_ok) {
        attron(COLOR_PAIR(CP_WARN));
        mvprintw(y++, 2, "%s", s->pmu_err);
        attroff(COLOR_PAIR(CP_WARN));
    } else {
        static double strip[MAX_CPUS];
        double instr_tot = 0, scaled = 0;
        for (int i = 0; i < n; i++) {
            instr_tot += s->pmu[i].v[EV_INSTR];
            scaled += s->pmu[i].scaled ? 1 : 0;
        }
        if (instr_tot <= 0) instr_tot = 1;
        struct {
            const char *lab;
            enum pmu_ev num, den;   /* den = EV__N -> per-1000-instr (MPKI) */
            double hi;              /* strip scale max */
        } rows[] = {
            { "L1d miss%", EV_L1D_MISS, EV_L1D_ACC, 0.25 },
            { "L2  miss%", EV_L2_MISS,  EV_L2_ACC,  0.60 },
            { "dTLB MPKI", EV_DTLB_MISS, EV__N,     5.0  },
            { "iTLB MPKI", EV_ITLB_MISS, EV__N,     2.0  },
        };
        for (size_t r = 0; r < sizeof rows / sizeof rows[0] && y < H - 3; r++) {
            double sum_n = 0, sum_d = 0;
            bool avail = true;
            for (int i = 0; i < n; i++) {
                double num = s->pmu[i].v[rows[r].num];
                if (!s->pmu[i].ok[rows[r].num]) { avail = false; continue; }
                sum_n += num;
                if (rows[r].den == EV__N) {
                    double in = s->pmu[i].v[EV_INSTR];
                    strip[i] = in > 0 ? num / (in / 1000.0) : 0;
                    sum_d += s->pmu[i].v[EV_INSTR];
                } else {
                    double den = s->pmu[i].v[rows[r].den];
                    strip[i] = den > 0 ? num / den : 0;
                    sum_d += den;
                }
            }
            attron(COLOR_PAIR(CP_LABEL));
            mvprintw(y, 0, "%-9s", rows[r].lab);
            attroff(COLOR_PAIR(CP_LABEL));
            if (!avail && sum_n == 0) {
                attron(A_DIM);
                mvprintw(y, 10, "event unavailable on this PMU");
                attroff(A_DIM);
                y++;
                continue;
            }
            draw_heat(y, 10, W - 34, strip, n, 0, rows[r].hi);
            double agg;
            char txt[64];
            if (rows[r].den == EV__N) {
                agg = sum_d > 0 ? sum_n / (sum_d / 1000.0) : 0;
                fmt_rate(sum_n, b1, sizeof b1);
                snprintf(txt, sizeof txt, "%6.2f avg  %s/s", agg, b1);
            } else {
                agg = sum_d > 0 ? sum_n / sum_d * 100.0 : 0;
                fmt_rate(sum_n, b1, sizeof b1);
                snprintf(txt, sizeof txt, "%5.1f%% avg  %s/s", agg, b1);
            }
            mvprintw(y, W - 23, "%22s", txt);
            y++;
        }
        attron(A_DIM);
        mvprintw(y++, 0, "L2 event: %s%s   IPC %.2f", s->l2_desc,
                 scaled > 0 ? "  [multiplexed]" : "",
                 ({ double c = 0; for (int i = 0; i < n; i++) c += s->pmu[i].v[EV_CYCLES];
                    c > 0 ? instr_tot / c : 0; }));
        attroff(A_DIM);
    }

    /* ---- TLB / IPI section ---- */
    if (y < H - 3) section(y++, "TLB SHOOTDOWNS & IPIs");
    {
        static double strip[MAX_CPUS];
        double mx = 1;
        for (int i = 0; i < n; i++) {
            strip[i] = s->proc.tlb_recv[i];
            if (strip[i] > mx) mx = strip[i];
        }
        attron(COLOR_PAIR(CP_LABEL));
        mvprintw(y, 0, "recv/CPU ");
        attroff(COLOR_PAIR(CP_LABEL));
        draw_heat(y, 10, W - 34, strip, n, 0, mx);
        fmt_rate(s->proc.tlb_recv_tot, b1, sizeof b1);
        mvprintw(y, W - 23, "%14s/s total", b1);
        y++;

        if (s->trace_ok && y < H - 3) {
            const uint64_t *cs = trace_cpu_sends(s->ts);
            double sm = 1, tot = 0;
            for (int i = 0; i < n; i++) {
                strip[i] = (double)cs[i] / s->dt;
                tot += strip[i];
                if (strip[i] > sm) sm = strip[i];
            }
            attron(COLOR_PAIR(CP_LABEL));
            mvprintw(y, 0, "send/CPU ");
            attroff(COLOR_PAIR(CP_LABEL));
            draw_heat(y, 10, W - 34, strip, n, 0, sm);
            fmt_rate(tot, b1, sizeof b1);
            mvprintw(y, W - 23, "%14s/s total", b1);
            y++;
        }
        if (y < H - 3) {
            fmt_rate(s->proc.res_tot, b1, sizeof b1);
            fmt_rate(s->proc.cal_tot, b2, sizeof b2);
            fmt_rate((double)s->pages_tot / s->dt, b3, sizeof b3);
            mvprintw(y, 0, "IPIs: resched %s/s  func-call %s/s   flushed pages %s/s",
                     b1, b2, b3);
            if (s->trace_ok) {
                if (s->lost) {
                    attron(COLOR_PAIR(CP_WARN) | A_BOLD);
                    printw("   LOST %lu attribution events!", (unsigned long)s->lost);
                    attroff(COLOR_PAIR(CP_WARN) | A_BOLD);
                } else {
                    attron(A_DIM);
                    printw("   lost 0 (exact)");
                    if (s->lost_ctx)
                        printw("  ctx-drop %lu", (unsigned long)s->lost_ctx);
                    attroff(A_DIM);
                }
            }
            y++;
        }
        if (s->trace_ok && y < H - 3) {
            move(y, 0);
            attron(COLOR_PAIR(CP_LABEL));
            printw("reasons: ");
            attroff(COLOR_PAIR(CP_LABEL));
            for (int r = 0; r < 6; r++) {
                fmt_rate((double)s->reason_tot[r] / s->dt, b1, sizeof b1);
                if (r == R_REMOTE_SEND) attron(A_BOLD);
                printw("%s %s/s  ", tlb_reason_short[r], b1);
                if (r == R_REMOTE_SEND) attroff(A_BOLD);
            }
            y++;
        }
        if (s->trace_ok && y < H - 3 && s->norigin > 0) {
            move(y, 0);
            attron(COLOR_PAIR(CP_LABEL));
            printw("origins: ");
            attroff(COLOR_PAIR(CP_LABEL));
            uint64_t tot = 0;
            for (int i = 0; i < s->norigin; i++) tot += s->origins[i].cnt;
            int shown = 0;
            for (int i = 0; i < s->norigin && shown < 4; i++) {
                if (!s->origins[i].cnt) break;
                printw("%s %.0f%%  ", trace_symname(s->ts, s->origins[i].sym),
                       tot ? 100.0 * s->origins[i].cnt / tot : 0);
                shown++;
            }
            if (!shown) {
                attron(A_DIM);
                printw("(idle)");
                attroff(A_DIM);
            }
            y++;
        }
        if (y < H - 3) {
            fmt_rate(s->proc.thp_collapse, b1, sizeof b1);
            fmt_rate(s->proc.thp_split_pmd, b2, sizeof b2);
            fmt_rate(s->proc.pgmigrate, b3, sizeof b3);
            fmt_rate(s->proc.pgsteal, b4, sizeof b4);
            attron(COLOR_PAIR(CP_LABEL));
            mvprintw(y, 0, "drivers: ");
            attroff(COLOR_PAIR(CP_LABEL));
            char b5[32], b6[32];
            fmt_rate(s->proc.compact_stall, b5, sizeof b5);
            fmt_rate(s->proc.numa_pte_updates, b6, sizeof b6);
            printw("THPcollapse %s/s  PMDsplit %s/s  migrate %s/s  reclaim %s/s  "
                   "compactstall %s/s  numa_pte %s/s", b1, b2, b3, b4, b5, b6);
            y++;
        }
    }

    /* ---- attribution table ---- */
    if (y < H - 2) {
        char hdr[96];
        snprintf(hdr, sizeof hdr, "WHO IS FLUSHING  (sort: %s%s)",
                 sort_name[st.sort], st.cumulative ? ", cumulative" : "");
        section(y++, hdr);
    }
    if (!s->trace_ok) {
        attron(COLOR_PAIR(CP_WARN));
        if (y < H - 1) mvprintw(y++, 2, "%s", s->trace_err);
        attroff(COLOR_PAIR(CP_WARN));
    } else if (y < H - 1) {
        attron(COLOR_PAIR(CP_TABHDR));
        move(y, 0);
        printw("%7s %-16s %8s %9s %8s %8s %8s %8s  %-24s",
               "PID", "COMM", st.cumulative ? "ΣSEND" : "SEND/s",
               st.cumulative ? "ΣPAGES" : "PAGES/s",
               "LOCAL", "LOCMM", "SWTCH", "RECV", "ORIGIN");
        for (int i = getcurx(stdscr); i < W; i++) addch(' ');
        attroff(COLOR_PAIR(CP_TABHDR));
        y++;

        qsort(s->pids, s->npid, sizeof s->pids[0], row_cmp);
        int rows_avail = H - 1 - y;
        int max_scroll = s->npid > rows_avail ? s->npid - rows_avail : 0;
        if (st.scroll > max_scroll) st.scroll = max_scroll;

        for (int i = st.scroll; i < s->npid && y < H - 1; i++, y++) {
            struct pid_stat *p = &s->pids[i];
            uint64_t tot = 0;
            for (int r = 0; r < REASON_MAX; r++)
                tot += st.cumulative ? p->cum[r] : p->cnt[r];
            if (tot == 0 && st.sort != SORT_CUMSEND && !st.cumulative) {
                attron(A_DIM);
            }
            char c1[16], c2[16], c3[16], c4[16], c5[16], c6[16];
            if (st.cumulative) {
                fmt_count(p->cum[R_REMOTE_SEND], c1, sizeof c1);
                fmt_count(p->pages_cum, c2, sizeof c2);
                fmt_count(p->cum[R_LOCAL], c3, sizeof c3);
                fmt_count(p->cum[R_LOCAL_MM], c4, sizeof c4);
                fmt_count(p->cum[R_TASK_SWITCH], c5, sizeof c5);
                fmt_count(p->cum[R_REMOTE_RECV], c6, sizeof c6);
            } else {
                fmt_rate(p->cnt[R_REMOTE_SEND] / s->dt, c1, sizeof c1);
                fmt_rate(p->pages / s->dt, c2, sizeof c2);
                fmt_rate(p->cnt[R_LOCAL] / s->dt, c3, sizeof c3);
                fmt_rate(p->cnt[R_LOCAL_MM] / s->dt, c4, sizeof c4);
                fmt_rate(p->cnt[R_TASK_SWITCH] / s->dt, c5, sizeof c5);
                fmt_rate(p->cnt[R_REMOTE_RECV] / s->dt, c6, sizeof c6);
            }
            /* dominant origin */
            uint32_t best = 0;
            uint64_t bc = 0;
            for (int k = 0; k < 4; k++)
                if (p->origin_cnt[k] > bc) { bc = p->origin_cnt[k]; best = p->origin_sym[k]; }
            char comm[COMM_LEN + 3];
            snprintf(comm, sizeof comm, p->kthread ? "[%s]" : "%s", p->comm);
            bool hot = (st.cumulative ? p->cum[R_REMOTE_SEND] : p->cnt[R_REMOTE_SEND]) > 0;
            if (hot) attron(A_BOLD);
            mvprintw(y, 0, "%7d %-16.16s %8s %9s %8s %8s %8s %8s  %-24.24s",
                     p->pid, comm, c1, c2, c3, c4, c5, c6,
                     best ? trace_symname(s->ts, best) : "-");
            if (hot) attroff(A_BOLD);
            attroff(A_DIM);
        }
    }

    /* ---- key bar ---- */
    move(H - 1, 0);
    static const struct { const char *k, *lab; } keys[] = {
        { "F1", "Help" }, { "F5", "Sort" }, { "c", "Cumul" }, { "p", "Pause" },
        { "m", "Grid" }, { "r", "Reset" }, { "q", "Quit" },
    };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        attron(A_BOLD);
        printw("%s", keys[i].k);
        attroff(A_BOLD);
        attron(COLOR_PAIR(CP_FKEY));
        printw("%-6s", keys[i].lab);
        attroff(COLOR_PAIR(CP_FKEY));
    }
    attron(A_DIM);
    char up[32];
    snprintf(up, sizeof up, " up %d:%02d:%02d", (int)s->uptime / 3600,
             ((int)s->uptime / 60) % 60, (int)s->uptime % 60);
    mvprintw(H - 1, W - (int)strlen(up) - 1, "%s", up);
    attroff(A_DIM);

    if (st.help) draw_help();
    refresh();
}

/*
 * htop-style front-end with two rendering backends sharing one draw path:
 *  - ncurses (default)
 *  - raw ANSI/VT100 escape codes (-A, or automatic when the host's curses
 *    cannot initialize a terminal - e.g. broken/absent terminfo databases)
 */
#include "tlba.h"
#include <ncurses.h>
#include <math.h>
#include <stdarg.h>
#include <termios.h>
#include <poll.h>
#include <unistd.h>
#include <sys/ioctl.h>

enum { CP_LABEL = 1, CP_USER, CP_SYS, CP_IRQ, CP_SOFTIRQ, CP_NICE,
       CP_FKEY, CP_TABHDR, CP_SEL, CP_HEAT_LO, CP_HEAT_MID, CP_HEAT_HI,
       CP_TITLE, CP_WARN, CP__N };

/* attribute byte: pair index + bold/dim flags */
#define AT_BOLD 0x40u
#define AT_DIM  0x80u
#define AT_PAIR(a) ((a) & 0x3fu)

enum sort_mode { SORT_SEND = 0, SORT_TOTAL, SORT_RECV, SORT_CUMSEND, SORT__N };
static const char *sort_name[SORT__N] = { "SEND/s", "TOTAL/s", "RECV/s", "cumSEND" };

static struct {
    int  sort;
    int  scroll;
    bool paused;
    bool help;
    int  cpu_view;      /* 0 auto, 1 force meters, 2 force compact strip */
    bool cumulative;    /* show cumulative counts in table */
    bool show_all;      /* include tasks with no flush activity */
} st;

/* flush activity = everything except benign context-switch flushes */
static uint64_t flush_activity(const struct pid_stat *p, bool cumulative)
{
    const uint64_t *v = cumulative ? p->cum : p->cnt;
    return v[R_REMOTE_SEND] + v[R_LOCAL] + v[R_LOCAL_MM] +
           v[R_REMOTE_RECV] + v[R_WRONG_CPU];
}

static bool ansi;

/* ================= ANSI grid backend ================= */

#define G_MAXR 200
#define G_MAXC 500
static struct cell { char ch; unsigned char at; } grid[G_MAXR][G_MAXC];
static int g_rows = 24, g_cols = 80;
static struct termios g_tio_save;
static bool g_tio_saved;

/* pair -> SGR color codes (fg, bg; 0 = default) */
static const struct { unsigned char fg, bg; } sgr_pair[CP__N] = {
    [0]           = {  0,  0 },
    [CP_LABEL]    = { 36,  0 }, [CP_USER]     = { 32,  0 },
    [CP_SYS]      = { 31,  0 }, [CP_IRQ]      = { 33,  0 },
    [CP_SOFTIRQ]  = { 35,  0 }, [CP_NICE]     = { 34,  0 },
    [CP_FKEY]     = { 30, 46 }, [CP_TABHDR]   = { 30, 42 },
    [CP_SEL]      = { 30, 46 }, [CP_HEAT_LO]  = { 32,  0 },
    [CP_HEAT_MID] = { 33,  0 }, [CP_HEAT_HI]  = { 31,  0 },
    [CP_TITLE]    = { 30, 46 }, [CP_WARN]     = { 31,  0 },
};

static void g_getsize(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        g_cols = ws.ws_col;
        g_rows = ws.ws_row;
    }
    if (g_cols > G_MAXC) g_cols = G_MAXC;
    if (g_rows > G_MAXR) g_rows = G_MAXR;
}

static void g_clear(void)
{
    for (int y = 0; y < g_rows; y++)
        for (int x = 0; x < g_cols; x++) {
            grid[y][x].ch = ' ';
            grid[y][x].at = 0;
        }
}

static char g_out[2 * 1024 * 1024];

static void g_emit(void)
{
    size_t o = 0;
    o += (size_t)snprintf(g_out + o, sizeof g_out - o, "\x1b[H");
    unsigned cur = 0xffu;
    for (int y = 0; y < g_rows; y++) {
        o += (size_t)snprintf(g_out + o, sizeof g_out - o, "\x1b[%d;1H", y + 1);
        int last = g_cols - 1;
        while (last > 0 && grid[y][last].ch == ' ' && grid[y][last].at == 0) last--;
        for (int x = 0; x <= last && o < sizeof g_out - 24; x++) {
            unsigned at = grid[y][x].at;
            if (at != cur) {
                unsigned p = AT_PAIR(at);
                o += (size_t)snprintf(g_out + o, sizeof g_out - o, "\x1b[0%s%s",
                                      (at & AT_BOLD) ? ";1" : "",
                                      (at & AT_DIM) ? ";2" : "");
                if (p && sgr_pair[p].fg)
                    o += (size_t)snprintf(g_out + o, sizeof g_out - o, ";%d", sgr_pair[p].fg);
                if (p && sgr_pair[p].bg)
                    o += (size_t)snprintf(g_out + o, sizeof g_out - o, ";%d", sgr_pair[p].bg);
                g_out[o++] = 'm';
                cur = at;
            }
            g_out[o++] = grid[y][x].ch;
        }
        if (cur != 0) {
            o += (size_t)snprintf(g_out + o, sizeof g_out - o, "\x1b[0m");
            cur = 0;
        }
        o += (size_t)snprintf(g_out + o, sizeof g_out - o, "\x1b[K");
    }
    o += (size_t)snprintf(g_out + o, sizeof g_out - o, "\x1b[0m");
    ssize_t r = write(STDOUT_FILENO, g_out, o);
    (void)r;
}

/* ================= drawing primitives (both backends) ================= */

static int P_rows(void) { return ansi ? g_rows : LINES; }
static int P_cols(void) { return ansi ? g_cols : COLS; }

static attr_t nc_attr(unsigned at)
{
    return (attr_t)(COLOR_PAIR(AT_PAIR(at)) |
                    ((at & AT_BOLD) ? A_BOLD : 0) |
                    ((at & AT_DIM) ? A_DIM : 0));
}

static void P_putc(int y, int x, unsigned at, char ch)
{
    if (y < 0 || x < 0 || y >= P_rows() || x >= P_cols()) return;
    if (ansi) {
        grid[y][x].ch = ch;
        grid[y][x].at = (unsigned char)at;
    } else {
        attr_t a = nc_attr(at);
        attron(a);
        mvaddch(y, x, (chtype)ch);
        attroff(a);
    }
}

/* printf at position; clips to width; returns x after the text */
static int P_print(int y, int x, unsigned at, const char *fmt, ...)
{
    char buf[G_MAXC + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    int len = (int)strlen(buf);
    if (y < 0 || y >= P_rows()) return x + len;
    if (ansi) {
        for (int i = 0; i < len; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= g_cols) continue;
            grid[y][xx].ch = buf[i];
            grid[y][xx].at = (unsigned char)at;
        }
    } else {
        int w = P_cols() - x;
        if (w > 0) {
            attr_t a = nc_attr(at);
            attron(a);
            mvaddnstr(y, x, buf, w);
            attroff(a);
        }
    }
    return x + len;
}

static void P_hline(int y, int x, int w, unsigned at)
{
    if (ansi) {
        for (int i = 0; i < w; i++) P_putc(y, x + i, at, '-');
    } else {
        attr_t a = nc_attr(at);
        attron(a);
        for (int i = 0; i < w && x + i < COLS; i++) mvaddch(y, x + i, ACS_HLINE);
        attroff(a);
    }
}

static void P_frame_begin(void)
{
    if (ansi) {
        g_getsize();
        g_clear();
    } else {
        erase();
    }
}

static void P_frame_end(void)
{
    if (ansi) g_emit();
    else refresh();
}

/* ================= init / input ================= */

void ui_init(bool use_ansi)
{
    ansi = use_ansi;
    if (ansi) {
        if (tcgetattr(STDIN_FILENO, &g_tio_save) == 0) {
            g_tio_saved = true;
            struct termios t = g_tio_save;
            t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
            t.c_cc[VMIN] = 0;
            t.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &t);
        }
        /* alt screen, hide cursor */
        ssize_t r = write(STDOUT_FILENO, "\x1b[?1049h\x1b[?25l\x1b[2J", 18);
        (void)r;
        g_getsize();
        return;
    }
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    timeout(50);            /* getch timeout doubles as UI pacing */
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

void ui_done(void)
{
    if (ansi) {
        ssize_t r = write(STDOUT_FILENO, "\x1b[0m\x1b[?25h\x1b[?1049l", 18);
        (void)r;
        if (g_tio_saved) tcsetattr(STDIN_FILENO, TCSANOW, &g_tio_save);
        return;
    }
    endwin();
}

/* blocking up to ~50ms; returns key or -1 */
int ui_getch(void)
{
    if (!ansi) {
        int ch = getch();
        return ch == ERR ? -1 : ch;
    }
    struct pollfd pf = { .fd = STDIN_FILENO, .events = POLLIN };
    if (poll(&pf, 1, 50) <= 0) return -1;
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    if (c != 0x1b) return c;
    /* escape sequence: arrows / pgup / pgdn / home / F-keys */
    unsigned char seq[4] = {0};
    struct pollfd p2 = { .fd = STDIN_FILENO, .events = POLLIN };
    if (poll(&p2, 1, 10) <= 0) return 0x1b;
    if (read(STDIN_FILENO, seq, 1) != 1) return 0x1b;
    if (seq[0] == 'O') {                       /* SS3: F1-F4 */
        if (read(STDIN_FILENO, seq + 1, 1) != 1) return -1;
        if (seq[1] == 'P') return 'h';         /* F1 = help */
        return -1;
    }
    if (seq[0] != '[') return -1;
    if (read(STDIN_FILENO, seq + 1, 1) != 1) return -1;
    switch (seq[1]) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'H': return KEY_HOME;
    case '5': case '6': case '1': {
        unsigned char rest[3] = {0};
        if (read(STDIN_FILENO, rest, 1) != 1) return -1;
        if (seq[1] == '5' && rest[0] == '~') return KEY_PPAGE;
        if (seq[1] == '6' && rest[0] == '~') return KEY_NPAGE;
        if (seq[1] == '1' && rest[0] == '5') {  /* ESC [ 1 5 ~  = F5 */
            if (read(STDIN_FILENO, rest + 1, 1) == 1 && rest[1] == '~') return 's';
        }
        return -1;
    }
    }
    return -1;
}

/* returns: 0 none, 1 quit, 2 reset-cumulative */
int ui_key(int ch)
{
    switch (ch) {
    case 'q': case 'Q': case KEY_F(10): return 1;
    case 'r': case 'R': return 2;
    case 'p': case ' ': st.paused = !st.paused; break;
    case 'h': case '?': case KEY_F(1): st.help = !st.help; break;
    case 'm': st.cpu_view = (st.cpu_view + 1) % 3; break;
    case 'z': st.show_all = !st.show_all; break;
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
    P_print(y, x, CP_LABEL, "%3d", cpu);
    P_putc(y, x + 3, AT_BOLD, '[');
    P_putc(y, x + 3 + inner + 1, AT_BOLD, ']');

    struct { float v; unsigned at; } seg[] = {
        { l->nice, CP_NICE }, { l->user, CP_USER }, { l->system, CP_SYS },
        { l->irq, CP_IRQ }, { l->softirq, CP_SOFTIRQ }, { l->steal, CP_IRQ },
    };
    int pos = 0;
    for (size_t i = 0; i < sizeof seg / sizeof seg[0]; i++) {
        int n = (int)lroundf(seg[i].v * (float)inner);
        if (pos + n > inner) n = inner - pos;
        for (int k = 0; k < n; k++) P_putc(y, x + 4 + pos + k, seg[i].at, '|');
        if (n > 0) pos += n;
    }
    char pct[8];
    snprintf(pct, sizeof pct, "%.1f%%", l->busy * 100.0);
    P_print(y, x + 4 + inner - (int)strlen(pct), AT_DIM, "%s", pct);
}

/* heat strip: one cell per CPU (or aggregated), colored by value/max */
static void draw_heat(int y, int x, int w, const double *val, int n, double lo, double hi)
{
    P_putc(y, x, AT_BOLD, '[');
    int cells = w - 2;
    if (cells < 1) return;
    if (cells > n) cells = n;                 /* one cell per CPU, no dead space */
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
        unsigned at = (f < 0.4 ? CP_HEAT_LO : f < 0.75 ? CP_HEAT_MID : CP_HEAT_HI) |
                      (g >= 7 ? AT_BOLD : 0);
        P_putc(y, x + 1 + c, at, glyph[g]);
    }
    P_putc(y, x + 1 + cells, AT_BOLD, ']');
}

static void section(int y, const char *title)
{
    P_print(y, 0, CP_LABEL | AT_BOLD, "%s", title);
    P_hline(y, (int)strlen(title) + 1, P_cols() - (int)strlen(title) - 1, AT_DIM);
}

/* ---------- table sorting ---------- */

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
        "  SEND    remote shootdowns this task initiated  <- the culprit",
        "  PAGES   pages invalidated by those sends (0 pages = full flush)",
        "  LOCAL   local-only flushes (own CPU, no shootdown)",
        "  RECV    shootdowns that interrupted this task (victim, not cause)",
        "  SWTCH   lazy-TLB flushes at context switch (benign)",
        "  %FL     this task's share of all flush activity",
        "  WHY     kernel function behind the flushes + plain-English meaning",
        "",
        "Tasks with no flush activity are hidden by default (press z).",
        "",
        "KEYS  s/F5 sort   c cumulative   z show all   p pause   m CPU view",
        "      r reset cumulative   arrows/PgUp/PgDn scroll   q quit",
        "",
        "                      press h to close",
    };
    int n = (int)(sizeof txt / sizeof txt[0]);
    int w = 74, h = n + 2;
    int y0 = (P_rows() - h) / 2, x0 = (P_cols() - w) / 2;
    if (y0 < 0) y0 = 0;
    if (x0 < 0) x0 = 0;
    for (int y = 0; y < h && y0 + y < P_rows(); y++)
        for (int x = 0; x < w && x0 + x < P_cols(); x++)
            P_putc(y0 + y, x0 + x, 0, ' ');
    for (int i = 0; i < n && y0 + 1 + i < P_rows(); i++)
        P_print(y0 + 1 + i, x0 + 2, i == 0 ? AT_BOLD : 0, "%.*s", w - 4, txt[i]);
    /* border */
    P_hline(y0, x0, w, CP_LABEL);
    P_hline(y0 + h - 1, x0, w, CP_LABEL);
    for (int y = 0; y < h; y++) {
        P_putc(y0 + y, x0, CP_LABEL, '|');
        P_putc(y0 + y, x0 + w - 1, CP_LABEL, '|');
    }
    P_putc(y0, x0, CP_LABEL, '+');
    P_putc(y0, x0 + w - 1, CP_LABEL, '+');
    P_putc(y0 + h - 1, x0, CP_LABEL, '+');
    P_putc(y0 + h - 1, x0 + w - 1, CP_LABEL, '+');
}

/* ---------- main draw ---------- */

void ui_draw(struct snapshot *s)
{
    P_frame_begin();
    int W = P_cols(), H = P_rows(), y = 0;
    /* cap panel width on very wide terminals so values stay near their strips */
    int Wc = W > 170 ? 170 : W;
    /* shared geometry for all per-CPU strips: label(10) strip(SW) text(right) */
    int SW = Wc - 56;
    if (SW < 14) SW = Wc > 42 ? Wc - 28 : 14;
    int TX = 10 + SW + 2;
    char b1[32], b2[32], b3[32], b4[32];
    const struct topology *t = s->topo;

    /* title bar */
    for (int i = 0; i < W; i++) P_putc(0, i, CP_TITLE, ' ');
    fmt_kb(t->l1d.size_kb, b1, sizeof b1);
    fmt_kb(t->l2.size_kb, b2, sizeof b2);
    fmt_kb(t->has_l3 ? t->l3.size_kb : 0, b3, sizeof b3);
    P_print(0, 1, CP_TITLE,
            "TLBanalyser %s %s  %dC/%dT %dskt %dnode  L1d %s L2 %s/%dc%s%s%s",
            TLBA_VERSION, t->model_name[0] ? t->model_name : "unknown CPU",
            t->ncore, t->ncpu, t->nsocket, t->nnode,
            b1, b2, t->l2.sharing,
            t->has_l3 ? " L3 " : "", t->has_l3 ? b3 : "",
            st.paused ? "  [PAUSED]" : "");
    y = 1;

    /* ---- setup problems, front and centre ---- */
    if (!s->trace_ok || !s->pmu_ok) {
        section(y++, "SETUP NEEDED");
        if (!s->trace_ok) {
            P_print(y++, 1, CP_WARN | AT_BOLD, "! flush attribution offline: %s",
                    s->trace_err);
            P_print(y++, 3, AT_DIM, "run 'sudo ./selftest.sh' to diagnose; "
                    "showing /proc page-fault churn suspects below instead");
        }
        if (!s->pmu_ok)
            P_print(y++, 1, CP_WARN | AT_BOLD, "! PMU counters offline: %s",
                    s->pmu_err);
    }

    /* ---- CPU meters ---- */
    int n = t->ncpu;
    int cols = W / 34;              /* meters may use the full width */
    if (cols < 1) cols = 1;
    if (cols > 8) cols = 8;
    int rows = (n + cols - 1) / cols;
    bool compact = st.cpu_view == 2 || (st.cpu_view == 0 && rows > 16);
    if (!compact) {
        int mw = W / cols;
        for (int i = 0; i < n; i++)
            draw_cpu_meter(y + i % rows, (i / rows) * mw, mw - 1, i, &s->proc.load[i]);
        y += rows;
    } else {
        static double busy[MAX_CPUS];
        for (int i = 0; i < n; i++) busy[i] = s->proc.load[i].busy;
        int cells = SW - 2 < n ? SW - 2 : n;
        int per = cells > 0 ? (n + cells - 1) / cells : 1;
        P_print(y++, 0, AT_DIM, "strips below: CPU0 at left, CPU%d at right, "
                "%d CPU%s per cell;  ' '=zero  '.'=low  '@'=high", n - 1, per,
                per > 1 ? "s" : "");
        P_print(y, 0, CP_LABEL, "CPU busy ");
        double avg = 0;
        for (int i = 0; i < n; i++) avg += busy[i];
        avg /= n > 0 ? n : 1;
        draw_heat(y, 10, SW, busy, n, 0, 1);
        P_print(y, TX, 0, "avg %.1f%% busy across %d CPUs", avg * 100, n);
        y++;
    }

    /* ---- CACHE section (only when the PMU delivers) ---- */
    if (s->pmu_ok) {
        if (y < H - 3) section(y++, "CACHE / TLB MISSES  (per-CPU PMU)");
        if (!compact && y < H - 3) {
            int cells = SW - 2 < n ? SW - 2 : n;
            int per = cells > 0 ? (n + cells - 1) / cells : 1;
            P_print(y++, 0, AT_DIM, "strips: CPU0 at left, CPU%d at right, "
                    "%d CPU%s per cell;  ' '=zero  '.'=low  '@'=high",
                    n - 1, per, per > 1 ? "s" : "");
        }
        static double strip[MAX_CPUS];
        double instr_tot = 0, cyc_tot = 0, scaled = 0;
        for (int i = 0; i < n; i++) {
            instr_tot += s->pmu[i].v[EV_INSTR];
            cyc_tot += s->pmu[i].v[EV_CYCLES];
            scaled += s->pmu[i].scaled ? 1 : 0;
        }
        if (instr_tot <= 0) instr_tot = 1;
        static const struct {
            const char *lab, *what;
            enum pmu_ev num, den;   /* den = EV__N -> per-1000-instr rate */
            double hi;              /* strip scale max */
        } cache_rows[] = {
            { "L1d miss ", "of loads miss L1d",   EV_L1D_MISS, EV_L1D_ACC, 0.25 },
            { "L2  miss ", "of L2 lookups miss",  EV_L2_MISS,  EV_L2_ACC,  0.60 },
            { "dTLB walk", "data-TLB",            EV_DTLB_MISS, EV__N,     5.0  },
            { "iTLB walk", "instruction-TLB",     EV_ITLB_MISS, EV__N,     2.0  },
        };
        for (size_t r = 0; r < sizeof cache_rows / sizeof cache_rows[0] && y < H - 3; r++) {
            double sum_n = 0, sum_d = 0;
            bool avail = true;
            for (int i = 0; i < n; i++) {
                double num = s->pmu[i].v[cache_rows[r].num];
                if (!s->pmu[i].ok[cache_rows[r].num]) { avail = false; continue; }
                sum_n += num;
                if (cache_rows[r].den == EV__N) {
                    double in = s->pmu[i].v[EV_INSTR];
                    strip[i] = in > 0 ? num / (in / 1000.0) : 0;
                    sum_d += s->pmu[i].v[EV_INSTR];
                } else {
                    double den = s->pmu[i].v[cache_rows[r].den];
                    strip[i] = den > 0 ? num / den : 0;
                    sum_d += den;
                }
            }
            P_print(y, 0, CP_LABEL, "%-9s", cache_rows[r].lab);
            if (!avail && sum_n == 0) {
                /* keep the bracket column so unavailable rows stay aligned */
                P_putc(y, 10, AT_BOLD, '[');
                P_putc(y, 10 + SW - 1, AT_BOLD, ']');
                P_print(y, 12, AT_DIM,
                        "counter not available on this machine");
                y++;
                continue;
            }
            draw_heat(y, 10, SW, strip, n, 0, cache_rows[r].hi);
            fmt_rate(sum_n, b1, sizeof b1);
            if (cache_rows[r].den == EV__N) {
                double agg = sum_d > 0 ? sum_n / (sum_d / 1000.0) : 0;
                P_print(y, TX, 0, "%.2f misses per 1k instructions (%s/s)",
                        agg, b1);
            } else {
                double agg = sum_d > 0 ? sum_n / sum_d * 100.0 : 0;
                P_print(y, TX, 0, "%.1f%% %s (%s misses/s)",
                        agg, cache_rows[r].what, b1);
            }
            y++;
        }
        P_print(y++, 0, AT_DIM, "IPC %.2f (instructions per cycle)   L2 counters: %s%s",
                cyc_tot > 0 ? instr_tot / cyc_tot : 0, s->l2_desc,
                scaled > 0 ? "   (counters time-shared, values scaled)" : "");
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
        P_print(y, 0, CP_LABEL, "recv/CPU ");
        draw_heat(y, 10, SW, strip, n, 0, mx);
        fmt_rate(s->proc.tlb_recv_tot, b1, sizeof b1);
        P_print(y, TX, 0, "%s shootdown IPIs received/s", b1);
        y++;

        if (s->trace_ok && y < H - 3) {
            const uint64_t *cs = trace_cpu_sends(s->ts);
            double sm = 1, tot = 0;
            for (int i = 0; i < n; i++) {
                strip[i] = (double)cs[i] / s->dt;
                tot += strip[i];
                if (strip[i] > sm) sm = strip[i];
            }
            P_print(y, 0, CP_LABEL, "send/CPU ");
            draw_heat(y, 10, SW, strip, n, 0, sm);
            fmt_rate(tot, b1, sizeof b1);
            P_print(y, TX, 0, "%s flushes initiated/s", b1);
            y++;
        }
        if (y < H - 3) {
            fmt_rate(s->proc.res_tot, b1, sizeof b1);
            fmt_rate(s->proc.cal_tot, b2, sizeof b2);
            fmt_rate((double)s->pages_tot / s->dt, b3, sizeof b3);
            int x = P_print(y, 0, 0,
                            "IPIs: resched %s/s  func-call %s/s   flushed pages %s/s",
                            b1, b2, b3);
            if (s->trace_ok) {
                if (s->lost) {
                    P_print(y, x, CP_WARN | AT_BOLD, "   LOST %lu attribution events!",
                            (unsigned long)s->lost);
                } else {
                    x = P_print(y, x, AT_DIM, "   lost 0 (exact)");
                    if (s->lost_ctx)
                        P_print(y, x, AT_DIM, "  ctx-drop %lu",
                                (unsigned long)s->lost_ctx);
                }
            }
            y++;
        }
        if (s->trace_ok && y < H - 3) {
            int x = P_print(y, 0, CP_LABEL, "reasons: ");
            for (int r = 0; r < 6; r++) {
                fmt_rate((double)s->reason_tot[r] / s->dt, b1, sizeof b1);
                x = P_print(y, x, r == R_REMOTE_SEND ? AT_BOLD : 0,
                            "%s %s/s  ", tlb_reason_short[r], b1);
            }
            P_print(y, x, AT_DIM, "  [%s]", trace_mode(s->ts));
            y++;
        }
        if (s->trace_ok && y < H - 3 && s->norigin > 0) {
            int x = P_print(y, 0, CP_LABEL, "origins: ");
            uint64_t tot = 0;
            for (int i = 0; i < s->norigin; i++) tot += s->origins[i].cnt;
            int shown = 0;
            for (int i = 0; i < s->norigin && shown < 4; i++) {
                if (!s->origins[i].cnt) break;
                x = P_print(y, x, 0, "%s %.0f%%  ",
                            trace_symname(s->ts, s->origins[i].sym),
                            tot ? 100.0 * s->origins[i].cnt / tot : 0);
                shown++;
            }
            if (!shown) P_print(y, x, AT_DIM, "(idle)");
            y++;
        }
        if (y < H - 2) {
            fmt_rate(s->proc.thp_collapse, b1, sizeof b1);
            fmt_rate(s->proc.thp_split_pmd, b2, sizeof b2);
            fmt_rate(s->proc.pgmigrate, b3, sizeof b3);
            fmt_rate(s->proc.pgsteal, b4, sizeof b4);
            char b5[32], b6[32];
            fmt_rate(s->proc.compact_stall, b5, sizeof b5);
            fmt_rate(s->proc.numa_pte_updates, b6, sizeof b6);
            int x = P_print(y, 0, CP_LABEL, "drivers: ");
            P_print(y, x, 0, "THPcollapse %s/s  PMDsplit %s/s  migrate %s/s  "
                    "reclaim %s/s  compactstall %s/s  numa_pte %s/s",
                    b1, b2, b3, b4, b5, b6);
            y++;
        }
        /* one-line verdict: what is causing the shootdowns right now */
        if (y < H - 2) {
            int x = P_print(y, 0, CP_LABEL | AT_BOLD, "assess:  ");
            if (!s->trace_ok) {
                P_print(y, x, AT_DIM, "attribution offline (tracepoint unavailable); "
                        "IPI receive counts above remain exact");
            } else {
                /* judge by IPI sends; if the host does no-IPI broadcast
                 * invalidation (AMD INVLPGB), judge by flush events instead */
                bool ipi = s->reason_tot[R_REMOTE_SEND] > 0;
                double total = ipi ? (double)s->reason_tot[R_REMOTE_SEND]
                    : (double)(s->reason_tot[R_REMOTE_RECV] + s->reason_tot[R_LOCAL] +
                               s->reason_tot[R_LOCAL_MM] + s->reason_tot[R_WRONG_CPU]);
                if (total < 1) {
                    P_print(y, x, AT_DIM,
                            "no shootdown/flush activity this interval "
                            "(recv %.0f IPIs/s)", s->proc.tlb_recv_tot);
                } else {
                    struct pid_stat *top = NULL;
                    uint64_t tm = 0;
                    for (int i = 0; i < s->npid; i++) {
                        struct pid_stat *p = &s->pids[i];
                        uint64_t m = ipi ? p->cnt[R_REMOTE_SEND]
                            : p->cnt[R_REMOTE_RECV] + p->cnt[R_LOCAL] +
                              p->cnt[R_LOCAL_MM] + p->cnt[R_WRONG_CPU];
                        if (m > tm) { tm = m; top = p; }
                    }
                    if (top) {
                        uint32_t bs = 0;
                        uint64_t bc = 0;
                        for (int k = 0; k < 4; k++)
                            if (top->origin_cnt[k] > bc) { bc = top->origin_cnt[k]; bs = top->origin_sym[k]; }
                        const char *sym = bs ? trace_symname(s->ts, bs) : NULL;
                        const char *hint = sym ? origin_hint(sym) : NULL;
                        x = P_print(y, x, AT_BOLD, "%s (pid %d) causes %.0f%% of %s",
                                    top->comm[0] ? top->comm : "?", top->pid,
                                    100.0 * (double)tm / total,
                                    ipi ? "IPI sends" : "TLB flushes (no-IPI/broadcast host)");
                        if (sym)
                            P_print(y, x, 0, "  via %s%s%s%s", sym,
                                    hint ? " = " : "", hint ? hint : "",
                                    top->kthread ? "  [kernel thread]" : "");
                    }
                }
            }
            y++;
        }
    }

    /* ---- attribution table ---- */
    if (!s->trace_ok) {
        /* /proc fallback: page-fault churn points at mmap-heavy processes */
        if (y < H - 2)
            section(y++, "SUSPECTS  (heuristic: page-fault/mmap churn - "
                         "flush events not available)");
        if (y < H - 1) {
            char hb[G_MAXC + 1];
            snprintf(hb, sizeof hb, "%7s %-16s %11s %11s %10s",
                     "PID", "COMM", "MINFLT/s", "MAJFLT/s", "RSS-MB");
            for (int i = 0; i < W; i++)
                P_putc(y, i, CP_TABHDR, i < (int)strlen(hb) ? hb[i] : ' ');
            y++;
            for (int i = 0; i < s->nsusp && y < H - 1; i++, y++) {
                struct suspect *sp = &s->susp[i];
                char c1[16], c2[16];
                fmt_rate(sp->minflt, c1, sizeof c1);
                fmt_rate(sp->majflt, c2, sizeof c2);
                P_print(y, 0, i == 0 ? AT_BOLD : 0,
                        "%7d %-16.16s %11s %11s %10ld",
                        sp->pid, sp->comm, c1, c2, sp->rss_kb / 1024);
            }
            if (s->nsusp == 0 && y < H - 1)
                P_print(y++, 2, AT_DIM, "(no page-fault activity this interval)");
        }
    } else if (y < H - 2) {
        char hdr[96];
        snprintf(hdr, sizeof hdr, "WHO IS FLUSHING  (sort: %s%s)",
                 sort_name[st.sort], st.cumulative ? ", cumulative" : "");
        section(y++, hdr);
    }
    if (s->trace_ok && y < H - 1) {
        char hb[G_MAXC + 1];
        snprintf(hb, sizeof hb, "%7s %-16s %8s %9s %8s %8s %8s %5s  %-40s",
                 "PID", "COMM", st.cumulative ? "SEND" : "SEND/s",
                 st.cumulative ? "PAGES" : "PAGES/s",
                 "LOCAL", "RECV", "SWTCH", "%FL", "WHY (kernel origin)");
        for (int i = 0; i < W; i++)
            P_putc(y, i, CP_TABHDR, i < (int)strlen(hb) ? hb[i] : ' ');
        y++;

        qsort(s->pids, s->npid, sizeof s->pids[0], row_cmp);

        /* hide the noise: only tasks with real flush activity, unless 'z' */
        static struct pid_stat *fr[512];
        int nf = 0, hidden = 0;
        uint64_t grand = 0;
        for (int i = 0; i < s->npid; i++) {
            uint64_t act = flush_activity(&s->pids[i], st.cumulative);
            grand += act;
            if (act > 0 || st.show_all) fr[nf++] = &s->pids[i];
            else hidden++;
        }
        if (!grand) grand = 1;

        int rows_avail = H - 2 - y;
        int max_scroll = nf > rows_avail ? nf - rows_avail : 0;
        if (st.scroll > max_scroll) st.scroll = max_scroll;

        for (int i = st.scroll; i < nf && y < H - 2; i++, y++) {
            struct pid_stat *p = fr[i];
            char c1[16], c2[16], c3[16], c5[16], c6[16];
            if (st.cumulative) {
                fmt_count(p->cum[R_REMOTE_SEND], c1, sizeof c1);
                fmt_count(p->pages_cum, c2, sizeof c2);
                fmt_count(p->cum[R_LOCAL] + p->cum[R_LOCAL_MM], c3, sizeof c3);
                fmt_count(p->cum[R_REMOTE_RECV], c6, sizeof c6);
                fmt_count(p->cum[R_TASK_SWITCH], c5, sizeof c5);
            } else {
                fmt_rate(p->cnt[R_REMOTE_SEND] / s->dt, c1, sizeof c1);
                fmt_rate(p->pages / s->dt, c2, sizeof c2);
                fmt_rate((p->cnt[R_LOCAL] + p->cnt[R_LOCAL_MM]) / s->dt, c3, sizeof c3);
                fmt_rate(p->cnt[R_REMOTE_RECV] / s->dt, c6, sizeof c6);
                fmt_rate(p->cnt[R_TASK_SWITCH] / s->dt, c5, sizeof c5);
            }
            uint64_t act = flush_activity(p, st.cumulative);
            /* dominant origin, with its plain-English meaning inline */
            uint32_t best = 0;
            uint64_t bc = 0;
            for (int k = 0; k < 4; k++)
                if (p->origin_cnt[k] > bc) { bc = p->origin_cnt[k]; best = p->origin_sym[k]; }
            char why[96] = "-";
            if (act > 0 && best) {
                const char *sym = trace_symname(s->ts, best);
                const char *hint = origin_hint(sym);
                if (hint) snprintf(why, sizeof why, "%s = %s", sym, hint);
                else snprintf(why, sizeof why, "%s", sym);
            }
            char comm[COMM_LEN + 3];
            snprintf(comm, sizeof comm, p->kthread ? "[%s]" : "%s", p->comm);
            bool hot = (st.cumulative ? p->cum[R_REMOTE_SEND] : p->cnt[R_REMOTE_SEND]) > 0;
            unsigned at = hot ? AT_BOLD : (act == 0 ? AT_DIM : 0);
            P_print(y, 0, at, "%7d %-16.16s %8s %9s %8s %8s %8s %4.0f%%  %-40.40s",
                    p->pid, comm, c1, c2, c3, c6, c5,
                    100.0 * (double)act / (double)grand, why);
        }
        if (hidden > 0 && y < H - 1)
            P_print(y++, 0, AT_DIM,
                    "  (+%d tasks with no flush activity hidden - press z to show)",
                    hidden);
        else if (nf == 0 && y < H - 1)
            P_print(y++, 2, AT_DIM, "(no flushing tasks this interval)");
    }

    /* ---- key bar ---- */
    {
        static const struct { const char *k, *lab; } keys[] = {
            { "F1", "Help" }, { "F5", "Sort" }, { "c", "Cumul" }, { "z", "All" },
            { "p", "Pause" }, { "m", "Grid" }, { "r", "Reset" }, { "q", "Quit" },
        };
        int x = 0;
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
            x = P_print(H - 1, x, AT_BOLD, "%s", keys[i].k);
            x = P_print(H - 1, x, CP_FKEY, "%-6s", keys[i].lab);
        }
        char up[40];
        snprintf(up, sizeof up, "%s up %d:%02d:%02d", ansi ? "[ansi]" : "",
                 (int)s->uptime / 3600, ((int)s->uptime / 60) % 60, (int)s->uptime % 60);
        P_print(H - 1, W - (int)strlen(up) - 1, AT_DIM, "%s", up);
    }

    if (st.help) draw_help();
    P_frame_end();
}

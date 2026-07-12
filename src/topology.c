#include "tlba.h"
#include <unistd.h>
#include <ctype.h>

static long read_long(const char *path, long dflt)
{
    FILE *f = fopen(path, "r");
    if (!f) return dflt;
    long v = dflt;
    if (fscanf(f, "%ld", &v) != 1) v = dflt;
    fclose(f);
    return v;
}

static int read_str(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, len, f)) { fclose(f); return -1; }
    fclose(f);
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}

/* count CPUs in a cpulist like "0-3,8-11" */
static int cpulist_count(const char *s)
{
    int n = 0;
    while (*s) {
        long a = strtol(s, (char **)&s, 10);
        if (*s == '-') {
            long b = strtol(s + 1, (char **)&s, 10);
            n += (int)(b - a + 1);
        } else {
            n += 1;
        }
        if (*s == ',') s++;
        else break;
    }
    return n;
}

static void read_cache(int cpu, int index, struct cache_desc *out, bool *found)
{
    char p[256], buf[256];
    snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cache/index%d/level", cpu, index);
    long level = read_long(p, -1);
    if (level < 0) { *found = false; return; }
    out->level = (int)level;

    snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cache/index%d/type", cpu, index);
    if (read_str(p, buf, sizeof buf) == 0)
        out->type = (buf[0] == 'D') ? 'D' : (buf[0] == 'I') ? 'I' : 'U';

    snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cache/index%d/size", cpu, index);
    if (read_str(p, buf, sizeof buf) == 0) {
        long sz = atol(buf);
        char unit = buf[strlen(buf) ? strlen(buf) - 1 : 0];
        out->size_kb = (unit == 'M') ? sz * 1024 : sz; /* "48K" / "3072K" / "24M" */
    }
    snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cache/index%d/shared_cpu_list", cpu, index);
    if (read_str(p, buf, sizeof buf) == 0)
        out->sharing = cpulist_count(buf);

    snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cache/index%d/coherency_line_size", cpu, index);
    out->line_size = (int)read_long(p, 64);
    snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cache/index%d/ways_of_associativity", cpu, index);
    out->ways = (int)read_long(p, 0);
    *found = true;
}

int topology_read(struct topology *t)
{
    memset(t, 0, sizeof *t);
    t->ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (t->ncpu > MAX_CPUS) t->ncpu = MAX_CPUS;

    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "vendor_id", 9)) {
                if (strstr(line, "GenuineIntel")) t->vendor = VENDOR_INTEL;
                else if (strstr(line, "AuthenticAMD")) t->vendor = VENDOR_AMD;
            } else if (!strncmp(line, "cpu family", 10)) {
                sscanf(line, "cpu family : %d", &t->family);
            } else if (!strncmp(line, "model name", 10)) {
                char *c = strchr(line, ':');
                if (c && !t->model_name[0]) {
                    c += 2;
                    c[strcspn(c, "\n")] = 0;
                    snprintf(t->model_name, sizeof t->model_name, "%s", c);
                }
            } else if (!strncmp(line, "model", 5) && isspace((unsigned char)line[5])) {
                sscanf(line, "model : %d", &t->model);
            }
        }
        fclose(f);
    }

    int max_pkg = 0, max_node = 0;
    /* core uniqueness via (pkg, core_id) - count distinct pairs cheaply */
    static bool seen_core[MAX_CPUS];
    memset(seen_core, 0, sizeof seen_core);
    t->ncore = 0;

    for (int i = 0; i < t->ncpu; i++) {
        char p[256];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", i);
        t->pkg_of[i] = (int)read_long(p, 0);
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/topology/core_id", i);
        t->core_of[i] = (int)read_long(p, i);
        if (t->pkg_of[i] > max_pkg) max_pkg = t->pkg_of[i];

        /* NUMA node */
        t->node_of[i] = 0;
        for (int n = 0; n < 64; n++) {
            snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/node%d", i, n);
            if (access(p, F_OK) == 0) { t->node_of[i] = n; break; }
        }
        if (t->node_of[i] > max_node) max_node = t->node_of[i];

        int key = (t->pkg_of[i] * 512 + t->core_of[i]) % MAX_CPUS;
        if (!seen_core[key]) { seen_core[key] = true; t->ncore++; }
    }
    t->nsocket = max_pkg + 1;
    t->nnode = max_node + 1;

    for (int idx = 0; idx < 8; idx++) {
        struct cache_desc d = {0};
        bool found = false;
        read_cache(0, idx, &d, &found);
        if (!found) break;
        if (d.level == 1 && d.type == 'D') t->l1d = d;
        else if (d.level == 1 && d.type == 'I') t->l1i = d;
        else if (d.level == 2) t->l2 = d;
        else if (d.level == 3) { t->l3 = d; t->has_l3 = true; }
    }
    return 0;
}

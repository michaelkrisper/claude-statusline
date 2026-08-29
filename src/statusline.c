// Claude Code status line, C port of src/main.rs.
//
// Fields: project dir, GPU/VRAM (cached), context window, 5 h rate limit with
// depletion forecast. Model name and clock are deliberately absent -- without the
// clock no terminal-width probing and no display-width accounting is needed.
//
// Optimized for per-invocation cost, in this order of impact: parsing the sample
// log (up to 4320 rows), syscall count, process startup. Build static against
// musl (see Makefile) -- that removes the dynamic loader from every invocation.

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LOOKBACK 2700          // burn-rate estimation window (45 min)
#define HALF_LIFE 900.0        // recent samples weigh more (15 min)
#define MIN_SPAN 300           // below this, fall back to window average
#define SAMPLE_GAP 10
#define W_PRIOR 1200.0         // prior counts like 20 min of live evidence
#define HARVEST_MIN_SPAN 900   // closed window must span this to yield a rate
#define HARVEST_MIN_PCT 2.0
#define RATES_KEEP 20
// samples worth keeping: the regression lookback, plus the window anchor and slack
#define MAX_LIVE (LOOKBACK / SAMPLE_GAP + 2)
#define GPU_REFRESH 10         // nvidia-smi is re-queried at most this often
#define URGENT_SECS 900        // a 5h depletion ETA closer than this is flagged
#define URGENT "\x1b[91m"      // bright-red signal color on the ETA clock time
#define PATH_COLOR "\x1b[38;5;110m"  // muted steel blue, quiet against the metrics
#define FG_RESET "\x1b[39m"    // reset foreground only, preserving surrounding bold

static char state[4096];  // cache directory path, see state_dir()

static int make_state_dir(void);

// ---------------------------------------------------------------- utilities

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) _exit(1);
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) {
        free(p);
        _exit(1);
    }
    return q;
}

// whole fd as a NUL-terminated heap buffer. `regular` marks a regular file, where
// a short read already means EOF -- that saves the extra read(2) each state file
// would otherwise cost.
static char *read_fd(int fd, size_t *len_out, int regular) {
    size_t cap = 1 << 13, len = 0;
    char *buf = xmalloc(cap);
    for (;;) {
        if (len + 1 >= cap) buf = xrealloc(buf, cap *= 2);
        size_t want = cap - len - 1;
        ssize_t n = read(fd, buf + len, want);
        if (n <= 0) break;
        len += (size_t)n;
        if (regular && (size_t)n < want) break;
    }
    buf[len] = '\0';
    if (len_out) *len_out = len;
    return buf;
}

// whole file as a NUL-terminated heap buffer, NULL if unreadable. fstat sizes the
// buffer exactly: one read, and an allocation small enough that musl serves it from
// the heap instead of a fresh mmap/munmap pair.
static char *read_file(const char *path, size_t *len_out) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        char *b = read_fd(fd, len_out, 1);
        close(fd);
        return b;
    }
    size_t n = (size_t)st.st_size;
    char *buf = xmalloc(n + 1);
    ssize_t got = n ? read(fd, buf, n) : 0;
    close(fd);
    if (got < 0) got = 0;
    buf[got] = '\0';
    if (len_out) *len_out = (size_t)got;
    return buf;
}

// first `max` bytes of a file. /proc entries report st_size 0, so read_file cannot
// size them -- and the fields wanted from them all sit in the first lines anyway.
static char *read_head(const char *path, size_t max) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    char *buf = xmalloc(max + 1);
    ssize_t n = read(fd, buf, max);
    close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return buf;
}

// the state directory is created lazily, so the common case -- it already exists
// -- costs no syscall at all
static int open_state_write(const char *path, int append) {
    int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC);
    int fd = open(path, flags, 0644);
    if (fd < 0 && errno == ENOENT && make_state_dir()) fd = open(path, flags, 0644);
    return fd;
}

static int write_file(const char *path, const char *data, size_t len, int append) {
    int fd = open_state_write(path, append);
    if (fd < 0) return -1;
    while (len) {
        ssize_t n = write(fd, data, len);
        if (n <= 0) break;
        data += n;
        len -= (size_t)n;
    }
    close(fd);
    return 0;
}

// shortest decimal that reads back identical, so files stay interchangeable with
// the Rust implementation's float formatting
static void fmt_f64(char *dst, size_t n, double v) {
    snprintf(dst, n, "%.15g", v);
    if (strtod(dst, NULL) != v) snprintf(dst, n, "%.17g", v);
}

// ---------------------------------------------------------------- number scanning

static const double POW10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                               1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                               1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

// leading integer, advancing *p past it; 0 if there is none
static int scan_ll(const char **p, long long *out) {
    const char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    int neg = *s == '-';
    if (*s == '-' || *s == '+') s++;
    if (!isdigit((unsigned char)*s)) return 0;
    unsigned long long v = 0;
    while (isdigit((unsigned char)*s)) v = v * 10 + (unsigned)(*s++ - '0');
    *out = neg ? -(long long)v : (long long)v;
    *p = s;
    return 1;
}

// leading double, advancing *p past it; 0 if there is none.
//
// strtod is a correctly-rounded general parser and costs ~240 ns a call, which at
// 4320 sample rows x 2 floats is milliseconds. The sample and rate files only ever
// hold short decimals, where mantissa/10^frac is exact in binary64 (both operands
// are representable, so IEEE division rounds correctly) -- Clinger's fast path,
// bit-identical to strtod and ~20x cheaper. Anything longer falls back to strtod.
static int scan_f64(const char **p, double *out) {
    const char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    const char *start = s;
    int neg = *s == '-';
    if (*s == '-' || *s == '+') s++;
    unsigned long long m = 0;
    int digits = 0, frac = 0;
    while (isdigit((unsigned char)*s)) {
        m = m * 10 + (unsigned)(*s++ - '0');
        digits++;
    }
    if (*s == '.') {
        s++;
        while (isdigit((unsigned char)*s)) {
            m = m * 10 + (unsigned)(*s++ - '0');
            digits++;
            frac++;
        }
    }
    if (digits && digits < 19 && frac < (int)(sizeof POW10 / sizeof *POW10) && *s != 'e' &&
        *s != 'E') {
        double v = (double)m / POW10[frac];
        *out = neg ? -v : v;
        *p = s;
        return 1;
    }
    char *end;
    double v = strtod(start, &end);
    if (end == start) return 0;
    *out = v;
    *p = end;
    return 1;
}

// ---------------------------------------------------------------- output line

typedef struct {
    char *buf;
    size_t len, cap;
} Out;

static void out_add(Out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void out_add(Out *o, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if ((size_t)n < o->cap - o->len) {
            o->len += (size_t)n;
            return;
        }
        o->buf = xrealloc(o->buf, o->cap = o->len + (size_t)n + 64);
    }
}

static Out out_new(size_t cap) {
    Out o = {xmalloc(cap), 0, cap};
    o.buf[0] = '\0';
    return o;
}

// an empty part contributes nothing -- not even its separator, so a block whose
// fields are all unavailable disappears together with the ` | ` in front of it
static void join(Out *o, const char *sep, const char *part) {
    if (!part || !*part) return;
    out_add(o, "%s%s", o->len ? sep : "", part);
}

static void push_field(Out *o, const char *field) { join(o, " ", field); }
static void push_block(Out *o, const char *block) { join(o, " | ", block); }

// ---------------------------------------------------------------- json

static const char *js_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

// p points at the opening quote; returns just past the closing quote
static const char *js_str_end(const char *p) {
    for (p++; *p; p++) {
        if (*p == '\\' && p[1]) p++;
        else if (*p == '"') return p + 1;
    }
    return p;
}

static const char *js_skip(const char *p) {
    p = js_ws(p);
    if (*p == '"') return js_str_end(p);
    if (*p == '{' || *p == '[') {
        char open = *p, close = open == '{' ? '}' : ']';
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = js_str_end(p);
                continue;
            }
            if (*p == open) depth++;
            else if (*p == close && --depth == 0) return p + 1;
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

// value of `key` in the object at `p`, NULL if absent
static const char *js_get(const char *p, const char *key) {
    if (!p) return NULL;
    p = js_ws(p);
    if (*p != '{') return NULL;
    size_t klen = strlen(key);
    for (p++;;) {
        p = js_ws(p);
        if (*p != '"') return NULL;
        const char *ks = p + 1, *ke = js_str_end(p) - 1;
        p = js_ws(js_str_end(p));
        if (*p != ':') return NULL;
        const char *val = js_ws(p + 1);
        if ((size_t)(ke - ks) == klen && !memcmp(ks, key, klen)) return val;
        p = js_ws(js_skip(val));
        if (*p != ',') return NULL;
        p++;
    }
}

static int js_num(const char *v, double *out) {
    if (!v || (*v != '-' && !isdigit((unsigned char)*v))) return 0;
    return scan_f64(&v, out);
}

// decoded string value into a fresh buffer, NULL unless `v` is a non-empty string
static char *js_str(const char *v) {
    if (!v || *v != '"') return NULL;
    const char *end = js_str_end(v) - 1;
    char *s = xmalloc((size_t)(end - v) + 1), *d = s;
    for (const char *p = v + 1; p < end;) {
        if (*p != '\\') {
            *d++ = *p++;
            continue;
        }
        p++;
        switch (*p) {
            case 'n': *d++ = '\n'; p++; break;
            case 't': *d++ = '\t'; p++; break;
            case 'r': *d++ = '\r'; p++; break;
            case 'b': *d++ = '\b'; p++; break;
            case 'f': *d++ = '\f'; p++; break;
            case 'u': {
                unsigned cp = 0;
                for (int i = 1; i <= 4 && p + i < end; i++)
                    cp = cp * 16 + (unsigned)(isdigit((unsigned char)p[i])
                                                  ? p[i] - '0'
                                                  : (tolower((unsigned char)p[i]) - 'a' + 10));
                p += 5;
                if (cp < 0x80) {
                    *d++ = (char)cp;
                } else if (cp < 0x800) {
                    *d++ = (char)(0xC0 | cp >> 6);
                    *d++ = (char)(0x80 | (cp & 0x3F));
                } else {
                    *d++ = (char)(0xE0 | cp >> 12);
                    *d++ = (char)(0x80 | (cp >> 6 & 0x3F));
                    *d++ = (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: *d++ = *p++;
        }
    }
    *d = '\0';
    if (!*s) {
        free(s);
        return NULL;
    }
    return s;
}

// ---------------------------------------------------------------- time

static int fmt_ts(char *dst, size_t n, long long ts, const char *fmt) {
    time_t t = (time_t)ts;
    struct tm tm;
    if (!localtime_r(&t, &tm)) return 0;
    return strftime(dst, n, fmt, &tm) > 0;
}

static int same_day(long long a, long long b) {
    time_t ta = (time_t)a, tb = (time_t)b;
    struct tm x, y;
    if (!localtime_r(&ta, &x) || !localtime_r(&tb, &y)) return 0;
    return x.tm_year == y.tm_year && x.tm_yday == y.tm_yday;
}

// " (~ETA / RESET)" if depletion lands before the reset, " (+Nh / RESET)" with the
// overshoot past the reset otherwise, " (RESET)" if no rate is known. When the ETA is
// less than URGENT_SECS away, the clock time is painted in the urgent signal color.
static void push_times(Out *o, int have_eta, long long eta, long long reset, long long now) {
    char e[64] = "", r[32] = "";
    if (have_eta) {
        if (eta >= reset) {
            long long h = (eta - reset + 3599) / 3600;
            snprintf(e, sizeof e, "+%lldh", h < 9999 ? h : 9999);
        } else {
            char hm[32];
            if (fmt_ts(hm, sizeof hm, eta, same_day(eta, now) ? "%H:%M" : "%a %H:%M"))
                snprintf(e, sizeof e, eta - now < URGENT_SECS ? "~" URGENT "%s" FG_RESET : "~%s",
                         hm);
        }
    }
    int has_r = fmt_ts(r, sizeof r, reset, "%H:%M");
    if (*e && has_r) out_add(o, " (%s / %s)", e, r);
    else if (has_r) out_add(o, " (%s)", r);
    else if (*e) out_add(o, " (%s)", e);
}

// ---------------------------------------------------------------- state files

// cache dir for the persisted samples, rates and CPU/GPU snapshots. Resolves the
// path only -- creation is left to make_state_dir().
static int state_dir(void) {
    const char *base = getenv("XDG_CACHE_HOME");
    if (base && *base) {
        snprintf(state, sizeof state, "%s/claude-statusline", base);
        return 1;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) home = getenv("USERPROFILE");
    if (!home || !*home) return 0;
    snprintf(state, sizeof state, "%s/.cache/claude-statusline", home);
    return 1;
}

// Reached only when a state file cannot be opened, i.e. once per fresh install.
// Adopts the directory the Rust implementation used if it is still around, so an
// existing forecast history carries over instead of starting from zero.
static int make_state_dir(void) {
    char *slash = strrchr(state, '/');
    if (!slash) return 0;
    *slash = '\0';
    mkdir(state, 0755);  // the ~/.cache level may be missing on a fresh account
    char legacy[sizeof state + 16];
    int n = snprintf(legacy, sizeof legacy, "%s/statusline-rs", state);
    *slash = '/';
    if (n > 0 && (size_t)n < sizeof legacy && rename(legacy, state) == 0) return 1;
    return mkdir(state, 0755) == 0 || errno == EEXIST;
}

static const char *spath(const char *name) {
    static char buf[4160];
    snprintf(buf, sizeof buf, "%s/%s", state, name);
    return buf;
}

// ---------------------------------------------------------------- gpu

// first GPU of an nvidia-smi csv line "util, mem.used, mem.total" (MiB) into
// (gpu %, vram %)
static int parse_gpu_csv(const char *data, long long *gpu, long long *vram) {
    double used, total;
    long long util;
    if (!scan_ll(&data, &util) || *data != ',') return 0;
    data++;
    if (!scan_f64(&data, &used) || *data != ',') return 0;
    data++;
    if (!scan_f64(&data, &total) || !(total > 0.0)) return 0;
    *gpu = util;
    *vram = (long long)llround(100.0 * used / total);
    return 1;
}

// cached (gpu %, vram %). nvidia-smi is slow (hundreds of ms on WSL2), so it is
// never awaited: at most every GPU_REFRESH seconds a detached child re-queries it
// into gpu.csv.tmp; a later invocation adopts the finished file atomically. The
// empty tmp file doubles as the in-flight marker against spawn stampedes.
//
// Two stat(2)s carry both the size and the mtime, and gpu.csv is opened only when
// the stat says it is there -- on a machine without a GPU that is the whole cost.
static int gpu_stats(long long now, long long *gpu, long long *vram) {
    char path[4160], tmp[4160];
    snprintf(path, sizeof path, "%s/gpu.csv", state);
    snprintf(tmp, sizeof tmp, "%s/gpu.csv.tmp", state);

    struct stat st_tmp, st_path;
    int has_tmp = stat(tmp, &st_tmp) == 0;
    int has_path = stat(path, &st_path) == 0;
    if (has_tmp && st_tmp.st_size > 0 && rename(tmp, path) == 0) {
        st_path = st_tmp;
        has_path = 1;
        has_tmp = 0;
    }
    int fresh = (has_path && now - (long long)st_path.st_mtime < GPU_REFRESH) ||
                (has_tmp && now - (long long)st_tmp.st_mtime < GPU_REFRESH);
    if (!fresh) {
        int fd = open_state_write(tmp, 0);
        if (fd >= 0) {
            pid_t pid = fork();
            if (pid == 0) {
                // stdout must not stay on our pipe: the caller reads it to EOF
                int null = open("/dev/null", O_RDWR);
                dup2(null, 0);
                dup2(fd, 1);
                dup2(null, 2);
                setsid();
                execlp("nvidia-smi", "nvidia-smi",
                       "--query-gpu=utilization.gpu,memory.used,memory.total",
                       "--format=csv,noheader,nounits", (char *)NULL);
                _exit(127);
            }
            close(fd);
        }
    }
    if (!has_path) return 0;
    char *data = read_file(path, NULL);
    if (!data) return 0;
    int r = parse_gpu_csv(data, gpu, vram);
    free(data);
    return r;
}

// ---------------------------------------------------------------- samples

typedef struct {
    long long ts;
    double five;
    long long five_reset;
    double seven;
    long long seven_reset;
} Sample;

typedef struct {
    long long reset;
    double rate;
} Rate;

// advance past the rest of the current line
static const char *next_line(const char *p) {
    while (*p && *p != '\n') p++;
    return *p ? p + 1 : p;
}

// trailing whitespace then end of line -- rejects rows with extra columns, the way
// the Rust implementation's field-count check does
static int line_done(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    return *p == '\n' || *p == '\0';
}

// `clean` reports whether every line parsed; a malformed one triggers a full
// rewrite of the log instead of the usual append
static Sample *load_samples(size_t *n_out, int *clean) {
    *n_out = 0;
    if (clean) *clean = 1;
    char *data = read_file(spath("samples.tsv"), NULL);
    if (!data) return NULL;
    size_t cap = 512, n = 0;
    Sample *v = xmalloc(cap * sizeof *v);
    for (const char *p = data; *p; p = next_line(p)) {
        const char *q = p;
        Sample s;
        if (scan_ll(&q, &s.ts) && scan_f64(&q, &s.five) && scan_ll(&q, &s.five_reset) &&
            scan_f64(&q, &s.seven) && scan_ll(&q, &s.seven_reset) && line_done(q)) {
            if (n == cap) v = xrealloc(v, (cap *= 2) * sizeof *v);
            v[n++] = s;
        } else if (clean) {
            *clean = 0;
        }
    }
    free(data);
    *n_out = n;
    return v;
}

static Rate *load_rates(size_t *n_out) {
    *n_out = 0;
    char *data = read_file(spath("rates.tsv"), NULL);
    if (!data) return NULL;
    size_t cap = 32, n = 0;
    Rate *v = xmalloc(cap * sizeof *v);
    for (const char *p = data; *p; p = next_line(p)) {
        const char *q = p;
        Rate r;
        if (scan_ll(&q, &r.reset) && scan_f64(&q, &r.rate) && line_done(q)) {
            if (n == cap) v = xrealloc(v, (cap *= 2) * sizeof *v);
            v[n++] = r;
        }
    }
    free(data);
    *n_out = n;
    return v;
}

// summarize closed 5h windows still present in the sample log into per-window
// burn rates (keyed by reset ts); returns whether the rate list changed
static int harvest_rates(const Sample *s, size_t ns, long long cur_reset, Rate **rates,
                         size_t *nr, size_t *cap) {
    int changed = 0;
    for (size_t i = 0; i < ns;) {
        long long r = s[i].five_reset;
        size_t j = i;
        while (j < ns && s[j].five_reset == r) j++;
        int known = 0;
        for (size_t k = 0; k < *nr; k++)
            if ((*rates)[k].reset == r) known = 1;
        if (r != cur_reset && !known) {
            const Sample *a = &s[i], *b = &s[j - 1];
            if (b->ts - a->ts >= HARVEST_MIN_SPAN && b->five - a->five >= HARVEST_MIN_PCT) {
                if (*nr == *cap)
                    *rates = xrealloc(*rates, (*cap = *cap ? *cap * 2 : 32) * sizeof **rates);
                (*rates)[(*nr)++] = (Rate){r, (b->five - a->five) / (double)(b->ts - a->ts)};
                changed = 1;
            }
        }
        i = j;
    }
    if (*nr > RATES_KEEP) {
        size_t cut = *nr - RATES_KEEP;
        memmove(*rates, *rates + cut, RATES_KEEP * sizeof **rates);
        *nr = RATES_KEEP;
        changed = 1;
    }
    return changed;
}

// Two kinds of sample still carry information: those inside the regression lookback
// of the current window, and the very first sample of the current window, which
// anchors the full-window burn rate harvested once that window closes. Everything
// else is dead weight -- closed windows have already been summarized into
// rates.tsv by then, and the middle of the current window is outside the lookback.
static size_t compact(Sample *s, size_t n, long long now, long long cur_reset) {
    size_t keep = 0;
    int anchored = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i].five_reset != cur_reset) continue;
        if (!anchored || s[i].ts >= now - LOOKBACK) s[keep++] = s[i];
        anchored = 1;
    }
    return keep;
}

// upper median, matching the Rust v[len/2] on the sorted slice. n is bounded by
// RATES_KEEP, where an inline insertion sort beats qsort's indirect comparator
// calls by ~25x and keeps qsort out of the binary entirely.
static int median(double *v, size_t n, double *out) {
    if (!n) return 0;
    for (size_t i = 1; i < n; i++) {
        double k = v[i];
        size_t j = i;
        while (j && v[j - 1] > k) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = k;
    }
    *out = v[n / 2];
    return 1;
}

// ---------------------------------------------------------------- host metrics

// cumulative (total, idle) jiffies since boot from /proc/stat's aggregate cpu line
static int parse_cpu_totals(const char *txt, unsigned long long *total, unsigned long long *idle) {
    if (strncmp(txt, "cpu", 3) || (txt[3] != ' ' && txt[3] != '\t')) return 0;
    const char *p = txt + 3;
    unsigned long long v[16];
    size_t n = 0;
    long long x;
    while (n < sizeof v / sizeof *v && scan_ll(&p, &x)) v[n++] = (unsigned long long)x;
    if (n < 4) return 0;
    unsigned long long sum = 0;
    for (size_t i = 0; i < n; i++) sum += v[i];
    *total = sum;
    *idle = v[3] + (n > 4 ? v[4] : 0);  // idle + iowait both count as not-working
    return 1;
}

// CPU usage over the interval since the previous invocation, via (total, idle)
// jiffies persisted in the state dir. Absent on the very first run (no baseline
// yet) and on platforms without /proc.
static int cpu_pct(long long *out) {
    char *txt = read_head("/proc/stat", 512);
    if (!txt) return 0;
    unsigned long long total, idle;
    int ok = parse_cpu_totals(txt, &total, &idle);
    free(txt);
    if (!ok) return 0;

    const char *path = spath("cpu.tsv");
    unsigned long long pt = 0, pi = 0;
    int have_prev = 0;
    char *prev = read_file(path, NULL);
    if (prev) {
        const char *q = prev;
        long long a, b;
        if (scan_ll(&q, &a) && scan_ll(&q, &b)) {
            pt = (unsigned long long)a;
            pi = (unsigned long long)b;
            have_prev = 1;
        }
        free(prev);
    }
    char line[64];
    int n = snprintf(line, sizeof line, "%llu %llu\n", total, idle);
    if (n > 0) write_file(path, line, (size_t)n, 0);

    if (!have_prev || total <= pt) return 0;
    unsigned long long dt = total - pt, di = idle > pi ? idle - pi : 0;
    if (di > dt) di = dt;
    *out = (long long)llround(100.0 * (1.0 - (double)di / (double)dt));
    return 1;
}

// used-RAM percentage from /proc/meminfo (MemAvailable vs MemTotal)
static int meminfo_pct(const char *data, long long *out) {
    double total = 0, avail = -1;
    for (const char *p = data; *p; p = next_line(p)) {
        const char *q;
        if (!strncmp(p, "MemTotal:", 9)) {
            q = p + 9;
            scan_f64(&q, &total);
        } else if (!strncmp(p, "MemAvailable:", 13)) {
            q = p + 13;
            scan_f64(&q, &avail);
        }
    }
    if (!(total > 0.0) || avail < 0.0) return 0;
    *out = (long long)llround(100.0 * (1.0 - avail / total));
    return 1;
}

// free bytes available to unprivileged users on the filesystem holding `path`
static int disk_free(const char *path, unsigned long long *out) {
    struct statvfs st;
    if (statvfs(path, &st) != 0) return 0;
    *out = (unsigned long long)st.f_bavail * (unsigned long long)st.f_frsize;
    return 1;
}

static void fmt_bytes(char *dst, size_t n, unsigned long long b) {
    double g = (double)b / (double)(1ULL << 30);
    if (g >= 1024.0) snprintf(dst, n, "%.1fT", g / 1024.0);
    else if (g >= 10.0) snprintf(dst, n, "%lluG", (unsigned long long)llround(g));
    else if (g >= 1.0) snprintf(dst, n, "%.1fG", g);
    else snprintf(dst, n, "%lluM", (unsigned long long)llround((double)b / (double)(1ULL << 20)));
}

// active Claude account. A single-pass substring scan rather than a full parse of
// the ~30 KB file: locate the oauthAccount object, then the first emailAddress
// string inside it.
static char *parse_email(const char *data) {
    const char *o = strstr(data, "\"oauthAccount\"");
    if (!o) return NULL;
    const char *e = strstr(o, "\"emailAddress\"");
    if (!e) return NULL;
    e = js_ws(e + 14);
    if (*e != ':') return NULL;
    e = js_ws(e + 1);
    if (*e != '"') return NULL;
    const char *end = strchr(++e, '"');
    if (!end) return NULL;
    size_t n = (size_t)(end - e);
    char *s = xmalloc(n + 1);
    memcpy(s, e, n);
    s[n] = '\0';
    return s;
}

// read live on every invocation so the line reflects the current login immediately
// after an account switch, with no caching
static char *account_email(void) {
    const char *home = getenv("HOME");
    if (!home || !*home) home = getenv("USERPROFILE");
    if (!home || !*home) return NULL;
    char path[4160];
    snprintf(path, sizeof path, "%s/.claude.json", home);
    char *data = read_file(path, NULL);
    if (!data) return NULL;
    char *mail = parse_email(data);
    free(data);
    return mail;
}

// ---------------------------------------------------------------- gauges

// Quarter-filled circle in a green/yellow/red tint, so a load percentage reads at a
// glance before any digits are parsed -- the value is encoded twice. Foreground-only
// reset keeps surrounding bold intact.
static void gauge_at(char *dst, size_t n, long long pct, long long yellow, long long red) {
    // 0-12% 13-37% 38-62% 63-87% 88-100%
    static const char *const GLYPH[5] = {"\xE2\x97\x8B", "\xE2\x97\x94", "\xE2\x97\x91",
                                         "\xE2\x97\x95", "\xE2\x97\x8F"};
    long long p = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    const char *color = p >= red ? "\x1b[31m" : p >= yellow ? "\x1b[33m" : "\x1b[32m";
    snprintf(dst, n, "%s%s" FG_RESET, color, GLYPH[(size_t)llround((double)p / 25.0)]);
}

static void gauge(char *dst, size_t n, long long pct) { gauge_at(dst, n, pct, 60, 85); }

// CTX turns yellow earlier and red earlier than the other gauges, since a full context
// window is more disruptive than a full CPU.
static void ctx_gauge(char *dst, size_t n, long long pct) { gauge_at(dst, n, pct, 25, 40); }

static void fmt_tokens(char *dst, size_t n, long long t) {
    if (t >= 1000) snprintf(dst, n, "%lldk", (long long)llround((double)t / 1000.0));
    else snprintf(dst, n, "%lld", t);
}

// "~/src" for a path under HOME. A degenerate HOME ("" or "/") must not swallow every
// path, and a sibling directory must not match on the prefix alone.
static void tilde(char *dst, size_t n, const char *cwd, const char *home) {
    size_t h = home ? strlen(home) : 0;
    while (h > 1 && home[h - 1] == '/') h--;
    if (h && !(h == 1 && home[0] == '/') && !strncmp(cwd, home, h) &&
        (cwd[h] == '\0' || cwd[h] == '/')) {
        snprintf(dst, n, "~%s", cwd + h);
        return;
    }
    snprintf(dst, n, "%s", cwd);
}

// ---------------------------------------------------------------- forecast

// exponentially weighted least-squares slope in %/sec
static int wslope(const long long *t, const double *y, size_t n, double *out) {
    if (n < 3) return 0;
    long long tl = t[n - 1];
    double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; i++) {
        double x = (double)(t[i] - tl);  // <= 0
        double w = exp(x / HALF_LIFE * M_LN2);
        sw += w;
        sx += w * x;
        sy += w * y[i];
        sxx += w * x * x;
        sxy += w * x * y[i];
    }
    double d = sw * sxx - sx * sx;
    if (!(fabs(d) > DBL_EPSILON)) return 0;
    *out = (sw * sxy - sx * sy) / d;
    return 1;
}

// projected time (unix secs) when the 5 h limit hits 100%. Rate is a shrinkage
// blend: live evidence (weighted regression, or the window average early on)
// weighted by its observed span, pulled toward the historical per-window prior
// weighted at W_PRIOR -- the prior dominates early, live data as span grows.
static int eta_5h(const Sample *s, size_t ns, long long now, double cur, long long reset,
                  long long window, int have_prior, double prior, long long *out) {
    long long *ts = xmalloc((ns + 1) * sizeof *ts);
    double *ys = xmalloc((ns + 1) * sizeof *ys);
    size_t n = 0;
    for (size_t i = 0; i < ns; i++)
        if (s[i].five_reset == reset && s[i].five >= 0.0 && s[i].ts >= now - LOOKBACK) {
            ts[n] = s[i].ts;
            ys[n++] = s[i].five;
        }
    if (!n || ts[n - 1] != now) {
        ts[n] = now;
        ys[n++] = cur;
    }
    long long span = ts[n - 1] - ts[0];

    double num = 0, den = 0, r;
    if (span >= MIN_SPAN) {
        if (wslope(ts, ys, n, &r)) {
            num += (double)span * (r > 0.0 ? r : 0.0);
            den += (double)span;
        }
    } else {
        // window starts at 0% on first use, so the window average is a fair early guess
        long long elapsed = now - (reset - window);
        if (elapsed >= 60) {
            double w = (double)(elapsed < MIN_SPAN ? elapsed : MIN_SPAN);
            num += w * (cur / (double)elapsed);
            den += w;
        }
    }
    free(ts);
    free(ys);
    if (have_prior) {
        // prior fades out as live evidence accumulates; gone at full lookback
        double frac = 1.0 - (double)span / (double)LOOKBACK;
        double w = W_PRIOR * (frac > 0.0 ? frac : 0.0);
        num += w * (prior > 0.0 ? prior : 0.0);
        den += w;
    }
    if (den <= 0.0) return 0;
    double rate = num / den;
    if (rate <= 0.0) return 0;
    double secs = (100.0 - cur) / rate;
    if (secs > 1e9) secs = 1e9;
    *out = now + (long long)secs;
    return 1;
}

// ---------------------------------------------------------------- main

#ifndef SL_TEST
static void put(int fd, const char *s, size_t n) {
    for (size_t off = 0; off < n;) {
        ssize_t w = write(fd, s + off, n - off);
        if (w <= 0) return;
        off += (size_t)w;
    }
}

static const char USAGE[] =
    "usage: statusline [--simple]\n"
    "\n"
    "Reads the Claude Code session JSON on stdin and writes one status line.\n"
    "\n"
    "  --simple   context window, 5 h limit, path and GPU/VRAM only; omits the\n"
    "             clock, the account, the model and the remaining host metrics\n"
    "  -h, --help this text\n";

int main(int argc, char **argv) {
    int simple = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--simple")) {
            simple = 1;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            put(1, USAGE, sizeof USAGE - 1);
            return 0;
        } else {
            put(2, USAGE, sizeof USAGE - 1);
            return 2;
        }
    }

    size_t len;
    char *in = read_fd(0, &len, 0);
    const char *root = js_ws(in);

    long long now = (long long)time(NULL);
    int have_state = state_dir();

    // the line is the clock, then two ` | `-separated blocks: what this session is
    // spending and where it runs, and what the host has left. Fields inside a block are
    // space-separated; a field with no source contributes nothing, and a block whose
    // fields are all unavailable disappears together with its separator.
    Out o = out_new(256), ctx = out_new(64), path = out_new(256), ident = out_new(128),
        session = out_new(160), host = out_new(160), what = out_new(512);
    char g[32], g2[32], f[128];

    double ctxp;
    if (js_num(js_get(js_get(root, "context_window"), "used_percentage"), &ctxp)) {
        ctx_gauge(g, sizeof g, (long long)llround(ctxp));
        out_add(&ctx, "CTX %s", g);
        // the same tokens the percentage is computed from, so gauge and count agree
        double tok;
        if (js_num(js_get(js_get(root, "context_window"), "total_input_tokens"), &tok)) {
            fmt_tokens(f, sizeof f, (long long)tok);
            out_add(&ctx, " %s", f);
        }
    }

    char *cwd = js_str(js_get(root, "cwd"));
    if (cwd) {
        size_t n = strlen(cwd) + 2;
        char *abbr = xmalloc(n);
        tilde(abbr, n, cwd, getenv("HOME"));
        out_add(&path, PATH_COLOR "%s" FG_RESET, abbr);
        free(abbr);
    }

    if (!simple) {
        char *mail = account_email();
        if (mail) {
            char *at = strchr(mail, '@');
            if (at) *at = '\0';
            push_field(&ident, mail);
        }
        free(mail);

        char *model = js_str(js_get(js_get(root, "model"), "display_name"));
        if (model) {
            push_field(&ident, model);
            char *effort = js_str(js_get(js_get(root, "effort"), "level"));
            if (effort) push_field(&ident, effort);
            free(effort);
        }
        free(model);

        unsigned long long freeb;
        if (disk_free(cwd ? cwd : "/", &freeb)) {
            fmt_bytes(g, sizeof g, freeb);
            snprintf(f, sizeof f, "DISK %s", g);
            push_field(&host, f);
        }

        long long v;
        if (have_state && cpu_pct(&v)) {
            gauge(g, sizeof g, v);
            snprintf(f, sizeof f, "CPU %s", g);
            push_field(&host, f);
        }
        char *mem = read_head("/proc/meminfo", 512);
        if (mem && meminfo_pct(mem, &v)) {
            gauge(g, sizeof g, v);
            snprintf(f, sizeof f, "RAM %s", g);
            push_field(&host, f);
        }
        free(mem);
    }

    long long gpu, vram;
    if (have_state && gpu_stats(now, &gpu, &vram)) {
        gauge(g, sizeof g, gpu);
        gauge(g2, sizeof g2, vram);
        snprintf(f, sizeof f, "GPU %s VRAM %s", g, g2);
        push_field(&host, f);
    }

    const char *j5 = js_get(js_get(root, "rate_limits"), "five_hour");
    const char *j7 = js_get(js_get(root, "rate_limits"), "seven_day");
    double fp = 0, sp = 0, dr;
    long long fr = 0, sr = 0;
    int have5 = js_num(js_get(j5, "used_percentage"), &fp) &&
                js_num(js_get(j5, "resets_at"), &dr) && ((fr = (long long)dr), 1);
    int have7 = js_num(js_get(j7, "used_percentage"), &sp) &&
                js_num(js_get(j7, "resets_at"), &dr) && ((sr = (long long)dr), 1);

    // projected depletion: sample usage over time, extrapolate burn rate to 100%
    int have_eta = 0;
    long long eta = 0;
    if (have5 && have_state) {
        size_t ns, nr, rcap;
        int clean;
        Sample *samples = load_samples(&ns, &clean);
        Rate *rates = load_rates(&nr);
        rcap = nr;
        if (harvest_rates(samples, ns, fr, &rates, &nr, &rcap)) {
            Out b = out_new(512);
            for (size_t i = 0; i < nr; i++) {
                char r[32];
                fmt_f64(r, sizeof r, rates[i].rate);
                out_add(&b, "%lld %s\n", rates[i].reset, r);
            }
            write_file(spath("rates.tsv"), b.buf, b.len, 0);
            free(b.buf);
        }
        if (!ns || now - samples[ns - 1].ts >= SAMPLE_GAP) {
            // Compacting means rewriting the file, so it is amortized: the log may
            // grow to twice what is useful, and a closed window is dropped as soon
            // as it shows up at the front. Every other tick just appends ~40 bytes.
            int rewrite = !clean || ns >= 2 * MAX_LIVE || (ns && samples[0].five_reset != fr);
            samples = xrealloc(samples, (ns + 1) * sizeof *samples);
            samples[ns++] = (Sample){now, fp, fr, have7 ? sp : -1.0, have7 ? sr : 0};
            char a[32], c[32];
            if (rewrite) {
                ns = compact(samples, ns, now, fr);
                Out b = out_new(1 << 14);
                for (size_t i = 0; i < ns; i++) {
                    fmt_f64(a, sizeof a, samples[i].five);
                    fmt_f64(c, sizeof c, samples[i].seven);
                    out_add(&b, "%lld %s %lld %s %lld\n", samples[i].ts, a, samples[i].five_reset,
                            c, samples[i].seven_reset);
                }
                write_file(spath("samples.tsv"), b.buf, b.len, 0);
                free(b.buf);
            } else {
                const Sample *s = &samples[ns - 1];
                fmt_f64(a, sizeof a, s->five);
                fmt_f64(c, sizeof c, s->seven);
                char line[192];
                int n = snprintf(line, sizeof line, "%lld %s %lld %s %lld\n", s->ts, a,
                                 s->five_reset, c, s->seven_reset);
                if (n > 0) write_file(spath("samples.tsv"), line, (size_t)n, 1);
            }
        }
        double prior;
        double *tmp = xmalloc((nr ? nr : 1) * sizeof *tmp);
        for (size_t i = 0; i < nr; i++) tmp[i] = rates[i].rate;
        int have_prior = median(tmp, nr, &prior);
        free(tmp);
        have_eta = eta_5h(samples, ns, now, fp, fr, 5 * 3600, have_prior, prior, &eta);
        free(samples);
        free(rates);
    }

    if (have5) {
        // the 5h window (usage + depletion forecast) is the headline metric, so
        // emphasize it in bold (\x1b[1m); the rest of the line stays default weight.
        // It is also the one gauge that keeps its digits -- the exact number matters
        // for pacing.
        long long pi = (long long)llround(fp);
        gauge(g, sizeof g, pi);
        Out seg = out_new(160);
        out_add(&seg, "SESSION %s %lld%%", g, pi);
        push_times(&seg, have_eta, eta, fr, now);
        out_add(&session, "\x1b[1m%s\x1b[0m", seg.buf);
        free(seg.buf);
    }

    if (!simple && fmt_ts(f, sizeof f, now, "%H:%M")) out_add(&o, "%s", f);

    push_field(&what, ctx.buf);
    push_field(&what, session.buf);
    push_field(&what, path.buf);
    push_field(&what, ident.buf);
    push_block(&o, what.buf);
    push_block(&o, host.buf);

    // a raw write keeps stdio -- and the fstat/ioctl it does to probe stdout -- out
    // of the binary entirely
    out_add(&o, "\n");
    put(1, o.buf, o.len);
    return 0;
}
#endif

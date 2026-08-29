// Unit tests for the forecast math and the parsers, ported from the #[cfg(test)]
// module of the Rust implementation. Built by `make test`.

#include "statusline.c"

#define NOW 1750000000LL

static int failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d  %s\n", __func__, __LINE__, #cond);      \
            failures++;                                                 \
        }                                                               \
    } while (0)

// `minutes` samples one minute apart, ending at `now`, rising by `per_min`
static size_t linear_samples(Sample *dst, long long now, long long reset, long long minutes,
                             double start, double per_min) {
    size_t n = 0;
    for (long long i = minutes; i >= 1; i--)
        dst[n++] = (Sample){now - i * 60, start + (double)(minutes - i) * per_min, reset, -1.0, 0};
    return n;
}

static void test_wslope(void) {
    long long t[10];
    double y[10];
    for (int i = 0; i < 10; i++) {
        t[i] = i * 60;
        y[i] = 10.0 + i * 0.5;
    }
    double s;
    CHECK(wslope(t, y, 10, &s) && fabs(s - 0.5 / 60.0) < 1e-12);

    for (int i = 0; i < 10; i++) y[i] = 42.0;
    CHECK(wslope(t, y, 10, &s) && fabs(s) < 1e-12);

    CHECK(!wslope(t, y, 2, &s));  // needs three points
}

static void test_median(void) {
    double a[] = {3.0, 1.0, 2.0}, b[] = {4.0, 1.0}, m;
    CHECK(!median(a, 0, &m));
    CHECK(median(a, 3, &m) && m == 2.0);
    CHECK(median(b, 2, &m) && m == 4.0);  // upper median on even n
}

static void test_eta(void) {
    Sample s[64];
    long long reset = NOW + 4 * 3600, t;

    // 0.9 %/min for 40 min, 66.0% now -> 100% in (34/0.9) min
    size_t n = linear_samples(s, NOW, reset, 40, 30.0, 0.9);
    CHECK(eta_5h(s, n, NOW, 66.0, reset, 5 * 3600, 0, 0.0, &t));
    long long want = NOW + (long long)(34.0 / 0.9 * 60.0);
    CHECK(llabs(t - want) < 5);

    // idle without a prior yields no projection
    n = linear_samples(s, NOW, reset, 40, 66.2, 0.0);
    CHECK(!eta_5h(s, n, NOW, 66.2, reset, 5 * 3600, 0, 0.0, &t));

    // only 2 min of samples -> window average (10% over 1h elapsed) -> 9h out
    n = linear_samples(s, NOW, reset, 2, 9.9, 0.05);
    CHECK(eta_5h(s, n, NOW, 10.0, reset, 5 * 3600, 0, 0.0, &t));
    want = NOW + (long long)(90.0 / (10.0 / 3600.0));
    CHECK(llabs(t - want) < 5);

    // no samples, window 5 min old at 2%: blend of window avg (w=300) and prior (w=1200)
    long long fresh_reset = NOW + 295 * 60;
    double prior = 0.02;
    CHECK(eta_5h(NULL, 0, NOW, 2.0, fresh_reset, 5 * 3600, 1, prior, &t));
    double rate = (300.0 * (2.0 / 300.0) + 1200.0 * prior) / 1500.0;
    CHECK(llabs(t - (NOW + (long long)(98.0 / rate))) < 5);

    // 45 min of flat live data: prior weight is 0, idle -> no projection
    n = linear_samples(s, NOW, reset, 45, 66.2, 0.0);
    CHECK(!eta_5h(s, n, NOW, 66.2, reset, 5 * 3600, 1, 0.02, &t));

    // samples of a previous window must not feed the regression: only the
    // window-average fallback remains (5% over 1h elapsed)
    n = linear_samples(s, NOW, reset - 7200, 40, 30.0, 0.9);
    CHECK(eta_5h(s, n, NOW, 5.0, reset, 5 * 3600, 0, 0.0, &t));
    want = NOW + (long long)(95.0 / (5.0 / 3600.0));
    CHECK(llabs(t - want) < 5);
}

static void test_harvest(void) {
    Sample s[128];
    long long cur_reset = NOW + 4 * 3600, old_reset = NOW - 7200;

    // 30 min span, +18% -> 0.01 %/sec
    size_t n = linear_samples(s, NOW - 7200 - 1800, old_reset, 30, 20.6, 0.6);
    n += linear_samples(s + n, NOW, cur_reset, 5, 1.0, 0.5);
    Rate *r = NULL;
    size_t nr = 0, cap = 0;
    CHECK(harvest_rates(s, n, cur_reset, &r, &nr, &cap));
    CHECK(nr == 1 && r[0].reset == old_reset && fabs(r[0].rate - 0.01) < 1e-6);
    free(r);

    // old window already harvested, current one must never be -> no change
    n = linear_samples(s, NOW - 7200 - 1800, old_reset, 30, 20.6, 0.6);
    n += linear_samples(s + n, NOW, cur_reset, 30, 1.0, 0.5);
    cap = 4;
    r = malloc(cap * sizeof *r);
    r[0] = (Rate){old_reset, 0.01};
    nr = 1;
    CHECK(!harvest_rates(s, n, cur_reset, &r, &nr, &cap));
    CHECK(nr == 1);
    free(r);

    // 10 min span (< HARVEST_MIN_SPAN) and 30 min but +1.5% (< HARVEST_MIN_PCT)
    r = NULL;
    nr = cap = 0;
    n = linear_samples(s, NOW - 7200, NOW - 3600, 10, 20.0, 0.6);
    CHECK(!harvest_rates(s, n, cur_reset, &r, &nr, &cap));
    n = linear_samples(s, NOW - 7200, NOW - 7000, 30, 20.0, 0.05);
    CHECK(!harvest_rates(s, n, cur_reset, &r, &nr, &cap));
    CHECK(nr == 0);
    free(r);

    // history is capped, oldest entries drop first
    cap = RATES_KEEP + 5;
    r = malloc(cap * sizeof *r);
    for (size_t i = 0; i < cap; i++) r[i] = (Rate){(long long)i, 0.01};
    nr = cap;
    CHECK(harvest_rates(NULL, 0, 999999, &r, &nr, &cap));
    CHECK(nr == RATES_KEEP && r[0].reset == 5);
    free(r);
}

static void test_compact(void) {
    Sample s[256];
    long long cur = NOW + 4 * 3600, old = NOW - 7200;

    // a closed window (already harvested into rates.tsv) is dropped wholesale
    size_t n = linear_samples(s, NOW - 7200, old, 30, 20.0, 0.6);
    // current window: 90 min of samples, only the last 45 min are in the lookback
    n += linear_samples(s + n, NOW, cur, 90, 0.0, 0.5);
    size_t k = compact(s, n, NOW, cur);
    CHECK(k == 1 + LOOKBACK / 60);  // anchor + one sample per minute of lookback
    CHECK(s[0].five_reset == cur);
    CHECK(s[0].ts == NOW - 90 * 60);            // anchor is the window's first sample
    CHECK(s[1].ts == NOW - LOOKBACK);           // then the lookback tail, in order
    CHECK(s[k - 1].ts == NOW - 60);

    // a window that fits entirely inside the lookback is kept as-is, no duplicate
    n = linear_samples(s, NOW, cur, 10, 0.0, 0.5);
    CHECK(compact(s, n, NOW, cur) == n);

    // nothing of the current window -> nothing to keep
    n = linear_samples(s, NOW, old, 10, 0.0, 0.5);
    CHECK(compact(s, n, NOW, cur) == 0);
    CHECK(compact(NULL, 0, NOW, cur) == 0);
}

static void test_push_times(void) {
    Out o = {malloc(256), 0, 256};
    o.buf[0] = '\0';
    push_times(&o, 1, NOW + 7 * 3600 + 1800, NOW, NOW);
    CHECK(strstr(o.buf, "(+8h / "));  // ceil(7.5h)

    // 30 min out (>= URGENT_SECS) -> plain clock time, no signal color
    o.len = 0;
    o.buf[0] = '\0';
    push_times(&o, 1, NOW + 1800, NOW + 3600, NOW);
    CHECK(strstr(o.buf, "(~") && strstr(o.buf, " / ") && !strstr(o.buf, URGENT));

    // 10 min out (< URGENT_SECS) -> ETA clock time wrapped in the signal color,
    // and the color must not leak past the ETA into the reset time
    o.len = 0;
    o.buf[0] = '\0';
    push_times(&o, 1, NOW + 600, NOW + 3600, NOW);
    CHECK(strstr(o.buf, URGENT) && strstr(o.buf, FG_RESET " / "));

    o.len = 0;
    o.buf[0] = '\0';
    push_times(&o, 0, 0, NOW, NOW);
    CHECK(!strchr(o.buf, '~') && !strchr(o.buf, '+'));
    CHECK(o.buf[0] == ' ' && o.buf[1] == '(' && o.buf[o.len - 1] == ')');
    free(o.buf);
}

static void test_parse_gpu_csv(void) {
    long long g, v;
    CHECK(parse_gpu_csv("7, 2126, 16303\n", &g, &v) && g == 7 && v == 13);
    // multi-GPU: first line wins
    CHECK(parse_gpu_csv("50, 8000, 16000\n10, 1, 16000\n", &g, &v) && g == 50 && v == 50);
    CHECK(!parse_gpu_csv("", &g, &v));
    CHECK(!parse_gpu_csv("[N/A], 0, 0\n", &g, &v));
    CHECK(!parse_gpu_csv("7, 2126, 0\n", &g, &v));
}

static void test_parse_cpu_totals(void) {
    unsigned long long t, i;
    // user nice system idle iowait irq softirq steal
    CHECK(parse_cpu_totals("cpu  100 0 50 800 40 5 5 0\ncpu0 50 0 25 400 20 2 3 0\n", &t, &i));
    CHECK(t == 1000 && i == 840);
    // pre-2.6 kernels: only 4 fields, no iowait
    CHECK(parse_cpu_totals("cpu 10 0 10 80\n", &t, &i) && t == 100 && i == 80);
    CHECK(!parse_cpu_totals("cpu0 1 2 3 4\n", &t, &i));
    CHECK(!parse_cpu_totals("cpu 1 2 3\n", &t, &i));
}

static void test_meminfo_pct(void) {
    long long p;
    CHECK(meminfo_pct("MemTotal:       16000000 kB\nMemFree:         1000000 kB\n"
                      "MemAvailable:    4000000 kB\n",
                      &p));
    CHECK(p == 75);
    CHECK(!meminfo_pct("MemTotal: 16000000 kB\n", &p));
    CHECK(!meminfo_pct("", &p));
}

static void test_fmt_bytes(void) {
    char b[32];
    fmt_bytes(b, sizeof b, 500ULL << 20);
    CHECK(!strcmp(b, "500M"));
    fmt_bytes(b, sizeof b, 5ULL << 30);
    CHECK(!strcmp(b, "5.0G"));
    fmt_bytes(b, sizeof b, 897ULL << 30);
    CHECK(!strcmp(b, "897G"));
    fmt_bytes(b, sizeof b, 1536ULL << 30);
    CHECK(!strcmp(b, "1.5T"));
}

static void test_parse_email(void) {
    char *s = parse_email(
        "{\"a\":\"b\",\"oauthAccount\":{\"accountUuid\":\"u\","
        "\"emailAddress\":\"me@example.com\"},\"c\":1}");
    CHECK(s && !strcmp(s, "me@example.com"));
    free(s);
    // an emailAddress before oauthAccount must not be picked up
    s = parse_email("{\"emailAddress\":\"decoy@x.io\",\"oauthAccount\":"
                    "{\"emailAddress\":\"real@x.io\"}}");
    CHECK(s && !strcmp(s, "real@x.io"));
    free(s);
    CHECK(!parse_email("{}"));
    CHECK(!parse_email("{\"oauthAccount\":{}}"));
}

static void test_gauge(void) {
    char g[32], want[32];
    struct {
        long long p;
        const char *color, *glyph;
    } cases[] = {{0, "32", "\xE2\x97\x8B"},   {35, "32", "\xE2\x97\x94"},
                 {50, "32", "\xE2\x97\x91"},  {68, "33", "\xE2\x97\x95"},
                 {97, "31", "\xE2\x97\x8F"},
                 // out-of-range input must not index past the glyph table
                 {140, "31", "\xE2\x97\x8F"}, {-5, "32", "\xE2\x97\x8B"}};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        gauge(g, sizeof g, cases[i].p);
        snprintf(want, sizeof want, "\x1b[%sm%s" FG_RESET, cases[i].color, cases[i].glyph);
        CHECK(!strcmp(g, want));
    }
    // the glyph boundaries follow the documented table
    gauge(g, sizeof g, 12);
    CHECK(strstr(g, "\xE2\x97\x8B"));
    gauge(g, sizeof g, 13);
    CHECK(strstr(g, "\xE2\x97\x94"));
    gauge(g, sizeof g, 87);
    CHECK(strstr(g, "\xE2\x97\x95"));
    gauge(g, sizeof g, 88);
    CHECK(strstr(g, "\xE2\x97\x8F"));
}

static void test_ctx_gauge(void) {
    char g[32];
    // CTX turns yellow at 25 and red at 40, earlier than the other gauges
    ctx_gauge(g, sizeof g, 24);
    CHECK(!strcmp(g, "\x1b[32m\xE2\x97\x94" FG_RESET));
    ctx_gauge(g, sizeof g, 25);
    CHECK(!strcmp(g, "\x1b[33m\xE2\x97\x94" FG_RESET));
    ctx_gauge(g, sizeof g, 39);
    CHECK(!strcmp(g, "\x1b[33m\xE2\x97\x91" FG_RESET));
    ctx_gauge(g, sizeof g, 40);
    CHECK(!strcmp(g, "\x1b[31m\xE2\x97\x91" FG_RESET));
}

static void test_fmt_tokens(void) {
    char b[32];
    fmt_tokens(b, sizeof b, 0);
    CHECK(!strcmp(b, "0"));
    fmt_tokens(b, sizeof b, 999);
    CHECK(!strcmp(b, "999"));
    fmt_tokens(b, sizeof b, 1000);
    CHECK(!strcmp(b, "1k"));
    fmt_tokens(b, sizeof b, 78400);
    CHECK(!strcmp(b, "78k"));
    fmt_tokens(b, sizeof b, 78600);
    CHECK(!strcmp(b, "79k"));
}

static void test_tilde(void) {
    char b[256];
    struct {
        const char *cwd, *home, *want;
    } cases[] = {{"/home/mik", "/home/mik", "~"},
                 {"/home/mik/", "/home/mik", "~/"},
                 {"/home/mik/src", "/home/mik", "~/src"},
                 {"/home/mik", "/home/mik/", "~"},
                 // no false prefix match on a sibling directory
                 {"/home/mikael", "/home/mik", "/home/mikael"},
                 {"/", "/home/mik", "/"},
                 {"/etc", NULL, "/etc"},
                 // a degenerate HOME must not swallow every path
                 {"/etc", "/", "/etc"},
                 {"/etc", "", "/etc"}};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        tilde(b, sizeof b, cases[i].cwd, cases[i].home);
        CHECK(!strcmp(b, cases[i].want));
    }
}

static void test_blocks(void) {
    Out o = out_new(64);
    // an empty part contributes nothing, not even its separator
    push_block(&o, "");
    CHECK(o.len == 0);
    push_block(&o, "a");
    CHECK(!strcmp(o.buf, "a"));
    push_block(&o, "");
    CHECK(!strcmp(o.buf, "a"));
    push_block(&o, "b");
    CHECK(!strcmp(o.buf, "a | b"));
    free(o.buf);

    o = out_new(64);
    push_field(&o, "");
    push_field(&o, "x");
    push_field(&o, "y");
    CHECK(!strcmp(o.buf, "x y"));
    free(o.buf);
}

static void test_json(void) {
    const char *j =
        "{\"cwd\":\"/tmp/a b\",\"model\":{\"display_name\":\"Fable 5\"},"
        "\"context_window\":{\"used_percentage\":38.2},"
        "\"rate_limits\":{\"five_hour\":{\"used_percentage\":66.4,\"resets_at\":1750000000}}}";
    char *s = js_str(js_get(j, "cwd"));
    CHECK(s && !strcmp(s, "/tmp/a b"));
    free(s);
    s = js_str(js_get(js_get(j, "model"), "display_name"));
    CHECK(s && !strcmp(s, "Fable 5"));
    free(s);

    double d;
    CHECK(js_num(js_get(js_get(j, "context_window"), "used_percentage"), &d) && d == 38.2);
    const char *five = js_get(js_get(j, "rate_limits"), "five_hour");
    CHECK(js_num(js_get(five, "resets_at"), &d) && (long long)d == 1750000000LL);
    CHECK(!js_get(j, "missing"));
    CHECK(!js_get(js_get(j, "missing"), "x"));
    // a key inside a nested object must not be found at the top level
    CHECK(!js_get(j, "used_percentage"));
    // escapes and non-ASCII survive decoding
    s = js_str(js_get("{\"cwd\":\"a\\\"b\\\\c\\u00e4\"}", "cwd"));
    CHECK(s && !strcmp(s, "a\"b\\c\xc3\xa4"));
    free(s);
    CHECK(!js_str(js_get("{\"cwd\":\"\"}", "cwd")));  // empty string is no value
    CHECK(!js_num(js_get("{\"a\":null}", "a"), &d));
    CHECK(!js_get("not json", "a"));
}

// the Clinger fast path must be bit-identical to strtod, never merely close
static void test_scanners(void) {
    static const char *cases[] = {"0",   "21",   "-1",     "66.4",  "0.01",  "100",
                                  "3.5", "0.5",  "99.999", "1e-3",  "-0.25", "0.1",
                                  "2.675", "1234567890.123456", "0.000001",
                                  "12345678901234567890.5", "1.7976931348623157e308"};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        const char *p = cases[i];
        double got;
        CHECK(scan_f64(&p, &got));
        double want = strtod(cases[i], NULL);
        CHECK(memcmp(&got, &want, sizeof got) == 0);
        CHECK(*p == '\0');  // the whole literal was consumed
    }
    const char *bad = "abc";
    double d;
    CHECK(!scan_f64(&bad, &d));

    const char *p = "  -42 7";
    long long v;
    CHECK(scan_ll(&p, &v) && v == -42);
    CHECK(scan_ll(&p, &v) && v == 7);
    CHECK(!scan_ll(&p, &v));
}

static void test_fmt_f64(void) {
    char b[32];
    // the sample/rate files stay byte-compatible with Rust's float formatting
    fmt_f64(b, sizeof b, 21.0);
    CHECK(!strcmp(b, "21"));
    fmt_f64(b, sizeof b, 66.4);
    CHECK(!strcmp(b, "66.4"));
    fmt_f64(b, sizeof b, -1.0);
    CHECK(!strcmp(b, "-1"));
    fmt_f64(b, sizeof b, 0.010000000000000002);
    CHECK(strtod(b, NULL) == 0.010000000000000002);
}

static void test_load_samples(void) {
    setenv("XDG_CACHE_HOME", "/tmp/statusline-c-test", 1);
    CHECK(state_dir());
    write_file(spath("samples.tsv"), "100 1.5 200 2.5 300\ngarbage\n101 1.6 200 -1 0\n1 2 3\n", 49, 0);
    size_t n;
    Sample *s = load_samples(&n, NULL);
    CHECK(n == 2);
    CHECK(s && s[1].ts == 101 && s[1].seven == -1.0);
    free(s);
    unlink(spath("samples.tsv"));

    write_file(spath("rates.tsv"), "100 0.01\nbad\n200 0.02 3\n300 0.03\n", 33, 0);
    Rate *r = load_rates(&n);
    CHECK(n == 2);
    CHECK(r && r[1].reset == 300 && r[1].rate == 0.03);
    free(r);
    unlink(spath("rates.tsv"));
}

int main(void) {
    test_wslope();
    test_median();
    test_eta();
    test_harvest();
    test_compact();
    test_push_times();
    test_parse_gpu_csv();
    test_parse_cpu_totals();
    test_meminfo_pct();
    test_fmt_bytes();
    test_parse_email();
    test_gauge();
    test_ctx_gauge();
    test_fmt_tokens();
    test_tilde();
    test_blocks();
    test_json();
    test_scanners();
    test_fmt_f64();
    test_load_samples();
    printf(failures ? "%d test(s) failed\n" : "all tests passed\n", failures);
    return failures ? 1 : 0;
}

/* OS bridge for the cruftar CLI.
 *
 * Cruft's standard library can read and write files and streams. It cannot
 * see argv, create directories, spawn cruftc, or compare strings. Those
 * operations live here. Command behavior stays in the .cruft sources.
 */
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <conio.h>
#include <direct.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#define SLOT_CAP 4096
#define SLOT_N 40
#define LIST_N 12
#define CMD_N 256

static char g_slot[SLOT_N][SLOT_CAP];
static int64_t g_slot_len[SLOT_N];

typedef struct Item {
    char *data;
    int64_t len;
} Item;

static Item *g_list[LIST_N];
static int g_list_count[LIST_N];
static int g_list_cap[LIST_N];

static char *g_buf;
static int64_t g_buf_len;
static int64_t g_buf_cap;

static char **g_argv;
static int g_argc;
static int g_args_ready;

static char *g_cmd[CMD_N];
static int64_t g_cmd_len[CMD_N];
static int g_cmd_n;
static char g_cmd_cwd[SLOT_CAP];

static char *g_captured;
static int64_t g_captured_len;

static int g_tty;
static int g_color;
static int g_inited;

static uint64_t g_tick;

#ifdef _WIN32
static volatile LONG g_spin_on;
static HANDLE g_spin_thread;
static char g_spin_msg[240];
#else
static volatile int g_spin_on;
static char g_spin_msg[240];
#endif

static int icmp_eq(const char *a, const char *b);

static int slot_ok(int32_t slot) { return slot >= 0 && slot < SLOT_N; }
static int list_ok(int32_t id) { return id >= 0 && id < LIST_N; }

static char *xstrdup_n(const char *s, int64_t n) {
    if (n < 0)
        n = 0;
    char *out = (char *)malloc((size_t)n + 1);
    if (!out)
        return NULL;
    if (n > 0 && s)
        memcpy(out, s, (size_t)n);
    out[n] = 0;
    return out;
}

void host_init(void) {
    if (g_inited)
        return;
    g_inited = 1;
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(out, &mode)) {
        g_tty = 1;
        SetConsoleMode(out, mode | 0x0004); /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */
    }
    if (GetConsoleMode(err, &mode))
        SetConsoleMode(err, mode | 0x0004);
    int in_tty = GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) ? 1 : 0;
    if (!in_tty)
        g_tty = 0;
#else
    g_tty = isatty(STDOUT_FILENO) && isatty(STDIN_FILENO);
#endif
    g_color = g_tty && getenv("NO_COLOR") == NULL;
}

int32_t host_is_tty(void) {
    host_init();
    return g_tty ? 1 : 0;
}

int32_t host_color(void) {
    host_init();
    return g_color ? 1 : 0;
}

static void ensure_args(void) {
    if (g_args_ready)
        return;
    g_args_ready = 1;
#ifdef _WIN32
    int argc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv || argc < 0) {
        g_argc = 0;
        g_argv = NULL;
        return;
    }
    g_argv = (char **)calloc((size_t)argc, sizeof(char *));
    g_argc = argc;
    if (!g_argv) {
        LocalFree(wargv);
        g_argc = 0;
        return;
    }
    for (int i = 0; i < argc; i++) {
        int need = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        if (need < 1)
            need = 1;
        g_argv[i] = (char *)malloc((size_t)need);
        if (!g_argv[i]) {
            g_argv[i] = xstrdup_n("", 0);
            continue;
        }
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, g_argv[i], need, NULL, NULL);
    }
    LocalFree(wargv);
#else
    FILE *f = fopen("/proc/self/cmdline", "rb");
    if (!f) {
        g_argc = 0;
        g_argv = NULL;
        return;
    }
    char tmp[32768];
    size_t n = fread(tmp, 1, sizeof(tmp), f);
    fclose(f);
    int count = 0;
    for (size_t i = 0; i < n; i++)
        if (tmp[i] == 0)
            count++;
    if (count < 1)
        count = n > 0 ? 1 : 0;
    g_argv = (char **)calloc((size_t)count, sizeof(char *));
    g_argc = 0;
    if (!g_argv)
        return;
    size_t start = 0;
    for (size_t i = 0; i < n && g_argc < count; i++) {
        if (tmp[i] == 0) {
            g_argv[g_argc++] = xstrdup_n(tmp + start, (int64_t)(i - start));
            start = i + 1;
        }
    }
#endif
}

int32_t host_argc(void) {
    ensure_args();
    return g_argc;
}

const char *host_argv(int32_t index) {
    ensure_args();
    if (index < 0 || index >= g_argc || !g_argv || !g_argv[index])
        return "";
    return g_argv[index];
}

int64_t host_argv_len(int32_t index) {
    const char *s = host_argv(index);
    return (int64_t)strlen(s);
}

int32_t host_streq(const char *data, int64_t n, const char *lit) {
    if (!lit)
        lit = "";
    if (!data)
        data = "";
    if (n < 0)
        n = 0;
    size_t m = strlen(lit);
    if ((size_t)n != m)
        return 0;
    return memcmp(data, lit, m) == 0 ? 1 : 0;
}

int32_t host_starts(const char *data, int64_t n, const char *lit) {
    if (!lit)
        return 1;
    if (!data)
        data = "";
    if (n < 0)
        n = 0;
    size_t m = strlen(lit);
    if ((size_t)n < m)
        return 0;
    return memcmp(data, lit, m) == 0 ? 1 : 0;
}

int32_t host_ends(const char *data, int64_t n, const char *lit) {
    if (!lit)
        return 1;
    if (!data)
        data = "";
    if (n < 0)
        n = 0;
    size_t m = strlen(lit);
    if ((size_t)n < m)
        return 0;
    return memcmp(data + (n - (int64_t)m), lit, m) == 0 ? 1 : 0;
}

int32_t host_contains(const char *data, int64_t n, const char *lit) {
    if (!lit || !lit[0])
        return 1;
    if (!data || n <= 0)
        return 0;
    size_t m = strlen(lit);
    if ((size_t)n < m)
        return 0;
    for (int64_t i = 0; i + (int64_t)m <= n; i++) {
        if (memcmp(data + i, lit, m) == 0)
            return 1;
    }
    return 0;
}

int32_t host_has_entry(const char *data, int64_t n) {
    return host_contains(data, n, "pub fn main");
}

int32_t host_count(const char *data, int64_t n, const char *lit) {
    if (!lit || !lit[0] || !data || n <= 0)
        return 0;
    size_t m = strlen(lit);
    int32_t c = 0;
    for (int64_t i = 0; i + (int64_t)m <= n; i++) {
        if (memcmp(data + i, lit, m) == 0)
            c++;
    }
    return c;
}

int32_t host_byte(const char *data, int64_t n, int64_t i) {
    if (!data || i < 0 || i >= n)
        return -1;
    return (int32_t)(unsigned char)data[i];
}

int64_t host_find_byte(const char *data, int64_t n, int64_t from, int32_t byte) {
    if (!data || from < 0)
        return -1;
    for (int64_t i = from; i < n; i++) {
        if ((unsigned char)data[i] == (unsigned char)byte)
            return i;
    }
    return -1;
}

const char *host_ptr_add(const char *p, int64_t n) {
    if (!p)
        return "";
    if (n < 0)
        n = 0;
    return p + n;
}

int64_t host_trim_off(const char *p, int64_t n) {
    if (!p || n <= 0)
        return 0;
    int64_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)p[i];
        if (c != ' ' && c != '\t' && c != '\r')
            break;
        i++;
    }
    return i;
}

int64_t host_trim_len(const char *p, int64_t n) {
    if (!p || n <= 0)
        return 0;
    int64_t b = host_trim_off(p, n);
    int64_t e = n;
    while (e > b) {
        unsigned char c = (unsigned char)p[e - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            break;
        e--;
    }
    return e - b;
}

void host_memcpy(void *dst, const void *src, int64_t n) {
    if (!dst || !src || n <= 0)
        return;
    memcpy(dst, src, (size_t)n);
}

static void buf_reserve(int64_t extra) {
    if (extra < 0)
        extra = 0;
    if (g_buf && g_buf_len + extra + 1 <= g_buf_cap)
        return;
    int64_t cap = g_buf_cap > 0 ? g_buf_cap : 256;
    while (cap < g_buf_len + extra + 1) {
        if (cap > (1 << 28))
            break;
        cap *= 2;
    }
    char *next = (char *)realloc(g_buf, (size_t)cap);
    if (!next)
        return;
    g_buf = next;
    g_buf_cap = cap;
}

void host_buf_clear(void) {
    g_buf_len = 0;
    if (g_buf)
        g_buf[0] = 0;
}

void host_buf_puts(const char *s) {
    if (!s)
        return;
    int64_t n = (int64_t)strlen(s);
    buf_reserve(n);
    if (!g_buf || g_buf_len + n + 1 > g_buf_cap)
        return;
    memcpy(g_buf + g_buf_len, s, (size_t)n);
    g_buf_len += n;
    g_buf[g_buf_len] = 0;
}

void host_buf_put(const char *s, int64_t n) {
    if (!s || n <= 0)
        return;
    buf_reserve(n);
    if (!g_buf || g_buf_len + n + 1 > g_buf_cap)
        return;
    memcpy(g_buf + g_buf_len, s, (size_t)n);
    g_buf_len += n;
    g_buf[g_buf_len] = 0;
}

void host_buf_putc(int32_t c) {
    char ch = (char)c;
    host_buf_put(&ch, 1);
}

void host_buf_put_i64(int64_t v) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    host_buf_puts(tmp);
}

void host_buf_put_i32(int32_t v) {
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%d", v);
    host_buf_puts(tmp);
}

void host_buf_put_size(int64_t bytes) {
    host_buf_put_i64(bytes);
    host_buf_puts(" bytes (");
    if (bytes < 1024) {
        host_buf_put_i64(bytes);
        host_buf_puts(" B)");
        return;
    }
    double v = (double)bytes;
    const char *unit = "KiB";
    v /= 1024.0;
    if (v >= 1024.0) {
        v /= 1024.0;
        unit = "MiB";
    }
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%.1f %s)", v, unit);
    host_buf_puts(tmp);
}

void host_buf_bar(int32_t done, int32_t total) {
    int width = 20;
    if (total < 1)
        total = 1;
    if (done < 0)
        done = 0;
    if (done > total)
        done = total;
    int fill = (int)((done * width) / total);
    for (int i = 0; i < width; i++)
        host_buf_putc(i < fill ? '#' : '-');
}

void host_buf_put_quoted(const char *s, int64_t n) {
    host_buf_putc('"');
    if (!s)
        n = 0;
    for (int64_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\\' || c == '"')
            host_buf_putc('\\');
        host_buf_putc(c);
    }
    host_buf_putc('"');
}

void host_buf_mark(int32_t on) {
    if (!host_color())
        return;
    host_buf_puts(on ? "\033[7m" : "\033[0m");
}

void host_buf_puts_slot(int32_t slot) {
    if (!slot_ok(slot))
        return;
    host_buf_put(g_slot[slot], g_slot_len[slot]);
}

int64_t host_buf_len(void) { return g_buf_len; }

const char *host_buf_ptr(void) {
    if (!g_buf)
        return "";
    return g_buf;
}

int32_t host_buf_contains(const char *lit) {
    return host_contains(host_buf_ptr(), host_buf_len(), lit);
}

int32_t host_slot_set(int32_t slot, const char *s, int64_t n) {
    if (!slot_ok(slot))
        return 0;
    if (!s || n < 0)
        n = 0;
    if (!s)
        s = "";
    if (n >= SLOT_CAP)
        n = SLOT_CAP - 1;
    memcpy(g_slot[slot], s, (size_t)n);
    g_slot[slot][n] = 0;
    g_slot_len[slot] = n;
    return 1;
}

int32_t host_slot_set_cstr(int32_t slot, const char *s) {
    if (!s)
        s = "";
    return host_slot_set(slot, s, (int64_t)strlen(s));
}

void host_slot_clear(int32_t slot) { host_slot_set(slot, "", 0); }

const char *host_slot_ptr(int32_t slot) {
    if (!slot_ok(slot) || g_slot_len[slot] <= 0)
        return "";
    return g_slot[slot];
}

int64_t host_slot_len(int32_t slot) {
    if (!slot_ok(slot) || g_slot_len[slot] < 0)
        return 0;
    return g_slot_len[slot];
}

int32_t host_slot_eq(int32_t slot, const char *lit) {
    return host_streq(host_slot_ptr(slot), host_slot_len(slot), lit);
}

void host_slot_copy(int32_t dst, int32_t src) {
    if (!slot_ok(dst) || !slot_ok(src))
        return;
    if (dst == src)
        return;
    host_slot_set(dst, g_slot[src], g_slot_len[src]);
}

int32_t host_slot_i32(int32_t slot, int32_t fallback) {
    const char *s = host_slot_ptr(slot);
    if (!s || !s[0])
        return fallback;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s)
        return fallback;
    return (int32_t)v;
}

int32_t host_slot_byte(int32_t slot, int64_t i) {
    return host_byte(host_slot_ptr(slot), host_slot_len(slot), i);
}

static int is_sep(char c) { return c == '/' || c == '\\'; }

static void normalize_slot(int32_t slot) {
    if (!slot_ok(slot))
        return;
    for (int64_t i = 0; i < g_slot_len[slot]; i++) {
        if (g_slot[slot][i] == '\\')
            g_slot[slot][i] = '/';
    }
}

int32_t host_join_cstr(int32_t base, const char *rel, int32_t dest) {
    if (!slot_ok(base) || !slot_ok(dest))
        return 0;
    if (!rel)
        rel = "";
    char tmp[SLOT_CAP];
    int64_t bl = g_slot_len[base];
    if (bl < 0)
        bl = 0;
    if (bl >= SLOT_CAP)
        bl = SLOT_CAP - 1;
    memcpy(tmp, g_slot[base], (size_t)bl);
    tmp[bl] = 0;
    size_t len = (size_t)bl;
    size_t rl = strlen(rel);
    int rel_abs = rl > 0 && (rel[0] == '/' || rel[0] == '\\' ||
                             (rl > 2 && isalpha((unsigned char)rel[0]) && rel[1] == ':'));
    if (rel_abs) {
        return host_slot_set(dest, rel, (int64_t)rl);
    }
    if (len > 0 && !is_sep(tmp[len - 1]) && rl > 0) {
        if (len + 1 < sizeof(tmp))
            tmp[len++] = '/';
    }
    if (len + rl >= sizeof(tmp))
        rl = sizeof(tmp) - len - 1;
    memcpy(tmp + len, rel, rl);
    len += rl;
    tmp[len] = 0;
    return host_slot_set(dest, tmp, (int64_t)len);
}

int32_t host_slot_parent(int32_t slot) {
    if (!slot_ok(slot))
        return 0;
    normalize_slot(slot);
    int64_t n = g_slot_len[slot];
    while (n > 0 && g_slot[slot][n - 1] == '/')
        n--;
    int64_t i = n;
    while (i > 0 && g_slot[slot][i - 1] != '/')
        i--;
    if (i <= 0)
        return 0;
    int64_t keep = i - 1;
    if (keep == 2 && isalpha((unsigned char)g_slot[slot][0]) && g_slot[slot][1] == ':')
        keep = 3;
    if (keep <= 0)
        return 0;
    g_slot[slot][keep] = 0;
    g_slot_len[slot] = keep;
    return 1;
}

void host_basename_to(const char *s, int64_t n, int32_t slot) {
    if (!s)
        s = "";
    if (n < 0)
        n = 0;
    int64_t end = n;
    while (end > 0 && (s[end - 1] == '/' || s[end - 1] == '\\'))
        end--;
    int64_t i = end;
    while (i > 0 && s[i - 1] != '/' && s[i - 1] != '\\')
        i--;
    host_slot_set(slot, s + i, end - i);
}

void host_dirname_to(const char *s, int64_t n, int32_t slot) {
    if (!s)
        s = "";
    if (n < 0)
        n = 0;
    int64_t end = n;
    while (end > 0 && (s[end - 1] == '/' || s[end - 1] == '\\'))
        end--;
    int64_t i = end;
    while (i > 0 && s[i - 1] != '/' && s[i - 1] != '\\')
        i--;
    if (i <= 0) {
        host_slot_set_cstr(slot, ".");
        return;
    }
    int64_t keep = i - 1;
    if (keep <= 0)
        host_slot_set_cstr(slot, s[0] == '/' || s[0] == '\\' ? "/" : ".");
    else
        host_slot_set(slot, s, keep);
}

static int keyword(const char *s) {
    static const char *words[] = {
        "if", "else", "end", "ret", "fn", "pub", "pri", "import", "package",
        "var", "const", "while", "true", "false", "and", "or", "not", "scope",
        "arena", "export", "use", "extern", "shared", "module", "struct", NULL};
    for (int i = 0; words[i]; i++)
        if (strcmp(s, words[i]) == 0)
            return 1;
    return 0;
}

int32_t host_make_ident(const char *s, int64_t n, int32_t slot) {
    char tmp[256];
    int64_t o = 0;
    if (!s)
        n = 0;
    for (int64_t i = 0; i < n && o + 2 < (int64_t)sizeof(tmp); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '-' || c == '.' || c == ' ')
            tmp[o++] = '_';
        else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '_')
            tmp[o++] = (char)c;
    }
    tmp[o] = 0;
    if (o == 0 || (tmp[0] >= '0' && tmp[0] <= '9') || keyword(tmp)) {
        char wrapped[280];
        snprintf(wrapped, sizeof(wrapped), "pkg_%s", tmp);
        return host_slot_set_cstr(slot, wrapped);
    }
    return host_slot_set(slot, tmp, o);
}

void host_slot_concat(int32_t src, const char *suffix, int32_t dest) {
    char tmp[SLOT_CAP];
    const char *a = host_slot_ptr(src);
    if (!suffix)
        suffix = "";
    snprintf(tmp, sizeof(tmp), "%s%s", a, suffix);
    host_slot_set_cstr(dest, tmp);
}

static void list_free_item(Item *it) {
    free(it->data);
    it->data = NULL;
    it->len = 0;
}

void host_list_clear(int32_t id) {
    if (!list_ok(id))
        return;
    for (int i = 0; i < g_list_count[id]; i++)
        list_free_item(&g_list[id][i]);
    g_list_count[id] = 0;
}

int32_t host_list_add(int32_t id, const char *s, int64_t n) {
    if (!list_ok(id))
        return 0;
    if (!s)
        s = "";
    if (n < 0)
        n = 0;
    if (g_list_count[id] >= 4096)
        return 0;
    if (g_list_count[id] + 1 > g_list_cap[id]) {
        int cap = g_list_cap[id] == 0 ? 8 : g_list_cap[id] * 2;
        Item *next = (Item *)realloc(g_list[id], (size_t)cap * sizeof(Item));
        if (!next)
            return 0;
        g_list[id] = next;
        g_list_cap[id] = cap;
    }
    char *copy = xstrdup_n(s, n);
    if (!copy)
        return 0;
    g_list[id][g_list_count[id]].data = copy;
    g_list[id][g_list_count[id]].len = n;
    g_list_count[id]++;
    return 1;
}

int32_t host_list_count(int32_t id) {
    if (!list_ok(id))
        return 0;
    return g_list_count[id];
}

const char *host_list_ptr(int32_t id, int32_t index) {
    if (!list_ok(id) || index < 0 || index >= g_list_count[id] || !g_list[id][index].data)
        return "";
    return g_list[id][index].data;
}

int64_t host_list_len(int32_t id, int32_t index) {
    if (!list_ok(id) || index < 0 || index >= g_list_count[id])
        return 0;
    return g_list[id][index].len;
}

int32_t host_list_index(int32_t id, const char *s, int64_t n) {
    if (!list_ok(id))
        return -1;
    for (int i = 0; i < g_list_count[id]; i++) {
        if (g_list[id][i].len == n && (n == 0 || memcmp(g_list[id][i].data, s, (size_t)n) == 0))
            return i;
    }
    return -1;
}

void host_list_set(int32_t id, int32_t index, const char *s, int64_t n) {
    if (!list_ok(id) || index < 0 || index >= g_list_count[id])
        return;
    char *copy = xstrdup_n(s ? s : "", n < 0 ? 0 : n);
    if (!copy)
        return;
    free(g_list[id][index].data);
    g_list[id][index].data = copy;
    g_list[id][index].len = n < 0 ? 0 : n;
}

static const char *base_of(const char *s) {
    const char *b = s;
    for (const char *p = s; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    return b;
}

int32_t host_list_has_basename(int32_t id, const char *base) {
    if (!base)
        return 0;
    for (int i = 0; i < host_list_count(id); i++) {
        if (strcmp(base_of(host_list_ptr(id, i)), base) == 0)
            return 1;
    }
    return 0;
}

int32_t host_getenv(const char *name, int32_t slot) {
    if (!name)
        return 0;
#ifdef _WIN32
    char buf[SLOT_CAP];
    DWORD n = GetEnvironmentVariableA(name, buf, SLOT_CAP);
    if (n == 0 || n >= SLOT_CAP)
        return 0;
    return host_slot_set(slot, buf, (int64_t)n);
#else
    const char *v = getenv(name);
    if (!v || !v[0])
        return 0;
    return host_slot_set_cstr(slot, v);
#endif
}

int32_t host_which(const char *name, int32_t slot) {
    if (!name || !name[0])
        return 0;
#ifdef _WIN32
    char buf[SLOT_CAP];
    DWORD n = SearchPathA(NULL, name, NULL, SLOT_CAP, buf, NULL);
    if (n == 0 || n >= SLOT_CAP) {
        char exe[512];
        snprintf(exe, sizeof(exe), "%s.exe", name);
        n = SearchPathA(NULL, exe, NULL, SLOT_CAP, buf, NULL);
    }
    if (n == 0 || n >= SLOT_CAP)
        return 0;
    return host_slot_set(slot, buf, (int64_t)strlen(buf));
#else
    if (strchr(name, '/')) {
        if (access(name, X_OK) == 0)
            return host_slot_set_cstr(slot, name);
        return 0;
    }
    const char *path = getenv("PATH");
    if (!path)
        return 0;
    char tmp[SLOT_CAP];
    const char *p = path;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        if (len + strlen(name) + 2 >= sizeof(tmp))
            break;
        memcpy(tmp, p, len);
        tmp[len] = 0;
        if (len == 0)
            snprintf(tmp, sizeof(tmp), "%s", name);
        else
            snprintf(tmp, sizeof(tmp), "%s/%s", tmp, name);
        if (access(tmp, X_OK) == 0)
            return host_slot_set_cstr(slot, tmp);
        if (!colon)
            break;
        p = colon + 1;
    }
    return 0;
#endif
}

int32_t host_getcwd(int32_t slot) {
    char buf[SLOT_CAP];
#ifdef _WIN32
    DWORD n = GetCurrentDirectoryA(SLOT_CAP, buf);
    if (n == 0 || n >= SLOT_CAP)
        return 0;
#else
    if (!getcwd(buf, sizeof(buf)))
        return 0;
#endif
    return host_slot_set_cstr(slot, buf);
}

int32_t host_exe_dir(int32_t slot) {
    char buf[SLOT_CAP];
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, buf, SLOT_CAP);
    if (n == 0 || n >= SLOT_CAP)
        return 0;
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0)
        return 0;
    buf[n] = 0;
#endif
    host_slot_set_cstr(slot, buf);
    return host_slot_parent(slot);
}

static int attrs_ok(const char *path, int want_dir) {
    if (!path || !path[0])
        return 0;
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES)
        return 0;
    int dir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (want_dir == 1)
        return dir;
    if (want_dir == 0)
        return !dir;
    return 1;
#else
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    if (want_dir == 1)
        return S_ISDIR(st.st_mode);
    if (want_dir == 0)
        return S_ISREG(st.st_mode);
    return 1;
#endif
}

int32_t host_exists(const char *path) { return attrs_ok(path, 2); }
int32_t host_is_file(const char *path) { return attrs_ok(path, 0); }
int32_t host_is_dir(const char *path) { return attrs_ok(path, 1); }

int32_t host_mkdirs(const char *path) {
    if (!path || !path[0])
        return 0;
    char tmp[SLOT_CAP];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t n = strlen(tmp);
    for (size_t i = 0; i < n; i++)
        if (tmp[i] == '\\')
            tmp[i] = '/';
    size_t start = 0;
    if (n >= 2 && isalpha((unsigned char)tmp[0]) && tmp[1] == ':')
        start = 2;
    for (size_t i = start; i <= n; i++) {
        if (tmp[i] != '/' && tmp[i] != 0)
            continue;
        if (i == start)
            continue;
        char save = tmp[i];
        tmp[i] = 0;
        if (tmp[0] != 0 && !host_is_dir(tmp)) {
#ifdef _WIN32
            if (_mkdir(tmp) != 0 && !host_is_dir(tmp)) {
                tmp[i] = save;
                return 0;
            }
#else
            if (mkdir(tmp, 0777) != 0 && !host_is_dir(tmp)) {
                tmp[i] = save;
                return 0;
            }
#endif
        }
        tmp[i] = save;
    }
    return host_is_dir(path) || host_is_dir(tmp);
}

int32_t host_dir_state(const char *path) {
    if (!path || !path[0] || !host_exists(path))
        return 2;
    if (!host_is_dir(path))
        return -1;
#ifdef _WIN32
    char pattern[SLOT_CAP];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 1;
    int any = 0;
    do {
        if (strcmp(fd.cFileName, ".") != 0 && strcmp(fd.cFileName, "..") != 0)
            any = 1;
    } while (!any && FindNextFileA(h, &fd));
    FindClose(h);
    return any ? 0 : 1;
#else
    DIR *d = opendir(path);
    if (!d)
        return 0;
    int any = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
            any = 1;
            break;
        }
    }
    closedir(d);
    return any ? 0 : 1;
#endif
}

static int remove_tree_inner(const char *path) {
    if (!host_exists(path))
        return 1;
    if (!host_is_dir(path)) {
#ifdef _WIN32
        return DeleteFileA(path) ? 1 : 0;
#else
        return unlink(path) == 0 ? 1 : 0;
#endif
    }
#ifdef _WIN32
    char pattern[SLOT_CAP];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
                continue;
            char child[SLOT_CAP];
            snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName);
            remove_tree_inner(child);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryA(path) ? 1 : 0;
#else
    DIR *d = opendir(path);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d))) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            char child[SLOT_CAP];
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            remove_tree_inner(child);
        }
        closedir(d);
    }
    return rmdir(path) == 0 ? 1 : 0;
#endif
}

int32_t host_remove_tree(const char *path) { return remove_tree_inner(path); }

static int skip_dir_name(const char *name) {
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strcmp(name, ".git") == 0 ||
           strcmp(name, "node_modules") == 0 || strcmp(name, "target") == 0 ||
           strcmp(name, "rust") == 0;
}

static void delete_artifacts_inner(const char *dir) {
#ifdef _WIN32
    char pattern[SLOT_CAP];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        char child[SLOT_CAP];
        snprintf(child, sizeof(child), "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!skip_dir_name(fd.cFileName) && strcmp(fd.cFileName, "build") != 0 &&
                strcmp(fd.cFileName, "bin") != 0)
                delete_artifacts_inner(child);
        } else if (host_ends(fd.cFileName, (int64_t)strlen(fd.cFileName), ".ll") ||
                   host_ends(fd.cFileName, (int64_t)strlen(fd.cFileName), ".o") ||
                   host_ends(fd.cFileName, (int64_t)strlen(fd.cFileName), ".obj")) {
            DeleteFileA(child);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char child[SLOT_CAP];
        snprintf(child, sizeof(child), "%s/%s", dir, ent->d_name);
        if (host_is_dir(child)) {
            if (!skip_dir_name(ent->d_name) && strcmp(ent->d_name, "build") != 0 &&
                strcmp(ent->d_name, "bin") != 0)
                delete_artifacts_inner(child);
        } else if (host_ends(ent->d_name, (int64_t)strlen(ent->d_name), ".ll") ||
                   host_ends(ent->d_name, (int64_t)strlen(ent->d_name), ".o") ||
                   host_ends(ent->d_name, (int64_t)strlen(ent->d_name), ".obj")) {
            unlink(child);
        }
    }
    closedir(d);
#endif
}

int32_t host_delete_artifacts(const char *root) {
    if (!root || !host_is_dir(root))
        return 0;
    delete_artifacts_inner(root);
    return 1;
}

static int list_dir_into(const char *dir, int32_t list, const char *suffix, const char *prefix,
                         int recurse) {
#ifdef _WIN32
    char pattern[SLOT_CAP];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        int dirent = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        char rel[SLOT_CAP];
        if (prefix && prefix[0])
            snprintf(rel, sizeof(rel), "%s/%s", prefix, fd.cFileName);
        else
            snprintf(rel, sizeof(rel), "%s", fd.cFileName);
        if (dirent) {
            if (recurse && !skip_dir_name(fd.cFileName)) {
                char child[SLOT_CAP];
                snprintf(child, sizeof(child), "%s\\%s", dir, fd.cFileName);
                list_dir_into(child, list, suffix, rel, 1);
            }
        } else if (!suffix || host_ends(fd.cFileName, (int64_t)strlen(fd.cFileName), suffix)) {
            host_list_add(list, rel, (int64_t)strlen(rel));
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 1;
#else
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char child[SLOT_CAP];
        snprintf(child, sizeof(child), "%s/%s", dir, ent->d_name);
        char rel[SLOT_CAP];
        if (prefix && prefix[0])
            snprintf(rel, sizeof(rel), "%s/%s", prefix, ent->d_name);
        else
            snprintf(rel, sizeof(rel), "%s", ent->d_name);
        if (host_is_dir(child)) {
            if (recurse && !skip_dir_name(ent->d_name))
                list_dir_into(child, list, suffix, rel, 1);
        } else if (!suffix || host_ends(ent->d_name, (int64_t)strlen(ent->d_name), suffix)) {
            host_list_add(list, rel, (int64_t)strlen(rel));
        }
    }
    closedir(d);
    return 1;
#endif
}

int32_t host_list_dir(const char *dir, int32_t list) {
    host_list_clear(list);
    if (!dir || !host_is_dir(dir))
        return 0;
    return list_dir_into(dir, list, NULL, "", 0);
}

int32_t host_walk_suffix(const char *dir, const char *suffix, int32_t list) {
    host_list_clear(list);
    if (!dir || !host_is_dir(dir))
        return 0;
    const char *prefix = dir;
    if (dir[0] == '/' || dir[0] == '\\' || (strlen(dir) > 2 && dir[1] == ':'))
        prefix = base_of(dir);
    return list_dir_into(dir, list, suffix, prefix, 1);
}

int64_t host_file_size(const char *path) {
    if (!path || !host_is_file(path))
        return -1;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info))
        return -1;
    return ((int64_t)info.nFileSizeHigh << 32) | (int64_t)info.nFileSizeLow;
#else
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return (int64_t)st.st_size;
#endif
}

const char *host_host_triple(void) {
#ifdef _WIN32
    return "x86_64-pc-windows-gnu";
#elif defined(__APPLE__)
    return "arm64-apple-darwin";
#else
    return "x86_64-unknown-linux-gnu";
#endif
}

const char *host_exe_suffix(void) {
#ifdef _WIN32
    return ".exe";
#else
    return "";
#endif
}

void host_tick(void) {
#ifdef _WIN32
    g_tick = GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_tick = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
#endif
}

int64_t host_tock(void) {
#ifdef _WIN32
    return (int64_t)(GetTickCount64() - g_tick);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
    return (int64_t)(now - g_tick);
#endif
}

static int valid_chars(const char *s, int64_t n, int allow_dot) {
    if (!s || n <= 0)
        return 0;
    unsigned char c0 = (unsigned char)s[0];
    if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_'))
        return 0;
    for (int64_t i = 1; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '_';
        if (allow_dot && (c == '.' || c == '-'))
            ok = 1;
        if (!allow_dot && (c == '+' || c == '-' || c == '.'))
            ok = 0;
        if (!ok)
            return 0;
    }
    return 1;
}

int32_t host_valid_name(const char *s, int64_t n) { return valid_chars(s, n, 1); }

int32_t host_valid_ident(const char *s, int64_t n) {
    if (!valid_chars(s, n, 0))
        return 0;
    char tmp[256];
    if (n >= (int64_t)sizeof(tmp))
        return 0;
    memcpy(tmp, s, (size_t)n);
    tmp[n] = 0;
    return keyword(tmp) ? 0 : 1;
}

int32_t host_valid_lib(const char *s, int64_t n) {
    if (!s || n <= 0)
        return 0;
    for (int64_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '_' || c == '+' || c == '.' || c == '-';
        if (!ok)
            return 0;
    }
    return 1;
}

int32_t host_unquote_slot(int32_t src, int32_t dest) {
    const char *s = host_slot_ptr(src);
    int64_t n = host_slot_len(src);
    if (n < 2 || s[0] != '"')
        return 0;
    char tmp[SLOT_CAP];
    int64_t o = 0;
    int64_t i = 1;
    int closed = 0;
    while (i < n) {
        char c = s[i++];
        if (c == '"') {
            closed = 1;
            break;
        }
        if (c == '\\' && i < n)
            c = s[i++];
        if (o + 1 >= SLOT_CAP)
            return 0;
        tmp[o++] = c;
    }
    if (!closed)
        return 0;
    return host_slot_set(dest, tmp, o);
}

int32_t host_split_eq(const char *p, int64_t n, int32_t key_slot, int32_t value_slot) {
    int64_t off = host_trim_off(p, n);
    int64_t len = host_trim_len(p, n);
    if (len <= 0) {
        host_slot_clear(key_slot);
        host_slot_clear(value_slot);
        return 0;
    }
    const char *s = p + off;
    int64_t eq = -1;
    for (int64_t i = 0; i < len; i++) {
        if (s[i] == '=') {
            eq = i;
            break;
        }
    }
    if (eq < 0) {
        host_slot_set(key_slot, s, len);
        host_slot_clear(value_slot);
        return 2;
    }
    int64_t koff = host_trim_off(s, eq);
    int64_t klen = host_trim_len(s, eq);
    const char *vs = s + eq + 1;
    int64_t vn = len - (eq + 1);
    int64_t voff = host_trim_off(vs, vn);
    int64_t vlen = host_trim_len(vs, vn);
    host_slot_set(key_slot, s + koff, klen);
    host_slot_set(value_slot, vs + voff, vlen);
    return 1;
}

void host_stem(const char *path, int64_t n, int32_t slot) {
    host_basename_to(path, n, slot);
    const char *b = host_slot_ptr(slot);
    int64_t len = host_slot_len(slot);
    int64_t dot = -1;
    for (int64_t i = 0; i < len; i++)
        if (b[i] == '.')
            dot = i;
    if (dot > 0)
        host_slot_set(slot, b, dot);
}

void host_strip_lib_name(int32_t slot) {
    char tmp[SLOT_CAP];
    snprintf(tmp, sizeof(tmp), "%s", host_slot_ptr(slot));
    size_t n = strlen(tmp);
    const char *exts[] = {".dll", ".dylib", ".lib", ".obj", ".so", ".o", ".a", NULL};
    for (int i = 0; exts[i]; i++) {
        size_t m = strlen(exts[i]);
        if (n > m && icmp_eq(tmp + n - m, exts[i])) {
            tmp[n - m] = 0;
            n -= m;
            break;
        }
    }
    if (n > 3 && (tmp[0] == 'l' || tmp[0] == 'L') && (tmp[1] == 'i' || tmp[1] == 'I') &&
        (tmp[2] == 'b' || tmp[2] == 'B')) {
        memmove(tmp, tmp + 3, n - 2);
    }
    host_slot_set_cstr(slot, tmp);
}

static int icmp_eq(const char *a, const char *b) {
#ifdef _WIN32
    return _stricmp(a, b) == 0;
#else
    return strcasecmp(a, b) == 0;
#endif
}

void host_cmd_begin(void) {
    for (int i = 0; i < g_cmd_n; i++) {
        free(g_cmd[i]);
        g_cmd[i] = NULL;
        g_cmd_len[i] = 0;
    }
    g_cmd_n = 0;
    g_cmd_cwd[0] = 0;
}

void host_cmd_arg(const char *s, int64_t n) {
    if (g_cmd_n >= CMD_N - 1)
        return;
    if (!s)
        s = "";
    if (n < 0)
        n = 0;
    g_cmd[g_cmd_n] = xstrdup_n(s, n);
    g_cmd_len[g_cmd_n] = n;
    if (g_cmd[g_cmd_n])
        g_cmd_n++;
}

void host_cmd_arg_cstr(const char *s) {
    if (!s)
        s = "";
    host_cmd_arg(s, (int64_t)strlen(s));
}

void host_cmd_arg_slot(int32_t slot) { host_cmd_arg(host_slot_ptr(slot), host_slot_len(slot)); }

void host_cmd_cwd_slot(int32_t slot) {
    snprintf(g_cmd_cwd, sizeof(g_cmd_cwd), "%s", host_slot_ptr(slot));
}

static char *quote_cmdline(void) {
    size_t cap = 256;
    char *out = (char *)malloc(cap);
    if (!out)
        return NULL;
    size_t len = 0;
    out[0] = 0;
    for (int a = 0; a < g_cmd_n; a++) {
        const char *s = g_cmd[a] ? g_cmd[a] : "";
        int quote = s[0] == 0;
        for (const char *p = s; *p; p++) {
            if (*p == ' ' || *p == '\t' || *p == '"')
                quote = 1;
        }
        size_t need = strlen(s) * 2 + 4;
        if (len + need + 2 >= cap) {
            cap = (len + need + 2) * 2;
            char *next = (char *)realloc(out, cap);
            if (!next) {
                free(out);
                return NULL;
            }
            out = next;
        }
        if (a) {
            out[len++] = ' ';
            out[len] = 0;
        }
        if (!quote) {
            memcpy(out + len, s, strlen(s));
            len += strlen(s);
            out[len] = 0;
            continue;
        }
        out[len++] = '"';
        int bs = 0;
        for (const char *p = s; *p; p++) {
            if (*p == '\\') {
                bs++;
                continue;
            }
            if (*p == '"') {
                for (int i = 0; i < bs * 2 + 1; i++)
                    out[len++] = '\\';
                out[len++] = '"';
                bs = 0;
                continue;
            }
            for (int i = 0; i < bs; i++)
                out[len++] = '\\';
            bs = 0;
            out[len++] = *p;
        }
        for (int i = 0; i < bs * 2; i++)
            out[len++] = '\\';
        out[len++] = '"';
        out[len] = 0;
    }
    return out;
}

static void capture_set(const char *s, int64_t n) {
    free(g_captured);
    g_captured = xstrdup_n(s ? s : "", n < 0 ? 0 : n);
    g_captured_len = g_captured ? (n < 0 ? 0 : n) : 0;
}

static void capture_add(const char *s, int64_t n) {
    if (!s || n <= 0)
        return;
    char *next = (char *)realloc(g_captured, (size_t)g_captured_len + (size_t)n + 1);
    if (!next)
        return;
    g_captured = next;
    memcpy(g_captured + g_captured_len, s, (size_t)n);
    g_captured_len += n;
    g_captured[g_captured_len] = 0;
}

int32_t host_cmd_run(int32_t capture) {
    if (g_cmd_n < 1)
        return 127;
    capture_set("", 0);
#ifdef _WIN32
    char *line = quote_cmdline();
    if (!line)
        return 127;
    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wr = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    if (capture) {
        if (!CreatePipe(&rd, &wr, &sa, 0)) {
            free(line);
            return 127;
        }
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = wr;
        si.hStdError = wr;
    }
    BOOL ok = CreateProcessA(NULL, line, NULL, NULL, TRUE, 0, NULL,
                             g_cmd_cwd[0] ? g_cmd_cwd : NULL, &si, &pi);
    free(line);
    if (!ok) {
        if (rd)
            CloseHandle(rd);
        if (wr)
            CloseHandle(wr);
        return 127;
    }
    if (wr)
        CloseHandle(wr);
    if (rd) {
        char tmp[4096];
        DWORD got = 0;
        while (ReadFile(rd, tmp, sizeof(tmp), &got, NULL) && got > 0)
            capture_add(tmp, (int64_t)got);
        CloseHandle(rd);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int32_t)code;
#else
    int pipefd[2] = {-1, -1};
    if (capture && pipe(pipefd) != 0)
        return 127;
    pid_t pid = fork();
    if (pid < 0)
        return 127;
    if (pid == 0) {
        if (g_cmd_cwd[0])
            chdir(g_cmd_cwd);
        if (capture) {
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
        }
        char *args[CMD_N + 1];
        for (int i = 0; i < g_cmd_n; i++)
            args[i] = g_cmd[i];
        args[g_cmd_n] = NULL;
        execvp(args[0], args);
        _exit(127);
    }
    if (capture)
        close(pipefd[1]);
    if (capture) {
        char tmp[4096];
        ssize_t got = 0;
        while ((got = read(pipefd[0], tmp, sizeof(tmp))) > 0)
            capture_add(tmp, (int64_t)got);
        close(pipefd[0]);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 1;
#endif
}

const char *host_captured_ptr(void) { return g_captured ? g_captured : ""; }
int64_t host_captured_len(void) { return g_captured ? g_captured_len : 0; }

#ifdef _WIN32
static DWORD WINAPI spin_thread(LPVOID arg) {
    (void)arg;
    const char *frames[10] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    int i = 0;
    while (InterlockedCompareExchange(&g_spin_on, 1, 1) == 1) {
        fprintf(stderr, "\r[%s] %s", frames[i], g_spin_msg);
        fflush(stderr);
        i = (i + 1) % 10;
        Sleep(80);
    }
    return 0;
}
#endif

void host_flush(void) {
    fflush(stdout);
    fflush(stderr);
}

void host_spin_start(const char *msg) {
    host_init();
    if (!msg)
        msg = "";
    snprintf(g_spin_msg, sizeof(g_spin_msg), "%s", msg);
    if (!g_tty || getenv("NO_COLOR") != NULL) {
        fprintf(stderr, "%s\n", g_spin_msg);
        fflush(stderr);
#ifdef _WIN32
        g_spin_on = 0;
#endif
        return;
    }
#ifdef _WIN32
    g_spin_on = 1;
    g_spin_thread = CreateThread(NULL, 0, spin_thread, NULL, 0, NULL);
    if (!g_spin_thread) {
        g_spin_on = 0;
        fprintf(stderr, "%s\n", g_spin_msg);
    }
#else
    fprintf(stderr, "%s\n", g_spin_msg);
    fflush(stderr);
#endif
}

void host_spin_stop(void) {
#ifdef _WIN32
    if (!g_spin_on)
        return;
    InterlockedExchange(&g_spin_on, 0);
    if (g_spin_thread) {
        WaitForSingleObject(g_spin_thread, INFINITE);
        CloseHandle(g_spin_thread);
        g_spin_thread = NULL;
    }
    int width = (int)strlen(g_spin_msg) + 8;
    fprintf(stderr, "\r%*s\r", width, "");
    fflush(stderr);
#endif
}

int32_t host_read_key(void) {
    host_init();
    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    int c = _getch();
    if (c == 0 || c == 224) {
        int n = _getch();
        if (n == 72)
            return 1;
        if (n == 80)
            return 2;
        return 0;
    }
    if (c == '\r' || c == '\n')
        return 3;
    if (c == 'q' || c == 'Q' || c == 27)
        return 4;
    if (c >= '1' && c <= '9')
        return 10 + (c - '1');
    return 0;
#else
    char buf[8];
    if (read(STDIN_FILENO, buf, 1) != 1)
        return 3;
    if (buf[0] == '\033') {
        if (read(STDIN_FILENO, buf, 2) == 2) {
            if (buf[0] == '[' && buf[1] == 'A')
                return 1;
            if (buf[0] == '[' && buf[1] == 'B')
                return 2;
        }
        return 4;
    }
    if (buf[0] == '\n' || buf[0] == '\r')
        return 3;
    if (buf[0] == 'q' || buf[0] == 'Q')
        return 4;
    if (buf[0] >= '1' && buf[0] <= '9')
        return 10 + (buf[0] - '1');
    return 0;
#endif
}

void host_cursor_up(int32_t lines) {
    if (!host_is_tty() || lines <= 0)
        return;
    fprintf(stdout, "\033[%dA", lines);
    fflush(stdout);
}

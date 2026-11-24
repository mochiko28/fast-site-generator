#include "core.h"

#define ARENA_SIZE (384UL << 20)
#define ALLOC_LIMIT (ARENA_SIZE - 16)
#define OUTPUT_LIMIT (128UL << 20)
#define ENV_LIMIT (2UL << 20)

static char *arena;
static size used;
static void *failure[5];
int latin1;

enum LinuxCall {
    SYS_READ = 0, SYS_WRITE = 1, SYS_OPEN = 2, SYS_CLOSE = 3, SYS_FSTAT = 5,
    SYS_MMAP = 9, SYS_MUNMAP = 11, SYS_GETPID = 39, SYS_RENAME = 82, SYS_UNLINK = 87,
    SYS_EXIT_GROUP = 231,
};

static inline long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

static long map_arena(void) {
    long r;
    register long flags __asm__("r10") = 0x22;  // MAP_PRIVATE | MAP_ANONYMOUS
    register long fd __asm__("r8") = -1;
    register long offset __asm__("r9") = 0;
    __asm__ volatile ("syscall" : "=a"(r) : "a"((long)SYS_MMAP), "D"(0L), "S"(ARENA_SIZE), "d"(3L),
                      "r"(flags), "r"(fd), "r"(offset) : "rcx", "r11", "memory");
    return r;
}

static long open_file(const char *path, long flags, long mode) { return sys3(SYS_OPEN, (long)path, flags, mode); }
static long close_file(long fd) { return sys3(SYS_CLOSE, fd, 0, 0); }

_Noreturn void fail(void) {
    __builtin_longjmp(failure, 1);
}

void *alloc(size element, size count) {
    if (count && element > (ALLOC_LIMIT - used) / count) fail();
    size bytes = element * count;
    size aligned = (bytes + 7) & ~7UL;
    if (aligned > ALLOC_LIMIT - used) fail();
    void *result = arena + used;
    used += aligned;
    return result;
}

typedef long Word __attribute__((aligned(1), may_alias));
typedef uint Half __attribute__((aligned(1), may_alias));
typedef unsigned short Pair __attribute__((aligned(1), may_alias));
typedef char Vector __attribute__((vector_size(16), aligned(1), may_alias));

void copy(void *dst, const void *src, size n) {
    char *d = dst;
    const char *s = src;
    while (n >= 16) {
        *(Vector *)d = *(const Vector *)s;
        d += 16;
        s += 16;
        n -= 16;
    }
    if (n >= 8) {
        *(Word *)d = *(const Word *)s;
        *(Word *)(d + n - 8) = *(const Word *)(s + n - 8);
    } else if (n >= 4) {
        *(Half *)d = *(const Half *)s;
        *(Half *)(d + n - 4) = *(const Half *)(s + n - 4);
    } else if (n >= 2) {
        *(Pair *)d = *(const Pair *)s;
        d[n - 1] = s[n - 1];
    } else if (n) {
        *d = *s;
    }
}

void *zero(void *dst, size n) {
    char *d = dst;
    while (n >= 16) {
        *(Vector *)d = (Vector){0};
        d += 16;
        n -= 16;
    }
    for (size i = 0; i < n; i++) d[i] = 0;
    return dst;
}

// GCC can emit these for struct and array operations even in freestanding mode.
__attribute__((used, externally_visible)) void *memcpy(void *d, const void *s, size n) { copy(d, s, n); return d; }
__attribute__((used, externally_visible)) void *memset(void *d, int c, size n) {
    if (!c) return zero(d, n);
    for (size i = 0; i < n; i++) ((char *)d)[i] = (char)c;
    return d;
}

char *grow(Buf *buf, size extra) {
    if (buf->p == arena || extra > ARENA_SIZE - buf->n) fail();
    size need = buf->n + extra;
    size cap = buf->cap ? buf->cap * 2 : 128;
    if (cap < need) cap = need;
    size aligned = (buf->cap + 7) & ~7UL;
    if (buf->p && buf->p + aligned == arena + used) {
        used -= aligned;
        alloc(1, cap);
    } else {
        char *p = NEW(char, cap);
        copy(p, buf->p, buf->n);
        buf->p = p;
    }
    buf->cap = cap;
    return buf->p + buf->n;
}

Str take(const Buf *buf) { return (Str){buf->p, buf->n}; }

Str join(size count, const Str *parts) {
    size n = 0;
    for (size i = 0; i < count; i++) {
        if (parts[i].n > ARENA_SIZE - n) fail();
        n += parts[i].n;
    }
    char *p = NEW(char, n);
    size off = 0;
    for (size i = 0; i < count; i++) {
        copy(p + off, parts[i].p, parts[i].n);
        off += parts[i].n;
    }
    return (Str){p, n};
}

Str number(size n) {
    char digits[24];
    size i = sizeof(digits);
    do {
        digits[--i] = '0' + n % 10;
        n /= 10;
    } while (n);
    return JOIN(((Str){digits + i, sizeof(digits) - i}));
}

size decimal(Str s) {
    size n = 0;
    for (size i = 0; i < s.n; i++) n = n * 10 + s.p[i] - '0';
    return n;
}

size find_char(Str s, size i, char c) {
    Vector needle = (Vector){c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c};
    for (; i < s.n && s.n - i >= 16; i += 16) {
        int mask = __builtin_ia32_pmovmskb128(*(const Vector *)(s.p + i) == needle);
        if (mask) return i + __builtin_ctz((uint)mask);
    }
    for (; i < s.n; i++) if (s.p[i] == c) return i;
    return NPOS;
}

int equal(Str a, Str b) {
    if (a.n != b.n) return 0;
    size n = a.n, i = 0;
    for (; n - i >= 8; i += 8)
        if (*(const Word *)(a.p + i) != *(const Word *)(b.p + i)) return 0;
    for (; i < n; i++) if (a.p[i] != b.p[i]) return 0;
    return 1;
}

int starts(Str s, Str prefix) { return s.n >= prefix.n && equal(slice(s, 0, prefix.n), prefix); }
int ends(Str s, Str suffix) { return s.n >= suffix.n && equal(tail(s, s.n - suffix.n), suffix); }

size find_str(Str s, size pos, Str needle) {
    if (!needle.n) return pos <= s.n ? pos : NPOS;
    while ((pos = find_char(s, pos, needle.p[0])) != NPOS) {
        if (starts(tail(s, pos), needle)) return pos;
        pos++;
    }
    return NPOS;
}

static size hash(Str s) {
    size h = 0x84222325cbf29ce4UL;
    size i = 0;
    for (; s.n - i >= 8; i += 8) h = (h ^ (size)*(const Word *)(s.p + i)) * 0x100000001b3UL;
    for (; i < s.n; i++) h = (h ^ (byte)s.p[i]) * 0x100000001b3UL;
    return (h ^ (h >> 32)) | 1;
}

static Entry *slot(const Map *map, Str key, size h) {
    size mask = map->cap - 1;
    size i = h & mask;
    while (map->entries[i].hash) {
        Entry *e = &map->entries[i];
        if (e->hash == h && equal(key, e->key)) return e;
        i = (i + 1) & mask;
    }
    return &map->entries[i];
}

void *get(const Map *map, Str key) {
    return map->cap ? slot(map, key, hash(key))->val : 0;
}

void set(Map *map, Str key, void *val) {
    if (map->used * 2 >= map->cap) {
        Entry *old = map->entries;
        size count = map->cap;
        map->cap = count ? count * 2 : 16;
        map->entries = zero(NEW(Entry, map->cap), map->cap * sizeof(Entry));
        for (size i = 0; i < count; i++)
            if (old[i].hash) *slot(map, old[i].key, old[i].hash) = old[i];
    }
    size h = hash(key);
    Entry *e = slot(map, key, h);
    map->used += !e->hash;
    *e = (Entry){key, val, h};
}

static void merge_sort(void **items, void **tmp, size n, int (*order)(const void *, const void *)) {
    if (n < 2) return;
    size middle = n / 2;
    merge_sort(items, tmp, middle, order);
    merge_sort(items + middle, tmp, n - middle, order);
    if (order(items[middle - 1], items[middle]) <= 0) return;
    size a = 0, b = middle, k = 0;
    while (a < middle && b < n)
        tmp[k++] = order(items[a], items[b]) <= 0 ? items[a++] : items[b++];
    while (a < middle) tmp[k++] = items[a++];
    while (b < n) tmp[k++] = items[b++];
    copy(items, tmp, n * sizeof(void *));
}

void sort(void **items, size n, int (*order)(const void *, const void *)) {
    if (n > 1) merge_sort(items, NEW(void *, n), n, order);
}

size checkpoint(void) { return used; }
void rewind_arena(size mark) { used = mark; }

static int read_file(long fd, char *bytes, size count) {
    while (count) {
        long n = sys3(SYS_READ, fd, (long)bytes, count);
        if (n == -4) continue;  // EINTR
        if (n <= 0) return 0;
        bytes += n;
        count -= n;
    }
    return 1;
}

static Str read_input(const char *path) {
    long fd = open_file(path, 0, 0);
    if (fd < 0) fail();

    // Linux x86-64 struct stat, including st_mode and st_size.
    struct {
        unsigned long dev, ino, nlink;
        uint mode, uid, gid, pad;
        unsigned long rdev;
        long length, remaining[11];
    } info;
    if (sys3(SYS_FSTAT, fd, (long)&info, 0) < 0 || (info.mode & 0170000) != 0100000
        || info.length < 0 || (size)info.length >= ALLOC_LIMIT - used) {
        close_file(fd);
        fail();
    }
    char *data = NEW(char, (size)info.length + 1);
    int ok = read_file(fd, data, info.length);
    char extra;
    if (ok) ok = sys3(SYS_READ, fd, (long)&extra, 1) == 0;
    close_file(fd);
    if (!ok) fail();
    return (Str){data, info.length};
}

static void paths(Str *input, Str *output) {
    long fd = open_file("/proc/self/environ", 0, 0);
    if (fd < 0) fail();
    char *bytes = NEW(char, ENV_LIMIT);
    size pos = 0;
    long n;
    do {
        n = sys3(SYS_READ, fd, (long)(bytes + pos), ENV_LIMIT - pos);
        if (n == -4) continue;
        if (n > 0) pos += n;
    } while (n && (n > 0 || n == -4) && pos < ENV_LIMIT);
    close_file(fd);
    if (n || (pos && bytes[pos - 1])) fail();

    Str in = S("/input"), out = S("/output"), env = {bytes, pos};
    int have_in = 0, have_out = 0;
    for (size i = 0; i < pos;) {
        size end = find_char(env, i, 0);
        Str field = slice(env, i, end);
        if (!have_in && starts(field, S("MAKEFUN_INPUT_DIR="))) {
            in = tail(field, S("MAKEFUN_INPUT_DIR=").n);
            have_in = 1;
        } else if (!have_out && starts(field, S("MAKEFUN_OUTPUT_DIR="))) {
            out = tail(field, S("MAKEFUN_OUTPUT_DIR=").n);
            have_out = 1;
        }
        i = end + 1;
    }
    validate_utf8(in);
    validate_utf8(out);
    *input = JOIN(in, S("/cases.json\0"));
    *output = JOIN(out, S("/answers.json\0"));
}

static int publish(Str path, const Buf *out) {
    // Publish only a completed file. A failed write leaves the final path intact.
    Str temporary = JOIN(slice(path, 0, path.n - 1), S(".native-"),
                         number(sys3(SYS_GETPID, 0, 0, 0)), S("\0"));
    long fd = open_file(temporary.p, 1 | 64 | 128, 0666);  // O_WRONLY | O_CREAT | O_EXCL
    if (fd < 0) return 0;
    size pos = 0;
    while (pos < out->n) {
        long wrote = sys3(SYS_WRITE, fd, (long)(out->p + pos), out->n - pos);
        if (wrote == -4) continue;
        if (wrote <= 0) break;
        pos += wrote;
    }
    if (close_file(fd) == 0 && pos == out->n
        && sys3(SYS_RENAME, (long)temporary.p, (long)path.p, 0) == 0) return 1;
    sys3(SYS_UNLINK, (long)temporary.p, 0, 0);
    return 0;
}

static void job(void) {
    Str input, output;
    paths(&input, &output);
    Str source = read_input(input.p);
    Buf answer = {arena, 0, OUTPUT_LIMIT};
    build_answers(&answer, source);
    if (!publish(output, &answer)) fail();
    sys3(SYS_EXIT_GROUP, 0, 0, 0);
    __builtin_trap();
}

__attribute__((visibility("default")))
void *napi_register_module_v1(void *env, void *exports) {
    (void)env;
    long address = map_arena();
    if ((size)address >= (size)-4095) return exports;
    arena = (char *)address;
    used = OUTPUT_LIMIT;
    if (!__builtin_setjmp(failure)) job();
    sys3(SYS_MUNMAP, address, ARENA_SIZE, 0);
    return exports;
}

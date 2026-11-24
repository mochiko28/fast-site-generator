#include "core.h"

#define PAGE_SIZE 4096UL
#define LARGE_PAGE (2UL << 20)
#define ENV_LIMIT LARGE_PAGE
#define PATH_LIMIT PAGE_SIZE
#define ENV_WINDOW (2 * LARGE_PAGE)
#define SCRATCH_LIMIT (256UL << 20)
#define ALLOC_LIMIT (SCRATCH_LIMIT - 16)
#ifndef SITE_OUTPUT_CAPACITY
#define SITE_OUTPUT_CAPACITY (64UL << 10)
#endif
#define INCLUDE_CACHE_LIMIT (32UL << 10)
#define HTML_LIMIT SCRATCH_LIMIT
#define RESERVED_SIZE (LARGE_PAGE + ENV_WINDOW + SCRATCH_LIMIT + HTML_LIMIT + SITE_OUTPUT_CAPACITY)

_Static_assert(SCRATCH_LIMIT <= (uint)-1, "map key lengths fit in uint");

typedef struct {
    char *base;
    size mapped;
} Window;

static Window scratch, html;
static size used, html_used, include_cache_used;
static Buf answer;
static struct {
    Str path;
    long fd;
    size written, old_length;
    char temporary[PATH_LIMIT + 32];
} stream;
static void *failure[5];
int latin1;

static void flush_answer(void);

enum LinuxCall {
    SYS_READ = 0, SYS_WRITE = 1, SYS_OPEN = 2, SYS_CLOSE = 3, SYS_FSTAT = 5,
    SYS_MMAP = 9, SYS_MPROTECT = 10, SYS_MUNMAP = 11, SYS_GETPID = 39,
    SYS_FLOCK = 73, SYS_FTRUNCATE = 77, SYS_RENAME = 82, SYS_UNLINK = 87,
    SYS_EXIT_GROUP = 231,
};

static inline long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

enum MapFlag { MAP_PRIVATE = 2, MAP_FIXED = 16, MAP_ANONYMOUS = 32, MAP_POPULATE = 0x8000 };

static long map_memory(char *address, size length, long protection, long flags) {
    long r;
    register long map_flags __asm__("r10") = flags;
    register long fd __asm__("r8") = -1;
    register long offset __asm__("r9") = 0;
    __asm__ volatile ("syscall" : "=a"(r) : "a"((long)SYS_MMAP), "D"(address), "S"(length), "d"(protection),
                      "r"(map_flags), "r"(fd), "r"(offset) : "rcx", "r11", "memory");
    return r;
}

static size round_up(size n, size alignment) {
    return (n + alignment - 1) & ~(alignment - 1);
}

static int extend(Window *window, size length, int populate) {
    if (length <= window->mapped) return 1;
    char *base = window->base + window->mapped;
    if (populate) {
        if (map_memory(base, length - window->mapped, 3,
                       MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS | MAP_POPULATE) != (long)base) return 0;
    } else if (sys3(SYS_MPROTECT, (long)base, length - window->mapped, 3) < 0) {
        return 0;
    }
    window->mapped = length;
    return 1;
}

static int fit(Window *window, size needed, size limit) {
    if (needed <= window->mapped) return 1;
    size length = window->mapped * 2;
    if (length < needed) length = needed;
    length = round_up(length, length < LARGE_PAGE ? PAGE_SIZE : LARGE_PAGE);
    return extend(window, length < limit ? length : limit, 0);
}

static long open_file(const char *path, long flags, long mode) { return sys3(SYS_OPEN, (long)path, flags, mode); }
static long close_file(long fd) { return sys3(SYS_CLOSE, fd, 0, 0); }

_Noreturn void fail(void) {
    __builtin_longjmp(failure, 1);
}

void *alloc(size element, size count) {
    size bytes;
    if (__builtin_mul_overflow(element, count, &bytes) || bytes > ALLOC_LIMIT - used) fail();
    size aligned = (bytes + 7) & ~7UL;
    if (aligned > ALLOC_LIMIT - used) fail();
    if (__builtin_expect(used + aligned + 16 > scratch.mapped, 0)
        && !fit(&scratch, used + aligned + 16, SCRATCH_LIMIT)) fail();
    void *result = scratch.base + used;
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
    // The JSON decoder also uses this with a destination below its source.
    if (n >= 128) {
        __asm__ volatile ("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
        return;
    }
    while (n >= 16) {
        *(Vector *)d = *(const Vector *)s;
        d += 16;
        s += 16;
        n -= 16;
    }
    if (n >= 8) {
        Word first = *(const Word *)s, last = *(const Word *)(s + n - 8);
        *(Word *)d = first;
        *(Word *)(d + n - 8) = last;
    } else if (n >= 4) {
        Half first = *(const Half *)s, last = *(const Half *)(s + n - 4);
        *(Half *)d = first;
        *(Half *)(d + n - 4) = last;
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
    if (buf == &answer) {
        if (extra > buf->cap) fail();
        flush_answer();
        return buf->p;
    }
    if (extra > SCRATCH_LIMIT - buf->n) fail();
    size need = buf->n + extra;
    if (buf->p == html.base + html_used) {
        if (need > HTML_LIMIT - html_used || !fit(&html, html_used + need, HTML_LIMIT)) fail();
        buf->cap = html.mapped - html_used;
        return buf->p + buf->n;
    }
    size cap = buf->cap ? buf->cap * 2 : 128;
    if (cap < need) cap = need;
    size aligned = (buf->cap + 7) & ~7UL;
    if (buf->p && buf->p + aligned == scratch.base + used) {
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

void append_more(Buf *buf, Str str) {
    if (buf == &answer) {
        while (str.n > buf->cap - buf->n) {
            size filled = buf->cap - buf->n;
            copy(buf->p + buf->n, str.p, filled);
            buf->n = buf->cap;
            flush_answer();
            str = tail(str, filled);
        }
    }
    copy(room(buf, str.n), str.p, str.n);
    buf->n += str.n;
}

size answer_position(const Buf *out) { return stream.written + out->n; }

Str cache_answer(const Buf *out, size start) {
    // A flush may have overwritten the beginning of this include.
    if (start < stream.written) return (Str){0};
    Str text = tail(take(out), start - stream.written);
    if (text.n > INCLUDE_CACHE_LIMIT - include_cache_used) return (Str){0};
    include_cache_used += text.n;
    return text.n ? JOIN(text) : S("");
}

Str take(const Buf *buf) { return (Str){buf->p, buf->n}; }

Str join(size count, const Str *parts) {
    size n = 0;
    for (size i = 0; i < count; i++) {
        if (parts[i].n > SCRATCH_LIMIT - n) fail();
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

Str format_number(char *end, size n) {
    char *first = end;
    do {
        *--first = '0' + n % 10;
        n /= 10;
    } while (n);
    return (Str){first, end - first};
}

Str number(size n) {
    char digits[20];
    return JOIN(format_number(digits + sizeof(digits), n));
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
        if (mask) return i + first_bit((uint)mask);
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

static uint hash(Str s) {
    size h = 0x84222325cbf29ce4UL;
    size i = 0;
    for (; s.n - i >= 8; i += 8) h = (h ^ (size)*(const Word *)(s.p + i)) * 0x100000001b3UL;
    for (; i < s.n; i++) h = (h ^ (byte)s.p[i]) * 0x100000001b3UL;
    uint folded = h ^ (h >> 32);
    return folded ? folded : 1;
}

static Entry *slot(const Map *map, Str key, uint h) {
    size mask = map->cap - 1;
    size i = h & mask;
    while (map->entries[i].hash) {
        Entry *e = &map->entries[i];
        if (e->hash == h && equal(key, (Str){e->key, e->length})) return e;
        i = (i + 1) & mask;
    }
    return &map->entries[i];
}

void *get(const Map *map, Str key) {
    return map->cap ? slot(map, key, hash(key))->val : 0;
}

void reserve_map(Map *map, size count) {
    if (count <= map->cap / 2) return;
    if (count > ALLOC_LIMIT / sizeof(Entry) / 2) fail();
    size cap = map->cap ? map->cap * 2 : 16;
    while (count > cap / 2) cap *= 2;
    Entry *entries = NEW(Entry, cap);
    zero(entries, cap * sizeof(*entries));
    Map old = *map;
    map->cap = cap;
    map->entries = entries;
    for (size i = 0; i < old.cap; i++) {
        const Entry *e = &old.entries[i];
        if (e->hash) *slot(map, (Str){e->key, e->length}, e->hash) = *e;
    }
}

void **entry_value(Map *map, Str key) {
    uint h = hash(key);
    if (!map->cap) reserve_map(map, 1);
    Entry *e = slot(map, key, h);
    if (!e->hash) {
        if (map->used == map->cap / 2) {
            reserve_map(map, map->used + 1);
            e = slot(map, key, h);
        }
        map->used++;
        *e = (Entry){key.p, 0, key.n, h};
    }
    return &e->val;
}

void set(Map *map, Str key, void *val) {
    *entry_value(map, key) = val;
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
    if (n < 2) return;
    size mark = used;
    merge_sort(items, NEW(void *, n), n, order);
    used = mark;
}

size checkpoint(void) { return used; }

void rewind_arena(size mark) {
    used = mark;
    html_used = 0;
    include_cache_used = 0;
}

// Heading labels and excerpts keep slices of this buffer, so growth must preserve
// its address. Commit only the bytes rendered before starting another page.
Buf start_html(void) { return (Buf){html.base + html_used, 0, html.mapped - html_used}; }
void finish_html(const Buf *out) { html_used += round_up(out->n, 8); }
size html_checkpoint(void) { return html_used; }

Str retain_html(size mark, Str text) {
    char *dst = html.base + mark;
    if (text.n && dst != text.p) copy(dst, text.p, text.n);
    html_used = mark + round_up(text.n, 8);
    return (Str){dst, text.n};
}

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

// Linux x86-64 struct stat.
typedef struct {
    unsigned long dev, ino, nlink;
    uint mode, uid, gid, pad;
    unsigned long rdev;
    long length, remaining[11];
} FileStatus;

static Str read_input(const char *path) {
    long fd = open_file(path, 0, 0);
    if (fd < 0) fail();
    FileStatus info;
    if (sys3(SYS_FSTAT, fd, (long)&info, 0) < 0 || (info.mode & 0170000) != 0100000
        || info.length < 0 || (size)info.length > ALLOC_LIMIT - 16) {
        close_file(fd);
        fail();
    }
    size length = info.length;
    size scratch_size = round_up(length * 2 + 65536, length >= (1UL << 20) ? LARGE_PAGE : PAGE_SIZE);
    if (scratch_size > SCRATCH_LIMIT) scratch_size = SCRATCH_LIMIT;
    // This whole prefix will be written by read(), and its boundary preserves THP alignment.
    (void)extend(&scratch, length & ~(LARGE_PAGE - 1), 1);
    if (!extend(&scratch, scratch_size, 0)) {
        close_file(fd);
        fail();
    }
    // Leave room for the parser's last SIMD load at EOF.
    used = round_up(length + 16, 8);
    char *data = scratch.base;
    int ok = read_file(fd, data, length);
    char extra;
    if (ok) ok = sys3(SYS_READ, fd, (long)&extra, 1) == 0;
    close_file(fd);
    if (!ok) fail();
    return (Str){data, length};
}

static void paths(Str *input, Str *output) {
    long fd = open_file("/proc/self/environ", 0, 0);
    if (fd < 0) fail();
    Window environment = {scratch.base - ENV_WINDOW, 0};
    size capacity = PAGE_SIZE, pos = 0;
    char *bytes = environment.base;
    long n = -1;
    while (extend(&environment, capacity, 0)) {
        do {
            n = sys3(SYS_READ, fd, (long)(bytes + pos), capacity - pos);
            if (n == -4) continue;
            if (n > 0) pos += n;
        } while (n && (n > 0 || n == -4) && pos < capacity);
        if (n <= 0 || capacity == ENV_LIMIT) break;
        capacity *= 2;
    }
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
    Str input_suffix = S("/cases.json\0"), output_suffix = S("/answers.json\0");
    size input_size = in.n + input_suffix.n, output_size = out.n + output_suffix.n;
    if (input_size > PATH_LIMIT || output_size > PATH_LIMIT) fail();
    // Keep both paths after the environment bytes, usually in the same page.
    if (!extend(&environment, round_up(pos + input_size + output_size, PAGE_SIZE), 0)) fail();
    char *input_path = bytes + pos;
    char *output_path = input_path + input_size;
    copy(input_path, in.p, in.n);
    copy(input_path + in.n, input_suffix.p, input_suffix.n);
    copy(output_path, out.p, out.n);
    copy(output_path + out.n, output_suffix.p, output_suffix.n);
    *input = (Str){input_path, input_size};
    *output = (Str){output_path, output_size};
}

static long open_direct_answer(void) {
    // Serialize native writers and reuse the existing regular file's cached pages.
    long fd = open_file(stream.path.p, 1 | 64 | 2048 | 131072, 0666);  // O_WRONLY | O_CREAT | O_NONBLOCK | O_NOFOLLOW
    if (fd < 0) return fd;
    FileStatus info;
    if (sys3(SYS_FSTAT, fd, (long)&info, 0) < 0 || (info.mode & 0170000) != 0100000 || info.nlink != 1) {
        close_file(fd);
        return -1;
    }
    long locked;
    do locked = sys3(SYS_FLOCK, fd, 2, 0); while (locked == -4);  // LOCK_EX
    // Another native writer can change the size before this writer gets the lock.
    if (locked < 0 || sys3(SYS_FSTAT, fd, (long)&info, 0) < 0 || info.nlink != 1) {
        close_file(fd);
        return -1;
    }
    stream.old_length = info.length;
    return fd;
}

static void open_answer(void) {
    PHASE_BEGIN(previous, OPEN_OUTPUT);
    stream.fd = open_direct_answer();
    if (stream.fd < 0) {
        // Keep replacement semantics for symlinks and multiply linked files.
        size n = stream.path.n - 1;
        Str suffix = S(".native-");
        char digits[20];
        Str pid = format_number(digits + sizeof(digits), sys3(SYS_GETPID, 0, 0, 0));
        copy(stream.temporary, stream.path.p, n);
        copy(stream.temporary + n, suffix.p, suffix.n);
        copy(stream.temporary + n + suffix.n, pid.p, pid.n);
        stream.temporary[n + suffix.n + pid.n] = 0;
        stream.fd = open_file(stream.temporary, 1 | 64 | 128, 0666);  // O_WRONLY | O_CREAT | O_EXCL
        if (stream.fd < 0) {
            stream.temporary[0] = 0;
            fail();
        }
    }
    PHASE_END(previous);
}

static void flush_answer(void) {
    if (!answer.n) return;
    if (stream.fd < 0) open_answer();
    PHASE_BEGIN(previous, WRITE);
    size pos = 0;
    while (pos < answer.n) {
        long wrote = sys3(SYS_WRITE, stream.fd, (long)(answer.p + pos), answer.n - pos);
        if (wrote == -4) continue;
        if (wrote <= 0) fail();
        pos += wrote;
    }
    stream.written += answer.n;
    answer.n = 0;
    PHASE_END(previous);
}

static void finish_answer(void) {
    flush_answer();
    if (stream.old_length > stream.written) {
        PHASE(TRUNCATE_OUTPUT);
        if (sys3(SYS_FTRUNCATE, stream.fd, stream.written, 0) != 0) fail();
    }
    PHASE(CLOSE_OUTPUT);
    long closed = close_file(stream.fd);
    stream.fd = -1;
    if (closed != 0) fail();
    if (stream.temporary[0]) {
        PHASE(RENAME_OUTPUT);
        if (sys3(SYS_RENAME, (long)stream.temporary, (long)stream.path.p, 0) != 0) fail();
        stream.temporary[0] = 0;
    }
}

#ifdef SITE_PROFILE
// Linux x86-64 struct rusage uses two timevals followed by fourteen longs.
typedef struct {
    long user[2], system[2], maxrss, ixrss, idrss, isrss, minor, major, rest[8];
} Usage;

static const Str phase_names[] = {
    S("setup"), S("input"), S("parse"), S("front"), S("markdown"),
    S("templates"), S("slugs"), S("serialize"), S("open_output"), S("write"),
    S("truncate_output"), S("close_output"), S("rename_output"),
};
_Static_assert(COUNT(phase_names) == PHASE_COUNT, "phase names");

static size cycles[PHASE_COUNT], cpu[PHASE_COUNT];
static size last_cycle, last_cpu, cpu_start, startup_cpu;
static enum Phase current_phase;
static Usage usage_start;
static int usage_ok;

static size cpu_time(long clock) {
    long ts[2];
    if (sys3(228, clock, (long)ts, 0) < 0) return 0;  // clock_gettime
    return ts[0] * 1000000000UL + ts[1];
}

static size ticks(void) {
    uint lo, hi;
    __asm__ volatile ("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((size)hi << 32) | lo;
}

enum Phase profile_phase(enum Phase next) {
    size now = ticks();
    enum Phase previous = current_phase;
    cycles[previous] += now - last_cycle;
    last_cycle = now;
    current_phase = next;
    return previous;
}

void profile_stage(enum Phase next) {
    if (current_phase == next) return;
    profile_phase(next);
    size now = cpu_time(3);
    size total = 0;
    for (size i = 0; i < PHASE_COUNT; i++) total += cycles[i];
    // Subdivide each stage's CPU time using TSC for its short nested phases.
    for (size i = 0; i < PHASE_COUNT; i++) {
        if (!cycles[i]) continue;
        cpu[i] += (now - last_cpu) * ((double)cycles[i] / total);
        cycles[i] = 0;
    }
    last_cpu = now;
    last_cycle = ticks();
}

static void profile_start(void) {
    startup_cpu = cpu_time(2);  // CLOCK_PROCESS_CPUTIME_ID
    usage_ok = sys3(98, 0, (long)&usage_start, 0) == 0;  // getrusage(RUSAGE_SELF)
    current_phase = SETUP;
    zero(cycles, sizeof(cycles));
    zero(cpu, sizeof(cpu));
    last_cpu = cpu_start = cpu_time(3);  // CLOCK_THREAD_CPUTIME_ID
    last_cycle = ticks();
}

static void profile_field(Buf *out, Str key, size value) {
    append(out, key);
    put(out, '=');
    append(out, number(value));
    put(out, ' ');
}

static void profile_finish(void) {
    PHASE(SETUP);
    size elapsed = cpu_time(3) - cpu_start;
    Usage end;
    int have_usage = usage_ok && sys3(98, 0, (long)&end, 0) == 0;
    Buf out = {0};
    profile_field(&out, S("native_ns"), elapsed);
    profile_field(&out, S("startup_process_ns"), startup_cpu);
    if (have_usage) {
        profile_field(&out, S("minflt"), end.minor - usage_start.minor);
        profile_field(&out, S("majflt"), end.major - usage_start.major);
    }
    for (size i = 0; i < PHASE_COUNT; i++)
        profile_field(&out, phase_names[i], cpu[i] / 1000);
    put(&out, '\n');
    sys3(SYS_WRITE, 2, (long)out.p, out.n);
}
#endif

static void job(void) {
    Str input;
    paths(&input, &stream.path);
    PHASE(INPUT);
    Str source = read_input(input.p);
    size alignment = source.n >= (1UL << 20) ? LARGE_PAGE : PAGE_SIZE;
    size html_size = round_up(source.n + 65536, alignment);
    if (!extend(&html, html_size < HTML_LIMIT ? html_size : HTML_LIMIT, 0) ||
        sys3(SYS_MPROTECT, (long)answer.p, answer.cap, 3) < 0) fail();
    build_answers(&answer, source);
    finish_answer();
#ifdef SITE_PROFILE
    profile_finish();
#endif
    sys3(SYS_EXIT_GROUP, 0, 0, 0);
    __builtin_trap();
}

__attribute__((visibility("default")))
void *napi_register_module_v1(void *env, void *exports) {
    (void)env;
#ifdef SITE_PROFILE
    profile_start();
#endif
    // Reserve addresses so arena slices stay valid as input-sized windows grow.
    long address = map_memory(0, RESERVED_SIZE, 0, MAP_PRIVATE | MAP_ANONYMOUS);
    if ((size)address >= (size)-4095) return exports;
    scratch = (Window){(char *)round_up(address, LARGE_PAGE) + ENV_WINDOW, 0};
    html = (Window){scratch.base + SCRATCH_LIMIT, 0};
    answer = (Buf){html.base + HTML_LIMIT, 0, SITE_OUTPUT_CAPACITY};
    used = html_used = include_cache_used = 0;
    stream.fd = -1;
    stream.written = stream.old_length = 0;
    stream.temporary[0] = 0;
    if (!__builtin_setjmp(failure)) job();
    // Returning from registration runs the JS driver, which truncates any partial answer.
    if (stream.fd >= 0) close_file(stream.fd);
    if (stream.temporary[0]) sys3(SYS_UNLINK, (long)stream.temporary, 0, 0);
    sys3(SYS_MUNMAP, address, RESERVED_SIZE, 0);
    return exports;
}

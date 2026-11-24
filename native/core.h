#ifndef SITE_CORE_H
#define SITE_CORE_H

typedef unsigned char byte;
typedef unsigned int uint;
typedef unsigned long size;
// Strings carry UTF-8 bytes. In UTF-16 mode an isolated surrogate uses WTF-8.
typedef struct { const char *p; size n; } Str;
typedef struct { char *p; size n, cap; } Buf;

#define S(literal) ((Str){literal, sizeof(literal) - 1})
#define NPOS ((size)-1)
#define COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define NEW(type, n) ((type *)alloc(sizeof(type), (n)))
#define ADD(buffer, literal) do { \
    Buf *add_target = (buffer); \
    size add_length = sizeof(literal) - 1; \
    __builtin_memcpy(room(add_target, add_length), (literal), add_length); \
    add_target->n += add_length; \
} while (0)

extern int latin1;

_Noreturn void fail(void);
void *alloc(size element, size count);
void *zero(void *dst, size n);
void copy(void *dst, const void *src, size n);
char *grow(Buf *buf, size extra);
static inline char *room(Buf *buf, size extra) {
    if (extra <= buf->cap - buf->n) return buf->p + buf->n;
    return grow(buf, extra);
}
static inline void append(Buf *buf, Str str) {
    if (!str.n) return;
    copy(room(buf, str.n), str.p, str.n);
    buf->n += str.n;
}
static inline void put(Buf *buf, char c) {
    *room(buf, 1) = c;
    buf->n++;
}
Buf start_html(void);
void finish_html(const Buf *out);
Str take(const Buf *buf);
Str join(size count, const Str *parts);
#define JOIN(...) join(COUNT(((Str[]){__VA_ARGS__})), (Str[]){__VA_ARGS__})
Str format_number(char *end, size n);
Str number(size n);
size decimal(Str s);
size find_char(Str s, size at, char c);
size find_str(Str s, size at, Str needle);
int equal(Str a, Str b);
int starts(Str s, Str prefix);
int ends(Str s, Str suffix);
int compare(Str a, Str b);

static inline Str slice(Str s, size a, size b) { return (Str){s.p ? s.p + a : 0, b - a}; }
static inline Str tail(Str s, size a) { return slice(s, a, s.n); }
static inline int at(Str s, size i) { return i < s.n ? (byte)s.p[i] : -1; }
static inline int digit(int c) { return c >= '0' && c <= '9'; }
static inline int letter(int c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
static inline int lower_digit(int c) { return (c >= 'a' && c <= 'z') || digit(c); }
static inline int indent(int c) { return c == ' ' || c == '\t'; }
// The caller supplies a nonzero mask. Use the baseline BSF encoding.
static inline uint first_bit(uint mask) {
    uint index;
    __asm__("bsfl %1, %0" : "=r"(index) : "r"(mask) : "cc");
    return index;
}

typedef struct { Str key; void *val; size hash; } Entry;
typedef struct { Entry *entries; size used, cap; } Map;
void reserve_map(Map *map, size count);
void *get(const Map *map, Str key);
void **entry_value(Map *map, Str key);
void set(Map *map, Str key, void *val);
void sort(void **items, size n, int (*order)(const void *, const void *));

uint decode(Str s, size *i);
void encode(Buf *out, uint cp);
void validate_utf8(Str s);
size space_width(Str s, size i);
int has_space(Str s);
int has_line_separator(Str s);
Str trim(Str s);
Str trim_line(Str s);
int only_indent(Str s);
void json(Buf *out, Str s, int html, int escape_pairs);
void escape_rendered(Buf *out, Str s);
void json_code_unit(Buf *out, uint cp);
void json_ascii(Buf *out, int c);
Str slugify(Str s);
Str unique_slug(Map *used, Str slug);

enum ValueKind { NIL, STRING, NUMBER, BOOLEAN, ARRAY, RECORD, RENDERED };
typedef struct Value Value;
enum Property { PROP_UNKNOWN,
    PROP_BASE_URL, PROP_CONTENT, PROP_COUNT, PROP_DATE, PROP_DATE_LONG, PROP_EXCERPT,
    PROP_FIRST, PROP_HTML, PROP_ID, PROP_INDEX, PROP_IS_POST, PROP_LAST,
    PROP_LENGTH, PROP_LEVEL, PROP_NAME, PROP_NEWER, PROP_NEWER_URL, PROP_NUMBER,
    PROP_OLDER, PROP_OLDER_URL, PROP_PAGE, PROP_PAGER, PROP_POSTS, PROP_READING_MINUTES,
    PROP_RECENT, PROP_SITE, PROP_SLUG, PROP_SUMMARY, PROP_TAG, PROP_TAGS,
    PROP_TITLE, PROP_TOC, PROP_TOTAL, PROP_URL
};
enum RecordLayout {
    PAGE_FIELDS, TAG_FIELDS, HEADING_FIELDS, LOOP_FIELDS, PAGER_FIELDS,
    INDEX_FIELDS, SITE_FIELDS, GLOBAL_FIELDS, PAGE_ROOT, TAG_ROOT
};
struct Value {
    enum ValueKind kind;
    union {
        Str text;
        size number;
        struct { const Value **items; size n; } array;
        struct { const Value *const *items; uint n; enum RecordLayout layout; } record;
    };
};

static inline Value string(Str s) { return (Value){.kind = STRING, .text = s}; }
static inline Value numeric(size n) { return (Value){.kind = NUMBER, .number = n}; }
static inline Value boolean(int n) { return (Value){.kind = BOOLEAN, .number = n}; }
static inline Value array(const Value **items, size n) {
    return (Value){.kind = ARRAY, .array = {items, n}};
}
static inline Value record(enum RecordLayout layout, const Value *const *items, uint n) {
    return (Value){.kind = RECORD, .record = {items, n, layout}};
}

enum Property property_id(Str name);
const Value *property(const Value *value, enum Property name);
static inline Value rendered(Str escaped) { return (Value){.kind = RENDERED, .text = escaped}; }

typedef struct Node Node;
typedef struct Memo Memo;
typedef struct Op Op;
typedef struct {
    union { Node *nodes; Op *ops; };
    Str error;
    Memo *memo[9];
} Template;
Template parse_template(Str source);
void prepare_template(Template *template, const Map *templates);
void render_template(Buf *out, const Template *template, const Value *root, const Value *globals);

typedef struct {
    Value html, excerpt, toc;
    size minutes;
} Markdown;
typedef Str (*ResolveLink)(void *context, Str target);
Markdown markdown(Str body, ResolveLink resolve, void *context);

size checkpoint(void);
void rewind_arena(size mark);
void build_answers(Buf *out, Str input);

enum Phase {
    SETUP, INPUT, PARSE, FRONT, MARKDOWN, TEMPLATES, SLUGS, SERIALIZE,
    OPEN_OUTPUT, WRITE, TRUNCATE_OUTPUT, CLOSE_OUTPUT, RENAME_OUTPUT, PHASE_COUNT
};
#ifdef SITE_PROFILE
void profile_stage(enum Phase next);
enum Phase profile_phase(enum Phase next);
#define PHASE(next) profile_stage(next)
#define PHASE_BEGIN(name, next) enum Phase name = profile_phase(next)
#define PHASE_END(name) profile_phase(name)
#else
#define PHASE(next) ((void)0)
#define PHASE_BEGIN(name, next) ((void)0)
#define PHASE_END(name) ((void)0)
#endif

#endif

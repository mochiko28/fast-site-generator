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
Str take(const Buf *buf);
Str join(size count, const Str *parts);
#define JOIN(...) join(COUNT(((Str[]){__VA_ARGS__})), (Str[]){__VA_ARGS__})
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

typedef struct { Str key; void *val; size hash; } Entry;
typedef struct { Entry *entries; size used, cap; } Map;
void *get(const Map *map, Str key);
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
typedef struct { Str name; const Value *value; } Field;
struct Value {
    enum ValueKind kind;
    union {
        Str text;
        size number;
        struct { const Value **items; size n; } array;
        struct { const Field *items; size n; } record;
    };
};

static inline Value string(Str s) { return (Value){.kind = STRING, .text = s}; }
static inline Value numeric(size n) { return (Value){.kind = NUMBER, .number = n}; }
static inline Value boolean(int n) { return (Value){.kind = BOOLEAN, .number = n}; }
static inline Value array(const Value **items, size n) {
    return (Value){.kind = ARRAY, .array = {items, n}};
}
static inline Value record(const Field *items, size n) {
    return (Value){.kind = RECORD, .record = {items, n}};
}

const Value *property(const Value *value, Str name);
static inline Value rendered(Str escaped) { return (Value){.kind = RENDERED, .text = escaped}; }

typedef struct Node Node;
typedef struct Memo Memo;
typedef struct { Node *nodes; Str error; Memo *memo[9]; } Template;
Template parse_template(Str source);
void prepare_template(Template template, const Map *templates);
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

#endif

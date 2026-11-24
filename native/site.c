#include "core.h"

enum JsonKind { JSTRING, JNUMBER, JARRAY, JOBJECT, JNULL, JBOOLEAN };

typedef struct Json Json;
// The input window bounds every JSON slice and stays at one address during parsing.
typedef struct { uint start, length; } InputSlice;
struct Json {
    Json *next;
    union {
        InputSlice text;
        Json *child;
    };
    InputSlice key;
    uint count;
    byte kind, has_cr;
};

static const char *input_base;

static inline InputSlice input_slice(Str s) {
    return (InputSlice){s.p ? s.p - input_base : 0, s.n};
}
static inline Str read_slice(InputSlice s) {
    return (Str){input_base + s.start, s.length};
}

#define INPUT_CHUNK (128UL << 10)

typedef struct {
    Str source;
    size pos, loaded, remaining;
    unsigned encoding_flags;
    size utf8_overlap;  // Leading bytes validated by the previous refill.
} Parser;
typedef char Vec __attribute__((vector_size(16), aligned(1), may_alias));
#define SPLAT(c) ((Vec){c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c})

static int json_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static void *encoding_restart[5];
static int has_unicode_escape(Str input, unsigned *flags);

static __attribute__((noinline)) int refill(Parser *p, size i) {
    while (i >= p->source.n && p->remaining) {
        size n = p->remaining < INPUT_CHUNK ? p->remaining : INPUT_CHUNK;
        PHASE_BEGIN(previous, INPUT);
        char *bytes = (char *)p->source.p + p->source.n;
        size following = p->remaining - n;
        size overlap = latin1 ? 5 : 3;
        size lookahead = following < overlap ? following : overlap;
        read_input(bytes, n + lookahead, p->loaded);
        if (!latin1) {
            // Finish any code point that starts in this block with the lookahead.
            size end = n;
            while (end < n + lookahead && ((byte)bytes[end] & 0xc0) == 0x80) end++;
            validate_utf8((Str){bytes + p->utf8_overlap, end - p->utf8_overlap});
            p->utf8_overlap = end - n;
        } else if (has_unicode_escape((Str){bytes, n + lookahead}, &p->encoding_flags)) {
            __builtin_longjmp(encoding_restart, 1);
        }
        PHASE_END(previous);
        p->source.n += n;
        p->loaded += n;
        p->remaining -= n;
    }
    return i < p->source.n;
}

static inline int parser_at(Parser *p, size i) {
    if (i >= p->source.n && !refill(p, i)) return -1;
    return (byte)p->source.p[i];
}

static inline int peek(Parser *p) {
    size i = p->pos;
    for (;;) {
        Str source = p->source;
        while (i < source.n) {
            int c = (byte)source.p[i];
            if (!json_space(c)) {
                p->pos = i;
                return c;
            }
            i++;
        }
        if (!refill(p, i)) {
            p->pos = i;
            return -1;
        }
    }
}

// Between cases no parsed string refers to the input. Discard completely
// consumed blocks so runs of inter-case whitespace do not grow the window.
static int peek_batch(Parser *p) {
    for (;;) {
        while (p->pos < p->source.n) {
            int c = (byte)p->source.p[p->pos];
            if (!json_space(c)) return c;
            p->pos++;
        }
        p->pos = p->source.n = 0;
        if (!refill(p, 0)) return -1;
    }
}

static void expect(Parser *p, int c) {
    if (peek(p) != c) fail();
    p->pos++;
}

static int hex_value(int c) {
    if (digit(c)) return c - '0';
    c |= 32;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static int hex4(Str input, size pos) {
    int cp = 0;
    for (size j = 0; j < 4; j++) {
        int hex = hex_value(at(input, pos + j));
        if (hex < 0) return -1;
        cp = cp * 16 + hex;
    }
    return cp;
}

static uint string_marks(Vec bytes) {
    Vec hit = ((bytes & SPLAT((char)0xe0)) == SPLAT(0)) | (bytes == SPLAT('"')) | (bytes == SPLAT('\\'));
    return __builtin_ia32_pmovmskb128(hit);
}

static size json_mark(Str input, size i) {
    while (i < input.n) {
        uint mask = string_marks(*(const Vec *)(input.p + i));
        if (mask) {
            size end = i + first_bit(mask);
            return end < input.n ? end : input.n;
        }
        i += 16;
    }
    return input.n;
}

typedef char Wide __attribute__((vector_size(32), aligned(1), may_alias));
#define SPLAT_WIDE(c) ((Wide){c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c})

enum EncodingFlag { LOWERCASE_U = 1, WIDE_ESCAPE = 2 };
static inline int encoding_escape(Str input, size i, unsigned *flags) {
    unsigned lower = input.p[i + 1] == 'u';
    unsigned wide = hex4(input, i + 2) >= 128;
    *flags |= lower | (wide * WIDE_ESCAPE);
    return *flags == (LOWERCASE_U | WIDE_ESCAPE);
}

static __attribute__((target("avx2"), noinline)) size unicode_escape_wide(Str input, unsigned *flags) {
    size i = 0;
    for (; input.n - i >= 33; i += 32) {
        Wide first = *(const Wide *)(input.p + i);
        Wide second = *(const Wide *)(input.p + i + 1);
        uint hits = __builtin_ia32_pmovmskb256((first == SPLAT_WIDE('\\')) &
            ((second | SPLAT_WIDE(32)) == SPLAT_WIDE('u')));
        for (; hits; hits &= hits - 1)
            if (encoding_escape(input, i + first_bit(hits), flags)) return NPOS;
    }
    return i;
}

static int has_unicode_escape(Str input, unsigned *flags) {
    size i = has_avx2 ? unicode_escape_wide(input, flags) : 0;
    if (i == NPOS) return 1;
    for (; input.n - i >= 17; i += 16) {
        Vec first = *(const Vec *)(input.p + i);
        Vec second = *(const Vec *)(input.p + i + 1);
        uint hits = __builtin_ia32_pmovmskb128((first == SPLAT('\\')) &
            ((second | SPLAT(32)) == SPLAT('u')));
        for (; hits; hits &= hits - 1)
            if (encoding_escape(input, i + first_bit(hits), flags)) return 1;
    }
    for (; i + 1 < input.n; i++)
        if (input.p[i] == '\\' && (input.p[i + 1] | 32) == 'u' &&
            encoding_escape(input, i, flags)) return 1;
    return 0;
}

static const unsigned short unescape[256] = {
    ['\\'] = '\\', ['"'] = '"', ['/'] = '/',
    ['b'] = '\b', ['f'] = '\f', ['n'] = '\n', ['r'] = 256 | '\r', ['t'] = '\t',
};

// Shuffle a single escape byte out of a full vector.
static const Vec drop_lane[16] = {
    {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0},
    {0,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0},
    {0,1,3,4,5,6,7,8,9,10,11,12,13,14,15,0},
    {0,1,2,4,5,6,7,8,9,10,11,12,13,14,15,0},
    {0,1,2,3,5,6,7,8,9,10,11,12,13,14,15,0},
    {0,1,2,3,4,6,7,8,9,10,11,12,13,14,15,0},
    {0,1,2,3,4,5,7,8,9,10,11,12,13,14,15,0},
    {0,1,2,3,4,5,6,8,9,10,11,12,13,14,15,0},
    {0,1,2,3,4,5,6,7,9,10,11,12,13,14,15,0},
    {0,1,2,3,4,5,6,7,8,10,11,12,13,14,15,0},
    {0,1,2,3,4,5,6,7,8,9,11,12,13,14,15,0},
    {0,1,2,3,4,5,6,7,8,9,10,12,13,14,15,0},
    {0,1,2,3,4,5,6,7,8,9,10,11,13,14,15,0},
    {0,1,2,3,4,5,6,7,8,9,10,11,12,14,15,0},
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,15,0},
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,0}
};

// Compact up to two simple escapes in a fully loaded vector.
// A zero result leaves the input for the general decoder.
static inline __attribute__((target("avx2"))) int compact_json_vector(
        char *write, const char *read, Vec block, uint hits, uint *cr) {
    if (!hits) {
        *(Vec *)write = block;
        return 16;
    }
    uint n = first_bit(hits);
    if (n == 15 || read[n] != '\\') return 0;
    uint decoded = unescape[(byte)read[n + 1]];
    if (!decoded) return 0;
    uint remaining = hits & (-1u << (n + 2));
    if (remaining) {
        uint m = first_bit(remaining);
        if (m == 15 || read[m] != '\\' || (hits >> (m + 2))) return 0;
        uint second = unescape[(byte)read[m + 1]];
        if (!second) return 0;
        Vec first = __builtin_ia32_pshufb128(block, drop_lane[n + 1]);
        *(Vec *)write = __builtin_ia32_pshufb128(first, drop_lane[m]);
        write[n] = (byte)decoded;
        write[m - 1] = (byte)second;
        *cr |= decoded | second;
        return 14;
    }
    *(Vec *)write = __builtin_ia32_pshufb128(block, drop_lane[n + 1]);
    write[n] = (byte)decoded;
    *cr |= decoded;
    return 15;
}

static __attribute__((target("avx2"), noinline)) size copy_json_wide(
    Str input, size from, char **destination, uint *carriage_return, int *special) {
    char *write = *destination;
    uint cr = *carriage_return;
    const char *bytes = input.p;
    const char *read = bytes + from;
    const char *limit = bytes + input.n;
    while (limit - read >= 16) {
        Vec block = *(const Vec *)read;
        uint hits = string_marks(block);
        int copied = compact_json_vector(write, read, block, hits, &cr);
        if (copied) {
            read += 16;
            write += copied;
            continue;
        }
        uint n = first_bit(hits);
        if (read + n - write >= 16) *(Vec *)write = block;
        else copy(write, read, n);
        write += n;
        read += n;
        if (*read == '\\' && limit - read >= 2) {
            uint decoded = unescape[(byte)read[1]];
            if (decoded) {
                *write++ = (byte)decoded;
                read += 2;
                cr |= decoded;
                continue;
            }
        }
        *special = 1;
        goto done;
    }
    *special = 0;
done:
    *destination = write;
    *carriage_return = cr;
    return read - bytes;
}

static Str read_string(Parser *p, int *has_cr) {
    Str input = p->source;
    size from = ++p->pos;
    size end = json_mark(input, from);
    while (end == input.n && refill(p, end)) {
        input = p->source;
        end = json_mark(input, end);
    }
    if (at(input, end) == '"') {
        p->pos = end + 1;
        return slice(input, from, end);
    }
    // Decode into the input buffer. The read cursor stays ahead of the write cursor.
    char *text = (char *)input.p + from;
    char *write = (char *)input.p + end;
    uint carriage_return = 0;
    for (;;) {
decode_escape:
        if (at(input, end) == '"') break;
        if (at(input, end) != '\\') fail();
        unsigned c = parser_at(p, end + 1);
        input = p->source;
        from = end + 2;
        if (c >= COUNT(unescape)) fail();
        uint decoded = unescape[c];
        if (decoded) {
            *write++ = (byte)decoded;
            carriage_return |= decoded;
        } else {
            if (c != 'u') fail();
            if (end + 11 >= input.n) {
                refill(p, end + 11);
                input = p->source;
            }
            int cp = hex4(input, from);
            if (cp < 0) fail();
            if (cp == '\r') carriage_return |= 256;
            from += 4;
            if (cp >= 0xd800 && cp <= 0xdbff && at(input, from) == '\\' && at(input, from + 1) == 'u') {
                int low = hex4(input, from + 2);
                if (low >= 0xdc00 && low <= 0xdfff) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                    from += 6;
                }
            }
            Buf encoded = {write, 0, 4};
            encode(&encoded, cp);
            write += encoded.n;
        }
        if (has_avx2 && input.n - from >= 16) {
            int special;
            from = copy_json_wide(input, from, &write, &carriage_return, &special);
            if (special) {
                end = from;
                goto decode_escape;
            }
        } else if (input.p + from - write >= 16) {
            // The speculative store stays entirely behind the unread source.
            while (input.n - from >= 16) {
                Vec bytes = *(const Vec *)(input.p + from);
                *(Vec *)write = bytes;
                uint hits = string_marks(bytes);
                if (!hits) {
                    from += 16;
                    write += 16;
                    continue;
                }
                uint n = first_bit(hits);
                end = from + n;
                write += n;
                if (input.p[end] != '\\' || end + 1 == input.n)
                    goto decode_escape;
                uint simple = unescape[(byte)input.p[end + 1]];
                if (!simple) goto decode_escape;
                *write++ = (byte)simple;
                carriage_return |= simple;
                from = end + 2;
            }
        }
        for (;;) {
            if (from >= input.n) {
                if (!refill(p, from)) fail();
                input = p->source;
            }
            // The input arena has a readable SIMD tail.
            Vec bytes = *(const Vec *)(input.p + from);
            uint hits = string_marks(bytes);
            if (hits || input.n - from < 16) {
                size n = hits ? first_bit(hits) : 16;
                if (n > input.n - from) n = input.n - from;
                // Widen the copy only when it stays clear of the unread source.
                if (input.p + from + n - write >= 16) *(Vec *)write = bytes;
                else copy(write, input.p + from, n);
                write += n;
                end = from + n;
                if (end < input.n) break;
                from = end;
                continue;
            }
            *(Vec *)write = bytes;
            write += 16;
            from += 16;
        }
    }
    p->pos = end + 1;
    if (has_cr) *has_cr = carriage_return >> 8;
    return (Str){text, write - text};
}

static Str read_number(Parser *p) {
    size i = p->pos;
    if (parser_at(p, i) == '-') i++;
    if (parser_at(p, i) == '0') {
        i++;
    } else {
        if (!digit(parser_at(p, i))) fail();
        do i++; while (digit(parser_at(p, i)));
    }
    if (parser_at(p, i) == '.') {
        i++;
        if (!digit(parser_at(p, i))) fail();
        do i++; while (digit(parser_at(p, i)));
    }
    int c = parser_at(p, i);
    if (c == 'e' || c == 'E') {
        c = parser_at(p, ++i);
        if (c == '+' || c == '-') i++;
        if (!digit(parser_at(p, i))) fail();
        do i++; while (digit(parser_at(p, i)));
    }
    Str result = slice(p->source, p->pos, i);
    p->pos = i;
    return result;
}

static Json *read_value(Parser *p, uint depth) {
    if (depth > 128) fail();
    int first = peek(p);
    Json *value = NEW(Json, 1);
    value->next = 0;
    if (first == '"') {
        value->kind = JSTRING;
        int has_cr = 0;
        value->text = input_slice(read_string(p, &has_cr));
        value->has_cr = has_cr;
    } else if (first == '[' || first == '{') {
        value->kind = first == '[' ? JARRAY : JOBJECT;
        int close = first == '[' ? ']' : '}';
        p->pos++;
        value->count = 0;
        Json **next = &value->child;
        if (peek(p) != close) {
            for (;;) {
                Str name = {0};
                if (first == '{') {
                    if (peek(p) != '"') fail();
                    name = read_string(p, 0);
                    expect(p, ':');
                }
                Json *item = read_value(p, depth + 1);
                item->key = input_slice(name);
                *next = item;
                next = &item->next;
                value->count++;
                if (peek(p) != ',') break;
                p->pos++;
            }
        }
        *next = 0;
        expect(p, close);
    } else if (first == '-' || digit(first)) {
        value->kind = JNUMBER;
        value->text = input_slice(read_number(p));
    } else {
        Str token = first == 't' ? S("true") : first == 'f' ? S("false") : S("null");
        size end = p->pos + token.n - 1;
        if ((end >= p->source.n && !refill(p, end)) ||
            !starts(tail(p->source, p->pos), token)) fail();
        value->kind = first == 'n' ? JNULL : JBOOLEAN;
        p->pos += token.n;
    }
    return value;
}

static const Json *field(const Json *node, Str name) {
    if (!node || node->kind != JOBJECT) fail();
    const Json *last = 0;
    for (const Json *f = node->child; f; f = f->next)
        if (equal(read_slice(f->key), name)) last = f;
    if (!last) fail();
    return last;
}

static Str as_string(const Json *value) {
    if (value->kind != JSTRING) fail();
    return read_slice(value->text);
}

static long as_integer(const Json *value) {
    if (value->kind != JNUMBER || value->text.length > 10) fail();
    Str str = read_slice(value->text);
    int negative = at(str, 0) == '-';
    str = tail(str, negative);
    for (size i = 0; i < str.n; i++) if (!digit(str.p[i])) fail();
    long n = decimal(str);
    return negative ? -n : n;
}

typedef struct { Str path, text; int has_cr, written; } File;
typedef struct Warning Warning;
struct Warning { Str text; Warning *next; };

typedef struct {
    Str title, date, summary, layout, slug, body, error;
    Str *tag_names;
    size tag_count;
    int draft;
} Front;

enum PageField {
    P_TITLE, P_URL, P_DATE, P_DATE_LONG, P_SUMMARY, P_EXCERPT, P_MINUTES, P_TAGS,
    P_SLUG, P_CONTENT, P_TOC, P_NEWER, P_OLDER, P_IS_POST, P_FIELDS
};

typedef struct Page Page;
typedef struct TagEntry TagEntry;
struct Page {
    Str path, output, body;
    Str *tag_names;
    uint tag_count, directory_length;
    const Template *layout;
    int post;
    Value card, link, data;
    Value values[P_FIELDS];
};

enum TagField { T_NAME, T_SLUG, T_URL, T_COUNT, T_POSTS, T_FIELDS };
struct TagEntry {
    Str slug;
    Value values[T_FIELDS];
    Value reference, metadata, listing;
    const Value **cards;
    size n, cap;
};

typedef struct {
    Str base;
    Map templates, sources, statics, directories, links, broken, tags, tag_slugs;
    Warning *warnings;
    size warning_count;
    TagEntry **tag_order;
    size tag_count;
    int comma, index_statics;  // Generated template paths all end in index.html.
} Site;

static void warn(Site *site, Str text) {
    Warning *warning = NEW(Warning, 1);
    *warning = (Warning){text, site->warnings};
    site->warnings = warning;
    site->warning_count++;
}

static int string_order(const void *a, const void *b) { return compare(*(const Str *)a, *(const Str *)b); }
static int file_order(const void *a, const void *b) { return compare(((const File *)a)->path, ((const File *)b)->path); }
static int template_order(const void *a, const void *b) { return compare(read_slice(((const Json *)a)->key), read_slice(((const Json *)b)->key)); }

static Str directory(Str path) {
    size i = path.n;
    while (i && path.p[i - 1] != '/') i--;
    return i ? slice(path, 0, i - 1) : S("");
}

static Str normalize_lines(Str text) {
    size i = find_char(text, 0, '\r');
    if (i == NPOS) return text;
    // The file owns this slice of the writable JSON input.
    char *data = (char *)text.p;
    size end = i;
    do {
        data[end++] = '\n';
        size from = i + 1 + (at(text, i + 1) == '\n');
        i = find_char(text, from, '\r');
        size next = i == NPOS ? text.n : i;
        if (data + end != text.p + from) copy(data + end, text.p + from, next - from);
        end += next - from;
    } while (i != NPOS);
    return (Str){data, end};
}

static Str read_title(Str raw, Str *value) {
    if (at(raw, 0) == '[') return S("a bare string opens on [");
    if (at(raw, 0) != '"') {
        *value = raw;
        return (Str){0};
    }
    if (raw.n < 2 || at(raw, raw.n - 1) != '"') return S("a quoted string is not closed");
    Str inner = slice(raw, 1, raw.n - 1);
    Buf text = {0};
    size from = 0;
    for (size i = 0; i < inner.n; i++) {
        int c = at(inner, i);
        if (c == '"') return S("a quoted string holds a stray quote");
        if (c != '\\') continue;
        int next = at(inner, i + 1);
        if (next != '\\' && next != '"') return S("a quoted string holds an escape other than \\\" or \\\\");
        append(&text, slice(inner, from, i));
        put(&text, next);
        from = i + 2;
        i++;
    }
    if (!text.n) *value = inner;
    else {
        append(&text, tail(inner, from));
        *value = take(&text);
    }
    return (Str){0};
}

static int valid_date(Str s) {
    if (s.n != 10 || s.p[4] != '-' || s.p[7] != '-') return 0;
    for (size i = 0; i < 10; i++)
        if (i != 4 && i != 7 && !digit(s.p[i])) return 0;
    size y = decimal(slice(s, 0, 4)), m = decimal(slice(s, 5, 7)), d = decimal(tail(s, 8));
    if (y < 1970 || y > 2099 || m < 1 || m > 12 || d < 1) return 0;
    static const byte days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return d <= (size)days[m - 1] + (m == 2 && y % 4 == 0);
}

typedef struct { char text[10]; byte length; } ShortLiteral;
#define SHORT(literal) {literal, sizeof(literal) - 1}

static Str long_date(Str s) {
    static const ShortLiteral months[] = {
        SHORT("January"), SHORT("February"), SHORT("March"), SHORT("April"), SHORT("May"), SHORT("June"),
        SHORT("July"), SHORT("August"), SHORT("September"), SHORT("October"), SHORT("November"), SHORT("December")
    };
    if (!s.n) return s;
    const ShortLiteral *month = &months[decimal(slice(s, 5, 7)) - 1];
    char day[20];
    return JOIN(format_number(day + sizeof(day), decimal(tail(s, 8))), S(" "),
                ((Str){month->text, month->length}), S(" "), slice(s, 0, 4));
}

static Str read_tags(Str raw, Front *front) {
    if (at(raw, 0) != '[' || at(raw, raw.n - 1) != ']') return S("not a list [a, b]");
    raw = slice(raw, 1, raw.n - 1);
    if (!trim(raw).n) return (Str){0};
    size count = 1;
    for (size i = 0; i < raw.n; i++) count += raw.p[i] == ',';
    front->tag_names = NEW(Str, count);
    front->tag_count = count;
    size from = 0;
    for (size i = 0; i < count; i++) {
        size end = find_char(raw, from, ',');
        if (end == NPOS) end = raw.n;
        Str name = trim(slice(raw, from, end));
        if (!name.n || find_char(name, 0, '"') != NPOS ||
            find_char(name, 0, '[') != NPOS || find_char(name, 0, ']') != NPOS)
            return S("an empty tag, or one holding \" [ or ]");
        front->tag_names[i] = name;
        from = end + 1;
    }
    return (Str){0};
}

enum FrontKey { TITLE, DATE, TAGS, DRAFT, LAYOUT, SLUG, SUMMARY };
static const ShortLiteral keys[] = {
    SHORT("title"), SHORT("date"), SHORT("tags"), SHORT("draft"), SHORT("layout"), SHORT("slug"), SHORT("summary")
};
#undef SHORT

static Str key_value(Front *front, enum FrontKey key, Str raw) {
    switch (key) {
        case TITLE: return read_title(raw, &front->title);
        case SUMMARY: return read_title(raw, &front->summary);
        case TAGS: return read_tags(raw, front);
        case DATE:
            if (!valid_date(raw)) return S("not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)");
            front->date = raw;
            break;
        case DRAFT:
            if (equal(raw, S("true"))) front->draft = 1;
            else if (!equal(raw, S("false"))) return S("true or false");
            break;
        case LAYOUT:
            for (size i = 0; i < raw.n; i++)
                if (!lower_digit(raw.p[i]) && raw.p[i] != '-') return S("not a template name");
            front->layout = raw;
            break;
        case SLUG:
            if (raw.n > 64 || !lower_digit(at(raw, 0))) return S("not a slug");
            for (size i = 1; i < raw.n; i++)
                if (!lower_digit(raw.p[i]) && (raw.p[i] != '-' || !lower_digit(at(raw, i + 1))))
                    return S("not a slug");
            front->slug = raw;
            break;
    }
    return (Str){0};
}

static Str parse_front_line(Front *front, Str line, size number_line, uint *seen) {
    if (at(line, 0) == '#' || only_indent(line)) return (Str){0};
    size colon = 0;
    for (int c; ((c = at(line, colon)) >= 'a' && c <= 'z') || c == '_';) colon++;
    if (!colon || at(line, colon) != ':' || (colon + 1 < line.n && at(line, colon + 1) != ' ')
        || has_line_separator(line))
        return JOIN(S("line "), number(number_line), S(" is not \"key: value\""));
    Str key = slice(line, 0, colon);
    size k = 0;
    while (k < COUNT(keys) && !equal(key, (Str){keys[k].text, keys[k].length})) k++;
    if (k == COUNT(keys)) return JOIN(S("unknown key \""), key, S("\""));
    if (*seen & (1u << k)) return JOIN(S("key \""), key, S("\" is given twice"));
    *seen |= 1u << k;
    size from = colon + 2, end = line.n;
    while (end > from && indent(line.p[end - 1])) end--;
    if (end <= from) return JOIN(S("key \""), key, S("\" has no value"));
    Str error = key_value(front, k, slice(line, from, end));
    if (error.n) return JOIN(S("key \""), key, S("\": "), error);
    return (Str){0};
}

static Front front_matter(Str text) {
    if (!starts(text, S("---\n"))) return (Front){.error = equal(text, S("---"))
        ? S("the front matter is not closed") : S("no front matter")};
    size close = 3;
    for (;;) {
        close = find_str(text, close, S("\n---"));
        if (close == NPOS) return (Front){.error = S("the front matter is not closed")};
        close += 4;
        if (close == text.n || at(text, close) == '\n') break;
    }
    Front front = {0};
    uint seen = 0;
    for (size from = 4, n = 2; from <= close - 4; n++) {
        size end = find_char(text, from, '\n');
        front.error = parse_front_line(&front, slice(text, from, end), n, &seen);
        if (front.error.n) return front;
        from = end + 1;
    }
    if (!(seen & (1u << TITLE))) front.error = S("no title");
    else if (close < text.n) front.body = tail(text, close + 1);
    return front;
}

static void load_templates(Site *site, const Json *templates, const Value *settings) {
    if (templates->kind != JOBJECT) fail();
    reserve_map(&site->templates, templates->count);
    const Json **names = NEW(const Json *, templates->count);
    size n = 0;
    for (const Json *item = templates->child; item; item = item->next) names[n++] = item;
    sort((void **)names, n, template_order);

    for (size i = 0; i < n; i++) {
        const Json *entry = names[i];
        Str name = read_slice(entry->key);
        if (i + 1 < n && equal(name, read_slice(names[i + 1]->key))) continue;
        Template *t = NEW(Template, 1);
        *t = parse_template(as_string(entry));
        if (t->error.n) warn(site, JOIN(S("template \""), name, S("\": "), t->error));
        else set(&site->templates, name, t);
    }
    for (size i = 0; i < site->templates.cap; i++) {
        Template *t = site->templates.entries[i].val;
        if (t) prepare_template(t, &site->templates, settings);
    }
}

static void add_page(Site *site, Page *page, const File *file) {
    Front front = front_matter(file->has_cr ? normalize_lines(file->text) : file->text);
    int post = starts(file->path, S("posts/"));
    Str layout = front.layout.n ? front.layout : post ? S("post") : S("page");
    const Template *t = 0;
    Str reason = front.error;
    if (!reason.n) {
        if (starts(file->path, S("tags/"))) reason = S("tags/ is the generator's own");
        else if (starts(file->path, S("page/"))) reason = S("page/ is the generator's own");
        else if (post && !front.date.n) reason = S("a post needs a date");
        else if (!(t = get(&site->templates, layout))) reason = JOIN(S("layout \""), layout, S("\" is no template that parses"));
    }
    if (reason.n) {
        warn(site, JOIN(S("refused "), file->path, S(": "), reason));
        return;
    }
    if (front.draft) {
        warn(site, JOIN(S("draft "), file->path));
        return;
    }

    PHASE_BEGIN(front_phase, SLUGS);
    Str dir = directory(file->path), slug_dir = post ? S("posts") : dir;
    Map *used = get(&site->directories, slug_dir);
    if (!used) {
        used = NEW(Map, 1);
        *used = (Map){0};
        set(&site->directories, slug_dir, used);
    }
    Str slug = unique_slug(used, front.slug.n ? front.slug : slugify(front.title));
    Str output_suffix = S("/index.html");
    Str output = JOIN(slug_dir, slug_dir.n ? S("/") : (Str){0}, slug, output_suffix);
    Str output_dir = slice(output, 0, output.n - output_suffix.n);
    *page = (Page){
        .path = file->path, .directory_length = dir.n, .body = front.body, .post = post, .layout = t,
        .tag_names = front.tag_names, .tag_count = front.tag_count,
        .output = output,
    };
    Value *v = page->values;
    v[P_TITLE] = prepare_text(front.title);
    v[P_URL] = template_text(JOIN(site->base, S("/"), output_dir, S("/")));
    v[P_DATE] = literal_string(front.date);
    v[P_SUMMARY] = prepare_text(front.summary);
    v[P_SLUG] = literal_string(slug);
    set(&site->sources, file->path, page);
    PHASE_END(front_phase);
}

typedef struct { Site *site; Page *page; Map *resolved; } LinkContext;

static int scheme(Str target) {
    if (!letter(at(target, 0))) return 0;
    for (size i = 1; i < target.n; i++) {
        int c = target.p[i];
        if (c == ':') return 1;
        if (!letter(c) && !digit(c) && c != '+' && c != '.' && c != '-') break;
    }
    return 0;
}

// A slash followed by a slash or dot may need component normalization.
static int path_needs_split(Str path) {
    size i = 0;
    for (; i + 16 < path.n; i += 16) {
        Vec first = *(const Vec *)(path.p + i);
        Vec second = *(const Vec *)(path.p + i + 1);
        if (__builtin_ia32_pmovmskb128((first == SPLAT('/')) &
            ((second | SPLAT(1)) == SPLAT('/')))) return 1;
    }
    for (; i + 1 < path.n; i++)
        if (path.p[i] == '/' && (path.p[i + 1] | 1) == '/') return 1;
    return 0;
}

static Str relative_path(Str dir, Str rel, char *storage, size capacity) {
    // Resolve canonical paths directly, dropping leading parent components.
    Str base = dir, rest = rel;
    while (base.n && rest.n >= 3 && rest.p[0] == '.' && rest.p[1] == '.' && rest.p[2] == '/') {
        Str parent = directory(base);
        if (!parent.n && base.p[0] == '/') break;
        base = parent;
        rest = tail(rest, 3);
    }
    if (rest.n && rest.p[0] != '/' && rest.p[0] != '.' && rest.p[rest.n - 1] != '/' &&
        !path_needs_split(rest)) {
        if (!base.n) return rest;
        size bytes = base.n + 1 + rest.n;
        char *out = bytes <= capacity ? storage : NEW(char, bytes);
        copy(out, base.p, base.n);
        out[base.n] = '/';
        copy(out + base.n + 1, rest.p, rest.n);
        return (Str){out, bytes};
    }
    Str initial[64];
    Str *parts = initial;
    size cap = COUNT(initial), count = 0;
    for (size j = 0; j < 2; j++) {
        Str path = j ? rel : dir;
        if (!path.n) continue;
        for (size from = 0, end; from <= path.n; from = end + 1) {
            end = find_char(path, from, '/');
            if (end == NPOS) end = path.n;
            Str part = slice(path, from, end);
            if (j) {
                if (!part.n || equal(part, S("."))) continue;
                if (equal(part, S(".."))) {
                    if (!count) return (Str){0};
                    count--;
                    continue;
                }
            }
            if (count == cap) {
                Str *larger = NEW(Str, cap * 2);
                copy(larger, parts, cap * sizeof(Str));
                parts = larger;
                cap *= 2;
            }
            parts[count++] = part;
        }
    }
    if (count < 2) return count ? parts[0] : S("");
    size bytes = count - 1;
    for (size i = 0; i < count; i++) bytes += parts[i].n;
    char *out = bytes <= capacity ? storage : NEW(char, bytes);
    size at = 0;
    for (size i = 0; i < count; i++) {
        if (i) out[at++] = '/';
        copy(out + at, parts[i].p, parts[i].n);
        at += parts[i].n;
    }
    return (Str){out, bytes};
}

static Value link_target(void *context, Str target) {
    int first = at(target, 0);
    if (first == '/' || first == '#' || scheme(target)) return string(target);
    LinkContext *link = context;
    Site *site = link->site;
    Str dir = {link->page->path.p, link->page->directory_length};
    if (!link->resolved) {
        void **slot = entry_value(&site->links, dir);
        if (!*slot) {
            Map *links = NEW(Map, 1);
            *links = (Map){0};
            *slot = links;
        }
        link->resolved = *slot;
    }
    // Successful links depend only on the directory and the target within a case.
    Value *saved = get(link->resolved, target);
    if (saved) return *saved;
    size hash = find_char(target, 0, '#');
    if (hash == NPOS) hash = target.n;
    Str frag = tail(target, hash);
    char path_bytes[256];
    Str path = relative_path(dir, slice(target, 0, hash), path_bytes, sizeof(path_bytes));
    Value resolved = string(target);
    if (path.p) {
        const Page *page = get(&site->sources, path);
        if (page) resolved = frag.n ? template_text(JOIN(value_text(&page->values[P_URL]), frag)) : page->values[P_URL];
        else if (get(&site->statics, path)) resolved = template_text(JOIN(site->base, S("/"), path, frag));
    }
    if (resolved.bytes == target.p) {
        Str warning = JOIN(S("broken-link "), link->page->path, S(" -> "), target);
        if (!get(&site->broken, warning)) {
            set(&site->broken, warning, (void *)1);
            warn(site, warning);
        }
    } else {
        saved = NEW(Value, 1);
        *saved = resolved;
        set(link->resolved, retain_link_key(target), saved);
    }
    return resolved;
}

static int post_order(const void *a, const void *b) {
    const Page *x = a, *y = b;
    int dates = compare(value_text(&y->values[P_DATE]), value_text(&x->values[P_DATE]));
    return dates ? dates : compare(value_text(&x->values[P_SLUG]), value_text(&y->values[P_SLUG]));
}

static int tag_order(const void *a, const void *b) {
    const TagEntry *x = a, *y = b;
    if (x->n != y->n) return x->n > y->n ? -1 : 1;
    return compare(x->slug, y->slug);
}

static TagEntry *make_tag(Site *site, Str name, Str slug, int post) {
    TagEntry *tag = NEW(TagEntry, 1);
    *tag = (TagEntry){
        .slug = slug,
        .values = {
            [T_NAME] = prepare_text(name), [T_SLUG] = literal_string(slug),
            [T_URL] = template_text(post ? JOIN(site->base, S("/tags/"), slug, S("/")) : S("")),
        },
    };
    tag->reference = record_values(TAG_FIELDS, tag->values, T_COUNT);
    tag->metadata = record_values(TAG_FIELDS, tag->values, T_POSTS);
    tag->listing = record_values(TAG_FIELDS, tag->values, T_FIELDS);
    if (post) set(&site->tags, slug, tag);
    return tag;
}

static void add_tag_post(TagEntry *tag, const Page *page) {
    if (tag->n == tag->cap) {
        size cap = tag->cap ? tag->cap * 2 : 16;
        const Value **cards = NEW(const Value *, cap);
        copy(cards, tag->cards, tag->n * sizeof(*cards));
        tag->cards = cards;
        tag->cap = cap;
    }
    tag->cards[tag->n++] = &page->card;
}

static Value page_tags(Site *site, Page *page) {
    size n = page->tag_count;
    const Value **items = NEW(const Value *, n);
    Str slugs[8];
    Map seen = {0};
    size count = 0;
    for (size i = 0; i < n; i++) {
        Str name = page->tag_names[i];
        const Str *saved = get(&site->tag_slugs, name);
        if (!saved) {
            Str *slot = NEW(Str, 1);
            *slot = slugify(name);
            set(&site->tag_slugs, name, slot);
            saved = slot;
        }
        Str slug = *saved;
        if (n <= COUNT(slugs)) {
            size j = 0;
            while (j < count && !equal(slug, slugs[j])) j++;
            if (j != count) continue;
            slugs[count] = slug;
        } else {
            if (get(&seen, slug)) continue;
            set(&seen, slug, (void *)1);
        }
        TagEntry *tag = get(&site->tags, slug);
        if (!tag) tag = make_tag(site, name, slug, page->post);
        if (page->post) add_tag_post(tag, page);
        items[count++] = &tag->reference;
    }
    return array(items, count);
}

static void card(Site *site, Page *page) {
    Value *v = page->values;
    v[P_DATE_LONG] = literal_string(long_date(value_text(&v[P_DATE])));
    v[P_TAGS] = page_tags(site, page);
    v[P_IS_POST] = boolean(page->post);
    page->link = record_values(PAGE_FIELDS, v, P_SUMMARY);
    page->card = record_values(PAGE_FIELDS, v, P_SLUG);
    page->data = record_values(PAGE_FIELDS, v, P_FIELDS);
}

static void render_markdown(Site *site, Page *page) {
    PHASE_BEGIN(previous, MARKDOWN);
    LinkContext context = {.site = site, .page = page};
    Markdown md = markdown(page->body, link_target, &context);
    Value *v = page->values;
    v[P_EXCERPT] = md.excerpt;
    v[P_MINUTES] = numeric(md.minutes);
    v[P_CONTENT] = md.html;
    v[P_TOC] = md.toc;
    PHASE_END(previous);
}

static Value collect_tags(Site *site) {
    site->tag_count = site->tags.used;
    site->tag_order = NEW(TagEntry *, site->tag_count);
    size n = 0;
    for (size i = 0; i < site->tags.cap; i++) {
        TagEntry *tag = site->tags.entries[i].val;
        if (!tag) continue;
        tag->values[T_COUNT] = numeric(tag->n);
        tag->values[T_POSTS] = array(tag->cards, tag->n);
        site->tag_order[n++] = tag;
    }
    sort((void **)site->tag_order, site->tag_count, tag_order);
    const Value **metadata = NEW(const Value *, site->tag_count);
    for (size i = 0; i < n; i++) metadata[i] = &site->tag_order[i]->metadata;
    return array(metadata, n);
}

static void write_key(Site *site, Buf *out, Str path) {
    PHASE_BEGIN(render_phase, SERIALIZE);
    if (site->comma) put(out, ',');
    site->comma = 1;
    put(out, '"');
    json(out, path, 0, 0);
    ADD(out, "\":\"");
    PHASE_END(render_phase);
}

static void write_template(Site *site, Buf *out, Str path, const Template *template,
                           const Value *locals, const Value *globals) {
    if (site->index_statics) {
        File *file = get(&site->statics, path);
        if (file) file->written = 1;
    }
    write_key(site, out, path);
    render_template(out, template, locals, globals);
    put(out, '"');
}

static Str index_url(Str base, size n) {
    return n == 1 ? JOIN(base, S("/")) : JOIN(base, S("/page/"), number(n), S("/"));
}

static void write_indexes(Site *site, Buf *out, const Template *template, const Value *globals,
                          const Value **cards, size post_count, size per_page) {
    size pages = post_count ? (post_count - 1) / per_page + 1 : 1;
    Str prev = {0}, current = index_url(site->base, 1);
    for (size n = 1; n <= pages; n++) {
        Str next = n < pages ? index_url(site->base, n + 1) : (Str){0};
        Value number_value = numeric(n), total_value = numeric(pages), url = string(current);
        Value newer = string(prev), older = string(next);
        const Value *pager_fields[] = {&number_value, &total_value, &url, &newer, &older};
        size first = (n - 1) * per_page, count = post_count - first;
        if (count > per_page) count = per_page;
        Value posts = array(cards + first, count), pager = record(PAGER_FIELDS, pager_fields, COUNT(pager_fields));
        const Value *fields[] = {&posts, &pager};
        Value locals = record(INDEX_FIELDS, fields, COUNT(fields));
        Str path = n == 1 ? S("index.html") : JOIN(S("page/"), number(n), S("/index.html"));
        write_template(site, out, path, template, &locals, globals);
        prev = current;
        current = next;
    }
}

static void write_statics(Site *site, Buf *out, File **files, size n) {
    for (size i = 0; i < n; i++) {
        const File *f = files[i];
        // The map entry is shared by inputs with the same path.
        File *file = get(&site->statics, f->path);
        if (file->written) {
            warn(site, JOIN(S("shadowed "), f->path, S(": a built page has its path")));
        } else if (!equal(f->path, S("__proto__"))) {
            file->written = 1;
            write_key(site, out, f->path);
            json(out, f->text, 0, 0);
            put(out, '"');
        }
    }
}

static void write_report(Site *site, Buf *out, size pages, size posts, size statics) {
    const Str **warnings = NEW(const Str *, site->warning_count);
    size i = 0;
    for (Warning *w = site->warnings; w; w = w->next) warnings[i++] = &w->text;
    sort((void **)warnings, i, string_order);
    write_key(site, out, S("build-report.txt"));
    for (size j = 0; j < i; j++) {
        json(out, *warnings[j], 0, 0);
        ADD(out, "\\n");
    }
    Str done = JOIN(S("built "), number(pages), S(" pages, "), number(posts), S(" posts, "),
        number(site->tag_count), S(" tags and "), number(statics), S(" static files"));
    append(out, done);
    ADD(out, "\\n\"");
}

static void build_site(Buf *out, const Json *input) {
    const Json *settings = field(input, S("site"));
    Str title = as_string(field(settings, S("title")));
    Str base = as_string(field(settings, S("base_url")));
    long per_page = as_integer(field(settings, S("per_page")));
    long recent = as_integer(field(settings, S("recent")));
    if (per_page <= 0) fail();
    while (base.n && base.p[base.n - 1] == '/') base.n--;

    Value title_value = prepare_text(title), base_value = template_text(base);
    const Value *site_fields[] = {&title_value, &base_value};
    Value site_value = record(SITE_FIELDS, site_fields, COUNT(site_fields));

    Site site = {.base = base};
    PHASE(TEMPLATES);
    load_templates(&site, field(input, S("templates")), &site_value);
    PHASE(FRONT);
    const Json *input_files = field(input, S("files"));
    if (input_files->kind != JARRAY) fail();
    size n = input_files->count;
    reserve_map(&site.sources, n);
    File *data = NEW(File, n);
    File **files = NEW(File *, n);
    size i = 0;
    for (const Json *entry = input_files->child; entry; entry = entry->next, i++) {
        const Json *content = field(entry, S("content"));
        Str text = as_string(content);
        data[i] = (File){.path = as_string(field(entry, S("path"))), .text = text, .has_cr = content->has_cr};
        files[i] = &data[i];
    }
    sort((void **)files, n, file_order);

    Map reserved = {0};
    set(&reserved, S("posts"), (void *)1);
    set(&reserved, S("tags"), (void *)1);
    set(&reserved, S("page"), (void *)1);
    set(&site.directories, S(""), &reserved);

    Page *pages = NEW(Page, n);
    Page **posts = NEW(Page *, n);
    size page_count = 0, post_count = 0, static_count = 0;
    for (i = 0; i < n; i++) {
        File *f = files[i];
        if (ends(f->path, S(".md"))) {
            Page *p = &pages[page_count];
            p->layout = 0;
            add_page(&site, p, f);
            if (!p->layout) continue;
            page_count++;
            if (p->post) posts[post_count++] = p;
        } else {
            set(&site.statics, f->path, f);
            site.index_statics |= ends(f->path, S("index.html"));
            files[static_count++] = f;
        }
    }

    sort((void **)posts, post_count, post_order);
    const Value **cards = NEW(const Value *, post_count);
    for (i = 0; i < post_count; i++) {
        card(&site, posts[i]);
        cards[i] = &posts[i]->card;
    }
    for (i = 0; i < post_count; i++) {
        if (i > 0) posts[i]->values[P_NEWER] = posts[i - 1]->link;
        if (i + 1 < post_count) posts[i]->values[P_OLDER] = posts[i + 1]->link;
    }
    for (i = 0; i < page_count; i++)
        if (!pages[i].post) card(&site, &pages[i]);

    Value tags_value = collect_tags(&site);
    if (recent < 0) recent += (long)post_count;
    if (recent < 0) recent = 0;
    if ((size)recent > post_count) recent = post_count;
    // Page templates can read the excerpts and reading times of recent posts.
    for (i = 0; i < (size)recent; i++) render_markdown(&site, posts[i]);
    Value recent_value = array(cards, recent);
    const Value *global_fields[] = {&site_value, &tags_value, &recent_value};
    Value globals = record(GLOBAL_FIELDS, global_fields, COUNT(global_fields));

    PHASE(TEMPLATES);
    put(out, '{');
    for (i = 0; i < page_count; i++) {
        Page *p = &pages[i];
        int pending = p->values[P_CONTENT].kind == NIL;
        size mark = html_checkpoint();
        if (pending) render_markdown(&site, p);
        const Value *local = &p->data;
        Value root = record(PAGE_ROOT, &local, 1);
        write_template(&site, out, p->output, p->layout, &root, &globals);
        if (pending) {
            // Listing templates use post cards, which retain the excerpt and minutes.
            Str excerpt = p->post ? value_text(&p->values[P_EXCERPT]) : (Str){0};
            p->values[P_EXCERPT] = rendered(retain_html(mark, excerpt));
            p->values[P_CONTENT] = (Value){0};
            p->values[P_TOC] = (Value){0};
        }
    }
    const Template *index = get(&site.templates, S("index"));
    if (index) write_indexes(&site, out, index, &globals, cards, post_count, per_page);
    const Template *tag_template = get(&site.templates, S("tag"));
    if (tag_template) {
        for (i = 0; i < site.tag_count; i++) {
            const TagEntry *t = site.tag_order[i];
            const Value *local = &t->listing;
            Value root = record(TAG_ROOT, &local, 1);
            Str path = JOIN(S("tags/"), t->slug, S("/index.html"));
            write_template(&site, out, path, tag_template, &root, &globals);
        }
    }
    const Template *tags_template = get(&site.templates, S("tags"));
    if (tags_template) write_template(&site, out, S("tags/index.html"), tags_template, 0, &globals);
    PHASE(SERIALIZE);
    write_statics(&site, out, files, static_count);
    write_report(&site, out, page_count - post_count, post_count, static_count);
    put(out, '}');
}

static void parse_batch(Buf *out, char *source, size length) {
    Parser parser = {.source = {source, 0}, .remaining = length};
    PHASE(PARSE);
    if (peek_batch(&parser) != '[') fail();
    parser.pos++;
    put(out, '[');
    if (peek_batch(&parser) != ']') {
        for (;;) {
            size mark = checkpoint();
            PHASE(PARSE);
            const Json *item = read_value(&parser, 0);
            PHASE(FRONT);
            build_site(out, field(item, S("input")));
            PHASE(PARSE);
            rewind_arena(mark);
            if (peek_batch(&parser) != ',') break;
            parser.pos++;
            // No slice from the completed case is live after the arena rewind.
            if (length > INPUT_CHUNK) {
                size unread = parser.source.n - parser.pos;
                copy(source, source + parser.pos, unread);
                parser.source.n = unread;
                parser.pos = 0;
            }
            put(out, ',');
        }
    }
    if (peek_batch(&parser) != ']') fail();
    parser.pos++;
    if (peek_batch(&parser) != -1) fail();
    put(out, ']');
}

void build_answers(Buf *out, char *source, size length) {
    // Scan raw blocks before decoding them. Wide mode applies to the entire
    // batch, so restart from byte zero if a later block changes the decision.
    input_base = source;
    size mark = checkpoint();
    latin1 = 1;
    if (__builtin_setjmp(encoding_restart)) {
        rewind_arena(mark);
        restart_answer();
        latin1 = 0;
    }
    PHASE(INPUT);
    parse_batch(out, source, length);
}

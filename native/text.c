#include "core.h"

typedef char Vec __attribute__((vector_size(16), aligned(1), may_alias));
#define SPLAT(c) ((Vec){c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c})

uint decode(Str s, size *i) {
    uint cp = (byte)s.p[(*i)++];
    if (cp < 0x80) return cp;
    uint length, minimum;
    if (cp < 0xc2 || cp > 0xf4) fail();
    if (cp < 0xe0) {
        cp &= 31;
        length = 1;
        minimum = 0x80;
    } else if (cp < 0xf0) {
        cp &= 15;
        length = 2;
        minimum = 0x800;
    } else {
        cp &= 7;
        length = 3;
        minimum = 0x10000;
    }
    if (s.n - *i < length) fail();
    while (length--) {
        uint byte = (unsigned char)s.p[(*i)++];
        if ((byte & 0xc0) != 0x80) fail();
        cp = (cp << 6) | (byte & 63);
    }
    if (cp < minimum || cp > 0x10ffff) fail();
    return cp;
}

void validate_utf8(Str s) {
    size i = 0;
    while (s.n - i >= 16) {
        uint high = __builtin_ia32_pmovmskb128(*(const Vec *)(s.p + i));
        if (!high) {
            i += 16;
            continue;
        }
        i += first_bit(high);
        uint cp = decode(s, &i);
        if (cp >= 0xd800 && cp <= 0xdfff) fail();
    }
    while (i < s.n) {
        uint cp = decode(s, &i);
        if (cp >= 0xd800 && cp <= 0xdfff) fail();
    }
}

void encode(Buf *out, uint cp) {
    char *p = room(out, 4);
    if (cp < 0x80) {
        *p++ = (char)cp;
    } else {
        if (cp < 0x800) {
            *p++ = (char)(0xc0 | (cp >> 6));
        } else {
            if (cp < 0x10000) {
                *p++ = (char)(0xe0 | (cp >> 12));
            } else {
                *p++ = (char)(0xf0 | (cp >> 18));
                *p++ = (char)(0x80 | ((cp >> 12) & 63));
            }
            *p++ = (char)(0x80 | ((cp >> 6) & 63));
        }
        *p++ = (char)(0x80 | (cp & 63));
    }
    out->n = p - out->p;
}

static uint code_unit(Str s, size *i, uint *pending) {
    if (*pending) {
        uint low = *pending;
        *pending = 0;
        return low;
    }
    if (*i == s.n) return 0x10000;
    uint cp = decode(s, i);
    if (cp <= 0xffff) return cp;
    cp -= 0x10000;
    *pending = 0xdc00 + (cp & 1023);
    return 0xd800 + (cp >> 10);
}

int compare(Str a, Str b) {
    size n = a.n < b.n ? a.n : b.n;
    size i = 0;
    for (; n - i >= 16; i += 16) {
        Vec x = *(const Vec *)(a.p + i), y = *(const Vec *)(b.p + i);
        uint different = (uint)__builtin_ia32_pmovmskb128(x == y) ^ 0xffff;
        if (different) {
            i += first_bit(different);
            break;
        }
    }
    if (i + 8 <= n) {
        typedef size Word __attribute__((aligned(1), may_alias));
        size x = *(const Word *)(a.p + i), y = *(const Word *)(b.p + i);
        size diff = x ^ y;
        if (!diff) i += 8;
        else i += __builtin_ctzl(diff) / 8;
    }
    for (; i < n; i++) {
        int x = (byte)a.p[i], y = (byte)b.p[i];
        if (x == y) continue;
        if (x < 128 || y < 128) return x - y;
        // UTF-8 and UTF-16 differ at supplementary/BMP boundaries.
        while (i && ((byte)a.p[i] & 0xc0) == 0x80 && ((byte)b.p[i] & 0xc0) == 0x80) i--;
        size j = i;
        uint low_a = 0, low_b = 0;
        for (;;) {
            uint ca = code_unit(a, &i, &low_a), cb = code_unit(b, &j, &low_b);
            if (ca == cb && ca != 0x10000) continue;
            if (ca == cb) return 0;
            if (ca == 0x10000 || cb == 0x10000) return cb == 0x10000 ? 1 : -1;
            return (int)ca - (int)cb;
        }
    }
    return (a.n > b.n) - (a.n < b.n);
}

size space_width(Str s, size i) {
    int c = at(s, i);
    if (c == ' ' || (c >= '\t' && c <= '\r')) return 1;
    int b = at(s, i + 1), d = at(s, i + 2);
    switch (c) {
        case 0xc2: return b == 0xa0 ? 2 : 0;
        case 0xe1: return b == 0x9a && d == 0x80 ? 3 : 0;
        case 0xe2:
            if (b == 0x80) return (d >= 0x80 && d <= 0x8a) || d == 0xa8 || d == 0xa9 || d == 0xaf ? 3 : 0;
            return b == 0x81 && d == 0x9f ? 3 : 0;
        case 0xe3: return b == 0x80 && d == 0x80 ? 3 : 0;
        case 0xef: return b == 0xbb && d == 0xbf ? 3 : 0;
        default: return 0;
    }
}

int has_space(Str s) {
    size i = 0;
    for (; s.n - i >= 16; i += 16) {
        Vec bytes = *(const Vec *)(s.p + i);
        Vec ascii = (bytes == SPLAT(' ')) | ((bytes > SPLAT(8)) & (bytes < SPLAT(14)));
        if (__builtin_ia32_pmovmskb128(ascii)) return 1;
        if (!__builtin_ia32_pmovmskb128(bytes)) continue;
        uint leads = __builtin_ia32_pmovmskb128(
            (bytes == SPLAT((char)0xc2)) | (bytes == SPLAT((char)0xe1)) |
            (bytes == SPLAT((char)0xe2)) | (bytes == SPLAT((char)0xe3)) |
            (bytes == SPLAT((char)0xef)));
        for (; leads; leads &= leads - 1)
            if (space_width(s, i + first_bit(leads))) return 1;
    }
    for (; i < s.n; i++) {
        int c = (byte)s.p[i];
        if (c <= ' ' || c >= 0xc2)
            if (space_width(s, i)) return 1;
    }
    return 0;
}

int has_line_separator(Str s) {
    for (size i = 0; (i = find_char(s, i, (char)0xe2)) != NPOS && i + 2 < s.n; i++)
        if ((byte)s.p[i + 1] == 0x80 &&
            ((byte)s.p[i + 2] == 0xa8 || (byte)s.p[i + 2] == 0xa9)) return 1;
    return 0;
}

Str trim(Str s) {
    size a = 0, b = s.n, width;
    while ((width = space_width(s, a))) a += width;
    while (b > a) {
        size i = b - 1;
        while (i > a && ((byte)s.p[i] & 0xc0) == 0x80) i--;
        if (space_width(s, i) != b - i) break;
        b = i;
    }
    return slice(s, a, b);
}

int only_indent(Str s) {
    for (size i = 0; i < s.n; i++) if (!indent(s.p[i])) return 0;
    return 1;
}

Str trim_line(Str s) {
    size a = 0, b = s.n;
    while (a < b && indent(s.p[a])) a++;
    while (b > a && indent(s.p[b - 1])) b--;
    return slice(s, a, b);
}

static Str html_entity(int c) {
    switch (c) {
        case '&': return S("&amp;");
        case '<': return S("&lt;");
        case '>': return S("&gt;");
        case '"': return S("&quot;");
        case '\'': return S("&#39;");
        default: return (Str){0};
    }
}
static const char hex[] = "0123456789abcdef";
static const byte escape_class[256] = {
    [0 ... 31] = 1, ['"'] = 1, ['\\'] = 1, ['&'] = 2, ['<'] = 2, ['>'] = 2, ['\''] = 2,
    [128 ... 236] = 4, [0xed] = 4 | 8, [238 ... 255] = 4,
};

void json_code_unit(Buf *out, uint cp) {
    char *p = room(out, 6);
    p[0] = '\\';
    p[1] = 'u';
    p[2] = hex[(cp >> 12) & 15];
    p[3] = hex[(cp >> 8) & 15];
    p[4] = hex[(cp >> 4) & 15];
    p[5] = hex[cp & 15];
    out->n += 6;
}

static inline uint json_marks(Vec w, int html, int unicode) {
    Vec hit = ((w ^ SPLAT(-128)) < SPLAT(-96)) | (w == SPLAT('"')) | (w == SPLAT('\x5c'));
    if (html) hit |= (w == SPLAT('&')) | (w == SPLAT('<')) | (w == SPLAT('>')) | (w == SPLAT('\x27'));
    if (unicode == 1) hit |= w == SPLAT((char)0xed);
    if (unicode == 2) hit |= w;
    return __builtin_ia32_pmovmskb128(hit);
}

static size next_json(Str s, size i, int html, int unicode) {
    for (; s.n - i >= 16; i += 16) {
        uint mask = json_marks(*(const Vec *)(s.p + i), html, unicode);
        if (mask) return i + first_bit(mask);
    }
    if (i >= 16 && i < s.n) {
        // The last vector overlaps earlier bytes. Mask lanes before the cursor.
        size end = s.n - 16;
        uint mask = json_marks(*(const Vec *)(s.p + end), html, unicode) & (0xffffu << (i - end));
        return mask ? end + first_bit(mask) : s.n;
    }
    if (s.n - i >= 8) {
        typedef long Word __attribute__((aligned(1), may_alias));
        // Check the ends together. Any overlapping match appears in the first half.
        size last = s.n - 8;
        union { long words[2]; Vec bytes; } block = {
            .words = {*(const Word *)(s.p + i), *(const Word *)(s.p + last)}
        };
        uint mask = json_marks(block.bytes, html, unicode);
        if (mask & 255) return i + first_bit(mask);
        if (mask >> 8) return last + first_bit(mask >> 8);
        return s.n;
    }
    uint mask = 1 | (html ? 2 : 0) | (unicode == 1 ? 8 : unicode == 2 ? 4 : 0);
    for (; i < s.n && !(escape_class[(byte)s.p[i]] & mask); i++) {}
    return i;
}

Value template_text(Str s) {
    return next_json(s, 0, 1, latin1 ? 0 : 2) == s.n ? literal_string(s) : string(s);
}

static Str json_escape(int c) {
    switch (c) {
        case '\\': return S("\\\\");
        case '"': return S("\\\"");
        case '\b': return S("\\b");
        case '\t': return S("\\t");
        case '\n': return S("\\n");
        case '\f': return S("\\f");
        case '\r': return S("\\r");
        default: return (Str){0};
    }
}

void json_ascii(Buf *out, int c) {
    Str special = json_escape(c);
    if (special.n) append(out, special);
    else if (c < 32) json_code_unit(out, c);
    else put(out, c);
}

void json(Buf *out, Str s, int html, int escape_pairs) {
    int unicode = latin1 ? 0 : 1 + !!escape_pairs;
    size from = 0;
    for (size i = 0; (i = next_json(s, i, html, unicode)) < s.n;) {
        uint c = (byte)s.p[i];
        size next = i + 1;
        Str entity = {0};
        if (c < 128) {
            if (html) entity = html_entity(c);
            if (!entity.n) entity = json_escape(c);
        } else {
            next = i;
            c = decode(s, &next);
            if (c < 0xd800 || (c > 0xdfff && (!escape_pairs || c < 0x10000))) {
                i = next;
                continue;
            }
        }
        append(out, slice(s, from, i));
        if (entity.n) append(out, entity);
        else if (c < 0x10000) {
            if (!escape_pairs && c >= 0xd800 && c <= 0xdbff && at(s, next) == 0xed) {
                size end = next;
                uint low = decode(s, &end);
                if (low >= 0xdc00 && low <= 0xdfff) {
                    encode(out, 0x10000 + ((c - 0xd800) << 10) + low - 0xdc00);
                    from = i = end;
                    continue;
                }
            }
            json_code_unit(out, c);
        } else {
            c -= 0x10000;
            json_code_unit(out, 0xd800 + (c >> 10));
            json_code_unit(out, 0xdc00 + (c & 1023));
        }
        from = i = next;
    }
    append(out, tail(s, from));
}

void escape_rendered(Buf *out, Str s) {
    size from = 0;
    for (size i = 0; (i = next_json(s, i, 1, 0)) < s.n;) {
        uint c = (byte)s.p[i];
        size next = i + 1;
        if (c == '\\') {
            c = at(s, next++);
            if (c != '"') {
                i = next;
                continue;
            }
        }
        append(out, slice(s, from, i));
        append(out, html_entity(c));
        from = i = next;
    }
    append(out, tail(s, from));
}

#include "unicode.inc"

static Str slug_projection(uint cp) {
    size lo = 0, hi = COUNT(slug_drops);
    while (lo < hi) {
        size mid = (lo + hi) / 2;
        uint word = slug_drops[mid], first = word >> 8;
        if (cp < first) hi = mid;
        else if (cp - first > (word & 255)) lo = mid + 1;
        else return (Str){0};
    }
    lo = 0;
    hi = COUNT(slug_maps);
    while (lo < hi) {
        size mid = (lo + hi) / 2;
        uint word = slug_maps[mid], key = word >> 15;
        if (cp < key) hi = mid;
        else if (cp > key) lo = mid + 1;
        else return (Str){slug_data + ((word >> 3) & 4095), word & 7};
    }
    return S("-");
}

static const byte slug_ascii[] = {
    '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-',
    '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-',
    '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-',
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '-', '-', '-', '-', '-', '-',
    '-', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
    'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '-', '-', '-', '-', '-',
    '-', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
    'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '-', '-', '-', '-', '-'
};

static inline void slug_byte(char *buf, size *n, byte c) {
    byte folded = slug_ascii[c];
    buf[*n] = folded;
    // Skip leading and repeated separators. Trim the trailing one at the end.
    *n += folded != '-' || (*n && buf[*n - 1] != '-');
}

Str slugify(Str s) {
    char chars[64];
    size n = 0;
    for (size i = 0; i < s.n && n < sizeof(chars);) {
        uint c = (byte)s.p[i];
        if (c < 128) {
            i++;
            slug_byte(chars, &n, c);
        } else {
            Str projection = slug_projection(decode(s, &i));
            for (size j = 0; j < projection.n && n < sizeof(chars); j++)
                slug_byte(chars, &n, projection.p[j]);
        }
    }
    if (n && chars[n - 1] == '-') n--;
    return n ? JOIN(((Str){chars, n})) : S("untitled");
}

Str unique_slug(Map *used, Str slug) {
    void **base = entry_value(used, slug);
    size next = (size)*base;
    if (!next) {
        *base = (void *)2;
        return slug;
    }
    if (next == 1) next = 2;  // A reserved directory.
    char chars[64 + 1 + 20];
    if (slug.n > 64) fail();
    copy(chars, slug.p, slug.n);
    chars[slug.n] = '-';
    for (;;) {
        char digits[20];
        Str suffix = format_number(digits + sizeof(digits), next++);
        copy(chars + slug.n + 1, suffix.p, suffix.n);
        Str candidate = {chars, slug.n + 1 + suffix.n};
        if (!get(used, candidate)) {
            Str name = JOIN(candidate);
            *base = (void *)next;
            set(used, name, (void *)2);
            return name;
        }
    }
}

enum Property property_id(Str name) {
    switch (name.n) {
        case 2:
            if (equal(name, S("id"))) return PROP_ID;
            break;
        case 3:
            if (equal(name, S("tag"))) return PROP_TAG;
            if (equal(name, S("toc"))) return PROP_TOC;
            if (equal(name, S("url"))) return PROP_URL;
            break;
        case 4:
            if (equal(name, S("date"))) return PROP_DATE;
            if (equal(name, S("html"))) return PROP_HTML;
            if (equal(name, S("last"))) return PROP_LAST;
            if (equal(name, S("name"))) return PROP_NAME;
            if (equal(name, S("page"))) return PROP_PAGE;
            if (equal(name, S("site"))) return PROP_SITE;
            if (equal(name, S("slug"))) return PROP_SLUG;
            if (equal(name, S("tags"))) return PROP_TAGS;
            break;
        case 5:
            if (equal(name, S("count"))) return PROP_COUNT;
            if (equal(name, S("first"))) return PROP_FIRST;
            if (equal(name, S("index"))) return PROP_INDEX;
            if (equal(name, S("level"))) return PROP_LEVEL;
            if (equal(name, S("newer"))) return PROP_NEWER;
            if (equal(name, S("older"))) return PROP_OLDER;
            if (equal(name, S("pager"))) return PROP_PAGER;
            if (equal(name, S("posts"))) return PROP_POSTS;
            if (equal(name, S("title"))) return PROP_TITLE;
            if (equal(name, S("total"))) return PROP_TOTAL;
            break;
        case 6:
            if (equal(name, S("length"))) return PROP_LENGTH;
            if (equal(name, S("number"))) return PROP_NUMBER;
            if (equal(name, S("recent"))) return PROP_RECENT;
            break;
        case 7:
            if (equal(name, S("content"))) return PROP_CONTENT;
            if (equal(name, S("excerpt"))) return PROP_EXCERPT;
            if (equal(name, S("is_post"))) return PROP_IS_POST;
            if (equal(name, S("summary"))) return PROP_SUMMARY;
            break;
        case 8:
            if (equal(name, S("base_url"))) return PROP_BASE_URL;
            break;
        case 9:
            if (equal(name, S("date_long"))) return PROP_DATE_LONG;
            if (equal(name, S("newer_url"))) return PROP_NEWER_URL;
            if (equal(name, S("older_url"))) return PROP_OLDER_URL;
            break;
        case 15:
            if (equal(name, S("reading_minutes"))) return PROP_READING_MINUTES;
            break;
    }
    return PROP_UNKNOWN;
}

// The record's width distinguishes prefix views such as cards and page links.
static const byte field_slots[][PROP_URL + 1] = {
    [PAGE_FIELDS] = {
        [PROP_TITLE] = 1, [PROP_URL] = 2, [PROP_DATE] = 3, [PROP_DATE_LONG] = 4,
        [PROP_SUMMARY] = 5, [PROP_EXCERPT] = 6, [PROP_READING_MINUTES] = 7, [PROP_TAGS] = 8,
        [PROP_SLUG] = 9, [PROP_CONTENT] = 10, [PROP_TOC] = 11, [PROP_NEWER] = 12,
        [PROP_OLDER] = 13, [PROP_IS_POST] = 14,
    },
    [TAG_FIELDS] = {
        [PROP_NAME] = 1, [PROP_SLUG] = 2, [PROP_URL] = 3, [PROP_COUNT] = 4, [PROP_POSTS] = 5,
    },
    [HEADING_FIELDS] = {[PROP_LEVEL] = 1, [PROP_ID] = 2, [PROP_HTML] = 3},
    [LOOP_FIELDS] = {[PROP_INDEX] = 1, [PROP_FIRST] = 2, [PROP_LAST] = 3, [PROP_LENGTH] = 4},
    [PAGER_FIELDS] = {
        [PROP_NUMBER] = 1, [PROP_TOTAL] = 2, [PROP_URL] = 3, [PROP_NEWER_URL] = 4, [PROP_OLDER_URL] = 5,
    },
    [INDEX_FIELDS] = {[PROP_POSTS] = 1, [PROP_PAGER] = 2},
    [SITE_FIELDS] = {[PROP_TITLE] = 1, [PROP_BASE_URL] = 2},
    [GLOBAL_FIELDS] = {[PROP_SITE] = 1, [PROP_TAGS] = 2, [PROP_RECENT] = 3},
    [PAGE_ROOT] = {[PROP_PAGE] = 1},
    [TAG_ROOT] = {[PROP_TAG] = 1},
};

const Value *property(const Value *value, enum Property name) {
    if (!value || name == PROP_UNKNOWN) return 0;
    if (value->kind == RECORD) {
        enum RecordLayout layout = value->shape.layout;
        uint slot = field_slots[layout][name];
        if (!slot || slot > value->shape.fields) return 0;
        if (layout <= HEADING_FIELDS) return &value->values[slot - 1];
        return value->fields[slot - 1];
    }
    if (value->kind == ARRAY && name == PROP_LENGTH) {
        // Template paths cannot contain a numeric segment.
        static Value length;
        length = numeric(value->n);
        return &length;
    }
    return 0;
}


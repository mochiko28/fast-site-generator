#include "core.h"

typedef char Vec __attribute__((vector_size(16), aligned(1), may_alias));
#define SPLAT(c) ((Vec){c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c})

static const byte escapable[128] = {
    ['\\'] = 1, ['`'] = 1, ['*'] = 1, ['_'] = 1, ['['] = 1, [']'] = 1,
    ['('] = 1, [')'] = 1, ['#'] = 1, ['+'] = 1, ['-'] = 1, ['.'] = 1, ['!'] = 1, ['>'] = 1,
};

typedef struct { char text[7]; byte length; } Entity;
#define ENTITY(c, s) [c] = {s, sizeof(s) - 1}
static const Entity escapes[128] = {
    ENTITY(0, "\\u0000"), ENTITY(1, "\\u0001"), ENTITY(2, "\\u0002"), ENTITY(3, "\\u0003"),
    ENTITY(4, "\\u0004"), ENTITY(5, "\\u0005"), ENTITY(6, "\\u0006"), ENTITY(7, "\\u0007"),
    ENTITY(8, "\\b"), ENTITY(9, "\\t"), ENTITY(10, "\\n"), ENTITY(11, "\\u000b"),
    ENTITY(12, "\\f"), ENTITY(13, "\\r"), ENTITY(14, "\\u000e"), ENTITY(15, "\\u000f"),
    ENTITY(16, "\\u0010"), ENTITY(17, "\\u0011"), ENTITY(18, "\\u0012"), ENTITY(19, "\\u0013"),
    ENTITY(20, "\\u0014"), ENTITY(21, "\\u0015"), ENTITY(22, "\\u0016"), ENTITY(23, "\\u0017"),
    ENTITY(24, "\\u0018"), ENTITY(25, "\\u0019"), ENTITY(26, "\\u001a"), ENTITY(27, "\\u001b"),
    ENTITY(28, "\\u001c"), ENTITY(29, "\\u001d"), ENTITY(30, "\\u001e"), ENTITY(31, "\\u001f"),
    ENTITY('&', "&amp;"), ENTITY('<', "&lt;"), ENTITY('>', "&gt;"),
    ENTITY('"', "&quot;"), ENTITY('\'', "&#39;"),
    ENTITY('#', "#"),
    ENTITY('\\', "\\\\"), ENTITY('^', "^"), ENTITY('|', "|"), ENTITY('~', "~"),
};
#undef ENTITY

typedef struct {
    Vec control, quote_mask, quotes, angle_mask, angles, bracket, star, backtick, surrogate;
} InlineClasses;
#define INLINE_CLASSES(surrogate) { \
    SPLAT((char)0xe0), SPLAT((char)0xfa), SPLAT('"'), SPLAT((char)0x9d), SPLAT(0x1c), \
    SPLAT('['), SPLAT('*'), SPLAT(0x60), SPLAT(surrogate) \
}
static const InlineClasses inline_classes[2] __attribute__((aligned(16))) = {
    INLINE_CLASSES((char)0xed), INLINE_CLASSES(0),
};
#undef INLINE_CLASSES

static inline uint marks(Vec bytes, const InlineClasses *c) {
    // Grouped byte tests also flag a few ASCII literals. The caller copies them unchanged.
    Vec hits = ((bytes & c->control) == SPLAT(0)) |
        ((bytes & c->quote_mask) == c->quotes) |
        ((bytes & c->angle_mask) == c->angles) |
        (bytes == c->bracket) | (bytes == c->star) |
        (bytes == c->backtick) | (bytes == c->surrogate);
    return __builtin_ia32_pmovmskb128(hits);
}

// Code spans and fences borrow SIMD-padded Markdown storage, and their
// escaped text is appended to the same contiguous HTML arena.
static void escape_code(Buf *out, Str s) {
    if (!latin1 || s.n * 6 + 16 > out->cap - out->n) {
        json(out, s, 1, 0);
        return;
    }
    char *dst = out->p + out->n;
    size i = 0;
    while (i < s.n) {
        Vec w = *(const Vec *)(s.p + i);
        *(Vec *)dst = w;
        Vec marked = ((w & SPLAT((char)0xe0)) == SPLAT(0)) |
            ((w & SPLAT((char)0xfa)) == SPLAT('"')) |
            ((w & SPLAT((char)0x9d)) == SPLAT(0x1c));
        uint bits = __builtin_ia32_pmovmskb128(marked);
        size count = bits ? first_bit(bits) : 16;
        if (count >= s.n - i) {
            dst += s.n - i;
            break;
        }
        dst += count;
        i += count;
        if (bits) {
            const Entity *escape = &escapes[(byte)s.p[i++]];
            __builtin_memcpy(dst, escape, sizeof(*escape));
            dst += escape->length;
        }
    }
    out->n = dst - out->p;
}

static inline size arena_char(Str s, size from, char c) {
    Vec needle = SPLAT(c);
    while (from < s.n) {
        uint bits = __builtin_ia32_pmovmskb128(*(const Vec *)(s.p + from) == needle);
        if (bits) {
            size pos = from + first_bit(bits);
            return pos < s.n ? pos : NPOS;
        }
        from += 16;
    }
    return NPOS;
}

static size closer(Str s, size from, char c, int strong, int single_star) {
    for (size i = from; (i = arena_char(s, i, c)) != NPOS; i++) {
        if (strong && at(s, i + 1) != c) continue;
        size slash = i;
        while (slash > from && s.p[slash - 1] == '\\') slash--;
        if ((i - slash) & 1) continue;
        if (!single_star || at(s, i + 1) != '*') return i;
        i++;
    }
    return NPOS;
}

static size link_end(Str s, size i) {
    for (; i < s.n; i += 16) {
        Vec w = *(const Vec *)(s.p + i);
        Vec ws = (w == SPLAT(' ')) | ((w > SPLAT(8)) & (w < SPLAT(14)));
        uint stops = __builtin_ia32_pmovmskb128(ws | w | (w == SPLAT(')')));
        for (; stops; stops &= stops - 1) {
            size end = i + first_bit(stops);
            if (end >= s.n) return NPOS;
            byte c = s.p[end];
            if (c == ')') return end;
            if (c < 128 || space_width(s, end)) return NPOS;
        }
    }
    return NPOS;
}

#define INLINE_HTML inline_sse2
#define INLINE_TARGET
#define INLINE_SHUFFLE 0
#include "inline.inc"
#undef INLINE_HTML
#undef INLINE_TARGET
#undef INLINE_SHUFFLE

#define INLINE_HTML inline_avx2
#define INLINE_TARGET __attribute__((target("avx2")))
#define INLINE_SHUFFLE 1
#include "inline.inc"
#undef INLINE_HTML
#undef INLINE_TARGET
#undef INLINE_SHUFFLE

static inline void inline_html(Buf *out, Str s, LinkContext *context, uint depth) {
    if (has_avx2) inline_avx2(out, s, context, depth);
    else inline_sse2(out, s, context, depth);
}

enum Block { PARAGRAPH, FENCE = 7, BREAK, BLANK };

static int fence_char(int c) {
    return letter(c) || digit(c) || c == '_' || c == '+' || c == '-';
}

enum BlockLead { TEXT_LEAD, INDENT_LEAD, HEADING_LEAD, FENCE_LEAD, RULE_LEAD };
static const byte block_lead[256] = {
    [' '] = INDENT_LEAD, ['\t'] = INDENT_LEAD, ['#'] = HEADING_LEAD,
    [0x60] = FENCE_LEAD, ['-'] = RULE_LEAD, ['*'] = RULE_LEAD,
};

static __attribute__((noinline)) int special_block(Str line, byte lead) {
    int c = (byte)line.p[0];
    switch (lead) {
        case INDENT_LEAD:
            if (only_indent(tail(line, 1))) return BLANK;
            break;
        case HEADING_LEAD: {
            size n = 1;
            while (at(line, n) == '#') n++;
            if (n <= 6 && indent(at(line, n)) && !only_indent(tail(line, n + 1))
                && !has_line_separator(tail(line, n))) return n;
            break;
        }
        case FENCE_LEAD:
            if (at(line, 1) == 0x60 && at(line, 2) == 0x60) {
                size i = 3;
                while (fence_char(at(line, i))) i++;
                if (only_indent(tail(line, i))) return FENCE;
            }
            break;
        case RULE_LEAD:
            if (at(line, 1) == c && at(line, 2) == c && only_indent(tail(line, 3))) return BREAK;
            break;
    }
    return PARAGRAPH;
}

static inline int block(Str line) {
    if (!line.n) return BLANK;
    byte lead = block_lead[(byte)line.p[0]];
    return lead == TEXT_LEAD ? PARAGRAPH : special_block(line, lead);
}

static __attribute__((noinline)) size special_list_marker(Str line, int c) {
    if (c == '-' || c == '*') return at(line, 1) == ' ' && !has_line_separator(line) ? 2 : 0;
    size i = 1;
    while (i < 10 && digit(at(line, i))) i++;
    return i < 10 && at(line, i) == '.' && at(line, i + 1) == ' ' &&
        !has_line_separator(line) ? i + 2 : 0;
}

static inline size list_marker(Str line) {
    int c = at(line, 0);
    return c == '-' || c == '*' || digit(c) ? special_list_marker(line, c) : 0;
}

static int continued(Str line) {
    if (at(line, 0) != ' ' || at(line, 1) != ' ') return 0;
    size i = 2;
    while (at(line, i) == ' ') i++;
    return i < line.n && !space_width(line, i);
}

typedef struct {
    Str body, line;
    size at, end, next;
    size words;
    uint preceding_gap, carry;
    uint read_line, end_lines;
    uint ends[128];  // Byte offsets within the bounded input arena.
} Lines;

// Count word starts from the two bytes of a SIMD mask on baseline x86-64.
#define POP2(n) n, n + 1, n + 1, n + 2
#define POP4(n) POP2(n), POP2(n + 1), POP2(n + 1), POP2(n + 2)
#define POP6(n) POP4(n), POP4(n + 1), POP4(n + 1), POP4(n + 2)
static const byte popcounts[] = {POP6(0), POP6(1), POP6(1), POP6(2)};
#undef POP6
#undef POP4
#undef POP2

static uint bit_count(uint bits) {
    return popcounts[bits & 255] + popcounts[bits >> 8];
}

typedef char Wide __attribute__((vector_size(32), aligned(1), may_alias));
#define SPLAT_WIDE(c) ((Wide){c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c})

static inline uint queue_lines(uint *ends, uint count, size base, uint breaks) {
    for (; breaks; breaks &= breaks - 1)
        ends[count++] = base + first_bit(breaks);
    return count;
}

static inline __attribute__((target("avx2"))) uint unicode_space_leads(Wide w) {
    // C2 shares its low nibble with E2 and gets a separate byte comparison.
    const Wide leads = {0, (char)0xe1, (char)0xe2, (char)0xe3, 0, 0, 0, 0,
                        0, 0, 0, 0, 0, 0, 0, (char)0xef,
                        0, (char)0xe1, (char)0xe2, (char)0xe3, 0, 0, 0, 0,
                        0, 0, 0, 0, 0, 0, 0, (char)0xef};
    Wide match = (w == __builtin_ia32_pshufb256(leads, w & SPLAT_WIDE(15))) |
                 (w == SPLAT_WIDE((char)0xc2));
    return __builtin_ia32_pmovmskb256(match & w);
}

static __attribute__((target("avx2,popcnt"), noinline)) void scan_wide(Lines *lines) {
    Str body = lines->body;
    size i = lines->next;
    int count_words = body.n > 400;
    size words = 0, gaps = lines->preceding_gap | ((size)lines->carry << 1);
    uint count = lines->end_lines;
    for (; body.n - i >= 32 && count <= COUNT(lines->ends) - 32; i += 32) {
        Wide w = *(const Wide *)(body.p + i);
        uint breaks = __builtin_ia32_pmovmskb256(w == SPLAT_WIDE('\n'));
        if (count_words) {
            Wide ws = (w == SPLAT_WIDE(' ')) | ((w > SPLAT_WIDE(8)) & (w < SPLAT_WIDE(14)));
            // Keep enough high bits for whitespace that crosses into the next vector.
            size mask = (gaps >> 1) | (uint)__builtin_ia32_pmovmskb256(ws);
            for (uint leads = unicode_space_leads(w); leads; leads &= leads - 1) {
                uint j = first_bit(leads);
                size width = space_width(body, i + j);
                mask |= ((1UL << width) - 1) << j;
            }
            words += __builtin_popcount((uint)(~mask & ((mask << 1) | (gaps & 1))));
            // Keep the preceding gap and any carried lanes together.
            gaps = mask >> 31;
        }
        // A sentinel lets empty vectors and the first newline share one store.
        uint first = __builtin_ctzl((size)breaks | (1UL << 32));
        lines->ends[count] = i + first;
        count += breaks != 0;
        // The first break covers the common one-break vector. Dense lines
        // reuse the ordinary bit iteration for any remaining breaks.
        count = queue_lines(lines->ends, count, i, breaks & (breaks - 1));
    }
    lines->end_lines = count;
    lines->words += words;
    lines->preceding_gap = gaps & 1;
    lines->carry = gaps >> 1;
    lines->next = i;
}

// Queue a batch of line ends while visiting each vector for the word count.
static __attribute__((noinline)) void scan_lines(Lines *lines) {
    Str body = lines->body;
    lines->read_line = lines->end_lines = 0;
    if (has_avx2 && lines->next < body.n && body.n - lines->next >= 32)
        scan_wide(lines);
    int count_words = body.n > 400;
    for (size i = lines->next; i < body.n && lines->end_lines <= COUNT(lines->ends) - 16; i += 16) {
        // Stay on the wide path until only the final partial vector remains.
        if (has_avx2 && body.n - i >= 32) break;
        Vec w = *(const Vec *)(body.p + i);
        uint valid = body.n - i >= 16 ? 0xffff : (1u << (body.n - i)) - 1;
        lines->next = i + 16;
        uint breaks = __builtin_ia32_pmovmskb128(w == SPLAT('\n')) & valid;
        if (count_words) {
            Vec ws = (w == SPLAT(' ')) | ((w > SPLAT(8)) & (w < SPLAT(14)));
            uint gaps = lines->carry | (uint)__builtin_ia32_pmovmskb128(ws);
            if (__builtin_ia32_pmovmskb128(w)) {
                // These are the five UTF-8 lead bytes that can start JS whitespace.
                uint leads = __builtin_ia32_pmovmskb128(
                    (w == SPLAT((char)0xc2)) | (w == SPLAT((char)0xe1)) | (w == SPLAT((char)0xe2)) |
                    (w == SPLAT((char)0xe3)) | (w == SPLAT((char)0xef))) & valid;
                for (; leads; leads &= leads - 1) {
                    uint j = first_bit(leads);
                    size width = space_width(body, i + j);
                    gaps |= ((1u << width) - 1) << j;
                }
            }
            lines->words += bit_count(~gaps & ((gaps << 1) | lines->preceding_gap) & valid);
            lines->preceding_gap = (gaps >> 15) & 1;
            lines->carry = gaps >> 16;
        }
        lines->end_lines = queue_lines(lines->ends, lines->end_lines, i, breaks);
    }
    if (lines->next >= body.n && lines->end_lines < COUNT(lines->ends))
        lines->ends[lines->end_lines++] = body.n;
}

static void advance(Lines *lines) {
    lines->at = lines->end + 1;
    if (lines->at > lines->body.n) {
        lines->line = (Str){0};
        return;
    }
    if (lines->read_line == lines->end_lines) scan_lines(lines);
    lines->end = lines->ends[lines->read_line++];
    lines->line = slice(lines->body, lines->at, lines->end);
}

static void code_block(Buf *html, Lines *lines) {
    Str language = trim_line(tail(lines->line, 3));
    if (language.n) {
        ADD(html, "<pre><code class=\\\"language-");
        append(html, language);
        ADD(html, "\\\">");
    } else {
        ADD(html, "<pre><code>");
    }
    Str body = lines->body;
    size first = lines->end + 1;
    advance(lines);
    while (lines->at <= body.n) {
        if (starts(lines->line, S("```")) && only_indent(tail(lines->line, 3))) break;
        advance(lines);
    }
    if (lines->at <= body.n) escape_code(html, slice(body, first, lines->at));
    else if (first <= body.n) {
        escape_code(html, tail(body, first));
        ADD(html, "\\n");
    }
    ADD(html, "</code></pre>\\n");
    if (lines->at <= body.n) advance(lines);
}

static void quote_block(Buf *html, Lines *lines, LinkContext *context) {
    Buf *content = start_markdown_text();
    do {
        Str trimmed = trim_line(tail(lines->line, 1));
        if (trimmed.n) {
            if (content->n) put(content, '\n');
            append(content, trimmed);
        }
        advance(lines);
    } while (at(lines->line, 0) == '>');
    ADD(html, "<blockquote>\\n");
    if (content->n) {
        ADD(html, "<p>");
        inline_html(html, take(content), context, 0);
        ADD(html, "</p>\\n");
    }
    ADD(html, "</blockquote>\\n");
}

static void list_block(Buf *html, Lines *lines, size marker, LinkContext *context) {
    int ordered = marker != 2;
    size start = ordered ? decimal(slice(lines->line, 0, marker - 2)) : 1;
    if (start != 1) {
        ADD(html, "<ol start=\\\"");
        append(html, number(start));
        ADD(html, "\\\">\\n");
    } else {
        append(html, ordered ? S("<ol>\\n") : S("<ul>\\n"));
    }
    for (;;) {
        Str item = trim_line(tail(lines->line, marker));
        advance(lines);
        if (continued(lines->line)) {
            Buf *merged = start_markdown_text();
            append(merged, item);
            do {
                put(merged, '\n');
                append(merged, trim_line(lines->line));
                advance(lines);
            } while (continued(lines->line));
            item = take(merged);
        }
        ADD(html, "<li>");
        inline_html(html, item, context, 0);
        ADD(html, "</li>\\n");
        marker = list_marker(lines->line);
        if (!marker || (marker != 2) != ordered) break;
    }
    append(html, ordered ? S("</ol>\\n") : S("</ul>\\n"));
}

static Str paragraph(Lines *lines, int *kind) {
    size from = lines->at, to;
    Buf *text = 0;
    do {
        Str line = lines->line;
        if (line.n && (indent(line.p[0]) || indent(line.p[line.n - 1]))) {
            if (!text) text = start_markdown_text();
            append(text, slice(lines->body, from, lines->at));
            append(text, trim_line(line));
            from = lines->end;
        }
        to = lines->end;
        advance(lines);
        *kind = block(lines->line);
    } while (*kind == PARAGRAPH);
    Str last = slice(lines->body, from, to);
    if (!text || !text->n) return last;
    append(text, last);
    return take(text);
}

typedef struct Heading Heading;
struct Heading {
    Heading *next;
    Value item, fields[3];
};

Markdown markdown(Str body, LinkContext *context) {
    Lines lines = {.body = body, .end = NPOS, .preceding_gap = 1};
    Markdown result = {.excerpt = {.kind = RENDERED}};
    Buf html = start_html();
    Entry local_ids[8] = {0};
    Map ids = {.entries = local_ids, .cap = COUNT(local_ids)};
    Heading *toc = 0, **next = &toc;
    size count = 0;
    int excerpt = 0;
    advance(&lines);
    int kind = block(lines.line);

    while (lines.at <= body.n) {
        if (kind == FENCE) {
            code_block(&html, &lines);
        } else if (kind == BREAK) {
            ADD(&html, "<hr>\\n");
            advance(&lines);
        } else if (kind == BLANK) {
            do advance(&lines); while (lines.at <= body.n && !lines.line.n);
        } else if (kind != PARAGRAPH) {
            Str text = trim_line(tail(lines.line, kind));
            PHASE_BEGIN(markdown_phase, SLUGS);
            Str id = unique_slug(&ids, slugify(text));
            PHASE_END(markdown_phase);
            ADD(&html, "<h");
            put(&html, '0' + kind);
            ADD(&html, " id=\\\"");
            append(&html, id);
            ADD(&html, "\\\">");
            size from = html.n;
            inline_html(&html, text, context, 0);
            if (kind == 2 || kind == 3) {
                Heading *heading = NEW(Heading, 1);
                heading->next = 0;
                heading->fields[0] = numeric(kind);
                heading->fields[1] = literal_string(id);
                heading->fields[2] = rendered(tail(take(&html), from));
                heading->item = record_values(HEADING_FIELDS, heading->fields, COUNT(heading->fields));
                *next = heading;
                next = &heading->next;
                count++;
            }
            ADD(&html, "</h");
            put(&html, '0' + kind);
            ADD(&html, ">\\n");
            advance(&lines);
        } else if (at(lines.line, 0) == '>') {
            quote_block(&html, &lines, context);
        } else {
            size marker = list_marker(lines.line);
            if (marker) {
                list_block(&html, &lines, marker, context);
            } else {
                Str text = paragraph(&lines, &kind);
                ADD(&html, "<p>");
                size from = html.n;
                inline_html(&html, text, context, 0);
                if (!excerpt) {
                    result.excerpt = rendered(tail(take(&html), from));
                    excerpt = 1;
                }
                ADD(&html, "</p>\\n");
                continue;
            }
        }
        kind = block(lines.line);
    }

    const Value **items = NEW(const Value *, count);
    for (size i = 0; toc; i++, toc = toc->next) items[i] = &toc->item;
    result.minutes = lines.words ? (lines.words + 199) / 200 : 1;
    result.toc = array(items, count);
    finish_html(&html);
    result.html = rendered(take(&html));
    return result;
}

#include "core.h"

typedef char Vec __attribute__((vector_size(16), aligned(1), may_alias));
#define SPLAT(c) ((Vec){c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c})

static int escapable(int c) {
    switch (c) {
        case '\\': case '`': case '*': case '_': case '[': case ']':
        case '(': case ')': case '#': case '+': case '-': case '.': case '!': case '>':
            return 1;
        default: return 0;
    }
}

typedef struct { char text[8]; byte length; } Entity;
static const Entity escapes[64] = {
    ['&'] = {"&amp;", 5}, ['<'] = {"&lt;", 4}, ['>'] = {"&gt;", 4},
    ['"'] = {"&quot;", 6}, ['\''] = {"&#39;", 5},
};

enum InlineMark { PLAIN, ENTITY, CONTROL, BACKSLASH, BACKTICK, STAR, LABEL, SURROGATE };
static const byte inline_class[256] = {
    [0 ... 31] = CONTROL, ['&'] = ENTITY, ['<'] = ENTITY, ['>'] = ENTITY,
    ['"'] = ENTITY, ['\''] = ENTITY, ['\\'] = BACKSLASH, ['\x60'] = BACKTICK,
    ['*'] = STAR, ['['] = LABEL, [0xed] = SURROGATE,
};

static inline uint marks(Vec bytes, Vec surrogate) {
    // The grouped byte tests include a few plain bytes, handled by inline_class.
    Vec hits = ((bytes ^ SPLAT(-128)) < SPLAT(-96)) |
        ((bytes & SPLAT((char)0xfa)) == SPLAT('"')) |
        ((bytes & SPLAT((char)0x9d)) == SPLAT(0x1c)) |
        (bytes == SPLAT('[')) | (bytes == SPLAT('*')) |
        (bytes == SPLAT('\x60')) | (bytes == surrogate);
    return __builtin_ia32_pmovmskb128(hits);
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

static void inline_html(Buf *out, Str s, ResolveLink resolve, void *context, uint depth) {
    if (!s.n) return;
    if (depth > 128) fail();
    Vec surrogate = SPLAT(latin1 ? 0 : (char)0xed);
    int labels = 1;
    size i = 0;
    char *dst = room(out, s.n * 6 + 16);
    while (i < s.n) {
        // Both slices have space for a vector at the end. A later token replaces
        // any speculatively copied bytes beyond the current plain text.
        Vec bytes = *(const Vec *)(s.p + i);
        *(Vec *)dst = bytes;
        uint hits = marks(bytes, surrogate);
        size plain = hits ? first_bit(hits) : 16;
        if (plain >= s.n - i) {
            dst += s.n - i;
            break;
        }
        i += plain;
        dst += plain;
        if (!hits) continue;

        unsigned c = (byte)s.p[i];
        size next = i + 1;
        switch (inline_class[c]) {
            case ENTITY: {
                const Entity *entity = &escapes[c];
                __builtin_memcpy(dst, entity->text, sizeof(entity->text));
                dst += entity->length;
                break;
            }
            case CONTROL:
                if (c == '\n') {
                    *dst++ = '\\';
                    *dst++ = 'n';
                } else {
                    out->n = dst - out->p;
                    json_ascii(out, c);
                    dst = out->p + out->n;
                }
                break;
            case BACKSLASH:
                c = at(s, next);
                if (!escapable(c)) {
                    *dst++ = '\\';
                    *dst++ = '\\';
                } else {
                    next++;
                    if (c == '>') {
                        __builtin_memcpy(dst, "&gt;", 4);
                        dst += 4;
                    } else {
                        if (c == '\\') *dst++ = '\\';
                        *dst++ = c;
                    }
                }
                break;
            case BACKTICK: {
                size end = arena_char(s, next, '\x60');
                if (end == NPOS || end == next) goto literal;
                out->n = dst - out->p;
                ADD(out, "<code>");
                json(out, slice(s, next, end), 1, 0);
                ADD(out, "</code>");
                next = end + 1;
                dst = room(out, (s.n - next) * 6 + 16);
                break;
            }
            case STAR: {
                int strong = at(s, next) == '*';
                next += strong;
                size end = closer(s, next, '*', strong, !strong);
                if (end == NPOS || end == next) {
                    *dst++ = '*';
                    if (strong) *dst++ = '*';
                    break;
                }
                out->n = dst - out->p;
                append(out, strong ? S("<strong>") : S("<em>"));
                inline_html(out, slice(s, next, end), resolve, context, depth + 1);
                append(out, strong ? S("</strong>") : S("</em>"));
                next = end + 1 + strong;
                dst = room(out, (s.n - next) * 6 + 16);
                break;
            }
            case LABEL: {
                if (!labels) goto literal;
                size label_end = closer(s, next, ']', 0, 0);
                if (label_end == NPOS) {
                    labels = 0;
                    goto literal;
                }
                if (at(s, label_end + 1) != '(') goto literal;
                size end = arena_char(s, label_end + 2, ')');
                if (end == NPOS || end == label_end + 2) goto literal;
                Str target = slice(s, label_end + 2, end);
                if (has_space(target)) goto literal;
                Str href = resolve(context, target);
                out->n = dst - out->p;
                ADD(out, "<a href=\\\"");
                json(out, href, 1, 0);
                ADD(out, "\\\">");
                inline_html(out, slice(s, next, label_end), resolve, context, depth + 1);
                ADD(out, "</a>");
                next = end + 1;
                dst = room(out, (s.n - next) * 6 + 16);
                break;
            }
            case SURROGATE: {
                next = i;
                uint cp = decode(s, &next);
                if (cp < 0xd800 || cp > 0xdfff) goto literal;
                out->n = dst - out->p;
                json_code_unit(out, cp);
                dst = out->p + out->n;
                break;
            }
            case PLAIN:
literal:
                *dst++ = s.p[i];
                next = i + 1;
                break;
        }
        i = next;
    }
    out->n = dst - out->p;
}

enum Block { PARAGRAPH, FENCE = 7, BREAK, BLANK };

static int fence_char(int c) {
    return letter(c) || digit(c) || c == '_' || c == '+' || c == '-';
}

static int block(Str line) {
    int c = at(line, 0);
    if (c == -1 || (indent(c) && only_indent(tail(line, 1)))) return BLANK;
    if (c == '#') {
        size n = 1;
        while (at(line, n) == '#') n++;
        if (n <= 6 && indent(at(line, n)) && !only_indent(tail(line, n + 1))
            && !has_line_separator(tail(line, n))) return n;
    } else if (c == '`' && starts(line, S("```"))) {
        size i = 3;
        while (fence_char(at(line, i))) i++;
        if (only_indent(tail(line, i))) return FENCE;
    } else if ((c == '-' || c == '*') && at(line, 1) == c && at(line, 2) == c
        && only_indent(tail(line, 3))) {
        return BREAK;
    }
    return PARAGRAPH;
}

static size list_marker(Str line) {
    int c = at(line, 0);
    if (c == '-' || c == '*') return at(line, 1) == ' ' && !has_line_separator(line) ? 2 : 0;
    if (!digit(c)) return 0;
    size i = 1;
    while (i < 10 && digit(at(line, i))) i++;
    return i < 10 && at(line, i) == '.' && at(line, i + 1) == ' ' &&
        !has_line_separator(line) ? i + 2 : 0;
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
    uint breaks, preceding_gap, carry;
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

// A vector may contain several lines. Keep its remaining breaks for advance()
// and count word starts when loading the vector.
static size scan_line_end(Lines *lines) {
    Str body = lines->body;
    while (!lines->breaks) {
        size i = lines->next;
        if (i >= body.n) return body.n;
        Vec w = *(const Vec *)(body.p + i);
        uint valid = body.n - i >= 16 ? 0xffff : (1u << (body.n - i)) - 1;
        lines->next += 16;
        lines->breaks = __builtin_ia32_pmovmskb128(w == SPLAT('\n')) & valid;
        if (body.n > 400) {
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
    }
    size end = lines->next - 16 + first_bit(lines->breaks);
    lines->breaks &= lines->breaks - 1;
    return end;
}

static void advance(Lines *lines) {
    lines->at = lines->end + 1;
    if (lines->at > lines->body.n) {
        lines->line = (Str){0};
        return;
    }
    lines->end = scan_line_end(lines);
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
    if (lines->at <= body.n) json(html, slice(body, first, lines->at), 1, 0);
    else if (first <= body.n) {
        json(html, tail(body, first), 1, 0);
        ADD(html, "\\n");
    }
    ADD(html, "</code></pre>\\n");
    if (lines->at <= body.n) advance(lines);
}

static void quote_block(Buf *html, Lines *lines, ResolveLink resolve, void *context) {
    Buf content = {0};
    do {
        Str trimmed = trim_line(tail(lines->line, 1));
        if (trimmed.n) {
            if (content.n) put(&content, '\n');
            append(&content, trimmed);
        }
        advance(lines);
    } while (at(lines->line, 0) == '>');
    ADD(html, "<blockquote>\\n");
    if (content.n) {
        ADD(html, "<p>");
        inline_html(html, take(&content), resolve, context, 0);
        ADD(html, "</p>\\n");
    }
    ADD(html, "</blockquote>\\n");
}

static void list_block(Buf *html, Lines *lines, size marker, ResolveLink resolve, void *context) {
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
            Buf merged = {0};
            append(&merged, item);
            do {
                put(&merged, '\n');
                append(&merged, trim_line(lines->line));
                advance(lines);
            } while (continued(lines->line));
            item = take(&merged);
        }
        ADD(html, "<li>");
        inline_html(html, item, resolve, context, 0);
        ADD(html, "</li>\\n");
        marker = list_marker(lines->line);
        if (!marker || (marker != 2) != ordered) break;
    }
    append(html, ordered ? S("</ol>\\n") : S("</ul>\\n"));
}

static Str paragraph(Lines *lines, int *kind) {
    size from = lines->at, to;
    Buf text = {0};
    do {
        Str line = lines->line;
        if (indent(at(line, 0)) || indent(at(line, line.n - 1))) {
            append(&text, slice(lines->body, from, lines->at));
            append(&text, trim_line(line));
            from = lines->end;
        }
        to = lines->end;
        advance(lines);
        *kind = block(lines->line);
    } while (*kind == PARAGRAPH);
    Str last = slice(lines->body, from, to);
    if (!text.n) return last;
    append(&text, last);
    return take(&text);
}

typedef struct Heading Heading;
struct Heading {
    Heading *next;
    Value item, level, id, html;
    const Value *fields[3];
};

Markdown markdown(Str body, ResolveLink resolve, void *context) {
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
            advance(&lines);
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
            inline_html(&html, text, resolve, context, 0);
            if (kind == 2 || kind == 3) {
                Heading *heading = NEW(Heading, 1);
                heading->next = 0;
                heading->level = numeric(kind);
                heading->id = string(id);
                heading->html = rendered(tail(take(&html), from));
                heading->fields[0] = &heading->level;
                heading->fields[1] = &heading->id;
                heading->fields[2] = &heading->html;
                heading->item = record(HEADING_FIELDS, heading->fields, COUNT(heading->fields));
                *next = heading;
                next = &heading->next;
                count++;
            }
            ADD(&html, "</h");
            put(&html, '0' + kind);
            ADD(&html, ">\\n");
            advance(&lines);
        } else if (at(lines.line, 0) == '>') {
            quote_block(&html, &lines, resolve, context);
        } else {
            size marker = list_marker(lines.line);
            if (marker) {
                list_block(&html, &lines, marker, resolve, context);
            } else {
                Str text = paragraph(&lines, &kind);
                ADD(&html, "<p>");
                size from = html.n;
                inline_html(&html, text, resolve, context, 0);
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

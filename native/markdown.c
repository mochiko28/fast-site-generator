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

static size closer(Str s, size from, Str mark, int single_star) {
    for (size i = from; (i = find_char(s, i, mark.p[0])) != NPOS; i++) {
        if (!starts(tail(s, i), mark)) continue;
        size slash = i;
        while (slash > from && s.p[slash - 1] == '\\') slash--;
        if ((i - slash) & 1) continue;
        if (!single_star || at(s, i + 1) != '*') return i;
        i++;
    }
    return NPOS;
}

static const Str escapes[64] = {
    ['&'] = S("&amp;"), ['<'] = S("&lt;"), ['>'] = S("&gt;"), ['"'] = S("&quot;"), ['\''] = S("&#39;"),
};

static uint marks(Vec bytes) {
    Vec hit =
        (bytes == SPLAT('\\')) | (bytes == SPLAT('`')) | (bytes == SPLAT('*')) | (bytes == SPLAT('[')) |
        (bytes == SPLAT('&')) | (bytes == SPLAT('<')) | (bytes == SPLAT('>')) |
        (bytes == SPLAT('"')) | (bytes == SPLAT('\'')) |
        ((bytes ^ SPLAT(-128)) < SPLAT(-96));
    if (!latin1) hit |= bytes == SPLAT((char)0xed);
    return __builtin_ia32_pmovmskb128(hit);
}

// Copy plain runs as they are scanned. A vector store can include the mark
// and bytes past it, which the following token overwrites as needed.
static size copy_inline_text(Buf *out, Str s, size i) {
    size from = i;
    char *dst = room(out, s.n - i);
    for (; s.n - i >= 16; i += 16) {
        Vec bytes = *(const Vec *)(s.p + i);
        *(Vec *)(dst + i - from) = bytes;
        uint hit = marks(bytes);
        if (hit) {
            i += first_bit(hit);
            out->n += i - from;
            return i;
        }
    }
    if (i < s.n) {
        // A Markdown slice has a readable SIMD tail in the arena.
        uint hit = marks(*(const Vec *)(s.p + i)) & ((1u << (s.n - i)) - 1);
        size n = hit ? (size)first_bit(hit) : s.n - i;
        copy(dst + i - from, s.p + i, n);
        i += n;
    }
    out->n += i - from;
    return i;
}

static void inline_html(Buf *out, Str s, ResolveLink resolve, void *context, uint depth) {
    if (depth > 128) fail();
    int labels = 1;
    for (size i = 0; i < s.n && (i = copy_inline_text(out, s, i)) < s.n;) {
        unsigned c = (byte)s.p[i];
        size next = i + 1;
        if (c == '\n') {
            ADD(out, "\\n");
        } else if (c < COUNT(escapes) && escapes[c].n) {
            append(out, escapes[c]);
        } else if (c <= 31) {
            json_ascii(out, c);
        } else if (c == '\\') {
            c = at(s, next);
            if (!escapable(c)) ADD(out, "\\\\");
            else {
                next++;
                if (c == '>') ADD(out, "&gt;");
                else if (c == '\\') ADD(out, "\\\\");
                else put(out, c);
            }
        } else if (c == '`') {
            size end = find_char(s, next, '`');
            if (end == NPOS || end == next) goto literal;
            ADD(out, "<code>");
            json(out, slice(s, next, end), 1, 0);
            ADD(out, "</code>");
            next = end + 1;
        } else if (c == '*') {
            int strong = at(s, next) == '*';
            next += strong;
            size end = closer(s, next, strong ? S("**") : S("*"), !strong);
            if (end == NPOS || end == next) {
                put(out, '*');
                if (strong) put(out, '*');
                i = next;
                continue;
            }
            append(out, strong ? S("<strong>") : S("<em>"));
            inline_html(out, slice(s, next, end), resolve, context, depth + 1);
            append(out, strong ? S("</strong>") : S("</em>"));
            next = end + 1 + strong;
        } else if (c == '[' && labels) {
            size label_end = closer(s, next, S("]"), 0);
            if (label_end == NPOS) {
                labels = 0;
                goto literal;
            }
            if (at(s, label_end + 1) != '(') goto literal;
            size end = find_char(s, label_end + 2, ')');
            if (end == NPOS || end == label_end + 2) goto literal;
            Str target = slice(s, label_end + 2, end);
            if (has_space(target)) goto literal;
            Str href = resolve(context, target);
            ADD(out, "<a href=\\\"");
            json(out, href, 1, 0);
            ADD(out, "\\\">");
            inline_html(out, slice(s, next, label_end), resolve, context, depth + 1);
            ADD(out, "</a>");
            next = end + 1;
        } else if (c == 0xed && !latin1) {
            next = i;
            uint cp = decode(s, &next);
            if (cp < 0xd800 || cp > 0xdfff) goto literal;
            json_code_unit(out, cp);
        } else {
literal:
            put(out, s.p[i]);
            next = i + 1;
        }
        i = next;
    }
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
    uint *ends;
    size count, cap;
} Lines;

static void add_line_end(Lines *lines, size end) {
    if (lines->count == lines->cap) {
        size cap = lines->cap * 2;
        uint *ends = NEW(uint, cap);
        copy(ends, lines->ends, lines->count * sizeof(*ends));
        lines->ends = ends;
        lines->cap = cap;
    }
    lines->ends[lines->count++] = end;
}

static void advance(Lines *lines) {
    lines->at = lines->end + 1;
    if (lines->at > lines->body.n) {
        lines->line = (Str){0};
        return;
    }
    lines->end = lines->ends[lines->next++];
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

static uint bit_count(uint bits) {
    bits -= (bits >> 1) & 0x5555;
    bits = (bits & 0x3333) + ((bits >> 2) & 0x3333);
    bits = (bits + (bits >> 4)) & 0x0f0f;
    return (bits + (bits >> 8)) & 31;
}

// Index line boundaries and count word starts in the same pass.
static size index_lines(Lines *lines) {
    Str body = lines->body;
    int count_words = body.n > 400;
    size words = 0, i = 0;
    uint last = 1, carry = 0;
    for (; body.n - i >= 16; i += 16) {
        Vec w = *(const Vec *)(body.p + i);
        Vec lf = w == SPLAT('\n');
        uint breaks = __builtin_ia32_pmovmskb128(lf);
        for (uint bits = breaks; bits; bits &= bits - 1)
            add_line_end(lines, i + first_bit(bits));

        if (count_words) {
            Vec ws = (w == SPLAT(' ')) | ((w > SPLAT(8)) & (w < SPLAT(14)));
            uint gaps = carry | (uint)__builtin_ia32_pmovmskb128(ws);
            uint unicode = __builtin_ia32_pmovmskb128(
                (w == SPLAT((char)0xc2)) | (w == SPLAT((char)0xe1)) | (w == SPLAT((char)0xe2)) |
                (w == SPLAT((char)0xe3)) | (w == SPLAT((char)0xef)));
            for (; unicode; unicode &= unicode - 1) {
                uint j = first_bit(unicode);
                size width = space_width(body, i + j);
                gaps |= ((1u << width) - 1) << j;
            }
            words += bit_count(~gaps & ((gaps << 1) | last) & 0xffff);
            last = (gaps >> 15) & 1;
            carry = gaps >> 16;
        }
    }

    // A multibyte whitespace character can straddle the final vector and tail.
    i += (carry & 1) + (carry >> 1);
    while (i < body.n) {
        if (body.p[i] == '\n') add_line_end(lines, i);
        if (count_words) {
            size width = space_width(body, i);
            if (width) {
                last = 1;
                i += width;
                continue;
            }
            words += last;
            last = 0;
        }
        i++;
    }
    add_line_end(lines, body.n);
    return words ? (words + 199) / 200 : 1;
}

typedef struct Heading Heading;
struct Heading {
    Heading *next;
    Value item, level, id, html;
    Field fields[3];
};

Markdown markdown(Str body, ResolveLink resolve, void *context) {
    uint line_ends[128];
    Lines lines = {.body = body, .end = NPOS, .ends = line_ends, .cap = COUNT(line_ends)};
    Markdown result = {.excerpt = {.kind = RENDERED}, .minutes = index_lines(&lines)};
    Buf html = {0};
    if (body.n) room(&html, body.n + body.n / 2 + 128);
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
                heading->fields[0] = (Field){PROP_LEVEL, &heading->level};
                heading->fields[1] = (Field){PROP_ID, &heading->id};
                heading->fields[2] = (Field){PROP_HTML, &heading->html};
                heading->item = record(heading->fields, COUNT(heading->fields));
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
    result.toc = array(items, count);
    result.html = rendered(take(&html));
    return result;
}

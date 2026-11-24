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

static void inline_html(Buf *out, Str s, ResolveLink resolve, void *context, uint depth) {
    if (depth > 128) fail();
    // A match can consume later matches in the same vector.
    size from = 0, base = 0, block_start = 0;
    uint pending = 0;
    while (base < s.n || pending) {
        if (!pending) {
            // Markdown slices point into the arena, which has a readable SIMD tail.
            block_start = base;
            pending = marks(*(const Vec *)(s.p + base));
            size left = s.n - base;
            if (left < 16) {
                pending &= (1u << left) - 1;
                base = s.n;
            } else {
                base += 16;
            }
            if (!pending) continue;
        }
        size i = block_start + __builtin_ctz(pending);
        pending &= pending - 1;
        if (i < from) continue;

        int c = (byte)s.p[i];
        size next = i + 1;
        Str prefix = {0}, suffix = {0}, fragment = {0}, href = {0};
        int mode = 0;
        if (c < (int)COUNT(escapes) && escapes[c].n) {
            prefix = escapes[c];
        } else if (c == '\\') {
            if (escapable(at(s, next))) {
                c = s.p[next++];
                if (c == '>') prefix = escapes[c];
                else if (c == '\\') prefix = S("\\\\");
                else prefix = slice(s, next - 1, next);
            } else {
                prefix = S("\\\\");
            }
        } else if (c == '`') {
            size end = find_char(s, next, '`');
            if (end == NPOS || end == next) continue;
            prefix = S("<code>");
            suffix = S("</code>");
            fragment = slice(s, next, end);
            next = end + 1;
            mode = 1;
        } else if (c == '*') {
            int strong = at(s, next) == '*';
            next += strong;
            size end = closer(s, next, strong ? S("**") : S("*"), !strong);
            if (end == NPOS || end == next) {
                if (strong) {
                    append(out, slice(s, from, next));
                    from = next;
                }
                continue;
            }
            prefix = strong ? S("<strong>") : S("<em>");
            suffix = strong ? S("</strong>") : S("</em>");
            fragment = slice(s, next, end);
            next = end + 1 + strong;
            mode = 2;
        } else if (c == '[') {
            size label_end = closer(s, next, S("]"), 0);
            if (label_end == NPOS || at(s, label_end + 1) != '(') continue;
            size end = find_char(s, label_end + 2, ')');
            if (end == NPOS || end == label_end + 2) continue;
            Str target = slice(s, label_end + 2, end);
            if (has_space(target)) continue;
            href = resolve(context, target);
            prefix = S("<a href=\\\"");
            suffix = S("</a>");
            fragment = slice(s, next, label_end);
            next = end + 1;
            mode = 2;
        } else if (c <= 31) {
            append(out, slice(s, from, i));
            json_ascii(out, c);
            from = next;
            continue;
        } else if (c == 0xed && !latin1) {
            next = i;
            uint cp = decode(s, &next);
            if (cp >= 0xd800 && cp <= 0xdfff) {
                append(out, slice(s, from, i));
                json_code_unit(out, cp);
                from = next;
            }
            continue;
        } else {
            continue;
        }

        append(out, slice(s, from, i));
        append(out, prefix);
        if (c == '[' && suffix.n) {
            json(out, href, 1, 0);
            ADD(out, "\\\">");
        }
        if (mode == 1) json(out, fragment, 1, 0);
        else if (mode == 2) inline_html(out, fragment, resolve, context, depth + 1);
        append(out, suffix);
        from = next;
        if (next >= base) {
            base = next;
            pending = 0;
        }
    }
    append(out, tail(s, from));
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

typedef struct { Str body, line; size at, end; } Lines;

static void advance(Lines *lines) {
    lines->at = lines->end + 1;
    if (lines->at > lines->body.n) {
        lines->line = (Str){0};
        return;
    }
    size end = find_char(lines->body, lines->at, '\n');
    lines->end = end == NPOS ? lines->body.n : end;
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

static size reading_minutes(Str body) {
    if (body.n <= 400) return 1;
    size words = 0, i = 0;
    uint last = 1;
    for (; body.n - i >= 16; i += 16) {
        Vec w = *(const Vec *)(body.p + i);
        Vec ws = (w == SPLAT(' ')) | ((w > SPLAT(8)) & (w < SPLAT(14)));
        uint gaps = __builtin_ia32_pmovmskb128(ws);
        uint unicode = __builtin_ia32_pmovmskb128(
            (w == SPLAT((char)0xc2)) | (w == SPLAT((char)0xe1)) | (w == SPLAT((char)0xe2)) |
            (w == SPLAT((char)0xe3)) | (w == SPLAT((char)0xef)));
        if (unicode) {
            do {
                uint j = __builtin_ctz(unicode);
                if (space_width(body, i + j)) goto scalar;
                unicode &= unicode - 1;
            } while (unicode);
        }
        words += bit_count(~gaps & ((gaps << 1) | last) & 0xffff);
        last = gaps >> 15;
    }
scalar:
    while (i < body.n) {
        size width = space_width(body, i);
        if (width) {
            last = 1;
            i += width;
        } else {
            words += last;
            last = 0;
            i++;
        }
    }
    return words ? (words + 199) / 200 : 1;
}

typedef struct Heading Heading;
struct Heading {
    Heading *next;
    Value item, level, id, html;
    Field fields[3];
};

Markdown markdown(Str body, ResolveLink resolve, void *context) {
    Markdown result = {.excerpt = {.kind = RENDERED}, .minutes = reading_minutes(body)};
    Buf html = {0};
    if (body.n) room(&html, body.n + body.n / 2 + 128);
    Map ids = {0};
    Heading *toc = 0, **next = &toc;
    size count = 0;
    int excerpt = 0;
    Lines lines = {.body = body, .end = NPOS};
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
            Str id = unique_slug(&ids, slugify(text));
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
                heading->fields[0] = (Field){S("level"), &heading->level};
                heading->fields[1] = (Field){S("id"), &heading->id};
                heading->fields[2] = (Field){S("html"), &heading->html};
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

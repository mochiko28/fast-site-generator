// Instantiated by markdown.c with the baseline or AVX2 marker classifier.
static INLINE_TARGET void INLINE_HTML(Buf *out, Str s, LinkContext *context, uint depth) {
    if (!s.n) return;
    if (depth > 128) fail();
#if INLINE_SHUFFLE
    const Vec surrogate = SPLAT(latin1 ? 0 : (char)0xed);
#else
    const InlineClasses *classes = &inline_classes[latin1];
#endif
    int labels = 1;
    size i = 0;
    char *dst = room(out, s.n * 6 + 16);
    while (i < s.n) {
        // Both slices have space for a vector at the end. A later token replaces
        // any speculatively copied bytes beyond the current plain text.
        Vec bytes = *(const Vec *)(s.p + i);
        *(Vec *)dst = bytes;
#if INLINE_SHUFFLE
        // PSHUFB uses the low nibble for ASCII and clears lanes with the high
        // bit set. Backslash shares a low nibble with less-than.
        const Vec choices = {'`', 0, '"', 0, 0, 0, '&', '\'', 0, 0, '*', '[', '<', 0, '>', 0};
        Vec marks = (bytes == __builtin_ia32_pshufb128(choices, bytes)) |
            (bytes == SPLAT('\\')) | (bytes == surrogate) |
            ((bytes & SPLAT((char)0xe0)) == SPLAT(0));
        uint hits = __builtin_ia32_pmovmskb128(marks);
#else
        uint hits = marks(bytes, classes);
#endif
        size plain = hits ? first_bit(hits) : 16;
        if (plain >= s.n - i) {
            dst += s.n - i;
            break;
        }
        i += plain;
        dst += plain;
        if (!hits) continue;

        unsigned c = (byte)s.p[i];
        while (c < 64 && c != '*') {
            const Entity *entity = &escapes[c];
            __builtin_memcpy(dst, entity, sizeof(*entity));
            dst += entity->length;
            i++;
            // The remaining hits in this vector still describe the original source.
            size base = i - plain - 1;
            hits &= hits - 1;
            size end = hits ? base + first_bit(hits) : base + 16;
            if (end > s.n) end = s.n;
            *(Vec *)dst = *(const Vec *)(s.p + i);
            dst += end - i;
            i = end;
            if (!hits || i == s.n) break;
            plain = i - base;
            c = (byte)s.p[i];
        }
        if (!hits || i == s.n) continue;
        size next = i + 1;
        switch (c) {
            case '\\':
                c = at(s, next);
                if (c >= COUNT(escapable) || !escapable[c]) {
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
            case 0x60: {
                size end = arena_char(s, next, '\x60');
                if (end == NPOS || end == next) goto literal;
                out->n = dst - out->p;
                ADD(out, "<code>");
                escape_code(out, slice(s, next, end));
                ADD(out, "</code>");
                next = end + 1;
                dst = room(out, (s.n - next) * 6 + 16);
                break;
            }
            case '*': {
                int strong = at(s, next) == '*';
                next += strong;
                // Consume a plain emphasis span directly when its closer is
                // also the next marked byte in the SIMD load.
                uint remaining = hits & (-1u << (plain + 1 + strong));
                if (remaining) {
                    size end = i - plain + first_bit(remaining);
                    if (end > next && end < s.n && s.p[end] == '*' &&
                        (at(s, end + 1) == '*') == strong) {
                        if (strong) {
                            __builtin_memcpy(dst, "<strong>", 8);
                            dst += 8;
                        } else {
                            __builtin_memcpy(dst, "<em>", 4);
                            dst += 4;
                        }
                        *(Vec *)dst = *(const Vec *)(s.p + next);
                        dst += end - next;
                        if (strong) {
                            __builtin_memcpy(dst, "</strong>", 9);
                            dst += 9;
                        } else {
                            __builtin_memcpy(dst, "</em>", 5);
                            dst += 5;
                        }
                        // Plain emphasis fits in the existing six-byte expansion bound.
                        i = end + 1 + strong;
                        continue;
                    }
                }
                size end = closer(s, next, '*', strong, !strong);
                if (end == NPOS || end == next) {
                    *dst++ = '*';
                    if (strong) *dst++ = '*';
                    break;
                }
                out->n = dst - out->p;
                append(out, strong ? S("<strong>") : S("<em>"));
                INLINE_HTML(out, slice(s, next, end), context, depth + 1);
                append(out, strong ? S("</strong>") : S("</em>"));
                next = end + 1 + strong;
                dst = room(out, (s.n - next) * 6 + 16);
                break;
            }
            case '[': {
                if (!labels) goto literal;
                size label_end = closer(s, next, ']', 0, 0);
                if (label_end == NPOS) {
                    labels = 0;
                    goto literal;
                }
                if (at(s, label_end + 1) != '(') goto literal;
                size end = link_end(s, label_end + 2);
                if (end == NPOS || end == label_end + 2) goto literal;
                Str target = slice(s, label_end + 2, end);
                Value href = resolve_link(context, target);
                out->n = dst - out->p;
                ADD(out, "<a href=\\\"");
                if (href.kind == LITERAL_STRING) append(out, value_text(&href));
                else json(out, value_text(&href), 1, 0);
                ADD(out, "\\\">");
                INLINE_HTML(out, slice(s, next, label_end), context, depth + 1);
                ADD(out, "</a>");
                next = end + 1;
                dst = room(out, (s.n - next) * 6 + 16);
                break;
            }
            case 0xed: {
                next = i;
                uint cp = decode(s, &next);
                if (cp < 0xd800 || cp > 0xdfff) goto literal;
                out->n = dst - out->p;
                json_code_unit(out, cp);
                dst = out->p + out->n;
                break;
            }
            default:
literal:
                *dst++ = s.p[i];
                next = i + 1;
                break;
        }
        i = next;
    }
    out->n = dst - out->p;
}

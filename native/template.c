#include "core.h"

enum NodeKind { END, TEXT, GLOBAL_RUN, RAW, ESCAPED, TAG, FOR, IF, INCLUDE };

enum PathRoot { FREE_NAME, LOOP_ITEM, LOOP_INFO };
typedef struct {
    Str name;
    enum Property *parts;
    uint n;
    unsigned short root, up;
} Path;
struct Node {
    enum NodeKind kind;
    int negate;
    Str text;
    Path path;
    Node *next, *body, *alternative;
};

static int identifier_start(int c) { return letter(c) || c == '_'; }
static int identifier_char(int c) { return identifier_start(c) || digit(c); }

static size identifier_end(Str s, size i) {
    if (!identifier_start(at(s, i))) return i;
    do i++; while (identifier_char(at(s, i)));
    return i;
}

static int name_valid(Str s) {
    if (!s.n) return 0;
    for (size i = 0; i < s.n; i++)
        if (!lower_digit(s.p[i]) && s.p[i] != '-') return 0;
    return 1;
}

static Str path_error(Str path) {
    Buf buf = {0};
    put(&buf, '"');
    json(&buf, path, 0, 0);
    ADD(&buf, "\" is not a path");
    return take(&buf);
}

static int parse_path(Path *path, Str text) {
    size count = 0;
    for (size i = 0;;) {
        size end = identifier_end(text, i);
        if (end == i) return 0;
        count++;
        if (end == text.n) break;
        if (text.p[end] != '.') return 0;
        i = end + 1;
    }
    *path = (Path){.name = slice(text, 0, identifier_end(text, 0)),
                   .parts = NEW(enum Property, count), .n = count};
    size from = 0;
    for (size i = 0; i < count; i++) {
        size end = identifier_end(text, from);
        path->parts[i] = property_id(slice(text, from, end));
        from = end + 1;
    }
    return 1;
}

static Node *new_node(enum NodeKind kind, Str text) {
    Node *node = NEW(Node, 1);
    *node = (Node){.kind = kind, .text = text};
    return node;
}

static void literal(Node ***next, Str source) {
    if (!source.n) return;
    Buf escaped = {0};
    json(&escaped, source, 0, 0);
    Node *node = new_node(TEXT, take(&escaped));
    **next = node;
    *next = &node->next;
}

static Template tokenize(Str source) {
    Node *root = 0, **next = &root;
    size text = 0, search = 0;
    for (size open; (open = find_char(source, search, '{')) != NPOS;) {
        search = open + 1;
        int second = at(source, search);
        if (second != '{' && second != '%') continue;
        enum NodeKind kind = second == '%' ? TAG : at(source, open + 2) == '{' ? RAW : ESCAPED;
        Str close = kind == TAG ? S("%}") : kind == RAW ? S("}}}") : S("}}");
        size end = find_str(source, open + close.n, close);
        if (end == NPOS)
            return (Template){.error = JOIN(S("a "), slice(source, open, open + close.n), S(" is not closed"))};
        size text_end = open;
        search = end + close.n;
        if (kind == TAG) {
            size line_start = open;
            while (line_start && source.p[line_start - 1] != '\n') line_start--;
            size newline = find_char(source, search, '\n');
            size line_end = newline == NPOS ? source.n : newline;
            if (only_indent(slice(source, line_start, open)) && only_indent(slice(source, search, line_end))) {
                text_end = line_start;
                search = newline == NPOS ? source.n : newline + 1;
            }
        }
        if (text_end > text) literal(&next, slice(source, text, text_end));
        Str body = trim(slice(source, open + close.n, end));
        Node *node = new_node(kind, body);
        if (kind != TAG && !parse_path(&node->path, body)) return (Template){.error = path_error(body)};
        *next = node;
        next = &node->next;
        text = search;
    }
    literal(&next, tail(source, text));
    return (Template){.nodes = root};
}

static Str block_name(enum NodeKind kind) { return kind == FOR ? S("for") : S("if"); }

typedef struct { Node *node; Node **next; int has_else; } ParseFrame;

Template parse_template(Str source) {
    Template tokens = tokenize(source);
    if (tokens.error.n) return tokens;

    Template result = {0};
    ParseFrame stack[129];
    stack[0] = (ParseFrame){.next = &result.nodes};
    size depth = 0;
    for (Node *token = tokens.nodes; token;) {
        Node *node = token;
        token = token->next;
        node->next = 0;
        ParseFrame *frame = &stack[depth];
        if (node->kind == TAG) {
            Str tag = node->text, path = {0};
            if (starts(tag, S("for "))) {
                size name_end = identifier_end(tag, 4);
                if (name_end > 4 && starts(tail(tag, name_end), S(" in "))) {
                    path = tail(tag, name_end + 4);
                    if (path.n && !has_space(path)) {
                        node->kind = FOR;
                        node->text = slice(tag, 4, name_end);
                    }
                }
            } else if (starts(tag, S("if "))) {
                Str body = tail(tag, 3);
                int negate = starts(body, S("not "));
                path = negate ? tail(body, 4) : body;
                if (path.n && !has_space(path)) {
                    node->kind = IF;
                    node->negate = negate;
                }
            }
            if (node->kind == FOR || node->kind == IF) {
                if (!parse_path(&node->path, path)) return (Template){.error = path_error(path)};
                if (depth + 1 == COUNT(stack)) fail();
                *frame->next = node;
                frame->next = &node->next;
                stack[++depth] = (ParseFrame){node, &node->body, 0};
                continue;
            }
            if (equal(tag, S("else"))) {
                if (!depth || frame->node->kind != IF || frame->has_else)
                    return (Template){.error = S("an else outside an if")};
                frame->has_else = 1;
                frame->next = &frame->node->alternative;
                continue;
            }
            int end_if = equal(tag, S("endif"));
            if (end_if || equal(tag, S("endfor"))) {
                enum NodeKind kind = end_if ? IF : FOR;
                if (!depth || frame->node->kind != kind)
                    return (Template){.error = JOIN(S("an "), tag, S(" with no open "), block_name(kind))};
                depth--;
                continue;
            }
            if (tag.n >= 10 && starts(tag, S("include \"")) && ends(tag, S("\""))) {
                Str name = slice(tag, 9, tag.n - 1);
                if (name_valid(name)) {
                    node->kind = INCLUDE;
                    node->text = name;
                }
            }
            if (node->kind == TAG) return (Template){.error = JOIN(S("{% "), tag, S(" %} is not a tag"))};
        }
        *frame->next = node;
        frame->next = &node->next;
    }
    if (depth) return (Template){.error = JOIN(S("a "), block_name(stack[depth].node->kind), S(" is not closed"))};
    return result;
}

typedef struct Binding Binding;
struct Binding { Str name; const Binding *outer; };

typedef struct {
    Str text;
    const Op *last;
} GlobalRun;

static void write_value(Buf *out, const Value *v, int raw);

struct Op {
    enum NodeKind kind;
    union { int negate; enum NodeKind original_kind; };
    Path path;
    union {
        struct { Str text; const GlobalRun *run; };
        Template *include;
        struct {
            const Op *body;
            union { const Op *alternative; Str name; };
        };
    };
};

static const Value *site_path(const Op *op, const Value *site) {
    if ((op->kind == RAW || op->kind == ESCAPED) &&
        op->path.root == FREE_NAME && op->path.n == 2 && op->path.parts[0] == PROP_SITE)
        return property(site, op->path.parts[1]);
    return 0;
}

// Site settings are immutable. Merge their interpolations with neighboring
// literals when a template runs outside any scope that could shadow "site".
static void prepare_global_runs(Op *ops, const Value *site) {
    for (Op *first = ops; first->kind != END;) {
        Op *end = first;
        int setting = 0;
        for (;;) {
            if (end->kind != TEXT) {
                if (!site_path(end, site)) break;
                setting = 1;
            }
            end++;
        }
        if (setting && end > first + 1) {
            Buf text = {0};
            for (Op *op = first; op < end; op++) {
                if (op->kind == TEXT) append(&text, op->text);
                else write_value(&text, site_path(op, site), op->kind == RAW);
            }
            GlobalRun *run = NEW(GlobalRun, 1);
            *run = (GlobalRun){take(&text), end - 1};
            first->original_kind = first->kind;
            first->kind = GLOBAL_RUN;
            first->run = run;
        }
        first = end == first ? end + 1 : end;
    }
}

// Keep each branch contiguous and resolve lookups bound by loops in this
// template. Includes can also inherit names from the calling scope.
static Op *compile_nodes(const Node *nodes, const Map *templates, const Binding *bindings, const Value *site) {
    if (!nodes) return 0;
    size n = 0;
    for (const Node *node = nodes; node; node = node->next) n++;
    Op *ops = NEW(Op, n + 1);
    size i = 0;
    for (const Node *node = nodes; node; node = node->next, i++) {
        Op *op = &ops[i];
        *op = (Op){.kind = node->kind, .path = node->path, .negate = node->negate};
        if (op->path.n) {
            for (const Binding *b = bindings; b; b = b->outer, op->path.up++) {
                if (equal(op->path.name, S("loop"))) {
                    op->path.root = LOOP_INFO;
                    break;
                }
                if (equal(op->path.name, b->name)) {
                    op->path.root = LOOP_ITEM;
                    break;
                }
            }
        }
        if (node->kind == TEXT) op->text = node->text;
        else if (node->kind == INCLUDE) op->include = get(templates, node->text);
        else if (node->kind == FOR) {
            op->name = node->text;
            Binding binding = {node->text, bindings};
            op->body = compile_nodes(node->body, templates, &binding, site);
        } else if (node->kind == IF) {
            op->body = compile_nodes(node->body, templates, bindings, site);
            op->alternative = compile_nodes(node->alternative, templates, bindings, site);
        }
    }
    ops[n].kind = END;
    if (!bindings) prepare_global_runs(ops, site);
    return ops;
}

void prepare_template(Template *template, const Map *templates, const Value *site) {
    template->ops = compile_nodes(template->nodes, templates, 0, site);
}

typedef struct Scope Scope;
struct Scope {
    const Scope *parent;
    Str name;
    const Value *item, *loop;
    const Value *const *properties;
    size root_names, start;
};

static const Value *lookup(const Scope *scope, const Path *path) {
    for (uint up = path->up; up; up--) scope = scope->parent;
    const Value *v;
    if (path->root != FREE_NAME) {
        v = path->root == LOOP_ITEM ? scope->item : scope->loop;
    } else {
        while (scope->parent) {
            if (equal(path->name, S("loop"))) {
                v = scope->loop;
                goto descend;
            }
            if (equal(path->name, scope->name)) {
                v = scope->item;
                goto descend;
            }
            scope = scope->parent;
        }
        v = scope->properties[path->parts[0]];
    }
descend:
    for (size i = 1; v && i < path->n; i++) v = property(v, path->parts[i]);
    return v;
}

static int truthy(const Value *v) {
    if (!v) return 0;
    switch (v->kind) {
        case NIL: return 0;
        case STRING: case LITERAL_STRING: return v->n != 0;
        case RENDERED: return v->n != 0;
        case NUMBER: case BOOLEAN: return v->number != 0;
        case ARRAY: return v->n != 0;
        case RECORD: return 1;
    }
    return 0;
}

static void write_number(Buf *out, size value) {
    char digits[20];
    append(out, format_number(digits + sizeof(digits), value));
}

static void write_value(Buf *out, const Value *v, int raw) {
    if (!v) return;
    switch (v->kind) {
        case LITERAL_STRING:
            append(out, value_text(v));
            break;
        case STRING:
            json(out, value_text(v), !raw, !raw && !latin1);
            break;
        case RENDERED:
            if (raw) append(out, value_text(v));
            else escape_rendered(out, value_text(v));
            break;
        case NUMBER:
            write_number(out, v->number);
            break;
        case BOOLEAN:
            append(out, v->number ? S("true") : S("false"));
            break;
        case ARRAY:
            for (size i = 0; i < v->n; i++) {
                if (i) ADD(out, ", ");
                write_value(out, v->items[i], raw);
            }
            break;
        default:
            break;
    }
}

static void render_ops(Buf *out, const Op *ops, const Scope *scope, uint include_depth, uint stack_depth);

// An included template is reusable when its free names resolve to globals,
// missing values, and at most one loop item. Loop metadata varies by iteration.
struct Memo {
    Str names[8], constant;
    enum Property properties[8];
    size count, root_names;
    int disabled;
    Map by_item;
    size local_scope, local_end;
    size local_length;
};
_Static_assert(PROP_URL < 64, "template property mask");

static void add_free_name(Memo *memo, Path path, const Binding *bindings) {
    Str name = path.name;
    for (; bindings; bindings = bindings->outer)
        if (equal(name, bindings->name) || equal(name, S("loop"))) return;
    for (size i = 0; i < memo->count; i++) if (equal(name, memo->names[i])) return;
    if (memo->count == COUNT(memo->names)) memo->disabled = 1;
    else {
        enum Property property = path.parts[0];
        memo->root_names |= 1UL << property;
        memo->properties[memo->count] = property;
        memo->names[memo->count++] = name;
    }
}

static void collect_free(Memo *memo, const Op *ops, uint depth, const Binding *bindings, size *budget) {
    if (!ops) return;
    for (const Op *node = ops; node->kind != END && !memo->disabled; node++) {
        if (!*budget) {
            memo->disabled = 1;
            return;
        }
        --*budget;
        if (node->path.n) add_free_name(memo, node->path, bindings);
        if (node->kind == FOR) {
            Binding binding = {node->name, bindings};
            collect_free(memo, node->body, depth, &binding, budget);
        } else if (node->kind == IF) {
            collect_free(memo, node->body, depth, bindings, budget);
            collect_free(memo, node->alternative, depth, bindings, budget);
        } else if (node->kind == INCLUDE && node->include && depth < 8) {
            collect_free(memo, node->include->ops, depth + 1, bindings, budget);
        }
    }
}

static void render_include(Buf *out, Template *template, const Scope *scope, uint depth, uint stack_depth) {
    Memo *memo = template->memo[depth];
    if (!memo) {
        template->memo[depth] = memo = zero(NEW(Memo, 1), sizeof(*memo));
        size budget = 4096;
        collect_free(memo, template->ops, depth, 0, &budget);
    }
    if (memo->disabled) {
        render_ops(out, template->ops, scope, depth, stack_depth);
        return;
    }

    const Value *item = 0;
    size binding_index = 0;
    if (!scope->parent && (scope->root_names & memo->root_names)) {
        // Root-dependent includes can still repeat within one page render.
        if (memo->local_scope == scope->start && memo->local_end) {
            Str saved = answer_fragment(out, memo->local_end - memo->local_length, memo->local_length);
            if (saved.p) {
                append(out, saved);
                return;
            }
        }
        size start = answer_position(out);
        render_ops(out, template->ops, scope, depth, stack_depth);
        memo->local_scope = scope->start;
        memo->local_end = answer_position(out);
        memo->local_length = memo->local_end - start;
        return;
    }
    for (size i = 0; scope->parent && i < memo->count; i++) {
        Str name = memo->names[i];
        const Scope *s = scope;
        while (s->parent) {
            if (equal(name, S("loop"))) goto uncached;
            if (equal(name, s->name)) break;
            s = s->parent;
        }
        if (s->parent) {
            if (item) goto uncached;
            item = s->item;
            binding_index = i;
        } else if (s->root_names & (1UL << memo->properties[i])) {
            goto uncached;
        }
    }
    if (item) {
        size parts[] = {(size)item, binding_index};
        Str key = {(const char *)parts, sizeof(parts)};
        const Str *saved = get(&memo->by_item, key);
        if (saved) {
            append(out, *saved);
            return;
        }
        size start = answer_position(out);
        render_ops(out, template->ops, scope, depth, stack_depth);
        Str text = cache_answer(out, start);
        if (text.p) {
            Str *cached = NEW(Str, 1);
            *cached = text;
            set(&memo->by_item, JOIN(key), cached);
        }
        return;
    }
    if (memo->constant.p) {
        append(out, memo->constant);
        return;
    }
    size start = answer_position(out);
    render_ops(out, template->ops, scope, depth, stack_depth);
    memo->constant = cache_answer(out, start);
    return;

uncached:
    render_ops(out, template->ops, scope, depth, stack_depth);
}


static void render_loop(Buf *out, const Op *node, const Scope *scope, uint depth, uint stack_depth) {
    const Value *v = lookup(scope, &node->path);
    if (!v || v->kind != ARRAY || !node->body) return;
    size count = v->n;
    Value index = numeric(0), first = boolean(0), last = boolean(0), length = numeric(count);
    const Value *fields[] = {&index, &first, &last, &length};
    Value loop = record(LOOP_FIELDS, fields, COUNT(fields));
    Scope child = {.parent = scope, .name = node->name, .loop = &loop};
    for (size i = 0; i < count; i++) {
        index.number = i + 1;
        first.number = i == 0;
        last.number = i + 1 == count;
        child.item = v->items[i];
        render_ops(out, node->body, &child, depth, stack_depth);
    }
}

static void render_ops(Buf *out, const Op *ops, const Scope *scope, uint include_depth, uint stack_depth) {
    if (!ops) return;
    if (stack_depth > 512) fail();
    for (const Op *node = ops; node->kind != END; node++) {
        switch (node->kind) {
            case GLOBAL_RUN:
                if (!scope->parent) {
                    append(out, node->run->text);
                    node = node->run->last;
                } else if (node->original_kind == TEXT) {
                    append(out, node->text);
                } else {
                    write_value(out, lookup(scope, &node->path), node->original_kind == RAW);
                }
                break;
            case TEXT:
                append(out, node->text);
                break;
            case RAW: case ESCAPED:
                write_value(out, lookup(scope, &node->path), node->kind == RAW);
                break;
            case FOR:
                render_loop(out, node, scope, include_depth, stack_depth + 1);
                break;
            case IF: {
                int yes = truthy(lookup(scope, &node->path)) != node->negate;
                render_ops(out, yes ? node->body : node->alternative, scope, include_depth, stack_depth + 1);
                break;
            }
            case INCLUDE:
                if (node->include && include_depth < 8)
                    render_include(out, node->include, scope, include_depth + 1, stack_depth + 1);
                break;
            case TAG: case END:
                fail();
        }
    }
}

void render_template(Buf *out, const Template *template, const Value *root, const Value *globals) {
    const Value *properties[PROP_URL + 1] = {
        [PROP_SITE] = globals->fields[0],
        [PROP_TAGS] = globals->fields[1],
        [PROP_RECENT] = globals->fields[2],
    };
    Scope scope = {.properties = properties, .start = answer_position(out)};
    if (root) {
        switch (root->shape.layout) {
            case PAGE_ROOT:
                scope.root_names = 1UL << PROP_PAGE;
                properties[PROP_PAGE] = root->fields[0];
                break;
            case TAG_ROOT:
                scope.root_names = 1UL << PROP_TAG;
                properties[PROP_TAG] = root->fields[0];
                break;
            case INDEX_FIELDS:
                scope.root_names = (1UL << PROP_POSTS) | (1UL << PROP_PAGER);
                properties[PROP_POSTS] = root->fields[0];
                properties[PROP_PAGER] = root->fields[1];
                break;
            default: fail();
        }
    }
    render_ops(out, template->ops, &scope, 0, 0);
}

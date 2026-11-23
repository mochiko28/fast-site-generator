'use strict';

const HIGH = /[^\x00-\x7f]/;
const ASCII_SPACE = /[\t-\r ]/;
const ASCII_TRIM = /^[\t-\r ]+|[\t-\r ]+$/g;
const BYTE_SPACE = /\xc2\xa0|\xe1\x9a\x80|\xe2\x80[\x80-\x8a\xa8\xa9\xaf]|\xe2\x81\x9f|\xe3\x80\x80|\xef\xbb\xbf/;
const BYTE_SEPARATOR = /\xe2\x80[\xa8\xa9]/;
const SEPARATOR = /[\u2028\u2029]/;
const SUPPLEMENTARY_BYTE = /[\xf0-\xf4]/;
const MONTHS = [
  'January', 'February', 'March', 'April', 'May', 'June', 'July', 'August',
  'September', 'October', 'November', 'December',
];
const RESERVED = ['posts', 'tags', 'page'];

function decode(bytes) {
  return Buffer.from(bytes, 'latin1').toString('utf8');
}

function trim(text, latin1) {
  if (!latin1) return text.trim();
  if (!BYTE_SPACE.test(text)) return text.replace(ASCII_TRIM, '');
  return Buffer.from(decode(text).trim(), 'utf8').toString('latin1');
}

function hasSpace(text, latin1) {
  return latin1 ? ASCII_SPACE.test(text) || BYTE_SPACE.test(text) : /\s/.test(text);
}

function slugify(text, latin1) {
  if (HIGH.test(text)) {
    if (latin1) text = decode(text);
    text = text.normalize('NFKD').replace(/\p{M}/gu, '');
  }
  return text.replace(/[^A-Za-z0-9]+/g, '-').toLowerCase().replace(/^-/, '')
    .slice(0, 64).replace(/-$/, '') || 'untitled';
}

function reserveSlug(slug, used) {
  let candidate = slug;
  for (let suffix = 2; used.has(candidate); suffix++) candidate = slug + '-' + suffix;
  used.add(candidate);
  return candidate;
}

function compare(a, b) {
  return a < b ? -1 : a > b ? 1 : 0;
}

function compareBytes(a, b) {
  // Only supplementary characters can sort differently in UTF-16.
  if (a !== b && (SUPPLEMENTARY_BYTE.test(a) || SUPPLEMENTARY_BYTE.test(b))) {
    return compare(decode(a), decode(b));
  }
  return compare(a, b);
}

function trimLine(line) {
  let start = 0;
  let end = line.length;
  while (line.charCodeAt(start) === 32 || line.charCodeAt(start) === 9) start++;
  while (end > start && (line.charCodeAt(end - 1) === 32 || line.charCodeAt(end - 1) === 9)) end--;
  return start === 0 && end === line.length ? line : line.slice(start, end);
}

function onlyIndent(line, start = 0) {
  for (let i = start; i < line.length; i++) {
    const c = line.charCodeAt(i);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

function quotedValue(raw) {
  if (raw[0] === '[') throw 'a bare string opens on [';
  if (raw[0] !== '"') return raw;
  if (raw.length < 2 || raw.at(-1) !== '"') throw 'a quoted string is not closed';
  let value = '';
  let start = 1;
  const end = raw.length - 1;
  for (let i = 1; i < end; i++) {
    const c = raw.charCodeAt(i);
    if (c === 34) throw 'a quoted string holds a stray quote';
    if (c !== 92) continue;
    const next = raw.charCodeAt(i + 1);
    if (i + 1 === end || (next !== 34 && next !== 92)) {
      throw 'a quoted string holds an escape other than \\" or \\\\';
    }
    value += raw.slice(start, i) + raw[i + 1];
    start = i + 2;
    i++;
  }
  return value + raw.slice(start, end);
}

const FRONT_KEYS = new Set(['title', 'summary', 'date', 'tags', 'draft', 'layout', 'slug']);
const FRONT_LINE = /^([a-z_]+):(?: (.*))?$/;
const BAD_DATE = 'not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)';

function frontValue(key, raw, latin1) {
  switch (key) {
    case 'title':
    case 'summary':
      return quotedValue(raw);
    case 'date': {
      if (!/^\d{4}-\d{2}-\d{2}$/.test(raw)) throw BAD_DATE;
      const year = +raw.slice(0, 4);
      const month = +raw.slice(5, 7);
      const day = +raw.slice(8, 10);
      const limit = month === 2 ? (year % 4 === 0 ? 29 : 28)
        : (month === 4 || month === 6 || month === 9 || month === 11 ? 30 : 31);
      // Within 1970–2099 the century year, 2000, is a leap year.
      if (year < 1970 || year > 2099 || month < 1 || month > 12 || day < 1 || day > limit) throw BAD_DATE;
      return raw;
    }
    case 'tags': {
      if (raw[0] !== '[' || raw.at(-1) !== ']') throw 'not a list [a, b]';
      const inner = raw.slice(1, -1);
      if (!trim(inner, latin1)) return [];
      const tags = inner.split(',');
      for (let i = 0; i < tags.length; i++) {
        const name = trim(tags[i], latin1);
        if (!name || /["[\]]/.test(name)) throw 'an empty tag, or one holding " [ or ]';
        tags[i] = name;
      }
      return tags;
    }
    case 'draft':
      if (raw === 'true') return true;
      if (raw === 'false') return false;
      throw 'true or false';
    case 'layout':
      if (/^[a-z0-9-]+$/.test(raw)) return raw;
      throw 'not a template name';
    case 'slug':
      if (raw.length <= 64 && /^[a-z0-9]+(?:-[a-z0-9]+)*$/.test(raw)) return raw;
      throw 'not a slug';
  }
}

function frontMatter(text, latin1) {
  if (!text.startsWith('---\n')) return { error: text === '---' ? 'the front matter is not closed' : 'no front matter' };
  let close = 3;
  do {
    close = text.indexOf('\n---', close);
    if (close < 0) return { error: 'the front matter is not closed' };
    close += 4;
  } while (close < text.length && text.charCodeAt(close) !== 10);

  const data = {};
  const lines = text.slice(4, close - 4).split('\n');
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    if (line.charCodeAt(0) === 35 || onlyIndent(line)) continue;
    const match = FRONT_LINE.exec(line);
    if (!match || (latin1 && BYTE_SEPARATOR.test(line))) {
      return { error: `line ${i + 2} is not "key: value"` };
    }
    const key = match[1];
    if (!FRONT_KEYS.has(key)) return { error: `unknown key "${key}"` };
    if (Object.hasOwn(data, key)) return { error: `key "${key}" is given twice` };
    const raw = (match[2] ?? '').replace(/[ \t]+$/, '');
    if (!raw) return { error: `key "${key}" has no value` };
    try {
      data[key] = frontValue(key, raw, latin1);
    } catch (reason) {
      return { error: `key "${key}": ${reason}` };
    }
  }
  return Object.hasOwn(data, 'title') ? { data, body: text.slice(close + 1) } : { error: 'no title' };
}

const HTML_ENTITIES = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
const HTML_JSON_BYTES = /[&<>"'\\\x00-\x1f]/g;
const HTML_JSON_UTF16 = /[&<>"'\\\x00-\x1f\ud800-\udfff]/g;
const JSON_CHAR = Array.from({ length: 32 }, (_, c) => JSON.stringify(String.fromCharCode(c)).slice(1, -1));

function htmlReplacement(character) {
  const entity = HTML_ENTITIES[character];
  if (entity !== undefined) return entity;
  const code = character.charCodeAt(0);
  if (code < 32) return JSON_CHAR[code];
  if (code === 92) return '\\\\';
  return '\\u' + code.toString(16);
}

function htmlJson(text, latin1) {
  return text.replace(latin1 ? HTML_JSON_BYTES : HTML_JSON_UTF16, htmlReplacement);
}

function jsonText(text) {
  return JSON.stringify(text).slice(1, -1);
}

class RenderedString {
  constructor(json) {
    this.json = json;
    this.text = undefined;
  }

  plain() {
    return this.text ??= JSON.parse('"' + this.json + '"');
  }
}

const ESCAPABLE = new Uint8Array(128);
for (const c of '\\`*_[]()#+-.!>') ESCAPABLE[c.charCodeAt(0)] = 1;

const INLINE_ESCAPE = JSON_CHAR.slice();
INLINE_ESCAPE[34] = '&quot;';
INLINE_ESCAPE[38] = '&amp;';
INLINE_ESCAPE[39] = '&#39;';
INLINE_ESCAPE[60] = '&lt;';
INLINE_ESCAPE[62] = '&gt;';
INLINE_ESCAPE[92] = '\\\\';

function closingMark(text, mark, start, singleStar = false) {
  let position = text.indexOf(mark, start);
  while (position !== -1) {
    let previous = position - 1;
    while (previous >= start && text.charCodeAt(previous) === 92) previous--;
    // Both possible closing characters are backslash-escapable.
    const escaped = ((position - previous) & 1) === 0;
    if (!escaped && (!singleStar || text.charCodeAt(position + 1) !== 42)) return position;
    position = text.indexOf(mark, position + (!escaped && singleStar ? 2 : 1));
  }
  return -1;
}

function inline(text, link, latin1, find) {
  let parts;
  let start = 0;
  for (let i = 0; i < text.length; i++) {
    find.lastIndex = i;
    if (!find.test(text)) break;
    i = find.lastIndex - 1;
    const c = text.charCodeAt(i);
    let replacement;
    let end = i;
    switch (c) {
      case 92: {
        const escaped = text.charCodeAt(i + 1);
        if (ESCAPABLE[escaped]) {
          replacement = INLINE_ESCAPE[escaped] ?? text[i + 1];
          end++;
        } else replacement = INLINE_ESCAPE[92];
        break;
      }
      case 96: {
        const close = text.indexOf('`', i + 1);
        if (close > i + 1) {
          replacement = '<code>' + htmlJson(text.slice(i + 1, close), latin1) + '</code>';
          end = close;
        }
        break;
      }
      case 42: {
        const strong = text.charCodeAt(i + 1) === 42;
        const inner = i + (strong ? 2 : 1);
        const close = strong ? closingMark(text, '**', inner) : closingMark(text, '*', inner, true);
        if (close > inner) {
          const name = strong ? 'strong' : 'em';
          replacement = '<' + name + '>' + inline(text.slice(inner, close), link, latin1, find) + '</' + name + '>';
          end = close + (strong ? 1 : 0);
        } else if (strong) {
          i++;
        }
        break;
      }
      case 91: {
        const close = closingMark(text, ']', i + 1);
        if (close >= 0 && text.charCodeAt(close + 1) === 40) {
          const stop = text.indexOf(')', close + 2);
          if (stop > close + 2) {
            const target = text.slice(close + 2, stop);
            if (!hasSpace(target, latin1)) {
              replacement = '<a href=\\"' + link(target) + '\\">'
                + inline(text.slice(i + 1, close), link, latin1, find) + '</a>';
              end = stop;
            }
          }
        }
        break;
      }
      default:
        if (c <= 62) replacement = INLINE_ESCAPE[c];
        else if (!latin1 && (c & 0xf800) === 0xd800) replacement = '\\u' + c.toString(16);
    }
    if (replacement !== undefined) {
      (parts ??= []).push(text.slice(start, i), replacement);
      start = end + 1;
      i = end;
    }
  }
  if (parts === undefined) return text;
  parts.push(text.slice(start));
  return parts.join('');
}

const FENCE = 7;
const BREAK = 8;
const BLANK = 9;

function blockStart(line, separator) {
  const first = line.charCodeAt(0);
  if (first === 35) {
    let level = 1;
    while (line.charCodeAt(level) === 35) level++;
    const next = line.charCodeAt(level);
    if (level <= 6 && (next === 32 || next === 9) && !onlyIndent(line, level + 1)
      && !separator.test(line)) return level;
  } else if (first === 96 && line.charCodeAt(1) === 96 && line.charCodeAt(2) === 96) {
    let i = 3;
    for (; i < line.length; i++) {
      const c = line.charCodeAt(i);
      const lower = c | 32;
      if ((lower >= 97 && lower <= 122) || (c >= 48 && c <= 57) || c === 95 || c === 43 || c === 45) continue;
      break;
    }
    if (onlyIndent(line, i)) return FENCE;
  } else if ((first === 45 || first === 42) && line.charCodeAt(1) === first
    && line.charCodeAt(2) === first && onlyIndent(line, 3)) {
    return BREAK;
  } else if (line.length === 0 || ((first === 32 || first === 9) && onlyIndent(line, 1))) {
    return BLANK;
  }
  return 0;
}

function listMarker(line, separator) {
  const first = line.charCodeAt(0);
  if (first === 42 || first === 45) {
    return line.charCodeAt(1) === 32 && !separator.test(line) ? 2 : 0;
  }
  if (first < 48 || first > 57 || line.length === 0) return 0;
  let digits = 1;
  for (; digits < 10; digits++) {
    const next = line.charCodeAt(digits);
    if (!(next >= 48 && next <= 57)) break;
  }
  return digits < 10 && line.charCodeAt(digits) === 46 && line.charCodeAt(digits + 1) === 32
    && !separator.test(line) ? digits + 2 : 0;
}

function continuesList(line, latin1) {
  if (line.charCodeAt(0) !== 32 || line.charCodeAt(1) !== 32) return false;
  let i = 2;
  while (line.charCodeAt(i) === 32) i++;
  if (i === line.length) return false;
  const next = line.charCodeAt(i);
  if (next < 128) return next < 9 || next > 13;
  if (!latin1) return !/\s/.test(line[i]);
  const space = BYTE_SPACE.exec(line.slice(i));
  return space === null || space.index !== 0;
}

function renderMarkdown(body, link, latin1, find) {
  const lines = body.split('\n');
  const separator = latin1 ? BYTE_SEPARATOR : SEPARATOR;
  const ids = new Set();
  const toc = [];
  const blocks = [];
  let excerpt = null;
  let at = 0;
  let kind = blockStart(lines[0], separator);

  while (at < lines.length) {
    const line = lines[at++];
    if (kind === FENCE) {
      const language = trimLine(line.slice(3));
      blocks.push(language ? '<pre><code class=\\"language-' + language + '\\">' : '<pre><code>');
      while (at < lines.length) {
        const code = lines[at++];
        if (code.startsWith('```') && onlyIndent(code, 3)) break;
        blocks.push(htmlJson(code, latin1) + '\\n');
      }
      blocks.push('</code></pre>\\n');
    } else if (kind === BREAK) {
      blocks.push('<hr>\\n');
    } else if (kind > 0 && kind < FENCE) {
      const heading = trimLine(line.slice(kind));
      const id = reserveSlug(slugify(heading, latin1), ids);
      const text = inline(heading, link, latin1, find);
      blocks.push('<h' + kind + ' id=\\"' + id + '\\">' + text + '</h' + kind + '>\\n');
      if (kind === 2 || kind === 3) toc.push({ level: kind, id, html: new RenderedString(text) });
    } else if (kind !== BLANK) {
      if (line.charCodeAt(0) === 62) {
        let content = trimLine(line.slice(1));
        while (at < lines.length && lines[at].charCodeAt(0) === 62) {
          const next = trimLine(lines[at++].slice(1));
          if (next) content = content ? content + '\n' + next : next;
        }
        blocks.push('<blockquote>\\n');
        if (content) blocks.push('<p>' + inline(content, link, latin1, find) + '</p>\\n');
        blocks.push('</blockquote>\\n');
      } else {
        const marker = listMarker(line, separator);
        if (marker) {
          const ordered = marker !== 2;
          const tag = ordered ? 'ol' : 'ul';
          const first = ordered ? Number(line.slice(0, marker - 2)) : 1;
          blocks.push(first !== 1 ? '<ol start=\\"' + first + '\\">\\n' : '<' + tag + '>\\n');
          let item = trimLine(line.slice(marker));
          while (at < lines.length) {
            const next = lines[at];
            const prefix = listMarker(next, separator);
            if ((ordered && prefix > 2) || (!ordered && prefix === 2)) {
              blocks.push('<li>' + inline(item, link, latin1, find) + '</li>\\n');
              item = trimLine(next.slice(prefix));
            } else if (continuesList(next, latin1)) {
              item += '\n' + trimLine(next);
            } else {
              break;
            }
            at++;
          }
          blocks.push('<li>' + inline(item, link, latin1, find) + '</li>\\n</' + tag + '>\\n');
        } else {
          let paragraph = trimLine(line);
          while (at < lines.length && (kind = blockStart(lines[at], separator)) === 0) {
            paragraph += '\n' + trimLine(lines[at++]);
          }
          const text = inline(paragraph, link, latin1, find);
          excerpt ??= text;
          blocks.push('<p>' + text + '</p>\\n');
          continue;
        }
      }
    }
    if (at < lines.length) kind = blockStart(lines[at], separator);
  }
  return { content: new RenderedString(blocks.join('')), toc, excerpt: new RenderedString(excerpt ?? '') };
}

const TEMPLATE_PATH = /^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$/;
const FOR_TAG = /^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$/;
const IF_TAG = /^if (not )?(\S+)$/;
const INCLUDE_TAG = /^include "([a-z0-9-]+)"$/;

class TemplateSyntax extends Error {}

function templateError(name, message) {
  throw new TemplateSyntax('template "' + name + '": ' + message);
}

function templatePath(name, path) {
  if (!TEMPLATE_PATH.test(path)) templateError(name, JSON.stringify(path) + ' is not a path');
  return path.split('.');
}

function templateTokens(name, source, latin1) {
  const tokens = [];
  let textStart = 0;
  let search = 0;
  for (;;) {
    const open = source.indexOf('{', search);
    if (open < 0) break;
    search = open + 1;
    const second = source[search];
    if (second !== '{' && second !== '%') continue;
    const tag = second === '%';
    const raw = !tag && source[open + 2] === '{';
    const length = raw ? 3 : 2;
    const close = source.indexOf(tag ? '%}' : raw ? '}}}' : '}}', open + length);
    if (close < 0) templateError(name, 'a ' + source.slice(open, open + length) + ' is not closed');

    let textEnd = open;
    search = close + length;
    if (tag) {
      const lineStart = open === 0 ? 0 : source.lastIndexOf('\n', open - 1) + 1;
      const newline = source.indexOf('\n', search);
      const lineEnd = newline < 0 ? source.length : newline;
      if (onlyIndent(source.slice(lineStart, open)) && onlyIndent(source.slice(search, lineEnd))) {
        textEnd = lineStart;
        search = newline < 0 ? source.length : newline + 1;
      }
    }
    if (textEnd > textStart) tokens.push({ kind: 'text', text: jsonText(source.slice(textStart, textEnd)) });
    const value = trim(source.slice(open + length, close), latin1);
    tokens.push(tag ? { kind: 'tag', value } : { kind: raw ? 'raw' : 'escaped', path: templatePath(name, value) });
    textStart = search;
  }
  if (textStart < source.length) tokens.push({ kind: 'text', text: jsonText(source.slice(textStart)) });
  return tokens;
}

function parseTemplate(name, source, latin1) {
  const root = { body: [] };
  const stack = [root];
  for (const token of templateTokens(name, source, latin1)) {
    const frame = stack[stack.length - 1];
    if (token.kind !== 'tag') {
      frame.body.push(token);
      continue;
    }
    const tag = token.value;
    const grammar = latin1 && HIGH.test(tag) ? decode(tag) : tag;
    const loop = FOR_TAG.exec(grammar);
    const condition = loop === null ? IF_TAG.exec(grammar) : null;
    if (loop !== null || condition !== null) {
      const branch = [];
      const node = loop !== null
        ? { kind: 'for', name: loop[1], path: templatePath(name, tag.slice(8 + loop[1].length)), body: branch }
        : { kind: 'if', negate: condition[1] !== undefined, path: templatePath(name, tag.slice(condition[1] ? 7 : 3)), yes: branch, no: null };
      frame.body.push(node);
      stack.push({ node, body: branch });
    } else if (tag === 'else') {
      if (frame.node?.kind !== 'if' || frame.node.no !== null) templateError(name, 'an else outside an if');
      frame.body = frame.node.no = [];
    } else if (tag === 'endif' || tag === 'endfor') {
      if (frame.node?.kind !== tag.slice(3)) templateError(name, 'an ' + tag + ' with no open ' + tag.slice(3));
      stack.pop();
    } else {
      const include = INCLUDE_TAG.exec(tag);
      if (include === null) templateError(name, '{% ' + tag + ' %} is not a tag');
      frame.body.push({ kind: 'include', name: include[1] });
    }
  }
  if (stack.length > 1) templateError(name, 'a ' + stack[stack.length - 1].node.kind + ' is not closed');
  return root.body;
}

function property(value, name) {
  if (value !== null && typeof value === 'object' && !(value instanceof RenderedString) && Object.hasOwn(value, name)) {
    return value[name];
  }
}

function outputValue(value, raw, latin1) {
  if (value instanceof RenderedString) return raw ? value.json : htmlJson(value.plain(), latin1);
  switch (typeof value) {
    case 'string': return raw ? jsonText(value) : htmlJson(value, latin1);
    case 'number': return String(value);
    case 'boolean': return value ? 'true' : 'false';
    default:
      return Array.isArray(value) ? value.map((item) => outputValue(item, raw, latin1)).join(', ') : '';
  }
}

function truthy(value) {
  if (value instanceof RenderedString) return value.json.length !== 0;
  if (Array.isArray(value)) return value.length !== 0;
  return value !== undefined && value !== null && value !== false && value !== '' && value !== 0;
}

function renderPlan(plan, frame) {
  return typeof plan === 'string' ? plan : plan.render(frame);
}

function collectDependencies(plans, slots = new Set()) {
  for (const plan of plans) {
    if (typeof plan !== 'string') {
      for (const slot of plan.slots) slots.add(slot);
    }
  }
  return slots;
}

function joinPlans(plans) {
  const parts = [];
  let text = '';
  for (const plan of plans) {
    if (typeof plan === 'string') {
      text += plan;
    } else {
      if (text) parts.push(text);
      text = '';
      parts.push(plan);
    }
  }
  if (parts.length === 0) return text;
  if (text) parts.push(text);
  if (parts.length === 1) return parts[0];
  return {
    slots: [...collectDependencies(parts)],
    render(frame) {
      let output = '';
      for (const part of parts) output += renderPlan(part, frame);
      return output;
    },
  };
}

function cacheElement(plan, slot) {
  if (typeof plan === 'string' || plan.memoized || plan.slots.length !== 1 || plan.slots[0] !== slot) return plan;
  const cache = new WeakMap();
  return {
    slots: plan.slots,
    memoized: true,
    render(frame) {
      const key = frame[slot];
      if (key === null || typeof key !== 'object') return plan.render(frame);
      const saved = cache.get(key);
      if (saved !== undefined) return saved;
      const output = plan.render(frame);
      cache.set(key, output);
      return output;
    },
  };
}

class Templates {
  constructor(parsed, constants, latin1) {
    this.parsed = parsed;
    this.constants = constants;
    this.latin1 = latin1;
    this.free = new WeakMap();
    this.specializations = new WeakMap();
  }

  scope(names, parent = null) {
    const start = parent === null ? 0 : parent.size;
    const bindings = new Map(names.map((name, i) => [name, start + i]));
    return { bindings, parent, size: parent === null ? 2 : start + names.length, children: new Map(), includes: [],
      itemSlot: parent === null ? -1 : start };
  }

  loopScope(scope, name) {
    let child = scope.children.get(name);
    if (child === undefined) {
      child = this.scope([name, 'loop'], scope);
      scope.children.set(name, child);
    }
    return child;
  }

  slotOf(head, scope) {
    for (let current = scope; current !== null; current = current.parent) {
      const slot = current.bindings.get(head);
      if (slot !== undefined) return slot;
    }
    return -1;
  }

  lookup(path, scope) {
    const head = path[0];
    const slot = this.slotOf(head, scope);
    if (slot === -1) {
      let value = Object.hasOwn(this.constants, head) ? this.constants[head] : undefined;
      for (let i = 1; i < path.length; i++) value = property(value, path[i]);
      return { value };
    }
    const read = path.length === 1 ? (frame) => frame[slot]
      : path.length === 2 ? (frame) => property(frame[slot], path[1])
        : (frame) => {
          let value = frame[slot];
          for (let i = 1; i < path.length; i++) value = property(value, path[i]);
          return value;
        };
    return { read, slot };
  }

  compile(nodes, scope, depth) {
    return joinPlans(nodes.map((node) => this.compileNode(node, scope, depth)));
  }

  freeNames(nodes, depth) {
    let levels = this.free.get(nodes);
    if (levels === undefined) this.free.set(nodes, levels = []);
    if (levels[depth] !== undefined) return levels[depth];
    const names = new Set();
    for (const node of nodes) {
      if (node.path !== undefined) names.add(node.path[0]);
      if (node.kind === 'for') {
        for (const name of this.freeNames(node.body, depth)) {
          if (name !== node.name && name !== 'loop') names.add(name);
        }
      } else if (node.kind === 'if') {
        for (const name of this.freeNames(node.yes, depth)) names.add(name);
        if (node.no !== null) for (const name of this.freeNames(node.no, depth)) names.add(name);
      } else if (node.kind === 'include' && depth < 8) {
        const included = this.parsed.get(node.name);
        if (included !== undefined) for (const name of this.freeNames(included, depth + 1)) names.add(name);
      }
    }
    return levels[depth] = names;
  }

  template(name, scope, depth = 0) {
    const nodes = this.parsed.get(name);
    if (nodes === undefined || depth > 8) return '';
    const cache = scope.includes[depth] ??= new Map();
    let plan = cache.get(name);
    if (plan === undefined) {
      // An include can share a plan, and its element cache, across root layouts
      // when it uses the same bindings and the same slots for nested loops.
      let levels = this.specializations.get(nodes);
      if (levels === undefined) this.specializations.set(nodes, levels = []);
      const specialized = levels[depth] ??= new Map();
      let signature = String(scope.size);
      for (const binding of this.freeNames(nodes, depth)) signature += ',' + this.slotOf(binding, scope);
      plan = specialized.get(signature);
      if (plan === undefined) {
        plan = cacheElement(this.compile(nodes, scope, depth), scope.itemSlot);
        specialized.set(signature, plan);
      }
      cache.set(name, plan);
    }
    return plan;
  }

  compileNode(node, scope, depth) {
    if (node.kind === 'text') return node.text;
    if (node.kind === 'include') return this.template(node.name, scope, depth + 1);
    const lookup = this.lookup(node.path, scope);
    const { read, value, slot } = lookup;
    if (node.kind === 'if') {
      if (read === undefined) {
        const selected = truthy(value) !== node.negate ? node.yes : node.no;
        return selected === null ? '' : this.compile(selected, scope, depth);
      }
      const yes = this.compile(node.yes, scope, depth);
      const no = node.no === null ? '' : this.compile(node.no, scope, depth);
      if (typeof yes === 'string' && yes === no) return yes;
      return {
        slots: [...collectDependencies([yes, no], new Set([slot]))],
        render: (frame) => renderPlan(truthy(read(frame)) !== node.negate ? yes : no, frame),
      };
    }
    if (node.kind === 'for') {
      if (read === undefined && (!Array.isArray(value) || value.length === 0)) return '';
      const child = this.loopScope(scope, node.name);
      const itemSlot = child.itemSlot;
      const loopSlot = itemSlot + 1;
      const body = cacheElement(this.compile(node.body, child, depth), itemSlot);
      if (body === '') return '';
      const dependencies = typeof body === 'string' ? [] : body.slots;
      const needsLoop = dependencies.includes(loopSlot);
      const external = new Set(dependencies.filter((index) => index < itemSlot));
      if (read !== undefined) external.add(slot);
      const render = (frame) => {
        const items = read === undefined ? value : read(frame);
        if (!Array.isArray(items)) return '';
        let text = '';
        const length = items.length;
        for (let i = 0; i < length; i++) {
          if (!(i in items)) continue;
          frame[itemSlot] = items[i];
          if (needsLoop) frame[loopSlot] = { index: i + 1, first: i === 0, last: i === length - 1, length };
          text += renderPlan(body, frame);
        }
        return text;
      };
      return external.size === 0 ? render([]) : { slots: [...external], render };
    }
    const raw = node.kind === 'raw';
    const latin1 = this.latin1;
    return read === undefined ? outputValue(value, raw, latin1)
      : { slots: [slot], render: (frame) => outputValue(read(frame), raw, latin1) };
  }
}

function sourceDir(path) {
  const slash = path.lastIndexOf('/');
  return slash < 0 ? '' : path.slice(0, slash);
}

const URI_SCHEME = /^[A-Za-z][A-Za-z0-9+.-]*:/;

function linkResolver(source, sources, statics, broken, base, latin1) {
  const dir = sourceDir(source.path);
  const directory = dir === '' ? [] : dir.split('/');
  const targets = new Map();
  return (target) => {
    const cached = targets.get(target);
    if (cached !== undefined) return cached;
    let href = target;
    if (target[0] !== '/' && target[0] !== '#' && !URI_SCHEME.test(target)) {
      const hash = target.indexOf('#');
      const relative = hash < 0 ? target : target.slice(0, hash);
      const parts = directory.slice();
      let valid = relative !== '';
      for (const part of relative.split('/')) {
        if (part === '' || part === '.') continue;
        if (part === '..') {
          if (parts.length === 0) {
            valid = false;
            break;
          }
          parts.pop();
        } else {
          parts.push(part);
        }
      }
      const path = valid ? parts.join('/') : null;
      const page = sources.get(path);
      const fragment = hash < 0 ? '' : target.slice(hash);
      if (page !== undefined) href = page.url + fragment;
      else if (statics.has(path)) href = base + '/' + path + fragment;
      else broken.add('broken-link ' + source.path + ' -> ' + target);
    }
    const escaped = htmlJson(href, latin1);
    targets.set(target, escaped);
    return escaped;
  };
}

function readingMinutes(body, latin1) {
  // The number of matches is ceil(words / 200). In byte mode every
  // Unicode whitespace character has one of the sequences in BYTE_SPACE.
  if (latin1 && !BYTE_SPACE.test(body)) {
    return Math.max(1, body.match(/[^\t-\r ]+(?:[\t-\r ]+[^\t-\r ]+){0,199}/g)?.length ?? 0);
  }
  if (latin1) body = decode(body);
  return Math.max(1, body.match(/\S+(?:\s+\S+){0,199}/g)?.length ?? 0);
}

function linkRecord(source) {
  if (source === undefined) return null;
  const { title, url, date, date_long } = source.card;
  return { title, url, date, date_long };
}

function makeCard(source, tagRefs, latin1) {
  const { title, date = '', summary = '' } = source.data;
  const dateLong = date ? Number(date.slice(8, 10)) + ' ' + MONTHS[Number(date.slice(5, 7)) - 1]
    + ' ' + date.slice(0, 4) : '';
  return {
    title,
    url: source.url,
    date,
    date_long: dateLong,
    summary,
    excerpt: source.markdown.excerpt,
    reading_minutes: readingMinutes(source.body, latin1),
    tags: tagRefs,
  };
}

function collectTags(posts, pages, base, latin1) {
  const tags = new Map();
  const slugs = new Map();
  const references = (source, post) => {
    const own = [];
    for (const name of source.data.tags ?? []) {
      let slug = slugs.get(name);
      if (slug === undefined) {
        slug = slugify(name, latin1);
        slugs.set(name, slug);
      }
      if (own.some((entry) => entry.slug === slug)) continue;
      let tag = tags.get(slug);
      if (tag === undefined && post) {
        tag = { ref: { name, slug, url: base + '/tags/' + slug + '/' }, posts: [] };
        tags.set(slug, tag);
      }
      if (post) tag.posts.push(source);
      own.push(tag?.ref ?? { name, slug, url: '' });
    }
    source.card = makeCard(source, own, latin1);
  };
  for (const post of posts) references(post, true);
  for (const page of pages) if (!page.post) references(page, false);
  return [...tags.values()].sort((a, b) => b.posts.length - a.posts.length || compare(a.ref.slug, b.ref.slug));
}

function run(input, opts) {
  const latin1 = opts?.latin1 === true;
  const base = input.site.base_url.replace(/\/+$/, '');
  const parsed = new Map();
  const report = [];
  const names = Object.keys(input.templates);
  const templateOrder = latin1 && names.some((name) => SUPPLEMENTARY_BYTE.test(name)) ? compareBytes : compare;
  for (const name of names.sort(templateOrder)) {
    try {
      parsed.set(name, parseTemplate(name, input.templates[name], latin1));
    } catch (error) {
      if (!(error instanceof TemplateSyntax)) throw error;
      report.push(error.message);
    }
  }

  const sources = [];
  const statics = [];
  const byPath = new Map();
  const dirs = new Map([['', new Set(RESERVED)]]);
  const pathOrder = latin1 && input.files.some((file) => SUPPLEMENTARY_BYTE.test(file.path)) ? compareBytes : compare;
  for (const file of [...input.files].sort((a, b) => pathOrder(a.path, b.path))) {
    const { path, content } = file;
    if (!path.endsWith('.md')) {
      statics.push(file);
      continue;
    }
    const front = frontMatter(content.includes('\r') ? content.replace(/\r\n?/g, '\n') : content, latin1);
    const post = path.startsWith('posts/');
    const own = path.startsWith('tags/') ? 'tags' : path.startsWith('page/') ? 'page' : '';
    const layout = front.data?.layout ?? (post ? 'post' : 'page');
    let reason = front.error;
    if (!reason) {
      if (own) reason = own + "/ is the generator's own";
      else if (post && front.data.date === undefined) reason = 'a post needs a date';
      else if (!parsed.has(layout)) reason = 'layout "' + layout + '" is no template that parses';
    }
    if (reason) {
      report.push('refused ' + path + ': ' + reason);
      continue;
    }
    const { data, body } = front;
    if (data.draft === true) {
      report.push('draft ' + path);
      continue;
    }
    const dir = post ? 'posts' : sourceDir(path);
    let used = dirs.get(dir);
    if (used === undefined) dirs.set(dir, used = new Set());
    const slug = reserveSlug(data.slug ?? slugify(data.title, latin1), used);
    const outDir = dir === '' ? slug : dir + '/' + slug;
    const source = { path, post, layout, data, body, slug, outDir, url: base + '/' + outDir + '/' };
    sources.push(source);
    byPath.set(path, source);
  }

  const staticPaths = new Set(statics.map((file) => file.path));
  const broken = new Set();
  const find = latin1 ? /[&<>"'\\\x60*\[\x00-\x1f]/g : /[&<>"'\\\x60*\[\x00-\x1f\ud800-\udfff]/g;
  for (const source of sources) {
    const link = linkResolver(source, byPath, staticPaths, broken, base, latin1);
    source.markdown = renderMarkdown(source.body, link, latin1, find);
  }

  const posts = sources.filter((source) => source.post);
  posts.sort((a, b) => compare(b.data.date, a.data.date) || compare(a.slug, b.slug));
  const tags = collectTags(posts, sources, base, latin1);
  const cards = posts.map((post, i) => {
    post.newer = posts[i - 1];
    post.older = posts[i + 1];
    return post.card;
  });
  const constants = {
    site: { title: input.site.title, base_url: base },
    tags: tags.map((tag) => ({ ...tag.ref, count: tag.posts.length })),
    recent: cards.slice(0, input.site.recent),
  };
  const templates = new Templates(parsed, constants, latin1);
  const pageScope = templates.scope(['page']);
  const indexScope = templates.scope(['posts', 'pager']);
  const tagScope = templates.scope(['tag']);
  const emptyScope = templates.scope([]);

  const files = [];
  const written = new Set();
  const write = (path, escaped) => {
    written.add(path);
    files.push(JSON.stringify(path) + ':"' + escaped + '"');
  };
  for (const source of sources) {
    const page = {
      ...source.card,
      slug: source.slug,
      content: source.markdown.content,
      toc: source.markdown.toc,
      newer: linkRecord(source.newer),
      older: linkRecord(source.older),
      is_post: source.post,
    };
    write(source.outDir + '/index.html', renderPlan(templates.template(source.layout, pageScope), [page]));
  }

  if (parsed.has('index')) {
    const perPage = input.site.per_page;
    const pages = Math.max(1, Math.ceil(posts.length / perPage));
    const indexUrl = (number) => number === 1 ? base + '/' : base + '/page/' + number + '/';
    const plan = templates.template('index', indexScope);
    for (let number = 1; number <= pages; number++) {
      const pager = {
        number,
        total: pages,
        url: indexUrl(number),
        newer_url: number === 1 ? '' : indexUrl(number - 1),
        older_url: number === pages ? '' : indexUrl(number + 1),
      };
      const frame = [cards.slice((number - 1) * perPage, number * perPage), pager];
      write(number === 1 ? 'index.html' : 'page/' + number + '/index.html', renderPlan(plan, frame));
    }
  }
  if (parsed.has('tag')) {
    const plan = templates.template('tag', tagScope);
    for (const tag of tags) {
      const record = { ...tag.ref, count: tag.posts.length, posts: tag.posts.map((post) => post.card) };
      write('tags/' + record.slug + '/index.html', renderPlan(plan, [record]));
    }
  }
  if (parsed.has('tags')) write('tags/index.html', renderPlan(templates.template('tags', emptyScope), []));

  for (const { path, content } of statics) {
    if (written.has(path)) report.push('shadowed ' + path + ': a built page has its path');
    else if (path !== '__proto__') write(path, jsonText(content));
  }
  for (const warning of broken) report.push(warning);
  report.sort(latin1 && report.some((line) => SUPPLEMENTARY_BYTE.test(line)) ? compareBytes : compare);
  report.push('built ' + (sources.length - posts.length) + ' pages, ' + posts.length + ' posts, '
    + tags.length + ' tags and ' + statics.length + ' static files');
  write('build-report.txt', jsonText(report.join('\n') + '\n'));
  const answer = '{' + files.join(',') + '}';
  return opts?.json ? answer : JSON.parse(answer);
}

module.exports = { run };

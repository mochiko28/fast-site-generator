'use strict';

const HIGH = /[^\x00-\x7f]/;
const UNICODE_SPACE = /\s/;
const ASCII_SPACE = /[\t-\r ]/;
const ASCII_TRIM = /^[\t-\r ]+|[\t-\r ]+$/g;
const BYTE_SPACE = /\xc2\xa0|\xe1\x9a\x80|\xe2\x80[\x80-\x8a\xa8\xa9\xaf]|\xe2\x81\x9f|\xe3\x80\x80|\xef\xbb\xbf/;
const MARKDOWN_BYTE_SPACE = new RegExp(ASCII_SPACE.source + '|' + BYTE_SPACE.source);
const BYTE_SEPARATOR = /\xe2\x80[\xa8\xa9]/;
const MARKDOWN_BYTE_SEPARATOR = /\r|\xe2\x80[\xa8\xa9]/;
const MARKDOWN_SEPARATOR = /[\r\u2028\u2029]/;
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
  return latin1 ? MARKDOWN_BYTE_SPACE.test(text) : UNICODE_SPACE.test(text);
}

const COMBINING_MARKS = /\p{M}/gu;
const SLUG_SEPARATORS = /[^A-Za-z0-9]+/g;

function slugify(text, latin1) {
  if (HIGH.test(text)) {
    if (latin1) text = decode(text);
    text = text.normalize('NFKD').replace(COMBINING_MARKS, '');
  }
  const slug = text.replace(SLUG_SEPARATORS, '-');
  const start = slug.charCodeAt(0) === 45 ? 1 : 0;
  const end = Math.min(slug.length, start + 64);
  return slug.slice(start, slug.charCodeAt(end - 1) === 45 ? end - 1 : end).toLowerCase() || 'untitled';
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

function trimLine(line, start, end) {
  while (line.charCodeAt(start) === 32 || line.charCodeAt(start) === 9) start++;
  while (end > start && (line.charCodeAt(end - 1) === 32 || line.charCodeAt(end - 1) === 9)) end--;
  return line.slice(start, end);
}

function onlyIndent(line, start = 0, end = line.length) {
  for (let i = start; i < end; i++) {
    const c = line.charCodeAt(i);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

const QUOTED_SPECIAL = /["\\]/;

function quotedValue(raw) {
  if (raw[0] === '[') throw 'a bare string opens on [';
  if (raw[0] !== '"') return raw;
  if (raw.length < 2 || raw.at(-1) !== '"') throw 'a quoted string is not closed';
  const inner = raw.slice(1, -1);
  if (!QUOTED_SPECIAL.test(inner)) return inner;
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
const DATE_FIELD = /^\d{4}-\d{2}-\d{2}$/;
const INVALID_TAG = /["[\]]/;
const LAYOUT_NAME = /^[a-z0-9-]+$/;
const EXPLICIT_SLUG = /^[a-z0-9]+(?:-[a-z0-9]+)*$/;
const FRONT_PADDING = /[ \t]+$/;
const BAD_DATE = 'not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)';

function twoDigits(text, at) {
  return (text.charCodeAt(at) - 48) * 10 + (text.charCodeAt(at + 1) - 48);
}

function frontValue(key, raw, latin1) {
  switch (key) {
    case 'title':
    case 'summary':
      return quotedValue(raw);
    case 'date': {
      if (!DATE_FIELD.test(raw)) throw BAD_DATE;
      const year = 100 * twoDigits(raw, 0) + twoDigits(raw, 2);
      const month = twoDigits(raw, 5);
      const day = twoDigits(raw, 8);
      const limit = month === 2 ? (year % 4 === 0 ? 29 : 28)
        : (month === 4 || month === 6 || month === 9 || month === 11 ? 30 : 31);
      // Within 1970–2099 the century year, 2000, is a leap year.
      if (year < 1970 || year > 2099 || month < 1 || month > 12 || day < 1 || day > limit) throw BAD_DATE;
      return raw;
    }
    case 'tags': {
      if (raw[0] !== '[' || raw.at(-1) !== ']') throw 'not a list [a, b]';
      const inner = raw.slice(1, -1);
      if (inner === '') return [];
      const tags = inner.split(',');
      for (let i = 0; i < tags.length; i++) {
        const name = trim(tags[i], latin1);
        if (name === '' && tags.length === 1) return [];
        if (!name || INVALID_TAG.test(name)) throw 'an empty tag, or one holding " [ or ]';
        tags[i] = name;
      }
      return tags;
    }
    case 'draft':
      if (raw === 'true') return true;
      if (raw === 'false') return false;
      throw 'true or false';
    case 'layout':
      if (LAYOUT_NAME.test(raw)) return raw;
      throw 'not a template name';
    case 'slug':
      if (raw.length <= 64 && EXPLICIT_SLUG.test(raw)) return raw;
      throw 'not a slug';
  }
}

// Fixed field order rules out duplicate or unknown keys.
const ORDERED_FRONT = new RegExp(
  String.raw`^---\n(?:#[^\n]*\n)?title: (.*[^ \t\n\r\u2028\u2029])\n`
  + String.raw`(?:date: (\d{4}-\d\d-\d\d)\n)?(?:tags: (\[.*\])\n)?(?:draft: (true|false)\n)?`
  + String.raw`(?:slug: ([a-z0-9]+(?:-[a-z0-9]+)*)\n)?(?:summary: (.*[^ \t\n\r\u2028\u2029])\n)?`
  + String.raw`(?:layout: ([a-z0-9-]+)\n)?---(?:\n|$)`,
);

function frontMatter(text, latin1) {
  const match = ORDERED_FRONT.exec(text);
  if (match !== null && (!latin1 || !BYTE_SEPARATOR.test(match[0]))) {
    const [, title, date, tags, draft, slug, summary, layout] = match;
    if (slug === undefined || slug.length <= 64) {
      try {
        const data = { title: quotedValue(title), slug, layout };
        if (date !== undefined) data.date = frontValue('date', date, latin1);
        if (tags !== undefined) data.tags = frontValue('tags', tags, latin1);
        if (draft !== undefined) data.draft = draft === 'true';
        if (summary !== undefined) data.summary = quotedValue(summary);
        return { data, body: text.slice(match[0].length) };
      } catch {
        // The general parser reports the first invalid field.
      }
    }
  }
  return parseFrontMatter(text, latin1);
}

function parseFrontMatter(text, latin1) {
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

const RENDERED_JSON = Symbol();
const RENDERED_TEXT = Symbol();

class RenderedString {
  constructor(text, encoded = false) {
    this[RENDERED_JSON] = encoded ? text : jsonText(text);
    this[RENDERED_TEXT] = undefined;
  }

  plain() {
    return this[RENDERED_TEXT] ??= JSON.parse('"' + this[RENDERED_JSON] + '"');
  }
}

const HTML_SPECIAL = /[&<>"']/g;
const htmlEntity = (character) => HTML_ENTITIES[character];

function escapeHtml(text) {
  return text.replace(HTML_SPECIAL, htmlEntity);
}

const HTML_ENTITY = /&(?:amp|lt|gt|quot|#39);/g;
const ENTITY_TEXT = { '&amp;': '&', '&lt;': '<', '&gt;': '>', '&quot;': '"', '&#39;': "'" };

const entityText = (entity) => ENTITY_TEXT[entity];

function unescapeHtml(text) {
  return text.includes('&') ? text.replace(HTML_ENTITY, entityText) : text;
}

const INLINE_MARK = /[\\`*\[]/g;
const ESCAPABLE = '\\`*_[]()#+.!-';
// Delimiters around plain text can be consumed without another inline pass.
const SIMPLE_STRONG = /[^*\\`\[]+\*\*/y;
const SIMPLE_EMPHASIS = /[^*\\`\[]+\*(?!\*)/y;
const SIMPLE_LABEL = /[^\]\\`*\[]*\]\(/y;

function findCloser(text, mark, from, singleStar = false) {
  let close = text.indexOf(mark, from);
  while (close !== -1) {
    // Consecutive backslashes escape each other in pairs. The scan starts
    // inside the current construct, so earlier backslashes do not count.
    let slash = close;
    while (slash > from && text.charCodeAt(slash - 1) === 92) slash--;
    if ((close - slash) % 2 === 0) {
      if (!singleStar || text.charCodeAt(close + 1) !== 42) return close;
      close++;
    }
    close = text.indexOf(mark, close + 1);
  }
  return -1;
}

function renderInline(text, link, latin1) {
  let out = '';
  let copied = 0;
  INLINE_MARK.lastIndex = 0;
  while (INLINE_MARK.test(text)) {
    const at = INLINE_MARK.lastIndex - 1;
    let after = at + 1;
    let html;
    switch (text.charCodeAt(at)) {
      case 92:
        // A raw > is escapable and has already become an entity.
        if (after < text.length && (ESCAPABLE.includes(text[after]) || text.startsWith('&gt;', after))) html = '';
        break;
      case 96: {
        const close = text.indexOf('`', after);
        if (close > after) {
          html = '<code>' + text.slice(after, close) + '</code>';
          after = close + 1;
        }
        break;
      }
      case 42: {
        const strong = text.charCodeAt(after) === 42;
        if (strong) after++;
        const simple = strong ? SIMPLE_STRONG : SIMPLE_EMPHASIS;
        simple.lastIndex = after;
        const plain = simple.test(text);
        const width = strong ? 2 : 1;
        const close = plain ? simple.lastIndex - width : findCloser(text, strong ? '**' : '*', after, !strong);
        if (close > after) {
          const child = text.slice(after, close);
          const inner = plain ? child : renderInline(child, link, latin1);
          html = strong ? '<strong>' + inner + '</strong>' : '<em>' + inner + '</em>';
          after = close + width;
        }
        break;
      }
      case 91: {
        SIMPLE_LABEL.lastIndex = after;
        const plain = SIMPLE_LABEL.test(text);
        const labelEnd = plain ? SIMPLE_LABEL.lastIndex - 2 : findCloser(text, ']', after);
        if (labelEnd === -1 || text.charCodeAt(labelEnd + 1) !== 40) break;
        const close = text.indexOf(')', labelEnd + 2);
        if (close <= labelEnd + 2) break;
        const target = text.slice(labelEnd + 2, close);
        if (hasSpace(target, latin1)) break;
        const href = link(target);
        const label = text.slice(after, labelEnd);
        html = '<a href="' + href + '">' + (plain ? label : renderInline(label, link, latin1)) + '</a>';
        after = close + 1;
        break;
      }
    }
    if (html !== undefined) {
      out += text.slice(copied, at) + html;
      copied = after;
      // An escaped structural character is copied with the literal text.
      INLINE_MARK.lastIndex = html === '' ? after + 1 : after;
    } else if (after > at + 1) {
      INLINE_MARK.lastIndex = after;
    }
  }
  return copied === 0 ? text : out + text.slice(copied);
}

const FENCE = 7;
const BREAK = 8;
const BLANK = 9;

function blockStart(body, at, end, separator) {
  if (at === end) return BLANK;
  const first = body.charCodeAt(at);
  if (first > 45 && first !== 96) return 0;
  if (first === 35) {
    let text = at + 1;
    while (body.charCodeAt(text) === 35) text++;
    const level = text - at;
    const next = body.charCodeAt(text);
    if (level <= 6 && (next === 32 || next === 9) && !onlyIndent(body, text + 1, end)
      && (separator === null || !separator.test(body.slice(text, end)))) return level;
  } else if (first === 96 && body.startsWith('```', at)) {
    let i = at + 3;
    for (; i < end; i++) {
      const c = body.charCodeAt(i);
      const lower = c | 32;
      if ((lower >= 97 && lower <= 122) || (c >= 48 && c <= 57) || c === 95 || c === 43 || c === 45) continue;
      break;
    }
    if (onlyIndent(body, i, end)) return FENCE;
  } else if ((first === 45 || first === 42) && body.charCodeAt(at + 1) === first
    && body.charCodeAt(at + 2) === first && onlyIndent(body, at + 3, end)) {
    return BREAK;
  } else if ((first === 32 || first === 9) && onlyIndent(body, at + 1, end)) {
    return BLANK;
  }
  return 0;
}

function listMarker(body, at, end, separator) {
  const first = body.charCodeAt(at);
  if (first === 42 || first === 45) {
    return body.charCodeAt(at + 1) === 32
      && (separator === null || !separator.test(body.slice(at, end))) ? 2 : 0;
  }
  if (first < 48 || first > 57 || at === end) return 0;
  let digits = 1;
  for (; digits < 10; digits++) {
    const next = body.charCodeAt(at + digits);
    if (!(next >= 48 && next <= 57)) break;
  }
  return digits < 10 && body.charCodeAt(at + digits) === 46 && body.charCodeAt(at + digits + 1) === 32
    && (separator === null || !separator.test(body.slice(at, end))) ? digits + 2 : 0;
}

const BYTE_SPACE_AT = new RegExp(BYTE_SPACE.source, 'y');

function continuesList(body, at, end, latin1) {
  if (body.charCodeAt(at) !== 32 || body.charCodeAt(at + 1) !== 32) return false;
  let i = at + 2;
  while (body.charCodeAt(i) === 32) i++;
  if (i === end) return false;
  const next = body.charCodeAt(i);
  if (next < 128) return next < 9 || next > 13;
  if (!latin1) return !UNICODE_SPACE.test(body[i]);
  BYTE_SPACE_AT.lastIndex = i;
  return !BYTE_SPACE_AT.test(body);
}

function lineEnd(body, at) {
  const end = body.indexOf('\n', at);
  return end === -1 ? body.length : end;
}

function renderMarkdown(source, link, latin1) {
  const body = escapeHtml(source);
  const lineSeparator = latin1 ? MARKDOWN_BYTE_SEPARATOR : MARKDOWN_SEPARATOR;
  const separator = lineSeparator.test(body) ? lineSeparator : null;
  const ids = new Set();
  const toc = [];
  let html = '';
  let excerpt = null;
  let at = 0;
  let end = lineEnd(body, at);
  let kind = blockStart(body, at, end, separator);

  while (at <= body.length) {
    if (kind === FENCE) {
      const language = trimLine(body, at + 3, end);
      const open = language ? '<pre><code class="language-' + language + '">' : '<pre><code>';
      const start = end + 1;
      let fence = body.indexOf('\n```', end);
      while (fence !== -1) {
        end = lineEnd(body, fence + 1);
        if (onlyIndent(body, fence + 4, end)) break;
        fence = body.indexOf('\n```', end);
      }
      let code = '';
      if (fence !== -1) code = body.slice(start, fence + 1);
      else if (start <= body.length) code = body.slice(start) + '\n';
      html += open + code + '</code></pre>\n';
      at = fence === -1 ? body.length + 1 : end + 1;
    } else if (kind === BREAK) {
      html += '<hr>\n';
      at = end + 1;
    } else if (kind > 0 && kind < FENCE) {
      const heading = trimLine(body, at + kind, end);
      const id = reserveSlug(slugify(unescapeHtml(heading), latin1), ids);
      const text = renderInline(heading, link, latin1);
      html += '<h' + kind + ' id="' + id + '">' + text + '</h' + kind + '>\n';
      if (kind === 2 || kind === 3) toc.push({ level: kind, id, html: new RenderedString(text) });
      at = end + 1;
    } else if (kind === BLANK) {
      at = end + 1;
      while (body.charCodeAt(at) === 10) at++;
    } else if (body.charCodeAt(at) === 38 && body.startsWith('&gt;', at)) {
      // An actual '>' becomes '&gt;' while a literal '&gt;' becomes '&amp;gt;'.
      let content = '';
      do {
        const next = trimLine(body, at + 4, end);
        if (next) content = content ? content + '\n' + next : next;
        at = end + 1;
        end = lineEnd(body, at);
      } while (body.startsWith('&gt;', at));
      html += content
        ? '<blockquote>\n<p>' + renderInline(content, link, latin1) + '</p>\n</blockquote>\n'
        : '<blockquote>\n</blockquote>\n';
      kind = blockStart(body, at, end, separator);
      continue;
    } else {
      const first = body.charCodeAt(at);
      const marker = first === 42 || first === 45 || (first >= 48 && first <= 57)
        ? listMarker(body, at, end, separator) : 0;
      if (marker) {
        const ordered = marker !== 2;
        const tag = ordered ? 'ol' : 'ul';
        const start = ordered ? Number(body.slice(at, at + marker - 2)) : 1;
        html += start !== 1 ? '<ol start="' + start + '">\n' : '<' + tag + '>\n';
        let item = trimLine(body, at + marker, end);
        at = end + 1;
        while (at <= body.length) {
          end = lineEnd(body, at);
          const prefix = listMarker(body, at, end, separator);
          if ((ordered && prefix > 2) || (!ordered && prefix === 2)) {
            html += '<li>' + renderInline(item, link, latin1) + '</li>\n';
            item = trimLine(body, at + prefix, end);
          } else if (continuesList(body, at, end, latin1)) {
            item += '\n' + trimLine(body, at, end);
          } else {
            break;
          }
          at = end + 1;
        }
        html += '<li>' + renderInline(item, link, latin1) + '</li>\n</' + tag + '>\n';
        kind = blockStart(body, at, end, separator);
        continue;
      }
      // Keep consecutive untrimmed lines in a single slice of the body.
      let from = at;
      let paragraph = '';
      let last;
      do {
        const head = body.charCodeAt(at);
        const tail = body.charCodeAt(end - 1);
        if (head === 32 || head === 9 || tail === 32 || tail === 9) {
          paragraph += body.slice(from, at) + trimLine(body, at, end);
          from = end;
        }
        last = end;
        at = end + 1;
        if (at > body.length) break;
        end = lineEnd(body, at);
        kind = blockStart(body, at, end, separator);
      } while (kind === 0);
      const text = renderInline(paragraph + body.slice(from, last), link, latin1);
      excerpt ??= text;
      html += '<p>' + text + '</p>\n';
      continue;
    }
    end = lineEnd(body, at);
    kind = blockStart(body, at, end, separator);
  }
  return { content: html, toc, excerpt: new RenderedString(excerpt ?? '') };
}

// Serialize the rendered bodies together so V8 can flatten their HTML in
// one pass. If a body contains the separator, encode each body on its own.
function encodeContents(sources) {
  let length = sources.length - 1;
  const bodies = sources.map((source) => {
    const content = source.markdown.content;
    length += content.length;
    return content;
  });
  let fragments;
  // Limit the size of the temporary string even when every code unit escapes.
  if (bodies.length > 1 && length <= 32 * 1024 * 1024) {
    const separator = '\x7f';
    const batch = jsonText(bodies.join(separator)).split(separator);
    if (batch.length === bodies.length) fragments = batch;
  }
  for (let i = 0; i < sources.length; i++) {
    sources[i].markdown.content = new RenderedString(fragments?.[i] ?? bodies[i], fragments !== undefined);
  }
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
  if (value !== null && typeof value === 'object' && Object.hasOwn(value, name)) {
    return value[name];
  }
}

function outputValue(value, raw, latin1) {
  switch (typeof value) {
    case 'string': return raw ? jsonText(value) : htmlJson(value, latin1);
    case 'number': return String(value);
    case 'boolean': return value ? 'true' : 'false';
    default:
      if (value instanceof RenderedString) return raw ? value[RENDERED_JSON] : jsonText(escapeHtml(value.plain()));
      return Array.isArray(value) ? value.map((item) => outputValue(item, raw, latin1)).join(', ') : '';
  }
}

function truthy(value) {
  if (!value) return Number.isNaN(value);
  if (typeof value !== 'object') return true;
  if (value instanceof RenderedString) return value[RENDERED_JSON].length !== 0;
  return !Array.isArray(value) || value.length !== 0;
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
  const strings = [''];
  const renderers = [];
  const slots = new Set();
  for (const plan of plans) {
    if (typeof plan === 'string') {
      strings[strings.length - 1] += plan;
    } else {
      renderers.push(plan);
      strings.push('');
      for (const slot of plan.slots) slots.add(slot);
    }
  }
  if (renderers.length === 0) return strings[0];
  if (renderers.length === 1 && strings[0] === '' && strings[1] === '') return renderers[0];
  return {
    slots: [...slots],
    render(frame) {
      let output = strings[0];
      for (let i = 0; i < renderers.length; i++) output += renderers[i].render(frame) + strings[i + 1];
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
    this.escapedStrings = new Map();
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
    if (read === undefined) return outputValue(value, raw, latin1);
    if (raw) return { slots: [slot], render: (frame) => outputValue(read(frame), true, latin1) };
    const cache = this.escapedStrings;
    return {
      slots: [slot],
      render(frame) {
        const text = read(frame);
        if (typeof text !== 'string' || text.length > 128) return outputValue(text, false, latin1);
        let escaped = cache.get(text);
        if (escaped === undefined) {
          escaped = htmlJson(text, latin1);
          cache.set(text, escaped);
        }
        return escaped;
      },
    };
  }
}

function sourceDir(path) {
  const slash = path.lastIndexOf('/');
  return slash < 0 ? '' : path.slice(0, slash);
}

const HTML_PUNCTUATION = /[&<>"']/;
const URI_SCHEME = /^[A-Za-z][A-Za-z0-9+.-]*:/;
const EMPTY_DIRECTORY_SEGMENT = /^\/|\/\/|\/$/;
const PATH_NEEDS_NORMALIZATION = /(?:^|\/)\.{1,2}(?=\/|$)|\/\/|\/$/;

function normalizedPath(dir, target, end) {
  if (end === 0) return null;
  const relative = end === target.length ? target : target.slice(0, end);
  if (!PATH_NEEDS_NORMALIZATION.test(relative)) return dir ? dir + '/' + relative : relative;
  if (!EMPTY_DIRECTORY_SEGMENT.test(dir)) {
    let path = dir;
    for (let at = 0; at < end;) {
      const slash = target.indexOf('/', at);
      const stop = slash < 0 || slash > end ? end : slash;
      const size = stop - at;
      if (size === 2 && target.startsWith('..', at)) {
        if (path === '') return null;
        const parent = path.lastIndexOf('/');
        path = parent < 0 ? '' : path.slice(0, parent);
      } else if (size !== 0 && !(size === 1 && target.charCodeAt(at) === 46)) {
        const segment = target.slice(at, stop);
        path = path === '' ? segment : path + '/' + segment;
      }
      at = stop + 1;
    }
    return path;
  }
  const parts = dir === '' ? [] : dir.split('/');
  for (const part of relative.split('/')) {
    if (part === '' || part === '.') continue;
    if (part === '..') {
      if (parts.length === 0) return null;
      parts.pop();
    } else {
      parts.push(part);
    }
  }
  return parts.join('/');
}

function linkResolver(sources, statics, broken, base) {
  const directories = new Map();
  let sourcePath;
  let dir;
  let targets;
  let sourceWarnings;

  function from(path) {
    sourcePath = path;
    dir = sourceDir(path);
    targets = directories.get(dir);
    sourceWarnings = undefined;
  }

  function link(escapedTarget) {
    const first = escapedTarget[0];
    if (first === '#' || first === '/' || URI_SCHEME.test(escapedTarget)) return escapedTarget;
    if (targets === undefined) directories.set(dir, targets = new Map());
    let resolved = targets.get(escapedTarget);
    if (resolved === undefined) {
      const target = unescapeHtml(escapedTarget);
      const hash = target.indexOf('#');
      const path = normalizedPath(dir, target, hash < 0 ? target.length : hash);
      const page = sources.get(path);
      if (page !== undefined || statics.has(path)) {
        const fragment = hash < 0 ? '' : target.slice(hash);
        const href = (page !== undefined ? page.url : base + '/' + path) + fragment;
        resolved = HTML_PUNCTUATION.test(href) ? escapeHtml(href) : href;
      } else {
        resolved = null;
      }
      targets.set(escapedTarget, resolved);
    }
    if (resolved !== null) return resolved;
    sourceWarnings ??= new Set();
    if (!sourceWarnings.has(escapedTarget)) {
      sourceWarnings.add(escapedTarget);
      broken.add('broken-link ' + sourcePath + ' -> ' + unescapeHtml(escapedTarget));
    }
    return escapedTarget;
  }

  return { from, link };
}

function hasUnicodeByteSpace(body) {
  // Most bodies have none of these byte sequences. Search the fixed ones
  // directly and inspect the final byte only for the shared U+20xx prefix.
  if (body.includes('\xc2\xa0') || body.includes('\xe1\x9a\x80') || body.includes('\xe2\x81\x9f')
    || body.includes('\xe3\x80\x80') || body.includes('\xef\xbb\xbf')) return true;
  let i = body.indexOf('\xe2\x80');
  while (i !== -1) {
    const tail = body.charCodeAt(i + 2);
    if ((tail >= 0x80 && tail <= 0x8a) || tail === 0xa8 || tail === 0xa9 || tail === 0xaf) return true;
    i = body.indexOf('\xe2\x80', i + 3);
  }
  return false;
}

const ASCII_WORD_GROUP = /[^\t-\r ]+(?:[\t-\r ]+[^\t-\r ]+){0,199}/g;
const WORD_GROUP = /\S+(?:\s+\S+){0,199}/g;

function countWordGroups(body, pattern) {
  let count = 0;
  while (pattern.test(body)) count++;
  return Math.max(1, count);
}

function readingMinutes(body, latin1) {
  // A 201st word needs at least 401 code units including its separators.
  if (body.length <= 400) return 1;
  // The number of matches is ceil(words / 200). In byte mode every
  // Unicode whitespace character has one of the sequences in BYTE_SPACE.
  if (latin1 && !hasUnicodeByteSpace(body)) {
    return countWordGroups(body, ASCII_WORD_GROUP);
  }
  if (latin1) body = decode(body);
  return countWordGroups(body, WORD_GROUP);
}

function linkRecord(source) {
  if (source === undefined) return null;
  const { title, url, date, date_long } = source.card;
  return { title, url, date, date_long };
}

function makeCard(source, tagRefs, latin1) {
  const { title, date = '', summary = '' } = source.data;
  const dateLong = date ? twoDigits(date, 8) + ' ' + MONTHS[twoDigits(date, 5) - 1]
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

const END_SLASHES = /\/+$/;
const LINE_ENDINGS = /\r\n?/g;

function run(input, opts) {
  const latin1 = opts?.latin1 === true;
  const base = input.site.base_url.replace(END_SLASHES, '');
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
    const front = frontMatter(content.includes('\r') ? content.replace(LINE_ENDINGS, '\n') : content, latin1);
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
  const { from, link } = linkResolver(byPath, staticPaths, broken, base);
  for (const source of sources) {
    from(source.path);
    source.markdown = renderMarkdown(source.body, link, latin1);
  }

  encodeContents(sources);
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

  let files = '';
  let comma = '{';
  const written = new Set();
  const write = (path, escaped) => {
    written.add(path);
    files += comma + JSON.stringify(path) + ':"' + escaped + '"';
    comma = ',';
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
  const answer = files + '}';
  return opts?.json ? answer : JSON.parse(answer);
}

module.exports = { run };

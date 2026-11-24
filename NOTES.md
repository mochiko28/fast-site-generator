# makefun-site drop-in

The driver normally carries UTF-8 bytes in Latin-1 strings from input to output. An escaped non-ASCII code unit selects UTF-8 mode for the whole input file. The byte mode recognizes the UTF-8 sequences for JavaScript whitespace and line separators. Slug normalization and comparisons that may differ in UTF-16 order decode the relevant strings.

Markdown HTML-escapes each body up front, then scans block boundaries and inline marks. Paragraphs reuse contiguous body slices. Heading slugs and link resolution use the original text recovered from entities. Each rendered body, excerpt, and TOC label passes through native JSON serialization once. The renderer decodes fragments lazily for escaped template output.

Reading time still matches groups of up to 200 words. In byte mode, native string searches detect Unicode whitespace before choosing the word pattern.

On practice, the seven-run median CPU time went from 279 ms to 267 ms. The practice set and all 32 additional generated sets matched the reference.

Templates compile against numeric scope slots. Compilation folds constant expressions and subtrees, specializes includes by their referenced bindings and depth, and caches eligible loop output by element identity. Dynamic render steps alternate with prejoined literal strings. Each build tracks written paths for static-file shadowing. The driver writes each answer as it finishes. All case data and render caches belong to one build.

The latest pass matches common front-matter layouts in one native call and uses the general parser for detailed errors. It trims slug edges by index, bypasses relative resolution for absolute links, and skips the word scan for bodies of at most 400 code units. Each build caches escaped template strings of at most 128 code units and concatenates the answer for the driver to flatten on write.

The nine-run practice median was 269 ms, down from 279 ms for v3. All 43 sets passed and their answer files matched v3 byte for byte. That includes eight new seeded sets and targeted cases in both encoding modes. The paragraph scan, guarded inline replacements, and GC settings tested in this pass were dropped after timing them.

## Native addon

Run `native/build.sh` to rebuild the Linux x86-64 `site.node` with GCC. The addon has no dynamic dependencies or versioned symbols. `main.ts` loads it before the JS driver. The addon reads both directory overrides from the initial environment, builds the cases in a single arena, and buffers the answer across cases. It writes a sibling temporary file, renames it into place after the write and close succeed, and calls `exit_group(0)`. An unsupported input or failed native check returns to the JS driver. The arena reserves 384 MiB of virtual address space, with 128 MiB available for the final JSON. Scratch allocations are reused between cases.

Generated inputs parse in one pass. If an escaped non-ASCII code unit selects UTF-16 mode, the addon restarts the buffered build in that mode. Strings use UTF-8 with a representation for isolated surrogates. Comparisons follow UTF-16 code-unit order, and Markdown word counts use JavaScript's whitespace set. `native/gen-unicode.mjs` generates the slug table from Node 22.23.5's Unicode 17.0 NFKD and mark classification. The table projects the result onto ASCII alphanumerics and separators after dropping marks. Markdown produces JSON-escaped HTML as it renders, and template includes reuse output when their referenced bindings permit it.

### Verification

- The kit generator and oracle produced 315 sites from 105 distinct SHA-256 seeds. All answer files matched the oracle byte for byte. The native path handled all 315 sites.
- `test.sh`, `test2.sh`, and `test3.sh` produced 9, 16, and 8 `OK` lines. All 20 sets in `cases4` and `cases5` passed through C, including both copies of `edge` and `edge-utf16`.
- Another 200 synthetic inputs matched the JS driver's output bytes, covering both encoding modes, higher Unicode code points, isolated surrogates, control characters, and mixed-mode detection after earlier cases. A batch with unsupported fractional pagination preserved the existing output on native failure, then the JS fallback produced the expected bytes.
- The kit's process-driver worktree grade returned `{"cases_passed":3,"cases_total":3,"passed":1}`. The CLI defaults to grading HEAD, so `--worktree` was used for the uncommitted addon. In this sandbox a scratch preload redirected unused Git subprocess stdin from a pipe to `/dev/null` to avoid `spawnSync git EPERM`.

The instrumented JS port measured non-ASCII text on the same 315 generated sites.

| Operation | Non-ASCII calls | Total calls | Share |
| --- | ---: | ---: | ---: |
| Slugification | 28,368 | 285,755 | 9.93% |
| HTML escaping, including full Markdown bodies | 106,548 | 107,368 | 99.24% |
| Combined HTML and JSON escaping of template values | 9,134 | 450,690 | 2.03% |
| JSON serialization of strings and output keys | 170,008 | 487,331 | 34.89% |

### Practice CPU time

The 11-run `ru.py` medians were 268 ms for `v5base`, 112 ms for the native entry, and 88 ms for the kit's null entry. Subtracting the control median gives 180 ms for `v5base` and 24 ms for the native entry. In a separate 21-run comparison with the full driver, `require` and `process.dlopen` both had a 113 ms median. The entry uses the shorter `require` call. An instrumented build measured 30.1 ms of native thread CPU time for the three practice cases, including file I/O. The native stage exceeded the 10 ms target on this host.

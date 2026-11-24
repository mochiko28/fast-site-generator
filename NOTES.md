# makefun-site drop-in

The driver normally carries UTF-8 bytes in Latin-1 strings from input to output. An escaped non-ASCII code unit selects UTF-8 mode for the whole input file. The byte mode recognizes the UTF-8 sequences for JavaScript whitespace and line separators. Slug normalization and comparisons that may differ in UTF-16 order decode the relevant strings.

Markdown HTML-escapes each body up front, then scans block boundaries and inline marks. Paragraphs reuse contiguous body slices. Heading slugs and link resolution use the original text recovered from entities. Each rendered body, excerpt, and TOC label passes through native JSON serialization once. The renderer decodes fragments lazily for escaped template output.

Reading time still matches groups of up to 200 words. In byte mode, native string searches detect Unicode whitespace before choosing the word pattern.

On practice, the seven-run median CPU time went from 279 ms to 267 ms. The practice set and all 32 additional generated sets matched the reference.

Templates compile against numeric scope slots. Compilation folds constant expressions and subtrees, specializes includes by their referenced bindings and depth, and caches eligible loop output by element identity. Dynamic render steps alternate with prejoined literal strings. Each build tracks written paths for static-file shadowing. The driver writes each answer as it finishes. All case data and render caches belong to one build.

The latest pass matches common front-matter layouts in one native call and uses the general parser for detailed errors. It trims slug edges by index, bypasses relative resolution for absolute links, and skips the word scan for bodies of at most 400 code units. Each build caches escaped template strings of at most 128 code units and concatenates the answer for the driver to flatten on write.

The nine-run practice median was 269 ms, down from 279 ms for v3. All 43 sets passed and their answer files matched v3 byte for byte. That includes eight new seeded sets and targeted cases in both encoding modes. The paragraph scan, guarded inline replacements, and GC settings tested in this pass were dropped after timing them.

## Native addon

Run `native/build.sh` to rebuild the Linux x86-64 `site.node` with GCC. The build uses the baseline x86-64 ISA and has no undefined dynamic symbols, dynamic dependencies, or versioned symbols. `main.ts` loads the addon first. If native processing returns, `main.ts` loads the original JS driver from `fallback.js`.

The addon reads both directory overrides from the initial environment and reads the input directly into its arena. A raw escape scan selects the string mode before the parser decodes strings in place. The parser also records carriage returns so Markdown normalization can skip unaffected files. Strings use UTF-8 with a representation for isolated surrogates. Comparisons follow UTF-16 code-unit order, and Markdown word counts use JavaScript's whitespace set. `native/gen-unicode.mjs` generates the slug table from Node 22.23.5's Unicode 17.0 NFKD and mark classification. The table projects the result onto ASCII alphanumerics and separators after dropping marks.

The first native pass indexed line breaks and counted words in one pass. It copied ordinary inline runs while scanning for marks and emitted JSON-escaped HTML for replacements. Templates compiled record properties to numeric IDs and reused included output when their referenced bindings permitted it. Known map sizes are reserved up front, larger tag lists deduplicate by hash, and each base slug remembers the next suffix to try.

The arena reserves 384 MiB of virtual address space with 128 MiB available for the final JSON. Scratch allocations are reused between cases. Templates write into the final JSON buffer. The first native pass published through a sibling temporary file and rename. Both native passes finish the answer buffer before opening the output and handle interrupted and short writes.

### Verification

- The final build matched the oracle byte for byte on all 315 sites generated from the original 105 SHA-256 seeds. The fallback probe recorded zero fallbacks.
- `test.sh`, `test2.sh`, and `test3.sh` produced 9, 16, and 8 `OK` lines. All 20 sets in `cases4` and `cases5` passed, including both copies of `edge` and `edge-utf16`.
- All 53 runs from those five test groups also produced answer files byte-identical to `cand6`.
- Another 200 synthetic inputs matched the JS driver's output bytes, covering both encoding modes, higher Unicode code points, isolated surrogates, control characters, and mixed-mode detection after earlier cases. A batch with unsupported fractional pagination preserved the existing output on native failure, then the JS fallback produced the expected bytes.
- The previous native pass's process-driver worktree grade returned `{"cases_passed":3,"cases_total":3,"passed":1}`. The CLI defaults to grading HEAD, so `--worktree` was used for that run.

The instrumented JS port measured non-ASCII text on the same 315 generated sites.

| Operation | Non-ASCII calls | Total calls | Share |
| --- | ---: | ---: | ---: |
| Slugification | 28,368 | 285,755 | 9.93% |
| HTML escaping, including full Markdown bodies | 106,548 | 107,368 | 99.24% |
| Combined HTML and JSON escaping of template values | 9,134 | 450,690 | 2.03% |
| JSON serialization of strings and output keys | 170,008 | 487,331 | 34.89% |

### Practice CPU time

The requested 21-run comparison used the production binaries and the original kit null entry.

```text
$ python3 /work/.tmp/ru.py 21 /work/.tmp/cand6 /work/entry /work/.tmp/t12/null
/work/.tmp/cand6             user 85 sys 24 total 108 minflt 6807 vcsw 15
/work/entry                  user 74 sys 23 total 97 minflt 6738 vcsw 15
/work/.tmp/t12/null          user 67 sys 20 total 87 minflt 7044 vcsw 20
```

CPU time above the kit null entry fell from 21 ms to 10 ms. A separate 21-run comparison with the literally empty `main.ts` measured 108 ms for `cand6`, 98 ms for the final entry, and 71 ms for the control. The corresponding net times were 37 ms and 27 ms.

Build with `native/build.sh -DSITE_PROFILE` to print phase times on stderr. The profiler measures thread CPU time at stage boundaries with `clock_gettime`, uses `rdtsc` to subdivide short nested phases, and records process fault deltas with `getrusage`. It reports phase durations in microseconds and total and startup CPU time in nanoseconds. Rebuild without the define for scoring.

| Practice phase | Baseline ms | Final ms |
| --- | ---: | ---: |
| Arena and environment | 0.500 | 0.509 |
| Input read | 0.669 | 0.684 |
| JSON parse | 3.001 | 2.804 |
| Front matter and site preparation | 1.872 | 1.383 |
| Markdown | 12.414 | 10.329 |
| Template compilation and rendering | 5.771 | 3.982 |
| Page and heading slugs | 1.012 | 0.633 |
| Output keys, statics and report serialization | 0.460 | 0.330 |
| Write, close and rename | 3.675 | 3.698 |
| **Total native thread CPU** | **29.561** | **24.683** |
| Minor faults during native processing | 12 | 11 |
| Major faults during native processing | 0 | 0 |

Each row is the median of 21 interleaved runs. Markdown remained the largest phase, and native CPU time was above the 8 ms target. Prefaulting 8 MiB of each arena region with `MADV_POPULATE_WRITE` measured 24.867 ms and 11 minor faults. The production arena remains lazy.

In a separate 21-run startup comparison, process CPU measured at addon entry was 76.184 ms with the original driver, 70.836 ms with the two-line `require` driver, and 70.556 ms with the two-line `process.dlopen` driver. The control measured 72.191 ms at its first statement. The entry uses `require`.

### Second native pass

- Markdown consumes newline masks as it counts word starts. ASCII vectors skip Unicode whitespace classification, and a padded SSE2 scan copies inline text directly into the JSON-escaped HTML buffer. A byte table dispatches inline marks and entities. Heading labels and the first-paragraph excerpt are slices of the rendered HTML. Successful relative links share results by directory and target within a case. Broken-link warnings still use the referring page.
- Templates compile once per case into contiguous operations. Record layouts map property IDs to direct slots. Paths bound to a template's own loops use precomputed scope offsets. Includes resolve inherited names in the caller's scope. Include output is reused only when its free names resolve to the same case globals, missing values, and at most one immutable loop item. The memo also accounts for include depth.
- The JSON parser borrows strings without escapes, as in the first pass. Escaped strings decode in place through SSE2 scans and a table of single-byte escapes. The parser stores scalar text and container children in the same slot.
- The common output path opens the regular `answers.json` after the full buffer is ready, locks it for native writers, writes from offset zero, and trims a stale tail when necessary. Symlinks, multiply linked files, and other exceptional paths retain temporary-file publication. Readers can see a direct write in progress.

The final binary produced the following verification results.

| Check | Output |
| --- | --- |
| Original SHA-256 fuzz seeds | 315 byte-identical sites from 105 seeds, 0 JS fallbacks |
| New SHA-256 fuzz seeds | 300 byte-identical sites from 100 seeds, 0 JS fallbacks |
| `test.sh` / `test2.sh` / `test3.sh` | 9 / 16 / 8 `OK` lines |
| `cases4` and `cases5` | 20 `OK` lines |
| Forced JS fallback and direct native overwrite | Both replaced a 12,000,000-byte file with the exact 7,446,432-byte practice answer |
| Additional synthetic checks | 200 byte-identical sites in two encoding modes, plus a failed native build that preserved the old output before JS rewrote it |
| ELF | No undefined dynamic symbols, `DT_NEEDED` entries, or symbol versions. GNU property reports `x86-64-baseline`. |

The generator hashed `makefun native addon fuzz 2026-11-24 round ` followed by the seed index with SHA-256. The original rerun used indices 0 through 104. The new run used 105 through 204.

The production binaries gave these 21-run medians with the kit null entry.

```text
$ python3 /work/.tmp/ru.py 21 /work/.tmp/cand7 /work/entry /work/.tmp/t12/null
/work/.tmp/cand7             user 38 sys 15 total 54 minflt 6717 vcsw 18
/work/entry                  user 39 sys 13 total 52 minflt 6718 vcsw 17
/work/.tmp/t12/null          user 36 sys 11 total 46 minflt 7014 vcsw 23
```

The phase comparison used 21 interleaved runs per build on CPU 7. The baseline was the `3749b8b` addon with output profiling split into separate operations. The phase comparison reused the same practice output path. The current baseline measured 13.602 ms and reached the addon in 37.498 ms of process CPU. The historical 24.683 ms run reached the addon in 70.968 ms, so the two builds were remeasured together.

| Practice phase | `3749b8b` ms | Second pass ms |
| --- | ---: | ---: |
| Arena and environment | 0.370 | 0.372 |
| Input read | 0.490 | 0.500 |
| JSON parse | 1.604 | 1.644 |
| Front matter and site preparation | 0.713 | 0.702 |
| Markdown | 5.168 | 5.071 |
| Template compilation and rendering | 2.146 | 2.012 |
| Page and heading slugs | 0.375 | 0.374 |
| Output keys, statics and report serialization | 0.163 | 0.154 |
| Open output and prepare to write | 0.044 | 0.013 |
| Write | 1.650 | 1.110 |
| Truncate | 0.000 | 0.000 |
| Close | 0.003 | 0.003 |
| Rename | 0.767 | 0.000 |
| **Total native thread CPU** | **13.602** | **11.990** |
| Minor faults | 11 | 11 |
| Major faults | 0 | 0 |

Rows are medians of individual phases. The total is the median of complete runs. The profiled build measured 11.9897 ms, just under 12 ms. Direct writes accounted for most of the measured reduction. The parse median increased by 0.040 ms.

The direct 1 MiB chunked trial measured 1.192 ms in write and 11.897 ms total. One full-buffer write measured 1.110 ms in write and remains the default. Short writes and interruptions retry with the remaining bytes.

Separate instrumentation on the baseline measured 0.939 ms for fused line indexing and word counts, 1.048 ms for the remaining block work, 3.130 ms for inline parsing and plain-text copies, 0.750 ms for instrumented escapes, 0.563 ms for link resolution, 0.152 ms for heading IDs, and 0.009 ms for excerpt capture. The added per-span probes increase the detailed run's CPU time.

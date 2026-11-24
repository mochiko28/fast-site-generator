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

The first two native passes reserved 384 MiB of virtual address space with 128 MiB available for the final JSON. Scratch allocations were reused between cases. Templates wrote into the final JSON buffer. The first native pass published through a sibling temporary file and rename. Both passes finished the answer buffer before opening the output and handled interrupted and short writes.

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

Each row is the median of 21 interleaved runs. Markdown remained the largest phase, and native CPU time was above the 8 ms target. Prefaulting 8 MiB of each arena region with `MADV_POPULATE_WRITE` measured 24.867 ms and 11 minor faults. That build kept its arena lazy.

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

### Native memory pass

- The input length determines the initial writable sizes for the input and scratch arena, rendered Markdown, and final JSON. The writable ranges can grow in place within a 646 MiB address reservation. The practice input starts with 6 MiB for input and scratch, 4 MiB for Markdown, and 10 MiB for final JSON. The environment and the two paths usually share one 4 KiB page.
- Rendered Markdown is packed back to back. Heading labels and excerpts keep slices across growth. Both the general and Markdown arenas rewind after each case, and merge sort releases its temporary storage immediately. Carriage-return normalization compacts the file's input bytes in place.
- The input uses one content read on the usual path, followed by the EOF check. `MAP_POPULATE` covers only the input prefix in complete 2 MiB regions. Those pages will all be written by the read. The remaining mappings use lazy paging. Each case renders directly into the final JSON buffer, which is written once on the usual path after the batch completes.

The I/O trial on `e5f5707` used 19 interleaved practice runs per variant with THP disabled. Linux included the prefaulted pages in the minor-fault counts. The populated read was faster than private file mapping even with file population. The per-case output trial reused the buffer by staging writes in a sibling file. With THP enabled, it took 12.549 ms of native CPU versus 11.884 ms for the single-write baseline.

| I/O trial, THP disabled | Native minor faults | Native CPU ms |
| --- | ---: | ---: |
| HEAD, lazy input and one output write | 3,688 | 14.676 |
| Lazy read into a separate mapping | 3,687 | 14.400 |
| Populated read into a separate mapping | 3,687 | 14.050 |
| Lazy private file mapping | 3,718 | 14.431 |
| Populated private file mapping | 3,687 | 14.349 |
| Populated output prefix, one write | 3,688 | 13.728 |
| Per-case writes to a sibling file | 2,631 | 14.498 |
| Populated read and output prefix, per-case writes | 2,634 | 13.710 |

GCC's LTO stack report shows a largest individual frame of 3,440 bytes in `parse_template`. The addon entry frame is 544 bytes and the Markdown frame is 848 bytes. The test with an 80,000-byte environment value also covered writable-range growth and a page-aligned input EOF. It matched the JS driver on all 4,894,554 answer bytes with no fallback.

| Final verification | Output |
| --- | --- |
| Original SHA-256 fuzz seeds | 315 byte-identical sites from 105 seeds, 0 JS fallbacks |
| New SHA-256 fuzz seeds | 300 byte-identical sites from 100 seeds, 0 JS fallbacks |
| `test.sh`, `test2.sh`, `test3.sh` | 9, 16, and 8 `OK` lines |
| `cases4` and `cases5` | 20 `OK` lines |
| All five fixture groups with THP disabled | 53 `OK` lines, 0 JS fallbacks |
| Forced JS fallback | Replaced a 12,000,000-byte file with the exact 7,446,432-byte practice answer |
| Additional synthetic inputs | 200 byte-identical sites across both encoding modes. An unsupported batch preserved the existing output until the fallback wrote the exact answer. |
| ELF | No undefined dynamic symbols, `DT_NEEDED` entries, or symbol versions. GNU property reports `x86-64-baseline`. |

The practice comparison used 41 interleaved runs per build and mode on CPU 7, reusing the output path. The `nothp` wrapper sets `PR_SET_THP_DISABLE` before exec. `/proc/self/status` reported `THP_enabled: 0` through the wrapper and `THP_enabled: 1` in the normal launch. Native columns use separate `SITE_PROFILE` builds with process minor-fault deltas from `getrusage(RUSAGE_SELF)` and thread CPU time. Whole-process columns use the uninstrumented binaries and `wait4` user plus system CPU.

| Entry | Mode | Native minor faults | Native CPU ms | Whole-process minor faults | Whole-process CPU ms |
| --- | --- | ---: | ---: | ---: | ---: |
| `e5f5707` | Normal | 11 | 12.038 | 6,720 | 50.567 |
| Final | Normal | 11 | 11.394 | 6,712 | 50.677 |
| Kit null | Normal | — | — | 7,014 | 45.254 |
| `e5f5707` | THP disabled | 3,688 | 15.027 | 11,985 | 54.705 |
| Final | THP disabled | 3,282 | 13.720 | 11,569 | 53.856 |
| Kit null | THP disabled | — | — | 8,589 | 45.892 |

With THP disabled, the final native run incurred 406 fewer minor faults and used 1.307 ms less CPU. Its total process CPU was 7.964 ms above the kit null median. With THP enabled, the native median improved by 0.644 ms and the whole-process medians were within 0.110 ms. The gVisor score remains unmeasured.

The wrapper source and binary, runner, baseline copy, profiled builds, and logs are in `/work/.tmp/t15`.

### Streaming and resident pages (t16)

- The addon renders into a reusable 64 KiB buffer. It opens `answers.json` on the first flush, holds the native writer lock through the last write, and truncates any stale tail. Symlinks and multiply linked files still use a sibling temporary file and rename. Include memoization copies up to 32 KiB of rendered text per case. Includes that cross a flush or exceed the cache budget render again on reuse.
- Input reading, encoding detection, and each site's JSON parse, settings checks, and template compilation precede that site's output. A failure can still occur after output starts, for example when a later site has unsupported settings. Native failure closes the descriptor, unlinks any staging file, and returns to `main.ts`. The JavaScript driver opens the final file with `'w'` and rewrites the batch.
- Page templates can read excerpts and reading times from the recent-post list, so those posts render their Markdown first. The other pages render Markdown immediately before their templates. The renderer keeps post excerpts for index and tag listings and reuses the rest of the HTML arena. Page records store their Markdown values directly, and hash table entries are 24 bytes each, down from 32.

The I/O trials used 19 interleaved practice runs per variant on CPU 7 through `nothp`. These variants used the streamed writer before the Markdown and hash entry changes. The private file mappings were writable so the parser could keep decoding strings in place.

| I/O trial | Native minor faults | Native CPU ms | Whole-process CPU ms, profiled |
| --- | ---: | ---: | ---: |
| HEAD with full answer buffer | 3,282 | 13.469 | 54.694 |
| 64 KiB stream and populated read | 1,457 | 11.759 | 51.631 |
| 256 KiB stream and populated read | 1,505 | 11.627 | 51.906 |
| 1 MiB stream and populated read | 1,697 | 11.786 | 51.535 |
| 64 KiB stream and lazy read | 1,457 | 11.917 | 51.968 |
| 64 KiB stream and lazy private file mapping | 1,487 | 12.023 | 51.510 |
| 64 KiB stream and populated private file mapping | 1,457 | 11.738 | 52.119 |

The 64 KiB stream touched 48 fewer pages than the 256 KiB stream for an additional 0.132 ms of native CPU in the trial. The populated read and populated private mapping tied on faults and were within 0.021 ms of native CPU. The populated read had the lower measured process CPU of those two and avoids a mapped-file fault if the input shrinks.

The following `mincore` counts are resident 4 KiB pages after a full run through `nothp`. The native fault count includes other regions such as code and the stack. The final addon's entire read-only data section is 24,924 bytes.

| Build | Input pages | Per-case scratch pages | Rendered Markdown pages | Answer buffer pages | Native minor faults |
| --- | ---: | ---: | ---: | ---: | ---: |
| HEAD | 720 | 406 | 334 | 1,818 | 3,282 |
| 64 KiB stream only | 720 | 382 | 334 | 16 | 1,457 |
| Final | 720 | 348 | 26 | 16 | 1,116 |

| Verification | Output |
| --- | --- |
| Original SHA-256 fuzz seeds 0–104 | 315 byte-identical sites, 0 JS fallbacks |
| New SHA-256 fuzz seeds 105–204 | 300 byte-identical sites, 0 JS fallbacks |
| Original seeds 0–34 through `nothp` | 105 byte-identical sites, 0 JS fallbacks |
| `test.sh`, `test2.sh`, and `test3.sh` | 9, 16, and 8 lines beginning with `OK` |
| `cases4` and `cases5` | 20 `OK` lines |
| Forced early fallback, normal and `nothp` | 0 native bytes written, existing 12,000,000-byte file untouched until JS wrote 7,391,492 exact bytes |
| Forced late fallback, normal and `nothp` | 1,441,792 exact native bytes observed before JS replaced the entire file with 7,362,887 exact bytes |
| Other output checks | Exact 7,446,432-byte native overwrite, staged replacement of symlink and hardlink, and cleanup on forced staged failure |
| Targeted `nothp` checks | Empty batch and four sites with oversized literals, nested includes, Markdown, statics, and varying recent counts. All exact, 0 JS fallbacks. |
| ELF | No undefined symbols, `DT_NEEDED` entries, or symbol versions. GNU property reports `x86-64-baseline`. |

The practice comparison used 41 interleaved runs per entry, build, and mode on CPU 7, reusing the output path. The native columns use `SITE_PROFILE` process fault deltas and native thread CPU. The whole-process columns use `wait4` with production binaries. The normal launch reported `THP_enabled: 1` and the wrapper reported `THP_enabled: 0`. Normal-mode fault counts varied between local runs even with THP enabled.

| Entry | Mode | Native minor faults | Native CPU ms | Whole-process minor faults | Whole-process CPU ms |
| --- | --- | ---: | ---: | ---: | ---: |
| HEAD `3336fef` | Normal | 3,282 | 13.424 | 10,530 | 53.005 |
| Final | Normal | 1,116 | 11.289 | 8,352 | 49.836 |
| Kit null | Normal | — | — | 7,522 | 45.962 |
| HEAD `3336fef` | THP disabled | 3,282 | 13.587 | 11,583 | 53.919 |
| Final | THP disabled | 1,116 | 11.599 | 9,409 | 51.401 |
| Kit null | THP disabled | — | — | 8,593 | 45.445 |

With THP disabled, native minor faults fell by 2,166 (66%) and whole-process minor faults fell by 2,174. The process CPU median fell by 2.518 ms. The final process is 816 minor faults and 5.956 ms above the kit null median in this mode. Timings are local.

The runner, trial builds, fixture and fuzz logs, fallback probes, and raw measurements are in `/work/.tmp/t16`. The final benchmark log is `final-41.log`.

### Weighted native fault pass (t18)

- The encoding prepass reads `cases.json` backwards in blocks of up to 256 KiB, with five bytes of overlap for escapes at block boundaries. The parser reads forward in 128 KiB refills and reuses the first block from the prepass in Latin-1 mode. In UTF-16 mode each refill validates its raw UTF-8 with up to three bytes of lookahead. The parser keeps the current case because decoded strings borrow its input bytes. After rendering, it compacts the unread suffix into the same window. `cases.json` contains all site inputs.
- The scratch arena rewinds between cases. Quotes, continued list items, and trimmed paragraphs reuse one Markdown text buffer. Link cache keys that borrow that buffer are copied before reuse. Short resolved paths use stack storage. Page, tag, and heading records expose their contiguous value arrays directly and hash tables grow at 75% occupancy.
- One demand-paged mapping supplies the arenas. The answer buffer grew to 256 KiB. The direct writer takes the lock before checking the regular file's type, link count, and length, with a blocking retry for contended regular files. The symlink and hardlink paths still stage through a sibling file.
- The generated Unicode slug tables use packed entries, and their lookup runs only for non-ASCII characters. Inline literals removed the dynamic relocations. The build omits unwind tables and disables GCC's interprocedural constant-propagation cloning. Markdown's AVX2 scan checks CPUID and XCR0 before use. It processes complete 32-byte vectors and shares the word-count semantics of the SSE2 path, which also handles the tail.

Each comparison below used 41 interleaved practice runs through `/work/.tmp/t15/nothp`. The paired delta is the median of the trial minus reference weighted costs within each round. Absolute timings varied between trial groups.

| Trial | Reference faults | Trial faults | Reference weighted ms | Trial weighted ms | Paired delta ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| 128 KiB refills and 256 KiB encoding scan vs HEAD | 1,116 | 727 | 15.158 | 14.242 | −0.958 |
| Compact page records, Markdown reuse, short paths, and denser maps | 727 | 613 | 16.992 | 16.624 | −0.450 |
| Packed Unicode, inline literals, and smaller unwind metadata | 613 | 612 | 17.475 | 17.361 | −0.100 |
| One mapping, one output status check, and 256 KiB output | 612 | 660 | 16.897 | 16.090 | −0.495 |
| Contiguous value arrays for page, tag, and heading records | 659 | 644 | 16.694 | 16.414 | −0.355 |
| Disable interprocedural constant-propagation cloning | 644 | 643 | 16.121 | 16.131 | −0.078 |
| Optional AVX2 Markdown scan | 644 | 644 | 15.520 | 15.263 | −0.197 |

Resident pages are 4 KiB pages measured with `mincore` after the practice run through `nothp`. HEAD's input and scratch share a mapping boundary page, counted with input.

| Build | Input | Case scratch | Markdown text | Markdown HTML | Answer | Environment and paths | Total |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| HEAD | 720 | 348 | 0 | 26 | 16 | 1 | 1,111 |
| Input trial | 328 | 349 | 0 | 26 | 16 | 1 | 720 |
| Scratch trial | 328 | 235 | 1 | 26 | 16 | 1 | 607 |
| Final | 328 | 219 | 1 | 26 | 64 | 1 | 639 |

| Production ELF section | HEAD bytes | Final bytes |
| --- | ---: | ---: |
| `.text` | 96,375 | 90,773 |
| `.rodata` | 24,924 | 16,048 |
| `.data` | 0 | 0 |
| `.data.rel.ro` | 2,352 | 0 |
| `.bss` | 4,360 | 264 |
| `.rela.dyn` | 576 | 0 |
| `.eh_frame` and `.eh_frame_hdr` | 4,588 | 0 |
| Complete file | 136,368 | 115,592 |

A separate 41-run interleaved `/proc/self/pagemap` probe immediately before addon registration measured a median of 10 present file-backed ELF pages at HEAD and 6 in the final build, in both modes. The read-only load segment went from eight mapped pages to four, and executable pages went from 24 to 23. `readelf` found zero relocations, undefined dynamic symbols, dependencies, and symbol versions in the final build. The GNU property records AVX2-class ISA use without declaring it required.

The sandbox lacks `strace` and rejects `ptrace` with `EPERM`. The syscall counts come from counting at each raw syscall instruction site in separate builds. They exclude profiling and reporting calls and agree in both launch modes. The output path is reused for the practice benchmark.

| Native syscall | HEAD | Final |
| --- | ---: | ---: |
| `read` | 4 | 2 |
| `pread64` | 0 | 35 |
| `write` | 114 | 29 |
| `open` | 3 | 3 |
| `close` | 3 | 3 |
| `fstat` | 3 | 2 |
| `mmap` | 2 | 1 |
| `mprotect` | 4 | 0 |
| `flock` | 1 | 1 |
| `exit_group` | 1 | 1 |
| **Total** | **135** | **77** |

The final practice benchmark used 41 interleaved runs per entry, build, and mode on CPU 7. Native CPU and faults came from `SITE_PROFILE`. Process metrics came from `wait4` on production binaries. Weighted cost uses the CPU of the row's mode plus 0.003 ms per `nothp` native minor fault and 0.005 ms per native syscall. These are local measurements.

| Entry | Mode | Native minor faults | Native CPU ms | Native syscalls | Weighted ms | Process minor faults | Process CPU ms |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| HEAD `b50ee87` | Normal | 24 | 12.354 | 135 | 16.377 | 6,738 | 56.208 |
| Final | Normal | 7 | 12.411 | 77 | 14.725 | 6,713 | 57.444 |
| Kit null | Normal | 0 | 0 | 0 | 0 | 7,024 | 53.164 |
| HEAD `b50ee87` | `nothp` | 1,116 | 12.628 | 135 | 16.651 | 9,417 | 59.432 |
| Final | `nothp` | 643 | 12.233 | 77 | 14.547 | 8,935 | 58.334 |
| Kit null | `nothp` | 0 | 0 | 0 | 0 | 8,594 | 54.439 |

Under `nothp`, weighted cost fell 2.104 ms (12.6%), native minor faults fell by 473, and process CPU fell 1.098 ms. Normal-mode process CPU rose 1.236 ms.

| Verification | Output |
| --- | --- |
| SHA-256 seeds 0–104 | 315 byte-identical sites each with automatic dispatch, forced SSE2, and forced AVX2. Zero JS fallbacks. |
| SHA-256 seeds 105–204 | 300 byte-identical sites each with automatic dispatch, forced SSE2, and forced AVX2. Zero JS fallbacks. |
| SHA-256 seeds 0–34 through `nothp` | 105 byte-identical sites, zero JS fallbacks |
| `test.sh`, `test2.sh`, `test3.sh` | 9, 16, and 8 `OK` lines |
| `cases4` and `cases5` | 20 `OK` lines |
| Forced early fallback in both modes | 0 native bytes written. JS replaced the existing file with 7,391,492 exact bytes. |
| Forced late fallback in both modes | 1,310,720 exact native bytes. JS rewrote the complete 7,362,887-byte answer. |
| Output publication | Exact 7,446,432-byte overwrite and truncate, staging for symlinks and hardlinks, staging cleanup on failure, and successful retry behind a held writer lock |
| Input boundaries under `nothp` | 120 refill and encoding-overlap checks, oversized whitespace, large UTF-16 cases, and an empty batch matched JS with zero fallbacks. |
| SIMD boundaries under `nothp` | 352 Markdown files with Unicode spaces across vector boundaries, byte-identical in automatic, forced SSE2, and forced AVX2 modes |

The measurements and verification outputs are in `/work/.tmp/t18/logs`. `final-41.json` contains the individual benchmark runs.

### Native CPU pass (t19)

- Markdown queues line ends in batches during the existing SIMD word count. Block dispatch uses a byte table, and paragraphs assemble trimmed text only when they need to. Inline rendering copies short plain emphasis spans directly from a matched vector. Code escaping uses a SIMD scan and a table of output bytes. Link parsing checks the closing delimiter and whitespace in one scan. Resolved URLs can reuse their template escape classification.
- JSON stores parsed slices as pairs of 32-bit offsets and lengths. Its AVX2 path scans for Unicode escapes in 32-byte blocks and can remove one simple string escape with a shuffle of a full 16-byte vector. The SSE2 path can copy whole vectors as soon as the write cursor has moved 16 bytes behind the read cursor. Refills and the scalar escape decoder still handle truncated vectors, Unicode escapes, and surrogate pairs.
- Templates precompute literal runs containing immutable site settings and select them when the scope cannot shadow `site`. Root-dependent includes can reuse a fragment within the same render while both copies fit in the current output buffer. The per-case include cache increased from 32 to 64 KiB. JSON nodes, template operations, and template values use narrower layouts. The case scratch arena touched 202 pages in the `nothp` probe, down from 219 at HEAD.
- The copy routine uses overlapping vector loads for short spans and `rep movsb` for larger ones. The scratch trial that shared cached SIMD masks between Markdown block and inline scans was slower and was discarded.

#### Fine profile

Separate instrumented builds collected 21 interleaved runs per build and mode on CPU 7. The probes used `lfence; rdtsc` at sub-step boundaries, exclusive timing for nested probes, and an empty-probe calibration. The numbers below are median millions of TSC ticks under `nothp` after subtracting the per-interval calibration. These probes add overhead and record elapsed ticks, so the CPU comparison uses the separate coarse profiles and thread CPU clock.

| HEAD rank | Sub-step | HEAD TSC ticks (M) | Final TSC ticks (M) |
| ---: | --- | ---: | ---: |
| 1 | Markdown inline scan, dispatch, and copies | 7.528 | 6.698 |
| 2 | JSON string escape decoding | 4.202 | 3.735 |
| 3 | Input reads | 3.788 | 3.882 |
| 4 | Output writes | 2.938 | 2.908 |
| 5 | Template value writes | 2.664 | 1.780 |
| 6 | Markdown link resolution | 2.547 | 2.324 |
| 7 | Markdown line scans and word counts | 2.413 | 2.195 |
| 8 | Remaining site preparation and serialization | 1.684 | 1.662 |
| 9 | Remaining Markdown block output and metadata | 1.149 | 1.062 |
| 10 | Template path lookup | 1.057 | 0.750 |

Template dispatch ranked ninth in the final profile. In the coarse `nothp` profile, Markdown fell from 5.185 to 4.505 ms, templates from 1.984 to 1.733 ms, and JSON parse from 1.361 to 1.205 ms. In normal mode those phases went from 5.205 to 4.455 ms, 1.935 to 1.571 ms, and 1.367 to 1.222 ms, respectively.

#### Practice comparison

Each entry and build ran 41 times per mode, interleaved on CPU 7 with a reused output path. Native CPU and minor faults use `SITE_PROFILE` thread CPU and process fault deltas. Whole-process metrics use `wait4` on production builds. Weighted cost uses each row's native CPU, 0.003 ms per `nothp` native minor fault, and 0.005 ms per native syscall. Separate syscall-counting builds reported 77 calls in both modes for HEAD and final.

| Entry | Mode | Native minor faults | Native CPU ms | Native syscalls | Weighted ms | Process minor faults | Process CPU ms |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| HEAD `e39170c` | Normal | 643 | 12.078 | 77 | 14.392 | 8,893 | 57.639 |
| Final | Normal | 626 | 10.559 | 77 | 12.825 | 8,865 | 56.347 |
| Kit null | Normal | 0 | 0 | 0 | 0 | 8,055 | 52.409 |
| HEAD `e39170c` | `nothp` | 643 | 12.124 | 77 | 14.438 | 8,951 | 57.652 |
| Final | `nothp` | 627 | 10.894 | 77 | 13.160 | 8,925 | 55.697 |
| Kit null | `nothp` | 0 | 0 | 0 | 0 | 8,600 | 52.868 |

Under `nothp`, weighted cost fell 1.278 ms (8.9%) and native CPU fell 1.230 ms (10.1%). The median paired weighted change under `nothp` was −1.399 ms. `/proc/self/status` reported `THP_enabled: 1` in normal mode and `THP_enabled: 0` through the wrapper.

#### Verification

| Check | Output |
| --- | --- |
| SHA-256 seeds 0–104 | 315 byte-identical sites each in auto, forced SSE2, and forced AVX2. Zero JS fallbacks. |
| SHA-256 seeds 105–204 | 300 byte-identical sites each in auto, forced SSE2, and forced AVX2. Zero JS fallbacks. |
| SHA-256 seeds 0–34 through `nothp` | 105 byte-identical sites, zero JS fallbacks |
| `test.sh`, `test2.sh`, and `test3.sh` | 9, 16, and 8 `OK` lines |
| `cases4` and `cases5` | 20 `OK` lines using `MAKEFUN_OUTPUT_DIR=/work/.tmp/t19/out` |
| Forced early fallback, normal and `nothp` | 0 native bytes written. JS rewrote 7,391,492 exact bytes. |
| Forced late fallback, normal and `nothp` | 1,310,720 exact native prefix bytes. JS rewrote all 7,362,887 answer bytes. |
| Output publication | Exact 7,446,432-byte overwrite and truncate, staged replacements for symlinks and hardlinks, staging cleanup on failure, and successful retry behind a held writer lock |
| Input and SIMD boundaries | 120 refill and encoding-overlap checks, oversized whitespace and UTF-16 inputs, an empty batch, 1,056 escape-boundary sites, and 352 Markdown files with Unicode spaces across vectors in each SIMD mode. Zero JS fallbacks. |
| Template and Markdown edges | Dense line batches, link whitespace, shadowed site settings, and repeated includes across output flushes matched JS in both encoding modes with all three dispatch modes. |
| ELF | Only `napi_register_module_v1` exported. Zero undefined dynamic symbols, relocations, dependencies, or symbol versions. AVX2 appears in ISA usage and is selected at runtime. |

The logs and individual measurements are under `/work/.tmp/t19/logs`. See `final-41.json` for the benchmark runs and `fine-final-21.json` for the fine profile. The worktree addon rebuilt byte for byte to the tested staging binary.

Changed files are `native/core.c`, `native/core.h`, `native/markdown.c`, `native/site.c`, `native/template.c`, `native/text.c`, `site.node`, and `NOTES.md`.

# makefun-site drop-in

The driver normally carries UTF-8 bytes in Latin-1 strings from input to output. An escaped non-ASCII code unit selects UTF-8 mode for the whole input file. The byte mode recognizes the UTF-8 sequences for JavaScript whitespace and line separators. Slug normalization and comparisons that may differ in UTF-16 order decode the relevant strings.

Markdown HTML-escapes each body up front, then scans block boundaries and inline marks. Paragraphs reuse contiguous body slices. Heading slugs and link resolution use the original text recovered from entities. Each rendered body, excerpt, and TOC label passes through native JSON serialization once. The renderer decodes fragments lazily for escaped template output.

Reading time still matches groups of up to 200 words. In byte mode, native string searches detect Unicode whitespace before choosing the word pattern.

On practice, the seven-run median CPU time went from 279 ms to 267 ms. The practice set and all 32 additional generated sets matched the reference.

Templates compile against numeric scope slots. Compilation folds constant expressions and subtrees, specializes includes by their referenced bindings and depth, and caches eligible loop output by element identity. Dynamic render steps alternate with prejoined literal strings. Each build tracks written paths for static-file shadowing. The driver writes each answer as it finishes. All case data and render caches belong to one build.

The latest pass matches common front-matter layouts in one native call and uses the general parser for detailed errors. It trims slug edges by index, bypasses relative resolution for absolute links, and skips the word scan for bodies of at most 400 code units. Each build caches escaped template strings of at most 128 code units and concatenates the answer for the driver to flatten on write.

The nine-run practice median was 269 ms, down from 279 ms for v3. All 43 sets passed and their answer files matched v3 byte for byte. That includes eight new seeded sets and targeted cases in both encoding modes. The paragraph scan, guarded inline replacements, and GC settings tested in this pass were dropped after timing them.

The driver releases the input text after parsing and drops each case from the input array as its build starts. Output is flushed at file boundaries after accumulating 32 KiB, so written strings can be collected during the build. Reading time counts word-group matches without retaining the matched strings. V8 uses a semi-space growth factor of 1 and disables parallel scavenging and background minor GC tasks.

Against v5base, nine-run practice medians went from 20,337 to 11,907 minor faults and from 169 to 167 ms total CPU. Median peak RSS fell from 114,212 to 80,352 KiB. All 53 sets passed and matched v5base byte for byte.

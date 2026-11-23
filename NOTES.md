# makefun-site drop-in

The driver normally carries UTF-8 bytes in Latin-1 strings from input to output. An escaped non-ASCII code unit selects UTF-8 mode for the whole input file. The byte mode recognizes the UTF-8 sequences for JavaScript whitespace and line separators. Slug normalization and comparisons that may differ in UTF-16 order decode the relevant strings.

Markdown HTML-escapes each body up front, then scans block boundaries and inline marks. Paragraphs reuse contiguous body slices. Heading slugs and link resolution use the original text recovered from entities. Each rendered body, excerpt, and TOC label passes through native JSON serialization once. The renderer decodes fragments lazily for escaped template output.

Reading time still matches groups of up to 200 words. In byte mode, native string searches detect Unicode whitespace before choosing the word pattern.

On practice, the seven-run median CPU time went from 279 ms to 267 ms. The practice set and all 32 additional generated sets matched the reference.

Templates compile against numeric scope slots. Compilation folds constant expressions and subtrees, specializes includes by their referenced bindings and depth, and caches eligible loop output by element identity. Dynamic render steps alternate with prejoined literal strings. Each build joins its JSON fragments once and tracks written paths for static-file shadowing. The driver writes each answer as it finishes. All case data and render caches belong to one build.

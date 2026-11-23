# makefun-site drop-in

The driver normally carries UTF-8 bytes in Latin-1 strings from input to output. An escaped non-ASCII code unit selects UTF-8 mode for the whole input file. The byte mode recognizes the UTF-8 sequences for JavaScript whitespace and line separators. Slug normalization and comparisons that may differ in UTF-16 order decode the relevant strings.

Markdown classifies lines as it renders blocks. A native regular expression matches complete inline constructs and emits HTML with JSON escaping already applied. The renderer skips the full matcher for plain labels and emphasis text. It decodes rendered fragments lazily when a template asks for HTML escaping. Reading time matches groups of up to 200 words using the appropriate whitespace rules.

Templates compile against numeric scope slots. Compilation folds constant expressions and subtrees, specializes includes by their referenced bindings and depth, and caches eligible loop output by element identity. Dynamic render steps alternate with prejoined literal strings. Each build joins its JSON fragments once and tracks written paths for static-file shadowing. The driver writes each answer as it finishes. All case data and render caches belong to one build.

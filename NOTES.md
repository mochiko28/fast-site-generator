# makefun-site drop-in

The driver normally carries UTF-8 bytes in Latin-1 strings from input to output. An escaped non-ASCII code unit selects UTF-8 mode for the whole input file. The byte mode recognizes the UTF-8 sequences for JavaScript whitespace and line separators. Slug normalization and comparisons that may differ in UTF-16 order decode the relevant strings.

Markdown classifies lines as it renders blocks. Its inline scanner skips literal spans and emits HTML with the JSON escaping already applied. The renderer decodes these fragments lazily when a template asks for HTML escaping. Reading time matches groups of up to 200 words using the appropriate whitespace rules.

Templates compile against numeric scope slots. Compilation folds constant expressions and subtrees, specializes includes by their referenced bindings and depth, and caches eligible loop output by element identity. Each build emits JSON members directly and tracks written paths for static-file shadowing. All case data and render caches belong to one build.

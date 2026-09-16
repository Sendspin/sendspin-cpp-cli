# Agent guidelines

## Comments

Less is more. A comment earns its place only when the code cannot say it, and one line wins.

- **Doc comments are for the caller.** One line of plain `///` saying what a thing is or does.
  No `@file` or `@brief`. Omit the comment when the name already says it. Use `@param` /
  `@return` only for what the signature does not: units, ownership, a sentinel's meaning.
- **Inline comments explain non-obvious mechanics** of a complex block, in one line. Never
  restate the code.
- **No rationale essays.** No history ("before X existed…"), no narration of other files or
  the spec, no "deliberately not X but Y" paragraphs.
- **Traps get one line.** Where a reader would otherwise break something — an ordering
  constraint, a locking rule, a silent clamp upstream, a shell gotcha — say so in a single line.
- **Trailing `///<` comments** on members and enumerators are fine when short; drop them when
  they repeat the name.
- The same rules apply to scripts, workflows, `CMakeLists.txt` and packaging files.
- Leave license headers and tool directives (`NOLINT`, `clang-format off/on`, `shellcheck`)
  untouched.

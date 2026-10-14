# Handoff: typed-plan (PROTO-3, wire_weaver SW-44)

Done:
- wire_weaver branch s/14oct2026-typed-plan (/tmp/wt/typed-plan/wire_weaver): SW-44 fixed-layout fast path in
  `#[derive_shrink_wrap]` (detected, or required by `fixed_layout`), `shrink_wrap::fixed_layout` (trait, C prelude,
  `write_c_header`), tests, docs, size-bench cases `header_*` / `fixed_*`. 623 tests and `just lint` green.
- chunky branch: `chunky_format` (format 2 types, `signed!`, golden vectors, generated C reader + `just test-c`),
  `plan_size` example, `just size-bench`, FEATURES PROTO-3, CHANGELOG.

Decisions: id PROTO-3 (tpm claim; the task said PROTO-2). Signed<T> is a macro (`signed!`), stable Rust cannot
size `[u8; T::ENCODED_LEN]` in a generic struct. Hashes 8 bytes (MAN-2). ww_dfu unchanged: it carries octets.

Next (not started): switch chunky to format 2 (apply engine, planner, ChunkHash 8, descriptor v2, Trust as
SealCheck, sweeps); land wire_weaver SW-44 first (chunky_format path-depends on ../wire_weaver).

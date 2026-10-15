# HANDOFF: chunky-format2 (P2625#15oct-0919)

Plan and threat model: `~/git/tpm/hobby/P2625-chunky-ota/design/format2-switch.md`.

## Done

- b321216: chunky on format 2 (descriptor v2 + Trust as SealCheck, verify before decode, apply on Step::view,
  body length, step framing, 8-byte hash, MAN-2 manifest), server/swap/bank/sim/cli/bench adapted, format 1
  removed, SECURITY.md, FEATURES, CHANGELOG. `just test` and `just lint` green.

## Running

- 500-seed sweeps (`CHUNKY_SWEEP_SEEDS=500`, sweep + bank), log `/tmp/cf2-sweep500.log`.
- Children off b321216 (`--from-here`, omarchy-m1): fmt2-fuzz (SIM-3, sonnet, #15oct-0923), fmt2-vectors (golden
  vectors end to end, sonnet, #15oct-0923b), fmt2-spec (SPEC.md, opus, #15oct-0923c, local only). They land on
  their own after this branch.

## Next

- Security pass written into the ready note; `git rm HANDOFF.md`; `tpm work ready`.
- Filed, not spawned: SRV-5 collision-fallback (#15oct-0923d), also keys the blob store by full SHA-256.

## Decisions and gotchas

- Unsigned image on a signed device is `BadSignature` now (seal first); `WrongScheme` = sealed body naming another
  scheme.
- `finish` compares the descriptor body (not the seal bytes) with the one announced.
- chunky_format must not dev-depend on chunky (two copies of its types): cross-crate tests live in chunky_sim.
- `--from-here` spawns only work on this PC.

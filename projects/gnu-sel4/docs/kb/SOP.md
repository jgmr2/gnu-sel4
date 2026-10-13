# Known errors: the catalogue and what to do with an error

`errors.jsonl`: one JSON object per known error (append; never rewrite history), with
`id, date, symptom, cause, component, layer, arch, severity, detected_by, fix, lesson, tags`.
- layer: abi | protocol | semantics | sync | driver | config | tooling | tests | security
- severity: critical (data loss, crash, security) | high (a feature broken) | medium (wrong but
  usable) | low (cosmetic, a workaround exists) | none (only the tests or tools)
- A port to another architecture: read every `layer` abi/protocol entry first (`jq 'select(.layer=="abi")'`).

## When something fails
1. Search the catalogue for the symptom (`grep -i <words> docs/kb/errors.jsonl`).
2. Reproduce once with logs on (TRACE_COMM=<prog> TRACE_CONSOLE=1, ?debug, tools/test/*): never
   run again to guess; add the log that is missing instead.
3. Fix with the smallest change; check with `tools/test/smoke-arm.sh` (one JSON summary line).
4. Add the entry (open errors too, with `fix` empty and the tag "open").

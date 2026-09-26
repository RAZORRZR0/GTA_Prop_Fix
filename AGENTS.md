# Agent rules

## Testing

- Never write unit tests after you write code.
- Highly prefer E2E tests as the sole testing mechanism. Use them to verify complex features work.
- At the end of an E2E test, produce a verifiable and repeatable artifact (e.g. a saved log or output file that a second run reproduces).
- If you must test a system in isolation, first write down all the ways it could fail, then write the code.

### In this repo

- `test\run_test.bat <path\to\SanAndreas.exe>` builds and runs `test/offline_test.cpp`.
- `TestBinary` is the E2E baseline: it maps the real `SanAndreas.exe`, resolves every signature and installs every hook.
- The synthetic `Test*` functions exist only for plugin decision logic that `TestBinary` cannot reach (prop outcome, unit/axis conversion, timers, config flags, thresholds, use-after-free guards).
- Do not add checks that pin log wording, call order or counts with no player-visible effect, mock echoes, fake setup, or default values.

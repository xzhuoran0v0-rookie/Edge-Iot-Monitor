# Status page with charts; LLM narration merged; pressure path removed
Date: 2026-07-25 23:47 | Branch: feat/status-page | Prev: none

## Context & Goal
Step 2–3 of the plan in `~/.claude/plans/fair-enough-what-s-next-sharded-hollerith.md`:
take the LLM off the alarm path (narration only), then make the system visible.
Driver is a competition demo where the hardware alone is too thin to show.

## What Was Done
- Merged PR #3 (combined firmware), #4 (narration), #5 (pressure removal) into `main`.
  Closed PR #2; branch `llm_reasoning` deleted, tip preserved as tag `archive/llm_reasoning`.
- `a63e6f7` removed the pressure path end to end — hardware is SHT30 only, so
  `pressure_delta` could never fire. Pressure-only payloads now 400.
- `7527e35` added `GET /api/status` + `GET /` (page compiled into the binary).
- `c8166d9` added `GET /api/history` + hand-written SVG charts; page text switched to Chinese.
- `scripts/test_backend_api.py` extended: status/history 400/401, one row per sensor,
  time-ascending points, `minutes` fallback and clamp, page renders, no external assets.
  Both suites pass. Assertion sensitivity verified by deliberately breaking expectations.
- All three commits pushed; PR #6 open and unreviewed.

## References
- `backend/src/StatusPage.h` — the whole page, inlined as a string constant
- `backend/src/DataIngestor.cpp` — `handleStatus`, `handleHistory`
- `backend/src/StorageEngine.cpp` — `getLatestPerSensor`, `getReadingsSince`, `getRecentAnalyses`
- `backend/src/AIQueryDispatcher.cpp` — `evaluateTrigger`, `buildPrompt`

## Key Decisions
- Deterministic code decides, LLM only explains — Reason: a model on the alarm path makes every failure a missed alarm — Proposed by: joint
- Charts added but big numbers kept — Reason: number = "what is it now", chart = "what happened"; the chart also makes the model's rate claims checkable — Proposed by: user (charts), agent (keep both)
- Latest-per-sensor is its own query, selected by `MAX(id)` not `MAX(timestamp)` — Reason: one sensor can push another out of a recent-N window; both metrics in one report share a second-resolution timestamp — Proposed by: agent
- English everywhere except rendered page text — Reason: application paper is English; judges read Chinese — Proposed by: user
- `verdict` stays ASCII — Reason: it feeds the OLED, which has no Chinese font — Proposed by: joint
- Firmware should use two timers: 2 s local, 10 s cloud — Reason: IQR needs 10 samples per 60 s window (10 s reporting gives 6, so it never runs); cloud quota is 10k msg/day, floor of 8.64 s — Proposed by: joint. NOT IMPLEMENTED — firmware unchanged.
- One codebase with a language switch, never a forked repo — Reason: two copies diverge and the demo build rots — Proposed by: agent

## Pitfalls
- IQR never fires at the current 10 s firmware interval — window is 60 s / 10 samples. Unfixed; needs the firmware change above.
- Chart drew a straight line across data gaps, i.e. invented readings during an outage — fixed, trace now breaks at gaps.
- Value lost its timestamp when the numbers row became chart headers; header "实时" only means the backend answered — fixed, age shown per value.
- `summarize()` takes rate from window oldest/newest, so a gap in the window poisons the rate the model reasons about — known, unfixed; harmless at 2 s steady reporting.
- `qwen2.5:3b` called 41.7 °C "normal" and a 14 °C ramp routine. Demo path is DeepSeek, which is untested — `config/config.yaml` holds a placeholder key.

## Open Questions
- Demo choreography (what is on the table, what provokes the change, how long) — still unknown, blocks page restyling only.
- Whether a Chinese prompt improves Qwen's Chinese output — untested, deliberately skipped.

## Next Steps
1. Review and merge PR #6. (user)
2. Flash the two-timer firmware split: 2 s local, 10 s cloud. (user)
3. Supply a real DeepSeek key and re-check narration quality. (user)
4. Capture a real IoTDA envelope, then build `POST /api/iotda/push` (Step 4). (joint)
5. Rotate the WiFi password — still outstanding from the earlier leak. (user)

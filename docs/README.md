# corvus documentation

Start here. Pick the reading path that matches why you're here.

## Reading paths

**New to the project** (about an hour)
1. [ARCHITECTURE.md](ARCHITECTURE.md): what corvus is, how the parts fit, and why it's shaped this way.
2. [CODE_TOUR.md](CODE_TOUR.md): every file and function, in reading order. Keep it open beside the code.
3. [HISTORY.md](HISTORY.md): how the code got here, era by era, with commits.

**Coming back after a break**
1. [HISTORY.md](HISTORY.md), from the last era you remember.
2. [CODE_TOUR.md Part 6](CODE_TOUR.md#part-6--the-loop) (the loop) and
   [Part 7](CODE_TOUR.md#part-7--end-to-end-one-run-call-by-call) (one run, end to end).
3. [CODE_TOUR Appendix A](CODE_TOUR.md#appendix-a--sharp-edges-index): the current known gaps.

**About to add a feature**
1. [CODE_TOUR.md Part 8](CODE_TOUR.md#part-8--scaffolding-and-workflows): build, tests, recipes, PR checklist.
2. The spec for the area you're touching (table below).
3. [CONTRIBUTING.md](../CONTRIBUTING.md).

## Every doc

| Doc | Purpose | Status | Last updated |
|---|---|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | Big picture, patterns, invariants, decisions | Current | 2026-09-29 |
| [CODE_TOUR.md](CODE_TOUR.md) | File-by-file, function-by-function walkthrough | Current | 2026-09-29 |
| [HISTORY.md](HISTORY.md) | Eras, decisions, and commits, step by step | Current | 2026-09-29 |
| [specs/2026-06-29-jarvis-cpp-design.md](specs/2026-06-29-jarvis-cpp-design.md) | Original strategy, architecture, roadmap | Current (decision record) | 2026-07-14 |
| [specs/2026-07-04-memory-design.md](specs/2026-07-04-memory-design.md) | Memory policies, overflow backstop | Current (decision record) | 2026-07-14 |
| [specs/2026-07-06-phase0-hardening-design.md](specs/2026-07-06-phase0-hardening-design.md) | Phase 0 API hardening | Implemented | 2026-07-06 |
| [specs/2026-07-15-cloud-clients-design.md](specs/2026-07-15-cloud-clients-design.md) | Phase 1 transport, clients, errors, retries | Current (PR 1 of 5 done) | 2026-07-15 |
| [plans/2026-07-15-cloud-clients-plan.md](plans/2026-07-15-cloud-clients-plan.md) | Phase 1 cloud clients, split into 5 PRs | Current | 2026-07-15 |
| [specs/2026-09-29-onboarding-docs-design.md](specs/2026-09-29-onboarding-docs-design.md) | Design of this documentation set | Implemented | 2026-09-29 |
| [history/phase-0-explained.md](history/phase-0-explained.md) | Plain-language Phase 0 walkthrough | Historical snapshot | 2026-07-14 |
| [history/pr1-http-transport-explained.md](history/pr1-http-transport-explained.md) | Plain-language PR #1 walkthrough | Historical snapshot | 2026-07-15 |

**Specs vs guides.** Specs and plans are the *decision record*: what was decided, and why, at the
time. They aren't rewritten when the code moves on. The three guides above describe the code *as it
is now*, and are kept up to date with every PR (see the PR checklist in CODE_TOUR Part 8).

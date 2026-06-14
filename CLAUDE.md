# Socket Codec — Project Rules

## Workflow (strict order)
1. **Spec first**: Before writing any code, read the relevant section from `.claude/plan.md` and `.claude/todo.md`
2. **Tests first**: Write tests defining expected input/output before implementing
3. **Implement**: Write the code to pass the tests
4. **Compile**: Run `scripts/build.sh` (or `make`) — must succeed with no errors
5. **Test**: Run tests — must pass
6. **Commit**: Git commit with descriptive message, ≤200 lines changed per commit

## Coding Rules
- Experimental project — get basic function working first, don't handle corner cases
- Match existing code style: C++23, Google-ish naming (`snake_case` for files/vars, `PascalCase` for classes)
- Keep changes small and focused — one logical change per commit
- x264 codec only for MVP
- Platform: macOS + Linux (no Windows)

## Build
- Main project: `make` from project root
- x264: built from `third_party/x264/` source (not a pre-built lib)
- One-tap: `scripts/build.sh` builds everything
- Local run: `scripts/run_local.sh` (no mahimahi needed on macOS)

## Architecture References
- Plan: `.claude/plan.md`
- Tasks: `.claude/todo.md`
- After each phase: write `phaseN_summary.md` in project root

## Git
- Branch: `develop` for active work
- Commit messages: imperative mood, concise (e.g., "Add ClockThread with frame-tick support")
- Never commit generated/binary files (build/, *.o, *.a)

## Key Interfaces
- `Encoder` (codec/encoder.h) — base class for all encoders
- `MessageHandler` (transmission/message_handler.h) — base for packet handlers
- `CongestionController` — base for CC algorithms (to be created)
- `NetworkSender` wraps socket send + optional simulator (to be created)
- `ClockThread` — single timing source for all threads (to be created)

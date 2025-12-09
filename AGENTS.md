# Repository Guidelines

## Project Structure & Module Organization
- `src/`: runnable radio modes (quick_mode, fast_mode, p2p_mode, p2p_simple_mode, p3p_mode, robust_mode, robust_mode_reset) that link the shared layers.
- `libs/`: reusable layers (`nrf24` driver, `logger`, `utils`, app/presentation/transport/link stacks) consumed by modes and tests.
- `tests/`: standalone C harnesses (`test_logger.c`, `test_link_layer.c`, etc.) plus generated `.exe` artifacts; mirror library functionality.
- `scripts/`: deployment/autostart helpers for field devices; execute from repo root.
- `tx_files/` and `logs/`: payloads and generated telemetry; `bin/` is created by `make`.

## Build, Test, and Development Commands
- `make help` lists supported targets.
- `make quick|fast|p2p|p2p_simple|p3p|robust|robust_reset` builds the selected mode into `bin/`.
- `make all` builds every mode; `make clean` removes objects/binaries.
- Example run after build: `./bin/p2p_mode` (expects nRF24 hardware attached and matching channel/bitrate).
- For orchestration workflows, run `python orchestrator.py` from the repo root (activate `.venv` if used).

## Coding Style & Naming Conventions
- C code is built with `-Wall -Wextra -O2`; keep changes warning-free and C99-compatible.
- Use 4-space indentation, K&R braces, snake_case for functions/variables, and uppercase for macros/const globals.
- Prefer `static` helpers for file-local scope; keep headers minimal, guarded, and free of unnecessary includes.

## Testing Guidelines
- Tests are small executables under `tests/`; rebuild with `gcc -Wall -Wextra -O2 -Ilibs -I. tests/test_logger.c libs/logger.c libs/utils.c -o tests/test_logger.exe` as a template.
- Run via `./tests/test_logger.exe`, `./tests/test_link_layer.exe`, etc.; capture console output for PR notes.
- Name new harnesses `test_<area>.c` and avoid committing generated `.exe` or `.log` files (ignored in `.gitignore`).

## Commit & Pull Request Guidelines
- Commit messages are short, present-tense summaries (e.g., `log folder`, `working on handshake`); keep scope focused per commit.
- PRs should describe purpose, list commands/tests executed, and call out hardware context (channels, data rates, wiring changes).
- Link issues when available and attach screenshots/log excerpts for behavioral shifts or user-visible output.

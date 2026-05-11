# Memory Balloon Driver - Comprehensive Project Documentation

## 1. Project Overview

This project implements a virtio-style memory balloon workflow with:
- a guest kernel balloon module,
- a host-side controller daemon using real QMP,
- a shared-memory command/acknowledgement contract,
- proof-driven validation logs.

Primary objective:
- dynamically adjust guest memory between lower and higher targets (inflate/deflate),
- while showing pressure-aware behavior under constrained free memory.

## 2. Final Outcome of the Project

What works now:
- Host can command memory target changes via QMP.
- Guest balloon driver reclaims and returns pages.
- Shared-memory protocol fields and sequence semantics are implemented and validated.
- Replay-safe publish behavior is implemented.
- ivshmem transport proof exists (host<->guest marker exchange).
- Pressure-triggered deflate behavior is observable in guest logs.

What is intentionally not finished:
- kernel-direct shared-memory control path is not fully merged yet;
- host-side adaptive policy logic is still minimal and will be expanded in future work.

## 3. Architecture and Design

## 3.1 High-level architecture

1. `balloond` publishes target command and sequence in shared memory.
2. `balloond` sends QMP `balloon` command to QEMU.
3. Guest balloon driver receives target via virtio balloon config.
4. Guest inflates/deflates pages to match target (subject to pressure logic).
5. `balloond` polls QMP `query-balloon` and logs actual value.
6. Shared-memory acknowledgement fields are used to represent command lifecycle.

## 3.2 Design principles used

- Keep the control loop simple and observable.
- Separate transport/protocol concerns from balloon mechanics.
- Prefer deterministic smoke validation with explicit proof files.
- Explicitly document incomplete advanced areas as future work.

## 3.3 Shared-memory protocol design

Contract reference:
- [docs/PROTOCOL.md](docs/PROTOCOL.md)

Fields:
- `magic`, `version`, `flags`
- `target_bytes`, `actual_bytes`
- `cmd_seq`, `ack_seq`
- `status`, `last_error`

Ownership discipline:
- Host writes: `target_bytes`, `cmd_seq`
- Guest side writes: `actual_bytes`, `ack_seq`, `status`, `last_error`

Why this matters:
- Prevents race/ownership ambiguity.
- Makes replay/no-op and restart behavior predictable.

## 4. Source Code Walkthrough

## 4.1 Host daemon (`host/balloond`)

### 4.1.1 `src/main.c`
Role:
- main runtime loop for publishing targets, invoking QMP, and telemetry logging.

Key responsibilities:
- Resolve QMP socket path via:
  - `BALLOOND_QMP_SOCK` env var, else
  - `$HOME/virtio-balloon/logs/qmp.sock`, else
  - `./logs/qmp.sock`.
- Guard protocol sanity:
  - reject `ack_seq > cmd_seq`.
- Replay/no-op guard:
  - skip publish when target unchanged and no pending command.
- Interactive mode:
  - accepts runtime target input from terminal.
- Non-interactive mode:
  - suitable for scripts (`timeout ... < /dev/null`).

Important runtime log fields:
- `actual`, `target`, `cmd_seq`, `ack_seq`, `shm_status`, `shm_err`, `qmp_err`.

### 4.1.2 `src/shm.c`
Role:
- shared-memory mapping and initialization.

Behavior:
- Uses file-backed shared memory default:
  - `/dev/shm/balloon_ivshmem.bin`
- Supports override via:
  - `BALLOOND_SHM_FILE`.
- Initializes protocol header (`magic`, `version`) if missing/invalid.

### 4.1.3 `src/qmp.c`
Role:
- low-level QMP socket client.

Implemented operations:
- QMP handshake + capabilities.
- `qmp_set_target_bytes()` -> QMP `balloon`.
- `qmp_query_actual_bytes()` -> QMP `query-balloon`, parse `actual`.

Design choice:
- open a short-lived QMP connection for each operation;
- keeps code simple and predictable for scripted runs.

### 4.1.4 `src/log.c`
Role:
- logging helper output path.

Note:
- logs are emitted through standard error; this is why proofs require `2>&1 | tee`.

## 4.2 Guest kernel module (`guest/vballoon_lab/vballoon_lab.c`)

Role:
- actual balloon mechanics and pressure behavior.

### Core structures
- `vb_page`: one balloon-tracked page + PFN.
- `vb_lab`: device context, queues, lists, counters, worker state.

### Core flows
- Inflate path:
  - allocate page via `alloc_page(...)`,
  - enqueue PFN to inflate virtqueue,
  - on completion move to ballooned list,
  - increment `actual_pages`.
- Deflate path:
  - pop page from ballooned list,
  - return page via `__free_page(...)`,
  - decrement `actual_pages`.

### Worker logic
- Continuously compares effective pages vs target pages.
- Applies one action per cycle:
  - inflate if below target,
  - deflate if above target,
  - wait when no progress.

### Pressure logic
- Parameters:
  - `pressure_enable`
  - `pressure_min_free_mb`
  - `pressure_log_interval_ms`
- If free memory below threshold and pressure is enabled:
  - prioritizes deflate to return memory to guest.

Interpretation:
- Under strong pressure settings, exact target convergence can be intentionally delayed/overridden for stability.

## 4.3 Guest shared-memory bridge (`guest/shm_agent/main.c`)

Role:
- userspace bridge that maps ivshmem BAR and updates shared-memory ack/actual fields.

Current behavior:
- auto-detects ivshmem PCI function by vendor/device (`1af4:1110`) and maps its `resource2`,
- validates protocol header,
- on new command sequence:
  - sets `actual_bytes = target_bytes`,
  - updates `ack_seq`,
  - clears status/error.

Important note:
- This is currently userspace bridge logic.
- Kernel-direct shared-memory integration remains future work.

## 4.4 Scripts and automation flow (`scripts/`)

- `run_qemu_phase2.sh`:
  - starts QEMU VM with virtio balloon and QMP socket.
- `run_qemu_phase3_ivshmem.sh`:
  - starts QEMU with ivshmem device for transport experiments.
- `smoke_phase2.sh`:
  - rebuilds daemon,
  - runs inflate test to 2 GiB,
  - runs deflate test to 3 GiB,
  - writes proof logs.
- `start_balloond.sh`:
  - helper to launch daemon quickly.

## 5. Configuration Files Documentation

## 5.1 Cloud-init and image config
- [images/user-data](images/user-data):
  - creates `ubuntu` user,
  - enables SSH password auth,
  - sets initial password `ubuntu`.
- [images/meta-data](images/meta-data):
  - sets instance-id and hostname.

## 5.2 Build configs
- Host build:
  - [host/balloond/Makefile](host/balloond/Makefile)
- Guest module build:
  - [guest/vballoon_lab/Makefile](guest/vballoon_lab/Makefile)
- Guest agent build:
  - [guest/shm_agent/Makefile](guest/shm_agent/Makefile)

## 5.3 Runtime parameters and environment variables

Host daemon:
- `BALLOOND_QMP_SOCK` (optional QMP socket override)
- `BALLOOND_SHM_FILE` (optional shared-memory file override)

Guest module parameters:
- `pressure_enable`
- `pressure_min_free_mb`
- `pressure_log_interval_ms`

## 6. End-user Documentation (How to Run)

Primary runbook:
- [docs/QEMU_TEST_GUIDE.md](docs/QEMU_TEST_GUIDE.md)

Quick sequence:
1. Start VM with `./scripts/run_qemu_phase3_ivshmem.sh`.
2. Build/load guest module and bind driver.
3. Build/start guest `shm_agent` and verify process/log output.
4. Run shared-memory contract logs and verify `ack_seq` progression.
5. Disable pressure temporarily for deterministic smoke run.
6. Run `./scripts/smoke_phase2.sh 2>&1 | tee proofs/full_smoke_run.log`.
7. Re-enable pressure and capture pressure evidence logs.

## 7. Output and Proof Interpretation

## 7.1 Inflate/deflate convergence
- [proofs/inflate_convergence.log](proofs/inflate_convergence.log)
- [proofs/deflate_convergence.log](proofs/deflate_convergence.log)

Expected:
- inflate reaches `2147483648` bytes,
- deflate reaches `3221225472` bytes.

## 7.2 Full smoke markers
- [proofs/full_smoke_run.log](proofs/full_smoke_run.log)

Expected markers:
- `BEGIN INFLATE LOG`
- `BEGIN DEFLATE LOG`
- completion line.

## 7.3 Replay-safe behavior
- [proofs/replay_guard_noop.log](proofs/replay_guard_noop.log)

Expected signal:
- unchanged target with no pending command -> publish skipped.

## 7.4 Pressure behavior
- [proofs/pressure_deflate_dmesg.log](proofs/pressure_deflate_dmesg.log)

Expected signal:
- repeated `pressure deflate` lines.

## 7.5 ivshmem transport demonstration
- [proofs/ivshmem_transport_proof.log](proofs/ivshmem_transport_proof.log)

Expected:
- host->guest marker match,
- guest->host marker match.

## 8. Limitations and Future Work

1. Shared-memory control path is still userspace-bridged (`shm_agent`) for guest-side contract updates.
2. Kernel-direct shared-memory integration is pending.
3. Additional recovery hardening is pending.
4. Add a dedicated host policy module for adaptive targeting, bounds, and replay-safe decisioning.

## 9. Related Documents

- [README.md](README.md)
- [docs/STATUS.md](docs/STATUS.md)
- [docs/PROTOCOL.md](docs/PROTOCOL.md)
- [docs/QEMU_TEST_GUIDE.md](docs/QEMU_TEST_GUIDE.md)



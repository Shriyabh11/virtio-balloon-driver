# Shared Memory Protocol (Phase 2.5 Contract)

## Version
- `version = 1`

## Layout (packed)
- `u32 magic` = `0x42414C4E` (`BALN`)
- `u16 version`
- `u16 flags`
- `u64 target_bytes`
- `u64 actual_bytes`
- `u64 cmd_seq`
- `u64 ack_seq`
- `u32 status`
- `u32 last_error`

## Field Ownership (Strict)
- Host daemon owns writes to:
  - `target_bytes`
  - `cmd_seq`
- Guest side owns writes to:
  - `actual_bytes`
  - `ack_seq`
  - `status`
  - `last_error`
- Both sides may read all fields.
- `magic` and `version` are initialized at setup and validated by both sides.

## Command Lifecycle
1. Host publishes a new command by:
   - writing `target_bytes`
   - incrementing `cmd_seq` by exactly +1
2. Guest detects `cmd_seq > ack_seq`, processes latest command, updates:
   - `actual_bytes`
   - `status` / `last_error`
   - `ack_seq = cmd_seq` only after processing that command
3. Host considers command complete when `ack_seq == cmd_seq`.

## Replay and Restart Rules
- Duplicate command:
  - If `cmd_seq == ack_seq`, no pending work.
- Host restart:
  - Must not decrement or reset `cmd_seq`.
  - Should continue from current shared-memory `cmd_seq`.
- Guest restart:
  - Must re-validate `magic/version`.
  - If `cmd_seq > ack_seq`, process pending command and catch up.
- Out-of-order writes are invalid:
  - Host must never update `ack_seq`.
  - Guest must never update `cmd_seq`.

## Status/Error Semantics
- `status = 0` => command processed successfully.
- `status != 0` => processing or transport failure.
- `last_error` carries platform error code (`errno`-style when applicable).

## Current Scope Note
This protocol is active in the current host daemon path and documented as the authoritative contract for Phase B cleanup.

In the current `shm_agent` MVP implementation, `actual_bytes` is used as bridge-reported shared-memory state and is set equal to `target_bytes` when a command is acknowledged. The authoritative live balloon size is still obtained through QMP `query-balloon`.


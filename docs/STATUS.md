# Project Status & Validation Log

## Objective
Add host-daemon + shared-memory control path on top of a working virtio balloon driver baseline.

## Current State (v1.0)
The core infrastructure for the virtio memory balloon driver and the host-side control daemon is fully implemented and validated.

### Implemented Features
1. **Shared-Memory Protocol v1**
   - Fields: `magic`, `version`, `target_bytes`, `actual_bytes`, `cmd_seq`, `ack_seq`, `status`, `last_error`
2. **Host Daemon (`host/balloond`)**
   - Creates and maps shared memory
   - Publishes target updates with sequence numbers
   - Supports both interactive (terminal) and non-interactive (scripting) modes
3. **Guest Bridge Agent (`guest/shm_agent`)**
   - Maps shared memory via ivshmem PCI BAR
   - Detects new command sequences and acknowledges them
4. **Automation Scripts**
   - Full QEMU launch and end-to-end smoke testing suite

## Validation Results

All functional behavior has been proven with end-to-end testing. Proof logs are available in the `proofs/` directory.

### 1. Basic Inflate / Deflate Convergence
- **Scenario:** The host requests a memory target decrease (inflate) followed by an increase (deflate).
- **Result:** PASS. The guest successfully reclaims and returns pages to match the requested `target_bytes`.
- **References:** `inflate_convergence.log`, `deflate_convergence.log`

### 2. Command Idempotency & Replay Guard
- **Scenario:** The host attempts to publish an unchanged target.
- **Result:** PASS. The daemon correctly identifies the unchanged state and suppresses redundant shared-memory writes, keeping `cmd_seq` stable.
- **Reference:** `replay_guard_noop.log`

### 3. Transport Reliability (ivshmem)
- **Scenario:** Validate bidirectional communication over the PCI BAR-mapped shared memory region.
- **Result:** PASS. Explicit marker strings written by the host are read by the guest, and vice versa.
- **Reference:** `ivshmem_transport_proof.log`

### 4. Pressure-Driven Deflation
- **Scenario:** The guest OS experiences memory pressure (free memory drops below threshold).
- **Result:** PASS. The balloon driver intercepts the pressure signal and autonomously deflates to protect VM stability, overriding the host target if necessary.
- **Reference:** `pressure_deflate_dmesg.log`, `pressure_deflate_run.log`

## Known Limitations & Next Steps
- The shared-memory bridge is currently a userspace agent (`shm_agent`). Kernel-direct shared-memory integration remains future work.
- Host-side adaptive targeting policy (beyond manual inputs) will be added in future revisions.

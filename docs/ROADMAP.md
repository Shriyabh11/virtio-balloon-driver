# Feature Roadmap

This document outlines the planned technical evolution for the `virtio-balloon` project. The focus is on moving from a functional prototype to a robust, kernel-integrated control plane suitable for production-like environments.

## Phase 1: Kernel Integration of Shared Memory (Next Major Release)
Currently, the `ivshmem` contract is maintained by a userspace bridge (`shm_agent`). The immediate priority is to integrate this into the kernel driver.
- [ ] Map the `ivshmem` PCI BAR directly within `vballoon_lab.ko`.
- [ ] Implement a kernel-level worker thread to poll or interrupt-drive `cmd_seq` changes.
- [ ] Retire the userspace `shm_agent` entirely to reduce context-switching overhead and secure the control plane.

## Phase 2: Host Policy Engine
The host daemon currently accepts manual target inputs. We will introduce an adaptive policy engine to automate ballooning decisions.
- [ ] **Telemetry Ingestion:** Parse host-side memory statistics (e.g., cgroups, PSI) to determine global memory pressure.
- [ ] **Adaptive Bounds:** Implement configurable `min_target` and `max_target` limits per VM.
- [ ] **Rate Limiting:** Prevent thrashing by implementing hysteresis and rate-limiting on target adjustments.

## Phase 3: Reliability & Recovery Hardening
Ensure the system gracefully handles component failures and restarts.
- [ ] **Daemon Restart Recovery:** Allow `balloond` to crash and restart without losing synchronization with the guest (`cmd_seq` / `ack_seq` reconciliation).
- [ ] **Stale Memory Detection:** Implement a heartbeat or timestamp mechanism in the shared memory struct to detect if the guest agent/driver has crashed.

## Phase 4: Extended Observability
Improve operational visibility for debugging and monitoring.
- [ ] Export driver statistics via `sysfs` or `debugfs` (e.g., total pages inflated, deflation events triggered by pressure).
- [ ] Integrate host daemon logs with standard Linux logging facilities (syslog/journald) via structured JSON outputs.

## Phase 5: vhost-user Integration
Explore migrating the custom `ivshmem` transport to a standard `vhost-user` backend.
- [ ] Evaluate the performance and complexity tradeoffs of implementing a `vhost-user-balloon` device in the host daemon.
- [ ] Refactor the shared-memory protocol to conform to `vhost-user` message specifications if adopted.

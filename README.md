# virtio-balloon — Dynamic VM Memory Ballooning with Shared-Memory Control

A Linux kernel module that dynamically inflates and deflates virtual machine memory via the virtio balloon interface, coordinated by a host-side daemon through a shared-memory command protocol over ivshmem.

## Architecture

```
┌──────────────────────────────────────────────────────┐
│                     HOST                             │
│                                                      │
│  ┌──────────────┐    QMP (unix socket)   ┌────────┐  │
│  │  balloond     │──────────────────────▶│  QEMU  │  │
│  │  (host daemon)│                       │        │  │
│  └──────┬───────┘                        └───┬────┘  │
│         │                                    │       │
│         │  shared memory                     │virtio │
│         │  (/dev/shm or ivshmem)             │config │
│         │                                    │       │
│  ┌──────▼───────┐                        ┌───▼────┐  │
│  │  cmd_seq ++   │                       │balloon │  │
│  │  target_bytes │                       │device  │  │
│  └──────┬───────┘                        └───┬────┘  │
│         │                                    │       │
├─────────┼────────────────────────────────────┼───────┤
│         │           GUEST VM                 │       │
│         │                                    │       │
│  ┌──────▼───────┐                        ┌───▼────┐  │
│  │  shm_agent    │                       │vballoon│  │
│  │  (userspace   │                       │_lab.ko │  │
│  │   bridge)     │                       │(kernel)│  │
│  │  ack_seq =    │                       │        │  │
│  │  cmd_seq      │                       │inflate │  │
│  └──────────────┘                        │deflate │  │
│                                          │pressure│  │
│                                          └────────┘  │
└──────────────────────────────────────────────────────┘
```

**Data flow:**
1. Host daemon publishes `target_bytes` + increments `cmd_seq` in shared memory
2. Host daemon sends QMP `balloon` command to QEMU
3. QEMU propagates target to guest via virtio balloon config
4. Guest kernel module adjusts balloon size using `alloc_page()` / `__free_page()`
5. Guest userspace agent acknowledges command via `ack_seq` in shared memory
6. Under memory pressure, guest driver autonomously deflates to protect VM stability

## Features

- ✅ **Kernel-level balloon driver** — real virtio driver using `alloc_page()` / `__free_page()` with virtqueue I/O
- ✅ **Host daemon with QMP control** — sends `balloon` and `query-balloon` commands over UNIX socket
- ✅ **Shared-memory protocol** — structured command/ack contract with sequence numbers and ownership discipline
- ✅ **ivshmem transport** — bidirectional host↔guest communication over PCI BAR-mapped shared memory
- ✅ **Memory pressure detection** — guest-side free-memory threshold triggers autonomous deflation
- ✅ **Replay-safe publishing** — duplicate command suppression when target is unchanged
- ✅ **Auto-detection** — guest agent auto-discovers ivshmem PCI device by vendor/device ID
- ✅ **GPU workload simulator** — warp execution model with branch divergence and memory coalescing analysis
- ✅ **CPU pressure generator** — configurable memory allocation tasks that trigger balloon responses

## Tech Stack

| Layer | Technology |
|-------|-----------|
| Guest kernel module | C, Linux kernel API, virtio, virtqueues |
| Host daemon | C (POSIX), QMP JSON protocol, `mmap` shared memory |
| Shared memory transport | QEMU ivshmem-plain, PCI BAR mapping |
| VM hypervisor | QEMU with TCG/KVM acceleration |
| Guest OS | Ubuntu 24.04 Server (cloud image) |
| Build system | Make, kbuild (kernel module) |

## Project Structure

```
├── guest/
│   ├── vballoon_lab/          # Linux kernel balloon driver module
│   │   ├── vballoon_lab.c     # Driver: inflate/deflate, virtqueues, pressure logic
│   │   └── Makefile           # kbuild out-of-tree module build
│   └── shm_agent/             # Userspace shared-memory bridge
│       ├── main.c             # ivshmem PCI auto-detect, BAR mmap, ack loop
│       ├── include/protocol.h # Shared protocol struct definition
│       └── Makefile
│   └── workload_sim/          # CPU + GPU workload simulator
│       ├── workload_sim.c     # Warp divergence, coalescing, memory pressure
│       └── Makefile
├── host/
│   └── balloond/              # Host-side balloon control daemon
│       ├── src/
│       │   ├── main.c         # Daemon loop, target publishing, telemetry
│       │   ├── shm.c          # Shared memory setup and mapping
│       │   ├── qmp.c          # QMP socket client (balloon + query-balloon)
│       │   └── log.c          # Timestamped structured logging
│       ├── include/
│       │   ├── protocol.h     # Protocol struct + constants (magic, version)
│       │   ├── qmp.h          # QMP client API
│       │   └── log.h          # Logging API
│       └── Makefile
├── scripts/
│   ├── run_qemu_phase2.sh     # Launch QEMU with virtio-balloon + QMP
│   ├── run_qemu_phase3_ivshmem.sh  # Launch QEMU with ivshmem device
│   ├── smoke_phase2.sh        # Automated inflate/deflate smoke test
│   └── start_balloond.sh      # Quick daemon launcher
├── docs/
│   ├── ARCHITECTURE.md        # System architecture and design decisions
│   ├── PROTOCOL.md            # Shared-memory protocol specification
│   ├── ROADMAP.md             # Future work and planned extensions
│   ├── STATUS.md              # Development log and validation results
│   └── QEMU_TEST_GUIDE.md     # Step-by-step VM setup and test guide
├── proofs/                    # Captured validation logs
├── tests/
│   └── e2e.md                 # End-to-end test results
├── DESIGN.md                  # Comprehensive technical documentation
├── CONTRIBUTING.md            # Contribution guidelines
└── LICENSE                    # GPL-2.0 (kernel module requirement)
```

## Quick Start

### Prerequisites

- Linux host (Ubuntu 24.04+ recommended)
- QEMU with TCG or KVM support
- Build tools: `gcc`, `make`, `linux-headers`

```bash
sudo apt install -y qemu-system-x86 qemu-utils cloud-image-utils \
  build-essential socat
```

### 1. Set Up VM Image

```bash
cd images/
wget -O ubuntu-24.04-server-cloudimg-amd64.img \
  https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img
qemu-img resize ubuntu-24.04-server-cloudimg-amd64.img +20G
cloud-localds seed.iso user-data meta-data
```

### 2. Start QEMU

```bash
./scripts/run_qemu_phase3_ivshmem.sh
```

### 3. Build and Load Kernel Module (in guest)

```bash
ssh -p 2222 ubuntu@127.0.0.1  # password: ubuntu
cd ~/virtio-balloon/guest/vballoon_lab
make -j$(nproc)
sudo insmod ./vballoon_lab.ko pressure_enable=1 pressure_min_free_mb=128
```

### 4. Run Smoke Test (on host)

```bash
./scripts/smoke_phase2.sh
```

Expected output includes `BEGIN INFLATE LOG`, `BEGIN DEFLATE LOG`, and `smoke_phase2: completed (real QMP path)`.

For detailed setup instructions, see [docs/QEMU_TEST_GUIDE.md](docs/QEMU_TEST_GUIDE.md).

## How It Works

### Balloon Inflate (Reclaim Guest Memory)

1. Host sets lower memory target via QMP
2. Kernel module calls `alloc_page(GFP_HIGHUSER | __GFP_NORETRY)` to capture guest pages
3. Page frame numbers are published to the inflate virtqueue
4. On completion callback, pages move to the ballooned list
5. Guest reports updated `actual` pages to QEMU

### Balloon Deflate (Return Memory to Guest)

1. Host sets higher memory target via QMP
2. Kernel module pops pages from the ballooned list
3. Pages are returned via `__free_page()` — memory becomes available to guest again
4. Guest reports updated `actual` pages

### Pressure-Driven Deflation

When guest free memory drops below a configurable threshold (`pressure_min_free_mb`), the driver **overrides the host target** and deflates to protect VM stability. This is a safety mechanism — the guest prioritizes its own health over the host's ballooning request.

```
# Runtime-tunable via sysfs
echo 1 | sudo tee /sys/module/vballoon_lab/parameters/pressure_enable
echo 512 | sudo tee /sys/module/vballoon_lab/parameters/pressure_min_free_mb
```

### Shared-Memory Protocol

The host and guest communicate through a structured shared-memory region with strict field ownership:

| Field | Owner | Purpose |
|-------|-------|---------|
| `target_bytes` | Host | Desired balloon size |
| `cmd_seq` | Host | Command sequence number |
| `actual_bytes` | Guest | Current balloon size |
| `ack_seq` | Guest | Last processed command |
| `status` | Guest | Processing result |
| `last_error` | Guest | Error code (errno-style) |

Command lifecycle: host increments `cmd_seq` → guest processes → guest sets `ack_seq = cmd_seq`. See [docs/PROTOCOL.md](docs/PROTOCOL.md) for the full specification.

## Validation

All validation logs are captured in `proofs/`:

| Test | Log File | What It Proves |
|------|----------|----------------|
| Inflate convergence (3→2 GiB) | `inflate_convergence.log` | `actual` reaches target, `ack_seq` catches up |
| Deflate convergence (2→3 GiB) | `deflate_convergence.log` | Reverse transition, full convergence |
| Full smoke (inflate + deflate) | `full_smoke_run.log` | End-to-end round trip in one run |
| Replay guard (no-op) | `replay_guard_noop.log` | Duplicate publish suppression works |
| ivshmem transport | `ivshmem_transport_proof.log` | Bidirectional host↔guest marker exchange |
| Pressure deflation | `pressure_deflate_dmesg.log` | `dmesg` shows pressure-triggered deflate |

## License

This project is licensed under the [GNU General Public License v2.0](LICENSE) — required for Linux kernel modules.

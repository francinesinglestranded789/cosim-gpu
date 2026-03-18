# Multi-GPU xGMI Interconnect & Super-Node Scale-Out Implementation Plan

## Goal Description

Extend cosim-gpu from single MI300X GPU co-simulation (QEMU + gem5) to multi-GPU hive simulation with xGMI interconnect modeling (Path A), and eventually to super-node scale-out via SST Merlin network simulation (Path B). The implementation follows a progressive architecture: single-process multi-GPU instances for initial phases (2–4 GPUs), transitioning to multi-process architecture for full 8-GPU hive scale. Only the vfio-user cosim backend is supported for multi-GPU; the legacy socket backend is not extended. Path A and Path B can proceed in parallel after the 2-GPU xGMI link model is validated.

### Key Architecture Decisions (Clarified)

1. **gem5 Process Model**: Progressive — single gem5 process with multiple GPU instances for Milestone 1–2 (up to ~4 GPUs); multi-process (one gem5 container per GPU with IPC) introduced at Milestone 3 for 8-GPU scale.
2. **Dependency Relaxation**: SST Merlin integration (Milestone 4) depends only on the 2-GPU xGMI baseline from Milestone 2, NOT on the 8-GPU hive (Milestone 3). Milestones 3 and 4 can proceed in parallel.
3. **Backend Scope**: Multi-GPU is implemented exclusively on the vfio-user backend. The legacy socket backend is not extended for multi-GPU.
4. **Bandwidth Targets**: 128 GB/s per link and ~310–330 GB/s aggregate are configurable model parameters calibrated to real hardware specs, NOT hard pass/fail acceptance criteria. Acceptance is based on correct data transfer and configurable timing, not measured throughput numbers.
5. **Hardware Characterization**: Real hardware benchmarking (TransferBench, RCCL) is recommended for model calibration but not a gating prerequisite for any milestone.
6. **SST PoC**: Milestone 4 includes a lightweight PoC sub-milestone before full integration.

## Acceptance Criteria

Following TDD philosophy, each criterion includes positive and negative tests for deterministic verification.

### Milestone 1: Multi-GPU Instance Infrastructure

- AC-1: gem5 configuration supports instantiating N independent MI300X GPU timing models in a single process
  - Positive Tests (expected to PASS):
    - `mi300_cosim.py --num-gpus 2` creates 2 AMDGPUDevice instances, each with its own CU array, L1/L2 caches, and VRAM
    - `mi300_cosim.py --num-gpus 4` creates 4 independent GPU instances without resource conflicts
    - Each GPU instance has a unique `gpuId` and separate PM4/SDMA engine mappings
  - Negative Tests (expected to FAIL):
    - `mi300_cosim.py --num-gpus 0` is rejected with a validation error
    - Two GPU instances sharing the same VRAM shared memory region (must be independent `/dev/shm/mi300x-vram-{0..N}`)
    - BAR address ranges overlapping between GPU instances

- AC-2: QEMU exposes N PCI GPU endpoints via vfio-user, each connected to a separate gem5 GPU instance
  - Positive Tests (expected to PASS):
    - QEMU launches with N `-device vfio-user-pci,socket=<path-N>` parameters, each connecting to the corresponding gem5 vfio-user server
    - Guest `lspci` shows N AMD GPU devices with correct vendor/device IDs (1002:74a0 or equivalent)
    - Each GPU's BAR layout (BAR0+1=VRAM, BAR2+3=Doorbell, BAR4=MSI-X, BAR5=MMIO) is independently mapped and non-overlapping
  - Negative Tests (expected to FAIL):
    - QEMU startup with a socket path that doesn't correspond to any gem5 GPU instance (connection refused)
    - Guest accessing BAR region of GPU 0 and seeing data from GPU 1 (isolation violation)

- AC-3: Per-GPU shared memory regions are independently allocated and accessible
  - Positive Tests (expected to PASS):
    - `/dev/shm/mi300x-vram-0`, `/dev/shm/mi300x-vram-1`, ... exist as separate files with correct sizes
    - Guest writes to GPU 0 VRAM do not appear in GPU 1 VRAM
    - Each GPU's VRAM is independently accessible from both QEMU and gem5
  - Negative Tests (expected to FAIL):
    - Writing to GPU 0 VRAM region corrupts GPU 1 VRAM content

- AC-4: Launch infrastructure supports multi-GPU configuration
  - Positive Tests (expected to PASS):
    - `cosim_launch.sh --num-gpus 2` starts gem5 with 2 GPU instances and QEMU with 2 vfio-user-pci devices
    - `cosim_launch.sh --num-gpus 1` (or default) behaves identically to current single-GPU setup (backward compatible)
  - Negative Tests (expected to FAIL):
    - `cosim_launch.sh --num-gpus 2` with insufficient shared memory allocation

- AC-5: Guest OS initializes all GPU instances via updated setup service
  - Positive Tests (expected to PASS):
    - `cosim-gpu-setup.service` iterates ROM DD + modprobe for each of N GPUs
    - `amdgpu` driver successfully probes all N GPU devices
    - Each GPU appears in `/sys/class/drm/` as separate render nodes
  - Negative Tests (expected to FAIL):
    - ROM loaded to only one GPU while N>1 GPUs are present (remaining GPUs fail to initialize)

- AC-6: Each GPU instance can independently execute a compute kernel
  - Positive Tests (expected to PASS):
    - A simple HIP vector-add kernel runs on GPU 0 and produces correct results
    - The same kernel runs on GPU 1 and produces correct results independently
    - Two kernels running on different GPUs concurrently do not interfere
  - Negative Tests (expected to FAIL):
    - Kernel dispatched to GPU 0 executes on GPU 1's CU array

### Milestone 2: xGMI Link Model (2-GPU Hive)

- AC-7: xGMI bridge component exists in gem5, attached to each GPU's L2 cache egress
  - Positive Tests (expected to PASS):
    - xGMI bridge SimObject is instantiated and connected to GPU's L2 cache controller
    - Bridge accepts memory requests targeting remote GPU address ranges
    - Bridge forwards packets with correct (src_gpu, dst_gpu, addr, size, payload) header
  - Negative Tests (expected to FAIL):
    - Memory request to local GPU VRAM being routed through xGMI bridge (local access should not traverse xGMI)
    - xGMI bridge forwarding packets with incorrect src_gpu/dst_gpu fields

- AC-8: xGMI link parameters are configurable at launch time
  - Positive Tests (expected to PASS):
    - `--xgmi-bandwidth 128GBps --xgmi-latency 100ns` configures the link model accordingly
    - `--xgmi-topology mesh` creates full-mesh connectivity between GPU instances
    - `--xgmi-topology ring` creates ring connectivity
    - Default parameters match MI300X hardware specs (128 GB/s, 16 lanes)
  - Negative Tests (expected to FAIL):
    - `--xgmi-bandwidth 0` is rejected (invalid parameter)
    - `--xgmi-topology star` is rejected (unsupported topology)

- AC-9: GPU-to-GPU VRAM data transfer works correctly through xGMI
  - Positive Tests (expected to PASS):
    - GPU 0 writes a known pattern to GPU 1 VRAM via xGMI path; GPU 1 reads back the correct data
    - Bidirectional transfer: GPU 0→GPU 1 and GPU 1→GPU 0 simultaneously without corruption
  - Negative Tests (expected to FAIL):
    - Transfer to non-existent GPU ID (should return error/timeout, not silent corruption)
    - Data arriving at wrong address on destination GPU

- AC-10: Flow control prevents data loss under congestion
  - Positive Tests (expected to PASS):
    - Credit-based back-pressure stalls sender when receiver buffer is full
    - After back-pressure release, queued packets are delivered in order
  - Negative Tests (expected to FAIL):
    - Packets dropped silently when link is congested (must stall, not drop)

### Milestone 3: Full 8-GPU Hive (Path A Complete)

- AC-11: 8-GPU full-mesh xGMI topology operates correctly with multi-process architecture
  - Positive Tests (expected to PASS):
    - 8 gem5 processes (one per GPU) connect via IPC, forming 28 bidirectional xGMI links
    - Any GPU can access any other GPU's VRAM through xGMI
    - Global clock synchronization keeps all GPU timing models consistent
  - Negative Tests (expected to FAIL):
    - GPU-to-GPU transfer across 2+ hops in mesh (mesh is fully connected, all transfers are single-hop)
    - Timing inconsistency: GPU 0 observes event at simulated time T1, GPU 1 observes same event at different simulated time

- AC-12: SDMA engine supports xGMI copy path for GPU-to-GPU DMA
  - Positive Tests (expected to PASS):
    - SDMA copy command with remote GPU destination correctly transfers data via xGMI
    - SDMA completion interrupt fires after xGMI transfer finishes
  - Negative Tests (expected to FAIL):
    - SDMA xGMI copy targeting local VRAM uses xGMI path instead of local path

- AC-13: Multi-GPU workloads execute correctly across the hive
  - Positive Tests (expected to PASS):
    - RCCL allreduce across 8 GPUs produces mathematically correct results
    - RCCL allgather collects data from all 8 GPUs correctly
  - Negative Tests (expected to FAIL):
    - Collective operation completes with incorrect data on any participating GPU

- AC-14: Documentation covers xGMI model design
  - Positive Tests (expected to PASS):
    - `docs/en/xgmi-model.md` and `docs/zh/xgmi-model.md` exist with mutual cross-links
    - Documentation covers packet format, topology configuration, and calibration parameters
  - Negative Tests (expected to FAIL):
    - Documentation exists in only one language (must have both `en/` and `zh/` versions)

### Milestone 4: SST Merlin Integration (Path B Foundation)

- AC-15: Lightweight PoC demonstrates gem5 GPU ↔ SST basic communication
  - Positive Tests (expected to PASS):
    - A single gem5 GPU instance sends a test message through an SST Merlin network endpoint and receives a response
    - SST simulation event loop and gem5 event scheduler co-execute without deadlock
  - Negative Tests (expected to FAIL):
    - gem5 and SST event loops diverge in simulated time by more than one synchronization quantum

- AC-16: SST wrapper component encapsulates gem5 GPU timing model as SST endpoint
  - Positive Tests (expected to PASS):
    - gem5 GPU model registers as an SST component with correct network interface
    - SST Merlin routes packets between two gem5 GPU endpoints
  - Negative Tests (expected to FAIL):
    - SST component initialization fails due to missing gem5 library dependencies

- AC-17: Three-layer synchronization protocol coordinates QEMU, gem5, and SST
  - Positive Tests (expected to PASS):
    - QEMU (real-time KVM) → gem5 (GPU timing) → SST (network timing) chain maintains causal ordering
    - Guest driver initiates GPU-to-GPU transfer; timing flows through all three layers correctly
  - Negative Tests (expected to FAIL):
    - SST network event processed before corresponding gem5 GPU event (causality violation)

- AC-18: 2-GPU SST Merlin path produces comparable results to Path A xGMI model
  - Positive Tests (expected to PASS):
    - Same GPU-to-GPU transfer test produces identical data results through SST Merlin vs. Path A xGMI
    - SST Merlin configured with equivalent parameters (128 GB/s, matching latency) shows similar timing behavior
  - Negative Tests (expected to FAIL):
    - Data corruption when routing through SST Merlin (functional correctness must match Path A)

### Milestone 5: Super-Node Scale-Out (Path B Complete)

- AC-19: Multi-node topology support via SST Merlin
  - Positive Tests (expected to PASS):
    - Fat-tree topology with 2 nodes (each containing 8 GPUs) routes inter-node traffic correctly
    - Dragonfly topology configuration accepted and functional
  - Negative Tests (expected to FAIL):
    - Inter-node packet routed within a single node (must traverse NIC model)

- AC-20: Hybrid intra-node xGMI + inter-node Ethernet topology operates correctly
  - Positive Tests (expected to PASS):
    - Intra-node GPU communication uses xGMI path (low latency)
    - Inter-node GPU communication uses NIC/Ethernet path (higher latency)
    - RCCL collective across nodes produces correct results
  - Negative Tests (expected to FAIL):
    - Inter-node traffic bypassing NIC model and using xGMI directly

## Path Boundaries

Path boundaries define the acceptable range of implementation quality and choices.

### Upper Bound (Maximum Acceptable Scope)

The implementation includes:
- Full 8-GPU hive with multi-process gem5 architecture, global time synchronization, and 28 bidirectional xGMI links with credit-based flow control
- Complete SST Merlin integration with three-layer co-simulation synchronization, supporting both fat-tree and dragonfly topologies
- Multi-node scale-out (up to 8 nodes) with hybrid xGMI + Ethernet interconnect and RCCL collective benchmarking
- Simplified Atlas switch model for cross-chassis xGMI extension
- Performance profiling and bottleneck analysis tooling
- Full bilingual documentation (en + zh)
- Integration with job scheduler / workload manager for multi-node runs

### Lower Bound (Minimum Acceptable Scope)

The implementation includes:
- 2-GPU single-process co-simulation with independent VRAM, correct PCI enumeration, and per-GPU kernel execution (Milestone 1)
- Basic xGMI link model with configurable bandwidth/latency between 2 GPUs, functional data transfer verified (Milestone 2)
- One of: 8-GPU hive (Milestone 3) OR SST Merlin 2-GPU PoC (Milestone 4 PoC sub-milestone)

### Allowed Choices

- **Can use**:
  - vfio-user protocol for QEMU ↔ gem5 communication (required)
  - POSIX shared memory for per-GPU VRAM regions
  - Unix domain sockets or shared memory ring buffers for xGMI transport (single-process)
  - IPC (Unix sockets, shared memory, or MPI) for multi-process GPU-to-GPU communication
  - gem5 Ruby cache protocol extensions for L2 egress bridge
  - gem5 SimpleNetwork or custom network model for xGMI links
  - SST Merlin for network topology simulation (Milestone 4+)
  - Docker containers for gem5 process isolation
- **Cannot use**:
  - Legacy cosim socket backend for multi-GPU (single-GPU legacy remains untouched)
  - gem5 GarnetNetwork for xGMI (designed for on-chip, not inter-chip)
  - Modifications to upstream QEMU source code for multi-device support (use QEMU CLI parameters only)
  - Hardcoded GPU count assumptions (must be parameterized via `--num-gpus`)

## Feasibility Hints and Suggestions

> **Note**: This section is for reference and understanding only. These are conceptual suggestions, not prescriptive requirements.

### Conceptual Approach

**Milestone 1 — Multi-GPU in single gem5 process:**

```
# Pseudocode for mi300_cosim.py extension
for gpu_id in range(args.num_gpus):
    gpu_device = AMDGPUDevice(gpu_id=gpu_id)
    shader = createGPU(system, args, gpu_id)    # CU/L1/L2 per GPU
    vram_shmem = f"/mi300x-vram-{gpu_id}"
    socket_path = f"{args.socket_base}-{gpu_id}.sock"

    cosim_bridge = MI300XVfioUser(
        gpu_device=gpu_device,
        socket_path=socket_path,
        shmem_path=vram_shmem,
        vram_size=args.dgpu_mem_size,
    )
    system.gpu_devices.append(gpu_device)
    system.cosim_bridges.append(cosim_bridge)
```

QEMU side: add N `-device vfio-user-pci,socket=/tmp/gem5-mi300x-{N}.sock` parameters in `cosim_launch.sh`.

**Milestone 2 — xGMI bridge at L2 egress:**

The xGMI bridge intercepts memory requests at L2 cache egress. When the target address falls in a remote GPU's VRAM range, the request is forwarded through the xGMI link model instead of local memory.

```
L2 Cache → [address check] → local VRAM (if local)
                            → xGMI Bridge → Transport → Remote GPU L2 → Remote VRAM (if remote)
```

**Milestone 3 — Multi-process transition:**

Each GPU runs in a separate gem5 Docker container. A synchronization daemon manages global virtual time across all gem5 processes. xGMI transport uses shared memory ring buffers or Unix sockets for inter-process packet delivery.

**Milestone 4 — SST PoC approach:**

Start with a minimal integration: wrap a single gem5 GPU as an SST `SubComponent`, connect to a trivial Merlin network (2 endpoints, single link). Validate that SST's event loop and gem5's event scheduler can co-execute without deadlock or time drift. Only after PoC success, proceed to full 2-GPU integration with three-layer synchronization.

### Relevant References

- `gem5/configs/example/gpufs/mi300_cosim.py` — Current single-GPU cosim configuration, the primary file to extend for multi-GPU
- `gem5/src/dev/amdgpu/mi300x_vfio_user.hh/.cc` — vfio-user cosim bridge implementation, one instance needed per GPU
- `gem5/src/dev/amdgpu/amdgpu_device.hh` — AMDGPUDevice with gpuId, PM4/SDMA mappings; instantiate N times
- `gem5/src/dev/amdgpu/MI300XVfioUser.py` — SimObject definition for vfio-user bridge
- `scripts/cosim_launch.sh` — Launch orchestration; add `--num-gpus` parameter and multi-socket logic
- `scripts/cosim_guest_setup.sh` — Guest GPU init; iterate over N GPUs for ROM DD + modprobe
- `gem5/src/mem/ruby/` — Ruby cache protocol; L2 egress is the xGMI bridge attachment point
- [MGPUSim](https://github.com/sarchlab/mgpusim) — Multi-GPU simulator reference for pluggable interconnect design
- [SST + Balar](https://github.com/sstsimulator/sst-gpgpusim) — GPU integration in SST framework
- [gem5 + SST](https://github.com/sstsimulator/sst-elements) — gem5 as SST component, synchronization patterns

## Dependencies and Sequence

### Milestones

1. **Milestone 1**: Multi-GPU Instance Infrastructure (single-process, no interconnect)
   - Section A: gem5 config extension — parameterize `mi300_cosim.py` for N GPU instances with separate VRAM/PM4/SDMA
   - Section B: QEMU + launch scripts — multi-socket vfio-user launch, per-GPU shared memory allocation
   - Section C: Guest initialization — update `cosim-gpu-setup.service` for N-GPU ROM DD + modprobe iteration
   - Section D: Validation — PCI enumeration, BAR isolation, independent kernel execution per GPU

2. **Milestone 2**: xGMI Link Model (2-GPU hive, single-process)
   - Section A: xGMI specification — packet format, addressing scheme, topology configuration interface
   - Section B: gem5 bridge implementation — L2 egress bridge, xGMI transport (in-process), flow control
   - Section C: Launch integration — `--xgmi-topology` and `--xgmi-bandwidth` parameters
   - Section D: Validation — bidirectional VRAM transfer, data correctness, configurable timing verification

3. **Milestone 3**: Full 8-GPU Hive (multi-process architecture, Path A complete)
   - Section A: Multi-process architecture — per-GPU gem5 container, IPC transport for xGMI, global time synchronization
   - Section B: SDMA xGMI copy path — GPU-to-GPU DMA through SDMA engine
   - Section C: Workload validation — RCCL collective communication (allreduce, allgather)
   - Section D: Optional extensions — simplified Atlas switch model, performance profiling tooling
   - Section E: Documentation — bilingual xGMI model design docs

4. **Milestone 4**: SST Merlin Integration (Path B foundation)
   - Section A: PoC — gem5 single-GPU as SST SubComponent, minimal Merlin network, event loop co-execution validation
   - Section B: Full integration — SST wrapper for gem5 GPU, three-layer synchronization protocol (QEMU ↔ gem5 ↔ SST)
   - Section C: Validation — 2-GPU SST Merlin path, comparison with Milestone 2 xGMI baseline
   - Section D: Benchmarking — co-sim overhead analysis vs. direct transport

5. **Milestone 5**: Super-Node Scale-Out (Path B complete)
   - Section A: Multi-node topology — fat-tree, dragonfly via SST Merlin
   - Section B: NIC model — Ultra Ethernet / RoCE for inter-node path
   - Section C: Hybrid topology — intra-node xGMI mesh + inter-node Ethernet
   - Section D: Validation — multi-node RCCL collectives, scalability testing (2/4/8-node)

### Dependency Graph (Updated)

```
Milestone 1 (Multi-GPU Instances)
    │
    v
Milestone 2 (xGMI 2-GPU Link Model)
    │
    ├──────────────────────────┐
    v                          v
Milestone 3 (8-GPU Hive)    Milestone 4 (SST Merlin)
    │                          │
    │   ┌──────────────────────┘
    v   v
Milestone 5 (Super-Node Scale-Out)
```

Key dependency changes from original draft:
- Milestone 4 depends on Milestone 2 only (relaxed from original Phase 3→4 dependency)
- Milestone 3 and Milestone 4 can proceed in parallel
- Milestone 5 depends on both Milestone 3 (8-GPU hive validated) and Milestone 4 (SST integration proven)

### Critical Technical Dependencies

- Milestone 1 Section A (gem5 multi-instance) must complete before Section B (QEMU multi-socket)
- Milestone 2 Section A (xGMI spec) must complete before Section B (bridge implementation)
- Milestone 3 Section A (multi-process architecture) is the highest-risk item — it changes the fundamental execution model
- Milestone 4 Section A (PoC) is a gate: if PoC fails, the full SST integration approach needs re-evaluation

## Implementation Notes

### Code Style Requirements
- Implementation code and comments must NOT contain plan-specific terminology such as "AC-", "Milestone", "Step", "Phase", or similar workflow markers
- These terms are for plan documentation only, not for the resulting codebase
- Use descriptive, domain-appropriate naming in code instead

### Architecture Notes
- The transition from single-process to multi-process (Milestone 3) is the most significant architectural change. The single-process model from Milestones 1–2 should be designed with this transition in mind — e.g., xGMI transport should use an abstract interface that can switch between in-process function calls and IPC
- PCI topology: QEMU Q35 chipset has limited root port count. For 8 GPUs, PCIe switches or additional root ports may be needed. Investigate Q35 capacity early in Milestone 1
- amdgpu driver multi-GPU: the cosim environment uses `ip_block_mask=0x67` which disables PSP and SMU. Verify that xGMI topology discovery works without these blocks, or implement a minimal xGMI discovery stub
- Guest RAM shared memory (`/dev/shm/cosim-guest-ram`) is shared across all GPUs; only VRAM is per-GPU


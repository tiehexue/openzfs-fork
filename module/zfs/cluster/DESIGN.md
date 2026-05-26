# Cluster ZFS Architecture Design

## Overview

This document describes the architecture for transforming OpenZFS from a single-node filesystem into a cluster-coordinated shared-disk filesystem. The design enables multiple nodes to concurrently read and write to the same zpool while maintaining ZFS's core guarantees of data integrity, consistency, and atomicity.

## Motivation

ZFS is a robust filesystem with strong data integrity guarantees, but it is fundamentally a single-node design. A zpool can only be imported on one node at a time (enforced by MMP). To make ZFS a cluster filesystem where multiple nodes can import the same zpool concurrently, we must solve four fundamental problems:

1. **Write Isolation**: Different nodes must not write to the same disk blocks simultaneously
2. **Metadata Coherence**: Only one node should update the MOS (Meta Object Set) and uberblock
3. **TXG Coordination**: Transaction groups must be globally coordinated
4. **Failure Handling**: Node failures must be detected and recovered safely

## Architecture Principles

- **Single-Writer Model**: Only the coordinator writes MOS and uberblock
- **Metaslab Partitioning**: Each node owns disjoint metaslab subsets — no write conflicts on data blocks
- **Centralized TXG**: The coordinator generates and broadcasts TXG state transitions
- **Paxos Membership**: Majority-based quorum prevents split-brain
- **Disk-Backed Fencing**: Failed nodes are fenced via persistent records on shared storage

## Module Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                     Cluster ZFS Architecture                    │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────────┐  ┌─────────────┐  ┌──────────────────────┐   │
│  │ cluster_spa  │  │ cluster_zil │  │  cluster_recovery    │   │
│  │ (main integ.)│  │ (per-node   │  │  (fencing + ZIL      │   │
│  │             │  │  ZIL regions)│  │   replay + def.free) │   │
│  └──────┬──────┘  └──────┬──────┘  └───────────┬──────────┘   │
│         │                │                      │               │
│  ┌──────┴────────────────┴──────────────────────┴──────────┐   │
│  │                    cluster_dlm                           │   │
│  │            (Distributed Lock Manager)                    │   │
│  │  Resources: DATASET, OBJECT, ZAP, SPACE, POOL_CONFIG    │   │
│  └──────┬────────────┬────────────────┬────────────────────┘   │
│         │            │                │                         │
│  ┌──────┴─────┐ ┌────┴─────────┐ ┌───┴──────────────┐        │
│  │cluster_    │ │cluster_      │ │cluster_          │        │
│  │membership  │ │txg           │ │metaslab          │        │
│  │(Paxos +    │ │(TXG state    │ │(partitioning +   │        │
│  │ heartbeat) │ │ machine)     │ │  rebalancing)    │        │
│  └────────────┘ └──────────────┘ └──────────────────┘        │
│         │            │                │                         │
│  ┌──────┴────────────┴────────────────┴──────────────────┐   │
│  │                   cluster_sync                         │   │
│  │          (MOS/Uberblock single-writer)                 │   │
│  └───────────────────────────────────────────────────────┘   │
│                                                                 │
├─────────────────────────────────────────────────────────────────┤
│                    Existing ZFS Layers                          │
│  spa.c │ metaslab.c │ txg.c │ dsl_pool.c │ vdev_label.c │ zil │
└─────────────────────────────────────────────────────────────────┘
```

## Source Files

| File | Purpose |
|------|---------|
| `include/sys/cluster/cluster_types.h` | Core type definitions, constants, enums |
| `include/sys/cluster/cluster_membership.h` | Membership API |
| `include/sys/cluster/cluster_txg.h` | TXG coordination API |
| `include/sys/cluster/cluster_metaslab.h` | Metaslab partitioning API |
| `include/sys/cluster/cluster_dlm.h` | Distributed Lock Manager API |
| `include/sys/cluster/cluster_sync.h` | MOS/uberblock coordination API |
| `include/sys/cluster/cluster_zil.h` | Cluster ZIL API |
| `include/sys/cluster/cluster_recovery.h` | Recovery and fencing API |
| `include/sys/cluster/cluster_spa.h` | Main integration API |
| `module/zfs/cluster/cluster_membership.c` | Paxos membership implementation |
| `module/zfs/cluster/cluster_txg.c` | Distributed TXG coordination |
| `module/zfs/cluster/cluster_metaslab.c` | Metaslab partitioning |
| `module/zfs/cluster/cluster_dlm.c` | DLM implementation |
| `module/zfs/cluster/cluster_sync.c` | MOS/uberblock single-writer |
| `module/zfs/cluster/cluster_zil.c` | Per-node ZIL regions |
| `module/zfs/cluster/cluster_recovery.c` | Fencing and recovery |
| `module/zfs/cluster/cluster_spa.c` | Main integration module |

---

## 1. Metaslab Partitioning (`cluster_metaslab`)

### Problem
In single-node ZFS, any thread can allocate from any metaslab. In cluster mode, if two nodes allocate from the same metaslab simultaneously, they will corrupt each other's space maps.

### Solution
Each metaslab is assigned to exactly one node. Only the owning node can allocate from (or free to) that metaslab. Reads are unrestricted — any node can read any block.

### Design

```
Vdev 0 (10 TB)
├── Metaslab 0  → Node 0  (owns 0-1GB range)
├── Metaslab 1  → Node 1  (owns 1-2GB range)
├── Metaslab 2  → Node 0  (owns 2-3GB range)
├── Metaslab 3  → Node 2  (owns 3-4GB range)
├── Metaslab 4  → Node 1  (owns 4-5GB range)
├── Metaslab 5  → Node 2  (owns 5-6GB range)
└── ...         (round-robin or weighted assignment)
```

**Key data structure** (`cluster_ms_assign_t`):
```c
typedef struct cluster_ms_assign {
    uint64_t cma_vdev_id;
    uint64_t cma_ms_count;
    cluster_node_id_t *cma_owner;  // per-metaslab owner
    cluster_ms_partition_policy_t cma_policy;
    kmutex_t cma_lock;
} cluster_ms_assign_t;
```

### Partitioning Strategies

| Strategy | Description | When to Use |
|----------|-------------|-------------|
| **STATIC** | Even split at cluster creation, never rebalances | Fixed cluster size |
| **DYNAMIC** | Rebalances on membership change | Nodes join/leave frequently |
| **ADAPTIVE** | I/O-driven rebalancing based on utilization | Variable workloads |

### Integration Point: `metaslab_alloc_filter()`

The key integration point is in the metaslab allocation path. In `metaslab_class_allocatable()`, after building the list of candidate metaslabs, we filter out metaslabs not owned by this node:

```c
// In metaslab allocation path (conceptual):
for each candidate metaslab:
    if cluster_enabled && !cluster_metaslab_owns(spa, ms):
        skip this metaslab
    // ... normal allocation logic
```

This is implemented via the `ms_disabled` mechanism: non-owned metaslabs are disabled for the local allocator, which is an existing ZFS mechanism for excluding metaslabs from allocation.

### MOS Persistence

Metaslab assignments are stored in a ZAP object per top-level vdev:
```
ZAP: "cluster_ms_vdev_0"
  "ms_0" → 0   (node 0 owns metaslab 0)
  "ms_1" → 1   (node 1 owns metaslab 1)
  "ms_2" → 0
  ...
```

---

## 2. MOS/Uberblock Coordination (`cluster_sync`)

### Problem
The MOS (Meta Object Set) is the root metadata tree of a ZFS pool. It contains:
- Dataset directory (DSL)
- Space maps for all metaslabs
- Pool configuration
- All structural metadata

The uberblock is a 256-byte structure that points to the root of the MOS. It is written atomically to a ring buffer in the vdev labels.

If multiple nodes write the MOS simultaneously, they will corrupt each other's metadata. If multiple nodes write the uberblock, the pool will have an inconsistent view.

### Solution: Single-Writer Model

Only the **coordinator** node writes the MOS and uberblock. All other nodes (participants) write only data blocks.

### Sync Flow

```
         Coordinator                    Participants
         ===========                   ============
              │                              │
    TXG OPEN  │◄─────────────────────────────│
    broadcast │  TXG_OPEN(txg=5)             │
              │──────────────────────────────►│
              │                              │
    Nodes assign transactions                │
    to local TXG 5                           │
              │                              │
    TXG QUIESCE                             │
    broadcast │  TXG_QUIESCE(txg=5)          │
              │──────────────────────────────►│
              │                              │
    Nodes drain holds                        │
              │                              │
              │  "holds drained"             │
              │◄─────────────────────────────│
              │                              │
    Wait for sync barrier:                   │
    all participants report                  │
    data flushed                             │
              │                              │
    Write MOS (coordinator only)             │
    Write uberblock (coordinator only)       │
              │                              │
    TXG SYNC DONE                           │
    broadcast │  TXG_SYNC_DONE(txg=5)        │
              │──────────────────────────────►│
              │                              │
```

### Space Map Coordination

Since only the coordinator writes the MOS, and space maps live in the MOS, a mechanism is needed for participants to update their space maps:

**Option 1: Forward to Coordinator (implemented)**
- When a participant allocates/frees blocks, it sends the space map delta to the coordinator
- The coordinator applies the delta to the MOS space map during sync
- Simple but adds latency and coordinator load

**Option 2: Per-Node Log Objects (future enhancement)**
- Each node has a log object in the MOS
- During sync, the coordinator reads all log objects and applies them
- Batches updates, reducing coordinator load

**Option 3: Separate Space Map Trees (future enhancement)**
- Each node has its own space map for its metaslabs
- The coordinator merges them during sync
- Most scalable but complex

### Uberblock Extension

The existing uberblock is extended using spare bits in the MMP fields:

| Field | Bits | Purpose |
|-------|------|---------|
| `ub_mmp_config` bits 48-63 | 16 | Coordinator node ID |
| `ub_mmp_delay` | 64 | Cluster epoch number |

This allows any node reading the uberblock to determine:
- Which node is the coordinator
- The current cluster epoch (for fencing validation)

---

## 3. TXG Coordination (`cluster_txg`)

### Problem
In single-node ZFS, the TXG state machine runs locally: Open → Quiescing → Syncing. In cluster mode, all nodes must agree on TXG boundaries.

### Solution: Centralized TXG with Optional Hybrid Delegation

**Centralized Mode (default)**:
- The coordinator opens, quiesces, and syncs TXGs
- Participants are notified via broadcast messages
- Participants assign local transactions to the coordinator-assigned TXG

**Hybrid Mode (optimization)**:
- The coordinator delegates TXG ranges to participants
- Each participant can locally open/close TXGs within its range
- The coordinator still controls sync boundaries

### Key Data Structures

```c
typedef struct cluster_txg {
    kmutex_t ctx_lock;
    cluster_node_id_t ctx_coordinator;
    uint64_t ctx_open_txg;       // currently open TXG
    uint64_t ctx_quiescing_txg;  // currently quiescing
    uint64_t ctx_syncing_txg;    // currently syncing
    uint64_t ctx_epoch;          // cluster epoch
    uint64_t ctx_sync_barrier;   // participants pending
    // Per-node dirty data tracking
    uint64_t ctx_dirty_per_node[CLUSTER_MAX_NODES];
    uint64_t ctx_dirty_total;
} cluster_txg_t;
```

### TXG State Transitions

```
                    ┌──────────┐
                    │   OPEN   │◄──── coordinator broadcasts TXG_OPEN
                    └────┬─────┘
                         │ coordinator broadcasts TXG_QUIESCE
                    ┌────▼─────┐
                    │ QUIESCING│  participants drain holds
                    └────┬─────┘
                         │ all holds=0, coordinator waits for barrier
                    ┌────▼─────┐
                    │ SYNCING  │  coordinator writes MOS+uberblock
                    └────┬─────┘
                         │ coordinator broadcasts TXG_SYNC_DONE
                    ┌────▼─────┐
                    │  DONE    │  next TXG opens
                    └──────────┘
```

### Dirty Data Throttling

Each node tracks how much dirty data it has. The coordinator aggregates this across all nodes and enforces a global dirty data limit. If the limit is reached, new transactions are throttled.

---

## 4. Distributed Lock Manager (`cluster_dlm`)

### Problem
Even with metaslab partitioning, there are metadata operations that can conflict:
- Two nodes creating files in the same directory (ZAP update conflict)
- Two nodes writing to the same dataset (DSL dirty list conflict)
- Snapshot operations that affect multiple datasets

### Solution: Fine-Grained DLM on the Coordinator

The DLM runs on the coordinator and manages locks for semantic resources:

| Resource Type | Scope | Conflict Example |
|---------------|-------|------------------|
| `CLUSTER_LOCK_DATASET` | Per-dataset | Concurrent dataset operations |
| `CLUSTER_LOCK_OBJECT` | Per-DMU object | Concurrent writes to same object |
| `CLUSTER_LOCK_ZAP` | Per-ZAP object | Concurrent directory modifications |
| `CLUSTER_LOCK_SPACE` | Per-metaslab | Free/alloc conflict (backup for partitioning) |
| `CLUSTER_LOCK_POOL_CONFIG` | Pool-wide | Snapshot, scrub, config change |
| `CLUSTER_LOCK_SNAPSHOT` | Per-snapshot | Concurrent snapshot destroy |

### Compatibility Matrix

| Mode | SHARED | EXCLUSIVE |
|------|--------|-----------|
| SHARED | ✅ Compatible | ❌ Conflict |
| EXCLUSIVE | ❌ Conflict | ❌ Conflict |

### TXG-Scoped Locks

All DLM locks are scoped to a TXG. When a TXG completes, all locks for that TXG are automatically released. This prevents deadlocks:

```
TXG 5: Node 0 holds SHARED lock on dataset X
        Node 1 holds EXCLUSIVE lock on object 42
TXG 5 syncs → all locks released
TXG 6: Node 1 can now get SHARED lock on dataset X
```

### Deadlock Prevention: Wait-Die Strategy

When a lock conflict occurs, the younger transaction (higher TXG) waits for the older one. If a cycle would form, the younger transaction is aborted and retried.

---

## 5. Membership (`cluster_membership`)

### Problem
All cluster operations require knowing which nodes are alive. A split-brain (two subsets of nodes that can't communicate) must be prevented.

### Solution: Paxos-Based Membership with Majority Quorum

**Quorum Rule**: A majority of nodes (>50%) must be reachable for any cluster operation to proceed. Special case: in a 2-node cluster, both must be reachable.

**Paxos Coordinator Election**:
1. **Prepare**: Propose new epoch, get promises from majority
2. **Accept**: Propose self as coordinator, get accepts from majority
3. **Commit**: Write decided coordinator to MOS

**Heartbeat**:
- Each node sends heartbeat to coordinator every `cluster_hb_interval` ms (default: 1000ms)
- If coordinator doesn't hear from a node for `cluster_hb_timeout` ms (default: 5000ms), the node is suspected dead
- The coordinator then fences the dead node

**MOS Persistence**: Membership state is stored in a ZAP object:
```
ZAP: "cluster_membership"
  "epoch"       → 5
  "coordinator" → 0
  "num_nodes"   → 3
  "node_0_id"   → 0
  "node_0_guid" → 12345678
  "node_0_state"→ 2  (ACTIVE)
  "node_0_role" → 1  (COORDINATOR)
  ...
```

---

## 6. Cluster ZIL (`cluster_zil`)

### Problem
Each node needs synchronous write guarantees. A shared ZIL would be a contention point.

### Solution: Per-Node ZIL Regions

Each node gets a reserved region of the SLOG (or main vdevs). The ZIL layout on a SLOG:

```
SLOG (e.g., 64GB NVMe)
┌────────────┬───────────┬───────────┬───────────┬───────────┐
│  Header    │  Node 0   │  Node 1   │  Node 2   │  Node 3   │
│  (64KB)    │  Region   │  Region   │  Region   │  Region   │
│            │  (~16GB)  │  (~16GB)  │  (~16GB)  │  (~16GB)  │
└────────────┴───────────┴───────────┴───────────┴───────────┘
```

**Region assignment**: Even split of available SLOG capacity among active nodes. When nodes join/leave, regions can be resized.

**Without SLOG**: Each node's ZIL records are written to its own metaslabs (which are already partitioned). This is slower but correct.

### Recovery

When a node fails, the coordinator:
1. Reads the dead node's ZIL region
2. Claims all blocks (marks them allocated)
3. Replays committed transactions into the current TXG
4. Frees the ZIL blocks after replay

---

## 7. Recovery and Fencing (`cluster_recovery`)

### Problem
A failed node must be prevented from writing to shared storage. Its resources must be reclaimed.

### Fencing Mechanisms

| Mechanism | Strength | Requirement | Description |
|-----------|----------|-------------|-------------|
| **Persistent** | Medium | Shared disk | Write fencing records to all leaf vdevs |
| **MMP-based** | Medium | MMP enabled | Extend MMP to detect fenced nodes |
| **Hardware** | Strong | SCSI-3 PR | Persistent reservations block I/O at HBA level |

### Recovery Flow (7 Steps)

```
┌───────────────────────────────────────────────┐
│           Node Failure Recovery                │
├───────────────────────────────────────────────┤
│ 1. FENCE dead node                            │
│    └── Write fencing records to all vdevs      │
│                                               │
│ 2. RELEASE DLM locks                          │
│    └── All locks held by dead node released    │
│    └── Waiters are granted locks               │
│                                               │
│ 3. RECLAIM metaslabs                          │
│    └── Dead node's metaslabs become unowned    │
│                                               │
│ 4. REPLAY ZIL                                 │
│    └── Read dead node's ZIL region             │
│    └── Replay committed transactions           │
│                                               │
│ 5. PROCESS deferred frees                     │
│    └── Complete in-progress deferred frees     │
│    └── from dead node's metaslabs              │
│                                               │
│ 6. REASSIGN metaslabs                         │
│    └── Distribute reclaimed metaslabs          │
│    └── among surviving nodes                   │
│                                               │
│ 7. ADVANCE epoch                              │
│    └── Increment membership epoch              │
│    └── Write new membership to MOS             │
└───────────────────────────────────────────────┘
```

### Coordinator Failover

When the coordinator fails:
1. Surviving nodes detect heartbeat timeout
2. Paxos election selects new coordinator
3. New coordinator fences old coordinator
4. New coordinator takes over (cluster_sync_coordinator_takeover)
5. New coordinator recovers old coordinator's resources
6. New coordinator begins normal operation

### Self-Fencing

If a node detects it has been fenced (by reading fencing records), it must:
1. Immediately stop all I/O
2. Suspend the pool (zio_suspend)
3. Attempt to rejoin the cluster

---

## 8. Main Integration (`cluster_spa`)

### Pool Lifecycle

**Creation**:
```
zpool create -o cluster=on -o cluster_nodes=4 tank raidz /dev/sd[a-d]
```
1. Standard pool creation
2. Cluster metadata objects created in MOS
3. Node 0 becomes initial coordinator
4. Metaslabs assigned to node 0

**Join**:
```
zpool import -o cluster_node=1 tank
```
1. Standard spa_load (read uberblock, open vdevs)
2. Read cluster config from MOS
3. Contact coordinator
4. Receive metaslab assignments
5. Reserve ZIL region
6. Become active participant

**Leave** (graceful):
1. Send leave request to coordinator
2. Release DLM locks
3. Release ZIL region
4. Reassign metaslabs
5. Clean exit

**Failure** (ungraceful):
1. Coordinator detects heartbeat timeout
2. Fence dead node
3. Recovery flow (7 steps above)

### Integration Hooks into Existing ZFS

| Existing Function | Cluster Modification |
|-------------------|---------------------|
| `spa_sync()` | Skip MOS/uberblock write on participants |
| `metaslab_class_allocatable()` | Filter out non-owned metaslabs |
| `dmu_tx_assign()` | Report dirty data to coordinator |
| `vdev_label_write()` | Extended uberblock with cluster epoch |
| `spa_open()` / `spa_import()` | Check cluster membership |
| `zil_commit()` | Write to per-node ZIL region |
| `mmp_thread()` | Extended for cluster-aware activity checks |

### New Pool Properties

| Property | Type | Description |
|----------|------|-------------|
| `cluster` | boolean | Enable/disable cluster mode |
| `cluster_nodes` | uint64 | Maximum number of cluster nodes |
| `cluster_node_id` | uint64 | This node's ID in the cluster |
| `cluster_ms_policy` | string | Metaslab partitioning policy |
| `cluster_hb_interval` | uint64 | Heartbeat interval (ms) |
| `cluster_hb_timeout` | uint64 | Heartbeat timeout (ms) |

---

## Data Flow: Write Path

```
Application: write(fd, buf, len)
     │
     ▼
VFS: zfs_write()
     │
     ▼
DMU: dmu_write() → dmu_tx_assign(tx, txg)
     │                              │
     │                   ┌──────────┴──────────┐
     │                   │  Cluster Check:      │
     │                   │  Report dirty data   │
     │                   │  to coordinator      │
     │                   └──────────┬──────────┘
     │                              │
     ▼                              ▼
ZIL: zil_commit()          Cluster: DLM lock check
     │                     (if needed for object)
     ▼
Cluster ZIL: write to per-node region
     │
     ▼
ZIO Pipeline: OPEN → WRITE_BP_INIT → ... → DVA_ALLOCATE
                                         │
                              ┌──────────┴──────────┐
                              │  Cluster Metaslab    │
                              │  Filter: only use    │
                              │  metaslabs owned by  │
                              │  this node           │
                              └──────────┬──────────┘
                                         │
                                         ▼
                              VDEV_IO_START: write to disk
```

## Data Flow: Sync Path

```
                    Coordinator                         Participants
                    ===========                         ============
                         │                                    │
    TXG opens            │◄─── TXG_OPEN(txg=N) broadcast ────│
                         │                                    │
    Nodes write data     │          data blocks written       │
    blocks (all nodes)   │◄──────────────────────────────────►│
                         │                                    │
    TXG quiesces         │─── TXG_QUIESCE(txg=N) broadcast ──►│
                         │                                    │
    Nodes drain holds    │          holds drained             │
                         │◄───────────────────────────────────│
                         │                                    │
    Wait for all         │─── SYNC_BARRIER ──────────────────►│
    participants to      │◄── SYNC_BARRIER_ACK ──────────────│
    flush data           │                                    │
                         │                                    │
    Write MOS            │  (participants: skip this step)    │
    Write uberblock      │  (participants: skip this step)    │
                         │                                    │
    TXG sync done        │─── TXG_SYNC_DONE(txg=N) ─────────►│
                         │                                    │
```

## Consistency Model

### What is Consistent
- **Data blocks**: No conflicts due to metaslab partitioning
- **MOS/uberblock**: Single-writer guarantee (coordinator only)
- **ZIL**: Per-node isolation, coordinator replays on failure
- **Space maps**: Partitioning means only owner writes its space maps

### What Requires Coordination
- **Dataset operations**: DLM lock on DATASET resource
- **Directory operations**: DLM lock on ZAP resource
- **Pool config changes**: DLM lock on POOL_CONFIG resource
- **Snapshot/clone**: DLM lock on SNAPSHOT resource

### What is NOT Supported (Current Design)
- **Concurrent writes to the same file**: Requires OBJECT-level DLM lock (performance bottleneck)
- **POSIX atomic rename across nodes**: Requires ZAP lock (serialization)
- **Distributed mmap**: Not supported (requires cluster page cache)

These limitations are acceptable for the initial design. Future work could add a distributed page cache and more fine-grained locking for concurrent file writes.

---

## Safety Analysis

### Split-Brain Prevention
- Paxos-based coordinator election requires majority quorum
- Minority partition cannot make progress (cannot reach quorum)
- Fencing records on disk prevent fenced nodes from writing

### Data Integrity
- Checksums verified on read (existing ZFS guarantee)
- Copy-on-write prevents in-place updates (existing ZFS guarantee)
- Metaslab partitioning prevents allocation conflicts
- Single-writer for MOS ensures metadata consistency

### Crash Recovery
- ZIL replay recovers committed but not-yet-synced operations
- Deferred free completion prevents space leaks
- Uberblock ring buffer provides rollback capability
- MOS ZAP objects provide persistent cluster state

### Deadlock Prevention
- TXG-scoped DLM locks prevent lock cycles
- Wait-die strategy for lock conflicts
- Sync barrier has timeout (falls back to fencing if needed)

---

## Performance Considerations

### Scalability Bottlenecks
1. **Coordinator MOS writes**: All metadata updates flow through coordinator
   - Mitigation: Batch updates, per-node log objects
2. **DLM on coordinator**: All lock requests go to coordinator
   - Mitigation: Lock caching, TXG-scoped locks reduce request rate
3. **TXG serialization**: All nodes must complete before MOS write
   - Mitigation: Sync barrier timeout, adaptive TXG intervals

### Expected Performance
- **Sequential writes**: Near-linear scaling (metaslab partitioning)
- **Random writes**: Good scaling (independent metaslabs)
- **Metadata-heavy workloads**: Limited by coordinator (DLM + MOS)
- **Mixed workloads**: Good scaling for data, limited for metadata

### Optimization Opportunities (Future Work)
- Delegated TXG ranges for local transaction assignment
- Per-node log objects for space map batching
- DLM lock delegation for hot resources
- RDMA for cluster messaging (reduce latency)
- NVMe-oF for direct storage access from all nodes

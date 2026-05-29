# EZIO Chain Topology Design

**Version:** 1.0
**Date:** 2025-12-22
**Status:** DRAFT - Not scheduled for implementation
**Related Issue:** #44

> **Note:** This is a draft design document created for future reference.
> It will NOT be implemented unless specifically requested by the user.
> This document is archived here for planning purposes only.

---

## Executive Summary

This document describes the design and implementation plan for BitTorrent chain topology in EZIO, enabling sequential data flow through a linear chain of nodes to maximize LAN bandwidth efficiency.

**Goal:** Transform standard BitTorrent mesh topology into a linear chain:

```
Standard P2P Mesh:          Chain Topology:
     ┌─────┐                    ┌─────┐
  ┌──┤ S1  ├──┐              ┌──┤ S1  │
  │  └─────┘  │              │  └─────┘
  │           │              ↓
┌─┴─┐       ┌─┴─┐          ┌───┐     ┌───┐     ┌───┐
│ N1│←─────→│ N2│          │ N1├────→│ N2├────→│ N3│
└─┬─┘       └─┬─┘          └───┘     └───┘     └───┘
  │     ┌─────┘              ↓         ↓         ↓
  │   ┌─┴─┐                  ↓         ↓         ↓
  └──→│ N3│               (download  (download  (download
      └───┘                from prev) from prev) from prev)
```

**Key Requirements:**
- Dynamic node ordering via coordinator service
- Automatic chain repair when nodes fail
- Global chain shared across all torrents
- gRPC API for runtime control
- **Directional data flow enforcement** (prev→download, next→upload)

**Critical Challenge:**
BitTorrent peer connections are non-directional. Simply connecting to two peers (prev and next) does NOT guarantee that one connection will be used for download and the other for upload. This requires explicit enforcement mechanisms.

---

## 1. Architecture Overview

### 1.1 System Components

```
┌──────────────────────────────────────────────────────────────┐
│                    EZIO Application Layer                     │
├──────────────────────────────────────────────────────────────┤
│                                                                │
│  ┌──────────────┐         ┌────────────────────────────┐    │
│  │ gRPC Service │────────→│     Chain Manager          │    │
│  │ (service.cpp)│         │                            │    │
│  └──────────────┘         │  ├─ Node Registry          │    │
│         ↕                 │  ├─ State Machine          │    │
│  ┌──────────────┐         │  ├─ Failure Detector       │    │
│  │ EZIO Daemon  │←────────┤  ├─ Connection Controller  │    │
│  │ (daemon.cpp) │         │  └─ Flow Enforcer          │    │
│  └──────────────┘         └────────────────────────────┘    │
│         ↕                              ↕                      │
│  ┌──────────────┐         ┌────────────────────────────┐    │
│  │ Alert Handler│────────→│   HTTP Client              │    │
│  │ (log.cpp)    │         │   (coordinator comm)       │    │
│  └──────────────┘         └────────────────────────────┘    │
│                                        ↕                      │
├──────────────────────────────────────────────────────────────┤
│              libtorrent Session                               │
│  (connect_peer, ip_filter, alerts, sequential_download)      │
└──────────────────────────────────────────────────────────────┘
                              ↕
┌──────────────────────────────────────────────────────────────┐
│                 Chain Coordinator Service                     │
│              (External HTTP/REST Service)                     │
│                                                                │
│  Endpoints: /register, /heartbeat, /report_failure, /status  │
└──────────────────────────────────────────────────────────────┘
```

### 1.2 Component Responsibilities

**Chain Manager** (`chain_manager.{hpp,cpp}`)
- Registers with coordinator to get position in chain
- Maintains neighbor state (prev_node, next_node)
- Enforces connection topology via libtorrent APIs
- Monitors peer connections for failures
- Triggers recovery on node failures

**Coordinator Service** (`chain_coordinator/coordinator.py`)
- Maintains global chain state
- Assigns node positions dynamically
- Handles concurrent node registrations
- Provides chain repair instructions on failure

**Alert Handler** (`log.cpp`)
- Forwards peer_connect and peer_disconnected alerts to chain_manager
- Enables real-time failure detection

**gRPC Service** (`service.cpp`, `ezio.proto`)
- Exposes chain control API to external tools
- Methods: EnableChainTopology, DisableChainTopology, GetChainStatus, RepairChain

---

## 2. Critical Problem: Directional Data Flow

### 2.1 Problem Statement

**BitTorrent peer connections are inherently non-directional.**

When EZIO executes:
```cpp
torrent.connect_peer(prev_endpoint);  // Connect to previous node
torrent.connect_peer(next_endpoint);  // Connect to next node
```

This establishes two bidirectional connections, but **does NOT specify**:
- Which peer to download from (should be prev_node)
- Which peer to upload to (should be next_node)

**The actual upload/download direction is determined by:**
1. **Piece availability** (bitfield exchange)
2. **Interested/choke state machine**
3. **Peer selection algorithm** (libtorrent internals)
4. **Optimistic unchoke** (periodic randomization)

**Without intervention, data flow can be random:**
```
Node A ←→ Node B ←→ Node C
         (random directions)
```

**Desired behavior:**
```
Node A → Node B → Node C
      ↓        ↓
   download  download
   from prev from prev
```

### 2.2 Solution: Multi-Layered Enforcement

We employ **four complementary strategies** to enforce directional flow:

#### Strategy 1: Sequential Download (Mandatory)

**Force nodes to download pieces in sequential order.**

```cpp
// In daemon.cpp when adding torrent in chain mode:
atp.flags |= lt::torrent_flags::sequential_download;
```

**How it enforces directionality:**

```
Time T=0:
  Seeder has all pieces [0, 1, 2, 3, ...]
  Node A (first in chain) requests piece 0
  Node B cannot request anything yet (doesn't have piece 0)

Time T=1:
  Node A completes piece 0, starts requesting piece 1
  Node B now requests piece 0
    → Only source: Node A (seeder doesn't connect to all nodes)
    → Natural download from prev node (A)

Time T=2:
  Node A completes piece 1, requests piece 2
  Node B completes piece 0, requests piece 1
    → Only source: Node A
  Node C requests piece 0
    → Only source: Node B

Result: Natural sequential chain flow A→B→C
```

**Critical property:** Each node is always one piece behind its predecessor, creating natural download dependency.

#### Strategy 2: Connection Limits

**Restrict each node to exactly 2 peer connections (prev + next).**

```cpp
// Per-torrent configuration
atp.max_connections = 2;  // Only prev + next
atp.max_uploads = 1;      // Only unchoke one peer (should be next)

// Session-wide configuration
lt::settings_pack p;
p.set_bool(lt::settings_pack::enable_dht, false);  // Disable DHT
p.set_bool(lt::settings_pack::enable_lsd, false);  // Disable LSD
p.set_bool(lt::settings_pack::enable_pex, false);  // Disable PEX
m_session.apply_settings(p);
```

**Effect:**
- Prevents connections to other nodes
- With `max_uploads=1`, only one peer gets unchoked
- Sequential download bias favors next_node as the unchoked peer

#### Strategy 3: IP Filtering (Whitelist)

**Block all peer connections except prev and next nodes.**

```cpp
void chain_manager::apply_ip_filter() {
    lt::ip_filter filter;

    // Block all IPs by default
    filter.add_rule(
        lt::address_v4::from_string("0.0.0.0"),
        lt::address_v4::from_string("255.255.255.255"),
        lt::ip_filter::blocked
    );

    // Whitelist prev node
    if (!m_prev_node.ip.empty()) {
        filter.add_rule(
            lt::address_v4::from_string(m_prev_node.ip),
            lt::address_v4::from_string(m_prev_node.ip),
            0  // Allow
        );
    }

    // Whitelist next node
    if (!m_next_node.ip.empty()) {
        filter.add_rule(
            lt::address_v4::from_string(m_next_node.ip),
            lt::address_v4::from_string(m_next_node.ip),
            0  // Allow
        );
    }

    m_session.set_ip_filter(filter);
}
```

**Effect:**
- Enforces topology at network level
- Prevents accidental connections from DHT/PEX/LSD
- Complements `max_connections=2`

#### Strategy 4: Validation and Monitoring

**Continuously monitor peer connections to verify directional flow.**

```cpp
void chain_manager::validate_connections() {
    for (auto& torrent : m_session.get_torrents()) {
        std::vector<lt::peer_info> peers;
        torrent.get_peer_info(peers);

        for (auto& peer : peers) {
            std::string peer_ip = peer.ip.address().to_string();

            if (peer_ip == m_prev_node.ip) {
                // Verify we're downloading from prev
                if (peer.down_speed > 0) {
                    m_metrics.prev_download_bytes += peer.total_download;
                }
                // Warn if uploading to prev (reverse flow)
                if (peer.up_speed > 0) {
                    spdlog::warn("Reverse flow detected: uploading to prev node");
                }
            }
            else if (peer_ip == m_next_node.ip) {
                // Verify we're uploading to next
                if (peer.up_speed > 0) {
                    m_metrics.next_upload_bytes += peer.total_upload;
                }
                // Warn if downloading from next (reverse flow)
                if (peer.down_speed > 0) {
                    spdlog::warn("Reverse flow detected: downloading from next node");
                }
            }
        }
    }

    // Calculate directional accuracy
    double download_accuracy =
        m_metrics.prev_download_bytes /
        (m_metrics.prev_download_bytes + m_metrics.next_download_bytes);

    if (download_accuracy < 0.95) {
        spdlog::error("Directional flow accuracy below threshold: {:.1f}%",
                      download_accuracy * 100);
    }
}
```

**Metrics:**
- % of download bytes from prev_node (target: >95%)
- % of upload bytes to next_node (target: >95%)
- Logged every 30 seconds

### 2.3 Fallback Strategy

**If directional flow cannot be enforced:**
1. Log warnings with detailed peer statistics
2. Continue operating (graceful degradation)
3. Report metrics to coordinator for visibility
4. Allow user to disable chain mode via gRPC if performance degrades

**Rationale:** In most cases, sequential download alone provides sufficient directional bias. Perfect enforcement is not critical as long as majority of traffic flows in the correct direction.

---

## 3. Chain State Machine

### 3.1 States

```
┌──────────┐
│ DISABLED │  (Initial state, normal P2P mode)
└────┬─────┘
     │ EnableChainTopology()
     ↓
┌─────────────┐
│ REGISTERING │  (Contacting coordinator, waiting for position)
└─────┬───────┘
      │ Registration success
      ↓
┌────────┐
│ ACTIVE │  (Chain operational, monitoring neighbors)
└───┬────┘
    │ Neighbor failure detected
    ↓
┌────────────┐
│ RECOVERING │  (Reconnecting to new neighbors)
└───┬────────┘
    │ Recovery success
    ↓
┌────────┐
│ ACTIVE │
└────────┘
    │ Coordinator unreachable / repeated failures
    ↓
┌────────┐
│ ERROR  │  (Manual intervention required)
└────────┘
```

### 3.2 State Transitions

**DISABLED → REGISTERING**
- Trigger: `EnableChainTopology()` gRPC call
- Action: POST /register to coordinator with node_id and IP

**REGISTERING → ACTIVE**
- Trigger: Coordinator returns position and neighbors
- Actions:
  1. Store prev_node and next_node
  2. Apply IP filter (whitelist prev/next)
  3. Set max_connections=2, max_uploads=1
  4. Enable sequential_download on all torrents
  5. Connect to prev_node and next_node
  6. Start heartbeat thread (every 10s)
  7. Start validation thread (every 30s)

**ACTIVE → RECOVERING**
- Trigger: peer_disconnected_alert for prev or next node
- Actions:
  1. POST /report_failure to coordinator with failed_node_id
  2. Wait for new neighbors response
  3. Disconnect old peers
  4. Update IP filter with new neighbor IPs
  5. Connect to new neighbors

**RECOVERING → ACTIVE**
- Trigger: Successful connection to new neighbors
- Action: Log recovery success, resume monitoring

**ACTIVE → ERROR**
- Trigger: Coordinator unreachable for 60s OR 3 consecutive recovery failures
- Action: Log error, await manual DisableChainTopology() call

**ERROR → DISABLED**
- Trigger: `DisableChainTopology()` gRPC call
- Actions:
  1. Clear IP filter (allow all peers)
  2. Disconnect all peers
  3. Restore default max_connections, max_uploads
  4. Re-enable DHT/PEX/LSD
  5. Stop heartbeat and validation threads

---

## 4. Coordinator Service Design

### 4.1 REST API Endpoints

**POST /register**

Request:
```json
{
  "node_id": "node-001",
  "ip": "192.168.1.100",
  "port": 6881
}
```

Response:
```json
{
  "position": 2,
  "prev_node": {
    "node_id": "node-000",
    "ip": "192.168.1.99",
    "port": 6881
  },
  "next_node": {
    "node_id": "node-002",
    "ip": "192.168.1.101",
    "port": 6881
  },
  "chain_version": 5
}
```

**POST /heartbeat**

Request:
```json
{
  "node_id": "node-001",
  "timestamp": 1640000000
}
```

Response:
```json
{
  "status": "ok",
  "chain_version": 5
}
```

**POST /report_failure**

Request:
```json
{
  "reporter_node_id": "node-001",
  "failed_node_id": "node-002",
  "failure_time": 1640000000
}
```

Response:
```json
{
  "new_next_node": {
    "node_id": "node-003",
    "ip": "192.168.1.102",
    "port": 6881
  },
  "chain_version": 6
}
```

**GET /status**

Response:
```json
{
  "chain_version": 6,
  "total_nodes": 10,
  "nodes": [
    {"node_id": "node-000", "ip": "192.168.1.99", "status": "active"},
    {"node_id": "node-001", "ip": "192.168.1.100", "status": "active"},
    {"node_id": "node-003", "ip": "192.168.1.102", "status": "active"}
  ]
}
```

### 4.2 Failure Detection

**Coordinator-side timeout:**
- If no heartbeat received for 30 seconds, mark node as `suspected`
- If no heartbeat received for 60 seconds, mark node as `failed`
- Notify neighbors of failed node

**Node-side detection:**
- If peer_disconnected_alert received, immediately report to coordinator
- Don't wait for heartbeat timeout (faster recovery)

### 4.3 Concurrent Failure Handling

**Scenario:** Multiple nodes report failures simultaneously

**Solution: Atomic chain updates with versioning**

```python
# Pseudo-code in coordinator
def report_failure(reporter_id, failed_id):
    with chain_lock:
        current_version = chain_state.version

        # Find reporter's neighbors
        reporter_pos = chain_state.find_position(reporter_id)

        # Remove failed node from chain
        chain_state.remove_node(failed_id)

        # Increment version
        chain_state.version += 1

        # Return new neighbors based on updated chain
        return {
            'new_neighbors': chain_state.get_neighbors(reporter_pos),
            'chain_version': chain_state.version
        }
```

**Optimistic concurrency:**
- Each response includes `chain_version`
- Nodes can detect stale state if version mismatch
- Re-query coordinator for latest state

---

## 5. gRPC API Extensions

### 5.1 Protobuf Definitions

```protobuf
// ezio.proto additions

message ChainConfig {
    bool enabled = 1;
    string coordinator_url = 2;
    int32 heartbeat_interval = 3;  // seconds, default 10
    int32 failure_timeout = 4;     // seconds, default 30
}

message ChainNode {
    string node_id = 1;
    string ip = 2;
    int32 port = 3;
    string status = 4;  // "active", "suspected", "failed"
}

message ChainStatus {
    string state = 1;  // "DISABLED", "REGISTERING", "ACTIVE", "RECOVERING", "ERROR"
    int32 position = 2;
    ChainNode prev_node = 3;
    ChainNode next_node = 4;
    int64 chain_version = 5;

    // Metrics
    double download_from_prev_mb = 6;
    double upload_to_next_mb = 7;
    double directional_accuracy = 8;  // 0.0 to 1.0
}

service EZIO {
    // Existing RPCs...

    // New chain topology RPCs
    rpc EnableChainTopology(ChainConfig) returns (Empty) {}
    rpc DisableChainTopology(Empty) returns (Empty) {}
    rpc GetChainStatus(Empty) returns (ChainStatus) {}
    rpc RepairChain(Empty) returns (ChainStatus) {}  // Force re-registration
}
```

### 5.2 RPC Implementation

**EnableChainTopology**
```cpp
void service::EnableChainTopology(ChainConfig const& config,
                                   std::function<void()> callback) {
    if (m_chain_manager->state() != chain_state::DISABLED) {
        throw std::runtime_error("Chain already enabled");
    }

    m_chain_manager->set_coordinator_url(config.coordinator_url());
    m_chain_manager->set_heartbeat_interval(config.heartbeat_interval());
    m_chain_manager->enable();

    callback();
}
```

**GetChainStatus**
```cpp
ChainStatus service::GetChainStatus() {
    ChainStatus status;

    auto state = m_chain_manager->state();
    status.set_state(chain_state_to_string(state));
    status.set_position(m_chain_manager->position());

    auto prev = m_chain_manager->prev_node();
    if (prev) {
        *status.mutable_prev_node() = node_to_proto(*prev);
    }

    auto next = m_chain_manager->next_node();
    if (next) {
        *status.mutable_next_node() = node_to_proto(*next);
    }

    auto metrics = m_chain_manager->metrics();
    status.set_download_from_prev_mb(metrics.prev_download_mb);
    status.set_upload_to_next_mb(metrics.next_upload_mb);
    status.set_directional_accuracy(metrics.directional_accuracy);

    return status;
}
```

---

## 6. Implementation Phases

### Phase 1: Coordination Service (Week 1)

**Goal:** Build external coordinator for chain state management

**Tasks:**
1. Implement HTTP REST API service (Python/Flask or Go)
   - `/register` - Node registration with position assignment
   - `/heartbeat` - Liveness checking
   - `/report_failure` - Failure reporting and neighbor updates
   - `/status` - Query chain state

2. Chain state management
   - In-memory storage with Redis backup (optional)
   - Atomic updates with versioning
   - Concurrent failure handling

3. Testing
   - Unit tests for registration, failure scenarios
   - Concurrent failure simulation

**Deliverables:**
- `chain_coordinator/coordinator.py` (or `.go`)
- `chain_coordinator/README.md`
- Unit tests

**Files created:**
- NEW: `chain_coordinator/` directory

---

### Phase 2: Chain Manager Core (Week 2)

**Goal:** Implement EZIO-side chain topology management

**Tasks:**
1. Chain manager component
   - State machine implementation (5 states)
   - HTTP client for coordinator communication
   - Registration and heartbeat threads

2. Connection control
   - Sequential download enforcement
   - IP filter configuration
   - Manual peer connections (`torrent_handle::connect_peer()`)

3. Alert integration
   - Forward peer_connect/peer_disconnected alerts to chain_manager
   - Trigger recovery on disconnection

**Deliverables:**
- `chain_manager.hpp` - Interface and state machine
- `chain_manager.cpp` - Implementation (~800 lines)
- `http_client.hpp` - HTTP wrapper for libcurl

**Files modified:**
- MODIFY: `log.cpp` (lines 64-97) - Forward peer alerts
- MODIFY: `daemon.hpp` - Add `chain_manager` member
- MODIFY: `daemon.cpp` - Initialize chain_manager

**Example modification in log.cpp:**
```cpp
void log::report_alert() {
    std::vector<libtorrent::alert *> alerts;
    m_daemon.pop_alerts(&alerts);

    for (auto a : alerts) {
        spdlog::info("lt alert: {} {}", a->what(), a->message());

        // Forward peer alerts to chain manager
        if (auto* pc = lt::alert_cast<lt::peer_connect_alert>(a)) {
            m_daemon.chain_manager().on_peer_connected(pc);
        }
        else if (auto* pd = lt::alert_cast<lt::peer_disconnected_alert>(a)) {
            m_daemon.chain_manager().on_peer_disconnected(pd);
        }
    }
}
```

---

### Phase 3: gRPC API Integration (Week 3)

**Goal:** Expose chain control via gRPC interface

**Tasks:**
1. Protobuf definitions
   - `ChainConfig`, `ChainStatus`, `ChainNode` messages
   - 4 new RPC methods

2. gRPC service implementation
   - `EnableChainTopology()` - Start chain mode
   - `DisableChainTopology()` - Stop chain mode
   - `GetChainStatus()` - Query current state
   - `RepairChain()` - Force re-registration

3. CLI integration (optional)
   - Command-line flags: `--enable-chain`, `--coordinator-url`

**Deliverables:**
- MODIFY: `ezio.proto` - Add chain messages and RPCs
- MODIFY: `service.cpp` - Implement RPC handlers
- MODIFY: `main.cpp` - Add CLI flags (optional)

**Example CLI usage:**
```bash
# Start EZIO with chain mode
./ezio --enable-chain --coordinator-url http://coordinator:8080

# Or enable at runtime via gRPC
grpcurl -d '{"enabled": true, "coordinator_url": "http://coordinator:8080"}' \
    localhost:50051 EZIO/EnableChainTopology
```

---

### Phase 4: Directional Flow Enforcement (Week 4) **CRITICAL**

**Goal:** Ensure prev→download, next→upload behavior

**Tasks:**
1. Sequential download enforcement
   - Apply `sequential_download` flag to ALL torrents
   - Verify in add_torrent() codepath

2. Connection limits
   - Set `max_uploads=1`, `max_connections=2`
   - Disable DHT/PEX/LSD

3. IP filtering
   - Apply whitelist after neighbor assignment
   - Re-apply on recovery

4. Validation and metrics
   - Implement `validate_connections()` in chain_manager
   - Calculate directional accuracy (>95% target)
   - Log warnings on reverse flow
   - Report metrics via GetChainStatus RPC

**Deliverables:**
- MODIFY: `chain_manager.cpp` - Add `enforce_directional_flow()`
- MODIFY: `chain_manager.cpp` - Add `validate_connections()`
- MODIFY: `daemon.cpp` - Apply sequential_download flag in add_torrent()

**Example modification in daemon.cpp:**
```cpp
void ezio::add_torrent(..., bool chain_mode) {
    lt::add_torrent_params atp;
    // ... existing setup ...

    if (chain_mode) {
        // CRITICAL: Enable sequential download for chain topology
        atp.flags |= lt::torrent_flags::sequential_download;
        atp.max_uploads = 1;
        atp.max_connections = 2;

        spdlog::info("Chain mode: sequential download enabled");
    }

    lt::torrent_handle handle = session_.add_torrent(std::move(atp));
}
```

**Validation metrics logged every 30s:**
```
[chain] Download from prev: 1250 MB (98.5%)
[chain] Upload to next: 1230 MB (97.8%)
[chain] Directional accuracy: 98.2%
```

---

### Phase 5: Failure Recovery (Week 5)

**Goal:** Robust handling of node failures and chain repair

**Tasks:**
1. Failure detection
   - Handle peer_disconnected_alert
   - Detect heartbeat timeout (coordinator-side)

2. Recovery protocol
   - Report failure to coordinator
   - Fetch updated neighbors
   - Disconnect old peers
   - Connect to new neighbors
   - Re-apply IP filter

3. Edge cases
   - Coordinator unavailable (retry with exponential backoff)
   - Multiple simultaneous failures
   - Chain split/partition
   - Late-joining nodes

4. Testing
   - Simulate single node failure
   - Simulate multiple concurrent failures
   - Measure recovery time (<5s target)

**Deliverables:**
- MODIFY: `chain_manager.cpp` - Complete recovery logic
- MODIFY: `chain_coordinator/coordinator.py` - Handle concurrent failures

**Recovery time target:** <5 seconds from failure detection to new connection established

---

### Phase 6: Testing & Validation (Week 6)

**Goal:** Validate functionality and measure performance

**Test Scenarios:**
1. **Basic chain** - 10 nodes, sequential start
2. **Late join** - 5 nodes, then add 5 more mid-transfer
3. **Single node failure** - Kill middle node, verify recovery
4. **Multiple failures** - Kill 3 nodes simultaneously
5. **Coordinator failure** - Kill coordinator, verify chain continues (heartbeat fails but data flow continues)
6. **Directional flow validation** - Verify upload/download directions with tcpdump or peer_info logs

**Performance Metrics:**
- **Throughput:** Compare chain vs normal P2P
  - Target: >90% of baseline (acceptable tradeoff for sequential behavior)
- **Recovery time:** <5 seconds for single failure
- **Directional accuracy:** >95% of traffic in correct direction

**Tools:**
- Integration test suite (bash scripts or pytest)
- Performance benchmark script
- Grafana dashboard for real-time metrics (optional)

**Deliverables:**
- Test suite
- Performance results document
- README with usage examples

---

## 7. Critical Files Summary

### New Files

| File | Purpose | Estimated Lines |
|------|---------|-----------------|
| `chain_manager.hpp` | Chain manager interface | ~200 |
| `chain_manager.cpp` | Chain manager implementation | ~800 |
| `http_client.hpp` | HTTP client wrapper (libcurl) | ~100 |
| `chain_coordinator/coordinator.py` | Coordination service | ~500 |
| `chain_coordinator/README.md` | Coordinator documentation | ~100 |

**Total new code:** ~1,700 lines

### Modified Files

| File | Changes | Lines Modified |
|------|---------|----------------|
| `ezio.proto` | Add chain RPC messages | +50 |
| `service.cpp` | Implement chain RPCs | +100 |
| `daemon.hpp` | Add chain_manager member | +5 |
| `daemon.cpp` | Initialize chain_manager, apply sequential_download | +30 |
| `log.cpp` | Forward peer alerts to chain_manager | +20 |
| `main.cpp` | Add chain CLI flags (optional) | +15 |

**Total modifications:** ~220 lines

**Grand total:** ~1,920 lines of code

---

## 8. Key Design Decisions

### Decision 1: Sequential Download is Mandatory

**Rationale:**
Without sequential download, chain topology provides no benefit. Random piece selection would cause nodes to request pieces from any available source, breaking directional flow.

**Trade-off:**
Sequential download is less efficient for normal P2P (increases piece rarity imbalance). But in chain mode, this is acceptable since each node has exactly one download source.

**Validation:**
Verify sequential_download flag is set via torrent_handle::status().flags

---

### Decision 2: Global Chain (Not Per-Torrent)

**Rationale:**
Simplifies coordination. All torrents use same prev/next neighbors. Easier to reason about and implement.

**Trade-off:**
Cannot optimize different torrents differently. But user explicitly requested global chain.

**Alternative:**
Per-torrent chains would require separate coordinator state and more complex neighbor management.

---

### Decision 3: Centralized Coordinator

**Rationale:**
LAN environment assumption makes coordinator reliable. Simpler than distributed consensus. Faster failure recovery.

**Trade-off:**
Single point of failure for registration. Mitigated by:
1. Coordinator can be HA (active-standby)
2. Chain continues operating if coordinator down (just can't add new nodes)
3. Redis for coordinator state persistence

**Alternative:**
Distributed consensus (Raft, Paxos) is overkill for LAN environment.

---

### Decision 4: No Custom Unchoke Algorithm

**Rationale:**
libtorrent doesn't expose peer-level unchoke control. Would require forking libtorrent.

**Alternative:**
Rely on `max_uploads=1` + sequential download to naturally favor next node. Accept occasional reverse flow as acceptable (<5% of traffic).

**Future work:**
If directional accuracy <95%, consider libtorrent modifications.

---

## 9. Risk Mitigation

### Risk 1: Reverse Data Flow

**Risk:** Despite configuration, upload goes to prev instead of next.

**Detection:**
Monitor peer_info upload rates per peer. Log warnings if reverse flow detected.

**Mitigation:**
- Sequential download provides self-correction over time
- If persistent (accuracy <80%), escalate to ERROR state
- Allow user to disable chain mode via gRPC

**Probability:** Low (sequential download is strong forcing function)

---

### Risk 2: libtorrent Override

**Risk:** libtorrent's internal algorithms (optimistic unchoke) override manual configuration.

**Detection:**
Monitor unexpected peer connections via alerts.

**Mitigation:**
- Aggressive IP filter (block all except prev/next)
- Disable DHT/PEX/LSD
- Periodic validation and re-application of rules

**Probability:** Medium (optimistic unchoke is documented behavior)

---

### Risk 3: Coordinator SPOF

**Risk:** Coordinator failure prevents new nodes joining.

**Detection:**
Heartbeat timeout, connection refused errors.

**Mitigation:**
- Coordinator can run on multiple hosts (active-standby with shared Redis)
- Chain continues operating even if coordinator down (just can't add nodes)
- Exponential backoff retry for registration

**Probability:** Low (coordinator is simple HTTP service, easy to make HA)

---

### Risk 4: Recovery Time Exceeds Target

**Risk:** Recovery takes >5 seconds, causing data stall.

**Detection:**
Measure time from peer_disconnected_alert to new peer connection established.

**Mitigation:**
- Optimize coordinator response time (<100ms target)
- Use fast HTTP client (libcurl with connection pooling)
- Implement timeout-based fallback (if recovery >10s, disable chain mode)

**Probability:** Low (LAN latency is <1ms, HTTP overhead minimal)

---

## 10. Success Criteria

### Functional Requirements

- ✅ Chain forms automatically via dynamic coordination
- ✅ Each node connects only to prev and next neighbors
- ✅ Data flows sequentially: seeder → node1 → node2 → ...
- ✅ Single node failure triggers automatic chain repair (<5s)
- ✅ Multiple concurrent failures handled gracefully
- ✅ gRPC API provides full control

### Performance Requirements

- ✅ Throughput: >90% of max LAN bandwidth (compared to normal P2P)
- ✅ Directional accuracy: >95% of traffic in intended direction
- ✅ Recovery time: <5 seconds from detection to reconnection
- ✅ Coordinator latency: <100ms for registration/failure report

### Operational Requirements

- ✅ Zero-downtime enable/disable via gRPC
- ✅ Observable chain state (GetChainStatus RPC)
- ✅ Detailed logging for debugging
- ✅ Graceful degradation on errors (revert to normal P2P if chain fails)

---

## 11. Future Enhancements (Post-MVP)

1. **Per-torrent chains** - Allow different chain configurations for different torrents
2. **Dynamic re-ordering** - Reorder chain based on performance metrics
3. **Hybrid mode** - Allow some torrents in chain mode, others in P2P mode
4. **Metrics dashboard** - Grafana/Prometheus integration
5. **Advanced recovery** - Predict failures based on latency/bandwidth metrics
6. **Multi-chain** - Support multiple parallel chains for different data flows

---

## 12. References

### Related Documents
- `CLAUDE.md` - EZIO architecture overview
- `docs/SESSION_MEMORY.md` - Complete conversation history
- GitHub Issue #44 - Chain topology feature request

### External Projects
- [murder](https://github.com/lg/murder) - Twitter's BitTorrent deployment tool with chain mode
- libtorrent documentation - [Manual peer connections](https://www.libtorrent.org/manual-ref.html#torrent-handle)

### libtorrent APIs
- `torrent_handle::connect_peer()` - Manual peer connection
- `torrent_handle::get_peer_info()` - Query peer states
- `session::set_ip_filter()` - Whitelist/blacklist peers
- `torrent_flags::sequential_download` - Force sequential piece requests
- `peer_connect_alert`, `peer_disconnected_alert` - Peer lifecycle events

---

**Document Status:** DRAFT - Archived for future reference
**Implementation:** NOT SCHEDULED - Will only proceed if explicitly requested by user
**Estimated Total Effort:** 6 weeks (1 week per phase) if implemented

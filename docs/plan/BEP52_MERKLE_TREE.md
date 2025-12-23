# BEP 52 BitTorrent v2 Merkle Tree Architecture

**Document Version:** 1.0
**Last Updated:** 2025-12-23
**Status:** Design Documentation

---

## Executive Summary

BitTorrent v2 (BEP 52) introduces a **two-layer Merkle tree architecture** for data integrity verification:
1. **Per-piece Merkle trees**: Each piece contains a tree of 16KB block hashes
2. **File-level Merkle tree**: Combines all piece roots into a file root hash

**Key Requirements:**
- Piece size: Power of 2, minimum 16KB
- Block size: 16KB (fixed)
- Padding: Merkle tree leaf nodes padded to power of 2 (NOT block content)

**⚠️ Important:** Last block may be < 16KB and is hashed as-is without padding.

This design eliminates odd-node problems through systematic Merkle tree padding.

---

## BEP 52 Requirements

### 1. Piece Size Constraint

> **BEP 52:** "Piece size must be a power of two and at least 16KiB."

```
Valid piece sizes:
16KB  = 2^14 ✅
32KB  = 2^15 ✅
256KB = 2^18 ✅
16MB  = 2^24 ✅
32MB  = 2^25 ✅

Invalid:
24MB (not power of 2) ❌
10MB (not power of 2) ❌
```

### 2. Block Size Fixed at 16KB

```
Block size = 16KB = 2^14 (constant)
```

### 3. Mandatory Padding

> **BEP 52:** "The remaining leaf hashes beyond the end of the file required to construct upper layers of the merkle tree are set to zero."

All Merkle trees must be padded to the next power of 2.

**⚠️ Critical:** Padding applies to **Merkle tree structure** (leaf node count), NOT block content:
- ✅ Pad Merkle tree leaf nodes (hashes) to power of 2
- ❌ Never pad block data before hashing (last block hashed as-is)

---

## Two-Layer Merkle Tree Architecture

**Important:** In all Merkle trees, **leaf nodes are hashes**, not raw data:
- Per-piece tree: Leaves are `SHA256(block_data)`, not raw block data
- File-level tree: Leaves are piece root hashes (32 bytes), not pieces themselves

### Layer 1: Per-Piece Merkle Trees (Block → Piece Root)

Each piece is divided into 16KB blocks, forming a Merkle tree:

```
Piece size = 16MB
Blocks per piece = 16MB / 16KB = 1024 = 2^10

Raw blocks:          [Block0][Block1]...[Block1023]  (1024 × 16KB data)
                         ↓       ↓            ↓       (SHA256 each block)

Merkle tree:
Layer 0 (leaf):      [H0][H1][H2]...[H1023]          (1024 block hashes)
                       \  /  \  /      \  /
Layer 1:               [H'0][H'1]...[H'511]          (512 parent hashes)
                         \  /          \  /
Layer 2:                 [H''0]...[H''255]           (256 parent hashes)
                          ...
Layer 10 (root):         [Piece Root]                (1 hash)

Where: Hi = SHA256(Block i data), i ∈ [0, 1023]
```

**Key Property:** Since piece size is power of 2, blocks per piece is also power of 2:
```
blocks_per_piece = piece_size / 16KB = 2^n / 2^14 = 2^(n-14)
```

**Result:** Perfect binary tree, no odd nodes! ✅

### Layer 2: File-Level Merkle Tree (Piece Roots → File Root)

All piece roots form a file-level Merkle tree:

```
File with 5 pieces (80MB):

Layer 0 (leaf):  [P0][P1][P2][P3][P4][Z][Z][Z]  (8 piece root hashes)
                  \  /  \  /  \  /  \ /
Layer 1:          [Q0] [Q1] [Q2] [Q3]            (4 parent hashes)
                    \   /     \   /
Layer 2:            [R0]     [R1]                (2 parent hashes)
                      \       /
Layer 3:             [File Root]                 (1 hash)

Where:
- P0..P4: Actual piece root hashes (32 bytes each)
- Z: Zero piece root = merkle_root(zero_piece)
- Padded from 5 → 8 (next power of 2)
- All hashes are SHA256 (32 bytes)
```

---

## Padding Strategy

**Critical Distinction:**
1. ❌ **Block content padding**: Never pad block data before hashing
2. ✅ **Merkle tree padding**: Pad leaf nodes (hashes) to power of 2

### Block-Level Padding (Within Incomplete Piece)

**Scenario:** Last piece only 10MB + 8KB = 640 full blocks + 1 partial block

```
Step 1: Hash actual blocks (NO content padding!)
  Block 0-639: Each 16KB
    H0 = SHA256(Block 0, 16KB)
    ...
    H639 = SHA256(Block 639, 16KB)

  Block 640: Only 8KB (last block, truncated)
    H640 = SHA256(Block 640, 8KB)  ← Hash actual length, NO padding!

Step 2: Merkle tree padding (hash level)
  Actual block hashes: 641
  Next power of 2: 1024

  Layer 0: [H0][H1]...[H640][H_pad][H_pad]...[H_pad]
                             └─ 383 padding hashes (virtual)

Where:
- H0..H640: Real block hashes (641 hashes)
- H_pad: Pre-computed SHA256(zeros(16KB)) (383 virtual hashes)
- NO actual zero blocks exist in memory!
```

**Result:** Merkle tree has 1024 leaf nodes → perfect binary tree

**Important:**
- ⚠️ The last block (8KB) is hashed as-is, NOT padded to 16KB
- ⚠️ Padding happens at Merkle tree level (hashes), not data level
- ⚠️ "Padding hashes" are pre-computed constants, not real data

### Piece-Level Padding (File-Level Tree)

**Scenario:** File with 5 pieces

```
Actual pieces: 5
Next power of 2: 8 = 2^3

Padding:
[P0][P1][P2][P3][P4][pad][pad][pad]
                     └─ 3 padding pieces

Pad value:
pad_piece = merkle_root(zero_piece)
```

**Critical:** Padding piece is **NOT** `SHA256(16MB zeros)`!

---

## Zero Piece Root Calculation

### Problem: What is "Zero Piece Root"?

A zero piece is a piece where all blocks are zero:
```
Zero piece (16MB):
  Block 0: 16KB zeros
  Block 1: 16KB zeros
  ...
  Block 1023: 16KB zeros
```

### Naive Calculation (Inefficient)

```c
sha256_hash compute_zero_piece_root_naive(int piece_size) {
    int blocks = piece_size / 16384;

    // Step 1: Compute zero block hash (hash 16KB of zeros)
    uint8_t zero_block_data[16384] = {0};
    sha256_hash zero_block_hash = SHA256(zero_block_data, 16384);

    // Step 2: All blocks in zero piece have same hash
    sha256_hash *hashes = malloc(blocks * sizeof(sha256_hash));
    for (int i = 0; i < blocks; i++) {
        hashes[i] = zero_block_hash;  // All identical!
    }

    // Step 3: Build Merkle tree (all nodes identical at each layer)
    return build_merkle_tree(hashes, blocks);
}
```

### Optimized Calculation (libtorrent's merkle_pad)

**Key Insight:** All blocks are identical → all hashes at each layer are identical!

```c
sha256_hash compute_zero_piece_root(int piece_size) {
    int blocks_per_piece = piece_size / 16384;
    int layers = log2(blocks_per_piece);  // e.g., 1024 → 10 layers

    // Step 1: Zero block hash
    uint8_t zero_block[16384] = {0};
    sha256_hash h;
    SHA256(zero_block, 16384, h.data());

    // Step 2: Build tree upward (hash self twice at each layer)
    for (int i = 0; i < layers; i++) {
        uint8_t combined[64];
        memcpy(combined, h.data(), 32);
        memcpy(combined + 32, h.data(), 32);
        SHA256(combined, 64, h.data());  // h = SHA256(h || h)
    }

    return h;  // Zero piece root
}
```

**Example for 16MB piece:**
```
Raw data:    All 1024 blocks are 16KB zeros

Layer 0:     H_block = SHA256(zeros(16KB))
             [H_block][H_block]...[H_block]  (1024 identical hashes)

Layer 1:     H_L1 = SHA256(H_block || H_block)
             [H_L1][H_L1]...[H_L1]  (512 identical hashes)

Layer 2:     H_L2 = SHA256(H_L1 || H_L1)
             [H_L2][H_L2]...[H_L2]  (256 identical hashes)
...
Layer 10:    H_L10 = SHA256(H_L9 || H_L9)
             [H_L10]  ← Zero piece root for 16MB
```

### Pre-computed Constants

```c
// Compute once, use everywhere
const sha256_hash ZERO_PIECE_16MB = compute_zero_piece_root(16 * 1024 * 1024);
const sha256_hash ZERO_PIECE_32MB = compute_zero_piece_root(32 * 1024 * 1024);
const sha256_hash ZERO_PIECE_256KB = compute_zero_piece_root(256 * 1024);
```

---

## Implementation

### Step 1: Compute Piece Root

```c
sha256_hash compute_piece_root(uint8_t *piece_data, int piece_size) {
    // Calculate number of blocks (including partial last block)
    int full_blocks = piece_size / 16384;
    int remainder = piece_size % 16384;
    int actual_blocks = full_blocks + (remainder > 0 ? 1 : 0);

    int padded_blocks = next_power_of_2(actual_blocks);

    sha256_hash *hashes = malloc(padded_blocks * sizeof(sha256_hash));

    // 1. Hash full blocks (16KB each)
    for (int i = 0; i < full_blocks; i++) {
        SHA256(piece_data + i * 16384, 16384, hashes[i].data());
    }

    // 2. Hash last block (may be < 16KB)
    if (remainder > 0) {
        // ⚠️ Hash actual length, DO NOT pad to 16KB!
        SHA256(piece_data + full_blocks * 16384, remainder, hashes[full_blocks].data());
    }

    // 3. Pad Merkle tree with zero block hashes (virtual)
    sha256_hash zero_block_hash;
    uint8_t zero_block[16384] = {0};
    SHA256(zero_block, 16384, zero_block_hash.data());

    for (int i = actual_blocks; i < padded_blocks; i++) {
        hashes[i] = zero_block_hash;  // Pre-computed constant
    }

    // 4. Build Merkle tree
    sha256_hash root = build_merkle_tree(hashes, padded_blocks);

    free(hashes);
    return root;
}
```

### Step 2: Compute File Root

```c
sha256_hash compute_file_root(
    sha256_hash *piece_roots,
    int num_pieces,
    int piece_size
) {
    int padded_pieces = next_power_of_2(num_pieces);

    sha256_hash *file_tree = malloc(padded_pieces * sizeof(sha256_hash));

    // 1. Copy actual piece roots
    for (int i = 0; i < num_pieces; i++) {
        file_tree[i] = piece_roots[i];
    }

    // 2. Pad with zero piece roots
    sha256_hash zero_piece = compute_zero_piece_root(piece_size);

    for (int i = num_pieces; i < padded_pieces; i++) {
        file_tree[i] = zero_piece;  // Use pre-computed if possible
    }

    // 3. Build Merkle tree
    sha256_hash root = build_merkle_tree(file_tree, padded_pieces);

    free(file_tree);
    return root;
}
```

### Step 3: Build Merkle Tree (Generic)

```c
sha256_hash build_merkle_tree(sha256_hash *hashes, int count) {
    // Assumption: count is power of 2
    assert((count & (count - 1)) == 0);

    while (count > 1) {
        for (int i = 0; i < count / 2; i++) {
            uint8_t combined[64];
            memcpy(combined, hashes[i * 2].data(), 32);
            memcpy(combined + 32, hashes[i * 2 + 1].data(), 32);
            SHA256(combined, 64, hashes[i].data());
        }
        count /= 2;
    }

    return hashes[0];
}
```

---

## Complete Example: 80MB File with 16MB Pieces

### Input
```
File size: 80MB
Piece size: 16MB
Number of pieces: 5 (piece 0-4 are 16MB each)
```

### Step 1: Compute Each Piece Root

```
Piece 0 (16MB):
  Blocks: 1024 (all used)
  Padding: 0
  Root: compute_piece_root(piece_0_data, 16MB)

Piece 1-3: Same as Piece 0

Piece 4 (16MB):
  Blocks: 1024 (all used)
  Padding: 0
  Root: compute_piece_root(piece_4_data, 16MB)
```

**Result:**
```
piece_roots[0] = <hash of piece 0>
piece_roots[1] = <hash of piece 1>
piece_roots[2] = <hash of piece 2>
piece_roots[3] = <hash of piece 3>
piece_roots[4] = <hash of piece 4>
```

### Step 2: Compute File Root

```
Actual pieces: 5
Padded pieces: 8 (next power of 2)

File-level Merkle tree:
Layer 0 (leaf): [P0][P1][P2][P3][P4][Z][Z][Z]  (8 piece root hashes)
                 └─ Actual piece roots ─┘ └ Padding ┘
                  \  /  \  /  \  /  \ /
Layer 1:          [Q0] [Q1] [Q2] [Q3]            (4 parent hashes)
                    \   /     \   /
Layer 2:            [R0]     [R1]                (2 parent hashes)
                      \       /
Layer 3:              [File Root]                (1 hash)

Where:
- P0..P4: Actual piece root hashes (from Step 1)
- Z: ZERO_PIECE_16MB = merkle_root(zero piece)
- Q0 = SHA256(P0 || P1), Q1 = SHA256(P2 || P3), etc.
```

### Torrent Entry

```python
{
    'info': {
        'name': 'disk_image',
        'piece length': 16777216,  # 16MB
        'meta version': 2,
        'file tree': {
            '0000000000000000': {  # File at offset 0
                'length': 83886080,  # 80MB
                'pieces root': <File Root from Step 2>
            }
        }
    }
}
```

---

## Detailed Example: Piece with Truncated Last Block

### Scenario
```
Piece size: 10MB + 8KB = 10493952 bytes
Block size: 16KB = 16384 bytes
Full blocks: 640 (10MB / 16KB)
Last block: 8KB (10493952 % 16384 = 8192 bytes)
Total blocks: 641
```

### Step-by-Step Computation

```c
// Step 1: Hash full blocks (640 blocks, each 16KB)
for (int i = 0; i < 640; i++) {
    hashes[i] = SHA256(piece_data + i * 16384, 16384);
}

// Step 2: Hash last block (8KB, NOT 16KB!)
hashes[640] = SHA256(piece_data + 640 * 16384, 8192);  // ⚠️ Only 8KB!

// Step 3: Merkle tree padding
// Actual hashes: 641
// Next power of 2: 1024
sha256_hash zero_block_hash = SHA256(zeros(16384), 16384);
for (int i = 641; i < 1024; i++) {
    hashes[i] = zero_block_hash;  // Virtual padding hashes
}

// Step 4: Build tree
sha256_hash piece_root = build_merkle_tree(hashes, 1024);
```

### Visualization

```
Physical data:
  Blocks 0-639: [16KB][16KB]...[16KB]  (640 × 16KB = 10MB)
  Block 640:    [8KB]                  (last block, truncated)

Layer 0 (Merkle tree leaves):
  [H0][H1]...[H639][H640][H_pad][H_pad]...[H_pad]
                   └─ 8KB  └─ 383 virtual padding hashes

Where:
  H0..H639: SHA256(16KB block data)  (640 hashes)
  H640:     SHA256(8KB block data)   (1 hash)  ← Special case!
  H_pad:    SHA256(16KB zeros)       (383 identical hashes)
```

**Key Points:**
1. ✅ Block 640 (8KB) is hashed as-is: `SHA256(8KB data)`
2. ❌ Never do: `SHA256([8KB data + 8KB zeros])`
3. ✅ Merkle tree padded from 641 → 1024 leaf nodes
4. ✅ All padding hashes are pre-computed constant

---

## Comparison: Piece Tree vs File Tree Padding

| Aspect | Piece Tree | File Tree |
|--------|-----------|-----------|
| **Leaf Type** | 16KB blocks | Piece roots (32-byte hashes) |
| **Leaf Size** | 16KB data | 32 bytes hash |
| **Padding Value** | `SHA256(16KB zeros)` | `merkle_root(zero_piece)` |
| **Why Different?** | Padding a block | Padding a piece |
| **Pre-computable?** | Yes (1 value) | Yes (per piece size) |

**Critical Difference:**
- Block padding: `SHA256(zeros(16KB))`
- Piece padding: `merkle_root(1024 × SHA256(zeros(16KB)))`

---

## Odd Node Handling in BEP 52

### Question: How does BEP 52 handle odd nodes?

**Answer: It doesn't need to!**

With mandatory padding to power of 2:
- All trees are **perfect binary trees**
- Every layer has even number of nodes (until root)
- No odd-node problem exists

### Exception: Before Padding

Before padding, odd nodes may exist:
- 5 pieces → 3 nodes at layer 1 → 2 nodes at layer 2 → 1 root
- 640 blocks → 320 nodes → 160 → 80 → 40 → 20 → 10 → 5 (odd!) → 3 (odd!) → 2 → 1

**Solution: Pad before building tree**
- 5 pieces → pad to 8 → perfect tree
- 640 blocks → pad to 1024 → perfect tree

### libtorrent's Approach: Pad Hash Pairing

If implementing without pre-padding, libtorrent uses **pad hash pairing**:

```c
// If odd number of nodes at current layer
if (nodes & 1) {
    // Last node pairs with pad hash (not itself)
    parent = SHA256(last_node || pad_hash);

    // Update pad hash for next layer
    pad_hash = SHA256(pad_hash || pad_hash);
}
```

This is equivalent to padding before building tree.

---

## EZIO Implementation Notes

### Raw Disk Considerations

EZIO operates on raw disk (`/dev/sda1`), not filesystem:
- Partclone scans used blocks
- Generates torrent with offset-based "files"
- Each file = continuous used region

### Partclone Integration: Two-Phase Architecture

EZIO uses a **two-phase approach** for BitTorrent v2 integration:

**Phase 1 (Partclone - C):**
- Scan filesystem for used blocks
- Compute SHA256 for each 16KB block
- Split regions into 2GB files
- Output `.blockinfo` file with all block hashes

**Phase 2 (Python script):**
- Read `.blockinfo` file
- Build piece Merkle trees (per region)
- Build file Merkle trees (per region)
- Generate `.torrent` file (BEP 52 format)

**Architecture diagram:**
```
Disk (/dev/sda1)
    ↓ Partclone scans
[used: 0-3GB] [unused] [used: 5-7GB]
    ↓ Split into 2GB regions
Region 1: 0GB-2GB  → SHA256 all blocks → blockinfo
Region 2: 2GB-3GB  → SHA256 all blocks → blockinfo
Region 3: 5GB-7GB  → SHA256 all blocks → blockinfo
    ↓ Python reads blockinfo
Build Merkle trees → Generate .torrent
```

#### Why 2GB File Size Limit?

**Problem without limit:**
```
100GB continuous region:
  → 6,553,600 blocks × 32 bytes = 200 MB memory peak! ⚠️
  → 6,400 piece roots
  → Difficult to manage
```

**Solution with 2GB limit:**
```
100GB continuous region:
  → Split into 50 files (2GB each)
  → Per-file: 131,072 blocks × 32 bytes = 4 MB ✅
  → Per-file: 128 pieces (16MB piece size)
  → Memory bounded, parallelizable
```

**Benefits:**
- ✅ Memory bounded (max 4MB per file)
- ✅ Parallel processing friendly
- ✅ Reasonable download granularity
- ✅ FAT32 compatible (< 4GB)

#### Splitting Logic

```c
#define MAX_FILE_SIZE (2ULL * 1024 * 1024 * 1024)  // 2GB

while (scanning_filesystem) {
    if (is_block_used()) {
        // Hash and accumulate block
        SHA256(buffer, BLOCK_SIZE, hash);
        add_block_to_region(hash);
        current_region.length += BLOCK_SIZE;

        // Split if size limit reached (even if continuous!)
        if (current_region.length >= MAX_FILE_SIZE) {
            output_region_info(&current_region);
            start_new_region(current_disk_offset);
        }
    } else {
        // Split if unused block encountered
        if (current_region.length > 0) {
            output_region_info(&current_region);
        }
    }
}
```

#### blockinfo File Format

**Binary format:**
```
Header (16 bytes):
  [Magic: "EZIO" (4 bytes)]
  [Version: 2 (4 bytes)]
  [Block size: 16384 (4 bytes)]
  [Number of regions: N (4 bytes)]

For each region:
  [Offset: 8 bytes]              # Disk offset
  [Length: 8 bytes]              # Region size
  [Number of blocks: 4 bytes]    # Block count
  [Block hashes: N × 32 bytes]   # SHA256 hashes
```

**Example:**
```c
// Write region to blockinfo
void output_region_info(region_info_t *region) {
    fwrite(&region->offset, 8, 1, fp);
    fwrite(&region->length, 8, 1, fp);
    fwrite(&region->num_blocks, 4, 1, fp);
    for (uint32_t i = 0; i < region->num_blocks; i++) {
        fwrite(region->block_hashes[i], 32, 1, fp);
    }
}
```

#### Complete Partclone Integration

```c
typedef struct {
    uint64_t offset;
    uint64_t length;
    uint32_t num_blocks;
    uint8_t **block_hashes;
} region_info_t;

FILE *blockinfo_fp;
region_info_t current_region;

void partclone_scan_v2(const char *device) {
    blockinfo_fp = fopen("image.blockinfo", "wb");

    // Write header
    fwrite("EZIO", 4, 1, blockinfo_fp);
    uint32_t version = 2, block_size = 16384, num_regions = 0;
    fwrite(&version, 4, 1, blockinfo_fp);
    fwrite(&block_size, 4, 1, blockinfo_fp);
    long region_count_pos = ftell(blockinfo_fp);
    fwrite(&num_regions, 4, 1, blockinfo_fp);

    // Scan disk
    uint64_t disk_offset = 0;
    bool region_active = false;

    while (scanning) {
        if (is_block_used()) {
            if (!region_active) {
                start_new_region(disk_offset);
                region_active = true;
            }

            // Compute SHA256
            uint8_t hash[32];
            SHA256(block_data, BLOCK_SIZE, hash);
            add_block_to_region(hash);

            // Check 2GB limit
            if (current_region.length >= MAX_FILE_SIZE) {
                output_region_info(&current_region);
                num_regions++;
                region_active = false;
            }
        } else {
            if (region_active) {
                output_region_info(&current_region);
                num_regions++;
                region_active = false;
            }
        }
        disk_offset += BLOCK_SIZE;
    }

    // Finalize
    if (region_active) {
        output_region_info(&current_region);
        num_regions++;
    }

    // Update region count in header
    fseek(blockinfo_fp, region_count_pos, SEEK_SET);
    fwrite(&num_regions, 4, 1, blockinfo_fp);
    fclose(blockinfo_fp);
}
```

#### Python Torrent Generator

```python
#!/usr/bin/env python3
import struct
import hashlib

def load_blockinfo(filename):
    """Parse .blockinfo file"""
    with open(filename, 'rb') as f:
        magic = f.read(4)
        assert magic == b'EZIO'
        version, block_size, num_regions = struct.unpack('<III', f.read(12))

        regions = []
        for _ in range(num_regions):
            offset, length, num_blocks = struct.unpack('<QQI', f.read(20))
            block_hashes = [f.read(32) for _ in range(num_blocks)]
            regions.append((offset, length, block_hashes))

        return regions

def build_merkle_tree(hashes):
    """Build Merkle tree, pad to power of 2"""
    # Pad to power of 2
    target = 1
    while target < len(hashes):
        target *= 2
    zero_hash = hashlib.sha256(b'\x00' * 16384).digest()
    hashes = hashes + [zero_hash] * (target - len(hashes))

    # Build tree
    while len(hashes) > 1:
        next_layer = []
        for i in range(0, len(hashes), 2):
            parent = hashlib.sha256(hashes[i] + hashes[i+1]).digest()
            next_layer.append(parent)
        hashes = next_layer
    return hashes[0]

def create_torrent_v2(blockinfo_file, piece_size):
    """Generate BEP 52 torrent"""
    regions = load_blockinfo(blockinfo_file)
    blocks_per_piece = piece_size // 16384

    torrent_files = []
    for offset, length, block_hashes in regions:
        # Build piece roots
        piece_roots = []
        for i in range(0, len(block_hashes), blocks_per_piece):
            piece_blocks = block_hashes[i:i+blocks_per_piece]
            # Pad piece to blocks_per_piece
            zero_hash = hashlib.sha256(b'\x00' * 16384).digest()
            while len(piece_blocks) < blocks_per_piece:
                piece_blocks.append(zero_hash)
            piece_root = build_merkle_tree(piece_blocks)
            piece_roots.append(piece_root)

        # Build file root
        file_root = build_merkle_tree(piece_roots)

        torrent_files.append({
            'path': [f'{offset:016x}'],
            'length': length,
            'pieces root': file_root
        })

    # Generate torrent structure...
    return torrent_files

# Usage:
# $ sudo partclone.ext4 -c -s /dev/sda1 -O image/ --bt-v2
# $ ./create_torrent_v2.py image.blockinfo -o image.torrent
```

### File Boundaries in EZIO

Each continuous used region becomes a "file":
```python
{
    'file tree': {
        '0000000000000000': {  # Offset 0, 24MB used
            'length': 25165824,
            'pieces root': compute_file_root(...)
        },
        '.pad': {
            '8388608': {  # 8MB padding (24MB → 32MB)
                'length': 8388608,
                'attr': 'p'
            }
        },
        '0000000002000000': {  # Offset 32MB, 16MB used
            'length': 16777216,
            'pieces root': compute_file_root(...)
        }
        # No padding needed (already 16MB)
    }
}
```

---

## Performance Optimizations

### 1. Pre-compute Zero Hashes

```c
// Global constants (compute once at startup)
const sha256_hash ZERO_BLOCK_HASH = SHA256(zeros(16KB));
const sha256_hash ZERO_PIECE_16MB = compute_zero_piece_root(16 * 1024 * 1024);
const sha256_hash ZERO_PIECE_32MB = compute_zero_piece_root(32 * 1024 * 1024);
```

### 2. Memory Efficiency

Instead of allocating full padded array:
```c
// Only allocate actual blocks, compute padding on-the-fly
sha256_hash *hashes = malloc(actual_blocks * sizeof(sha256_hash));
// During tree building, use ZERO_BLOCK_HASH for padding indices
```

### 3. Parallel Piece Processing

Piece roots are independent:
```c
#pragma omp parallel for
for (int p = 0; p < num_pieces; p++) {
    piece_roots[p] = compute_piece_root(piece_data[p], piece_size);
}
```

---

## References

### Specifications
- **BEP 52:** https://www.bittorrent.org/beps/bep_0052.html
- **BEP 47:** Padding files (https://www.bittorrent.org/beps/bep_0047.html)

### Implementation Reference
- **libtorrent 2.0.10:** `src/merkle.cpp`
  - `merkle_root_scratch()`: Main Merkle tree computation
  - `merkle_pad()`: Compute pad hash for given layer
  - `merkle_num_leafs()`: Round up to power of 2

### Related Documents
- `docs/SESSION_MEMORY.md`: EZIO development history
- `docs/CLAUDE.md`: Architecture overview
- `README.md`: User-facing documentation

---

## Appendix: Helper Functions

### next_power_of_2()

```c
int next_power_of_2(int n) {
    if (n <= 0) return 1;
    if ((n & (n - 1)) == 0) return n;  // Already power of 2

    int power = 1;
    while (power < n) power <<= 1;
    return power;
}
```

### log2_int()

```c
int log2_int(int n) {
    // Assumes n is power of 2
    assert((n & (n - 1)) == 0);

    int log = 0;
    while (n > 1) {
        n >>= 1;
        log++;
    }
    return log;
}
```

### hash_to_hex()

```c
void hash_to_hex(sha256_hash hash, char *output) {
    for (int i = 0; i < 32; i++) {
        sprintf(output + i * 2, "%02x", hash.data()[i]);
    }
    output[64] = '\0';
}
```

---

**Document Status:** Complete
**Implementation Status:** Design phase
**Next Steps:** Implement async_hash2 in raw_disk_io.cpp following this specification

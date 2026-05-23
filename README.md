# EZIO

![build test](https://github.com/tjjh89017/ezio/actions/workflows/github_actions.yml/badge.svg)

**EZIO is a high-performance disk imaging tool for rapidly deploying dozens to hundreds of machines over a LAN.** It distributes disk images peer-to-peer with the BitTorrent protocol and writes directly to raw disk partitions, achieving far faster deployment than traditional multicast — and it gets *faster* as you add more clients, because every client also seeds.

It uses [`partclone`](http://partclone.org/) to capture only the used filesystem blocks, so images stay small without losing fidelity.

> **Just want to deploy?** EZIO ships inside **Clonezilla** as its **Lite Server Mode** (since Clonezilla 2.6.0-31). That is the easiest way to use EZIO — no manual build required. See [Easiest Path: Clonezilla](#easiest-path-clonezilla).

---

## Table of Contents

- [Quick Start](#quick-start)
- [Installation](#installation)
- [Usage](#usage)
- [Configuration & Performance Tuning](#configuration--performance-tuning)
- [How It Works](#how-it-works)
- [Benchmark](#benchmark)
- [Limitations](#limitations)
- [Publications](#publications)
- [Contributing & Support](#contributing--support)
- [License](#license)

---

## Quick Start

### Easiest Path: Clonezilla

Use **Clonezilla Live (>= 2.6.0-31)** and pick **Lite Server Mode**. EZIO is built in and driven through Clonezilla's menus — no compilation, no manual torrent handling. Recommended for most users.

### Manual Path (build from source)

The full workflow is: **build EZIO -> capture an image with partclone -> create a torrent -> seed on the source -> download on each target.**

```shell
# 1. Build (see Installation for dependencies)
mkdir build && cd build && cmake ../ && make && cd ..

# 2. Capture a partition into a partclone image + torrent.info
sudo partclone.extfs -c -T -s /dev/sda1 -O image_dir/ --buffer_size 16777216

# 3. Turn it into a torrent
utils/partclone_create_torrent.py \
    -c CloneZilla -p sda1 \
    -i image_dir/torrent.info -o sda1.torrent \
    -t 'http://<tracker-host>:6969/announce'

# 4. Generate the gRPC Python stubs once (needed by the utils/ scripts)
cd utils && ./ezio_create_proto_py.sh && cd ..

# --- On the SOURCE machine: seed the disk ---
sudo ./build/ezio                                   # start the daemon
utils/ezio_add_torrent.py -S sda1.torrent /dev/sda1 # -S = seeding mode

# --- On each TARGET machine: clone to disk ---
sudo ./build/ezio                                   # start the daemon
utils/ezio_add_torrent.py sda1.torrent /dev/sda1    # download + write to disk
```

That's it. As targets download they automatically seed to each other. Monitor progress with `utils/ezio_cli.py` or the TUI `utils/ezio_ui.py`.

> **Note:** `ezio` runs in the foreground and blocks until it is shut down, so the `ezio_add_torrent.py` / monitoring commands must be run from a *separate* terminal. Stop the daemon with `utils/ezio_shutdown.py` (or let `ezio_ui` auto-exit once seeding is done).

> **Tip:** `ezio` writes directly to raw devices, so it needs root (or CAP_DAC_OVERRIDE) for `/dev/sdX` access.

---

## Installation

### Requirements

- 64-bit Linux (Debian 11+ recommended)
- 2 GB RAM (~1.5 GB for EZIO at default settings + OS overhead)
- Root privileges for raw disk access

### Dependencies

- libtorrent-rasterbar >= 2.0.8
- libboost >= 1.74
- cmake >= 3.16
- spdlog (logging)
- gRPC (control interface)
- clang-format (development only)

```shell
sudo apt install build-essential cmake libboost-all-dev \
    libtorrent-rasterbar-dev libgrpc-dev libgrpc++-dev \
    libprotobuf-dev protobuf-compiler-grpc libspdlog-dev clang-format
```

Python helper scripts in `utils/` need:

```shell
pip install -r utils/requirements.txt
```

### Build

```shell
mkdir build && cd build
cmake ../
make
```

This produces the `ezio` binary in `build/`; the examples below run it as `./build/ezio`.

### Docker

```shell
docker build . -t ezio-latest-img
```

### For developers

Run clang-format before committing:

```shell
find . -maxdepth 1 -name "*.cpp" -o -name "*.hpp" | grep -v "./tmp/" | xargs clang-format -i
```

---

## Usage

### The `ezio` daemon

`ezio` is the disk I/O daemon. It exposes a gRPC control interface; you add and manage torrents through the `utils/` scripts. It runs in the foreground until shut down, so start it in its own terminal and run the scripts below from another.

```
Allowed Options:
  -h [ --help ]       show help
  -F [ --file ]       read/write a regular file instead of a raw disk
  -l [ --listen ] arg gRPC listen address:port (default 127.0.0.1:50051)
  --cache-size arg    unified cache size in MB (default 512)
  --aio-threads arg   threads for disk I/O and hashing (default 16)
  -v [ --version ]    show version
```

### Generate gRPC stubs (once)

The Python scripts need generated protobuf stubs:

```shell
cd utils && ./ezio_create_proto_py.sh && cd ..
```

### Creating a torrent

Capture a partition with partclone (this also emits `torrent.info`):

```shell
# -T also writes the BT image; use -t if you only want torrent.info
sudo partclone.extfs -c -T -s /dev/sda1 -O image_dir/ --buffer_size 16777216
```

Then build the `.torrent`:

```shell
utils/partclone_create_torrent.py \
    -c CloneZilla -p sda1 \
    -i image_dir/torrent.info -o sda1.torrent \
    -t 'http://<tracker-host>:6969/announce'
```

### Seeding and downloading

A single script, `ezio_add_torrent.py`, handles both. Add `-S` to seed.

```shell
# Seed an existing disk (source machine)
sudo ./build/ezio
utils/ezio_add_torrent.py -S sda1.torrent /dev/sda1

# Seed from a saved image file instead of a raw disk
sudo ./build/ezio -F
utils/ezio_add_torrent.py -S sda1.torrent /path/to/sda1.img

# Download and write to disk (target machine)
sudo ./build/ezio
utils/ezio_add_torrent.py sda1.torrent /dev/sda1

# Download to a file (proxy / save the image)
sudo ./build/ezio -F
utils/ezio_add_torrent.py sda1.torrent /path/to/save/sda1.img
```

`ezio_add_torrent.py` options:

```
  torrent                 torrent file path
  path                    destination disk or file
  -a, --address           gRPC server (default 127.0.0.1:50051)
  -S, --seeding           start in seeding mode
  -s, --sequential        sequential download
  -c, --connections       max total connections (default 3)
  -u, --uploads           max upload connections (default 2)
```

### Monitoring & control

```shell
utils/ezio_cli.py       # status / progress in the terminal
utils/ezio_ui.py        # full TUI (sorting, filtering, color, help via 'h')
utils/ezio_pause_torrent.py / ezio_resume_torrent.py / ezio_shutdown.py
```

### Deploying over the Internet

For WAN or bottlenecked links, proxy the torrent through a regular BitTorrent client such as [qBittorrent](https://www.qbittorrent.org/), and keep internal peers from connecting directly to outside peers.

---

## Configuration & Performance Tuning

### Log level

Control verbosity at runtime with `SPDLOG_LEVEL`:

```shell
SPDLOG_LEVEL=debug ./ezio                    # everything
SPDLOG_LEVEL=warn ./ezio                     # warnings + errors
SPDLOG_LEVEL=info,raw_disk_io=debug ./ezio   # per-component
```

Levels: `trace`, `debug`, `info` (default), `warn`, `error`, `critical`, `off`.

### Threads and cache

| Option | Default | Purpose |
|--------|---------|---------|
| `--aio-threads` | 16 | Threads for disk I/O **and** SHA-1 hashing |
| `--cache-size` | 512 | Lock-free unified cache size, MB |

Memory budget: cache (`--cache-size`, default 512 MB) + fixed buffer pools (512 MB read + 512 MB write) = ~1.5 GB by default.

### Sizing the cache

A larger cache improves the hit rate (seeders re-serve hot pieces from RAM instead of re-reading the disk), but EZIO must coexist with the OS and the running deployment. A good rule of thumb is to set `--cache-size` to about **1/4 of system RAM**, leaving the rest for buffer pools, the OS page cache, and headroom.

| System RAM | Suggested `--cache-size` |
|-----------:|-------------------------:|
| 4 GB | 1024 (1 GB) |
| 8 GB | 2048 (2 GB) |
| 16 GB | 4096 (4 GB) |
| 32 GB | 8192 (8 GB) |

```shell
./ezio --cache-size 4096   # ~16 GB machine
```

Remember the two 512 MB buffer pools (~1 GB) are always allocated on top of the cache, so keep total EZIO usage comfortably below available RAM.

### Tuning by storage type

```shell
./ezio --aio-threads 2     # HDD: fewer threads, less seek thrash
./ezio                     # SATA SSD: default 16 is fine
./ezio --aio-threads 32    # NVMe: high parallelism, can saturate 10GbE
```

Combine options as needed:

```shell
./ezio --aio-threads 32 --cache-size 1024 --listen 0.0.0.0:50051
```

Monitor with `iostat`/`iotop` (disk) and `iftop`/`nload` (network). Homogeneous deployments (all the same disk type) perform best.

---

## How It Works

### Direct raw disk I/O

EZIO implements a custom libtorrent [disk I/O interface](http://libtorrent.org/reference-Custom_Storage.html#overview) in `raw_disk_io.cpp/.hpp` that reads/writes `/dev/sdX` partitions directly, skipping the filesystem layer. Block offsets are computed on the fly, so EZIO streams data straight to the target disk — no need to buffer the whole image in RAM or stage it in temporary storage first. Images of any size deploy without extra disk space.

### Lock-free unified cache

A 512 MB (configurable) write-through cache with a **zero-mutex** design:

- **Consistent hashing** `hash(storage_index, piece_index) % aio_threads` routes every operation on a piece to the same I/O thread, guaranteeing a piece's `async_read` runs after any pending `async_write` for it.
- **1:1 thread-to-partition mapping** — each thread exclusively owns one cache partition, so no locking is needed. This removes the temporary `store_buffer` used by stock libtorrent.

### Torrent format

The disk offset (hex) is stored as the file path and the length as the file size, so BitTorrent can seek directly to the right disk position:

```
{
  'announce': 'http://tracker.example.com/announce',
  'info': {
    'name': 'root',
    'piece length': 262144,
    'files': [
      {'path': ['0000000000000000'], 'length': 4096},   // offset + length
      {'path': ['0000000000020000'], 'length': 8192},
      ...
    ],
    'pieces': '...'
  }
}
```

### Why BitTorrent beats multicast

Traditional multicast must synchronize all clients before starting, retransmits to the whole group on any single failure, and lets one broken client stall the deployment. With BitTorrent, clients seed as they download (load spreads across peers), failures cost only a single 16 KB block, and there's no global sync barrier. Supported filesystems include ext2/3/4, xfs, btrfs, f2fs, reiserfs, NTFS, FAT, HFS+, UFS and more; unsupported ones fall back to sector-by-sector `dd`.

---

## Benchmark

### Scaling vs. Clonezilla multicast

EZIO vs. Clonezilla multicast. Lower is better; EZIO pulls ahead as clients scale.

- Network: Cisco 3560G
- Server & clients: Dell T1700, Xeon E3-1226, 16 GB RAM, 1 TB HDD
- Image: Ubuntu Linux, 50 GB of data (multicast image compressed with `pzstd`, BT image is raw)

| Clients | Unicast (s) | EZIO (s) | Multicast (s) | EZIO / Multicast |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 474 | 675 | 390 | 1.731 |
| 2 | 948 | 1273 | 474 | 2.686 |
| 4 | 1896 | 1331 | 638 | 2.086 |
| 8 | 3792 | 1412 | 980 | 1.441 |
| 16 | 7584 | 1005 | 1454 | 0.691 |
| 24 | 11376 | 1048 | 1992 | 0.526 |
| 32 | 15168 | 1143 | 2203 | 0.519 |

At 16+ clients EZIO is roughly 2x faster than multicast and the gap keeps widening.

### Lab test — raw NVMe

A recent end-to-end run of the current build, deploying a real partclone image directly onto raw NVMe partitions across separate hosts on a LAN.

- 1 seeder + 1 or 3 leechers, each a separate Linux host
- Storage: ADATA SX8200 Pro NVMe SSD, raw partition `/dev/nvme0n1p1` (source and target)
- Image: 60.6 GiB partclone image (each leecher writes the full image)
- Settings: `--aio-threads 16`, prefetch 16 blocks; cache size as noted
- `blkdiscard` on each target between runs to avoid SLC-cache skew

Two metrics per leecher: **download speed** as reported by `ezio_ui` (network in), and **disk-write average** (bytes written to NVMe / elapsed). Download outruns disk-write because the cache absorbs network bursts ahead of the flush to disk.

| Scenario | Cache | Time | Download speed (median / peak) | Disk-write avg |
| --- | ---: | ---: | ---: | ---: |
| 1-on-1 | 512 MB | ~166 s | ~511 / 526 MB/s | ~374 MiB/s |
| 1-on-1 | 4 GB | ~116 s | ~836 / 874 MB/s | ~536 MiB/s |
| 1-to-3 | 512 MB | ~219 s | ~370 / 381 MB/s per leecher | ~283 MiB/s per leecher |
| 1-to-3 | 4 GB | ~210 s | ~375 / 444 MB/s per leecher | ~294 MiB/s per leecher |

A larger cache helps the **1-on-1** case most: once the seeder's cache is warm it serves the whole image from RAM, pushing download to ~840 MB/s and nearly halving the time. With 3 leechers the gain is small — the peers already offload the seeder by sharing pieces with each other, so the seeder reads only ~one image (~63 GiB) from disk per run regardless of leecher count, and aggregate delivered throughput (~1.1 GB/s across 3 leechers) scales with the number of peers.

> **Note:** These are small-scale lab figures and understate EZIO's real advantage. BitTorrent's strength is scale — the more clients you deploy to, the more they seed to each other and the further EZIO pulls ahead of unicast or multicast (see the scaling table above). With only 1-3 nodes you are seeing close to its worst case, not its best.

---

## Limitations

- Creating a torrent is slow: every piece must be SHA-1 hashed.
- EZIO is inefficient with very few clients — its strength is scale.
- Unsupported filesystems fall back to slower sector-by-sector `dd` copy.

---

## Roadmap

- **OpenStack Ironic integration** — bring peer-to-peer image distribution to
  bare-metal provisioning, building on and improving Mirantis' earlier work:
  [Cut Ironic Provisioning Time Using Torrents](https://web.archive.org/web/20211124125644/https://www.mirantis.com/blog/cut-ironic-provisioning-time-using-torrents/)
  (original blog post is now offline; link points to the Web Archive copy).

---

## Publications

- "A BitTorrent Mechanism-Based Solution for Massive System Deployment," *IEEE Access*, 2021. [IEEE Xplore](https://ieeexplore.ieee.org/document/9328243)
- "A Novel Massive Deployment Solution Based on the Peer-to-Peer Protocol," *Applied Sciences*, 2019. [MDPI](https://www.mdpi.com/2076-3417/9/2/296)

---

## Contributing & Support

- Issues: https://github.com/tjjh89017/ezio/issues
- Source: https://github.com/tjjh89017/ezio
- Maintainer email: tjjh89017@hotmail.com

**Special thanks** to the National Center for High-performance Computing (NCHC), Taiwan, for test hardware and support.

---

## License

GNU General Public License v2.0.

# EZIO Developer and User Guide

![build test](https://github.com/tjjh89017/ezio/actions/workflows/github_actions.yml/badge.svg)

## Introduction

EZIO is a high-performance disk imaging tool designed for rapid deployment of dozens to hundreds of machines in local area networks. By leveraging the BitTorrent protocol for peer-to-peer data distribution and direct raw disk I/O, EZIO achieves significantly faster deployment speeds compared to traditional multicast-based solutions. Using `partclone` to capture only used filesystem blocks, EZIO minimizes transfer size while maintaining full system fidelity.

**Note:** Clonezilla has integrated EZIO as its **Lite Server Mode** (available since version 2.6.0-31), making BitTorrent-based deployment accessible through Clonezilla's familiar interface. 

## Motivation

EZIO was inspired by Clonezilla for disk imaging and Resilio Sync (formerly BTsync) for peer-to-peer data distribution. While Clonezilla is widely used, its traditional multicast mode faced significant limitations in real-world deployments:

**Traditional Multicast Mode Limitations:**
- **Synchronization overhead**: All clients must register before deployment begins, causing long wait times
- **Failure amplification**: When a client fails to receive data correctly, the server must retransmit, consuming significant resources
- **Broken client problem**: Faulty machines repeatedly request retransmission until retry limits are exceeded, blocking deployment progress
- **No peer assistance**: Clients cannot help each other; all data flows from the server

**EZIO's BitTorrent Approach:**

By implementing the transfer layer on top of BitTorrent, EZIO transforms these weaknesses into strengths. Clients become seeders as they download, distributing load across the network. Failed transfers affect only individual pieces, not the entire deployment. The result is dramatically faster deployment times, especially as client count increases (see Benchmark section). This approach proved so successful that Clonezilla integrated EZIO as its Lite Server Mode. 

## Features

### Core Features

- **BitTorrent-powered distribution**: Peer-to-peer architecture scales efficiently with client count. Unlike multicast where adding clients increases load on the server, BitTorrent distributes load across all peers as they seed while downloading.

- **Block-level transfer**: Unlike file-level sync tools (e.g., Resilio Sync), EZIO transfers data in small blocks (16KB). When corruption occurs, only the affected block needs retransmission, not the entire file.

- **Direct raw disk I/O**: Custom libtorrent storage backend writes directly to `/dev/sdX` partitions without filesystem overhead, maximizing write performance.

- **No RAM or temporary storage constraints**: Unlike other BitTorrent-based imaging solutions, EZIO streams data directly to the target disk without intermediate buffering. Competing solutions typically require either (1) loading the entire image into RAM before writing, limiting image size to available memory, or (2) downloading to temporary storage first (e.g., qcow2 format) then converting to raw disk with tools like `qemu-img convert`, requiring 2× disk space. EZIO eliminates both constraints by calculating block offsets on-the-fly and can deploy images of any size without temporary storage.

- **Smart block capture**: Uses `partclone` to capture only used filesystem blocks, dramatically reducing image size and transfer time.

- **Broad filesystem support**:
  - **Linux**: ext2, ext3, ext4, reiserfs, reiser4, xfs, jfs, btrfs, f2fs, nilfs2
  - **Windows**: FAT12, FAT16, FAT32, NTFS
  - **macOS**: HFS+
  - **BSD**: UFS (FreeBSD, NetBSD, OpenBSD)
  - **Other**: Minix, VMFS3/VMFS5 (VMware ESX), Chrome OS/Chromium OS
  - Supports both 32-bit (x86) and 64-bit (x86-64) systems
  - For unsupported filesystems, falls back to sector-by-sector copy via dd

### Operational Features

- **gRPC control interface**: Programmatic control for automation and integration
- **Runtime log level control**: Adjust logging verbosity without recompilation via `SPDLOG_LEVEL` environment variable
- **Event-driven alerts**: Instant notification of errors and state changes using libtorrent's alert system
- **Configurable thread pools**: Tune disk I/O and hashing threads for different storage types (HDD/SSD/NVMe)
- **Unified buffer pool**: 256MB shared memory pool for efficient resource utilization 


# Installation

### Minimum System Requirements

- 64bit
- 1GB RAM

### Dependencies
- Debian 11 or above
- libtorrent-rasterbar>=2.0.8
- libboost>=1.74
- cmake>=3.16
- spdlog
- gRPC
```shell
sudo apt install build-essential cmake libboost-all-dev libtorrent-rasterbar-dev libgrpc-dev libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc libspdlog-dev
```

### Build and Install

```shell
mkdir build
cd build
cmake ../
make
sudo make install
```

We also provide a Dockerfile for the ease of installation and CI testing.
To build the image type this:

```shell
docker build . -t ezio-latest-img
```

## Runtime Configuration

### Log Level Control

EZIO supports runtime log level control via environment variables. You can adjust log verbosity without recompiling:

```shell
# Set global log level
export SPDLOG_LEVEL=debug    # Show all debug messages
export SPDLOG_LEVEL=info     # Default level
export SPDLOG_LEVEL=warn     # Only warnings and errors
export SPDLOG_LEVEL=error    # Only errors

# Component-specific log levels
export SPDLOG_LEVEL=info,raw_disk_io=debug    # Debug only raw_disk_io
export SPDLOG_LEVEL=warn,buffer_pool=info     # Different levels per component
```

**Available log levels** (from most to least verbose):
- `trace` - Very detailed debugging information
- `debug` - Debugging information
- `info` - Informational messages (default)
- `warn` - Warning messages
- `error` - Error messages
- `critical` - Critical errors
- `off` - Disable all logging

**Example usage:**
```shell
# Development: Enable debug logs
SPDLOG_LEVEL=debug ./ezio

# Production: Only show warnings and errors
SPDLOG_LEVEL=warn ./ezio

# Troubleshooting: Debug specific component
SPDLOG_LEVEL=info,raw_disk_io=debug ./ezio
```

## Performance Tuning

EZIO's disk I/O and hashing performance can be tuned via thread pool settings. The default values are optimized for mixed workloads but can be adjusted for specific scenarios.

### Thread Pool Configuration

**Command Line Options:**
```shell
./ezio --aio-threads <num>       # Disk I/O threads (default: 16)
./ezio --hashing-threads <num>   # Hashing threads (default: 8)
```

**Default Settings:**
- `aio_threads`: 16 (disk I/O operations: read, write)
- `hashing_threads`: 8 (SHA-1 piece verification)

These settings can now be adjusted at runtime without recompilation.

### Memory Configuration

**Buffer Pool:**
- Fixed size: 256 MB
- Unified pool for all I/O operations (read, write, hash)
- Dynamic allocation with watermarks (50% low, 87.5% high)

### Recommendations by Storage Type

For optimal performance, consider your storage hardware:

**HDD (Traditional Hard Disk):**
- Lower thread count recommended to reduce seek overhead
- Example: `./ezio --aio-threads 2 --hashing-threads 4`
- Sequential access performs better than parallel

**SATA SSD:**
- Moderate parallelism
- Default settings work well: `./ezio` (16 threads)
- Or explicit: `./ezio --aio-threads 16 --hashing-threads 8`

**NVMe SSD:**
- High parallelism for maximum throughput
- Example: `./ezio --aio-threads 32 --hashing-threads 8`
- Can saturate 10Gbps network with proper configuration

**Hashing Threads:**
- CPU-bound operation
- Default 8 threads matches typical server cores
- Adjust based on available CPU cores and workload

### Testing Different Configurations

You can easily test different thread pool configurations without recompilation:

```shell
# Test with minimal threads (HDD)
./ezio --aio-threads 2 --hashing-threads 2

# Test with default threads (SATA SSD)
./ezio

# Test with high parallelism (NVMe)
./ezio --aio-threads 32 --hashing-threads 8

# Combined with other options
./ezio --aio-threads 32 --cache-size 1024 --listen 0.0.0.0:50051
```

### Performance Monitoring

Monitor EZIO performance with:
- Log level: `SPDLOG_LEVEL=info` shows transfer rates
- System tools: `iostat`, `iotop` for disk utilization
- Network: `iftop`, `nload` for bandwidth usage

### Notes

- **Homogeneous deployments** (all same disk type) work best with current design
- **Heterogeneous setups** (mixed HDD/SSD) may need per-disk tuning in future versions
- Buffer pool size is fixed; future versions may support dynamic sizing

## Usage

### Partclone

[Partclone](http://partclone.org/) provides utilities to save and restore used filesystem blocks **(and skips the unused blocks)** from/to a partition.

The newest partclone will support dump your disk to EZIO image, and generate `torrent.info` simultaneously.
```shell
sudo partclone.extfs -c -T -s /dev/sda1 -O target/ --buffer_size 16777216
```
or you want generate torrent, but don't want BT image.
```shell
sudo partclone.extfs -c -t -s /dev/sda1 -O target/ --buffer_size 16777216
```

When finishing to dump disk, you will see the file like the picture. And using `utils/partclone_create_torrent.py` to generate torrent for deploy.
![](https://i.imgur.com/8o815PL.png)

```shell
utils/partclone_create_torrent.py -c CloneZilla -p sda1 -i <some_path>/torrent.info -o sda1.torrent -t 'http://<some tracker>:6969/announce'
```

### EZIO

When you have a `sda1.torrent` you can deploy or clone your disk via Network.

#### Help

```
Allowed Options:
  -h [ --help ]              some help
  -F [ --file ]              read data from file rather than raw disk
  --listen arg               gRPC service listen address and port, default is 127.0.0.1:50051
  --cache-size arg           unified cache size in MB, default is 512
  --aio-threads arg          number of threads for disk I/O, default is 16
  --hashing-threads arg      number of threads for hashing, default is 8
  -v [ --version ]           show version
```

#### Seeding

- Seeding from BT image
```shell
./ezio -F
./utils/create_proto_py.sh
./utils/add_torrent_seed.py sda1.torrent /some/path/to/sda1
```

- Seeding from Disk
```shell
./ezio
./utils/create_proto_py.sh
./utils/add_torrent_seed.py sda1.torrent /dev/sda1
```

#### Downloading

- Downloading to Disk
```shell
./ezio
./utils/create_proto_py.sh
./utils/add_torrent.py sda1.torrent /dev/sda1
```

- Proxy or save the image
```shell
./ezio -F
./utils/create_proto_py.sh
./utils/add_torrent.py sda1.torrent /some/path/to/save/sda1
```

#### Proxy

If you want to deploy over Internet or some bottleneck, you can proxy the torrent via regular BT software like [qBittorrent](https://www.qbittorrent.org/). And don't let internal peer connect outside directly.

## Easy Usage to Deploy Disk or OS via EZIO

Using CloneZilla Live (version>=testing-2.6.0-31). CloneZilla contains EZIO in its `Lite Server Mode`. It will be most easy way to deploy your disk or OS via BT.

## Design

### Custom Storage Implementation

EZIO implements a custom `libtorrent` [disk I/O interface](http://libtorrent.org/reference-Custom_Storage.html#overview) in `raw_disk_io.cpp/hpp`, allowing direct read/write to raw disk partitions without filesystem overhead.

**Key features:**
- **Direct disk access**: Writes received blocks directly to `/dev/sdX` partitions
- **Unified buffer pool**: 256MB shared memory pool for all I/O operations
- **Configurable thread pools**: Separate pools for disk I/O and hashing
- **Event-driven alerts**: Instant notification via `set_alert_notify()`

### Torrent Format

We store the disk "offset" in hexadecimal as the file path, and "length" as the file attribute. This allows BitTorrent to locate and seek to the exact disk position

```
{
    'announce': 'http://tracker.site1.com/announce',
    'info':
    {
        'name': 'root',
        'piece length': 262144,
        'files':
        [
            {'path': ['0000000000000000'], 'length': 4096}, // store offset and length of blocks
            {'path': ['0000000000020000'], 'length': 8192},
            ...
        ],
        'pieces': 'some piece hash here'
    }
}
```

## Benchmark

Compare with CloneZilla Multicast Mode with EZIO Mode.

### Experimental environment
- Network: Cisco 3560G
- Server: Dell T1700 with Intel Xeon E3-1226, 16G ram, 1TB hard disk
- PC Client: 32 Client, same as Server
- Image: Ubuntu Linux with 50GB data in disk. Multicast Image is compressed by `pzstd`. BT Image is raw file.

### Result
Time in second

| Number of client | Time (Unicast) | Time (EZIO) | Time (Multicast) | Ratio (BT/Multicast) |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 474 | 675 | 390 | 1.731 |
| 2 | 948 | 1273 | 474 | 2.686 |
| 4 | 1896 | 1331 | 638 | 2.086 |
| 8 | 3792 | 1412 | 980 | 1.441 |
| 16 | 7584 | 1005 | 1454 | 0.691 |
| 24 | 11376 | 1048 | 1992 | 0.526 |
| 32 | 15168 | 1143 | 2203 | 0.519 |

## Open Access Journal

More details about EZIO design and benchmark are in [A Novel Massive Deployment Solution Based on the Peer-to-Peer Protocol](https://www.mdpi.com/2076-3417/9/2/296).

## Limitation

- Making a torrent cost lots of time due to sha-1 hash need to be done on every single piece of data.
- EZIO will be extremely slow when the number of clients is too small.
- Due to partclone limitation, for unsupported filesystem, sector-to-sector copy is done by dd in EZIO.

## Future Improvements

### Performance Optimizations (In Progress)
- **Write coalescing**: Batch multiple writes with `writev()` for better HDD performance
- **Persistent cache**: Replace temporary store_buffer with sharded persistent cache

### Integration
- Integrate in OpenStack Ironic Project, improve Mirantis' works
    - http://web.archive.org/web/20211124125644/https://www.mirantis.com/blog/cut-ironic-provisioning-time-using-torrents/
    - https://review.opendev.org/c/openstack/ironic-specs/+/311091

### Completed Recent Improvements
- ✅ gRPC control interface
- ✅ Runtime log level control
- ✅ Event-driven alert handling
- ✅ Unified buffer pool (256MB)
- ✅ Configurable thread pools

## Contribute

- Issue Tracker: https://github.com/tjjh89017/ezio/issues
- Source Code: https://github.com/tjjh89017/ezio

## Support 

If you are having issues, please let us know.
EZIO main developer email is located at: tjjh89017@hotmail.com

## Special Thanks

- National Center for High-performance Computing, NCHC, Taiwan
    - Provide many devices to test stability and knowledge support.

## License

The project is licensed under the GNU General Public License v2.0 license.

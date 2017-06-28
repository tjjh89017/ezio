# EZIO Developer and User Guide

[![Build Status](https://travis-ci.org/tjjh89017/ezio.svg?branch=master)](https://travis-ci.org/tjjh89017/ezio)

## Introduction

EZIO is a tool for rapid server disk image cloning/deployment within local area network. We utilize BitTorrent protocol to speed up the data distribution. Also, we use `partclone` to dump used filesystem blocks, and EZIO receiver can directly write received blocks to raw disk, which greatly improves performance. 

## Motivation

EZIO is inspired by Clonezilla and BTsync (Resilio) for its idea to transfer data and massive deployment. The issue of Clonezilla is that, it is too slow in real world due to multicast feature. In real world, all clients must register to Clonezilla server before startting deployment, which cost too much time. In addition, whenever there is a client that doesn't get data or gets discorrect one and need to be re-transferred, it causes server a lot of effort. Most importanlty, in most case, the clients which cannot get data correctly may be broken, it will make server to re-transfer data again and again until it reaches its re-transfer limit and quit. Due to above issues of Clonezilla, EZIO make a difference by changing transfer mechanism. EZIO implement transfer function on top of BitTorrent, and make a lot of progress on deployment speed. 

## Feature


- Faster than Clonezilla by implementing data transfer on top of the BitTorrent protocol. Clonezilla uses multicast for transfer,for which in practice are extremely slow due to limitation of multicast and clients'  status. Limitation of multicast, for example, they will cost too much time waiting all the clients to register to the server. As for Computer status, for example, when there are a small amount of computers which don't have enough disk storage or might be broken, in this case, they won't get data from server successfully and need to re-transfer data, which  cost lots of time.



- Plenty of File systems are supported: 
    (1) ext2, ext3, ext4, reiserfs, reiser4, xfs, jfs, btrfs, f2fs and nilfs2 of GNU/Linux
    (2) FAT12, FAT16, FAT32, NTFS of MS Windows
    (3) HFS+ of Mac OS
    (4) UFS of FreeBSD, NetBSD, and OpenBSD
    (5) minix of Minix
    (6) VMFS3 and VMFS5 of VMWare ESX. 
    Therefore you can clone GNU/Linux, MS windows, Intel-based Mac OS, FreeBSD, NetBSD, OpenBSD, Minix, VMWare ESX and Chrome OS/Chromium OS, no matter it's 32-bit (x86) or 64-bit (x86-64) OS. For these file systems, only used blocks in partition are saved and restored. For unsupported file system, sector-to-sector copy is done by dd in EZIO.

- Different from BTsync file level transfer, EZIO is block level transfer. Whenever a client gets wrong data, in file level transfer, it will take a lot of time re-transfer whole file. However, in block level transfer, all we need to do is to re-transfer the specific piece of data.

- Saves data in the hard disk by using partclone. 


# Installation

### Minimum System Requirements

- 64bit
- 1GB RAM

### Dependencies

- Ubuntu>=16.10
```shell
sudo apt-get install libtorrent-rasterbar-dev -y
```

- Ubuntu==16.04
```shell
# compile libtorrent>=1.1.1
```





#### ezio
- libtorrent-rasterbar 1.1.1
  - this is a special distribution, make sure you install the right version
- boost 1.58 or later
- partclone
#### partclone
- openssh-server 
- libboost1.58-all-dev 
- libssl-dev 
- uuid-dev 
- ext2fs-dev 
- automake 
  - if you installed ver 1.15, soft link it to 1.14 may work
- ... and there are some other libraries, check 'INSTALL' document under partclone repository.

### Partclone

[Partclone](http://partclone.org/) provides utilities to save and restore used filesystem blocks **(and skips the unused blocks)** from/to a partition.

@mangokingTW made a fork of partclone that outputs sections of continuous blocks into files, these files are used later to create a torrent file.

```shell
sudo apt-get install libssl-dev uuid-dev -y
git clone https://github.com/tjjh89017/partclone
cd partclone
```
...... then choose the filesystem support you want. 

For example, extfs suppoort:

```shell
./configure --enable-extfs
```

...... and you should see the result like this picture:
![](https://i.imgur.com/KJ9f5Ie.png)
<br>
And we can start to build.

```shell
make
# if build success, src folder should appear.
cd src && ls
# you would see partclone.Xfs, where X is the fs you choose.
```

### EZIO

```shell
git clone https://github.com/tjjh89017/ezio
cd ezio && make
```
    
## Usage

Here we demonstrate how to clone a disk to machines across the network.

### Dump partition using Partclone

```shell
./partclone.Xfs -c -T -s (source partition) -o (output directory name)
```
    
- Notice that in current version, the generated files will directly appear in current directory. You may prefer to make an extra folder and use partclone under it.
- also, root permission may be required.

The generated result should look like this:
![](https://i.imgur.com/8o815PL.png)

Each file stores a section of used continuous filesystem blocks. The file name denotes its offset on the partition.

#### Example
```shell
./partclone.extfs -c -T -s /dev/sda1 -o target/ | ezio/utils/partclone_create_torrent.py
```
Then you will see `a.torrent` which contains all files in `target/`

```
-T output btfiles and output info for create torrent
-t output torrent info only (for parition-to-partition cloning)
```

### Make a torrent file

Using `qbittorrent` or similar softwares.
Todo: bt client

### Configure

TODO `ezio.conf`

```bash
TORRENT=/path/to/torrent/in/tftp/server/a.torrent
TARGET=/dev/sda1 # point to the target partition
```

### Start tracker, tftp server, and seeder

You can use opentracker to announce.

Normal BT client can be the seeder.

### Boot up receivers (clients)

TODO: PXE

## Troubleshooting
#### Todo: automake version conflict

## Design

In `main.cpp#28` implements a `libtorrent` [custom storage](http://libtorrent.org/reference-Custom_Storage.html#overview), to allow the receiver to write received blocks directly to raw disk.

We store the "offset" in hex into torrent, the "length" into file attribute.
so BT will know where the block is, and it can use the offset to seek in the disk

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


## Limitation


- Making a torrent cost lots of time due to sha-1 hash need to be done on every single piece of data.

- EZIO will be extremely slow when the number of clients is too small.

- Due to partclone limitation, for unsupported filesystem, sector-to-sector copy is done by dd in EZIO.

## Future

- Hope to become a sub-module in Clonezilla
- Make a entire disk, including partition, a torrent

## Contribute

- Issue Tracker: https://github.com/tjjh89017/ezio/issues
- Source Code: https://github.com/tjjh89017/ezio

## Support 

If you are having issues, please let us know.
EZIO main developer email is located at: tjjh89017@hotmail.com


## License

The project is licensed under the GNU General Public License v2.0 license.



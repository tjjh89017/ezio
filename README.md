# EZIO Developer and User Guide

[![Build Status](https://travis-ci.org/tjjh89017/ezio.svg?branch=master)](https://travis-ci.org/tjjh89017/ezio)

EZIO is a tool for rapid server disk image cloning/deployment within local area network. We utilize BitTorrent protocol to speed up the data distribution. Also, we use `partclone` to dump used filesystem blocks, and EZIO receiver can directly write received blocks to raw disk, which greatly improves performance. 
## Building on ubuntu
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

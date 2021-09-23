# EZIO-nbd

## Introduction

EZIO-nbd could let you emulate EZIO image into NBD (Network Block Device). You can mount EZIO image into readonly filesystem to dump files.

## Dependencies

- nbdkit>=1.24
- nbdkit-plugin-python>=1.24
- nbd-client>=3.21
- python>=3.8

## Usage

Start NDB server with EZIO image

```
nbdkit -r python ./ezio-nbd.py path=<ezio image path>
sudo nbd-client localhost /dev/nbd0
sudo mkdir /tmp/mnt
sudo mount /dev/nbd0 /tmp/mnt

# read file from /tmp/mnt

sudo umount /tmp/mnt
sudo nbd-client /dev/nbd0
```

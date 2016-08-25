# Ezio

## Description

BT-based Disk Cloner (?

## Design

### Client 

* Using [Custom Storage](http://libtorrent.org/reference-Custom_Storage.html#overview) for writing to disk directly
* Seeding until uploaded 150% or idle for 15 mins
* PXE Boot with Linux kernel and initramfs

### Server

* Using qBittorrent instead temporarily for seeding and tracker

### Future

* Be the one of CloneZilla options!

## Info

* [BT (BitTorrent)](https://en.wikipedia.org/wiki/BitTorrent)

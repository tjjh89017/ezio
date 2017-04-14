# Ezio

[![Build Status](https://travis-ci.org/tjjh89017/ezio.svg?branch=master)](https://travis-ci.org/tjjh89017/ezio)

<h1><font color="red">WARNING: Docs is out-of-date. Contact the maintainer if you need the latest usage.</font></h1>
<h1><font color="red">WARNING: It will erase your data.</font></h1>
<h1><font color="red">WARNING: Netboot is experimental.</font></h1>

## Description

BT-based Disk Cloner (?

## Installation & Use

### Ubuntu 16.04

### NOTE: depends libtorrent-rasterbar 1.1.1

### Prerequisite

``` bash
apt-get install libboost1.58-all-dev libssl-dev
```

### Build

```bash
git clone https://github.com/tjjh89017/ezio.git
cd ezio && make
sudo ./ezio '$MAGNET_URI' /dev/sdb
sudo partprobe
```

### Build Netboot image in Linux

```bash
make netboot
ls utils/{linux,initrd.img}
```

### Note: Torrent can only contain 1 file.

## Design

### Client 

* Using [Custom Storage](http://libtorrent.org/reference-Custom_Storage.html#overview) for writing to disk directly
* Seeding until uploaded 150% or idle for 15 mins
* PXE Boot with Linux kernel and initramfs

### Server

* Using qBittorrent instead temporarily for seeding and tracker

### Future

* Be the one of CloneZilla options!

## TODO

### Hard-Code
前期先把一些少量改動都寫死，後期在重構

### Choosing Disk to clone
要可以選硬碟去複寫

### Disk or Partition write
目標支援整個硬碟的複寫，以及分割區複寫

### Rename?
可能之後把名稱改成 BT 龍（ Torrent Dragon ）之類的

### Private Torrent
我不能讓資料外流，所以預設先關閉 DHT ，也必須使用私人種子

### Partclone
未來目標支援類似 Partclone 的方式，只複製檔案而非整個分割區或是硬碟，想法大概是把所有檔案都給 torrent 管理，直接寫入檔案系統，可能要注意檔案權限以及屬性的問題。

### PXE Bootable
可能用 `debian-installer` 修改後包裝

### Sequencial BT
連續寫入，減少 disk seeking time

## Info

* [BT (BitTorrent)](https://en.wikipedia.org/wiki/BitTorrent)
* [Udpcast 可能產生的問題](http://newtoypia.blogspot.tw/2015/04/udpcast.html)
* [Multicast 導致的速度緩慢](http://drbl.nchc.org.tw/fine-print.php?path=./faq/1_DRBL_common/49_multicast_slow.faq#49_multicast_slow.faq)
* `utils/init` modify from Ubuntu
* `utils/udhcpc.sh` modify from busybox examples
* `utils/inittab` modify from busybox

## 數據

某地使用 CloneZilla ，使用 4 台 server ，複製總共 60 台電腦，映像檔大小約為 80 GiB ，使用時間約為 1 小時。

2016/11/16
某地使用 BT 龍，使用 1 台 server，複製總共 40 台電腦，映像檔大小約為 67 GiB，使用時間約為 30 分鐘，速度約為 40 Mib/s。

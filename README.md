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

## TODO

### Hard-Code
前期先把一些少量改動都寫死，後期在重構

### Choosing Disk to clone
要可以選硬碟去複寫

### storage_interface
目前不知道怎麼實作 `readv / writev` ，文件太爛看不懂

Sloved! 直接用 `preadv` `pwritev`

### R/W Function
目前傾向用 syscall wrapper `open` 、 `read` 、 `write` ，而非使用 C library 提供的檔案操作，不使用 buffering 相關的操作，而是未來直接跟 BT 一起處理 buffer 問題。

Sloved! 直接用 `preadv` `pwritev`

### static linking
這樣可以讓包裝成 PXE bootable 時候簡單一些

### Disk or Partition write
目標支援整個硬碟的複寫，以及分割區複寫

### Config based
從 server 下載 config 跟 torrent ，然後依照 config 來執行。 config 可以選擇寫入哪個分割區以及硬碟。

### Rename?
可能之後把名稱改成 BT 龍（ Torrent Dragon ）之類的

## Info

* [BT (BitTorrent)](https://en.wikipedia.org/wiki/BitTorrent)
* [Udpcast 可能產生的問題](http://newtoypia.blogspot.tw/2015/04/udpcast.html)

## 數據

某地使用 CloneZilla ，使用 4 台 server ，複製總共 60 台電腦，映像檔大小約為 80 GiB ，使用時間約為 1 小時。
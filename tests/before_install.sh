#!/bin/bash

# build libtorrent
wget https://github.com/arvidn/libtorrent/releases/download/libtorrent-1_1_3/libtorrent-rasterbar-1.1.3.tar.gz
mkdir -p libtorrent-rasterbar && tar xf libtorrent-rasterbar-1.1.3.tar.gz -C libtorrent-rasterbar/ --strip-components=1
cd libtorrent-rasterbar/
mkdir -p build
./configure --prefix=$PWD/build/
make -j4
make install

#!/bin/bash

# build libtorrent
wget https://github.com/arvidn/libtorrent/releases/download/libtorrent-1_1_3/libtorrent-rasterbar-1.1.3.tar.gz
tar xf libtorrent-rasterbar-1.1.3.tar.gz
cd libtorrent-rasterbar-1.1.3/
cmake .
make -j
sudo make install

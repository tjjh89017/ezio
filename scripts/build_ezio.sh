#!/bin/bash -xe
sed -i 's/us\./tw\./g' /etc/apt/sources.list
apt update
apt install vim git openssh-server -y
apt install libboost1.58-all-dev libssl-dev -y
apt install uuid-dev ext2fs-dev -y
apt install automake -y
cd ~
wget https://github.com/arvidn/libtorrent/releases/download/libtorrent-1_1_1/libtorrent-rasterbar-1.1.1.tar.gz
tar xvf libtorrent-rasterbar-1.1.1.tar.gz
cd libtorrent-rasterbar-1.1.1
./configure
make install clean
cd ~ 
git clone https://github.com/mangokingTW/ezio
cd ezio
make
make netboot

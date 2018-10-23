#!/bin/bash

# install libtorrent
SSH_ARGS="-o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no"
scp $SSH_ARGS "ezio@ezio.kojuro.date:~/libtorrent-rasterbar.tar.gz" ./
mkdir -p libtorrent-rasterbar && tar xf libtorrent-rasterbar.tar.gz -C libtorrent-rasterbar/

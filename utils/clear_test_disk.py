#!/usr/bin/env python3

import libtorrent as lt
import sys
import os
import re
from argparse import ArgumentParser
from argparse import RawTextHelpFormatter

# parse args
parser = ArgumentParser(
        description='''clear disk''',
        formatter_class=RawTextHelpFormatter)
parser.add_argument("-d", "--disk", help="Input torrent.info from file. if none, read from stdin", dest="disk", required=True)
parser.add_argument("-t", "--torrent", help="Tracker for this torrent", dest="torrent", required=True)

args = parser.parse_args()
torrent = args.torrent
disk = args.disk

data = None

with open(torrent, 'rb') as f:
    data = f.read()

t = lt.bdecode(data)
files = t[b'info'][b'files']

with open(disk, 'wb') as f:
    for file in files:
        length = file[b'length']
        offset = int(file[b'path'][0].decode('ascii'), 16)
        print(f'clear {hex(offset)}, len: {length}')
        f.seek(offset)
        f.write(b'\00' * length)

#!/usr/bin/env python

import libtorrent as lt
import sys
import re

data = sys.stdin.read()
print data

# collect sha1
piece_hash = re.findall(r'^sha1: (.*)$', data, re.M)
print piece_hash

# collect offset and length
offset = re.findall(r'^offset: (.*)$', data, re.M)
length = re.findall(r'^length: (.*)$', data, re.M)

print "offset: ", offset
print "length: ", length

fs = lt.file_storage()
fs.set_piece_length(16 * 1024 * 1024) # 16MiB

for o, l in zip(offset, length):
    fs.add_file("a/" + o, int(l, 16))

torrent = lt.create_torrent(fs, 16 * 1024 * 1024, flags=0)
torrent.set_creator("Test")

print "file:", torrent.files().num_files()
print torrent.files().file_path(0)

for index, h in enumerate(piece_hash):
    torrent.set_hash(index, h.decode('hex'))

with open('a.torrent', 'wb') as f:
    f.write(lt.bencode(torrent.generate()))

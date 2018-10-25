#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Client for Ezio to get information about server with gRPC"""


import math
import time
import sys

from absl import app
from absl import flags
from tqdm import tqdm

import argparse
import grpc
import ezio_pb2
import ezio_pb2_grpc

FLAGS = flags.FLAGS
flags.DEFINE_string("hash", "", "Torrent hash")
flags.DEFINE_bool("progress", False, "Get progress for torrents")
flags.DEFINE_bool("download_speed", False, "Get download speed for torrents")
flags.DEFINE_bool("upload_speed", False, "Get upload speed for torrents")



def convert_size(size_bytes):
    """Convert byte size to human readable format"""

    if size_bytes == 0:
        return "0B"
    size_name = ("B", "KB", "MB", "GB", "TB")
    i = int(math.floor(math.log(size_bytes, 1024)))
    power = math.pow(1024, i)
    size = round(size_bytes / power, 2)
    return "%s %s" % (size, size_name[i])


def client(argv):
    """Main function"""

    channel = grpc.insecure_channel('localhost:50051')
    stub = ezio_pb2_grpc.EZIOStub(channel)
    valid = False


    if FLAGS.progress:
        get_progress(stub, FLAGS.hash)
    if FLAGS.download_speed:
        get_download_speed(stub, FLAGS.hash)
    if FLAGS.upload_speed:
        get_upload_speed(stub, FLAGS.hash)
    
    if len(sys.argv) <= 1:
        print("FATAL Flags parsing error: Please define at least one metrics to get")
        print(FLAGS.main_module_help())


def get_progress(stub, hashes):
    """Get current progress of Ezio"""

    request = ezio_pb2.UpdateRequest(hashes = hashes)
    result = stub.GetTorrentStatus(request)

    for torrent in result.hashes:
        prog = result.torrents[result.hashes[0]].progress*100 
        print("Current Progress (%s): %.2f %%" % (torrent, prog))

def get_download_speed(stub, hashes):
    """Get current download speed for Ezio"""

    request = ezio_pb2.UpdateRequest(hashes = hashes)
    result = stub.GetTorrentStatus(request)

    for torrent in result.hashes:
        speed = convert_size(result.torrents[torrent].download)
        print("Current Download Speed (" + torrent + "): " + speed + "/s")


def get_upload_speed(stub, hashes):
    """Get current upload speed for Ezio"""

    request = ezio_pb2.UpdateRequest(hashes = hashes)
    result = stub.GetTorrentStatus(request)

    for torrent in result.hashes:
        speed = convert_size(result.torrents[torrent].upload)
        print("Current Upload Speed (" + torrent + "): " + speed + "/s")

if __name__ == '__main__':
    app.run(client)

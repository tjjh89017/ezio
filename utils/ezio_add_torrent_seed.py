#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import math
import time
from functools import reduce

import grpc
import ezio_pb2
import ezio_pb2_grpc

import os
import sys


if __name__ == '__main__':
    
    if len(sys.argv) < 3:
        print("no torrent & path")
        sys.exit(1)

    address = "127.0.0.1:50051"
    channel = grpc.insecure_channel(address)
    stub = ezio_pb2_grpc.EZIOStub(channel)

    request = ezio_pb2.AddRequest()
    request.max_uploads = 1
    request.max_connections = 2
    request.save_path = sys.argv[2]
    request.seeding_mode = True
    with open(sys.argv[1], 'rb') as f:
        request.torrent = f.read()

    stub.AddTorrent(request)

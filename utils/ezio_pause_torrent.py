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
    
    if len(sys.argv) < 2:
        print("no torrent")
        sys.exit(1)

    address = "127.0.0.1:50051"
    channel = grpc.insecure_channel(address)
    stub = ezio_pb2_grpc.EZIOStub(channel)

    request = ezio_pb2.PauseTorrentRequest()
    request.hash = sys.argv[1]
    stub.PauseTorrent(request)

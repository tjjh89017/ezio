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

import argparse

if __name__ == '__main__':
    
    parser = argparse.ArgumentParser()
    parser.add_argument('torrent', type=str, help="Torrent path")
    parser.add_argument('path', type=str, help="Destination path")
    parser.add_argument('-a', '--address', type=str, default="127.0.0.1:50051", help="Send to gRPC server, default is 127.0.0.1:50051")
    parser.add_argument('-S', '--seeding', action='store_true', help="Seeding mode at start, default is no")
    parser.add_argument('-s', '--sequential', action='store_true', help="Set sequential download, default is no")
    parser.add_argument('-c', '--connections', type=int, default=3, help="Max total connections, default is 3")
    parser.add_argument('-u', '--uploads', type=int, default=2, help="Max upload connections, default is 2")

    args = parser.parse_args()

    address = args.address
    channel = grpc.insecure_channel(address)
    stub = ezio_pb2_grpc.EZIOStub(channel)

    request = ezio_pb2.AddRequest()
    request.max_uploads = args.uploads
    request.max_connections = args.connections
    request.sequential_download = args.sequential
    request.seeding_mode = args.seeding
    request.save_path = args.path
    with open(args.torrent, 'rb') as f:
        request.torrent = f.read()

    stub.AddTorrent(request)

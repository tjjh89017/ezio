#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import grpc
import ezio_pb2
import ezio_pb2_grpc

def main():

    channel = grpc.insecure_channel('localhost:50051')
    stub = ezio_pb2_grpc.EZIOStub(channel)

    request = ezio_pb2.UpdateRequest()
    request.hashs.append("1234")

    result = stub.GetTorrentStatus(request)
    print(result)

    pass


if __name__ == '__main__':
    main()

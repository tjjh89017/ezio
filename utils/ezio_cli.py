#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import sys
import math
import time
import datetime
from functools import reduce

import grpc
import ezio_pb2
import ezio_pb2_grpc

# in second
UPDATE_INTERVAL = 2
# in second
MIN_LAST_UPLOAD = 15
# in second
MIN_FINISHED_TIME = 15

# create grpc channel
# TODO use OO way
channel = grpc.insecure_channel("localhost:50051")
stub = ezio_pb2_grpc.EZIOStub(channel)

start_time = datetime.datetime.now()

def to_state(state):
    s = [
        '',
        'checking_files',
        'downloading_metadata',
        'downloading',
        'finished',
        'seeding',
        'unused_enum_for_backwards_compatibility_allocating',
        'checking_resume_data',
    ]

    return s[state]

def to_GBmin(speed):
    return (float(speed) * 60 / 1024 / 1024 / 1024)

def to_MBsec(speed):
    return (float(speed) / 1024 / 1024)

def get_version():
    request = ezio_pb2.Empty()
    result = stub.GetVersion(request)
    return result.version

def get_data():
    request = ezio_pb2.UpdateRequest()
    result = stub.GetTorrentStatus(request)
    return result

def get_avg(data):
    hashes = data.hashes
    torrents = data.torrents

    sum_download = 0
    sum_upload = 0
    sum_total = 0
    sum_total_done = 0
    for h in hashes:
        torrent = torrents[h]
        download = float(torrent.download_rate)
        upload = float(torrent.upload_rate)
        total = torrent.total
        total_done = torrent.total_done

        sum_download += download
        sum_upload += upload
        sum_total += total
        sum_total_done += total_done

    l = len(torrents)
    if l == 0:
        l = 1
    if sum_total == 0:
        sum_total = sum_total_done
    if sum_total == 0:
        sum_total = 1

    # 0%~100%
    progress_avg = float(sum_total_done) * 100 / sum_total

    return (progress_avg, sum_download, sum_upload)


def get_info(data):
    # print time diff
    seconds = (datetime.datetime.now() - start_time).seconds
    # print how many remain
    remain = len([x for x in data.torrents.values() if not x.is_finished])
    # progress, download rate, upload rate
    progress_avg, sum_download, sum_upload = get_avg(data)

    ss = f"T: {seconds}"
    ps = f"P: {progress_avg:3.0f}%"
    ds = "D: {:.2f}GB/min".format(to_GBmin(sum_download))
    us = "U: {:.2f}GB/min".format(to_GBmin(sum_upload))
    rs = f"R: {remain}"
    
    # [T: 120][P: 50%][D: 5GB/min][U: 5GB/min][R: 1]
    return f"[{ss}][{ps}][{ds}][{us}][{rs}]"

def check_stop(data):
    # detect upload ratio and seeding time
    try:
        if not data or len(data.hashes) <= 0:
            raise ValueError("No Data")

        for info_hash in data.hashes:
            need_stop = False
            t_stat = data.torrents[info_hash]
            if t_stat.is_paused:
                continue
            if not t_stat.is_finished:
                continue

            if t_stat.total_payload_upload > 3 * t_stat.total_done:
                need_stop = True
            if t_stat.finished_time > MIN_FINISHED_TIME and (t_stat.last_upload > MIN_LAST_UPLOAD or t_stat.last_upload == -1):
                need_stop = True

            if need_stop:
                # stop torrent
                request = ezio_pb2.PauseTorrentRequest()
                request.hash = info_hash
                stub.PauseTorrent(request)

        all_stop = True
        for k, t in data.torrents.items():
            if not t.is_finished or not t.is_paused:
                all_stop = False
                break

        if all_stop:
            # call shutdown
            request = ezio_pb2.Empty()
            stub.Shutdown(request)

            # shutdown
            time.sleep(10)
            return True

    except ValueError:
        pass

    # no stop
    return False

def main():
    # print version
    print("ezio " + get_version())

    # while loop
    while True:
        time.sleep(UPDATE_INTERVAL)
        # get data
        data = get_data()
        if not data:
            # retry it later
            continue

        # calc avg and print
        print(get_info(data), end="\r", flush=True)

        # check if stop
        if check_stop(data):
            print("")
            print("Finished. Exiting...")
            break

if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("-w", "--wait", default=15, type=int, help="The interval wait for other peer to keep upload (sec)")
    args = parser.parse_args()

    MIN_FINISHED_TIME = args.wait

    main()

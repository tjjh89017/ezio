#!/usr/bin/env python3
"""Helpers for loopback_test.sh.

loopback_ctl.py mktorrent <image> <out> <piece_size> <tracker_url>
loopback_ctl.py add <grpc_addr> <torrent> <target> [seed]
loopback_ctl.py wait <timeout_s> <grpc_addr>...
loopback_ctl.py uploaded <grpc_addr>
loopback_ctl.py shutdown <grpc_addr>

The gRPC commands need ezio_pb2 and ezio_pb2_grpc on PYTHONPATH.
"""
import os
import sys
import time


def mktorrent(image, out, piece, tracker):
    # Single-file v1 torrent; the file name is the disk offset.
    import libtorrent as lt
    fs = lt.file_storage()
    lt.add_files(fs, image)
    ct = lt.create_torrent(fs, int(piece), flags=lt.create_torrent.v1_only)
    ct.add_tracker(tracker)
    lt.set_piece_hashes(ct, os.path.dirname(image))
    with open(out, "wb") as f:
        f.write(lt.bencode(ct.generate()))
    print("pieces=%d piece_size=%d name=%s" % (fs.num_pieces(), fs.piece_length(), fs.file_name(0)))
    print("info_hash=%s" % lt.torrent_info(out).info_hash())


def stub(addr):
    import grpc
    import ezio_pb2_grpc
    return ezio_pb2_grpc.EZIOStub(grpc.insecure_channel(addr))


def add(addr, torrent, target, seed=None):
    import ezio_pb2
    r = ezio_pb2.AddRequest(save_path=target, seeding_mode=seed is not None,
                            max_uploads=4, max_connections=8)
    with open(torrent, "rb") as f:
        r.torrent = f.read()
    stub(addr).AddTorrent(r, timeout=30)


def wait(timeout, *addrs):
    """Poll until every node has finished; print 'finished <addr> <s>'."""
    import ezio_pb2
    timeout = float(timeout)
    start = time.monotonic()
    done = {}
    last = -10.0
    while len(done) < len(addrs):
        el = time.monotonic() - start
        if el > timeout:
            print("timeout after %.0f s, finished: %s" % (el, sorted(done)))
            sys.exit(1)
        report = el - last >= 10
        for a in addrs:
            if a in done:
                continue
            ts = stub(a).GetTorrentStatus(ezio_pb2.UpdateRequest(), timeout=10).torrents
            if ts and all(t.is_finished for t in ts.values()):
                done[a] = el
                print("finished %s %.2f s" % (a, el), flush=True)
            elif report:
                for t in ts.values():
                    print("  %s %.1f%% dl=%.1f MiB/s peers=%d" % (
                        a, t.progress * 100, t.download_rate / 1048576, t.num_peers), flush=True)
        if report:
            last = el
        time.sleep(0.2)


def uploaded(addr):
    """Print the payload bytes the node has uploaded, summed over its torrents."""
    import ezio_pb2
    ts = stub(addr).GetTorrentStatus(ezio_pb2.UpdateRequest(), timeout=10).torrents
    print(sum(t.total_payload_upload for t in ts.values()))


def shutdown(addr):
    import grpc
    import ezio_pb2
    try:
        stub(addr).Shutdown(ezio_pb2.Empty(), timeout=10)
    except grpc.RpcError:
        # The server may close the channel before it answers.
        pass


def main():
    cmds = {"mktorrent": mktorrent, "add": add, "wait": wait, "uploaded": uploaded,
            "shutdown": shutdown}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        sys.exit(__doc__)
    cmds[sys.argv[1]](*sys.argv[2:])


if __name__ == "__main__":
    main()

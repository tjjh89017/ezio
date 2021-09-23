#-*- coding: utf-8 -*-

import os
import re
import bisect
import nbdkit

API_VERSION = 2
builtin_open = open

path = None

def config(key, value):
    global path
    if key == 'path':
        path = os.path.abspath(value)
    else:
        raise RuntimeError("unknown parameter: " + key)

def config_complete():
    global path
    if path is None:
        raise RuntimeError("path parameter is required")

def thread_model():
    return nbdkit.THREAD_MODEL_PARALLEL

def open(readonly):
    global path

    fd_list = []
    filenames = []
    # assume partition size is 16TB by default
    size = 16 * 1024 * 1024 * 1024
    if readonly:
        pass
    else:
        pass

    try:
        if os.path.exists(os.path.join(path, 'torrent.info')):
            with builtin_open(os.path.join(path, 'torrent.info'), 'r') as f:
                data = f.read()
                block_size = int(re.findall(r'^block_size: (.*)$', data, re.M)[0])
                blocks_total = int(re.findall(r'^blocks_total: (.*)$', data, re.M)[0])
                size = block_size * blocks_total
    except:
        pass

    
    for f in os.listdir(path):
        fullpath = os.path.join(path, f)
        if f != 'torrent.info' and os.path.isfile(fullpath):
            filenames.append(f)

    filenames.sort()
    fd_list = [int(x, 16) for x in filenames]
    return {
        'path': path,
        'fd_list': fd_list,
        'filenames': filenames,
        'size': size
    }

def get_size(h):
    return h['size']

def pread(h, buf, offset, flags):
    nbdkit.debug("pread start ---")
    path = h['path']
    current_offset = offset
    buf_offset = 0
    fd_list = h['fd_list']
    filenames = h['filenames']
    # find closest value of offset
    index = bisect.bisect_left(fd_list, offset) - 1
    index = index if index > 0 else 0
    remain_len = len(buf)
    total_read = 0

    nbdkit.debug(f"offset: {offset}")

    while remain_len > 0:
        block_start = fd_list[index]
        fd = os.open(os.path.join(path, filenames[index]), os.O_RDONLY)
        sb = os.stat(fd)
        block_offset = current_offset - block_start
        read_len = remain_len

        if block_offset >= sb.st_size:
            nbdkit.debug(f"no data in {offset}")
            return

        if sb.st_size - block_offset < read_len:
            read_len = sb.st_size - block_offset

        nbdkit.debug(f"block_start: {block_start}")
        nbdkit.debug(f"block_offset: {block_offset}")
        nbdkit.debug(f"remain_len: {remain_len}")
        nbdkit.debug(f"read_len: {read_len}")
        nbdkit.debug(f"buf_offset: {buf_offset}")
        nbdkit.debug(f"current_offset: {current_offset}")
        nbdkit.debug(f"")
        tmp_buf = os.pread(fd, read_len, block_offset)
        buf[buf_offset:buf_offset+read_len] = tmp_buf

        remain_len -= read_len
        total_read += read_len
        buf_offset += read_len
        current_offset += read_len
        index += 1
        os.close(fd)

    nbdkit.debug(f"pread end -----")
    nbdkit.debug(f"")

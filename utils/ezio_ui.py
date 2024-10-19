#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import urwid

import sys
import math
import time
import datetime
from functools import reduce

import grpc
import ezio_pb2
import ezio_pb2_grpc

UPDATE_INTERVAL = 1

def to_state(state):
    '''
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
    '''
    s = [
        '',
        'C',
        'M',
        'D',
        'F',
        'S',
        'U',
        'R',
    ]

    return s[state]

def To_GBmin(speed):
    return (float(speed) * 60 / 1024 / 1024 / 1024)

def To_MBsec(speed):
    return (float(speed) / 1024 / 1024)

def to_size(size_bytes):
    if size_bytes == 0:
        return f"{0:7.2f}B"

    size_name = ("B", "KB", "MB", "GB", "TB")
    i = int(math.floor(math.log(size_bytes, 1024)))
    power = math.pow(1024, i)
    s = round(size_bytes / power, 2)
    return f"{s:7.2f}{size_name[i]}"

class UIModel:
    def __init__(self, address='localhost:50051'):
        self.data = None
        self.channel = grpc.insecure_channel(address)
        self.stub = ezio_pb2_grpc.EZIOStub(self.channel)

        self.update_data()

    def update_data(self):
        request = ezio_pb2.UpdateRequest()
        result = self.stub.GetTorrentStatus(request)
        self.data = result

    def get_data(self):
        return self.data

    def get_version(self):
        request = ezio_pb2.Empty()
        result = self.stub.GetVersion(request)
        return result.version

class UIView(urwid.WidgetWrap):
    """
    A class responsible for providing the application's interface and
    graph display.
    """
    palette = [
        ('body',         'black',      'light gray', 'standout'),
        ('header',       'white',      'dark red',   'bold'),
        ('screen edge',  'light blue', 'dark cyan'),
        ('main shadow',  'dark gray',  'black'),
        ('line',         'black',      'light gray', 'standout'),
        ('bg background','light gray', 'black'),
        ('bg 1',         'black',      'dark blue', 'standout'),
        ('bg 1 smooth',  'dark blue',  'black'),
        ('bg 2',         'black',      'dark cyan', 'standout'),
        ('bg 2 smooth',  'dark cyan',  'black'),
        ('button normal','light gray', 'dark blue', 'standout'),
        ('button select','white',      'dark green'),
        ('line',         'black',      'light gray', 'standout'),
        ('pg normal',    'white',      'black', 'standout'),
        ('pg complete',  'white',      'dark magenta'),
        ('pg smooth',     'dark magenta','black')
    ]

    def __init__(self, controller):
        self.controller = controller
        self.torrents = {}
        urwid.WidgetWrap.__init__(self, self.main_window())

    def exit_program(self, w):
        raise urwid.ExitMainLoop()

    def update_graph(self, force_update=False):
        data = self.controller.get_data()
        if not data:
            return

        hashes = data.hashes
        torrents = data.torrents

        sum_total = 0
        sum_total_done = 0
        sum_download = 0
        sum_upload = 0

        for h in hashes:
            torrent = torrents[h]
            download = float(torrent.download_rate)
            upload = float(torrent.upload_rate)

            sum_download += download
            sum_upload += upload

            if torrent.total == 0:
                torrent.total = torrent.total_done
            sum_total += torrent.total
            sum_total_done += torrent.total_done

            if h not in self.torrents:
                self.controller.loop.widget = self.main_window()

            self.torrents[h]['progress'].set_completion(torrent.progress)
            self.torrents[h]['download'].set_text(
                "D: {: 6.2f}GB/min, {: 7.2f}MB/s".format(To_GBmin(download), To_MBsec(download))
            )
            self.torrents[h]['upload'].set_text(
                "U: {: 6.2f}GB/min, {: 7.2f}MB/s".format(To_GBmin(upload), To_MBsec(upload))
            )
            self.torrents[h]['active_time'].set_text("[T: {}]".format(datetime.timedelta(seconds=torrent.active_time)))
            self.torrents[h]['num_peers'].set_text("[P: {:3d}]".format(torrent.num_peers))
            self.torrents[h]['state'].set_text("[S: {}]".format(to_state(torrent.state)))
            self.torrents[h]['total_done'].set_text("[F: {}]".format(to_size(torrent.total_done)))
            self.torrents[h]['total'].set_text(to_size(torrent.total))
            if torrent.last_upload != -1:
                self.torrents[h]['last_upload'].set_text("[L: {}]".format(datetime.timedelta(seconds=torrent.last_upload)))
            else:
                self.torrents[h]['last_upload'].set_text("[L:-00:00:01]".format(datetime.timedelta(seconds=torrent.last_upload)))

        self.progress.set_completion(sum_total_done / sum_total)
        self.download_upload.set_text(
            "D: {: 6.2f}GB/min, {: 7.2f}MB/s | U: {: 6.2f}GB/min, {: 7.2f}MB/s".format(To_GBmin(sum_download), To_MBsec(sum_download), To_GBmin(sum_upload), To_MBsec(sum_upload))
        )

    def progress_bar(self):
        return urwid.ProgressBar('pg normal', 'pg complete', 0, 1)

    def graph_controls(self):
        self.progress = self.progress_bar()
        self.progress_wrap = urwid.WidgetWrap(self.progress)
        self.download_upload = urwid.Text('D:    0MB/s,   0GB/min | U:    0MB/s,   0GB/min', align="right")

        version = self.controller.get_version()
        
        l = [
            urwid.Text("EZIO " + version, align="center"),
            urwid.Divider('-'),
            self.progress_wrap,
            self.download_upload,
            urwid.Divider('-'),
            # each progess and speed rate append
        ]

        data = self.controller.get_data()
        for h in data.hashes:
            torrent_name = urwid.Text("{}: {}".format(data.torrents[h].save_path, h), align="left")
            torrent_progress = self.progress_bar()
            torrent_download = urwid.Text('D:    0MB/s,   0GB/min', align="right")
            torrent_upload = urwid.Text('U:    0MB/s,   0GB/min', align="right")
            torrent_active_time = urwid.Text('[T: 00:00:00]', align="right")
            torrent_num_peers = urwid.Text('[P:   0]', align="right")
            torrent_state = urwid.Text('[S:  ]', align="right")
            torrent_total_done = urwid.Text('[F:    0MB]', align="right")
            torrent_total = urwid.Text('0GB', align="right")
            torrent_is_paused = urwid.Text('', align="right")
            torrent_save_path = urwid.Text('', align="right")
            torrent_last_upload = urwid.Text('[L: 00:00:00]', align="right")

            self.torrents[h] = {
                'name': torrent_name,
                'progress': torrent_progress,
                'download': torrent_download,
                'upload': torrent_upload,
                'active_time': torrent_active_time,
                'num_peers': torrent_num_peers,
                'state': torrent_state,
                'total_done': torrent_total_done,
                'total': torrent_total,
                'save_path': torrent_save_path,
                'last_upload': torrent_last_upload,
            }

            c1 = urwid.Columns([
                ('pack', torrent_name),
                ('pack', urwid.Text(' ')),
                ('pack', torrent_total),
            ])

            c2 = urwid.Columns([
                ('weight', 1, urwid.Text('')),
                ('pack', torrent_download),
                ('pack', urwid.Text(" | ")),
                ('pack', torrent_upload),
            ])

            c3 = urwid.Columns([
                ('weight', 1, urwid.Text('')),
                ('pack', torrent_total_done),
                ('pack', torrent_state),
                ('pack', torrent_num_peers),
                ('pack', torrent_active_time),
                ('pack', torrent_last_upload),
            ])

            p = urwid.Pile([
                ('pack', c1),
                ('pack', torrent_progress),
                ('pack', c2),
                ('pack', c3),
                ('pack', urwid.Divider('-')),
            ]) 

            l.append(p)

        self.torrent_list = urwid.ListBox(urwid.SimpleListWalker(l))
        return self.torrent_list

    def main_window(self):
        controls = self.graph_controls()
        w = urwid.Columns([('weight', 3, controls)])
        w = urwid.Padding(w, ('fixed left', 0), ('fixed right', 0))
        w = urwid.AttrWrap(w, 'body')
        w = urwid.LineBox(w)
        w = urwid.AttrWrap(w, 'line')
        return w

class UIController:
    def __init__(self):
        self.animate_alarm = None
        self.update_alarm = None
        self.model = UIModel()
        self.view = UIView(self)
        self.view.update_graph(True)

        self.loop = urwid.MainLoop(self.view, self.view.palette)

        self.update_data()
        self.animate_graph()
        self.detect_all_finished()

    def main(self):
        self.loop.run()

    def animate_graph(self, loop=None, user_data=None):
        self.view.update_graph()
        self.animate_alarm = self.loop.set_alarm_in(UPDATE_INTERVAL, self.animate_graph)

    def update_data(self, loop=None, user_data=None):
        self.model.update_data()
        self.update_alarm = self.loop.set_alarm_in(UPDATE_INTERVAL, self.update_data)

    def detect_all_finished(self, loop=None, user_data=None):
        # detect upload ratio and seeding time
        try:
            data = self.model.get_data()
            stub = self.model.stub
            if not data or len(data.hashes) <= 0:
                raise ValueError("No Data")

            for info_hash in data.hashes:
                t_stat = data.torrents[info_hash]
                if t_stat.is_paused:
                    continue
                if not t_stat.is_finished:
                    continue

                if t_stat.total_payload_upload > 3 * t_stat.total_done or t_stat.last_upload > 15 or t_stat.last_upload == -1:
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
                time.sleep(15)
                sys.exit(0)

                
        except ValueError:
            pass

        self.detect_alarm = self.loop.set_alarm_in(15, self.detect_all_finished)

    def get_data(self):
        return self.model.get_data()

    def get_version(self):
        return self.model.get_version()

def main():
    UIController().main()

if __name__ == '__main__':
    main()

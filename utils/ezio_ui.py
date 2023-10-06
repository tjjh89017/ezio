#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import urwid

import sys
import math
import time
from functools import reduce

import grpc
import ezio_pb2
import ezio_pb2_grpc

UPDATE_INTERVAL = 1

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

def To_GBmin(speed):
    return (float(speed) * 60 / 1024 / 1024 / 1024)

def To_MBsec(speed):
    return (float(speed) / 1024 / 1024)

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

        sum_progress = 0.0
        sum_download = 0
        sum_upload = 0
        l = []
        for h in hashes:
            torrent = torrents[h]
            download = float(torrent.download_rate)
            upload = float(torrent.upload_rate)
            sum_progress += torrents[h].progress
            sum_download += download
            sum_upload += upload

            if h not in self.torrents:
                self.controller.loop.widget = self.main_window()

            self.torrents[h]['progress'].set_completion(torrent.progress)
            self.torrents[h]['download'].set_text(
                "D: {:.2f}GB/min, {:.2f}MB/s".format(To_GBmin(download), To_MBsec(download))
            )
            self.torrents[h]['upload'].set_text(
                "U: {:.2f}GB/min, {:.2f}MB/s".format(To_GBmin(upload), To_MBsec(upload))
            )
            self.torrents[h]['active_time'].set_text("T: {} secs".format(torrent.active_time))
            self.torrents[h]['is_finished'].set_text("Finished: {}".format(str(torrent.is_finished)))
            self.torrents[h]['num_peers'].set_text("peers: {}".format(torrent.num_peers))
            self.torrents[h]['state'].set_text("state: {}".format(to_state(torrent.state)))
            self.torrents[h]['total_done'].set_text("total_done: {}".format(torrent.total_done))
            self.torrents[h]['total'].set_text("total: {}".format(torrent.total))
            self.torrents[h]['num_pieces'].set_text("num_pieces: {}".format(torrent.num_pieces))
            self.torrents[h]['finished_time'].set_text("finished_time: {}".format(torrent.finished_time))
            self.torrents[h]['seeding_time'].set_text("seeding_time: {}".format(torrent.seeding_time))
            self.torrents[h]['total_payload_download'].set_text("total_payload_download: {}".format(torrent.total_payload_download))
            self.torrents[h]['total_payload_upload'].set_text("total_payload_upload: {}".format(torrent.total_payload_upload))
            self.torrents[h]['is_paused'].set_text("paused: {}".format(str(torrent.is_paused)))
            self.torrents[h]['save_path'].set_text("save_path: {}".format(torrent.save_path))

        # avg progress
        l = len(torrents)
        if l == 0:
            l = 1
        self.progress.set_completion(sum_progress / l)
        self.download.set_text(
            "D: {:.2f}GB/min, {:.2f}MB/s".format(To_GBmin(sum_download), To_MBsec(sum_download))
        )
        self.upload.set_text(
            "U: {:.2f}GB/min, {:.2f}MB/s".format(To_GBmin(sum_upload), To_MBsec(sum_upload))
        )

    def button(self, t, fn):
        w = urwid.Button(t, fn)
        w = urwid.AttrWrap(w, 'button normal', 'button select')
        return w

    def progress_bar(self):
        return urwid.ProgressBar('pg normal', 'pg complete', 0, 1)

    def graph_controls(self):
        self.progress = self.progress_bar()
        self.progress_wrap = urwid.WidgetWrap(self.progress)
        self.download = urwid.Text('', align="right")
        self.upload = urwid.Text('', align="right")

        version = self.controller.get_version()
        
        l = [
            urwid.Text("EZIO " + version, align="center"),
            urwid.Divider('-'),
            #self.button("Quit", self.exit_program),
            #urwid.Divider('-'),
            urwid.Text("Total Progress", align="center"),
            self.progress_wrap,
            self.download,
            self.upload,
            urwid.Divider('-'),
            # each progess and speed rate append
        ]

        data = self.controller.get_data()
        for h in data.hashes:
            #torrent_name = urwid.Text("{}: {}".format(data.torrents[h].name, h), align="left")
            # set name as save path for better UI
            torrent_name = urwid.Text("{}: {}".format(data.torrents[h].save_path, h), align="left")
            torrent_progress = self.progress_bar()
            torrent_download = urwid.Text('', align="right")
            torrent_upload = urwid.Text('', align="right")
            torrent_active_time = urwid.Text('', align="right")
            torrent_is_finished = urwid.Text('', align="right")
            torrent_num_peers = urwid.Text('', align="right")
            torrent_state = urwid.Text('', align="right")
            torrent_total_done = urwid.Text('', align="right")
            torrent_total = urwid.Text('', align="right")
            torrent_num_pieces = urwid.Text('', align="right")
            torrent_finished_time = urwid.Text('', align="right")
            torrent_seeding_time = urwid.Text('', align="right")
            torrent_total_payload_download = urwid.Text('', align="right")
            torrent_total_payload_upload = urwid.Text('', align="right")
            torrent_is_paused = urwid.Text('', align="right")
            torrent_save_path = urwid.Text('', align="right")
            self.torrents[h] = {
                'name': torrent_name,
                'progress': torrent_progress,
                'download': torrent_download,
                'upload': torrent_upload,
                'active_time': torrent_active_time,
                'is_finished': torrent_is_finished,
                'num_peers': torrent_num_peers,
                'state': torrent_state,
                'total_done': torrent_total_done,
                'total': torrent_total,
                'num_pieces': torrent_num_pieces,
                'finished_time': torrent_finished_time,
                'seeding_time': torrent_seeding_time,
                'total_payload_download': torrent_total_payload_download,
                'total_payload_upload': torrent_total_payload_upload,
                'is_paused': torrent_is_paused,
                'save_path': torrent_save_path,
            }

            l.append(torrent_name)
            l.append(torrent_progress)
            l.append(torrent_download)
            l.append(torrent_upload)
            l.append(torrent_active_time)
            l.append(torrent_is_finished)
            l.append(torrent_num_peers)
            l.append(torrent_state)
            l.append(torrent_total_done)
            l.append(torrent_total)
            l.append(torrent_num_pieces)
            l.append(torrent_finished_time)
            l.append(torrent_seeding_time)
            l.append(torrent_total_payload_download)
            l.append(torrent_total_payload_upload)
            l.append(torrent_is_paused)
            l.append(urwid.Divider('-'))

        w = urwid.ListBox(urwid.SimpleListWalker(l))
        return w

    def main_shadow(self, w):
        return w

    def main_window(self):
        controls = self.graph_controls()
        w = urwid.Columns([('weight', 3, controls)])
        w = urwid.Padding(w, ('fixed left', 1), ('fixed right', 0))
        w = urwid.AttrWrap(w, 'body')
        w = urwid.LineBox(w)
        w = urwid.AttrWrap(w, 'line')
        w = self.main_shadow(w)
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

                if t_stat.total_payload_upload > 3 * t_stat.total_done or t_stat.seeding_time > 60:
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

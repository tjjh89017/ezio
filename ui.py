#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import urwid

import math
import time
from functools import reduce

import grpc
import ezio_pb2
import ezio_pb2_grpc

UPDATE_INTERVAL = 1

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
            download = float(torrent.download)
            upload = float(torrent.upload)
            # TODO dymanic bound
            # in MB/s
            l.append([To_MBsec(download), 0])
            l.append([0, To_MBsec(upload) / 1024 / 1024])
            sum_progress += torrents[h].progress
            sum_download += download
            sum_upload += upload

            self.torrents[h]['progress'].set_completion(torrent.progress)
            self.torrents[h]['download'].set_text(
                "D: {:.2f}GB/min, {:.2f}MB/s".format(To_GBmin(download), To_MBsec(download))
            )
            self.torrents[h]['upload'].set_text(
                "U: {:.2f}GB/min, {:.2f}MB/s".format(To_GBmin(upload), To_MBsec(upload))
            )
            self.torrents[h]['active_time'].set_text("T: {} secs".format(torrent.active_time))

        self.graph.set_data(l, 100)

        # avg progress
        self.progress.set_completion(sum_progress / len(torrents))
        self.download.set_text(
            "D: {:.2f}GB/min, {:.2f}MB/s".format(To_GBmin(sum_download), To_MBsec(sum_download))
        )
        self.upload.set_text(
            "U: {:.2f}GB/min, {:.2f}MB/s".format(To_GBmin(sum_upload), To_MBsec(sum_upload))
        )

        #self.graph.set_data([[80, 0], [0, 30], [10, 0]], 100)
        #self.progress.set_completion(0.3)

    def bar_graph(self, smooth=False):
        w = urwid.BarGraph(['bg background', 'bg 1', 'bg 2'])
        return w

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
        
        l = [
            urwid.Text("EZIO", align="center"),
            urwid.Divider('-'),
            self.button("Quit", self.exit_program),
            urwid.Divider('-'),
            urwid.Text("Total Progress", align="center"),
            self.progress_wrap,
            self.download,
            self.upload,
            urwid.Divider('-'),
            # each progess and speed rate append
        ]

        data = self.controller.get_data()
        for h in data.hashes:
            torrent_name = urwid.Text(data.torrents[h].name[:30], align="left")
            torrent_progress = self.progress_bar()
            torrent_download = urwid.Text('', align="right")
            torrent_upload = urwid.Text('', align="right")
            torrent_active_time = urwid.Text('', align="right")
            self.torrents[h] = {
                'name': torrent_name,
                'progress': torrent_progress,
                'download': torrent_download,
                'upload': torrent_upload,
                'active_time': torrent_active_time,
            }

            l.append(torrent_name)
            l.append(torrent_progress)
            l.append(torrent_download)
            l.append(torrent_upload)
            l.append(torrent_active_time)
            l.append(urwid.Divider('-'))

        w = urwid.ListBox(urwid.SimpleListWalker(l))
        return w

    def main_shadow(self, w):
        return w

    def main_window(self):
        self.graph = self.bar_graph()
        self.graph_wrap = urwid.WidgetWrap(self.graph)
        vline = urwid.AttrWrap(urwid.SolidFill(u'\u2502'), 'line')
        controls = self.graph_controls()

        w = urwid.Columns([('weight', 3, self.graph_wrap), ('fixed', 1, vline), ('fixed', 30, controls)])
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

    def main(self):
        self.loop.run()

    def animate_graph(self, loop=None, user_data=None):
        self.view.update_graph()
        self.animate_alarm = self.loop.set_alarm_in(UPDATE_INTERVAL, self.animate_graph)

    def update_data(self, loop=None, user_data=None):
        self.model.update_data()
        self.update_alarm = self.loop.set_alarm_in(UPDATE_INTERVAL, self.update_data)

    def get_data(self):
        return self.model.get_data()

def main():
    UIController().main()

if __name__ == '__main__':
    main()

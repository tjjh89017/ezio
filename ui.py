#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import urwid

import math
import time

UPDATE_INTERVAL = 1

class UIModel:
    def __init__(self):
        self.progress = 0
        self.download_rate = 0
        self.upload_rate = 0
    
    def update_data(self):
        # TODO test progress
        if self.progress >= 1:
            self.progress = 0
        else:
            self.progress += 0.05

        self.download_rate += 100
        self.upload_rate += 100

        pass
    def get_data(self):
        return {
            'progress': self.progress,
            'download_rate': self.download_rate,
            'upload_rate': self.upload_rate,
        }


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
        urwid.WidgetWrap.__init__(self, self.main_window())

    def exit_program(self, w):
        raise urwid.ExitMainLoop()

    def update_graph(self, force_update=False):
        data = self.controller.get_data()

        l = [[data['download_rate'], 0], [0, data['upload_rate']]]
        self.graph.set_data(l, 10000)

        self.progress.set_completion(data['progress'])

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
        
        l = [
            urwid.Text("EZIO", align="center"),
            urwid.Divider('-'),
            self.button("Quit", self.exit_program),
            urwid.Divider('-'),
            urwid.Text("Total Progress", align="center"),
            self.progress_wrap,
            urwid.Divider('-'),
            # each progess and speed rate append
        ]
        w = urwid.ListBox(urwid.SimpleListWalker(l))
        return w

    def main_shadow(self, w):
        return w

    def main_window(self):
        self.graph = self.bar_graph()
        self.graph_wrap = urwid.WidgetWrap(self.graph)
        vline = urwid.AttrWrap(urwid.SolidFill(u'\u2502'), 'line')
        controls = self.graph_controls()

        w = urwid.Columns([('weight', 3, self.graph_wrap), ('fixed', 1, vline), ('fixed', 25, controls)])
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

        self.animate_graph()
        self.update_data()

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

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
EZIO UI with component-based architecture, sorting, filtering, and highlighting.

Keyboard shortcuts:
  Sorting (press again to reverse):
    s - Sort by save path
    n - Sort by name (hash)
    p - Sort by progress
    d - Sort by download speed
    u - Sort by upload speed

  Filtering:
    a - Show all torrents
    1 - Show downloading only
    2 - Show seeding only
    3 - Show finished only

  Display:
    v - Toggle verbose mode
    q - Quit
    Arrow/PgUp/PgDn - Scroll

Color coding:
  Dark Green - High speed (>100 MB/s) / Finished
  Brown      - Medium speed (10-100 MB/s)
  Black      - Low speed (<10 MB/s)
  Dark Cyan  - Seeding
  Dark Red   - Paused/Error
"""

import urwid
import sys
import math
import time
import datetime
from functools import reduce
from enum import Enum

import grpc
import ezio_pb2
import ezio_pb2_grpc

# Constants
UPDATE_INTERVAL = 1


class SortMode(Enum):
    """Torrent sorting modes"""
    PATH = 'path'
    NAME = 'name'
    PROGRESS = 'progress'
    DOWNLOAD = 'download'
    UPLOAD = 'upload'


class FilterMode(Enum):
    """Torrent filtering modes"""
    ALL = 'all'
    DOWNLOADING = 'downloading'
    SEEDING = 'seeding'
    FINISHED = 'finished'


# =============================================================================
# Utility Functions
# =============================================================================

def to_state(state, long_form=False):
    """
    Convert torrent state enum to string.

    States (from libtorrent):
      0 - ''
      1 - checking_files
      2 - downloading_metadata
      3 - downloading
      4 - finished
      5 - seeding
      6 - unused_enum_for_backwards_compatibility_allocating
      7 - checking_resume_data
    """
    if long_form:
        states_long = [
            '',
            'checking_files',
            'downloading_metadata',
            'downloading',
            'finished',
            'seeding',
            'allocating',
            'checking_resume_data'
        ]
        return states_long[state] if state < len(states_long) else 'unknown'
    else:
        states_short = ['', 'C', 'M', 'D', 'F', 'S', 'U', 'R']
        return states_short[state] if state < len(states_short) else '?'


def to_GBmin(speed):
    """Convert bytes/sec to GB/min"""
    return float(speed) * 60 / 1024 / 1024 / 1024


def to_MBsec(speed):
    """Convert bytes/sec to MB/sec"""
    return float(speed) / 1024 / 1024


def to_size(size_bytes):
    """Convert bytes to human readable format"""
    if size_bytes == 0:
        return f"{0:7.2f}B"

    size_name = ("B", "KB", "MB", "GB", "TB")
    i = int(math.floor(math.log(size_bytes, 1024)))
    power = math.pow(1024, i)
    s = round(size_bytes / power, 2)
    return f"{s:7.2f}{size_name[i]}"


def format_speed(bytes_per_sec, prefix=''):
    """Format speed as 'D: xxx GB/min, xxx MB/s'"""
    return "{}: {: 6.2f}GB/min, {: 7.2f}MB/s".format(
        prefix,
        to_GBmin(bytes_per_sec),
        to_MBsec(bytes_per_sec)
    )


def format_timedelta(seconds):
    """Format seconds as HH:MM:SS"""
    if seconds == -1:
        return "-00:00:01"
    return str(datetime.timedelta(seconds=int(seconds)))


def get_speed_color(bytes_per_sec):
    """Get color attribute for speed"""
    mb_per_sec = to_MBsec(bytes_per_sec)
    if mb_per_sec > 100:
        return 'speed_high'
    elif mb_per_sec > 10:
        return 'speed_medium'
    else:
        return 'speed_low'


def get_state_color(torrent_data):
    """Get color attribute for torrent state"""
    state = torrent_data.state
    if torrent_data.is_paused:
        return 'state_paused'
    elif state == 5:  # seeding
        return 'state_seeding'
    elif state == 4:  # finished
        return 'state_finished'
    elif state == 3:  # downloading
        return 'state_downloading'
    else:
        return 'state_default'


# =============================================================================
# Helper Widgets
# =============================================================================

class LabeledValueWidget(urwid.Text):
    """Widget for displaying labeled values like '[Label: value]'"""

    def __init__(self, label, value="", show_brackets=True):
        self.label = label
        self.show_brackets = show_brackets
        super().__init__("", align="right")
        self.set_value(value)

    def set_label(self, label):
        """Update the label"""
        self.label = label

    def set_value(self, value):
        """Update the value"""
        if self.show_brackets:
            text = f"[{self.label}: {value}]"
        else:
            text = f"{self.label}: {value}"

        self.set_text(text)


class SpeedWidget(urwid.WidgetWrap):
    """Widget for displaying speed with color highlighting"""

    def __init__(self, prefix=''):
        self.prefix = prefix
        self.text_widget = urwid.Text("", align="right")
        self.attr_map = urwid.AttrMap(self.text_widget, 'speed_low')
        super().__init__(self.attr_map)
        self.set_speed(0)

    def set_speed(self, bytes_per_sec):
        """Update speed and color based on value"""
        speed_str = format_speed(bytes_per_sec, self.prefix)
        self.text_widget.set_text(speed_str)
        # Update color based on speed
        color = get_speed_color(bytes_per_sec)
        self.attr_map.set_attr_map({None: color})


# =============================================================================
# Widget Components
# =============================================================================

class TorrentWidget(urwid.WidgetWrap):
    """Widget for displaying a single torrent's information"""

    def __init__(self, info_hash, torrent_data, verbose=False):
        self.info_hash = info_hash
        self.torrent_data = torrent_data
        self.verbose = verbose

        # Create widgets
        self.name_text_widget = urwid.Text("")
        self.name_attr_map = urwid.AttrMap(self.name_text_widget, 'state_default')
        self.total_text = urwid.Text("", align="right")
        self.progress_bar = urwid.ProgressBar('pg normal', 'pg complete', 0, 1)

        # Speed widgets with automatic coloring
        self.download_widget = SpeedWidget('D')
        self.upload_widget = SpeedWidget('U')

        # Labeled value widgets
        self.total_done_widget = LabeledValueWidget('F')
        self.state_widget = LabeledValueWidget('S')
        self.num_peers_widget = LabeledValueWidget('P')
        self.active_time_widget = LabeledValueWidget('T')
        self.last_upload_widget = LabeledValueWidget('L')

        # Payload widgets (always shown)
        self.payload_down_widget = LabeledValueWidget('PD')
        self.payload_up_widget = LabeledValueWidget('PU')
        self.finished_time_widget = LabeledValueWidget('FT')

        # Build layout
        layout = self._build_layout()
        super().__init__(layout)

        # Initial update
        self.update(torrent_data, verbose)

    def _build_layout(self):
        """Build the widget layout"""
        # Line 1: Name and total size
        line1 = urwid.Columns([
            ('pack', self.name_attr_map),
            ('pack', urwid.Text(' ')),
            ('pack', self.total_text),
        ])

        # Line 2: Progress bar
        line2 = self.progress_bar

        # Line 3: Download and upload speeds
        line3 = urwid.Columns([
            ('weight', 1, urwid.Text('')),
            ('pack', self.download_widget),
            ('pack', urwid.Text(" | ")),
            ('pack', self.upload_widget),
        ])

        # Line 4: Stats
        line4 = urwid.Columns([
            ('weight', 1, urwid.Text('')),
            ('pack', self.total_done_widget),
            ('pack', self.state_widget),
            ('pack', self.num_peers_widget),
            ('pack', self.active_time_widget),
            ('pack', self.last_upload_widget),
        ])

        # Line 5: Payload info (always shown)
        line5 = urwid.Columns([
            ('weight', 1, urwid.Text('')),
            ('pack', self.payload_down_widget),
            ('pack', urwid.Text(' / ')),
            ('pack', self.payload_up_widget),
            ('pack', urwid.Text(' ')),
            ('pack', self.finished_time_widget),
        ])

        pile_items = [
            ('pack', line1),
            ('pack', line2),
            ('pack', line3),
            ('pack', line4),
            ('pack', line5),
            ('pack', urwid.Divider('-')),
        ]

        pile = urwid.Pile(pile_items)
        return pile

    def update(self, torrent_data, verbose=None):
        """Update widget with new torrent data"""
        if verbose is not None:
            self.verbose = verbose

        self.torrent_data = torrent_data

        # Update name with color based on state
        name = f"{torrent_data.save_path}: {self.info_hash}"
        self.name_text_widget.set_text(name)
        # Set color based on torrent state
        if torrent_data.is_finished:
            self.name_attr_map.set_attr_map({None: 'state_finished'})
        else:
            color = get_state_color(torrent_data)
            self.name_attr_map.set_attr_map({None: color})

        # Update sizes
        total = torrent_data.total if torrent_data.total > 0 else torrent_data.total_done
        self.total_text.set_text(to_size(total))

        # Update progress
        self.progress_bar.set_completion(torrent_data.progress)

        # Update speeds
        self.download_widget.set_speed(torrent_data.download_rate)
        self.upload_widget.set_speed(torrent_data.upload_rate)

        # Update stats
        self.total_done_widget.set_value(to_size(torrent_data.total_done))
        # Use long form state in verbose mode
        state_str = to_state(torrent_data.state, long_form=self.verbose)
        self.state_widget.set_value(state_str)
        self.num_peers_widget.set_value(f"{torrent_data.num_peers:3d}")
        self.active_time_widget.set_value(format_timedelta(torrent_data.active_time))

        if torrent_data.last_upload != -1:
            self.last_upload_widget.set_value(format_timedelta(torrent_data.last_upload))
        else:
            self.last_upload_widget.set_value("-00:00:01")

        # Update payload info (always shown)
        # Set label based on verbose mode
        if self.verbose:
            self.payload_down_widget.set_label('Payload Download')
            self.payload_up_widget.set_label('Payload Upload')
        else:
            self.payload_down_widget.set_label('PD')
            self.payload_up_widget.set_label('PU')

        self.payload_down_widget.set_value(to_size(torrent_data.total_payload_download))
        self.payload_up_widget.set_value(to_size(torrent_data.total_payload_upload))
        self.finished_time_widget.set_value(format_timedelta(torrent_data.finished_time))


class SummaryWidget(urwid.WidgetWrap):
    """Widget for displaying overall summary statistics"""

    def __init__(self):
        self.progress_bar = urwid.ProgressBar('pg normal', 'pg complete', 0, 1)
        self.download_widget = SpeedWidget('D')
        self.upload_widget = SpeedWidget('U')
        self.eta_text = urwid.Text('ETA: 00:00:00', align="right")

        layout = self._build_layout()
        super().__init__(layout)

    def _build_layout(self):
        """Build the summary layout"""
        speeds = urwid.Columns([
            ('weight', 1, urwid.Text('')),
            ('pack', self.download_widget),
            ('pack', urwid.Text(' | ')),
            ('pack', self.upload_widget),
            ('pack', urwid.Text(' | ')),
            ('pack', self.eta_text),
        ])

        pile = urwid.Pile([
            ('pack', self.progress_bar),
            ('pack', speeds),
        ])

        return pile

    def update(self, sum_total, sum_total_done, sum_download, sum_upload):
        """Update summary with aggregated data"""
        # Update progress
        if sum_total > 0:
            self.progress_bar.set_completion(sum_total_done / sum_total)
        else:
            self.progress_bar.set_completion(0)

        # Update speeds
        self.download_widget.set_speed(sum_download)
        self.upload_widget.set_speed(sum_upload)

        # Update ETA
        if sum_download > 0:
            eta = (sum_total - sum_total_done) / sum_download
            self.eta_text.set_text(f'ETA: {format_timedelta(eta)}')
        else:
            self.eta_text.set_text('ETA:      inf')


class TorrentListWidget(urwid.WidgetWrap):
    """Widget for managing sortable and filterable list of torrents"""

    def __init__(self):
        self.torrent_widgets = {}  # hash -> TorrentWidget
        self.sort_mode = SortMode.PATH
        self.sort_reverse = False
        self.filter_mode = FilterMode.ALL
        self.verbose = False

        self.list_walker = urwid.SimpleListWalker([])
        self.list_box = urwid.ListBox(self.list_walker)

        # Wrap ListBox with torrent_list_bg attribute
        body_wrapped = urwid.AttrMap(self.list_box, 'torrent_list_bg')

        # Add scroll bar
        scrollbar = urwid.ScrollBar(
            body_wrapped,
            thumb_char='█',
            trough_char='│'
        )

        layout = urwid.LineBox(scrollbar)
        super().__init__(layout)

    def update(self, data):
        """Update all torrents and re-sort if needed"""
        if not data or not data.hashes:
            return

        # Remove torrents that no longer exist
        current_hashes = set(data.hashes)
        removed = set(self.torrent_widgets.keys()) - current_hashes
        for h in removed:
            del self.torrent_widgets[h]

        # Update or create widgets
        for h in data.hashes:
            torrent_data = data.torrents[h]
            if h in self.torrent_widgets:
                self.torrent_widgets[h].update(torrent_data, self.verbose)
            else:
                self.torrent_widgets[h] = TorrentWidget(h, torrent_data, self.verbose)

        # Re-sort and update list
        self._rebuild_list(data)

    def _rebuild_list(self, data):
        """Rebuild the list with current sort order and filter"""
        # Filter and sort torrents
        filtered_hashes = self._filter_torrents(data)
        sorted_hashes = self._sort_torrents(data, filtered_hashes)

        # Rebuild list walker
        widgets = [self.torrent_widgets[h] for h in sorted_hashes]
        self.list_walker[:] = widgets

    def _filter_torrents(self, data):
        """Filter torrents based on current filter mode"""
        if self.filter_mode == FilterMode.ALL:
            return list(data.hashes)

        filtered = []
        for h in data.hashes:
            t = data.torrents[h]
            if self.filter_mode == FilterMode.DOWNLOADING:
                if t.state == 3:  # downloading
                    filtered.append(h)
            elif self.filter_mode == FilterMode.SEEDING:
                if t.state == 5:  # seeding
                    filtered.append(h)
            elif self.filter_mode == FilterMode.FINISHED:
                if t.is_finished:
                    filtered.append(h)

        return filtered

    def _sort_torrents(self, data, hashes):
        """Sort torrents based on current sort mode"""
        if self.sort_mode == SortMode.PATH:
            hashes.sort(key=lambda h: data.torrents[h].save_path, reverse=self.sort_reverse)
        elif self.sort_mode == SortMode.NAME:
            hashes.sort(reverse=self.sort_reverse)
        elif self.sort_mode == SortMode.PROGRESS:
            hashes.sort(key=lambda h: data.torrents[h].progress, reverse=not self.sort_reverse)
        elif self.sort_mode == SortMode.DOWNLOAD:
            hashes.sort(key=lambda h: data.torrents[h].download_rate, reverse=not self.sort_reverse)
        elif self.sort_mode == SortMode.UPLOAD:
            hashes.sort(key=lambda h: data.torrents[h].upload_rate, reverse=not self.sort_reverse)

        return hashes

    def set_sort_mode(self, mode, toggle_reverse=True):
        """Change sort mode, optionally toggle reverse if same mode"""
        if toggle_reverse and self.sort_mode == mode:
            self.sort_reverse = not self.sort_reverse
        else:
            self.sort_mode = mode
            if mode in [SortMode.PROGRESS, SortMode.DOWNLOAD, SortMode.UPLOAD]:
                self.sort_reverse = False  # Default: high to low
            else:
                self.sort_reverse = False  # Default: A to Z

    def set_filter_mode(self, mode):
        """Change filter mode"""
        self.filter_mode = mode

    def set_verbose(self, verbose):
        """Set verbose mode"""
        self.verbose = verbose
        for widget in self.torrent_widgets.values():
            widget.verbose = verbose


# =============================================================================
# Main UI View
# =============================================================================

class UIView(urwid.WidgetWrap):
    """Main UI view with component-based architecture"""

    palette = [
        ('body',              'black',       'light gray', 'standout'),
        ('header',            'white',       'dark red',   'bold'),
        ('screen edge',       'light blue',  'dark cyan'),
        ('main shadow',       'dark gray',   'black'),
        ('line',              'black',       'light gray', 'standout'),
        ('bg background',     'light gray',  'black'),
        ('bg 1',              'black',       'dark blue', 'standout'),
        ('bg 1 smooth',       'dark blue',   'black'),
        ('bg 2',              'black',       'dark cyan', 'standout'),
        ('bg 2 smooth',       'dark cyan',   'black'),
        ('button normal',     'light gray',  'dark blue', 'standout'),
        ('button select',     'white',       'dark green'),
        ('pg normal',         'white',       'black', 'standout'),
        ('pg complete',       'white',       'dark magenta'),
        ('pg smooth',         'dark magenta','black'),
        ('help_title',        'white',       'dark blue', 'bold'),
        ('scrollbar',         'light gray',  'dark gray'),
        ('scrollbar_smooth',  'dark gray',   'light gray'),
        # Speed colors
        ('speed_high',        'dark green',  'light gray', 'bold'),
        ('speed_medium',      'dark blue',   'light gray'),
        ('speed_low',         'black',       'light gray'),
        # State colors
        ('state_finished',    'dark green',  'light gray', 'bold'),
        ('state_seeding',     'dark cyan',   'light gray'),
        ('state_downloading', 'dark blue',   'light gray'),
        ('state_paused',      'dark gray',   'light gray'),
        ('state_default',     'black',       'light gray'),
        # Background colors (all same as body, but semantically separate)
        ('torrent_list_bg',   'black',       'light gray', 'standout'),
        ('help_content_bg',   'black',       'light gray', 'standout'),
        ('help_frame_bg',     'black',       'light gray', 'standout'),
        ('help_outer_bg',     'black',       'light gray', 'standout'),
        ('main_window_bg',    'black',       'light gray', 'standout'),
    ]

    def __init__(self, controller):
        self.controller = controller

        # Create components
        version = controller.get_version()
        self.header_text = urwid.Text(f"EZIO {version}", align="center")
        self.help_text = urwid.Text(
            "Press [h] for help | [q] Quit | Arrow/PgUp/PgDn: Scroll",
            align="center"
        )
        self.sort_text = urwid.Text("Sort: Path ↓ | Filter: All | Verbose: OFF", align="center")

        self.summary = SummaryWidget()
        self.torrent_list = TorrentListWidget()

        # Build main layout
        layout = self._build_main_layout()
        super().__init__(layout)

    def _build_main_layout(self):
        """Build the main window layout"""
        pile = urwid.Pile([
            ('pack', self.header_text),
            ('pack', self.help_text),
            ('pack', self.sort_text),
            ('pack', urwid.Divider('-')),
            ('pack', self.summary),
            ('weight', 1, self.torrent_list),
        ])

        w = urwid.Columns([('weight', 3, pile)])
        w = urwid.Padding(w, ('fixed left', 0), ('fixed right', 0))
        w = urwid.AttrWrap(w, 'main_window_bg')
        w = urwid.LineBox(w)
        w = urwid.AttrWrap(w, 'line')

        return w

    def update_graph(self, force_update=False):
        """Update all UI components with latest data"""
        data = self.controller.get_data()
        if not data or not data.hashes:
            return

        # Calculate summary
        sum_total = 0
        sum_total_done = 0
        sum_download = 0
        sum_upload = 0

        for h in data.hashes:
            torrent = data.torrents[h]
            sum_download += torrent.download_rate
            sum_upload += torrent.upload_rate

            total = torrent.total if torrent.total > 0 else torrent.total_done
            sum_total += total
            sum_total_done += torrent.total_done

        # Update components
        self.summary.update(sum_total, sum_total_done, sum_download, sum_upload)
        self.torrent_list.update(data)

        # Rebuild layout if torrents added/removed
        if force_update:
            self._w = self._build_main_layout()

    def set_sort_mode(self, mode):
        """Change sort mode and update display"""
        self.torrent_list.set_sort_mode(mode, toggle_reverse=True)
        self._update_status_text()
        self.update_graph(force_update=False)

    def set_filter_mode(self, mode):
        """Change filter mode and update display"""
        self.torrent_list.set_filter_mode(mode)
        self._update_status_text()
        self.update_graph(force_update=False)

    def toggle_verbose(self):
        """Toggle verbose mode and rebuild widgets"""
        verbose = not self.torrent_list.verbose
        self.torrent_list.set_verbose(verbose)
        self._update_status_text()

        # Need to rebuild all widgets
        data = self.controller.get_data()
        if data and data.hashes:
            # Clear existing widgets and recreate
            self.torrent_list.torrent_widgets.clear()
            self.torrent_list.update(data)

    def _update_status_text(self):
        """Update sort/filter/verbose status text"""
        sort_name = self.torrent_list.sort_mode.value.capitalize()
        sort_arrow = "↑" if self.torrent_list.sort_reverse else "↓"
        filter_name = self.torrent_list.filter_mode.value.capitalize()
        verbose_status = "ON" if self.torrent_list.verbose else "OFF"

        status = f"Sort: {sort_name} {sort_arrow} | Filter: {filter_name} | Verbose: {verbose_status}"
        self.sort_text.set_text(status)

    def show_help(self):
        """Show help dialog"""
        help_text = [
            "EZIO UI - Keyboard Shortcuts\n",
            "Sorting (press again to reverse):",
            "  s - Sort by save path",
            "  n - Sort by name (hash)",
            "  p - Sort by progress",
            "  d - Sort by download speed",
            "  u - Sort by upload speed",
            "",
            "Filtering:",
            "  a/0 - Show all torrents",
            "  1   - Show downloading only",
            "  2   - Show seeding only",
            "  3   - Show finished only",
            "",
            "Display:",
            "  v - Toggle verbose mode (show full state names)",
            "  h - Show this help",
            "  q - Quit",
            "  Arrow/PgUp/PgDn - Scroll",
            "",
            "Displayed Info:",
            "  D/U - Download/Upload speed (colored)",
            "  F - Total downloaded",
            "  S - State (C/M/D/F/S/U/R or full name in verbose)",
            "  P - Number of peers",
            "  T - Active time",
            "  L - Last upload time",
            "  PD/PU - Payload downloaded/uploaded",
            "  FT - Finished time",
            "",
            "Color coding:",
            "  Speed colors:",
            "    Dark Green (bold) - High speed (>100 MB/s)",
            "    Dark Blue         - Medium speed (10-100 MB/s)",
            "    Black             - Low speed (<10 MB/s)",
            "  Torrent state colors:",
            "    Dark Green (bold) - Finished",
            "    Dark Cyan         - Seeding",
            "    Dark Blue         - Downloading",
            "    Dark Gray         - Paused",
            "",
            "State abbreviations (short form):",
            "  C - Checking files",
            "  M - Downloading metadata",
            "  D - Downloading",
            "  F - Finished",
            "  S - Seeding",
            "  U - Allocating",
            "  R - Checking resume data",
        ]

        # Create list of text widgets for scrolling
        help_widgets = []
        for line in help_text:
            if line:  # Non-empty line
                help_widgets.append(urwid.Text(line))
            else:  # Empty line
                help_widgets.append(urwid.Divider())

        # Create scrollable list
        help_listwalker = urwid.SimpleFocusListWalker(help_widgets)
        help_listbox = urwid.ListBox(help_listwalker)

        # Wrap with help_content_bg attribute
        help_body_wrapped = urwid.AttrMap(help_listbox, 'help_content_bg')

        # Add scrollbar
        help_scrollbar = urwid.ScrollBar(help_body_wrapped, thumb_char='█', trough_char='│')

        # Add padding
        help_widget = urwid.Padding(help_scrollbar, left=2, right=2)

        title = urwid.Text("Help - Press [q]/[h]/[Esc] to close | Arrow/PgUp/PgDn to scroll", align="center")
        title = urwid.AttrMap(title, 'help_title')

        frame = urwid.Frame(help_widget, header=title)
        # Wrap frame with help_frame_bg attribute
        frame = urwid.AttrMap(frame, 'help_frame_bg')
        frame = urwid.LineBox(frame)
        # Wrap LineBox with help_outer_bg attribute for outermost layer
        frame = urwid.AttrMap(frame, 'help_outer_bg')

        # Overlay on top of main window
        overlay = urwid.Overlay(
            frame,
            self._w,
            align='center',
            width=('relative', 80),
            valign='middle',
            height=('relative', 80),
        )

        return overlay


# =============================================================================
# Model and Controller
# =============================================================================

class UIModel:
    """Data model for UI - handles gRPC communication"""

    def __init__(self, address='localhost:50051'):
        self.data = None
        self.channel = grpc.insecure_channel(address)
        self.stub = ezio_pb2_grpc.EZIOStub(self.channel)
        self.update_data()

    def update_data(self):
        """Fetch latest torrent status from server"""
        request = ezio_pb2.UpdateRequest()
        result = self.stub.GetTorrentStatus(request)
        self.data = result

    def get_data(self):
        """Get current data"""
        return self.data

    def get_version(self):
        """Get EZIO version"""
        request = ezio_pb2.Empty()
        result = self.stub.GetVersion(request)
        return result.version


class UIController:
    """Controller for coordinating model and view"""

    def __init__(self, min_finished_time=15, min_last_upload=15):
        self.animate_alarm = None
        self.update_alarm = None
        self.detect_alarm = None
        self.help_shown = False

        # Configuration
        self.min_finished_time = min_finished_time
        self.min_last_upload = min_last_upload

        self.model = UIModel()
        self.view = UIView(self)
        self.view.update_graph(True)

        self.loop = urwid.MainLoop(
            self.view,
            self.view.palette,
            unhandled_input=self.handle_input
        )

        self.update_data()
        self.animate_graph()
        self.detect_all_finished()

    def main(self):
        """Start the main loop"""
        self.loop.run()

    def handle_input(self, key):
        """Handle keyboard input"""
        # If help is shown, only specific keys close it
        if self.help_shown:
            if key in ('q', 'Q', 'h', 'H', 'esc'):
                self.help_shown = False
                self.loop.widget = self.view
            # Other keys are handled by ListBox for scrolling
            return

        # Normal key handling
        if key == 'q' or key == 'Q':
            raise urwid.ExitMainLoop()

        # Sort keys
        elif key == 's' or key == 'S':
            self.view.set_sort_mode(SortMode.PATH)
        elif key == 'n' or key == 'N':
            self.view.set_sort_mode(SortMode.NAME)
        elif key == 'p' or key == 'P':
            self.view.set_sort_mode(SortMode.PROGRESS)
        elif key == 'd' or key == 'D':
            self.view.set_sort_mode(SortMode.DOWNLOAD)
        elif key == 'u' or key == 'U':
            self.view.set_sort_mode(SortMode.UPLOAD)

        # Filter keys
        elif key == 'a' or key == 'A' or key == '0':
            self.view.set_filter_mode(FilterMode.ALL)
        elif key == '1':
            self.view.set_filter_mode(FilterMode.DOWNLOADING)
        elif key == '2':
            self.view.set_filter_mode(FilterMode.SEEDING)
        elif key == '3':
            self.view.set_filter_mode(FilterMode.FINISHED)

        # Display toggle keys
        elif key == 'v' or key == 'V':
            self.view.toggle_verbose()
        elif key == 'h' or key == 'H':
            self.help_shown = True
            self.loop.widget = self.view.show_help()

    def animate_graph(self, loop=None, user_data=None):
        """Update UI periodically"""
        self.view.update_graph()
        self.animate_alarm = self.loop.set_alarm_in(UPDATE_INTERVAL, self.animate_graph)

    def update_data(self, loop=None, user_data=None):
        """Fetch data from server periodically"""
        self.model.update_data()
        self.update_alarm = self.loop.set_alarm_in(UPDATE_INTERVAL, self.update_data)

    def detect_all_finished(self, loop=None, user_data=None):
        """Detect if all torrents finished and should shutdown"""
        try:
            data = self.model.get_data()
            stub = self.model.stub
            if not data or len(data.hashes) <= 0:
                raise ValueError("No Data")

            for info_hash in data.hashes:
                need_stop = False
                t_stat = data.torrents[info_hash]

                if t_stat.is_paused:
                    continue
                if not t_stat.is_finished:
                    continue

                # Stop conditions
                if t_stat.total_payload_upload > 3 * t_stat.total_done:
                    need_stop = True
                if t_stat.finished_time > self.min_finished_time and \
                   (t_stat.last_upload > self.min_last_upload or t_stat.last_upload == -1):
                    need_stop = True

                if need_stop:
                    request = ezio_pb2.PauseTorrentRequest()
                    request.hash = info_hash
                    stub.PauseTorrent(request)

            # Check if all stopped
            all_stop = True
            for k, t in data.torrents.items():
                if not t.is_finished or not t.is_paused:
                    all_stop = False
                    break

            if all_stop:
                # Shutdown server
                request = ezio_pb2.Empty()
                stub.Shutdown(request)

                # Wait for server cleanup
                time.sleep(15)
                sys.exit(0)

        except (ValueError, grpc.RpcError):
            pass

        self.detect_alarm = self.loop.set_alarm_in(10, self.detect_all_finished)

    def get_data(self):
        """Get current data from model"""
        return self.model.get_data()

    def get_version(self):
        """Get version from model"""
        return self.model.get_version()


# =============================================================================
# Main Entry Point
# =============================================================================

def main(min_finished_time=15, min_last_upload=15):
    """Main entry point"""
    UIController(min_finished_time, min_last_upload).main()


if __name__ == '__main__':
    import argparse

    parser = argparse.ArgumentParser(
        description='EZIO UI - Advanced BitTorrent monitoring interface'
    )
    parser.add_argument(
        "-w", "--wait",
        default=15,
        type=int,
        help="Minimum finished time before auto-pause (seconds)"
    )
    parser.add_argument(
        "-l", "--last-upload",
        default=15,
        type=int,
        help="Minimum time since last upload before auto-pause (seconds)"
    )
    args = parser.parse_args()

    main(min_finished_time=args.wait, min_last_upload=args.last_upload)

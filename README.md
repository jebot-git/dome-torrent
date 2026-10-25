# Dome Torrent

<p align="center"><img src="packaging/org.domed.Torrent.svg" width="96" height="96" alt="Dome Torrent icon"></p>

A native FLTK BitTorrent client for the de-xr suite, powered by libtorrent
Rasterbar 2. Magnet links are a primary input: paste a link, choose a download
folder, and press **Add** or Enter. No `.torrent` file is required. v1 `btih`,
base32 info hashes, v2 `btmh` and hybrid torrents use libtorrent's magnet parser
and metadata exchange. Peer discovery uses DHT, magnet trackers and peer exchange.
Metadata availability depends on reachable peers; an unresolved link stays in the
transfer list and survives restart.

Open `.torrent` files with **Torrent...**, choose a folder and press **Add**.
**Add paused** allows inspection before downloading. Select a transfer to inspect
its info hash, files, destination, progress and transfer rates. Pause, resume,
recheck, copy its magnet link, open its folder, or remove the transfer while
keeping its downloaded files. Completed transfers seed until paused or removed.

The GPU FLTK window follows DomeD's live appearance and Unicode BDF fonts. It
uses native Wayland surfaces and the universal menu, with toolbar controls and
keyboard shortcuts available in the window. It has no local menu bar. X11
appmenu association is an explicit build option; the FLTK toolkit's X11 backend
remains available for testing or use outside Wayland. DomeD can present the
ordinary application surface in desktop and XR compositors; this client has no
separate spatial content or headset-specific renderer.

The StatusNotifierItem registers with DomeD's tray manager, exports a theme-tinted
icon and shares the universal menu. Closing the window leaves transfers running
when a tray watcher is available. Otherwise closing saves and exits. **Quit**
(Ctrl+Q) always saves and exits. Ctrl+L focuses magnet input, Ctrl+O selects a
torrent, Ctrl+W closes to the tray, and Delete removes the selected transfer
with confirmation. `--tray` starts hidden, showing the window if no watcher
appears within two seconds.

## Build

Dependencies: a C++17 compiler, CMake, Ninja, the de-xr FLTK **1.4.5** toolkit
(with its GPU widget driver), libtorrent Rasterbar **2.0+**, Boost headers,
OpenSSL, Fontconfig, nlohmann-json, GIO, Wayland development files and
`wayland-scanner`. This project has no Qt dependency. Install development
packages through your system's package manager separately; the build does not
install dependencies or modify the desktop session.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DFLTK_DIR=/opt/fltk-vr/toolkit/share/fltk
cmake --build build -j4
ctest --test-dir build --output-on-failure
build/dome-torrent
build/dome-torrent 'magnet:?xt=urn:btih:YOUR_INFO_HASH'
```

A private libtorrent prefix can be selected with
`-DLibtorrentRasterbar_DIR=/path/to/prefix/lib64/cmake/LibtorrentRasterbar`.
For explicit X11 appmenu integration add `-DDOME_TORRENT_ENABLE_X11=ON`.
Release is the default build configuration. Build as a regular user, then stage:

```sh
DESTDIR="$PWD/stage" cmake --install build --prefix /usr/local
```

For a prepared system installation, create a checksum manifest and use the
installer only after verification:

```sh
python3 - <<'PYTHON'
from pathlib import Path
import hashlib, json
stage = Path('stage')
files = {str(p.relative_to(stage)): hashlib.sha256(p.read_bytes()).hexdigest()
         for p in (stage / 'usr/local').rglob('*') if p.is_file()}
(stage / 'install-manifest.json').write_text(json.dumps(files, indent=2) + '\n')
PYTHON
sudo -A python3 tools/install-system.py "$PWD/stage"
```

The installer verifies the manifest, backs up replaced files, installs atomically,
refreshes desktop/icon caches and checks installed hashes. It never builds as
root or restarts applications or sessions. `--with-domed` additionally installs
a reviewed, prebuilt `usr/local/bin/dome-compositor` and
`usr/local/share/domed/link-handlers.json` from the same stage. The new settings
categories take effect in the next DomeD session. `--runtime-rpm /path/to/runtime.rpm`
can install a local libtorrent RPM when its SHA-256 is recorded under
`runtime-rpm` in the manifest.

The staged application needs libtorrent at runtime, as well as the de-xr FLTK
libraries in `/opt/fltk-vr/toolkit/lib`. A private dependency may be supplied via
`LD_LIBRARY_PATH` when verifying a stage. Install system files only after
reviewing the stage. No session or display-manager restart is needed.

## Magnet and desktop dispatch

```sh
dome-torrent [--tray] [--local-only] [--] [magnet-link | file.torrent ...]
```

The desktop entry advertises `x-scheme-handler/magnet` and
`application/x-bittorrent`, with `TryExec=dome-torrent` to exclude an unavailable client. It accepts multiple links and local file URLs.
GApplication forwards subsequent launcher invocations to the running client;
relative filenames retain the launcher's working directory. When CMake finds `dome-link`, the installed desktop entry invokes it with
`--app-id org.domed.Torrent` before forwarding input, so an existing window follows
DomeD's active desktop/XR realm. Without the helper it launches directly.
`DOME_TORRENT_LINK_HELPER` can select a helper explicitly (set it to an empty value
to generate a plain launcher). DomeD's handler registry also recognizes the entry.

DomeD's **Default Applications** pane offers **BitTorrent files (.torrent)** and
**Magnet links** separately. Installing the client makes it the initial Dome
recommendation for either category without an existing Dome preference. Explicit
choices, generic user preferences and other desktops' defaults are preserved.
To explicitly select Dome Torrent after installing:

```sh
xdg-mime default org.domed.Torrent.desktop x-scheme-handler/magnet
xdg-mime default org.domed.Torrent.desktop application/x-bittorrent
```

`--local-only` disables DHT, local discovery and automatic port mapping, and
listens only on loopback. It is a test mode, not a network sandbox: supplied
tracker URLs or known peers can still be contacted. Normal mode listens on
TCP/UDP port 6881 for IPv4 and IPv6 and enables DHT, LSD, UPnP and NAT-PMP.
No firewall changes are made.

## State and shutdown

The transfer catalog consists of libtorrent resume files under
`$XDG_STATE_HOME/de-xr/dome-torrent`, falling back to
`~/.local/state/de-xr/dome-torrent`. The directory is mode 0700 and resume files
are mode 0600. A process lock prevents concurrent writers. Download folders are
stored in the resume data; the last chosen folder is kept separately in
`$XDG_CONFIG_HOME/de-xr/dome-torrent/settings.json`. The initial default is
`~/Downloads`.

The client records a magnet before receiving metadata. It checkpoints every
30 seconds and after metadata, completion and pause/resume changes. Atomic file
replacement and file/directory synchronization preserve the previous checkpoint
if a write fails. Quit drains outstanding asynchronous resume alerts, including
updates requested while a save is in flight, for up to ten seconds. Timeout or
save failures are reported; the previous checkpoint remains available. SIGINT
and SIGTERM use the same shutdown path. A forced kill can lose changes since the
last checkpoint. Payload files are never deleted by the Remove command.

All libtorrent calls and state I/O run on a worker thread. The UI reads snapshots;
it remains responsive during discovery, hashing, disk work and shutdown. Invalid
links, duplicate torrents, file errors, and corrupt checkpoints report errors
without terminating other transfers. This first version does not include
selective file download, torrent creation, proxies, bandwidth limits, scheduling,
or a remote-control API.

## Verification

`ctest` uses disposable folders and loopback peers with public discovery disabled.
It verifies actual metadata exchange and byte-exact downloads from v1, v2 and
hybrid magnets, duplicate and invalid inputs, pause/resume, recheck, checkpoint
restoration, pending magnets, a writer lock, corrupt checkpoint isolation, file
input, and removal without deleting payloads.

```sh
python3 tests/ui.py build/dome-torrent
python3 tests/wayland.py
```

UI QA requires Xvfb, Python Xlib, Pillow, PyGObject, `xdotool`, `gdbus` and a
session bus. It checks real magnet field input, invalid links, Dome MIME dispatch
of torrent files and magnets to an existing instance, universal commands, tray
registration and restoration,
checkpoint permissions and restart. Wayland QA additionally requires a built
`../DomeD/target/release/dome-compositor` (override with
`DOME_TORRENT_COMPOSITOR`). It starts a disposable nested compositor, checks
native app ID, appmenu surface association and Dome link forwarding, captures a screenshot and tests
tray restoration and clean shutdown. Neither test touches the active desktop
session or contacts public swarms. Artifacts go to `build/test-artifacts`.

## Application icon

The application icon depicts three peers feeding a download arrow into a tray.
Its scalable SVG and 16–256 pixel PNG versions use the de-xr stroke vocabulary.
The window and StatusNotifierItem render the same glyph in the current theme's
foreground color. To regenerate the PNG sizes after updating the SVG and native
glyph, run `python3 tools/render-icons.py` with CairoSVG installed.

DomeD integration is included as a focused [patch and instructions](integration/README.md).

## License

GPL-3.0-or-later. The reused de-xr appearance, BDF font, GPU window, icon and
universal-menu adapters retain their MIT notices. The tray adapter retains its
GPL-2.0-or-later notice. libtorrent is an external dependency under its own BSD
license. Its [magnet and resume APIs](https://www.libtorrent.org/tutorial.html)
provide the metadata and persistence foundation.

#!/usr/bin/env python3
"""Install a reviewed release stage; never compile or restart sessions as root."""
import argparse
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('stage', type=Path)
parser.add_argument('--runtime-rpm', type=Path, help='Optional local libtorrent runtime RPM to install with dnf')
parser.add_argument('--with-domed', action='store_true', help='Install the staged DomeD compositor and handler registry too')
args = parser.parse_args()
if os.geteuid() != 0:
    parser.error('Run with sudo -A after building and verifying the release stage as a regular user')
os.umask(0o022)
stage = args.stage.resolve()
files = ['usr/local/bin/dome-torrent',
         'usr/local/share/applications/org.domed.Torrent.desktop',
         'usr/local/share/icons/hicolor/scalable/apps/org.domed.Torrent.svg',
         'usr/local/share/doc/dome-torrent/README.md',
         'usr/local/share/doc/dome-torrent/LICENSE']
files += [f'usr/local/share/icons/hicolor/{size}x{size}/apps/org.domed.Torrent.png'
          for size in (16, 24, 32, 48, 64, 128, 256)]
if args.with_domed:
    files += ['usr/local/bin/dome-compositor', 'usr/local/share/domed/link-handlers.json']
manifest = json.loads((stage / 'install-manifest.json').read_text())
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
for name in files:
    source = stage / name
    if source.is_symlink() or not source.is_file() or manifest.get(name) != digest(source):
        raise SystemExit('Missing or changed staged artifact: ' + name)
if args.runtime_rpm:
    runtime = args.runtime_rpm.resolve()
    if manifest.get('runtime-rpm') != digest(runtime):
        raise SystemExit('Runtime RPM differs from the reviewed manifest')
    subprocess.run(['rpmkeys', '--checksig', str(runtime)], check=True)

with open('/run/lock/domed-install.lock', 'w') as lock:
    fcntl.flock(lock, fcntl.LOCK_EX)
    backup = Path('/var/backups/dome-torrent') / datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    backup.mkdir(parents=True, mode=0o700)
    for name in files:
        target = Path('/') / name
        if target.exists() or target.is_symlink():
            saved = backup / name
            saved.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(target, saved, follow_symlinks=False)
    (backup / 'manifest.json').write_text(json.dumps({'files': files, 'sha256': manifest}, indent=2) + '\n')
    if args.runtime_rpm:
        subprocess.run(['dnf', 'install', '-y', '--setopt=install_weak_deps=False', str(runtime)], check=True)
    for name in files:
        target = Path('/') / name
        target.parent.mkdir(parents=True, exist_ok=True)
        fd, temporary = tempfile.mkstemp(prefix='.dome-torrent-install-', dir=target.parent)
        try:
            with os.fdopen(fd, 'wb') as output:
                output.write((stage / name).read_bytes())
                os.fchmod(output.fileno(), 0o755 if '/bin/' in name else 0o644)
                output.flush()
                os.fsync(output.fileno())
            # Atomic replacement leaves a running executable's inode intact.
            os.replace(temporary, target)
        finally:
            Path(temporary).unlink(missing_ok=True)
    subprocess.run(['update-desktop-database', '/usr/local/share/applications'], check=True)
    cache = shutil.which('gtk-update-icon-cache') or shutil.which('gtk4-update-icon-cache')
    if cache:
        subprocess.run([cache, '-f', '-t', '/usr/local/share/icons/hicolor'], check=True)
    if shutil.which('restorecon'):
        subprocess.run(['restorecon', *['/' + name for name in files]], check=True)
    for name in files:
        if digest(Path('/') / name) != manifest[name]:
            raise SystemExit('Installed checksum mismatch: ' + name)
    print('Installed Dome Torrent system-wide. Backup:', backup)
    print('No application, desktop or display-manager session was restarted.')

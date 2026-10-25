# DomeD integration

`DomeD-torrent-handlers.patch` adds separate BitTorrent file and magnet categories
to DomeD's Default Applications pane. It recommends the installed client when no
Dome choice exists, preserves explicit choices, and registers its stable app ID
for desktop/XR routing and instance reuse. The patch includes backend, packaging,
and private compositor UI regression coverage.

The patch was prepared against DomeD commit
`a8aeb0db84ba6a893a4f0436bfde7b948f642cfe`. In the original de-xr workspace it
is already applied. For a separate checkout, from this repository:

```sh
git -C ../DomeD apply --check "$PWD/integration/DomeD-torrent-handlers.patch"
git -C ../DomeD apply "$PWD/integration/DomeD-torrent-handlers.patch"
python3 ../DomeD/scripts/test-default-apps.py
python3 ../DomeD/scripts/test-link-packaging.py
```

Build DomeD as a regular user using its release-build instructions, stage and
verify it, and install the reviewed binary without restarting the active session.
The new settings categories appear in the next compositor session. Installed
Dome Torrent already accepts files and magnet links through standard MIME launch
handling in an existing session.

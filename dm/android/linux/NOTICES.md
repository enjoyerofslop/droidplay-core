# DroidPlay @VERSION@ for Linux: what's inside and its licences

This AppImage carries DroidPlay Core and the software it runs on. Each part keeps its own licence; the full texts are in this folder (`licenses/` in
`~/.local/share/DroidPlay/@VERSION@/`, and `usr/share/doc/droidplay/licenses/` inside the AppImage). Nothing from any game is included.

## DroidPlay Core

[DroidPlay Core](https://github.com/enjoyerofslop/droidplay-core) @VERSION@ (`core/`), MIT licence: `DroidPlay-Core-LICENSE.txt`. Its source is the release's
tag on GitHub, and `core/` is that source as it is.

## Python @PYTHON_VERSION@

CPython @PYTHON_VERSION@ as built by [python-build-standalone](https://github.com/astral-sh/python-build-standalone) (release @PBS_BUILD@), in `runtime/`.

- CPython: the PSF License Agreement and the licences of the code CPython includes, in `Python-LICENSE.txt`. Source: https://www.python.org/downloads/source/
- The libraries python-build-standalone builds into it, each with its licence file in `python-build-standalone/` (taken from that build's own
  distribution): among them OpenSSL @OPENSSL_VERSION@ (Apache License 2.0, also in `Apache-2.0.txt`), SQLite @SQLITE_VERSION@ (public domain), libffi,
  XZ Utils, bzip2, zlib, zstd, libedit, ncurses, mpdecimal, libuuid and libb2. The build scripts and the sources they fetch are listed at
  https://github.com/astral-sh/python-build-standalone. Tcl/Tk (tkinter) is removed from this copy.

## Qt @QT_VERSION@ and Qt for Python (PySide6 @PYSIDE_VERSION@, Shiboken6)

`runtime/lib/python3.14/site-packages/PySide6` and `shiboken6`: the PySide6-Essentials and shiboken6 wheels from PyPI, unmodified, with the Qt modules
DroidPlay doesn't use removed (Qt Quick, QML, Designer, Network, SQL and the like).

- Licence: GNU Lesser General Public License version 3 (`LGPL-3.0.txt`, which adds to `GPL-3.0.txt`); Qt for Python is also offered under GPL-2.0
  (`GPL-2.0.txt`). Copyright The Qt Company Ltd. and other contributors.
- The Qt libraries are shared libraries, loaded from `runtime/lib/python3.14/site-packages/PySide6/Qt/lib`. You may replace them with your own build of
  the same Qt version: run the AppImage once (or `--appimage-extract` it) and swap the files in `~/.local/share/DroidPlay/@VERSION@/`.
- Source code: Qt @QT_VERSION@ at https://download.qt.io/official_releases/qt/ and https://code.qt.io/; Qt for Python @PYSIDE_VERSION@ at
  https://download.qt.io/official_releases/QtForPython/ and https://code.qt.io/cgit/pyside/pyside-setup.git/.
- Qt includes third-party code, listed with each licence at https://doc.qt.io/qt-6/licenses-used-in-qt.html. Of what this copy carries: ICU 73 (Unicode
  License v3, shipped as `libicu*.so.73`), PCRE2, double-conversion, HarfBuzz, libpng, libjpeg-turbo (the JPEG image plugin), md4c, TinyCBOR, forkfd,
  Pixman's drawing routines and the Unicode Character Database (BSD, MIT, zlib/libpng, IJG and Unicode licences).

## Python modules

The modules `scripts/nxnative_android.py.lock` pins, from their PyPI wheels; each one's licence files are under `python-modules/<module>/`:
@MODULES@

## X11 helper libraries

The xcb-util libraries, libxcb's extension libraries and libxkbcommon-x11 from Ubuntu 22.04 (`dm/android/linux/xcb_libs.tsv`), beside the Qt libraries:
MIT/X11 licences, each package's copyright file in `xcb/`. Source: https://xcb.freedesktop.org/ and https://xkbcommon.org/, and Ubuntu's source packages.

## DejaVu fonts

DejaVu Sans and DejaVu Sans Bold 2.37 (`core/dm/android/fonts`), which the build uses for a game's standard and button fonts when the system has neither
DejaVu nor Liberation: the Bitstream Vera licence with DejaVu's public-domain changes, in `DejaVu-LICENSE.txt`. Source: https://dejavu-fonts.github.io/

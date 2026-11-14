# DroidPlay @VERSION@ for Windows: what's inside and its licences

DroidPlay's Setup installs DroidPlay Core and the software it runs on. Each part keeps its own licence; the full texts are in this folder (`licenses\` in
`%LOCALAPPDATA%\Programs\DroidPlay\`). Nothing from any game is included. The tools Install what's missing downloads later (Microsoft's JDK and .NET SDK,
Google's Android SDK) come from their publishers under their own licences, and aren't part of Setup.

## DroidPlay Core

[DroidPlay Core](https://github.com/enjoyerofslop/droidplay-core) @VERSION@ (`core\`), MIT licence: `DroidPlay-Core-LICENSE.txt`. Its source is the release's
tag on GitHub, and `core\` is that source as it is.

## Python @PYTHON_VERSION@

CPython @PYTHON_VERSION@ as built by [python-build-standalone](https://github.com/astral-sh/python-build-standalone) (release @PBS_BUILD@), in `runtime\`.

- CPython: the PSF License Agreement and the licences of the code CPython includes, in `Python-LICENSE.txt`. Source: https://www.python.org/downloads/source/
- The libraries python-build-standalone builds into it, each with its licence file in `python-build-standalone\` (taken from that build's own
  distribution): among them OpenSSL @OPENSSL_VERSION@ (Apache License 2.0, also in `Apache-2.0.txt`), SQLite @SQLITE_VERSION@ (public domain), libffi,
  XZ Utils, bzip2, zlib, zstd and mpdecimal. The build scripts and the sources they fetch are listed at
  https://github.com/astral-sh/python-build-standalone. Tcl/Tk (tkinter) is removed from this copy.
- `vcruntime140.dll` and `vcruntime140_1.dll`: Microsoft's Visual C++ runtime, which Python is built against, redistributed as Microsoft allows.

## Qt @QT_VERSION@ and Qt for Python (PySide6 @PYSIDE_VERSION@, Shiboken6)

`runtime\Lib\site-packages\PySide6` and `shiboken6`: the PySide6-Essentials and shiboken6 wheels from PyPI, unmodified, with the Qt modules DroidPlay
doesn't use removed (Qt Quick, QML, Designer, Network, SQL and the like).

- Licence: GNU Lesser General Public License version 3 (`LGPL-3.0.txt`, which adds to `GPL-3.0.txt`); Qt for Python is also offered under GPL-2.0
  (`GPL-2.0.txt`). Copyright The Qt Company Ltd. and other contributors.
- The Qt libraries are DLLs (`Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Widgets.dll`, `Qt6Svg.dll` and the plugins under `plugins\`), loaded from
  `runtime\Lib\site-packages\PySide6`. You may replace them with your own build of the same Qt version.
- Source code: Qt @QT_VERSION@ at https://download.qt.io/official_releases/qt/ and https://code.qt.io/; Qt for Python @PYSIDE_VERSION@ at
  https://download.qt.io/official_releases/QtForPython/ and https://code.qt.io/cgit/pyside/pyside-setup.git/.
- Qt includes third-party code, listed with each licence at https://doc.qt.io/qt-6/licenses-used-in-qt.html. Of what this copy carries: PCRE2,
  double-conversion, HarfBuzz, libpng, libjpeg-turbo (the JPEG image plugin), md4c, TinyCBOR, Pixman's drawing routines and the Unicode Character Database
  (BSD, MIT, zlib/libpng, IJG and Unicode licences).
- The Microsoft Visual C++ runtime DLLs the wheel carries beside Qt (`msvcp140*.dll`, `vcruntime140*.dll`, `concrt140.dll` and the like), redistributed as
  Microsoft allows.

## Python modules

The modules `scripts\nxnative_android.py.lock` pins, from their PyPI wheels; each one's licence files are under `python-modules\<module>\`:
@MODULES@

# Stages DroidPlay for Windows as one self-contained folder: the input of its installer, and a DroidPlay that runs from where it is. From a checkout, on
# Windows with uv (astral.sh) on PATH or in %USERPROFILE%\.local\bin:
#   powershell -ExecutionPolicy Bypass -File dm\android\windows\stage.ps1 -Out <folder> [-Python 3.14.x] [-Version <x.y.z>] [-NoLaunch]
# It makes:
#   <folder>\runtime         Python (python-build-standalone, the build uv installs and the uv install of DroidPlay has run on) with the modules that
#                            scripts\nxnative_android.py.lock pins, precompiled. A real python.exe beside pythonw.exe: the packing script starts Python
#                            children everywhere (sys.executable scripts, a process pool, CMake's FindPython), which a frozen app would have to imitate.
#                            Its site-packages load its OpenSSL by full path as each Python starts (_droidplay_dlls.py)
#   <folder>\core            the tree of HEAD without docs\ and .github\, as a release's source archives have it, with VERSION (the phone app's version)
#   <folder>\DroidPlay.ico   drawn by the app (--write-icon)
#   <folder>\licenses        the licences of all of it, and NOTICES.md naming each part, gathered as the AppImage's are (dm\android\licenses\collect.py)
# then checks it: --check's Python rows, the icon, the native modules under Setup's DLL policy (check_runtime_dlls.py), and the window opened offscreen
# for a few seconds (-NoLaunch leaves that out: the window reads and writes this user's DroidPlay settings, so leave it out on a computer someone uses).
# The sizes it prints are the ones the installer has to carry.
param([Parameter(Mandatory = $true)] [string] $Out, [string] $Python = '3.14', [string] $Version = '', [switch] $NoLaunch)
$ErrorActionPreference = 'Stop'; $ProgressPreference = 'SilentlyContinue'
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$Tar = Join-Path $env:SystemRoot 'System32\tar.exe'   # Windows' own: a Git or MSYS tar earlier on PATH reads C: as a host name
function Say($m) { Write-Host "stage: $m" }
function Run($exe) {   # a native command and its arguments; its failure stops the script (PowerShell 5.1 has no $PSNativeCommandUseErrorActionPreference)
    & $exe @args; if ($LASTEXITCODE) { throw "$(Split-Path -Leaf $exe) $($args -join ' ') failed (exit $LASTEXITCODE)" }
}
$Uv = (Get-Command uv -ErrorAction SilentlyContinue).Source
if (-not $Uv) { $Uv = Join-Path $env:USERPROFILE '.local\bin\uv.exe' }
if (-not (Test-Path $Uv)) { throw 'uv was not found: install it (https://docs.astral.sh/uv/) or put it on PATH' }

if (Test-Path $Out) { Remove-Item -LiteralPath $Out -Recurse -Force }
$work = Join-Path $Out '.work'
New-Item -ItemType Directory -Force $Out, $work | Out-Null
$Out = (Resolve-Path $Out).Path; $work = (Resolve-Path $work).Path

# ---- core: the tree as a release ships it. Through a file, not a pipe: PowerShell 5.1 pipes native output as text and would break the archive
Say 'core: the tree of HEAD without docs and .github'
$core = Join-Path $Out 'core'; New-Item -ItemType Directory -Force $core | Out-Null
Run git -C $Repo archive --format=tar -o (Join-Path $work 'core.tar') HEAD ':!docs' ':!.github'
Run $Tar -xf (Join-Path $work 'core.tar') -C $core
if (-not $Version) { $Version = (& git -C $Repo describe --tags --always).Trim() }
$Version = $Version -replace '^v', ''   # a release's tag, v0.5.0, is version 0.5.0
[IO.File]::WriteAllText((Join-Path $core 'VERSION'), "$Version`n")   # no BOM, \n: build_android_from_nsp.py core_version reads it as it is

# ---- runtime: uv's Python, installed into a folder of this stage (not the user's), then copied whole: it is relocatable
Say "runtime: Python $Python"
$env:UV_PYTHON_INSTALL_DIR = Join-Path $work 'pythons'; $env:UV_PYTHON_BIN_DIR = Join-Path $work 'bin'; $env:UV_PYTHON_PREFERENCE = 'only-managed'
Run $Uv python install --quiet $Python
# the folder named with the whole version: uv adds a link named with the minor one (cpython-3.14-...) beside it
$found = @(Get-ChildItem (Join-Path $work 'pythons') -Directory | Where-Object { $_.Name -match '^cpython-\d+\.\d+\.\d+-windows-x86_64-none$' })
if ($found.Count -ne 1) { throw "uv python install left $($found.Count) Windows Pythons in $work\pythons" }
$runtime = Join-Path $Out 'runtime'
Copy-Item -LiteralPath $found[0].FullName -Destination $runtime -Recurse
Get-ChildItem $runtime -Recurse -Filter 'EXTERNALLY-MANAGED' | Remove-Item -Force   # a private Python: pip may add a module to it
$py = Join-Path $runtime 'python.exe'
foreach ($exe in 'python.exe', 'pythonw.exe') { if (-not (Test-Path (Join-Path $runtime $exe))) { throw "the runtime has no $exe" } }

Say 'runtime: the modules the lock pins'
$req = Join-Path $work 'requirements.txt'
Run $Uv export --quiet --script (Join-Path $Repo 'scripts\nxnative_android.py') --format requirements.txt --no-header -o $req
Run $Uv pip install --quiet --python $py --break-system-packages --no-cache -r $req
# the launchers pip leaves in Scripts\ name this stage's python.exe, which won't exist where the folder is installed; the app uses none of them
Get-ChildItem (Join-Path $runtime 'Scripts') -Filter '*.exe' -ErrorAction SilentlyContinue | Remove-Item -Force

# ---- what DroidPlay doesn't use, out of the runtime: about two thirds of PySide6-Essentials (Qt Quick and QML, Designer, Qt's tools, translations,
# a software OpenGL), pip, Tk and IDLE, the C headers. The app imports QtCore, QtGui, QtWidgets and QtSvg alone (nxnative_android.py): keep this list in
# step with its imports. The window check below fails on a module or a Qt library one of them needs that went
Say 'runtime: what DroidPlay does not use, out'
$pyside = Join-Path $runtime 'Lib\site-packages\PySide6'
$keep = @('__init__.py', '_config.py', '_git_pyside_version.py', 'py.typed', 'PySide6_Essentials.json', 'pyside6.abi3.dll', 'support', 'plugins', '__pycache__',
          'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Svg.dll', 'QtCore.pyd', 'QtGui.pyd', 'QtWidgets.pyd', 'QtSvg.pyd',
          'concrt140.dll', 'msvcp140*.dll', 'vcruntime140*.dll', 'vccorlib140.dll', 'vcamp140.dll', 'vcomp140.dll')   # the MSVC runtime Qt was built with
$plugins = @('platforms\qwindows.dll', 'platforms\qoffscreen.dll', 'imageformats\qjpeg.dll', 'imageformats\qico.dll', 'imageformats\qsvg.dll',
             'imageformats\qgif.dll', 'iconengines\qsvgicon.dll', 'styles\qmodernwindowsstyle.dll')   # the games' art is JPEG; --write-icon writes an .ico
Get-ChildItem -LiteralPath $pyside | Where-Object { $n = $_.Name; -not ($keep | Where-Object { $n -like $_ }) } | Remove-Item -Recurse -Force
Get-ChildItem -LiteralPath (Join-Path $pyside 'plugins') -Recurse -File | Where-Object { $plugins -notcontains $_.FullName.Substring($pyside.Length + 9) } | Remove-Item -Force
Get-ChildItem -LiteralPath (Join-Path $pyside 'plugins') -Directory | Where-Object { -not (Get-ChildItem -LiteralPath $_.FullName) } | Remove-Item -Force
foreach ($p in 'include', 'libs', 'tcl', 'Lib\idlelib', 'Lib\tkinter', 'Lib\turtledemo', 'Lib\ensurepip', 'DLLs\_tkinter.pyd', 'DLLs\tcl*.dll', 'DLLs\tk*.dll',
               'Lib\site-packages\pip', 'Lib\site-packages\pip-*') {
    Get-ChildItem -Path (Join-Path $runtime $p) -ErrorAction SilentlyContinue | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force }
}
# ---- the runtime's own OpenSSL, loaded by full path as each of its Pythons starts, ahead of a System32 copy (_droidplay_dlls.py says why)
$site = Join-Path $runtime 'Lib\site-packages'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '_droidplay_dlls.py') -Destination $site
[IO.File]::WriteAllText((Join-Path $site '_droidplay_dlls.pth'), "import _droidplay_dlls`n")
Run $py -m compileall -q -j 0 --invalidation-mode unchecked-hash (Join-Path $runtime 'Lib')   # read-only: no source to check against at each start
Remove-Item -LiteralPath $env:UV_PYTHON_INSTALL_DIR, $env:UV_PYTHON_BIN_DIR -Recurse -Force -ErrorAction SilentlyContinue
foreach ($v in 'UV_PYTHON_INSTALL_DIR', 'UV_PYTHON_BIN_DIR', 'UV_PYTHON_PREFERENCE') { Remove-Item "Env:$v" }

# ---- licences: each part's own files, and NOTICES.md naming the parts and their versions. The same texts as the AppImage (dm\android\licenses: the
# LGPL and GPL that Qt and PySide6 come under, Apache-2.0 for OpenSSL) and the same collector, so Setup no longer ships Qt without its notice
Say 'licences'
$lic = Join-Path $Out 'licenses'; New-Item -ItemType Directory -Force $lic | Out-Null
Copy-Item -LiteralPath (Join-Path $Repo 'LICENSE') -Destination (Join-Path $lic 'DroidPlay-Core-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $runtime 'LICENSE.txt') -Destination (Join-Path $lic 'Python-LICENSE.txt')
Copy-Item -Path (Join-Path $Repo 'dm\android\licenses\*.txt') -Destination $lic
$collect = Join-Path $Repo 'dm\android\licenses\collect.py'
Run $py -I $collect pbs $runtime (Join-Path $work 'downloads') (Join-Path $lic 'python-build-standalone')
$modules = & $py -I $collect modules $site (Join-Path $lic 'python-modules'); if ($LASTEXITCODE) { throw "collect.py modules failed (exit $LASTEXITCODE)" }
$facts = @{
    VERSION = $Version; PYTHON_VERSION = (& $py -c "import sys; print(*sys.version_info[:3], sep='.')"); PBS_BUILD = (Get-Content -Raw (Join-Path $runtime 'BUILD')).Trim()
    OPENSSL_VERSION = (& $py -c "import ssl; print(ssl.OPENSSL_VERSION.split()[1])"); SQLITE_VERSION = (& $py -c "import sqlite3; print(sqlite3.sqlite_version)")
    PYSIDE_VERSION = (& $py -c "import importlib.metadata as m; print(m.version('PySide6-Essentials'))"); QT_VERSION = (& $py -c "from PySide6.QtCore import qVersion; print(qVersion())")
    MODULES = ($modules -join "`n")
}
$saved = @{}; foreach ($k in $facts.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, [string]$facts[$k]) }
try { Run $py -I $collect notices (Join-Path $PSScriptRoot 'NOTICES.md') (Join-Path $lic 'NOTICES.md') }
finally { foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) } }   # VERSION is windows-package's own variable too
if (-not (Select-String -Path (Join-Path $lic 'NOTICES.md') -SimpleMatch 'LGPL-3.0.txt' -Quiet) -or -not (Test-Path (Join-Path $lic 'LGPL-3.0.txt'))) { throw 'the licences are incomplete' }
Say ('  ' + (@(Get-ChildItem -LiteralPath $lic -Recurse -File).Count) + ' files')

# ---- the icon, then the checks. UTF-8 mode, as the app gives its builds (nxnative's scripts read and write text without naming UTF-8)
$app = Join-Path $core 'scripts\nxnative_android.py'; $env:PYTHONUTF8 = '1'
Say 'checks: the icon, drawn offscreen'
$env:QT_QPA_PLATFORM = 'offscreen'
Run $py $app --write-icon (Join-Path $Out 'DroidPlay.ico')
if (-not (Test-Path (Join-Path $Out 'DroidPlay.ico'))) { throw 'the app wrote no icon' }

Say "checks: --check's Python rows"
$json = & $py (Join-Path $core 'scripts\build_android_from_nsp.py') --check
if ($LASTEXITCODE) { throw "--check failed (exit $LASTEXITCODE)" }
$rows = ($json -join "`n") | ConvertFrom-Json
$pyRows = @($rows | Where-Object { $_.name -like 'Python*' })
if (-not $pyRows) { throw '--check listed no Python rows' }
$bad = @($pyRows | Where-Object { -not $_.ok })
if ($bad) { throw ('the runtime lacks: ' + (($bad | ForEach-Object { $_.name }) -join ', ')) }
Say ('  ' + (($pyRows | ForEach-Object { $_.name -replace '^Python (module )?', '' }) -join ', ') + ': found')

Say 'checks: native modules in a process started as Setup starts DroidPlay (prefer System32 images)'
Run $py (Join-Path $PSScriptRoot 'check_runtime_dlls.py') $runtime

if (-not $NoLaunch) {
    Say 'checks: the window, offscreen, for 10 s'
    $log = Join-Path $work 'launch.log'
    $p = Start-Process -FilePath $py -ArgumentList "`"$app`"" -WorkingDirectory $core -RedirectStandardError $log -RedirectStandardOutput "$log.out" -PassThru -NoNewWindow
    Start-Sleep -Seconds 10
    if ($p.HasExited) { throw "the window closed by itself (exit $($p.ExitCode)): $(Get-Content -Raw $log)" }
    Run taskkill /T /F /PID $p.Id | Out-Null   # the app and the --check it started
}
Remove-Item Env:QT_QPA_PLATFORM

# ---- what the installer carries
Remove-Item -LiteralPath $work -Recurse -Force
function Get-Size($dir) { $f = @(Get-ChildItem -LiteralPath $dir -Recurse -File -Force); [pscustomobject]@{ Files = $f.Count; MB = [math]::Round(($f | Measure-Object Length -Sum).Sum / 1MB) } }
$sizes = [ordered]@{ runtime = Get-Size $runtime; core = Get-Size $core; total = Get-Size $Out }
foreach ($k in $sizes.Keys) { Say ('{0,-8} {1,6} files {2,6} MB' -f $k, $sizes[$k].Files, $sizes[$k].MB) }
Say "DroidPlay $Version staged in $Out"

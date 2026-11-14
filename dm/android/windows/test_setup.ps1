# Checks DroidPlay-Setup-<version>.exe (droidplay.iss) the way a player meets it, on a Windows with no DroidPlay of its own (CI's runner): never on a computer
# someone uses, since it installs over this user's DroidPlay, removes the one Install DroidPlay.cmd installed, and uninstalls.
#   powershell -ExecutionPolicy Bypass -File dm\android\windows\test_setup.ps1 -Setup <DroidPlay-Setup-x.y.z.exe>
# 1. A DroidPlay that Install DroidPlay.cmd installed (its folder, a desktop shortcut running uvw.exe), and some of the player's data, made up
# 2. Setup, silent: the files, the shortcuts with DroidPlay's AppUserModelID, the old install gone, --check's Python rows, the window opened offscreen
# 3. Setup again over a running DroidPlay: the Restart Manager closes it, its "Updating DroidPlay" window shows meanwhile, and /RELAUNCH opens the new one
# 4. The uninstaller, silent: the folder and the shortcuts gone, the player's data kept
param([Parameter(Mandatory = $true)] [string] $Setup)
$ErrorActionPreference = 'Stop'; $ProgressPreference = 'SilentlyContinue'
$Setup = (Resolve-Path $Setup).Path
$App = Join-Path $env:LOCALAPPDATA 'Programs\DroidPlay'; $Old = Join-Path $env:LOCALAPPDATA 'DroidPlay'; $Data = Join-Path $env:LOCALAPPDATA 'nxnative\android'
$Desk = [Environment]::GetFolderPath('Desktop'); $Menu = [Environment]::GetFolderPath('Programs')
$Logs = Join-Path ([IO.Path]::GetTempPath()) 'droidplay-setup-test'; New-Item -ItemType Directory -Force $Logs | Out-Null
function Say($m) { Write-Host "test: $m" }
function Check($ok, $what) { if (-not $ok) { throw "FAILED: $what" }; Say "ok: $what" }
function Run-Setup($name, $extra) {   # Setup alone is waited for: Start-Process -Wait waits for every process it starts too, such as the DroidPlay /RELAUNCH opens
    $p = Start-Process -FilePath $Setup -ArgumentList (@('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$Logs\$name.log`"") + $extra) -PassThru
    if (-not $p.WaitForExit(600000)) { throw "Setup ($name) didn't finish in 10 minutes" }
    if ($p.ExitCode) { Get-Content "$Logs\$name.log" -Tail 40 | Write-Host; throw "Setup ($name) exited $($p.ExitCode)" }
}
function Shortcut($lnk) { (New-Object -ComObject WScript.Shell).CreateShortcut($lnk) }
function Aumid($lnk) { (New-Object -ComObject Shell.Application).NameSpace((Split-Path $lnk)).ParseName((Split-Path -Leaf $lnk)).ExtendedProperty('System.AppUserModel.ID') }
function App-Processes { @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($App, [StringComparison]::OrdinalIgnoreCase) }) }

Say '1. a DroidPlay from Install DroidPlay.cmd, and the player''s data'
New-Item -ItemType Directory -Force (Join-Path $Old 'droidnative\scripts'), (Join-Path $Old 'downloads'), $Data | Out-Null
Set-Content (Join-Path $Old 'droidnative\scripts\nxnative_android.py') '# the uv install''s app'; Set-Content (Join-Path $Old 'version.txt') 'v0.4.0'
Set-Content (Join-Path $Data 'player.txt') 'the library, journals and tools stay'
$s = Shortcut (Join-Path $Desk 'DroidPlay.lnk'); $s.TargetPath = Join-Path $env:USERPROFILE '.local\bin\uvw.exe'; $s.Arguments = 'run --gui-script x'; $s.Save()

Say '2. Setup, silent'
Run-Setup 'install' @('/TASKS=desktopicon')
Check (-not (Select-String -Path "$Logs\install.log" -SimpleMatch 'Showing the update window' -Quiet)) 'no update window for an install'
foreach ($f in 'runtime\python.exe', 'runtime\pythonw.exe', 'core\scripts\nxnative_android.py', 'core\VERSION', 'DroidPlay.ico', 'licenses\NOTICES.md', 'licenses\LGPL-3.0.txt',
             'unins000.exe') { Check (Test-Path (Join-Path $App $f)) "$f installed" }
Check (-not (Test-Path (Join-Path $Old 'droidnative'))) 'the uv install''s folder removed'
Check (-not (Test-Path (Join-Path $Old 'version.txt'))) 'the uv install''s files removed'
foreach ($lnk in (Join-Path $Desk 'DroidPlay.lnk'), (Join-Path $Menu 'DroidPlay.lnk')) {
    $s = Shortcut $lnk
    Check ($s.TargetPath -eq (Join-Path $App 'runtime\pythonw.exe')) "$lnk runs the installed pythonw.exe"
    Check ($s.Arguments -like '*core\scripts\nxnative_android.py*') "$lnk opens the app"
    Check ((Aumid $lnk) -eq 'io.github.enjoyerofslop.droidplay.desktop') "$lnk carries DroidPlay's AppUserModelID"
}
$py = Join-Path $App 'runtime\python.exe'; $script = Join-Path $App 'core\scripts\nxnative_android.py'; $env:PYTHONUTF8 = '1'
$rows = ((& $py (Join-Path $App 'core\scripts\build_android_from_nsp.py') --check) -join "`n") | ConvertFrom-Json
Check ($LASTEXITCODE -eq 0 -and @($rows | Where-Object { $_.name -like 'Python*' -and -not $_.ok }).Count -eq 0 -and @($rows | Where-Object { $_.name -like 'Python*' }).Count -gt 0) '--check finds every Python module'
$env:QT_QPA_PLATFORM = 'offscreen'   # the window, without a desktop; the shortcuts' pythonw.exe and the app's taskbar identity were checked above
$w = Start-Process -FilePath (Join-Path $App 'runtime\pythonw.exe') -ArgumentList "`"$script`"" -WorkingDirectory (Join-Path $App 'core') -PassThru
Start-Sleep -Seconds 10
Check (-not $w.HasExited) 'the installed DroidPlay opens and stays open'

Say '3. Setup again, over the running DroidPlay'
Run-Setup 'update' @('/RELAUNCH')
Check $w.HasExited 'the running DroidPlay was closed for the update'
Check (Select-String -Path "$Logs\update.log" -SimpleMatch 'Showing the update window' -Quiet) 'the update showed its "Updating DroidPlay" window'
Start-Sleep -Seconds 5
$again = App-Processes | Where-Object { $_.Name -eq 'pythonw.exe' }
Check ($again.Count -ge 1) '/RELAUNCH opened DroidPlay again'
App-Processes | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Remove-Item Env:QT_QPA_PLATFORM

Say '4. the uninstaller, silent'
$u = Start-Process -FilePath (Join-Path $App 'unins000.exe') -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -PassThru
[void] $u.WaitForExit(120000)
for ($i = 0; $i -lt 120 -and (Test-Path $App); $i++) { Start-Sleep -Seconds 1 }   # the uninstaller works from a copy of itself, after the one started exits
Check (-not (Test-Path $App)) 'the installed folder removed'
Check (-not (Test-Path (Join-Path $Desk 'DroidPlay.lnk')) -and -not (Test-Path (Join-Path $Menu 'DroidPlay.lnk'))) 'the shortcuts removed'
Check (Test-Path (Join-Path $Data 'player.txt')) 'the player''s data kept'
Say ('passed; Setup is {0:n1} MB' -f ((Get-Item $Setup).Length / 1MB))

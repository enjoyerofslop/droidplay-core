# Stop for a build (scripts/nxnative_android.py): the packing script and every tool it started, not the script alone. A build runs cmake, ninja, dotnet,
# java, adb push and pools of Python workers; ending only the script left them running on Linux, holding the build folder and the phone, until they finished
# by themselves. Windows ends the whole tree with taskkill /T. Elsewhere the desktop app starts each build in a session of its own (QProcess's
# CreateNewSession), so the build leads a process group that its tools join, and the group is signalled. Without Qt, so its tests run without PySide6.
import os
import signal
import subprocess

NOWINDOW = 0x08000000 if os.name == 'nt' else 0   # CREATE_NO_WINDOW: taskkill, from a windowless app, opens no console


def stop_tree(pid, force=True):
    """Ends pid and what it started: the whole tree on Windows; its process group where pid leads one (a build the app started, or a console build, which a
    shell with job control starts as a group of its own), else pid alone, so a group that holds the shell or the app is never signalled. force, off Windows:
    SIGKILL, as taskkill /F; else SIGTERM, which cmake and ninja pass on to their own children. Windows always forces, as it did before"""
    if pid <= 0: return
    if os.name == 'nt':
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(pid)], capture_output=True, creationflags=NOWINDOW)
        return
    sig = signal.SIGKILL if force else signal.SIGTERM
    try:
        if os.getpgid(pid) == pid and pid != os.getpgid(0): os.killpg(pid, sig); return
    except OSError: pass   # gone already, or not ours
    try: os.kill(pid, sig)
    except OSError: pass

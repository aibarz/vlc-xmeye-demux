from pathlib import Path
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: add_external_control_preflight.py <build.sh>")

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
needle = 'info "Compiling"\nmake -j$JOBS'
replacement = (
    'info "Building VLC core prerequisites for XMEye external control"\n'
    'make -C compat -j$JOBS all\n'
    'make -C src -j$JOBS all\n'
    'make -C modules -j$JOBS module.rc.lo\n\n'
    'info "Compiling XMEye external control plugin only"\n'
    'make -C modules -j$JOBS libxmeyecontrol_plugin.la\n'
    'exit 0'
)

if needle not in text:
    raise SystemExit("compile marker not found")

path.write_text(text.replace(needle, replacement, 1), encoding="utf-8")

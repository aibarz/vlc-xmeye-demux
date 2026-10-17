from pathlib import Path
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: add_qt_preflight.py <build.sh>")

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
needle = 'info "Compiling"\nmake -j$JOBS'
replacement = (
    'info "Building VLC core prerequisites for XMEye Qt"\n'
    'make -C compat -j$JOBS all\n'
    'make -C src -j$JOBS all\n'
    'make -C modules -j$JOBS module.rc.lo\n'
    'UI_HEADERS=$(grep -o "gui/qt/ui/[A-Za-z0-9_./-]*\\.h" ../modules/gui/qt/Makefile.am | sort -u | tr "\\n" " ")\n'
    'make -C modules -j$JOBS $UI_HEADERS\n\n'
    'info "Compiling XMEye Qt preflight"\n'
    'make -C modules -j$JOBS gui/qt/dialogs/libqt_plugin_la-extended.lo\n\n'
    'info "Compiling XMEye Qt plugin only"\n'
    'make -C modules -j$JOBS libqt_plugin.la\n'
    'exit 0'
)

if needle not in text:
    raise SystemExit("compile marker not found")

path.write_text(text.replace(needle, replacement, 1), encoding="utf-8")

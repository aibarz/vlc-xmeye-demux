from pathlib import Path
import sys
if len(sys.argv)!=2: raise SystemExit('usage: add_avi_preflight.py <build.sh>')
p=Path(sys.argv[1]); s=p.read_text(encoding='utf-8')
needle='info "Compiling"\nmake -j$JOBS'
repl=('info "Building VLC core prerequisites for XMEye AVI"\n'
      'make -C compat -j$JOBS all\n'
      'make -C src -j$JOBS all\n'
      'make -C modules -j$JOBS module.rc.lo\n\n'
      'info "Compiling patched AVI plugin only"\n'
      'make -C modules -j$JOBS libavi_plugin.la\n'
      'exit 0')
if needle not in s: raise SystemExit('compile marker not found')
p.write_text(s.replace(needle,repl,1),encoding='utf-8')

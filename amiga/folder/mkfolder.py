#!/usr/bin/env python3
"""Assemble the ALICE68 folder for the SD card (amiga/out/ALICE68): the built
Amiga files, their ReadMe, Install and Uninstall for the Amiga's Installer
(with icons, mkinfo.py), the same as AmigaDOS scripts (Install-Shell,
Uninstall-Shell) for systems without it, and the volume's Disk.info - all made
from one file list.  amiga/folder/mkfolder.py OUT_DIR

alice68hdf.device mounts the folder as ALICE68: at boot. Install copies
each file to SYS: (TO=... for elsewhere) and, the first time only, keeps what
was there in SYS:Alice68-Backup - the file itself, or a NAME.none marker when
there was none - so that Uninstall can put the system back.
SPDX-License-Identifier: GPL-2.0-only"""
import os, shutil, sys

out = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
FILES = [                      # (built file in OUT_DIR, place in the system)
    ('Alice68-HDF', 'C'),
    ('Alice68-USBLog', 'C'),
    ('Alice68-NetCheck', 'C'),
    ('Alice68-ShowConfig', 'C'),
    ('Alice68-WiFi', 'C'),
    ('Alice68-Network', 'C'),
    ('Alice68-NetworkGT', 'C'),
    ('Alice68-Monitor', 'C'),
    ('alice68-usb.device', 'DEVS/USBHardware'),
    ('alice68net.device', 'DEVS'),
]
DATE = '07.11.2026'
VER = '1.1'
folder = os.path.join(out, 'ALICE68')
shutil.rmtree(folder, ignore_errors=True)
for name, place in FILES:
    os.makedirs(os.path.join(folder, place), exist_ok=True)
    shutil.copyfile(os.path.join(out, name), os.path.join(folder, place, name))
shutil.copyfile(os.path.join(here, 'ReadMe.txt'), os.path.join(folder, 'ReadMe.txt'))

T = '{TO$SYS:}'                # the system, as the script's TO argument gives it
B = T + 'Alice68-Backup/'
dirs = sorted({p for _, p in FILES} | {'DEVS'}, key=lambda p: p.count('/'))
head = ['.KEY TO/K', '.BRA {', '.KET }']

inst = head + [
    '; $VER: Install %s (%s) Alice68' % (VER, DATE),
    '; SPDX-License-Identifier: GPL-2.0-only',
    '; Made by amiga/folder/mkfolder.py: Alice68\'s Amiga files from ALICE68: into the system.',
    '; Execute ALICE68:Install [TO=<volume or drawer ending in : or />]',
    'FailAt 21',
    'If NOT EXISTS ALICE68:C',
    '  Echo "ALICE68: is not mounted: is the ALICE68 folder on the SD card?"',
    '  Quit 20',
    'EndIf',
    'If NOT EXISTS "%s"' % T,
    '  Echo "%s: not there"' % T,
    '  Quit 20',
    'EndIf',
    'Echo "Installing Alice68\'s Amiga files into %s"' % T,
    'If NOT EXISTS "%s"' % B[:-1],
    '  MakeDir "%s"' % B[:-1],
    'EndIf',
]
for p in dirs:
    inst += ['If NOT EXISTS "%s%s"' % (T, p), '  MakeDir "%s%s"' % (T, p), 'EndIf',
             'If NOT EXISTS "%s%s"' % (B, p), '  MakeDir "%s%s"' % (B, p), 'EndIf']
for name, place in FILES:
    f = '%s/%s' % (place, name)
    inst += [
        'If NOT EXISTS "%s%s"' % (B, f),
        '  If NOT EXISTS "%s%s.none"' % (B, f),
        '    If EXISTS "%s%s"' % (T, f),
        '      Copy "%s%s" "%s%s/" CLONE QUIET' % (T, f, B, place),
        '    Else',
        '      Echo >"%s%s.none" "not there before Alice68"' % (B, f),
        '    EndIf',
        '  EndIf',
        'EndIf',
        'Copy "ALICE68:%s" "%s%s/" CLONE QUIET' % (f, T, place),
        'If WARN',
        '  Echo "  %s: could not be copied"' % f,
        'Else',
        '  Echo "  %s"' % f,
        'EndIf',
    ]
inst += ['Echo "Done. What was there before is in %s"' % B[:-1],
         'Echo "Reboot so that the new devices are loaded."']

uninst = head + [
    '; $VER: Uninstall %s (%s) Alice68' % (VER, DATE),
    '; SPDX-License-Identifier: GPL-2.0-only',
    '; Made by amiga/folder/mkfolder.py: undoes ALICE68:Install from its backup.',
    'FailAt 21',
    'If NOT EXISTS "%s"' % B[:-1],
    '  Echo "No Alice68 backup in %s: nothing to undo"' % T,
    '  Quit 5',
    'EndIf',
]
for name, place in FILES:
    f = '%s/%s' % (place, name)
    uninst += [
        'If EXISTS "%s%s"' % (B, f),
        '  Copy "%s%s" "%s%s/" CLONE QUIET' % (B, f, T, place),
        '  Delete "%s%s" QUIET' % (B, f),
        '  Echo "  %s: put back"' % f,
        'Else',
        '  If EXISTS "%s%s.none"' % (B, f),
        '    Delete "%s%s" QUIET' % (T, f),
        '    Delete "%s%s.none" QUIET' % (B, f),
        '    Echo "  %s: removed"' % f,
        '  EndIf',
        'EndIf',
    ]
for p in sorted(dirs, key=lambda p: -p.count('/')):
    uninst.append('Delete >NIL: "%s%s" QUIET' % (B, p))
uninst += ['Delete >NIL: "%s" QUIET' % B[:-1], 'Echo "Done. Reboot to finish."']

inst[3] = inst[3].replace('Install ', 'Install-Shell ')
uninst[3] = uninst[3].replace('Uninstall ', 'Uninstall-Shell ')

# ---- the same for the Installer (Workbench: the icons start it) ----
def q(t): return '"' + t.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'
listing = '\n'.join('  %s/%s' % (pl, n) for n, pl in FILES)
ins = [
    '; $VER: Install %s (%s) Alice68' % (VER, DATE),
    '; SPDX-License-Identifier: GPL-2.0-only',
    '; For the Installer, made by amiga/folder/mkfolder.py: Alice68\'s Amiga files from',
    '; ALICE68: into the system. What it replaces is kept, the first time only, in',
    '; Alice68-Backup on the same volume, for Uninstall.',
    '(set @app-name "Alice68")',
    '(set @default-dest "SYS:")',
    '(if (= 0 (exists "ALICE68:C" (noreq)))',
    '  (abort %s))' % q("ALICE68: is not mounted.\n\nIs the folder ALICE68 on the SD card's\nFAT partition, next to kernel8.img?\n(and no alice68.folders=off in config.txt)"),
    '(welcome "Alice68 %s: the Amiga files")' % VER,
    '(message %s (all))' % q("This copies Alice68's Amiga files from\nALICE68: into your system:\n\n"
                              "eight tools into C:,\nalice68net.device into DEVS:,\nalice68-usb.device into DEVS:USBHardware.\n\n"
                              "What they replace is kept in\nAlice68-Backup, and Uninstall puts it back."),
    '(set #dest (askdir (prompt "Install into which system?") (help @askdir-help) (default @default-dest)))',
    '(set #bak (tackon #dest "Alice68-Backup"))',
    '(if (= 0 (exists #bak (noreq))) (makedir #bak))',
]
for p in dirs:
    ins.append('(if (= 0 (exists (tackon #dest %s) (noreq))) (makedir (tackon #dest %s)))' % (q(p), q(p)))
    ins.append('(if (= 0 (exists (tackon #bak %s) (noreq))) (makedir (tackon #bak %s)))' % (q(p), q(p)))
for i, (name, place) in enumerate(FILES):
    f = '%s/%s' % (place, name)
    ins += [
        '(if (= 0 (exists (tackon #bak %s) (noreq)))' % q(f),
        '  (if (= 0 (exists (tackon #bak %s) (noreq)))' % q(f + '.none'),
        '    (if (= 1 (exists (tackon #dest %s) (noreq)))' % q(f),
        '      (copyfiles (source (tackon #dest %s)) (dest (tackon #bak %s)) (nogauge) (optional "force"))' % (q(f), q(place)),
        '      (textfile (dest (tackon #bak %s)) (append "not there before Alice68\\n")))))' % q(f + '.none'),
        '(copyfiles (source %s) (dest (tackon #dest %s)) (nogauge) (optional "force"))' % (q('ALICE68:' + f), q(place)),
        '(complete %d)' % ((i + 1) * 100 // len(FILES)),
    ]
ins.append('(exit %s #bak %s)' % (q("Alice68's files are installed.\nWhat was there before is in\n"), q("\n\nReboot so that the new devices\nare loaded.")))

unins = [
    '; $VER: Uninstall %s (%s) Alice68' % (VER, DATE),
    '; SPDX-License-Identifier: GPL-2.0-only',
    '; For the Installer, made by amiga/folder/mkfolder.py: undoes Install from Alice68-Backup.',
    '(set @app-name "Alice68")',
    '(set @default-dest "SYS:")',
    '(welcome "Alice68 %s: remove the Amiga files")' % VER,
    '(set #dest (askdir (prompt "Remove Alice68\'s files from which system?") (help @askdir-help) (default @default-dest)))',
    '(set #bak (tackon #dest "Alice68-Backup"))',
    '(if (= 0 (exists #bak (noreq))) (abort "There is no Alice68-Backup on\\n" #dest ": nothing to undo."))',
    '(message %s (all))' % q("This puts back what Install replaced\nand removes the files that were not\nthere before."),
]
for i, (name, place) in enumerate(FILES):
    f = '%s/%s' % (place, name)
    unins += [
        '(if (= 1 (exists (tackon #bak %s) (noreq)))' % q(f),
        '  ((copyfiles (source (tackon #bak %s)) (dest (tackon #dest %s)) (nogauge) (optional "force"))' % (q(f), q(place)),
        '   (delete (tackon #bak %s) (optional "force")))' % q(f),
        '  (if (= 1 (exists (tackon #bak %s) (noreq)))' % q(f + '.none'),
        '    ((delete (tackon #dest %s) (optional "force"))' % q(f),
        '     (delete (tackon #bak %s) (optional "force")))))' % q(f + '.none'),
        '(complete %d)' % ((i + 1) * 100 // len(FILES)),
    ]
for p in sorted(dirs, key=lambda p: -p.count('/')):
    unins.append('(run (cat "Delete >NIL: \\"" (tackon #bak %s) "\\""))' % q(p))
unins.append('(run (cat "Delete >NIL: \\"" #bak "\\""))')
unins.append('(exit %s)' % q("Alice68's files are removed, and\nwhat they replaced is back.\n\nReboot to finish."))

def lisp_ok(fn, lines):
    depth, instr, esc = 0, False, False
    for l in lines:
        if any(ord(c) > 126 for c in l): sys.exit('%s: not ASCII: %s' % (fn, l))
        if l.startswith(';'): continue
        for c in l:
            if instr:
                if esc: esc = False
                elif c == '\\': esc = True
                elif c == '"': instr = False
            elif c == '"': instr = True
            elif c == '(': depth += 1
            elif c == ')':
                depth -= 1
                if depth < 0: sys.exit('%s: too many ) at %s' % (fn, l))
        if instr: sys.exit('%s: string runs past the line: %s' % (fn, l))
    if depth: sys.exit('%s: %d ( not closed' % (fn, depth))
    import re
    for l in lines:
        for t in re.findall(r'"((?:[^"\\]|\\.)*)"', l):
            for piece in t.split('\\n'):
                if len(piece) > 44 and not piece.startswith('Delete'): sys.exit('%s: a line the Installer would wrap: %s' % (fn, piece))
for fn, lines in (('Install', ins), ('Uninstall', unins)):
    lisp_ok(fn, lines)
    with open(os.path.join(folder, fn), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')

# ---- icons: the volume's, the scripts', the ReadMe's ----
sys.dont_write_bytecode = True             # keep the source tree clean
sys.path.insert(0, here)
import mkinfo
for name, (kind, draw, tool, tts, stack, pos) in mkinfo.ICONS.items():
    data = mkinfo.write_icon(os.path.join(folder, name + '.info'), kind, draw(), tool, tts, stack, pos)
    k, pic, t, tt, st = mkinfo.read_icon(data)
    if (k, t, tuple(tt), st) != (kind, tool, tuple(tts), stack) or pic.p != draw().p: sys.exit('%s.info does not read back' % name)

# ---- volume icons to copy onto drives (not installed): a hardfile's drive, a CD, an SD folder ----
import mkdiskicons
os.makedirs(os.path.join(folder, 'Icons'), exist_ok=True)
for name, label in (('hardfile', 'Hardfile'), ('cd', 'CD'), ('folder', 'Folder')):
    with open(os.path.join(folder, 'Icons', label + '.icon'), 'wb') as f:
        f.write(mkdiskicons.disk_icon(mkdiskicons.ICONS[name]))

for fn, lines in (('Install-Shell', inst), ('Uninstall-Shell', uninst)):
    # the Shell's rules: LF only, plain ASCII, Ifs closed, lines it can take
    depth = 0
    for l in lines:
        w = l.split()
        if w and w[0] == 'If': depth += 1
        if w and w[0] == 'EndIf': depth -= 1
        if depth < 0 or len(l) > 250 or any(ord(c) > 126 for c in l): sys.exit('%s: bad line: %s' % (fn, l))
    if depth: sys.exit('%s: If without EndIf' % fn)
    with open(os.path.join(folder, fn), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
print('ALICE68 folder: %d files, Install and Uninstall (Installer, with icons, and Shell), Disk.info' % len(FILES))

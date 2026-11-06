#!/usr/bin/env python3
"""Assemble the ALICE68 folder for the SD card (amiga/out/ALICE68): the built
Amiga files, their ReadMe, and an Install and Uninstall script for the Amiga
made from the same file list.  amiga/folder/mkfolder.py OUT_DIR

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
    ('Alice68-WiFi', 'C'),
    ('Alice68-Network', 'C'),
    ('Alice68-NetworkGT', 'C'),
    ('Alice68-Monitor', 'C'),
    ('alice68-usb.device', 'DEVS/USBHardware'),
    ('alice68net.device', 'DEVS'),
]
DATE = '07.11.2026'
VER = '1.0'
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

for fn, lines in (('Install', inst), ('Uninstall', uninst)):
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
print('ALICE68 folder: %d files, Install, Uninstall' % len(FILES))

#!/usr/bin/env python3
"""The volume icons alice68hdf.device shows as Disk.info on the volumes it
mounts that have none (folders.c: an SD folder; isos.c: a CD), drawn by
mkinfo.py, as C data for the device (OUT_DIR/disk_icons.c, read-only data in
the ROM module). Also the icons people can copy onto volumes themselves, for
the ALICE68 folder (mkfolder.py): a hardfile's drive, a CD, an SD folder.

    amiga/folder/mkdiskicons.py OUT_DIR
SPDX-License-Identifier: GPL-2.0-only"""
import os, sys
sys.dont_write_bytecode = True             # keep the source tree clean
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkinfo

def disk_icon(draw):
    show = SHOW.get(draw)
    data = mkinfo.write_icon(None, mkinfo.WBDISK, draw(), show=show)
    kind, pic, tool, tts, stack = mkinfo.read_icon(data)     # it reads back
    assert kind == mkinfo.WBDISK and mkinfo.drawer_show(data) == show
    return data

ICONS = {'folder': mkinfo.icon_sdfolder, 'cd': mkinfo.icon_cdrom, 'hardfile': mkinfo.icon_hardfile}
# CD images are mostly not Amiga discs (few .info files): their window shows all files
SHOW = {mkinfo.icon_cdrom: (mkinfo.DDFLAGS_SHOWALL, mkinfo.DDVM_BYICON)}

if __name__ == '__main__':
    out = sys.argv[1]
    with open(os.path.join(out, 'disk_icons.c'), 'w', newline='\n') as f:
        f.write('/* SPDX-License-Identifier: GPL-2.0-only\n   Volume icons (Disk.info), made by amiga/folder/mkdiskicons.py - do not edit. */\n')
        f.write('#include "../hdf/disk_icons.h"\n')
        for name in ('folder', 'cd'):
            b = disk_icon(ICONS[name])
            f.write('const unsigned char disk_icon_%s[] __attribute__((aligned(4))) = {\n' % name)
            for i in range(0, len(b), 12): f.write('  ' + ', '.join('0x%02x' % x for x in b[i:i + 12]) + ',\n')
            f.write('};\nconst unsigned long disk_icon_%s_len = %d;\n' % (name, len(b)))
    print('disk_icons.c: an SD folder\'s and a CD\'s Disk.info')

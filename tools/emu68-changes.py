#!/usr/bin/env python3
"""Write docs/EMU68-CHANGES.md: every file of Emu68 that Alice68's kernel has
changed, which project changed it (AGA-PiStorm, FRF's EMU68AGA Bare Metal,
Emu68's own development branch, Alice68) and why.

    python3 tools/emu68-changes.py          (run in the repository, after a merge)

The numbers come from git and from arm/emu68-aga.patch; the reasons are the
notes below, kept by hand: the script stops if a changed file has none.
SPDX-License-Identifier: GPL-2.0-only"""
import collections, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EMU = 'third_party/Emu68/'
BASE = '2b56a31'            # the upstream branch's import: Emu68 0ba3a89 + AGA-PiStorm 1.120's patch
FRF = '1b4c426'             # EMU68AGA Bare Metal v0.10.44, merged
EMU68_DEV = {'a3b1681'}     # Emu68 582253e (1_1_wip): the data-cache range opcodes

def git(*a):
    return subprocess.run(['git', '-C', ROOT] + list(a), capture_output=True, text=True, check=True).stdout

def layer(sha):
    if sha.startswith(FRF): return 'frf'
    if any(sha.startswith(x) for x in EMU68_DEV): return 'emu68'
    return 'alice'

# ---- why, per file ------------------------------------------------------------------------------
AGA_WHY = {
    'CMakeLists.txt': 'builds AGA-PiStorm\'s chipset core (`core/`, `arm/`) into the kernel',
    'include/M68k.h': 'one field: the interrupt level the virtual Paula asks for',
    'include/logo/logo_aga.h': 'AGA-PiStorm\'s boot logo (generated)',
    'include/logo/logo_aga_rgb.h': 'AGA-PiStorm\'s colour boot splash (generated)',
    'include/support_rpi.h': 'two declarations for the measured clock rate',
    'src/ExecutionLoop.c': 'interrupts from the virtual Paula; a counter of translation units run; the chipset core using the bus',
    'src/M68k_LINE4.c': 'RESET handling for the virtual machine',
    'src/M68k_MULDIV.c': 'an Emu68 fix for 64-bit MULx results (Emu68 `874c3a6`, carried by AGA-PiStorm)',
    'src/M68k_Translator.c': 'counts translations (for the sandbox\'s statistics)',
    'src/aarch64/mmu.c': 'reserves the virtual chip RAM and the GPU\'s room after the output frames',
    'src/aarch64/start.c': 'starts the chipset core on its own core; the virtual chip RAM; the RGB-port output',
    'src/aarch64/vectors.c': 'the custom chips\' registers ($DFFxxx) go to the virtual chipset; WHDLoad\'s hold on interrupts',
    'src/cache.c': 'cache handling for the virtual chip RAM',
    'src/pistorm/ps_classic_protocol.c': 'a locked bus API shared with the chipset core; Ctrl-Amiga-Amiga with the bus wedged',
    'src/pistorm/ps_protocol.c': 'the same for the PiStorm32 protocol',
    'src/raspi/start_rpi64.c': 'AGA-PiStorm\'s splash and logo',
    'src/raspi/support_rpi.c': 'asks the firmware for the clock rate it really measures',
    'toolchains/aarch64_be-arm-gnu.cmake': 'a toolchain file for the Arm GNU big-endian compiler',
}
WHY = {   # files changed since the import: (layer, what and why)
    'AGA-PISTORM-SOURCE.txt': [('frf', 'AGA-PiStorm\'s note about where its Emu68 came from; removed in the merge (it describes AGA-PiStorm\'s package, not this tree; `docs/PROVENANCE.md` says the same)')],
    'build-scripts/README.md': [('frf', 'Emu68\'s Docker build scripts, removed in the merge (Alice68 builds with `tools/build-alice68-pi4.sh`)')],
    'build-scripts/build-aarch64': [('frf', 'as above')],
    'build-scripts/build-m68k-amigaos': [('frf', 'as above')],
    'build-scripts/build-with-docker': [('frf', 'as above')],
    'CMakeLists.txt': [('frf', 'the `FRF_VIRTUAL_AGA` build: the standalone machine, TinyUSB, the virtual CIAs, the Zorro III RAM board'),
                       ('alice', 'builds Alice68\'s xHCI driver, Neptune pass-through, splash and network card (`src/alice/`)')],
    'include/logo/logo_aga_rgb.h': [('frf', 'FRF\'s boot picture in place of AGA-PiStorm\'s (generated)')],
    'include/mmu.h': [('frf', 'declares `mmu_unmap()`')],
    'src/ExecutionLoop.c': [('frf', 'standalone: no physical interrupt lines, only the virtual chipset\'s')],
    'src/M68k_LINE4.c': [('frf', 'the 68k RESET instruction resets the virtual machine (and re-runs autoconfig) instead of pulsing the PiStorm\'s reset line')],
    'cmake/verstring.cmake': [('alice', 'the version string names the kernel Alice68 (`ALICE_VERSION`), "based on Emu68" and its version; the `/emu68` device-tree node stays')],
    'src/M68k_LINEF.c': [('emu68', 'private LINE-F opcodes for data-cache maintenance over an address range (Emu68 `582253e`, Lukasz Winczura, from Emu68\'s `1_1_wip`), which rondoval\'s `-rangeops` drivers use')],
    'src/M68k_SR.c': [('emu68', 'as above: the supervisor check for those opcodes')],
    'src/aarch64/mmu.c': [('frf', 'mapping for the standalone machine'),
                          ('alice', 'the Pi 4: a 2 MB DMA pool below 1 GB for the xHCI driver (hidden from AmigaOS), and the Zorro III RAM board dropped where the Pi already has RAM at $40000000')],
    'src/aarch64/start.c': [('frf', 'starts the standalone machine: virtual CIAs, USB on CPU1, the ROM and ADF boot'),
                            ('emu68', 'the `/emu68` `dcache-range-ops` property for the range opcodes (placed by hand)'),
                            ('alice', 'starts Alice68\'s work on CPU1 (USB, the network card) and the splash, and stops the splash when AmigaOS draws')],
    'src/aarch64/vectors.c': [('frf', 'the virtual CIA-A and CIA-B, the standalone machine\'s view of the custom chips\' control registers'),
                              ('alice', 'which chipset control words a running machine may send (the PiStorm-only ones refused); each CIA\'s interrupt line kept for the chipset, so a cleared PORTS request is set again while the line is held')],
    'src/boards/68040.library.h': [('emu68', '68040.library 2.0, which uses the range opcodes (generated binary, from Emu68)')],
    'src/boards/emu68rom.c': [('alice', 'puts `alice68hdf.device` (hardfiles, SD-card folders) on Emu68\'s ROM board, beside Emu68\'s own drivers')],
    'src/overlays/diagnostic.dts': [('frf', 'a `hud` option for FRF\'s on-screen display')],
    'src/overlays/emu68.dts': [('frf', 'joystick button mapping options')],
    'src/pistorm/ps_classic_protocol.c': [('alice', 'no PiStorm on the header: the serial log goes out on GPIO 14 (header pin 8)')],
    'src/raspi/start_rpi64.c': [('frf', 'the standalone start: no PiStorm probe, the virtual machine set up'),
                                ('alice', 'the Pi 4 boot: the splash (status strip, log panels, animation; "based on Emu68" from the version string), the boot log, starting the xHCI driver and the network card')],
    'src/raspi/support_rpi.c': [('frf', 'the boot log on screen'),
                                ('alice', 'every kernel log line also into the USB card\'s log ring (for `Alice68-USBLog` and `Alice68-Monitor`)')],
}
NEW_WHY = {   # new files, by the start of their path: (what, whose)
    'third_party/tinyusb/': ('TinyUSB, the USB host stack FRF uses on the Pi\'s USB-C port (MIT)', 'frf'),
    'src/frf_usb/tinyusb/': ('FRF\'s TinyUSB glue: keyboards, mice, pads, USB mass storage', 'frf'),
    'src/frf_usb/native/': ('FRF\'s USB keyboard handling and native DWC2 code', 'frf'),
    'src/frf_usb/xhci/': ('Alice68\'s xHCI (VL805) driver for the Pi 4\'s USB-A ports, the Neptune pass-through (the USB card) and the virtual eject', 'alice'),
    'src/frf_usb/': ('FRF\'s USB board and joystick mapping', 'frf'),
    'src/alice/': ('Alice68\'s network card: WiFi (SDIO host, chip bring-up, FullMAC protocol) and Gigabit Ethernet (GENET)', 'alice'),
    'src/raspi/alice_splash': ('Alice68\'s boot splash', 'alice'),
    'src/boards/alice68hdf.device.h': ('`alice68hdf.device`, built from `amiga/hdf/` and embedded in the ROM board (generated)', 'alice'),
    'src/boards/frf_z3ram.c': ('FRF\'s Zorro III RAM board', 'frf'),
    'tests/': ('FRF\'s tests', 'frf'),
    'scripts/': ('FRF\'s build and test scripts, and its joystick map', 'frf'),
    'src/frf_net/': ('FRF\'s network bridge board and SDIO probe (built only with `FRF_NET_Z2_BRIDGE` / `FRF_WIFI_SD1_ENUM`, both off in Alice68)', 'frf'),
    'src/frf_wifi/': ('FRF\'s WiFi probes (not built in Alice68)', 'frf'),
    'include/logo/logo_alice.h': ('Alice68\'s emblem (generated by `tools/mklogo_alice.py`)', 'alice'),
}
NAMES = {'frf': 'FRF (EMU68AGA Bare Metal)', 'emu68': 'Emu68 `1_1_wip`', 'alice': 'Alice68'}

# ---- the numbers ----------------------------------------------------------------------------------
def aga_patch():
    p = open(os.path.join(ROOT, 'arm/emu68-aga.patch'), encoding='latin1').read()
    parts = re.split(r'^diff --git a/(\S+) b/\S+\n', p, flags=re.M)
    out = []
    for i in range(1, len(parts), 2):
        body = parts[i + 1].splitlines()
        add = sum(1 for l in body if l.startswith('+') and not l.startswith('+++'))
        rem = sum(1 for l in body if l.startswith('-') and not l.startswith('---'))
        out.append((parts[i], add, rem, any(l.startswith('new file mode') for l in body[:3])))
    return out

def per_layer():
    """{file: {layer: [added, removed]}} for every commit since BASE, merges left out"""
    res = collections.defaultdict(lambda: collections.defaultdict(lambda: [0, 0]))
    log = git('log', '--no-merges', '--numstat', '--format=@%h', BASE + '..HEAD', '--', EMU)
    cur = None
    for l in log.splitlines():
        if l.startswith('@'): cur = layer(l[1:]); continue
        f = l.split('\t')
        if len(f) == 3 and f[2].startswith(EMU):
            a = int(f[0]) if f[0] != '-' else 0
            r = int(f[1]) if f[1] != '-' else 0
            res[f[2][len(EMU):]][cur][0] += a
            res[f[2][len(EMU):]][cur][1] += r
    return res

status = {}
for l in git('diff', '--name-status', BASE, 'HEAD', '--', EMU).splitlines():
    st, f = l.split('\t')[0], l.split('\t')[-1]
    status[f[len(EMU):]] = st[0]
changed = sorted(f for f, s in status.items() if s in 'MD')
added = sorted(f for f, s in status.items() if s == 'A')
missing = [f for f in changed if f not in WHY]
if missing: sys.exit('no note for: %s (add it to WHY in tools/emu68-changes.py)' % ', '.join(missing))
aga = aga_patch()
layers = per_layer()
base_files = [f[len(EMU):] for f in git('ls-tree', '-r', '--name-only', BASE, '--', EMU).splitlines()]
aga_set = {f for f, _, _, _ in aga}
aga_new = {f for f, _, _, n in aga if n}
AGA_NOTE = {'AGA-PISTORM-SOURCE.txt'}      # AGA-PiStorm's own note, put in its Emu68 folder by the import
untouched = [f for f in base_files if f not in aga_set and f not in status]
head = git('rev-parse', '--short', 'HEAD').strip()

# ---- the document ----------------------------------------------------------------------------------
o = []
a = o.append
a('# What Alice68 changes in Emu68')
a('')
a('Alice68\'s kernel is [Emu68](https://github.com/michalsc/Emu68) by Michal Schulz (MPL-2.0), '
  'at upstream commit `0ba3a89` (1.1.0-beta.1), changed in four layers. This page lists every '
  'Emu68 file that is not as it is upstream, who changed it and why. Emu68\'s files keep their '
  'MPL-2.0 headers and the changes to them are MPL-2.0; new files say their own licence.')
a('')
a('Made by `tools/emu68-changes.py` from git, at Alice68 `%s`. To see any change itself:' % head)
a('')
a('- AGA-PiStorm\'s layer: `arm/emu68-aga.patch` (its patch against Emu68 `0ba3a89`, as released);')
a('- the other layers: `git diff %s HEAD -- third_party/Emu68/<file>`, or `git log %s..HEAD -- third_party/Emu68/<file>` for each commit.' % (BASE, BASE))
a('')
a('| Layer | What it is | Where |')
a('|---|---|---|')
a('| 1. **AGA-PiStorm** 1.120 (Astair86) | the virtual AGA chipset: Emu68 hooked to it | `arm/emu68-aga.patch`, applied on the `upstream` branch (`%s`) |' % BASE)
a('| 2. **FRF** EMU68AGA Bare Metal v0.10.44 (Future Retro Fusion) | the standalone machine: virtual CIAs, ROM and ADF boot, Zorro III RAM, TinyUSB on USB-C | commit `%s` |' % FRF)
a('| 3. **Emu68 `1_1_wip`** | an Emu68 change not yet in a release: data-cache range opcodes (`582253e`) | commit `a3b1681` |')
a('| 4. **Alice68** | the Pi 4 port, the USB card, the network card, the splash, `alice68hdf.device` on the ROM board | the commits since |')
a('')
n_aga = len(aga)
mod_since = [f for f in changed if status[f] == 'M']
emu_files = [f for f in base_files if f not in aga_new and f not in AGA_NOTE]   # Emu68's own, at 0ba3a89
touched = {f for f in emu_files if f in aga_set or f in status}
core = [f for f in layers if re.match(r'src/(M68k_[^/]*|ExecutionLoop)\.c$', f) and layers[f].get('alice')]
a('**In numbers:** Emu68 at `0ba3a89` has %d files; **%d of them are exactly as upstream**. '
  'AGA-PiStorm\'s patch changes %d of them and adds %d; since then %d files have been changed (%d of them also by '
  'AGA-PiStorm) and %d removed, and %d added - most of them TinyUSB and the new drivers, in files of their own.'
  % (len(emu_files), len(emu_files) - len(touched), n_aga - len(aga_new), len(aga_new), len(mod_since),
     len([f for f in mod_since if f in aga_set]), len([f for f in changed if status[f] == 'D']), len(added)))
a('')
if core:
    a('The 68k translator (`src/M68k_*.c`, `src/ExecutionLoop.c`) is changed by Alice68 in: %s.' % ', '.join('`%s`' % f for f in core))
else:
    a('**Alice68 has not changed the 68k translator** (`src/M68k_*.c`, `src/ExecutionLoop.c`): what differs there '
      'is AGA-PiStorm\'s, FRF\'s and Emu68\'s own development branch\'s (this sentence is checked by the script).')
a('')
a('## 1. AGA-PiStorm\'s patch (Emu68 `0ba3a89` to the `upstream` branch)')
a('')
a('| File | Lines | What and why |')
a('|---|---|---|')
for f, ad, rm, new in aga:
    a('| `%s`%s | +%d -%d | %s |' % (f, ' (new)' if new else '', ad, rm, AGA_WHY.get(f, '')))
a('')
a('## 2. Changed since: files that were there')
a('')
a('Lines per layer (added / removed), from each commit; a file changed by AGA-PiStorm too is marked *.')
a('')
a('| File | FRF | Emu68 `1_1_wip` | Alice68 | What and why |')
a('|---|---|---|---|---|')
for f in changed:
    L = layers.get(f, {})
    def cell(k):
        v = L.get(k)
        return '+%d -%d' % (v[0], v[1]) if v and (v[0] or v[1]) else ''
    why = '<br>'.join('**%s:** %s' % (NAMES[k].split(' ')[0] if k != 'emu68' else 'Emu68', t) for k, t in WHY[f])
    name = '`%s`%s%s' % (f, ' *' if f in aga_set else '', ' (removed)' if status[f] == 'D' else '')
    a('| %s | %s | %s | %s | %s |' % (name, cell('frf'), cell('emu68'), cell('alice'), why))
a('')
a('## 3. New files')
a('')
a('| Files | How many | Lines | Whose | What |')
a('|---|---|---|---|---|')
groups = collections.OrderedDict()
for f in added:
    key = next((k for k in sorted(NEW_WHY, key=len, reverse=True) if f.startswith(k)), None)
    if key is None: sys.exit('no note for the new file %s (add a NEW_WHY entry)' % f)
    groups.setdefault(key, []).append(f)
for key in sorted(groups, key=lambda k: (NEW_WHY[k][1] != 'alice', k)):
    fs = groups[key]
    lines = sum(sum(v[0] for v in layers.get(f, {}).values()) for f in fs)
    what, who = NEW_WHY[key]
    a('| `%s%s` | %d | %d | %s | %s |' % (key, '' if key.endswith('/') or key.endswith('.h') or key.endswith('.c') else '*', len(fs), lines, NAMES[who], what))
a('')
a('## Keeping it so')
a('')
a('- New Emu68 and AGA-PiStorm releases come in on the `upstream` branch (Emu68 plus AGA-PiStorm\'s patch, '
  'nothing of Alice68\'s) and are merged into `next`; CLAUDE.md\'s "Bring everything up to date" says how.')
a('- New work goes in files of its own (`src/alice/`, `src/frf_usb/xhci/`, `src/raspi/alice_splash.c`), '
  'so Emu68\'s own files change as little as possible and merges stay small.')
a('- After a merge, run `python3 tools/emu68-changes.py` and commit this page: it stops if a changed file has no note.')
open(os.path.join(ROOT, 'docs/EMU68-CHANGES.md'), 'w').write('\n'.join(o) + '\n')
print('docs/EMU68-CHANGES.md: %d of %d Emu68 files untouched, %d changed since, %d added' % (len(emu_files) - len(touched), len(emu_files), len(changed), len(added)))

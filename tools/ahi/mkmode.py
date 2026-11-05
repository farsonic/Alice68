"""The AHI audio mode file for aga-hdmi.audio: DEVS:AudioModes/AGA-HDMI.

    python tools/ahi/mkmode.py [OUT]       (default tools/ahi/AGA-HDMI)

An IFF FORM AHIM: AUDN with the driver's name (AHI loads DEVS:AHI/<it>.audio)
and one AUDM per mode - a TagItem list ending in TAG_DONE, then the mode's
name, AHIDB_Name holding its offset from the chunk's start - the layout of
AHI's own Void driver's VOID.s. Tag values from devices/ahi.h (AHI_TagBase =
TAG_USER, AHIDB_Name on AHI_TagBaseR). The ID's upper word, $0E68, is ours
by choice, not one AHI's author handed out.

SPDX-License-Identifier: GPL-2.0-only
"""
import struct
import sys

TAG_USER = 0x80000000
TAGBASE_R = TAG_USER | 0x8000
AHIDB_AudioID = TAG_USER + 100
AHIDB_Volume = TAG_USER + 103
AHIDB_Panning = TAG_USER + 104
AHIDB_Stereo = TAG_USER + 105
AHIDB_HiFi = TAG_USER + 106
AHIDB_MultTable = TAG_USER + 108
AHIDB_Name = TAGBASE_R + 109

DRIVER = b"aga-hdmi"
MODES = [
    (0x0E680001, False, b"AGA-PISTORM HDMI:16 bit stereo++"),
    (0x0E680002, True, b"AGA-PISTORM HDMI:HiFi 24 bit stereo++"),
    # 1.116 (driver 6.2): the same with low latency, for games - the driver
    # keeps less in its ring and the kernel starts it sooner (RGB2RTG 0.75-dev59)
    (0x0E680003, False, b"AGA-PISTORM HDMI:16 bit stereo++ low latency"),
    (0x0E680004, True, b"AGA-PISTORM HDMI:HiFi 24 bit stereo++ low latency"),
]


def chunk(cid, data):
    pad = b"\0" if len(data) & 1 else b""
    return cid + struct.pack(">I", len(data)) + data + pad


def audm(audio_id, hifi, name):
    tags = [(AHIDB_AudioID, audio_id), (AHIDB_Volume, 1), (AHIDB_Panning, 1), (AHIDB_Stereo, 1),
            (AHIDB_HiFi, 1 if hifi else 0), (AHIDB_MultTable, 0)]
    # the name after the tags, AHIDB_Name and the TAG_DONE longword
    off = 8 * (len(tags) + 1) + 4
    body = b"".join(struct.pack(">II", t, v) for t, v in tags)
    body += struct.pack(">II", AHIDB_Name, off) + struct.pack(">I", 0)
    assert len(body) == off
    return chunk(b"AUDM", body + name + b"\0")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "tools/ahi/AGA-HDMI"
    body = b"AHIM" + chunk(b"AUDN", DRIVER + b"\0") + b"".join(audm(*m) for m in MODES)
    data = b"FORM" + struct.pack(">I", len(body)) + body
    open(out, "wb").write(data)
    print(out, len(data), "bytes")


if __name__ == "__main__":
    main()

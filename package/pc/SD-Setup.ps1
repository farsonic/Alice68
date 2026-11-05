# SD-Setup.ps1 - the one-time PC step for AGA-PISTORM (run it via SD-Setup.cmd).
# SPDX-License-Identifier: GPL-2.0-only
#
# With the Amiga's SD card in this PC, it:
#   1. finds the Emu68 boot partition (the FAT drive with config.txt and the
#      Pi firmware; CaffeineOS's, with its KERNEL folder, wins)
#   2. reads which Emu68 the classic PiStorm boots ([gpio17=0] kernel=, else
#      every kernel= line outside a [gpio..] section - a card whose lines
#      start both a 1.0.x and a 1.1 Emu68 is left alone)
#   3. lets the Amiga write to that partition, the way that Emu68 wants it:
#        1.0.x (stock CaffeineOS 9317 ships 1.0.7): sd.unit0=rw emmc.unit0=rw
#              on boot/cmdline.txt - config.txt is NOT touched, because 1.0.x
#              hangs after its splash with the 1.1 overlay line in it
#        1.1:  dtoverlay=sdhc,unit0=rw ([pi3]) and dtoverlay=emmc,unit0=rw
#              ([pi4]) in the same marked block the Amiga installer uses, so
#              Uninstall removes them (config.txt.pre-aga keeps the original)
#   4. copies any Emu68 overlay the card is missing into overlays\
#   5. copies the AGA-Pistorm drawer onto the card, where the Amiga sees it as
#      EMU68:AGA-Pistorm
# The Amiga installer then switches the kernel to my bloody one (Emu68 1.1)
# and adds everything 1.1 needs in the same step - so a 1.0.x card changes
# over in one go, and never boots half-converted.
# Nothing else on the card is touched. The kernel and config.txt's kernel line
# are left to the Amiga installer, which changes them together with the RTG
# driver.
# -Drive G -Yes: no questions asked. -Drive also takes a folder that stands
# for the card (package\pc\test-sdsetup.ps1 runs it on such folders).
param([string]$Drive = "", [switch]$Yes)
$ErrorActionPreference = "Stop"

$Root   = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Amiga  = Join-Path $Root "Amiga"
$Latin1 = [Text.Encoding]::GetEncoding(28591)       # byte-for-byte text I/O
$BlkBegin = "# AGA-PISTORM { added by the AGA-PISTORM installer - its uninstaller removes this block"
$BlkEnd   = "# AGA-PISTORM }"

function Say($t, $c = "Gray") { Write-Host $t -ForegroundColor $c }

Say ""
Say "AGA-PISTORM - SD card setup (one time, on the PC)" "Cyan"
Say "=================================================" "Cyan"

# The package's Amiga folder, which steps 4 and 5 copy from, before anything
# on the card changes: run from inside an archive window, or with the .cmd
# copied out on its own, it is not there - and the script stopped at step 4
# with config.txt (or cmdline.txt) already changed.
if (-not (Test-Path (Join-Path $Amiga "AGA-Pistorm\Files\Overlays")) -or
    -not (Test-Path (Join-Path $Amiga "AGA-Pistorm.info"))) {
    Say "The package's Amiga folder is not next to this script:" "Red"
    Say "   $Amiga"
    Say "Unpack the whole AGA-PISTORM archive into a folder first, then run"
    Say "SD-Setup.cmd from there. Nothing was changed."
    exit 1
}

# ---- 1. the Emu68 boot partition -------------------------------------------------
# config.txt and the Pi firmware make it the boot partition. CaffeineOS also
# keeps its kernels in a KERNEL folder, and a drive with one wins; a card set
# up by hand has its kernel in the root and no such folder.
$cands = @(); $caff = @()
foreach ($d in Get-PSDrive -PSProvider FileSystem) {
    $r = $d.Root
    if ($r -like "C:*") { continue }
    try {
        if ((Test-Path (Join-Path $r "config.txt")) -and
            ((Test-Path (Join-Path $r "start4.elf")) -or (Test-Path (Join-Path $r "start.elf")))) {
            $cands += $r
            if (Test-Path (Join-Path $r "kernel")) { $caff += $r }
        }
    } catch { }
}
if ($caff.Count -gt 0) { $cands = $caff }
if ($Drive) {
    if ($Drive.TrimEnd(':', '\').Length -eq 1) { $cands = @("$($Drive.TrimEnd(':','\')):\") }
    else { $cands = @((Resolve-Path $Drive).Path) }                 # a folder
}
if ($cands.Count -eq 0) {
    Say "No Emu68 boot partition found." "Red"
    Say "Put the Amiga's SD card in this PC (the partition shows up as a drive"
    Say "with config.txt and start4.elf or start.elf) and run SD-Setup.cmd again."
    exit 1
}
if ($cands.Count -gt 1) {
    Say "More than one card looks like an Emu68 boot partition:" "Yellow"
    $cands | ForEach-Object { Say "   $_" }
    $pick = Read-Host "Type the drive letter to use"
    $cands = @("$($pick.TrimEnd(':','\')):\")
}
$Card = $cands[0]
$label = if ($Card.Length -le 3) { (Get-Volume -DriveLetter $Card[0] -ErrorAction SilentlyContinue).FileSystemLabel } else { "" }
Say ""
Say "Emu68 boot partition: $Card  ($label)" "Green"
if (-not $Yes) {
    $ok = Read-Host "Set it up for AGA-PISTORM? (Y/N)"
    if ($ok -notmatch '^[Yy]') { Say "Nothing was changed."; exit 1 }
}

# ---- 2. which Emu68 the classic PiStorm boots ---------------------------------
$cfg = Join-Path $Card "config.txt"
$bytes = [IO.File]::ReadAllBytes($cfg)
$text = $Latin1.GetString($bytes)
$eol = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
$lines = $text -split "`r?`n"
if ($text.EndsWith("`n")) { $lines = $lines[0..($lines.Count - 2)] }

function Get-Emu68Version($path) {
    if (-not (Test-Path $path)) { return $null }
    $b = [IO.File]::ReadAllBytes($path)
    if ($b.Length -gt 2 -and $b[0] -eq 0x1f -and $b[1] -eq 0x8b) {       # my kernels are gzipped
        $in = New-Object IO.MemoryStream(, $b)
        $gz = New-Object IO.Compression.GZipStream($in, [IO.Compression.CompressionMode]::Decompress)
        $out = New-Object IO.MemoryStream
        $gz.CopyTo($out); $gz.Close()
        $b = $out.ToArray()
    }
    $s = $Latin1.GetString($b)
    # the board it was built for: Emu68 prints it on its splash, so the name is
    # in the kernel - "PiStorm Classic" or "PiStorm16"/"PiStorm32lite". The file
    # name cannot tell: 1.0.x called the classic kernel Emu68-pistorm, 1.1 the
    # PiStorm16 one
    $script:kernelBoard = if ($s.Contains("PiStorm Classic, ")) { "classic" } elseif ($s.Contains("PiStorm16, ")) { "ps16" } else { $null }
    $m = [regex]::Match($s, '\$VER: Emu68 ([0-9][0-9A-Za-z.\-]*)')
    if ($m.Success) { return $m.Groups[1].Value }
    return $null
}
$kernelBoard = $null

$kernel = $null; $sect = ""; $cmdline = "cmdline.txt"; $pi4Kernel = $null; $plain = @()
foreach ($l in $lines) {
    $t = ($l -replace '#.*$', '').Trim()
    if ($t.StartsWith("[")) { $sect = $t.ToLower(); continue }
    if ($sect -eq "" -and $t -match '^cmdline=(\S+)') { $cmdline = $Matches[1] }
    if ($sect -eq "[gpio17=0]" -and -not $kernel -and $t -match '^kernel=(\S+)') { $kernel = $Matches[1] }
    # a card set up by hand has no board sections: its kernel= line is the one
    # every board reads, before any [filter] or under [all] - or one per Pi
    # model, under [pi4], [pi3] and the like. Any section but a [gpio..] one
    # (another board's), as the Amiga installer reads it (tools/agaboot/setup.c)
    elseif (-not $sect.StartsWith("[gpio") -and $t -match '^kernel=(\S+)') {
        $plain += $Matches[1]
        # the firmware boots the last kernel= line that applies
        if ($sect -eq "" -or $sect -eq "[all]" -or $sect -eq "[pi4]" -or $sect -eq "[cm4]") { $pi4Kernel = $Matches[1] }
    }
}
# Every kernel= line of a plain card, not the first alone: a global
# kernel=Emu68-pistorm.gz (1.0.7) with a later [pi4] kernel=Emu68-pi4.gz
# (1.1) read 1.0.7, left config.txt alone, and the Pi 4, which boots 1.1,
# never got its unit0=rw - the Amiga installer then could not write
# config.txt. Kernels of both kinds want opposite things (1.0.x hangs with
# the 1.1 lines), so such a card is left alone. A line whose file is not on
# the card, or that names no version, says nothing either way.
$mixed = @()
if (-not $kernel -and $plain.Count) {
    $vers = [ordered]@{}
    foreach ($k in $plain) {
        if (-not $vers.Contains($k)) { $vers[$k] = Get-Emu68Version (Join-Path $Card ($k -replace '/', '\')) }
    }
    $known = @($vers.Keys | Where-Object { $vers[$_] })
    $old = @($known | Where-Object { $vers[$_] -match '^1\.0\.' })
    if ($old.Count -gt 0 -and $old.Count -lt $known.Count) { $mixed = $known }
    # the one this card's Pi 4 boots, when it is on the card; else the first that is
    $kernel = if ($pi4Kernel -and $vers[$pi4Kernel]) { $pi4Kernel } elseif ($known.Count) { $known[0] } elseif ($pi4Kernel) { $pi4Kernel } else { $plain[0] }
}
if (-not $kernel) { $kernel = "kernel/Emu68-pistorm" }
$ver = Get-Emu68Version (Join-Path $Card ($kernel -replace '/', '\'))
$boardName = switch ($kernelBoard) { "classic" { ", classic PiStorm" } "ps16" { ", PiStorm16/32lite" } default { "" } }
if ($mixed.Count) {
    Say "Emu68 on this card: more than one version" "Yellow"
    foreach ($k in $mixed) { Say "   $($vers[$k]) ($k)" "Yellow" }
    Say "config.txt starts Emu68 1.0.x on one Pi and 1.1 on another, and the two" "Yellow"
    Say "want the Amiga's write access set up differently. Make every kernel=" "Yellow"
    Say "line start the same Emu68 (or delete the ones this card does not need)," "Yellow"
    Say "then run this again. Nothing was changed."
    exit 1
}
if ($ver) { Say "Emu68 on this card: $ver ($kernel$boardName)" } else { Say "Emu68 on this card: not found ($kernel)" "Yellow" }

# My block's start line without its end line after it - some bastard's hand
# edit gone to shit: step 1 below would drop every line from there to the end
# of the file, and the card would no longer boot. Then config.txt is left
# alone, as the Amiga installer leaves it (tools/agaboot/setup.c, block_open) -
# and the card altogether, whichever Emu68 it has: the Amiga installer stops
# on such a block, so it is fixed here on the PC first.
$blockOpen = $false
foreach ($l in $lines) {
    $t = $l.TrimStart()
    if ($t.StartsWith($BlkBegin)) { $blockOpen = $true }
    elseif ($t.StartsWith($BlkEnd)) { $blockOpen = $false }
}
if ($blockOpen) {
    Say "config.txt: left alone - it has the AGA-PISTORM block start without its" "Yellow"
    Say "            end line ($BlkEnd). Taking that block out would take" "Yellow"
    Say "            every line after it too. Put the end line back where the" "Yellow"
    Say "            block ends (or delete the block), then run this again." "Yellow"
    Say "Nothing was changed."
    exit 1
}

# ---- 3. let the Amiga write to this partition -----------------------------------
if ($ver -and $ver -match '^1\.0\.') {
    # Emu68 1.0.x: the command line switches, which it knows and 1.1 does not
    $cl = Join-Path $Card ($cmdline -replace '/', '\')
    if (-not (Test-Path $cl)) { Say "No $cmdline on the card - left alone." "Yellow" }
    else {
        $cb = [IO.File]::ReadAllBytes($cl)
        $ct = $Latin1.GetString($cb)
        $want = @("sd.unit0=rw", "emmc.unit0=rw") | Where-Object { $ct -notmatch ('(^|\s)' + [regex]::Escape($_) + '(\s|$)') }
        if (-not $want) { Say "cmdline: the Amiga can already write to this partition - left alone." }
        else {
            $bak = "$cl.pre-aga"
            if (-not (Test-Path $bak)) { [IO.File]::WriteAllBytes($bak, $cb) }
            $ceol = if ($ct.Contains("`r`n")) { "`r`n" } else { "`n" }
            $cls = $ct -split "`r?`n"
            $i = 0
            while ($i -lt $cls.Count -and ($cls[$i].Trim() -eq "" -or $cls[$i].TrimStart().StartsWith("#"))) { $i++ }
            if ($i -lt $cls.Count) { $cls[$i] = $cls[$i].TrimEnd() + " " + ($want -join " ") }
            else { $cls += ($want -join " ") }
            [IO.File]::WriteAllBytes($cl, $Latin1.GetBytes($cls -join $ceol))
            Say "cmdline: added $($want -join ' ') (the old file is $cmdline.pre-aga)" "Green"
        }
    }
    Say "config.txt: left alone - Emu68 1.0.x hangs with the 1.1 lines in it."
    Say "            The Amiga installer changes it, together with the kernel."
}
elseif (-not $ver) {
    Say "config.txt: left alone - without knowing the Emu68 version, changing it" "Yellow"
    Say "            could stop the card from booting."
}
else {

# Everything Emu68 1.1 needs, in the same marked block and with the same
# #AGA-PISTORM-WAS: convention for gpu_mem as the Amiga installer
# (tools/agaboot/setup.c), which takes the block out and builds it again:
#   gpu_mem=128                 a lower gpu_mem a Pi 4 reads is commented out
#   [pi3] / [pi02] sdhc,unit0=rw    the Amiga may write this partition
#   [pi4] emmc,unit0=rw, force_turbo=1, core_freq=500
# Not only the write access: a card that already runs 1.1 without the rest -
# one made on another Amiga - stops after its splash on a classic PiStorm,
# whose bus core_freq times, before the Amiga installer could ever run.
# The two clock lines only when the card's kernel is a classic PiStorm one: a
# PiStorm16 keeps the card's own clocks (core_freq=500 gave a tester's
# PiStorm16 trouble; it wants 400 there), and so does a kernel I cannot read
# for shit. A PiStorm16 card gets the emmc line alone, and only when it has
# none - its tester asked for nothing but the kernel line (2026-10-02), and a
# card that already runs 1.1 there has the emmc line. No gpu_mem (my kernel
# needs no more than a stock one), no sdhc lines (a PiStorm16 carries a Pi 4
# or CM4).
$Was = "#AGA-PISTORM-WAS: "
$ps16 = $kernelBoard -eq "ps16"

# 1. the card as its owner left it: an earlier block out, its gpu_mem back.
#    The lines in it that only the Amiga installer adds - dtoverlay=pal for an
#    NTSC Agnus, the Framethrower's unicam line, hdmi_drive=2 while Sound on
#    HDMI is on, HDMI rate's mode lines, the Pi 3's temp_soft_limit=70 and the
#    note that the cmdline's emmc.low_speed is mine - go back into the new
#    block where the Amiga puts them. Without that, running this again on an
#    installed card dropped them (a Framethrower's picture with them), and
#    the Amiga's next install had to put them back.
$SlowNote = "# the Pi 4's SD card at 25 MHz: AGA-PISTORM put emmc.low_speed in the cmdline"
$base = @(); $inBlk = $false; $amigaLines = @{}; $rateLines = @()
foreach ($l in $lines) {
    $t = $l.TrimStart()
    if ($t.StartsWith($BlkBegin)) { $inBlk = $true; continue }
    if ($inBlk) {
        $u = $t.TrimEnd()
        if ($t.StartsWith($BlkEnd)) { $inBlk = $false }
        elseif ($u -in "dtoverlay=pal", "dtoverlay=unicam,boot,smooth", "hdmi_drive=2", "temp_soft_limit=70", $SlowNote) { $amigaLines[$u] = $true }
        elseif ($u -match '^(hdmi_group=|hdmi_mode=|hdmi_force_mode=|hdmi_pixel_encoding=2$|disable_overscan=|hdmi_timings=)') { $rateLines += $u }
        continue
    }
    # its indentation before the mark, as setup.c keeps it
    if ($t.StartsWith($Was + "gpu_mem=")) { $base += $l.Substring(0, $l.Length - $t.Length) + $t.Substring($Was.Length); continue }
    $base += $l
}

# the lines a Pi 4 reads: before any filter, [all], [pi4], [cm4], a board's section
function Pi4-Reads($s) { return $s -eq "" -or $s -eq "[all]" -or $s -eq "[pi4]" -or $s -eq "[cm4]" -or $s -eq "[gpio17=0]" -or $s -eq "[gpio24=1]" }
function Has-Active($pattern, [switch]$Pi4, [switch]$Every) {
    $s = ""
    foreach ($l in $script:base) {
        $t = ($l -replace '#.*$', '').Trim()
        if ($t.StartsWith("[")) { $s = $t.ToLower(); continue }
        if ($Pi4 -and -not (Pi4-Reads $s)) { continue }
        if ($Every -and $s -ne "" -and $s -ne "[all]") { continue }   # where every Pi reads it
        if ($t -match $pattern) { return $true }
    }
    return $false
}
# an overlay loaded with $param: on its own line, or on a dtparam= line below
# it up to the next dtoverlay= line or [filter] - Emu68 Imager writes each
# setting on a line of its own ("dtoverlay=sdhc", a blank line,
# "dtparam=unit0=rw"), and setup.c's has_active counts such lines too
function Has-OverlayParam($name, $param) {
    $group = $false
    foreach ($l in $script:base) {
        $t = ($l -replace '#.*$', '').Trim()
        $head = $t -match ('^dtoverlay=' + $name + '\b')
        if ($t.StartsWith("[") -or $t.StartsWith("dtoverlay=")) { $group = $head }
        if (($head -or ($group -and $t.StartsWith("dtparam="))) -and $t.Contains($param)) { return $true }
    }
    return $false
}

# 2. gpu_mem: one of 128 or more that a Pi 4 reads stays; lower ones go.
#    A PiStorm16 card keeps its own.
$gpuKept = $ps16; $s = ""; $out = @()
foreach ($l in $base) {
    $t = ($l -replace '#.*$', '').Trim()
    if ($t.StartsWith("[")) { $s = $t.ToLower() }
    elseif (-not $ps16 -and (Pi4-Reads $s) -and $t -match '^gpu_mem=(\d+)') {
        # the mark after the line's indentation, which uninstall keeps
        # (1.112 dropped it, and config.txt came back without it)
        if ([int]$Matches[1] -lt 128) { $lt = $l.TrimStart(); $out += ($l.Substring(0, $l.Length - $lt.Length) + $Was + $lt); continue }
        $gpuKept = $true
    }
    $out += $l
}
$base = $out

$needSdhc  = -not $ps16 -and -not (Has-OverlayParam 'sdhc' 'unit0=rw')
$needEmmc  = -not (Has-OverlayParam 'emmc' 'unit0=rw')
$classic   = $kernelBoard -eq "classic"
$needTurbo = $classic -and -not (Has-Active '^force_turbo=1$' -Pi4)
$needCore  = $classic -and -not (Has-Active '^core_freq=500$' -Pi4)
$blk = @($BlkBegin)
# the Amiga installer's own lines, kept from the old block (1.), in its order
# and on its conditions (tools/agaboot/setup.c setup_config_block)
if (-not $ps16 -and $amigaLines["dtoverlay=pal"] -and -not (Has-Active '^dtoverlay=pal(,|\s|$)')) { $blk += "dtoverlay=pal" }
if (-not $ps16 -and $amigaLines["dtoverlay=unicam,boot,smooth"] -and -not (Has-Active '^dtoverlay=unicam')) { $blk += "dtoverlay=unicam,boot,smooth" }
if (-not $gpuKept) { $blk += "gpu_mem=128" }
if (-not $ps16 -and $amigaLines["hdmi_drive=2"] -and -not (Has-Active '^hdmi_drive=2' -Every)) { $blk += "hdmi_drive=2" }
# HDMI rate's lines as the Amiga worked them out (setup.c rate_lines)
if (-not $ps16) { $blk += $rateLines }
# the Pi 3's temp_soft_limit=70: classic only, a card's own line wins
$needTsl = $classic -and $amigaLines["temp_soft_limit=70"] -and -not (Has-Active '^temp_soft_limit=')
if ($needSdhc -or $needTsl) {
    $blk += "[pi3]"
    if ($needSdhc) { $blk += "dtoverlay=sdhc,unit0=rw" }
    if ($needTsl)  { $blk += "temp_soft_limit=70" }
    if ($needSdhc) { $blk += "[pi02]", "dtoverlay=sdhc,unit0=rw" }
}
if ($needEmmc -or $needTurbo -or $needCore) {
    $blk += "[pi4]"
    if ($needEmmc)  { $blk += "dtoverlay=emmc,unit0=rw" }
    if ($needTurbo) { $blk += "force_turbo=1" }
    if ($needCore)  { $blk += "core_freq=500" }
}
if ($needSdhc -or $needTsl -or $needEmmc -or $needTurbo -or $needCore) { $blk += "[all]" }
# the low speed option's note: the word stays in the cmdline, and so does mine
if ($amigaLines[$SlowNote]) { $blk += $SlowNote }
$blk += $BlkEnd

$new = $base
if ($blk.Count -gt 2) {
    # after the first cmdline= line of the global section, else before the
    # first [filter], else at the end - where the Amiga installer puts it too.
    # At the end, headed [all], when a display setting of the card's own comes
    # after that point (Emu68 Imager's screen mode, below its overlays): the
    # last one counts (setup.c's late_display)
    $at = -1
    for ($i = 0; $i -lt $base.Count; $i++) {
        $t = $base[$i].TrimStart()
        if ($t.StartsWith("[")) { $at = $i; break }
        if ($t.StartsWith("cmdline=")) { $at = $i + 1; break }
    }
    if ($at -lt 0) { $at = $base.Count }
    for ($i = $at; $i -lt $base.Count; $i++) {
        if ($base[$i].TrimStart() -cmatch '^(hdmi_group|hdmi_mode|hdmi_force_mode|hdmi_pixel_encoding|hdmi_drive)') {
            $at = $base.Count
            $blk = @($blk[0], "[all]") + $blk[1..($blk.Count - 1)]
            break
        }
    }
    $new = @()
    if ($at -gt 0) { $new += $base[0..($at - 1)] }
    $new += $blk
    if ($at -lt $base.Count) { $new += $base[$at..($base.Count - 1)] }
}
# A last line without a line ending keeps none, and a block that comes after
# it ends without one - as the Amiga installer writes it, so that its
# uninstall gives the file back byte for byte (it gave it back with a line
# ending added)
$newText = $new -join $eol
if ($text.Length -eq 0 -or $text.EndsWith("`n")) { $newText += $eol }
if ($newText -ceq $text -or $newText -ceq ($text + $eol)) {
    Say "config.txt: has everything Emu68 1.1 needs - left alone."
} else {
    $bak = Join-Path $Card "config.txt.pre-aga"
    if (-not (Test-Path $bak)) { [IO.File]::WriteAllBytes($bak, $bytes); Say "config.txt: backed up as config.txt.pre-aga" }
    [IO.File]::WriteAllBytes($cfg, $Latin1.GetBytes($newText))
    $what = @()
    if (-not $gpuKept) { $what += "gpu_mem=128" }
    if ($needTurbo -or $needCore) { $what += "the Pi 4 bus clock" }
    if ($needSdhc -or $needEmmc) { $what += "write access" }
    if ($what) { Say ("config.txt: Emu68 1.1 lines added - " + ($what -join ", ")) "Green" }
    else { Say "config.txt: took out the lines an earlier install added, which this card does not need." "Green" }
}
}   # Emu68 1.1

# ---- 4. Emu68 overlays -------------------------------------------------------------
$ovSrc = Join-Path $Amiga "AGA-Pistorm\Files\Overlays"
$ovDst = Join-Path $Card "overlays"
if (-not (Test-Path $ovDst)) { New-Item -ItemType Directory -Path $ovDst | Out-Null }
$added = 0
foreach ($f in Get-ChildItem $ovSrc -File) {
    $d = Join-Path $ovDst $f.Name
    if (-not (Test-Path $d)) { Copy-Item $f.FullName $d; $added++ }
}
Say "overlays: $added added (the ones already on the card were kept)"

# ---- 5. the Amiga package ------------------------------------------------------
Copy-Item (Join-Path $Amiga "AGA-Pistorm") $Card -Recurse -Force
Copy-Item (Join-Path $Amiga "AGA-Pistorm.info") $Card -Force
Say "AGA-Pistorm drawer copied to the card" "Green"

Say ""
Say "Done. Now:" "Cyan"
Say "  1. Safely eject the card, put it back in the Amiga and switch on."
$icon = if ($label) { $label } else { "boot partition" }
Say "  2. Open the $icon disk icon, then the AGA-Pistorm drawer."
Say "  3. Double-click Install and follow it."
Say ""
exit 0

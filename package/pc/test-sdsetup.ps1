# test-sdsetup.ps1 - SD-Setup.ps1 run on fake SD cards, no questions asked.
# SPDX-License-Identifier: GPL-2.0-only
#
# Each fake card is a folder with config.txt, start4.elf and a kernel file
# that names its Emu68 version and board the way a real one does; SD-Setup.ps1
# gets it as -Drive <folder> -Yes, from a fake package folder (PC\ and
# Amiga\, as tools/mkpackage.py lays the package out). It checks:
#   - my damn config.txt block without its end line: nothing on the card is
#     changed, whichever Emu68 it has
#   - kernel= lines that start a 1.0.x and a 1.1 Emu68: nothing is changed
#   - one kernel= line per Pi model ([pi4]/[pi3], either order): the Emu68
#     version is read from that line
#   - the package's Amiga folder missing: SD-Setup stops before it changes
#     anything on the card (every file's hash, before and after)
#   - CaffeineOS and plain AmigaOS 3.x cards still come out exactly as the
#     1.112 release's SD-Setup.ps1 made them, and a second run changes nothing
#   - a config.txt without a final line ending (LF and CRLF) gets none
#   - an indented gpu_mem keeps its indentation through the Amiga's uninstall
#   - a card made with Emu68 Imager (each overlay setting on a dtparam= line
#     of its own, the screen mode after the first [filter]): no second sdhc
#     or emmc line, the block at the end headed [all], a second run changes
#     nothing, the Amiga's install afterwards neither
#   - with a gcc for the host (the mingw one, or any on PATH): what SD-Setup
#     added, the Amiga's UNINSTALL CONFIG takes out again, byte for byte
#     (tools/agaboot/setup.c, built with -DHOST_TEST)
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File package\pc\test-sdsetup.ps1 [-Work <folder>]
# Not part of the package: tools/mkpackage.py ships SD-Setup.ps1 alone.
param([string]$Work = (Join-Path $env:TEMP "aga-sdsetup-test"))
$ErrorActionPreference = "Stop"

$repo   = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$tests  = Join-Path $repo "tools\agaboot\tests"
$Latin1 = [Text.Encoding]::GetEncoding(28591)
$Release = "351b176"                          # 1.112: what "as before" means
if (Test-Path $Work) { Remove-Item $Work -Recurse -Force }
New-Item -ItemType Directory -Force -Path $Work | Out-Null

$all = $true
function Check([string]$name, [bool]$ok) {
    Write-Host ("  {0,-66} {1}" -f $name, $(if ($ok) { "ok" } else { "FAILED" })) -ForegroundColor $(if ($ok) { "Gray" } else { "Red" })
    if (-not $ok) { $script:all = $false }
}

# a package folder holding the given SD-Setup.ps1 text; -NoAmiga leaves out
# the Amiga folder, as when the .cmd is run from inside an archive window
function New-Package([string]$name, [string]$script, [switch]$NoAmiga) {
    $p = Join-Path $Work $name
    New-Item -ItemType Directory -Force -Path (Join-Path $p "PC") | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $p "PC\SD-Setup.ps1"), $Latin1.GetBytes($script))
    if (-not $NoAmiga) {
        $ov = Join-Path $p "Amiga\AGA-Pistorm\Files\Overlays"
        New-Item -ItemType Directory -Force -Path $ov | Out-Null
        foreach ($o in "emmc.dtbo", "sdhc.dtbo", "unicam.dtbo") { [IO.File]::WriteAllBytes((Join-Path $ov $o), $Latin1.GetBytes("fake $o")) }
        [IO.File]::WriteAllBytes((Join-Path $p "Amiga\AGA-Pistorm\Install"), $Latin1.GetBytes("; fake`n"))
        [IO.File]::WriteAllBytes((Join-Path $p "Amiga\AGA-Pistorm.info"), $Latin1.GetBytes("fake icon"))
    }
    return (Join-Path $p "PC\SD-Setup.ps1")
}

# a card folder: config.txt (the bytes of a fixture), the Pi firmware, a
# kernel naming its version and board, and the 1.0.x command line file
function New-Card([string]$name, [string]$config, [string]$kernel, [string]$ver = "1.1.0", [string]$board = "PiStorm Classic") {
    $c = Join-Path $Work $name
    New-Item -ItemType Directory -Force -Path $c | Out-Null
    Copy-Item $config (Join-Path $c "config.txt")
    [IO.File]::WriteAllBytes((Join-Path $c "start4.elf"), $Latin1.GetBytes("fake firmware"))
    $k = Join-Path $c ($kernel -replace '/', '\')
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $k) | Out-Null
    [IO.File]::WriteAllBytes($k, $Latin1.GetBytes("Emu68`0`$VER: Emu68 $ver (01.10.2026)`0$board, Pi 4`0"))
    New-Item -ItemType Directory -Force -Path (Join-Path $c "boot") | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $c "boot\cmdline.txt"), $Latin1.GetBytes("console=serial`n"))
    return $c
}

function Run-Setup([string]$script, [string]$card) {
    $ErrorActionPreference = "Continue"
    $out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script -Drive $card -Yes 2>&1 | Out-String
    return @{ Out = $out; Rc = $LASTEXITCODE }
}

# every file on the card and its hash
function Snap([string]$dir) {
    return (Get-ChildItem $dir -Recurse -File | Sort-Object FullName |
            ForEach-Object { $_.FullName.Substring($dir.Length) + "=" + (Get-FileHash $_.FullName).Hash }) -join "`n"
}
function Hash([string]$p) { return (Get-FileHash $p).Hash }

$new = $Latin1.GetString([IO.File]::ReadAllBytes((Join-Path $PSScriptRoot "SD-Setup.ps1")))
# the 1.112 script, as released - but given a folder for -Drive, which it
# took only as a drive letter
$old = (& git -C $repo show "${Release}:package/pc/SD-Setup.ps1" | Out-String)
if ($LASTEXITCODE) { throw "cannot read SD-Setup.ps1 from $Release" }
$old = $old -replace "`r`n", "`n"
$oldDrive = 'if ($Drive) { $cands = @("$($Drive.TrimEnd('':'',''\'')):\") }'
if (-not $old.Contains($oldDrive)) { throw "the 1.112 script's -Drive line was not found" }
$old = $old.Replace($oldDrive, 'if ($Drive) { $cands = @($Drive) }')

$pkgNew  = New-Package "pkg-new" $new
$pkgOld  = New-Package "pkg-old" $old
$pkgNone = New-Package "pkg-noamiga" $new -NoAmiga

# ---- the block start without its end line ----------------------------------
Write-Host "block without its end line" -ForegroundColor Cyan
# whichever Emu68 the card has: on 1.0.x too, where only cmdline.txt would
# change, the Amiga installer stops on such a block - nothing on the card
# may change, so the user fixes it on the PC first
foreach ($f in "config-noend-caffeineos.txt", "config-noend-plain.txt") {
  foreach ($v in "1.1.0", "1.0.7") {
    $card = New-Card "noend-$v-$f" (Join-Path $tests $f) "kernel/Emu68-classic-aga.gz" $v
    $before = Hash (Join-Path $card "config.txt")
    $snap = Snap $card
    $r = Run-Setup $pkgNew $card
    Check "${f} (${v}): config.txt unchanged" ((Hash (Join-Path $card "config.txt")) -eq $before)
    Check "${f} (${v}): says why" ($r.Out -match "block start without its")
    Check "${f} (${v}): stops, no file on the card changed or added" ($r.Rc -ne 0 -and (Snap $card) -eq $snap)
  }
}

# ---- kernel= lines that start different Emu68 versions ---------------------------
# the firmware boots the last line that applies: a Pi 4 here boots the
# [pi4] one, 1.1, while the global one is 1.0.7. Reading the first line
# alone took the 1.0.x path and the Pi 4 never got its unit0=rw.
Write-Host "kernel lines of two Emu68 versions" -ForegroundColor Cyan
$mixCfg = Join-Path $Work "config-mixed-versions.txt"
[IO.File]::WriteAllBytes($mixCfg, $Latin1.GetBytes("kernel=Emu68-pistorm.gz`n[pi4]`nkernel=Emu68-pi4.gz`n[all]`ninitramfs kick.rom`n"))
$card = New-Card "mixed" $mixCfg "Emu68-pistorm.gz" "1.0.7"
[IO.File]::WriteAllBytes((Join-Path $card "Emu68-pi4.gz"), $Latin1.GetBytes("Emu68`0`$VER: Emu68 1.1.0 (01.10.2026)`0PiStorm Classic, Pi 4`0"))
$snap = Snap $card
$r = Run-Setup $pkgNew $card
Check "1.0.7 global, 1.1 under [pi4]: says so" ($r.Out -match "more than one version")
Check "1.0.7 global, 1.1 under [pi4]: stops, no file on the card changed or added" ($r.Rc -ne 0 -and (Snap $card) -eq $snap)
# the same kernels, both 1.1: the version the Pi 4 boots, and write access
[IO.File]::WriteAllBytes((Join-Path $card "Emu68-pistorm.gz"), $Latin1.GetBytes("Emu68`0`$VER: Emu68 1.1.0 (01.10.2026)`0PiStorm Classic, Pi 4`0"))
$r = Run-Setup $pkgNew $card
Check "both 1.1: version read from the [pi4] line, write access added" ($r.Rc -eq 0 -and $r.Out -match "Emu68 on this card: 1\.1\.0 \(Emu68-pi4\.gz" -and $r.Out -match "write access")

# ---- one kernel= line per Pi model -----------------------------------------------
Write-Host "kernel lines per Pi model" -ForegroundColor Cyan
foreach ($o in @(@("pi4", "pi3"), @("pi3", "pi4"))) {
    $f = "config-permodel-$($o[0])-$($o[1]).txt"
    $card = New-Card "permodel-$($o[0])" (Join-Path $tests $f) "Emu68-$($o[0]).gz"
    $before = Hash (Join-Path $card "config.txt")
    $r = Run-Setup $pkgNew $card
    Check "${f}: version read from the [$($o[0])] kernel line" ($r.Out -match "Emu68 on this card: 1\.1\.0 \(Emu68-$($o[0])\.gz")
    Check "${f}: write access added" ((Hash (Join-Path $card "config.txt")) -ne $before -and $r.Out -match "write access")
}

# ---- the package's Amiga folder missing ------------------------------------------
Write-Host "Amiga folder missing" -ForegroundColor Cyan
foreach ($v in "1.1.0", "1.0.7") {
    $card = New-Card "noamiga-$v" (Join-Path $tests "config-caffeineos-9317.txt") "kernel/Emu68-pistorm" $v
    $before = Snap $card
    $r = Run-Setup $pkgNone $card
    Check "Emu68 ${v}: stops (rc $($r.Rc))" ($r.Rc -ne 0 -and $r.Out -match "Nothing was changed")
    Check "Emu68 ${v}: no file on the card changed, none added" ((Snap $card) -eq $before)
}

# ---- normal cards: as the 1.112 release made them ------------------------------
Write-Host "normal cards, against the 1.112 release" -ForegroundColor Cyan
$gccDir = "C:\ProgramData\mingw64\mingw64\bin"
$gcc = if (Test-Path (Join-Path $gccDir "gcc.exe")) { Join-Path $gccDir "gcc.exe" } else { (Get-Command gcc -ErrorAction SilentlyContinue).Source }
$setuptest = $null
if ($gcc) {
    $setuptest = Join-Path $Work "setuptest.exe"
    $env:PATH = (Split-Path -Parent $gcc) + ";" + $env:PATH
    & $gcc -DHOST_TEST -o $setuptest (Join-Path $repo "tools\agaboot\setup.c")
    if ($LASTEXITCODE) { throw "cannot build tools/agaboot/setup.c" }
} else { Write-Host "  (no gcc: the Amiga uninstall is not checked)" -ForegroundColor Yellow }
$cases = @(
    @("config-caffeineos-9317.txt", "kernel/Emu68-pistorm", "1.1.0", "PiStorm Classic"),
    @("config-caffeineos-9317.txt", "kernel/Emu68-pistorm", "1.0.7", "PiStorm Classic"),
    @("config-caffeineos-9317.txt", "kernel/Emu68-pistorm", "1.1.0", "PiStorm16"),
    @("config-caffeineos-oc.txt",   "kernel/Emu68-pistorm", "1.1.0", "PiStorm Classic"),
    @("config-amigaos32-tester.txt", "Emu68-pistorm.gz", "1.1.0", "PiStorm Classic"),
    @("config-emu68-stock.txt",     "Emu68-pistorm32lite.gz", "1.1.0", "PiStorm Classic"),
    @("config-plain-nokernel.txt",  "kernel/Emu68-pistorm", "1.1.0", "PiStorm Classic"))
$n = 0
foreach ($c in $cases) {
    $n++
    $tag = "$($c[0]) ($($c[2]), $($c[3]))"
    $a = New-Card "new-$n" (Join-Path $tests $c[0]) $c[1] $c[2] $c[3]
    $b = New-Card "old-$n" (Join-Path $tests $c[0]) $c[1] $c[2] $c[3]
    $ra = Run-Setup $pkgNew $a
    $rb = Run-Setup $pkgOld $b
    Check "${tag}: rc 0" ($ra.Rc -eq 0)
    Check "${tag}: the same card as 1.112 made" ((Snap $a) -eq (Snap $b))
    Check "${tag}: the drawer is on the card" (Test-Path (Join-Path $a "AGA-Pistorm\Files\Overlays"))
    $once = Hash (Join-Path $a "config.txt")
    Run-Setup $pkgNew $a | Out-Null
    Check "${tag}: a second run changes nothing" ((Hash (Join-Path $a "config.txt")) -eq $once)
    if ($setuptest -and (Test-Path (Join-Path $a "config.txt.pre-aga"))) {
        $back = Join-Path $Work "back-$n.txt"
        & $setuptest --uninstall (Join-Path $a "config.txt") $back
        Check "${tag}: the Amiga's uninstall gives the original back" ($LASTEXITCODE -eq 0 -and (Hash $back) -eq (Hash (Join-Path $tests $c[0])))
    }
}

# ---- config.txt without a final line ending ---------------------------------------
# 1.112 added one; the Amiga's uninstall then gave the file back one line
# ending longer than it was
Write-Host "config.txt without a final line ending" -ForegroundColor Cyan
foreach ($f in "config-plain-noeol.txt", "config-plain-noeol-crlf.txt") {
    $card = New-Card "noeol-$f" (Join-Path $tests $f) "Emu68-pistorm.gz"
    $r = Run-Setup $pkgNew $card
    $cfgPath = Join-Path $card "config.txt"
    $t = $Latin1.GetString([IO.File]::ReadAllBytes($cfgPath))
    Check "${f}: rc 0, write access added" ($r.Rc -eq 0 -and $r.Out -match "write access")
    Check "${f}: still no final line ending" (-not $t.EndsWith("`n"))
    Check "${f}: line endings kept" ($t.Contains("`r`n") -eq $f.Contains("crlf") -and ($f.Contains("crlf") -or -not $t.Contains("`r")))
    $once = Hash $cfgPath
    Run-Setup $pkgNew $card | Out-Null
    Check "${f}: a second run changes nothing" ((Hash $cfgPath) -eq $once)
    if ($setuptest) {
        $back = Join-Path $Work "back-noeol-$f"
        & $setuptest --uninstall $cfgPath $back
        Check "${f}: the Amiga's uninstall gives the original back" ($LASTEXITCODE -eq 0 -and (Hash $back) -eq (Hash (Join-Path $tests $f)))
    }
}

# ---- a card made with Emu68 Imager --------------------------------------------------
# Its write access is "dtoverlay=sdhc", a blank line, "dtparam=unit0=rw", and
# its screen mode comes below the overlays, after the first [filter]
# (RGB2RTG 0c5af86): SD-Setup added a second sdhc and emmc line and put the
# block above that screen mode, where the Amiga's later HDMI lines lost to it.
Write-Host "a card made with Emu68 Imager" -ForegroundColor Cyan
foreach ($f in "config-emu68-imager.txt", "config-emu68-imager-ft.txt") {
    $card = New-Card "imager-$f" (Join-Path $tests $f) "Emu68-pistorm-classic.gz"
    $r = Run-Setup $pkgNew $card
    $cfgPath = Join-Path $card "config.txt"
    $t = $Latin1.GetString([IO.File]::ReadAllBytes($cfgPath))
    Check "${f}: rc 0" ($r.Rc -eq 0)
    Check "${f}: no write access line of ours - the card's dtparam= lines count" (-not $t.Contains("dtoverlay=sdhc,unit0=rw") -and -not $t.Contains("dtoverlay=emmc,unit0=rw"))
    $b = $t.IndexOf("# AGA-PISTORM {")
    Check "${f}: the block at the end, headed [all]" ($b -gt $t.IndexOf("[gpio17=0]") -and $t.Substring($b) -match "^# AGA-PISTORM \{[^\n]*\n\[all\]\r?\n")
    $once = Hash $cfgPath
    Run-Setup $pkgNew $card | Out-Null
    Check "${f}: a second run changes nothing" ((Hash $cfgPath) -eq $once)
    if ($setuptest) {
        $back = Join-Path $Work "back-imager-$f"
        & $setuptest --uninstall $cfgPath $back
        Check "${f}: the Amiga's uninstall gives the original back" ($LASTEXITCODE -eq 0 -and (Hash $back) -eq (Hash (Join-Path $tests $f)))
    }
}

# ---- an indented gpu_mem -----------------------------------------------------------
# commented out with its indentation dropped (1.112), it came back from the
# Amiga's uninstall without it
Write-Host "an indented gpu_mem" -ForegroundColor Cyan
foreach ($e in "`n", "`r`n") {
    $tag = if ($e -eq "`n") { "LF" } else { "CRLF" }
    $gpuCfg = Join-Path $Work "config-gpu-indented-$tag.txt"
    [IO.File]::WriteAllBytes($gpuCfg, $Latin1.GetBytes("  gpu_mem=32${e}kernel=Emu68-pistorm.gz$e"))
    $card = New-Card "gpu-indented-$tag" $gpuCfg "Emu68-pistorm.gz"
    $r = Run-Setup $pkgNew $card
    $cfgPath = Join-Path $card "config.txt"
    Check "${tag}: rc 0, gpu_mem=128 added" ($r.Rc -eq 0 -and $r.Out -match "gpu_mem=128")
    Check "${tag}: the old line keeps its indentation" ($Latin1.GetString([IO.File]::ReadAllBytes($cfgPath)).Contains("  #AGA-PISTORM-WAS: gpu_mem=32$e"))
    $once = Hash $cfgPath
    Run-Setup $pkgNew $card | Out-Null
    Check "${tag}: a second run changes nothing" ((Hash $cfgPath) -eq $once)
    if ($setuptest) {
        $back = Join-Path $Work "back-gpu-$tag.txt"
        & $setuptest --uninstall $cfgPath $back
        Check "${tag}: the Amiga's uninstall gives the original back" ($LASTEXITCODE -eq 0 -and (Hash $back) -eq (Hash $gpuCfg))
    }
}

# ---- a card the Amiga installer has set up, SD-Setup run on it again ---------
# Its block holds lines SD-Setup does not add itself: dtoverlay=pal (an NTSC
# Agnus), the Framethrower's unicam line and hdmi_drive=2 (Sound on HDMI).
# Rebuilding the block dropped them; now they stay, where the Amiga puts
# them, and the Amiga's own install finds nothing to change afterwards.
if ($setuptest) {
    Write-Host "the Amiga's own block lines kept" -ForegroundColor Cyan
    foreach ($f in "config-caffeineos-9317.txt", "config-amigaos32-tester.txt", "config-emu68-imager.txt") {
        $inst = Join-Path $Work "amiga-install-of-$f"
        & $setuptest --install --opts=7 (Join-Path $tests $f) $inst
        if ($LASTEXITCODE) { Check "${f}: the Amiga install for the test" $false; continue }
        # the kernel= line the install left is mine: SD-Setup reads the
        # version from that file, so it is the one the card must have
        $card = New-Card "amiga-installed-$f" $inst "kernel/Emu68-classic-aga.gz"
        $r = Run-Setup $pkgNew $card
        Check "${f}: SD-Setup read the Emu68 1.1 kernel (the block was rebuilt)" ($r.Out -match "Emu68 on this card: 1\.1\.0")
        $cfgPath = Join-Path $card "config.txt"
        # the Amiga's own lines in a block, in order, up to its first filter
        # (a card with its own unicam line, say, has none of mine for it)
        # (a block at the end of the file starts with [all], which is no filter here)
        function Amiga-Lines([string]$path) {
            $got = @(); $in = $false; $lead = $false
            foreach ($l in ($Latin1.GetString([IO.File]::ReadAllBytes($path)) -split "`r?`n")) {
                $t = $l.Trim()
                if ($t.StartsWith("# AGA-PISTORM {")) { $in = $true; $lead = $true; continue }
                if (-not $in) { continue }
                $la = $lead -and $t -eq "[all]"; $lead = $false
                if ($la) { continue }
                if ($t.StartsWith("# AGA-PISTORM }") -or $t.StartsWith("[")) { break }
                if ($t -in "dtoverlay=pal", "dtoverlay=unicam,boot,smooth", "hdmi_drive=2") { $got += $t }
            }
            return ($got -join ",")
        }
        $want = Amiga-Lines $inst
        Check "${f}: rc 0" ($r.Rc -eq 0)
        Check "${f}: the Amiga's block lines kept, in order, before the filters ($want)" ($want -match "hdmi_drive=2" -and (Amiga-Lines $cfgPath) -eq $want)
        $again = Join-Path $Work "amiga-again-$f"
        & $setuptest --install --opts=7 $cfgPath $again
        Check "${f}: the Amiga's install again changes nothing" ($LASTEXITCODE -eq 0 -and (Hash $again) -eq (Hash $cfgPath))
        $back = Join-Path $Work "amiga-back-$f"
        & $setuptest --uninstall $cfgPath $back
        Check "${f}: the Amiga's uninstall gives the original back" ($LASTEXITCODE -eq 0 -and (Hash $back) -eq (Hash (Join-Path $tests $f)))
    }
}

# ---- HDMI rate 50.2 Hz (1.118): its custom mode lines kept ------------------------
# hdmi_timings= is one of HDMI rate's lines now: SD-Setup run on a card the
# Amiga set to 1080p at 50.2 Hz keeps all four, and the Amiga's install
# afterwards changes nothing
if ($setuptest) {
    Write-Host "HDMI rate 50.2 Hz kept" -ForegroundColor Cyan
    $f = "config-caffeineos-9317.txt"
    $inst = Join-Path $Work "amiga-install-room-$f"
    & $setuptest --install --opts=28 (Join-Path $tests $f) $inst
    if ($LASTEXITCODE) { Check "${f}: the Amiga install for the test" $false }
    else {
        $card = New-Card "amiga-room-$f" $inst "kernel/Emu68-classic-aga.gz"
        $r = Run-Setup $pkgNew $card
        $cfgPath = Join-Path $card "config.txt"
        $t = $Latin1.GetString([IO.File]::ReadAllBytes($cfgPath))
        Check "${f}: rc 0" ($r.Rc -eq 0)
        Check "${f}: hdmi_mode=87 and its hdmi_timings line kept" ($t.Contains("`nhdmi_mode=87") -and $t.Contains("`nhdmi_timings=1920 1 528 44 148 1080 1 4 5 36 0 0 0 50 0 149094000 3"))
        $again = Join-Path $Work "amiga-room-again-$f"
        & $setuptest --install --opts=28 $cfgPath $again
        Check "${f}: the Amiga's install again changes nothing" ($LASTEXITCODE -eq 0 -and (Hash $again) -eq (Hash $cfgPath))
    }
}

Write-Host ""
if ($all) { Write-Host "ALL OK" -ForegroundColor Green; exit 0 }
Write-Host "FAILURES" -ForegroundColor Red; exit 1

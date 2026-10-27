# Download reference docs and test suites into refs/. Existing entries are skipped;
# to update one, delete its directory or file and rerun.
# Windows counterpart of fetch.sh: keep the two lists in sync. Needs tar.exe,
# which ships with Windows 10 1803 and later.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
# Windows PowerShell 5.1 may not enable TLS 1.2 by default
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

function download($url, $path) {
  Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $path
}

function snapshot($dir, $name, $url) {
  if (Test-Path $dir) { Write-Host "skip $dir"; return }
  Write-Host "fetch $name -> $dir"
  $tmp = "$dir.tmp"
  if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
  New-Item -ItemType Directory -Force $tmp | Out-Null
  download $url "$dir.tar.gz"
  tar -xzf "$dir.tar.gz" --strip-components=1 -C $tmp
  if ($LASTEXITCODE -ne 0) { throw "tar failed for $url" }
  Remove-Item "$dir.tar.gz"
  Move-Item $tmp $dir
}

# GitHub repo snapshot pinned to a commit
function tarball($dir, $repo, $sha) {
  snapshot $dir "$repo@$($sha.Substring(0, 7))" "https://codeload.github.com/$repo/tar.gz/$sha"
}

# Codeberg repo snapshot pinned to a commit
function codeberg($dir, $repo, $sha) {
  snapshot $dir "codeberg:$repo@$($sha.Substring(0, 7))" "https://codeberg.org/$repo/archive/$sha.tar.gz"
}

function file($path, $url) {
  if (Test-Path $path) { Write-Host "skip $path"; return }
  Write-Host "fetch $url -> $path"
  New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
  download $url "$path.tmp"
  Move-Item "$path.tmp" $path
}

# A page from the Wayback Machine, which serves it gzip-encoded
function page($path, $url) {
  if (Test-Path $path) { Write-Host "skip $path"; return }
  Write-Host "fetch $url -> $path"
  New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
  download $url "$path.tmp"
  $bytes = [IO.File]::ReadAllBytes((Resolve-Path "$path.tmp"))
  if ($bytes.Length -ge 2 -and $bytes[0] -eq 0x1f -and $bytes[1] -eq 0x8b) {
    $in = New-Object IO.MemoryStream(,$bytes)
    $gzip = New-Object IO.Compression.GZipStream($in, [IO.Compression.CompressionMode]::Decompress)
    $out = New-Object IO.MemoryStream
    $gzip.CopyTo($out)
    [IO.File]::WriteAllBytes((Join-Path (Resolve-Path (Split-Path $path)) (Split-Path $path -Leaf)), $out.ToArray())
    Remove-Item "$path.tmp"
  } else {
    Move-Item "$path.tmp" $path
  }
}

function archive($dir, $url) {
  if (Test-Path $dir) { Write-Host "skip $dir"; return }
  Write-Host "fetch $url -> $dir"
  $tmp = "$dir.tmp"
  if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
  New-Item -ItemType Directory -Force $tmp | Out-Null
  download $url "$dir.zip"
  Expand-Archive "$dir.zip" -DestinationPath $tmp
  Remove-Item "$dir.zip"
  Move-Item $tmp $dir
}

Push-Location $PSScriptRoot
try {
  # Docs
  tarball docs/riscv-isa riscv/riscv-isa-manual 6ee57d8997021aaa8025e208164763fe12e03c19
  tarball docs/riscv-psabi riscv-non-isa/riscv-elf-psabi-doc 74936a9e25dec3c5b297b19e4c37e97d14868b22
  tarball docs/pandocs gbdev/pandocs 0191af06ac49661587dcde3d57a241a626b8df75
  tarball docs/gbedg   Ashiepaws/GBEDG 97f198d330a51be558aa8fc9f3f0760846d02d95
  file docs/gbctr.pdf     https://gekkio.fi/files/gb-docs/gbctr.pdf
  file docs/Opcodes.json  https://raw.githubusercontent.com/gbdev/gb-opcodes/376f61c86fdac2048f7ce5fe838ae756b306017e/Opcodes.json
  file docs/gbz80.7       https://raw.githubusercontent.com/gbdev/rgbds/6818759a8ca8cd11bf092aa052e2a992b52ac8e5/man/gbz80.7
  file docs/tcagbd.pdf    https://raw.githubusercontent.com/AntonioND/giibiiadvance/ccb40c3cf7d9efec538858a36d4ab69bb4d0ce5c/docs/TCAGBD.pdf
  file docs/gb-sound-hardware.html https://gbdev.gg8.se/wiki/articles/Gameboy_sound_hardware
  file docs/gbatek.htm    https://problemkaputt.de/gbatek.htm
  # TCRF revision 2183057, as archived on 2026-09-18
  page docs/tcrf-cgb-boot-rom.html https://web.archive.org/web/20260918011510id_/https://tcrf.net/Notes:Game_Boy_Color_Bootstrap_ROM
  # Measurements of one GBC screen by Pokefan531, Handheld Color Space Project at
  # 66b7f4c, for tools/lcd_colors.py
  $gbcLcd = "https://raw.githubusercontent.com/Brankale/Handheld-Color-Space-Project/66b7f4c01c8528b07bed8e2cec5a5023327cd315/handhelds/Nintendo%20GBC/%5B2026-05-13%5D%5BPokefan531%5D/measurements"
  file "docs/gbc-lcd/README.md" "$gbcLcd/README.md"
  file "docs/gbc-lcd/gbc-color1 2026-05-13.ods" "$gbcLcd/gbc-color1%202026-05-13.ods"
  file "docs/gbc-lcd/gbc-color2 2026-05-13.ods" "$gbcLcd/gbc-color2%202026-05-13.ods"
  file "docs/gbc-lcd/gbc-gamma1 2026-05-09.txt" "$gbcLcd/gbc-gamma1%202026-05-09.txt"
  file "docs/gbc-lcd/gbc-gamma2 2026-05-09.txt" "$gbcLcd/gbc-gamma2%202026-05-09.txt"

  # Test suites: GB / GBC
  archive tests/game-boy-test-roms https://github.com/c-sp/game-boy-test-roms/releases/download/v7.0/game-boy-test-roms-v7.0.zip
  tarball tests/mooneye-source Gekkio/mooneye-test-suite 8d742b9d55055f6878a2f3017e0ccf2234cd692c
  tarball tests/sm83           SingleStepTests/sm83 f9c30210245dd691661db39f5ace022c465ecc2f
  tarball tests/gameboy-doctor robert/gameboy-doctor cc742777af7d71de3061c65733ecfa250237012b
  tarball tests/same-suite-source LIJI32/SameSuite f71b4b3c3773b771f94955f5081dd2b36aabe54d
  tarball tests/age-source c-sp/age-test-roms cd3f654d13bf7137fa4f7fb7e5dd041d10f7098e
  # Gambatte's test runner, for the digit patterns its tests display
  file tests/gambatte-testrunner/testrunner.cpp https://raw.githubusercontent.com/pokemon-speedrunning/gambatte-core/d819bad196942db6bc31fd4196b911584c415594/test/testrunner.cpp

  # Test suites: GBA
  tarball  tests/arm7tdmi          SingleStepTests/ARM7TDMI e3097d88d428752736b949d94c094d5da50f0db6
  tarball  tests/jsmolka-gba-tests jsmolka/gba-tests a7113b67e63f83a9b321696ddd7042ccfad6c881
  tarball  tests/alyosha-gba-tests alyosha-tas/gba-tests 66f5f1d60cd7030b600606886c76f5c821ecaf9d
  codeberg tests/nba-hw-test       nba-emu/hw-test fbc99140e06f083c0a47612467cfbb02470e56dc
} finally {
  Pop-Location
}

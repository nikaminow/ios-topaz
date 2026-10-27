# Build the asteroids probe into a boot image v4 for `fastboot boot` (RAM only, never flash it).
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$llvm = 'C:\Program Files\LLVM\bin'
$out = Join-Path $here 'out'
New-Item -ItemType Directory -Force $out | Out-Null

$cflags = @('--target=aarch64-none-elf', '-O2', '-ffreestanding', '-nostdlib', '-fno-pic', '-mgeneral-regs-only',
	'-mstrict-align', '-fno-stack-protector', '-fno-jump-tables', '-Wall', '-Wextra')
foreach ($f in 'start.S', 'probe.c') {
	& "$llvm\clang.exe" @cflags -c (Join-Path $here $f) -o (Join-Path $out ($f -replace '\.\w+$', '.o'))
	if ($LASTEXITCODE) { throw "compile $f failed" }
}

# The image runs wherever ABL loads it with no relocation pass: no absolute relocations in C code.
$abs = & "$llvm\llvm-readelf.exe" -r (Join-Path $out 'probe.o') | Select-String 'ABS64'
if ($abs) { $abs; throw 'absolute relocations in probe.o' }

& "$llvm\ld.lld.exe" -static -T (Join-Path $here 'shim.ld') --build-id=none -N `
	(Join-Path $out 'start.o') (Join-Path $out 'probe.o') -o (Join-Path $out 'probe.elf')
if ($LASTEXITCODE) { throw 'link failed' }
& "$llvm\llvm-objcopy.exe" -O binary (Join-Path $out 'probe.elf') (Join-Path $out 'Image')

# Pad the Image to image_size (_end) so .bss and the stacks exist as zeroed bytes.
$end = & "$llvm\llvm-nm.exe" (Join-Path $out 'probe.elf') | Select-String ' _end$'
$size = [Convert]::ToInt64(($end -split ' ')[0], 16)
$fs = [IO.File]::Open((Join-Path $out 'Image'), 'Open')
$fs.SetLength($size); $fs.Close()

python (Join-Path $here '..\mkbootimg\mkbootimg.py') --header_version 4 --kernel (Join-Path $out 'Image') `
	--os_version 16.0.0 --os_patch_level 2025-09 --cmdline 'console=null' -o (Join-Path $out 'probe-boot.img')
if ($LASTEXITCODE) { throw 'mkbootimg failed' }
Get-Item (Join-Path $out 'Image'), (Join-Path $out 'probe-boot.img') | Select-Object Name, Length

# One command: boot iOS on the Nothing Phone (3a) from RAM, root disk served from this PC over USB.
#   powershell -ExecutionPolicy Bypass -File C:\Users\qu33rl1nk\nura-3a\ios-topaz\tools\asteroids-boot.ps1
# Nothing is flashed. Hold Power for 15 s at any time to get Android back.
$ErrorActionPreference = 'Stop'
$base = 'C:\Users\qu33rl1nk\nura-3a'
$img = "$base\ios-topaz\shim-asteroids\out\shim-boot.img"
$root = "$base\ios-root.img"
$env:Path = [Environment]::GetEnvironmentVariable('Path', 'User') + ';' + [Environment]::GetEnvironmentVariable('Path', 'Machine')
$fb = (Get-ChildItem -Recurse "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Filter fastboot.exe | Select-Object -First 1).FullName
foreach ($f in $img, $root) { if (-not (Test-Path $f)) { throw "missing $f" } }

Write-Host '1/3 phone -> fastboot (connect it with USB, Android running or already in fastboot)'
$n = 0
while (-not (& $fb devices) -and $n -lt 120) {
	if ((adb get-state 2>$null) -eq 'device') { adb reboot bootloader | Out-Null }
	Start-Sleep 2; $n++
}
if (-not (& $fb devices)) { throw 'no phone in fastboot' }

Write-Host '2/3 fastboot boot (RAM only)'
& $fb boot $img

Write-Host '3/3 serving the iOS root disk over USB: keep this window open while iOS runs'
python "$base\ios-topaz\tools\usb-disk-serve.py" $root

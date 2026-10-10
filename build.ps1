# Builds the sysmodule and overlay and refreshes dist\sd. Needs devkitPro (Switch).
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
# make cannot handle spaces in paths, so map the project to a spare drive letter.
$drive = "P:"
if (-not (Test-Path "$drive\")) { subst $drive $root; $mapped = $true }
$env:DEVKITPRO = "/opt/devkitpro"
$env:PATH = "C:\devkitPro\msys2\usr\bin;C:\devkitPro\devkitA64\bin;C:\devkitPro\tools\bin;$env:PATH"
foreach ($p in "sysmodule", "overlay") { Push-Location "$drive\$p"; make; Pop-Location }
$sd = "$root\dist\sd"
Copy-Item "$root\package\*" $sd -Recurse -Force
Copy-Item "$root\sysmodule\FujinNX.nsp" "$sd\atmosphere\contents\420000000000F001\exefs.nsp"
Copy-Item "$root\overlay\FujinNX.ovl" "$sd\switch\.overlays\FujinNX.ovl"
if ($mapped) { subst $drive /d }
Write-Host "Done. Copy dist\sd\* to the root of your SD card."


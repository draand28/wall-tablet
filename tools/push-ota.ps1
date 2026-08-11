# Build the firmware and stage wall_tablet.bin for an over-the-air update.
#
# The tablet downloads its new firmware from OTA_URL (see main/config.h),
# so the .bin must be reachable over HTTP. Two options:
#
#   A) Host on the always-on Pi (best for a wall-mounted tablet):
#        1. One-time on the Pi:  mkdir -p /home/pi/fw && cd /home/pi/fw
#                               python3 -m http.server 8000 &
#        2. From this PC:        .\tools\push-ota.ps1 -SshTarget pi@192.168.1.230
#                               (copies the .bin to /home/pi/fw/)
#        3. Tap "UPDATE" on the tablet.
#
#   B) Host from this PC (temporary):
#        1. .\tools\push-ota.ps1 -ServeLocal
#           (starts a local HTTP server; set OTA_URL in config.h to this PC's
#            LAN IP before building)
#        2. Tap "UPDATE" on the tablet.
#
param(
    [string]$SshTarget = "",     # e.g. pi@192.168.1.230 (copies to /home/pi/fw/)
    [switch]$ServeLocal
)

$ErrorActionPreference = "Stop"
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"
& "C:\esp\esp-idf\export.ps1" | Out-Null

$proj = "Y:\DevStuff\door tablet\wall-tablet"
$bin = "C:\Users\draan\AppData\Local\Temp\opencode\wall-tablet-build\wall_tablet.bin"

Write-Host "Building..."
idf.py -B "C:\Users\draan\AppData\Local\Temp\opencode\wall-tablet-build" build
if ($LASTEXITCODE -ne 0) { Write-Error "build failed"; exit 1 }

if ($SshTarget) {
    Write-Host "Copying wall_tablet.bin to $SshTarget:/home/pi/fw/"
    scp "$bin" "${SshTarget}:/home/pi/fw/wall_tablet.bin"
    Write-Host "Done. Tap UPDATE on the tablet."
} elseif ($ServeLocal) {
    $dir = "$env:TEMP\opencode\ota-fw"
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    Copy-Item $bin "$dir\wall_tablet.bin" -Force
    Write-Host "Serving $dir on :8000  (set OTA_URL to http://<THIS-PC-IP>:8000/wall_tablet.bin)"
    python -m http.server 8000 --directory $dir
} else {
    Write-Host "Build OK: $bin"
    Write-Host "Deploy with: .\tools\push-ota.ps1 -SshTarget pi@192.168.1.230   (or -ServeLocal)"
}

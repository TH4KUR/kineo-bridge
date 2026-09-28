<#
  wsl_warmup.ps1 -- run at Windows login to eliminate two real,
  observed causes of KB-USB-003 (see PROJECT_MEMORY.md investigation
  notes, 2026-09-27):

  1. Cold WSL2 boot race: usbipd's Shared/bind state survives a reboot
     but the WSL-side attach does not, and KineoBridge.exe's own retry
     budget in ensure_camera_ready() is too tight to also absorb a
     genuine cold WSL2 VM boot. Running this at login (well before the
     user opens Kineo) gives the VM boot and the attach all the time
     they need, with nothing user-facing waiting on it.

  2. WSL2 VM idle-shutdown drops the attach again later: confirmed via
     `Get-Process vmwp` showing a fresh VM worker StartTime hours after
     a successful attach, with no reboot in between -- WSL2 tears its
     lightweight VM down after a period with no running Linux process,
     which silently reverts the camera from Attached back to Shared.
     Fixed here by keeping one trivial `sleep infinity` process alive
     inside the distro (same setsid/nohup/disown/pidfile pattern the
     bridge itself uses, see PROJECT_MEMORY.md do-not-regress notes) so
     WSL never considers itself idle between login and whenever the
     user actually launches Kineo.

  Native Windows PowerShell (not bash) -- has to drive wsl.exe/usbipd.exe
  before WSL itself is even up. Never touches the registry or
  .wslconfig, never needs elevation (attach on an already-Shared device
  doesn't; see PROJECT_MEMORY.md section 14). If the camera is
  NotShared, this script leaves that one-time bind to KineoBridge.exe's
  own elevated launch.
#>

$ErrorActionPreference = "SilentlyContinue"

$LogDir  = "$env:LOCALAPPDATA\KineoBridge"
$LogFile = Join-Path $LogDir "warmup.log"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

function Log([string]$msg) {
    $line = "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $msg"
    Add-Content -Path $LogFile -Value $line
}

$CameraVidPid = "1409:8000"
$MaxAttempts  = 40
$SleepSeconds = 3

Log "warmup: starting"

# Force the WSL2 VM to fully boot. This blocks until the VM is actually
# up (or wsl.exe gives up on its own) -- that's the point: absorb the
# cold-boot cost here, not in KineoBridge.exe's own tight retry loop.
& wsl.exe -- true 2>$null | Out-Null
Log "warmup: wsl.exe -- true returned exit=$LASTEXITCODE"

function Get-CameraState {
    $list = (& usbipd.exe list 2>$null | Out-String)
    if (-not $list) { return $null, $null }

    $connectedIdx = $list.IndexOf("Connected:")
    if ($connectedIdx -lt 0) { return $null, $null }
    $persistedIdx = $list.IndexOf("Persisted:")
    $seg = if ($persistedIdx -gt $connectedIdx) {
        $list.Substring($connectedIdx, $persistedIdx - $connectedIdx)
    } else {
        $list.Substring($connectedIdx)
    }

    foreach ($line in ($seg -split "`r?`n")) {
        if ($line -match [regex]::Escape($CameraVidPid)) {
            $busid = ($line.Trim() -split '\s+')[0]
            if ($line -match "Not shared") { return $busid, "NotShared" }
            elseif ($line -match "Attached") { return $busid, "Attached" }
            elseif ($line -match "Shared") { return $busid, "Shared" }
            return $busid, "Unknown"
        }
    }
    return $null, $null
}

$busid = $null
$state = $null
for ($attempt = 0; $attempt -lt $MaxAttempts; $attempt++) {
    $busid, $state = Get-CameraState
    Log "warmup: attempt=$attempt state=$state busid=$busid"

    if ($state -eq "Attached") { break }
    if ($state -eq "NotShared") {
        Log "warmup: camera NotShared -- one-time bind needs elevation, leaving it to KineoBridge.exe"
        break
    }
    if ($state -eq "Shared" -and $busid) {
        & usbipd.exe attach --wsl --busid $busid 2>$null | Out-Null
    }
    Start-Sleep -Seconds $SleepSeconds
}

switch ($state) {
    "Attached"  { Log "warmup: camera attached, done" }
    "NotShared" { Log "warmup: done (deferred bind to KineoBridge.exe)" }
    default     { Log "warmup: gave up after $MaxAttempts attempts, state=$state busid=$busid -- KineoBridge.exe will retry at launch" }
}

# Keep the WSL2 VM from idling out and silently dropping the attach
# again before the user gets to Kineo. Uses flock as an atomic OS-level
# singleton instead of a captured PID: `echo $! > pidfile` after `cmd &`
# was tried first and found to be UNRELIABLE specifically through the
# wsl.exe interop path in this environment (verified by direct repro:
# `wsl.exe -- bash -c 'sleep 5 & echo $!'` prints an empty PID every
# time here, and the real, shipped ~/.local/lib/kineobridge/bridge.pid
# is likewise just a bare newline -- see chat notes, 2026-09-27). flock
# sidesteps that entirely: re-running this script is safe and never
# stacks up duplicate keepalive processes (verified: ran twice in a
# row, exactly one `sleep infinity` survived both times).
$keepaliveCmd = 'mkdir -p ~/.local/lib/kineobridge; setsid bash -c ''{ flock -n 9 || exit 0; exec sleep infinity; } 9>~/.local/lib/kineobridge/keepalive.lock'' < /dev/null > /dev/null 2>&1 & disown; sleep 1; echo STARTED'
$keepaliveArgs = "-- bash -lc `"$keepaliveCmd`""
$keepalivePsi = New-Object System.Diagnostics.ProcessStartInfo
$keepalivePsi.FileName = "wsl.exe"
$keepalivePsi.Arguments = $keepaliveArgs
$keepalivePsi.RedirectStandardOutput = $true
$keepalivePsi.RedirectStandardError = $true
$keepalivePsi.UseShellExecute = $false
$keepaliveProc = [System.Diagnostics.Process]::Start($keepalivePsi)
$keepaliveOut = $keepaliveProc.StandardOutput.ReadToEnd().Trim()
$keepaliveProc.WaitForExit(15000) | Out-Null
Log "warmup: keepalive=$keepaliveOut"

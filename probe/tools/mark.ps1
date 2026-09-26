# mark.ps1 -- interactive marker utility for the real-Kineo workflow trace.
#
# Run this in its OWN PowerShell window while Kineo is running. Type a short
# label and press Enter each time you take an action in Kineo's UI (launch,
# navigate, click analysis, click start, click stop, close, etc.) -- any
# short text is fine, no fixed vocabulary required. Type "exit" or "quit" to
# stop.
#
# Timestamps use the same local-clock, millisecond-precision format
# (HH:mm:ss.fff) as the CTI's own probe_log() output, so the two logs can be
# directly correlated by wall-clock time.
#
# Usage: .\mark.ps1 [-LogPath <path>]
param(
    [string]$LogPath = "$PSScriptRoot\markers.log"
)

Write-Host "Marker log: $LogPath"
Write-Host "Type a short label and press Enter for each action. Type 'exit' or 'quit' to stop."
Write-Host ""

"=== marker session started $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff') ===" | Out-File -FilePath $LogPath -Append -Encoding utf8

while ($true) {
    $line = Read-Host "MARK"
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    if ($line -eq 'exit' -or $line -eq 'quit') { break }
    $ts = Get-Date -Format 'HH:mm:ss.fff'
    $entry = "[$ts] MARK: $line"
    Write-Host $entry
    $entry | Out-File -FilePath $LogPath -Append -Encoding utf8
}

"=== marker session ended $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff') ===" | Out-File -FilePath $LogPath -Append -Encoding utf8
Write-Host "Marker session ended. Log saved to $LogPath"

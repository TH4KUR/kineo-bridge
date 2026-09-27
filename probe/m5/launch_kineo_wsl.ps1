# launch_kineo_wsl.ps1 -- M5 golden-baseline launch script (real camera).
#
# Session-scoped only: no permanent env vars, no registry/driver changes,
# no modification of the Kineo installation. Requires the WSL bridge
# server already running (see wsl-camera/README or start it manually):
#
#   cd ~/kineo-bridge/wsl-camera
#   GI_TYPELIB_PATH=~/aravis-0.8.36/build/src LD_LIBRARY_PATH=~/aravis-0.8.36/build/src \
#     python3 -u kineo_camera_bridge.py --source camera --host 0.0.0.0 --port 9494
#
# Usage: run this script from a PowerShell terminal (not dot-sourced from
# elsewhere -- it changes location to the Kineo install directory itself,
# which Kineo's own Electron main process needs in order to resolve its
# relative-path spawn of KineoDeviceService.exe).

$env:KINEO_BRIDGE_SOURCE = "wsl"
$env:GENICAM_GENTL64_PATH = "C:\Users\IMV\kineo-bridge-test\m1\m5"
Set-Location "C:\IMVapps\Kineo Software"
& ".\Kineo Software.exe"

# launch_kineo_synthetic.ps1 -- M5 golden-baseline launch script (synthetic
# source, no camera/WSL bridge required). Same CTI as launch_kineo_wsl.ps1,
# only the frame source differs -- useful as a quick regression check that
# doesn't depend on usbipd/WSL bridge state.

$env:KINEO_BRIDGE_SOURCE = "synthetic"
$env:GENICAM_GENTL64_PATH = "C:\Users\IMV\kineo-bridge-test\m1\m5"
Set-Location "C:\IMVapps\Kineo Software"
& ".\Kineo Software.exe"

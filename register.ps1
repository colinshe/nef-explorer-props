# Points Explorer at the NEF property handler. The system handler key needs an
# administrator token; the COM class itself is registered for this user only.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$dll = Join-Path $here 'NefPropHandler.dll'
$log = Join-Path $here 'register.log'
$clsid = '{C4E8A1D7-6B25-4F0E-9A33-8D5F2E7B1C46}'

function Write-Log([string]$msg) {
    Add-Content -Path $log -Value ('{0} {1}' -f (Get-Date -Format 's'), $msg)
    Write-Output $msg
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
$admin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Write-Log 'Requesting administrator approval.'
    $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $arg = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    $proc = Start-Process -FilePath $ps -Verb RunAs -Wait -PassThru -ArgumentList $arg
    exit $proc.ExitCode
}

if (-not (Test-Path $dll)) {
    Write-Log "Missing $dll"
    exit 1
}

$userRoot = "HKCU\Software\Classes\CLSID\$clsid"
& reg.exe add $userRoot /ve /d 'NEF Photo Property Handler' /f | Out-Null
& reg.exe add $userRoot /v ManualSafeSave /t REG_DWORD /d 1 /f | Out-Null
& reg.exe add $userRoot /v DisableProcessIsolation /t REG_DWORD /d 1 /f | Out-Null
& reg.exe add "$userRoot\InProcServer32" /ve /d $dll /f | Out-Null
& reg.exe add "$userRoot\InProcServer32" /v ThreadingModel /d Both /f | Out-Null

$schema = Join-Path $here 'nef-datetaken.propdesc'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NefSchema {
  [DllImport("propsys.dll", CharSet = CharSet.Unicode)]
  public static extern int PSRegisterPropertySchema(string pszPath);
  [DllImport("propsys.dll", CharSet = CharSet.Unicode)]
  public static extern int PSUnregisterPropertySchema(string pszPath);
}
'@
[NefSchema]::PSUnregisterPropertySchema($schema) | Out-Null
$schemaHr = [NefSchema]::PSRegisterPropertySchema($schema)
Write-Log ("Date taken schema 0x{0:X8}" -f ($schemaHr -band 0xffffffff))
if ($schemaHr -lt 0) { exit 1 }

$preview = 'prop:PhotoGeoTagger.DateTaken;System.Image.Dimensions;System.Size;System.Author;System.Photo.CameraManufacturer;System.Photo.CameraModel;System.Photo.CameraSerialNumber;System.Photo.ISOSpeed;System.Photo.FNumber;System.Photo.ExposureTime;System.Photo.ExposureBias;System.Photo.ExposureProgram;System.Photo.MeteringMode;System.Photo.Flash;System.Photo.FocalLength;System.Photo.LensManufacturer;System.Photo.LensModel;System.GPS.Latitude;System.GPS.Longitude;System.GPS.Altitude'
foreach ($ext in '.nef', '.nrw') {
    $assoc = "HKCU:\Software\Classes\SystemFileAssociations\$ext"
    New-Item -Path $assoc -Force | Out-Null
    $src = Get-ItemProperty "HKLM:\SOFTWARE\Classes\SystemFileAssociations\$ext"
    $full = $src.FullDetails.Replace('System.Photo.DateTaken', 'PhotoGeoTagger.DateTaken')
    Set-ItemProperty -Path $assoc -Name FullDetails -Value $full
    Set-ItemProperty -Path $assoc -Name PreviewDetails -Value $preview
}

$backup = 'HKCU\Software\PhotoGeoTagger\NefPropertyHandler'
& reg.exe add $backup /f | Out-Null
foreach ($ext in '.nef', '.nrw') {
    $handler = "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\PropertySystem\PropertyHandlers\$ext"
    $old = (& reg.exe query $handler /ve | Select-String 'REG_SZ').ToString().Split(' ', [System.StringSplitOptions]::RemoveEmptyEntries)[-1]
    if ($old -ne $clsid) {
        & reg.exe add $backup /v $ext /d $old /f | Out-Null
        Write-Log "Saved previous $ext handler $old"
    }
    & reg.exe add $handler /ve /d $clsid /f | Out-Null
    $now = (& reg.exe query $handler /ve | Select-String 'REG_SZ').ToString().Split(' ', [System.StringSplitOptions]::RemoveEmptyEntries)[-1]
    Write-Log "$ext handler is $now"
    if ($now -ne $clsid) { exit 1 }
}

Write-Log 'Restarting Explorer.'
Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) {
    Start-Process explorer.exe
}
Write-Log 'Registered.'
exit 0

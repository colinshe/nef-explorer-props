# Restores the Windows photo property handler for .nef and .nrw.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$dll = Join-Path $here 'NefPropHandler.dll'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
$admin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    $arg = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    $proc = Start-Process -FilePath powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList $arg
    exit $proc.ExitCode
}

if (Test-Path $dll) {
    $regsvr = Join-Path $env:SystemRoot 'System32\regsvr32.exe'
    Start-Process -FilePath $regsvr -ArgumentList @('/s', '/u', $dll) -Wait | Out-Null
}
$schema = Join-Path $here 'nef-datetaken.propdesc'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NefSchema {
  [DllImport("propsys.dll", CharSet = CharSet.Unicode)]
  public static extern int PSUnregisterPropertySchema(string pszPath);
}
'@
if (Test-Path $schema) { [NefSchema]::PSUnregisterPropertySchema($schema) | Out-Null }
foreach ($ext in '.nef', '.nrw') {
    $assoc = "HKCU:\Software\Classes\SystemFileAssociations\$ext"
    if (Test-Path $assoc) {
        Remove-ItemProperty -Path $assoc -Name PreviewDetails -ErrorAction SilentlyContinue
        Remove-ItemProperty -Path $assoc -Name FullDetails -ErrorAction SilentlyContinue
    }
}

Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) {
    Start-Process explorer.exe
}
Write-Output 'Removed. .nef Properties uses the Windows photo handler again.'

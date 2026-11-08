<#
.SYNOPSIS
    Installs the CAMForAlibre add-on into Alibre Design.

.DESCRIPTION
    1. Copies CAMForAlibre.dll, CAMForAlibre.adc and CAMForAlibre.ico from the build output to
       %ProgramData%\Alibre AddOns\CAMForAlibre\
    2. Registers the add-on under HKLM\SOFTWARE\Alibre Design Add-Ons
       (value name = add-on GUID, data = install folder).

    Needs administrator rights; re-launches itself elevated if necessary.
    Close Alibre Design first (the DLL is locked while Alibre runs), and
    restart it afterwards - Alibre only reads add-ons at startup.

.PARAMETER Configuration
    Build configuration to install: Release (default) or Debug.
#>
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$AddOnName = 'CAMForAlibre'
$AddOnGuid = '{D3A31918-AFE0-45F2-A080-B531E6894179}'   # must match CAMForAlibre.adc
$RegKey    = 'SOFTWARE\Alibre Design Add-Ons'

# --- Elevation ---------------------------------------------------------------
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'Administrator rights are required - relaunching elevated...'
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Configuration', $Configuration)
    Start-Process -FilePath 'powershell.exe' -ArgumentList $argList -Verb RunAs -Wait
    exit
}

try {
    # --- Locate build output --------------------------------------------------
    $projectDir = Split-Path -Parent $PSScriptRoot
    $buildDir   = Join-Path $projectDir "bin\x64\$Configuration"
    $files      = @("$AddOnName.dll", "$AddOnName.adc", "$AddOnName.ico")

    foreach ($f in $files) {
        if (-not (Test-Path (Join-Path $buildDir $f))) {
            throw "Missing '$f' in '$buildDir'. Build the $Configuration|x64 configuration first."
        }
    }

    # --- Alibre must not be running ------------------------------------------
    if (Get-Process -Name 'Alibre Design' -ErrorAction SilentlyContinue) {
        throw 'Alibre Design is running. Save your work, close Alibre Design, then run this again.'
    }

    # --- Copy files ------------------------------------------------------------
    $targetDir = Join-Path $env:ProgramData "Alibre AddOns\$AddOnName"
    New-Item -ItemType Directory -Path $targetDir -Force | Out-Null   # creates 'Alibre AddOns' too
    foreach ($f in $files) {
        Copy-Item -Path (Join-Path $buildDir $f) -Destination $targetDir -Force
    }
    $pdb = Join-Path $buildDir "$AddOnName.pdb"
    if (Test-Path $pdb) { Copy-Item -Path $pdb -Destination $targetDir -Force }
    Write-Host "Copied add-on files to $targetDir"

    # --- Register --------------------------------------------------------------
    # Use the .NET API: New-Item -Force on HKLM:\SOFTWARE fails even when elevated.
    $key = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey($RegKey)
    try {
        $key.SetValue($AddOnGuid, $targetDir, [Microsoft.Win32.RegistryValueKind]::String)
    }
    finally {
        $key.Close()
    }
    Write-Host "Registered $AddOnGuid -> $targetDir under HKLM\$RegKey"

    Write-Host ''
    Write-Host 'Done. Start Alibre Design and look for the "CAMForAlibre" ribbon tab.' -ForegroundColor Green
}
catch {
    Write-Host "Install failed: $($_.Exception.Message)" -ForegroundColor Red
}

if ($Host.Name -eq 'ConsoleHost') {
    Write-Host ''
    Read-Host 'Press Enter to close'
}

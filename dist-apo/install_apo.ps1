# install_apo.ps1 - install / uninstall the SoundLock APO (must be elevated).
# Log is UTF-8 without BOM so every reader shows it correctly.
param([switch]$Uninstall, [switch]$Sign)

# host guard: never run under Windows PowerShell 5.1 - re-exec into PS7
if ($PSVersionTable.PSVersion.Major -lt 7) {
    $pwsh7 = 'D:\toolchain\pwsh7\pwsh.exe'
    if (Test-Path $pwsh7) {
        & $pwsh7 -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -Uninstall:$Uninstall -Sign:$Sign
        exit $LASTEXITCODE
    }
    Write-Host "ERROR: PowerShell 7 not found at $pwsh7" -ForegroundColor Red
    exit 1
}

$ErrorActionPreference = 'Continue'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$log  = Join-Path $here 'install_log.txt'
$cls  = '{A5C6D1E0-3B7F-4C21-9E44-9E1D2C0A7781}'
$pkey = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}'
$utf8 = New-Object System.Text.UTF8Encoding($false)
$nl   = [Environment]::NewLine

function Log([string]$m) {
    $line = "{0}  {1}" -f (Get-Date -Format 'HH:mm:ss'), $m
    [System.IO.File]::AppendAllText($log, $line + $nl, $utf8)
    Write-Host $line
}

[System.IO.File]::WriteAllText($log, "", $utf8)
Log "=== SoundLock APO $(if($Uninstall){'uninstall'}else{'install'}) ==="
Log ("host: PowerShell " + $PSVersionTable.PSVersion.ToString() + " (" + $PSVersionTable.PSEdition + ")")

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Log "admin=$isAdmin"
if (-not $isAdmin) { Log "ERROR: administrator rights required"; exit 1 }

# ---------------------------------------------------------------- uninstall
if ($Uninstall) {
    Stop-Service audiosrv -Force -ErrorAction SilentlyContinue
    Stop-Service AudioEndpointBuilder -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 600

    foreach ($k in Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render' -ErrorAction SilentlyContinue) {
        $fx = $k.PSPath + '\Fx\0'
        if (Test-Path $fx) {
            foreach ($n in @("$pkey,1", "$pkey,2", "$pkey,3")) {
                Remove-ItemProperty -Path $fx -Name $n -ErrorAction SilentlyContinue
            }
            Log "detached $($k.PSChildName)"
        }
    }
    Remove-Item -Path "Registry::HKEY_CLASSES_ROOT\CLSID\$cls" -Recurse -Force -ErrorAction SilentlyContinue
    Log "COM class removed"
    $d = Join-Path $env:SystemRoot 'System32\SoundLockApo.dll'
    if (Test-Path $d) { Remove-Item $d -Force -ErrorAction SilentlyContinue; Log "dll deleted" }

    Start-Service AudioEndpointBuilder -ErrorAction SilentlyContinue
    Start-Service audiosrv -ErrorAction SilentlyContinue
    Log "uninstalled"
    exit 0
}

# ---------------------------------------------------------------- sign
$dll = Join-Path $here 'SoundLockApo.dll'
$ctl = Join-Path $here 'SoundLockApoCtl.exe'
if ($Sign) {
    $subject = 'CN=SoundLock Self-Signed'
    $cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq $subject } | Select-Object -First 1
    if (-not $cert) {
        Log "creating self-signed code signing certificate"
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -CertStoreLocation Cert:\LocalMachine\My -NotAfter (Get-Date).AddYears(5)
    } else { Log "reusing certificate $($cert.Thumbprint)" }

    $cer = Join-Path $env:TEMP 'SoundLockSelfSign.cer'
    Export-Certificate -Cert $cert -FilePath $cer -Force | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    Log "certificate trusted (Root + TrustedPublisher)"

    foreach ($f in @($dll, $ctl)) {
        if (Test-Path $f) {
            $r = Set-AuthenticodeSignature -FilePath $f -Certificate $cert -HashAlgorithm SHA256
            Log "signed $(Split-Path -Leaf $f): $($r.Status)"
        }
    }
}

# ---------------------------------------------------------------- install
$dst = Join-Path $env:SystemRoot 'System32\SoundLockApo.dll'
Copy-Item $dll $dst -Force
Log "copied dll -> $dst"

New-Item -Path "Registry::HKEY_CLASSES_ROOT\CLSID\$cls" -Force | Out-Null
Set-ItemProperty -Path "Registry::HKEY_CLASSES_ROOT\CLSID\$cls" -Name '(default)' -Value 'SoundLock APO'
New-Item -Path "Registry::HKEY_CLASSES_ROOT\CLSID\$cls\InprocServer32" -Force | Out-Null
Set-ItemProperty -Path "Registry::HKEY_CLASSES_ROOT\CLSID\$cls\InprocServer32" -Name '(default)' -Value $dst
Set-ItemProperty -Path "Registry::HKEY_CLASSES_ROOT\CLSID\$cls\InprocServer32" -Name 'ThreadingModel' -Value 'Both'
Log "COM class registered"

Log "NOTE: endpoint attachment is not possible on this Windows build:"
Log "  - SetupDiSetDeviceProperty(PKEY_FX_*) returns ERROR_INVALID_DATA"
Log "  - hand-written MMDevices Fx values are deleted by AudioEndpointBuilder"
Log "  a signed driver package (extension INF) is required instead"

Start-Sleep -Milliseconds 300
Log "audio services left running"

$mods = @()
Get-Process audiodg -ErrorAction SilentlyContinue | ForEach-Object {
    $mods += ($_.Modules | Where-Object { $_.ModuleName -like 'SoundLock*' } | ForEach-Object { $_.ModuleName })
}
if ($mods.Count) { Log ("engine loaded: " + ($mods -join ',')) }
else             { Log "engine not loaded yet (play audio, then re-check)" }
Log "done"

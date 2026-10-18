# install_apo.ps1 - install / uninstall the SoundLock APO (must be elevated).
# Log is UTF-8 without BOM so every reader shows it correctly.
param([switch]$Uninstall, [switch]$Sign)

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

$attachTool = Join-Path $here 'apo_attach.exe'
if (Test-Path $attachTool) {
    & $attachTool attach 2>&1 | ForEach-Object { Log "  [attach] $_" }
    Log "attach exit=$LASTEXITCODE"
}

# AudioEndpointBuilder keeps endpoint properties in memory: stop it before writing
# the FX key, otherwise the values are reverted when its cache is flushed.
Stop-Service audiosrv -Force -ErrorAction SilentlyContinue
Stop-Service AudioEndpointBuilder -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800

$attached = 0
foreach ($k in Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render' -ErrorAction SilentlyContinue) {
    $state = (Get-ItemProperty $k.PSPath -Name DeviceState -ErrorAction SilentlyContinue).DeviceState
    if ($state -ne 1) { continue }
    $props = $k.PSPath + '\Properties'
    $name = (Get-ItemProperty $props -ErrorAction SilentlyContinue).'{a45c254e-df1c-4efd-8020-67d146a850e0},2'
    if (-not $name) { $name = '(unnamed)' }
    if ("$name" -match 'CABLE|Broadcast|Virtual|VoiceMeeter') { Log "skip [$name]"; continue }

    $fx = $k.PSPath + '\Fx\0'
    New-Item -Path $fx -Force | Out-Null
    foreach ($n in @("$pkey,1", "$pkey,2", "$pkey,3")) {
        New-ItemProperty -Path $fx -Name $n -Value $cls -PropertyType String -Force | Out-Null
    }
    $check = (Get-ItemProperty $fx)."$pkey,1"
    Log "attached [$name] -> $check"
    $attached++
}
Log "endpoints attached: $attached"

Start-Service AudioEndpointBuilder -ErrorAction SilentlyContinue
Start-Service audiosrv -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1500
Log "audio services restarted"

$mods = @()
Get-Process audiodg -ErrorAction SilentlyContinue | ForEach-Object {
    $mods += ($_.Modules | Where-Object { $_.ModuleName -like 'SoundLock*' } | ForEach-Object { $_.ModuleName })
}
if ($mods.Count) { Log ("engine loaded: " + ($mods -join ',')) }
else             { Log "engine not loaded yet (play audio, then re-check)" }
Log "done"

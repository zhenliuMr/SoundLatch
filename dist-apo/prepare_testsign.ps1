# prepare_testsign.ps1 - sign the APO driver package and enable Windows test signing.
# Run elevated. Requires a reboot afterwards.
param([switch]$Disable)

# host guard: never run under Windows PowerShell 5.1
if ($PSVersionTable.PSVersion.Major -lt 7) {
    $pwsh7 = 'D:\toolchain\pwsh7\pwsh.exe'
    if (Test-Path $pwsh7) {
        & $pwsh7 -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -Disable:$Disable
        exit $LASTEXITCODE
    }
}

$ErrorActionPreference = 'Continue'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$log    = Join-Path $here 'prepare_log.txt'
$utf8   = New-Object System.Text.UTF8Encoding($false)
$nl     = [Environment]::NewLine
$signtool = 'D:\toolchain\sdkx\bin\10.0.28000.0\x64\signtool.exe'

function Log([string]$m) {
    $line = "{0}  {1}" -f (Get-Date -Format 'HH:mm:ss'), $m
    [System.IO.File]::AppendAllText($log, $line + $nl, $utf8)
    Write-Host $line
}

[System.IO.File]::WriteAllText($log, "", $utf8)
Log "=== prepare test signing ==="
Log ("host: " + $PSVersionTable.PSVersion.ToString())

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Log "admin=$isAdmin"
if (-not $isAdmin) { Log "ERROR: administrator rights required"; exit 1 }

if ($Disable) {
    bcdedit /set testsigning off | Out-Null
    Log "testsigning disabled (reboot to apply)"
    exit 0
}

# ---------------------------------------------------------------- certificate
$subject = 'CN=SoundLock Self-Signed'
$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq $subject } | Select-Object -First 1
if (-not $cert) {
    Log "creating self-signed code signing certificate"
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -CertStoreLocation Cert:\LocalMachine\My -NotAfter (Get-Date).AddYears(5)
}
Log "certificate $($cert.Thumbprint)"

$cer = Join-Path $env:TEMP 'SoundLockSelfSign.cer'
Export-Certificate -Cert $cert -FilePath $cer -Force | Out-Null
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
Log "certificate trusted"

# ---------------------------------------------------------------- sign
Set-Location $here
foreach ($f in @('soundlockapo.cat', 'SoundLockApo.dll', 'SoundLockApoCtl.exe')) {
    if (-not (Test-Path $f)) { Log "missing $f"; continue }
    if (Test-Path $signtool) {
        $out = & $signtool sign /fd SHA256 /a /sm /s My /sha1 $cert.Thumbprint $f 2>&1
        Log "signtool $f -> exit=$LASTEXITCODE"
        $out | ForEach-Object { Log "  $_" }
    } else {
        $r = Set-AuthenticodeSignature -FilePath $f -Certificate $cert -HashAlgorithm SHA256
        Log "Set-AuthenticodeSignature $f -> $($r.Status)"
    }
}

if (Test-Path $signtool) {
    $v = & $signtool verify /pa /v soundlockapo.cat 2>&1
    Log "verify catalog exit=$LASTEXITCODE"
    ($v | Select-Object -First 6) | ForEach-Object { Log "  $_" }
}

# ---------------------------------------------------------------- Secure Boot check
$sb = $null
try { $sb = Confirm-SecureBootUEFI } catch { $sb = $null }
Log "SecureBoot=$sb"
if ($sb -eq $true) {
    Log "WARNING: Secure Boot is ON - trying anyway to see what the firmware says"
}

# ---------------------------------------------------------------- enable test signing
$out = bcdedit /set testsigning on 2>&1
Log ("bcdedit /set -> exit=" + $LASTEXITCODE)
$out | ForEach-Object { Log "  $_" }
if ($LASTEXITCODE -eq 0) {
    $state = (bcdedit /enum "{current}" | Select-String -Pattern 'testsigning')
    Log ("bcdedit: " + ($state -join ' '))
    Log "DONE - reboot now, then run install_apo.bat"
} else {
    Log "BLOCKED: test signing could not be enabled."
    Log "         Secure Boot is ON. Disable Secure Boot in BIOS/UEFI, boot Windows,"
    Log "         then run this script again. Signing itself already succeeded."
    exit 3
}

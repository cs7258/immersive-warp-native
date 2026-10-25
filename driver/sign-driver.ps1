# Sign-driver.ps1 - self-signed code-signing cert + test-sign IWNPPL.sys.
# The exported .cer must be installed on the target machine into
# Trusted Root AND Trusted Publishers (testsigning mode).
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$sys = Join-Path $root "build\IWNPPL.sys"
if (-not (Test-Path $sys)) { throw "IWNPPL.sys not built" }

$cert = New-SelfSignedCertificate -Type CodeSigningCert `
    -Subject "CN=IWN Test Driver" -KeyUsage DigitalSignature `
    -KeyAlgorithm RSA -KeyLength 2048 `
    -CertStoreLocation "Cert:\CurrentUser\My"
$pfxPass = ConvertTo-SecureString -String "IWNPPL-test" -Force -AsPlainText
Export-PfxCertificate -Cert $cert -FilePath "$root\build\IWNTestSigning.pfx" -Password $pfxPass | Out-Null
Export-Certificate -Cert $cert -FilePath "$root\build\IWNTestSigning.cer" | Out-Null
Write-Output ("cert: " + $cert.Thumbprint)

$signtool = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin" -Recurse -Filter signtool.exe |
    Where-Object { $_.FullName -match '\\x64\\' } |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $signtool) { throw "signtool.exe not found" }

& $signtool.FullName sign /fd SHA256 /f "$root\build\IWNTestSigning.pfx" /p "IWNPPL-test" "$sys"
if ($LASTEXITCODE -ne 0) { throw "signtool sign failed" }
# NOTE: no signtool verify here - the self-signed root is untrusted on the
# CI machine and importing it into the Root store pops a dialog that hangs
# non-interactive runners. The user installs IWNTestSigning.cer into
# Trusted Root + Trusted Publishers before loading the driver.
Write-Output "signed OK"

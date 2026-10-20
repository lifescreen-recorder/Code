param(
    [string]$FilePath
)

if (Test-Path $FilePath) {
    $cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -like "*Baneronetwo*" } | Select-Object -First 1
    if (-not $cert) {
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=Baneronetwo (BANSAFAn)" -CertStoreLocation "Cert:\CurrentUser\My" -NotAfter (Get-Date).AddYears(5)
    }
    Set-AuthenticodeSignature -FilePath $FilePath -Certificate $cert
    $sig = Get-AuthenticodeSignature -FilePath $FilePath
    Write-Host "Authenticode Signer: $($sig.SignerCertificate.Subject)"
    Write-Host "Authenticode Status: $($sig.Status)"
}

# Yayin dosyalarini olusturur (dist\):
#   KonvoyTP_Kurulum.exe  -> insanlarin indirdigi tek dosya (mod + SPF Framework icinde; kur / guncelle / kaldir)
#   KonvoyTP.dll          -> oyun acilisinda modun kendini guncellemesi icin
#   KonvoyTP_Kurulum.zip  -> eski indirme linkleri bozulmasin diye: icinde KonvoyTP_Kurulum.exe + OKU_BENI.txt
# SPF-Framework yoksa resmi GitHub surumunden (v1.2.7, Apache-2.0) indirilir ve SHA256 ile dogrulanir.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

# --- SPF-Framework 1.2.7 (resmi surum) ---
$spf = Join-Path $root 'third_party\SPF-Framework-1.2.7'
$spfDll = Join-Path $spf 'spf-framework.dll'
$spfSha = 'A38F1151E5D14B91D153EA2563FFB7C1F9C6E3FA8BD7D15FE96CDD247C001886'
if (-not (Test-Path $spfDll)) {
  $url = 'https://github.com/TrackAndTruckDevs/SPF-Framework/releases/download/v1.2.7/spf-framework_v1.2.7.zip'
  $tmp = Join-Path $env:TEMP ('spf127_' + [guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Force -Path $tmp | Out-Null
  Write-Host "SPF-Framework 1.2.7 indiriliyor..." -ForegroundColor Cyan
  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
  Invoke-WebRequest -Uri $url -OutFile (Join-Path $tmp 'spf.zip') -UseBasicParsing
  Expand-Archive -Path (Join-Path $tmp 'spf.zip') -DestinationPath (Join-Path $tmp 'x') -Force
  New-Item -ItemType Directory -Force -Path (Join-Path $spf 'spfAssets\localization') | Out-Null
  Copy-Item (Join-Path $tmp 'x\spf-framework.dll') $spfDll -Force
  Copy-Item (Join-Path $tmp 'x\spfAssets\localization\*') (Join-Path $spf 'spfAssets\localization') -Force
  Remove-Item -Recurse -Force $tmp
}
if ((Get-FileHash $spfDll -Algorithm SHA256).Hash -ne $spfSha) { throw "spf-framework.dll beklenen SPF 1.2.7 dosyasi degil (SHA256 uyusmuyor)." }

# --- derle (SPF bulundugu icin uygulamanin icine gomulur) ---
& cmd /c (Join-Path $root 'build.bat')
if ($LASTEXITCODE -ne 0) { throw "Derleme basarisiz." }
$app = Join-Path $root 'build\out\app\KonvoyTP_Kurulum.exe'
$dll = Join-Path $root 'build\out\KonvoyTP\KonvoyTP.dll'
if ((Get-Item $app).Length -lt 10MB) { throw "Uygulamanin icinde SPF Framework yok gibi (dosya cok kucuk)." }

# --- dist ---
$dist = Join-Path $root 'dist'
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force -Path $dist | Out-Null
Copy-Item $app (Join-Path $dist 'KonvoyTP_Kurulum.exe')
Copy-Item $dll (Join-Path $dist 'KonvoyTP.dll')
$z = Join-Path $dist 'zip\KonvoyTP_Kurulum'
New-Item -ItemType Directory -Force -Path $z | Out-Null
Copy-Item $app (Join-Path $z 'KonvoyTP_Kurulum.exe')
Copy-Item (Join-Path $root 'OKU_BENI.txt') $z
Compress-Archive -Path $z -DestinationPath (Join-Path $dist 'KonvoyTP_Kurulum.zip')
Remove-Item -Recurse -Force (Join-Path $dist 'zip')

Get-ChildItem $dist | ForEach-Object { "{0,-24} {1,12:N0} bayt  {2}" -f $_.Name, $_.Length, (Get-FileHash $_.FullName -Algorithm SHA256).Hash.Substring(0, 16) }
Write-Host "Paket hazir: $dist" -ForegroundColor Green

# Arkadaslara verilecek kurulum paketini olusturur: dist\KonvoyTP_Kurulum (+ .zip)
# SPF-Framework yoksa resmi GitHub surumunden (v1.2.7, Apache-2.0) indirilir.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$dll = Join-Path $root 'build\out\KonvoyTP\KonvoyTP.dll'
if (-not (Test-Path $dll)) { throw "Once build.bat ile derle: $dll yok" }

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
Copy-Item (Join-Path $root 'SPF_API\LICENSE-SPF-Framework.txt') (Join-Path $spf 'LICENSE-SPF-Framework.txt') -Force

# --- paket ---
$dist = Join-Path $root 'dist\KonvoyTP_Kurulum'
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
$files = Join-Path $dist 'dosyalar'
New-Item -ItemType Directory -Force -Path (Join-Path $files 'KonvoyTP') | Out-Null

Copy-Item (Join-Path $root 'installer\KUR.bat') $dist
Copy-Item (Join-Path $root 'installer\KALDIR.bat') $dist
Copy-Item (Join-Path $root 'OKU_BENI.txt') $dist
Copy-Item (Join-Path $root 'installer\kur.ps1') $files
Copy-Item $dll (Join-Path $files 'KonvoyTP')
Copy-Item -Recurse (Join-Path $root 'localization') (Join-Path $files 'KonvoyTP\localization')
Copy-Item -Recurse $spf (Join-Path $files 'SPF')

$zip = Join-Path $root 'dist\KonvoyTP_Kurulum.zip'
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $dist -DestinationPath $zip
Write-Host "Paket hazir: $zip" -ForegroundColor Green

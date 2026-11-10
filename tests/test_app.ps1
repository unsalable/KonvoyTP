# KonvoyTP uygulamasi uctan uca test: sahte bir oyun klasorune kur / guncelle / kaldir.
# Gercek oyun klasorune, masaustune, Baslat menusune ve kayit defterine DOKUNMAZ
# (KONVOYTP_TEST_KLASORU ile hepsi test klasorune yonlendirilir).
param([string]$Exe = "")
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $Exe) { $Exe = Join-Path $root 'build\out\app\KonvoyTP_Kurulum.exe' }
$T = Join-Path $env:TEMP ('ktp_apptest_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $T | Out-Null
$env:KONVOYTP_TEST_KLASORU = $T
$pass = 0; $fail = 0
function Check([bool]$c, [string]$m) { if ($c) { $script:pass++ } else { $script:fail++; Write-Host "  FAIL: $m" -ForegroundColor Red } }
function Run([string[]]$a) { $p = Start-Process -FilePath $Exe -ArgumentList $a -Wait -PassThru -WindowStyle Hidden; return $p.ExitCode }
function NewGame([string]$name) {
  $g = Join-Path $T $name
  New-Item -ItemType Directory -Force (Join-Path $g 'bin\win_x64\plugins') | Out-Null
  Set-Content -Path (Join-Path $g 'bin\win_x64\eurotrucks2.exe') -Value 'x'
  return $g
}
function Enabled([string]$g) {
  $f = Join-Path $g 'bin\win_x64\plugins\spfAssets\config\framework_settings.json'
  if (-not (Test-Path $f)) { return $null }
  $j = Get-Content $f -Raw | ConvertFrom-Json
  return $j.settings.plugin_states.KonvoyTP.enabled
}
$spfSha = 'A38F1151E5D14B91D153EA2563FFB7C1F9C6E3FA8BD7D15FE96CDD247C001886'

Write-Host "1) temiz oyuna kurulum (SPF yok, ayar dosyasi yok)"
$g1 = NewGame 'oyun1'
$rc = Run @('--kur', '--sessiz', '--oyun', "`"$g1`"")
$pl = Join-Path $g1 'bin\win_x64\plugins'
Check ($rc -eq 0) "cikis kodu 0 olmali ($rc)"
Check ((Get-Item (Join-Path $pl 'spfPlugins\KonvoyTP\KonvoyTP.dll')).VersionInfo.FileVersion -like '1.3.0*') "KonvoyTP.dll 1.3.0 kurulmali"
Check ((Get-FileHash (Join-Path $pl 'spf-framework.dll')).Hash -eq $spfSha) "resmi SPF 1.2.7 kurulmali"
Check ((Get-ChildItem (Join-Path $pl 'spfAssets\localization') -Filter *.json).Count -eq 15) "SPF dil dosyalari"
Check (Test-Path (Join-Path $pl 'spfAssets\licenses\SPF-Framework-LICENSE.txt')) "SPF lisansi"
Check (Test-Path (Join-Path $pl 'spfPlugins\KonvoyTP\localization\tr.json')) "eklenti metinleri"
Check ((Enabled $g1) -eq $true) "SPF ayarinda KonvoyTP acik olmali"
Check (Test-Path (Join-Path $T 'Programs\KonvoyTP\KonvoyTP.exe')) "uygulama kopyalanmali"
Check (Test-Path (Join-Path $T 'Desktop\KonvoyTP.lnk')) "masaustu kisayolu"
Check (Test-Path (Join-Path $T 'StartMenu\KonvoyTP.lnk')) "Baslat kisayolu"
Check (Test-Path (Join-Path $T 'kaldirma_kaydi.txt')) "kaldirma kaydi"

Write-Host "2) SPF ayar dosyasi var, KonvoyTP listede yok -> eklenir, diger her sey korunur"
$g2 = NewGame 'oyun2'
$cfgDir = Join-Path $g2 'bin\win_x64\plugins\spfAssets\config'
New-Item -ItemType Directory -Force $cfgDir | Out-Null
$cfg = @'
{
    "settings": {
        "notification_duration": 3.0,
        "plugin_states": {
            "KujeesLens": {
                "enabled": true
            }
        },
        "scale": 1.0
    },
    "localization": { "language": "en" }
}
'@
[IO.File]::WriteAllText((Join-Path $cfgDir 'framework_settings.json'), $cfg)
$rc = Run @('--kur', '--sessiz', '--uygulamasiz', '--oyun', "`"$g2`"")
Check ($rc -eq 0) "cikis kodu 0 ($rc)"
$j = Get-Content (Join-Path $cfgDir 'framework_settings.json') -Raw | ConvertFrom-Json
Check ($j.settings.plugin_states.KonvoyTP.enabled -eq $true) "KonvoyTP eklendi ve acik"
Check ($j.settings.plugin_states.KujeesLens.enabled -eq $true -and $j.settings.scale -eq 1.0 -and $j.localization.language -eq 'en') "diger ayarlar korunmali"
Check (Test-Path (Join-Path $cfgDir 'framework_settings.json.konvoytp-yedek')) "ayar dosyasinin yedegi"

Write-Host "3) KonvoyTP kapali -> acilir; eski surum mod -> guncellenir; yeni SPF -> dokunulmaz"
$g3 = NewGame 'oyun3'
$cfgDir3 = Join-Path $g3 'bin\win_x64\plugins\spfAssets\config'
New-Item -ItemType Directory -Force $cfgDir3 | Out-Null
[IO.File]::WriteAllText((Join-Path $cfgDir3 'framework_settings.json'), '{ "settings": { "plugin_states": { "KonvoyTP": { "enabled": false } } } }')
$ktp3 = Join-Path $g3 'bin\win_x64\plugins\spfPlugins\KonvoyTP'
New-Item -ItemType Directory -Force $ktp3 | Out-Null
$old = Join-Path $env:TEMP 'ktp_old_dll_probe.dll'
$real = 'F:\SteamLibrary\steamapps\common\Euro Truck Simulator 2\bin\win_x64\plugins\spfPlugins\KonvoyTP\KonvoyTP.dll'
if (Test-Path $real) { Copy-Item $real (Join-Path $ktp3 'KonvoyTP.dll') } else { Set-Content (Join-Path $ktp3 'KonvoyTP.dll') 'old' }
Copy-Item (Join-Path $pl 'spf-framework.dll') (Join-Path $g3 'bin\win_x64\plugins\spf-framework.dll')
$spfTime = (Get-Item (Join-Path $g3 'bin\win_x64\plugins\spf-framework.dll')).LastWriteTimeUtc
Set-Content (Join-Path $ktp3 'yerler.txt') 'Depo|1|2|3|0.5'
Start-Sleep -Milliseconds 1200
$rc = Run @('--kur', '--sessiz', '--uygulamasiz', '--oyun', "`"$g3`"")
Check ($rc -eq 0) "cikis kodu 0 ($rc)"
Check ((Enabled $g3) -eq $true) "kapali olan KonvoyTP acilmali"
Check ((Get-Item (Join-Path $ktp3 'KonvoyTP.dll')).VersionInfo.FileVersion -like '1.3.0*') "eski mod 1.3.0 olmali"
Check ((Get-Item (Join-Path $g3 'bin\win_x64\plugins\spf-framework.dll')).LastWriteTimeUtc -eq $spfTime) "guncel SPF'ye dokunulmamali"
Check ((Get-Content (Join-Path $ktp3 'yerler.txt') -Raw).Trim() -eq 'Depo|1|2|3|0.5') "kayitli yerler korunmali"

Write-Host "4) kaldirma: mod silinir, yerler masaustune yedeklenir, uygulama ve kisayollar kalkar"
$rc = Run @('--kaldir', '--sessiz', '--oyun', "`"$g3`"")
Check ($rc -eq 0) "cikis kodu 0 ($rc)"
Check (-not (Test-Path $ktp3)) "mod klasoru silinmeli"
Check ((Enabled $g3) -eq $false) "SPF ayarinda kapali olmali"
Check (Test-Path (Join-Path $g3 'bin\win_x64\plugins\spf-framework.dll')) "SPF yerinde kalmali"
Check ((Get-Content (Join-Path $T 'Desktop\KonvoyTP_yerler_yedek.txt') -Raw).Trim() -eq 'Depo|1|2|3|0.5') "yerler yedegi masaustunde"
Check (-not (Test-Path (Join-Path $T 'Desktop\KonvoyTP.lnk'))) "masaustu kisayolu silinmeli"
Check (-not (Test-Path (Join-Path $T 'StartMenu\KonvoyTP.lnk'))) "Baslat kisayolu silinmeli"
Check (-not (Test-Path (Join-Path $T 'kaldirma_kaydi.txt'))) "kaldirma kaydi silinmeli"
Check (-not (Test-Path (Join-Path $T 'Programs\KonvoyTP\KonvoyTP.exe'))) "kurulu uygulama silinmeli"

Write-Host "5) olmayan oyun klasoru -> hata, hicbir sey yazilmaz"
$rc = Run @('--kur', '--sessiz', '--uygulamasiz', '--oyun', "`"$(Join-Path $T 'yok')`"")
Check ($rc -ne 0) "hata kodu donmeli"

Write-Host ""
Write-Host "$pass gecti, $fail kaldi"
Remove-Item -Recurse -Force $T -ErrorAction SilentlyContinue
if ($fail) { exit 1 }

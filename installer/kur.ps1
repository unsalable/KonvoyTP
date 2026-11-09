# KonvoyTP kurulum / kaldirma betigi (Windows PowerShell 5.1+)
#   kur.ps1              -> kurar (SPF Framework yoksa onu da kurar)
#   kur.ps1 -Kaldir      -> KonvoyTP'yi kaldirir (SPF'ye dokunmaz)
#   kur.ps1 -OyunKlasoru "F:\SteamLibrary\steamapps\common\Euro Truck Simulator 2"
param(
  [string]$OyunKlasoru = "",
  [switch]$Kaldir,
  [switch]$Sessiz
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Say([string]$m, [string]$c = 'Gray') { Write-Host $m -ForegroundColor $c }
function Fail([string]$m) { Say ""; Say "HATA: $m" 'Red'; exit 1 }

Say ""
Say "=== KonvoyTP - ETS2 Konvoy Işınlanma ===" 'Cyan'

# --- 1) Oyun kapali olmali (SPF ayarlarini kapanista yazar) ---
if (Get-Process -Name 'eurotrucks2' -ErrorAction SilentlyContinue) {
  Fail "Euro Truck Simulator 2 açık. Önce oyunu tamamen kapat, sonra bu kurulumu tekrar çalıştır."
}

# --- 2) Oyun klasorunu bul ---
function Find-Ets2 {
  $roots = New-Object System.Collections.Generic.List[string]
  foreach ($k in @('HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 227300',
                   'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 227300')) {
    try {
      $il = (Get-ItemProperty -Path $k -ErrorAction Stop).InstallLocation
      if ($il -and (Test-Path (Join-Path $il 'bin\win_x64\eurotrucks2.exe'))) { return $il }
    } catch {}
  }
  $steam = $null
  try { $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath } catch {}
  if (-not $steam) { try { $steam = (Get-ItemProperty -Path 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam' -ErrorAction Stop).InstallPath } catch {} }
  if ($steam) {
    $steam = $steam -replace '/', '\'
    $roots.Add($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
      foreach ($m in (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' -AllMatches).Matches) {
        $roots.Add(($m.Groups[1].Value -replace '\\\\', '\'))
      }
    }
  }
  foreach ($r in ($roots | Select-Object -Unique)) {
    $p = Join-Path $r 'steamapps\common\Euro Truck Simulator 2'
    if (Test-Path (Join-Path $p 'bin\win_x64\eurotrucks2.exe')) { return $p }
  }
  return $null
}

$game = $OyunKlasoru
if (-not $game) { $game = Find-Ets2 }
if (-not $game -or -not (Test-Path (Join-Path $game 'bin\win_x64\eurotrucks2.exe'))) {
  if ($Sessiz) { Fail "Oyun klasörü bulunamadı." }
  Say "Oyun klasörü otomatik bulunamadı." 'Yellow'
  Say "Steam > Kütüphane > ETS2 > sağ tık > Yönet > Yerel dosyalara göz at ile açılan klasörün yolunu yapıştır:" 'Yellow'
  $game = (Read-Host "Oyun klasörü").Trim('"', ' ')
  if (-not (Test-Path (Join-Path $game 'bin\win_x64\eurotrucks2.exe'))) { Fail "Bu klasörde bin\win_x64\eurotrucks2.exe yok: $game" }
}
Say "Oyun: $game" 'Green'

$plug = Join-Path $game 'bin\win_x64\plugins'
$spfDll = Join-Path $plug 'spf-framework.dll'
$cfgDir = Join-Path $plug 'spfAssets\config'
$cfgFile = Join-Path $cfgDir 'framework_settings.json'
$ktpDir = Join-Path $plug 'spfPlugins\KonvoyTP'

# --- framework_settings.json icinde plugin_states.KonvoyTP.enabled ayari ---
function Set-PluginEnabled([bool]$enabled) {
  $val = if ($enabled) { 'true' } else { 'false' }
  if (-not (Test-Path $cfgFile)) {
    if (-not $enabled) { return }
    New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
    $json = "{`n    `"settings`": {`n        `"plugin_states`": {`n            `"KonvoyTP`": {`n                `"enabled`": true`n            }`n        }`n    }`n}`n"
    [IO.File]::WriteAllText($cfgFile, $json, (New-Object Text.UTF8Encoding $false))
    return
  }
  $text = [IO.File]::ReadAllText($cfgFile)
  $backup = "$cfgFile.konvoytp-yedek"
  if (-not (Test-Path $backup)) { Copy-Item $cfgFile $backup }
  $new = $text
  if ($text -match '"KonvoyTP"\s*:\s*\{\s*"enabled"\s*:') {
    $new = [regex]::Replace($text, '("KonvoyTP"\s*:\s*\{\s*"enabled"\s*:\s*)(true|false)', ('${1}' + $val))
  } elseif ($enabled) {
    if ($text -match '"plugin_states"\s*:\s*\{\s*\}') {
      $new = [regex]::Replace($text, '"plugin_states"\s*:\s*\{\s*\}', '"plugin_states": { "KonvoyTP": { "enabled": true } }', 1)
    } elseif ($text -match '"plugin_states"\s*:\s*\{') {
      $rx = New-Object regex '"plugin_states"\s*:\s*\{'
      $new = $rx.Replace($text, '"plugin_states": {' + "`n            " + '"KonvoyTP": { "enabled": true },', 1)
    } elseif ($text -match '"settings"\s*:\s*\{\s*\}') {
      $new = [regex]::Replace($text, '"settings"\s*:\s*\{\s*\}', '"settings": { "plugin_states": { "KonvoyTP": { "enabled": true } } }', 1)
    } elseif ($text -match '"settings"\s*:\s*\{') {
      $rx = New-Object regex '"settings"\s*:\s*\{'
      $new = $rx.Replace($text, '"settings": {' + "`n        " + '"plugin_states": { "KonvoyTP": { "enabled": true } },', 1)
    } else {
      Fail "framework_settings.json beklenmeyen biçimde. Oyunu bir kez açıp kapat, sonra tekrar dene."
    }
  }
  # dogrula
  try { $null = $new | ConvertFrom-Json } catch {
    Fail "Ayar dosyası güncellenemedi (JSON bozulurdu). Dosyaya dokunulmadı: $cfgFile"
  }
  if ($new -ne $text) { [IO.File]::WriteAllText($cfgFile, $new, (New-Object Text.UTF8Encoding $false)) }
}

# --- Kaldirma ---
if ($Kaldir) {
  if (Test-Path $ktpDir) {
    $keep = Join-Path $ktpDir 'yerler.txt'
    if (Test-Path $keep) { Copy-Item $keep (Join-Path $env:USERPROFILE 'Desktop\KonvoyTP_yerler_yedek.txt') -Force; Say "Kayıtlı yerlerin masaüstüne yedeklendi." }
    Remove-Item -Recurse -Force $ktpDir
  }
  Set-PluginEnabled $false
  Say "KonvoyTP kaldırıldı. (SPF Framework yerinde bırakıldı.)" 'Green'
  exit 0
}

# --- 3) SPF Framework ---
$pkgSpf = Join-Path $here 'SPF'
$needSpf = $true
if (Test-Path $spfDll) {
  try {
    $v = [version]((Get-Item $spfDll).VersionInfo.FileVersion)
    if ($v -ge [version]'1.2.7') { $needSpf = $false }
  } catch { $needSpf = $false }
}
if ($needSpf) {
  if (-not (Test-Path (Join-Path $pkgSpf 'spf-framework.dll'))) { Fail "SPF Framework bulunamadı ve pakette yok. https://github.com/TrackAndTruckDevs/SPF-Framework adresinden kur." }
  New-Item -ItemType Directory -Force -Path $plug | Out-Null
  if (Test-Path $spfDll) { Copy-Item $spfDll "$spfDll.eski" -Force; Say "Eski SPF yedeklendi: spf-framework.dll.eski" }
  Copy-Item (Join-Path $pkgSpf 'spf-framework.dll') $spfDll -Force
  New-Item -ItemType Directory -Force -Path (Join-Path $plug 'spfAssets\localization') | Out-Null
  Copy-Item (Join-Path $pkgSpf 'spfAssets\localization\*') (Join-Path $plug 'spfAssets\localization') -Force
  New-Item -ItemType Directory -Force -Path (Join-Path $plug 'spfAssets\licenses') | Out-Null
  Copy-Item (Join-Path $pkgSpf 'LICENSE-SPF-Framework.txt') (Join-Path $plug 'spfAssets\licenses\SPF-Framework-LICENSE.txt') -Force
  New-Item -ItemType Directory -Force -Path (Join-Path $plug 'spfPlugins') | Out-Null
  Say "SPF Framework 1.2.7 kuruldu." 'Green'
} else {
  Say "SPF Framework zaten kurulu." 'Green'
}

# --- 4) KonvoyTP dosyalari ---
New-Item -ItemType Directory -Force -Path $ktpDir | Out-Null
Copy-Item (Join-Path $here 'KonvoyTP\KonvoyTP.dll') (Join-Path $ktpDir 'KonvoyTP.dll') -Force
if (Test-Path (Join-Path $here 'KonvoyTP\localization')) {
  New-Item -ItemType Directory -Force -Path (Join-Path $ktpDir 'localization') | Out-Null
  Copy-Item (Join-Path $here 'KonvoyTP\localization\*') (Join-Path $ktpDir 'localization') -Force
}
Say "KonvoyTP kopyalandı: $ktpDir" 'Green'

# --- 5) Eklentiyi SPF'de etkinlestir ---
Set-PluginEnabled $true
Say "KonvoyTP SPF ayarlarında etkinleştirildi." 'Green'

Say ""
Say "KURULUM TAMAM!" 'Cyan'
Say " - Oyunu aç, profilini yükle, kamyondayken F8 tuşuna bas." 'White'
Say " - Arkadaşlarının da aynı kurulumu yapması gerekir (konum paylaşımı için)." 'White'
Say " - Oyun açılışında SPF uyarısı/penceresi çıkarsa kapatabilirsin." 'White'
exit 0

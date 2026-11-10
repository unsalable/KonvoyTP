// =====================================================================================
//  KonvoyTP - kurulum, kaldirma ve otomatik guncelleme uygulamasi
//
//  * Tek dosya: mod (KonvoyTP.dll), metinleri ve SPF Framework uygulamanin icinde gomulu (LZMS sikistirma).
//  * Acilista GitHub'daki son surume bakar: yeni surum varsa kendini indirip degistirir, yeniden acilir ve
//    oyundaki modu da gunceller. Oyun aciksa mod, oyun kapaninca guncellenir.
//  * "Kur" modu oyuna kurar ve uygulamayi %LOCALAPPDATA%\Programs\KonvoyTP altina kopyalar (masaustu ve
//    Baslat kisayolu, Windows "Uygulamalar" listesinde kaldirma kaydi).
//  * Arayuz: Direct2D + DirectWrite ile cizilen sade, acik renkli tek pencere.
//  Komut satiri: --kur / --kaldir [--sessiz] [--oyun "<klasor>"] [--uygulamasiz]
// =====================================================================================
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <compressapi.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "../src/ktp_update.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "cabinet.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif
#ifndef SPF_VERSION
#define SPF_VERSION "1.2.7"
#endif

namespace {

using ktpup::Narrow;
using ktpup::Widen;

// =====================================================================================
// Small helpers
// =====================================================================================
std::wstring ExePath() {
  wchar_t b[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH * 2);
  return std::wstring(b, n);
}

std::wstring DirOf(const std::wstring& p) {
  size_t s = p.find_last_of(L"\\/");
  return s == std::wstring::npos ? std::wstring(L".") : p.substr(0, s);
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  std::wstring r;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p)) && p) r = p;
  CoTaskMemFree(p);
  return r;
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

void MakeDirs(const std::wstring& d) { SHCreateDirectoryExW(nullptr, d.c_str(), nullptr); }

// KONVOYTP_TEST_KLASORU (only for automated tests): keeps settings, the installed copy, shortcuts and backups
// inside that folder instead of the user's AppData / desktop.
std::wstring TestRoot() {
  wchar_t b[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"KONVOYTP_TEST_KLASORU", b, MAX_PATH);
  return n && n < MAX_PATH ? std::wstring(b, n) : std::wstring();
}
std::wstring UserFolder(REFKNOWNFOLDERID id, const wchar_t* testName) {
  std::wstring t = TestRoot();
  if (!t.empty()) {
    std::wstring d = t + L"\\" + testName;
    MakeDirs(d);
    return d;
  }
  return KnownFolder(id);
}
std::wstring DataDir() { return UserFolder(FOLDERID_LocalAppData, L"LocalAppData") + L"\\KonvoyTP"; }
std::wstring InstallDir() { return UserFolder(FOLDERID_UserProgramFiles, L"Programs") + L"\\KonvoyTP"; }
std::wstring InstalledExe() { return InstallDir() + L"\\KonvoyTP.exe"; }
std::wstring DesktopDir() { return UserFolder(FOLDERID_Desktop, L"Desktop"); }
std::wstring DesktopLink() { return DesktopDir() + L"\\KonvoyTP.lnk"; }
std::wstring StartMenuLink() { return UserFolder(FOLDERID_Programs, L"StartMenu") + L"\\KonvoyTP.lnk"; }
const wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\KonvoyTP";

std::string Fmt(const char* f, ...) {
  char b[1024];
  va_list ap;
  va_start(ap, f);
  vsnprintf(b, sizeof(b), f, ap);
  va_end(ap);
  return b;
}

void Log(const char* fmt, ...) {
  std::wstring dir = DataDir();
  MakeDirs(dir);
  std::wstring path = dir + L"\\kurulum.log";
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 512 * 1024) MoveFileExW(path.c_str(), (path + L".eski").c_str(), MOVEFILE_REPLACE_EXISTING);
  FILE* f = _wfopen(path.c_str(), L"ab");
  if (!f) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d [%s] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, APP_VERSION);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputs("\r\n", f);
  fclose(f);
}

bool SameFile(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

void DeleteTree(const std::wstring& dir) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      std::wstring n = fd.cFileName;
      if (n == L"." || n == L"..") continue;
      std::wstring p = dir + L"\\" + n;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        DeleteTree(p);
      else {
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(p.c_str());
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  RemoveDirectoryW(dir.c_str());
}

// =====================================================================================
// Embedded payload (built by tools/pack.cpp)
// =====================================================================================
struct PakEntry {
  std::string name;
  uint32_t raw = 0, stored = 0;
  uint8_t method = 0;
  const char* data = nullptr;
};
std::vector<PakEntry> g_pak;

bool LoadPayload() {
  HRSRC r = FindResourceW(nullptr, L"PAYLOAD", RT_RCDATA);
  if (!r) return false;
  HGLOBAL hg = LoadResource(nullptr, r);
  const char* p = hg ? (const char*)LockResource(hg) : nullptr;
  DWORD n = SizeofResource(nullptr, r);
  if (!p || n < 12 || memcmp(p, "KTPPAK1\0", 8) != 0) return false;
  const char* e = p + n;
  uint32_t count = 0;
  memcpy(&count, p + 8, 4);
  p += 12;
  for (uint32_t i = 0; i < count; ++i) {
    if (e - p < 2) return false;
    uint16_t nl = 0;
    memcpy(&nl, p, 2);
    p += 2;
    if (e - p < (ptrdiff_t)nl + 9) return false;
    PakEntry x;
    x.name.assign(p, nl);
    p += nl;
    memcpy(&x.raw, p, 4);
    memcpy(&x.stored, p + 4, 4);
    x.method = (uint8_t)p[8];
    p += 9;
    if (e - p < (ptrdiff_t)x.stored) return false;
    x.data = p;
    p += x.stored;
    g_pak.push_back(x);
  }
  return true;
}

bool PakGet(const std::string& name, std::string& out) {
  for (const PakEntry& x : g_pak) {
    if (x.name != name) continue;
    if (x.method == 0) {
      out.assign(x.data, x.stored);
      return true;
    }
    DECOMPRESSOR_HANDLE d = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &d)) return false;
    out.resize(x.raw);
    SIZE_T got = 0;
    BOOL ok = Decompress(d, x.data, x.stored, x.raw ? &out[0] : nullptr, x.raw, &got);
    CloseDecompressor(d);
    if (!ok || got != x.raw) {
      out.clear();
      return false;
    }
    return true;
  }
  return false;
}

std::vector<std::string> PakNames(const std::string& prefix) {
  std::vector<std::string> v;
  for (const PakEntry& x : g_pak)
    if (x.name.rfind(prefix, 0) == 0) v.push_back(x.name);
  return v;
}

// =====================================================================================
// Game folder, versions, running state
// =====================================================================================
bool IsGameDir(const std::wstring& d) { return !d.empty() && Exists(d + L"\\bin\\win_x64\\eurotrucks2.exe"); }

std::wstring RegStr(HKEY root, const wchar_t* key, const wchar_t* val) {
  wchar_t b[2048];
  DWORD sz = sizeof(b);
  if (RegGetValueW(root, key, val, RRF_RT_REG_SZ, nullptr, b, &sz) == ERROR_SUCCESS) return b;
  return L"";
}

std::wstring NormalizeGameDir(std::wstring d) {
  while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
  for (int i = 0; i < 3 && !d.empty(); ++i) {  // also accept "...\bin" or "...\bin\win_x64"
    if (IsGameDir(d)) return d;
    d = DirOf(d);
  }
  return L"";
}

std::wstring FindGameDir() {
  for (const wchar_t* k : {L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 227300",
                           L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 227300"}) {
    std::wstring il = RegStr(HKEY_LOCAL_MACHINE, k, L"InstallLocation");
    if (IsGameDir(il)) return il;
  }
  std::vector<std::wstring> roots;
  std::wstring steam = RegStr(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
  if (steam.empty()) steam = RegStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
  for (wchar_t& c : steam)
    if (c == L'/') c = L'\\';
  if (!steam.empty()) {
    roots.push_back(steam);
    std::string vdf;
    if (ktpup::ReadFileAll(steam + L"\\steamapps\\libraryfolders.vdf", vdf)) {
      std::regex rx("\"path\"\\s+\"([^\"]+)\"");
      for (std::sregex_iterator it(vdf.begin(), vdf.end(), rx), end; it != end; ++it) {
        std::string p = (*it)[1].str(), q;
        for (size_t i = 0; i < p.size(); ++i) {
          if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') ++i;
          q += p[i];
        }
        roots.push_back(Widen(q));
      }
    }
  }
  for (const std::wstring& r : roots) {
    std::wstring p = r + L"\\steamapps\\common\\Euro Truck Simulator 2";
    if (IsGameDir(p)) return p;
  }
  return L"";
}

bool IsGameRunning() {
  HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (s == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W pe;
  pe.dwSize = sizeof(pe);
  bool found = false;
  if (Process32FirstW(s, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, L"eurotrucks2.exe") == 0) {
        found = true;
        break;
      }
    } while (Process32NextW(s, &pe));
  }
  CloseHandle(s);
  return found;
}

std::string FileVersionOf(const std::wstring& path) {
  DWORD h = 0, n = GetFileVersionInfoSizeW(path.c_str(), &h);
  if (!n) return "";
  std::vector<char> buf(n);
  if (!GetFileVersionInfoW(path.c_str(), 0, n, buf.data())) return "";
  VS_FIXEDFILEINFO* fi = nullptr;
  UINT len = 0;
  if (!VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) || !fi) return "";
  return Fmt("%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS));
}

struct GamePaths {
  std::wstring plugins, spfDll, cfgDir, cfgFile, ktpDir, ktpDll;
};

GamePaths PathsOf(const std::wstring& game) {
  GamePaths p;
  p.plugins = game + L"\\bin\\win_x64\\plugins";
  p.spfDll = p.plugins + L"\\spf-framework.dll";
  p.cfgDir = p.plugins + L"\\spfAssets\\config";
  p.cfgFile = p.cfgDir + L"\\framework_settings.json";
  p.ktpDir = p.plugins + L"\\spfPlugins\\KonvoyTP";
  p.ktpDll = p.ktpDir + L"\\KonvoyTP.dll";
  return p;
}

std::wstring LoadSavedGameDir() {
  std::string d;
  if (!ktpup::ReadFileAll(DataDir() + L"\\ayarlar.txt", d)) return L"";
  size_t i = d.find("oyun=");
  if (i == std::string::npos) return L"";
  size_t e = d.find_first_of("\r\n", i);
  return Widen(d.substr(i + 5, e == std::string::npos ? std::string::npos : e - i - 5));
}

void SaveGameDir(const std::wstring& dir) {
  MakeDirs(DataDir());
  ktpup::WriteFileAll(DataDir() + L"\\ayarlar.txt", "oyun=" + Narrow(dir) + "\r\n");
}

// =====================================================================================
// Install / uninstall (same steps as the old KUR.bat, plus the app itself)
// =====================================================================================
bool g_needAdmin = false;  // set when the game folder is not writable for this user

bool WriteGameFile(const std::wstring& path, const std::string& data, std::string& err) {
  if (ktpup::WriteFileAll(path, data)) return true;
  DWORD e = GetLastError();
  if (e == ERROR_ACCESS_DENIED) g_needAdmin = true;
  err = e == ERROR_ACCESS_DENIED ? "oyun klasörüne yazma izni yok" : Fmt("dosya yazılamadı (hata %lu)", e);
  Log("yazilamadi: %s (%lu)", Narrow(path).c_str(), e);
  return false;
}

// SPF keeps "settings.plugin_states.<Plugin>.enabled" in framework_settings.json; new plugins start disabled.
int PluginState(const ktpup::JVal& root) {
  const ktpup::JVal* s = root.Get("settings");
  const ktpup::JVal* ps = s ? s->Get("plugin_states") : nullptr;
  const ktpup::JVal* k = ps ? ps->Get("KonvoyTP") : nullptr;
  const ktpup::JVal* e = k ? k->Get("enabled") : nullptr;
  return e && e->k == ktpup::JVal::BOOL ? (e->b ? 1 : 0) : -1;
}

bool SetPluginEnabled(const GamePaths& p, bool enabled, std::string& err) {
  if (!Exists(p.cfgFile)) {
    if (!enabled) return true;
    MakeDirs(p.cfgDir);
    return WriteGameFile(p.cfgFile,
                         "{\n    \"settings\": {\n        \"plugin_states\": {\n            \"KonvoyTP\": {\n                \"enabled\": true\n            }\n        }\n    }\n}\n",
                         err);
  }
  std::string text;
  if (!ktpup::ReadFileAll(p.cfgFile, text)) {
    err = "SPF ayar dosyası okunamadı";
    return false;
  }
  ktpup::JVal root;
  if (!ktpup::ParseJson(text, root)) {
    err = "SPF ayar dosyası bozuk; dokunulmadı";
    return false;
  }
  int want = enabled ? 1 : 0;
  if (PluginState(root) == want || (!enabled && PluginState(root) < 0)) return true;
  using std::regex;
  const auto first = std::regex_constants::format_first_only;
  std::string out;
  regex rxKey("(\"KonvoyTP\"\\s*:\\s*\\{\\s*\"enabled\"\\s*:\\s*)(true|false)");
  if (std::regex_search(text, rxKey)) {
    out = std::regex_replace(text, rxKey, std::string("$1") + (enabled ? "true" : "false"), first);
  } else {
    regex a("\"plugin_states\"\\s*:\\s*\\{\\s*\\}"), b("\"plugin_states\"\\s*:\\s*\\{"), c("\"settings\"\\s*:\\s*\\{\\s*\\}"), d("\"settings\"\\s*:\\s*\\{");
    if (std::regex_search(text, a))
      out = std::regex_replace(text, a, "\"plugin_states\": { \"KonvoyTP\": { \"enabled\": true } }", first);
    else if (std::regex_search(text, b))
      out = std::regex_replace(text, b, "\"plugin_states\": {\n            \"KonvoyTP\": { \"enabled\": true },", first);
    else if (std::regex_search(text, c))
      out = std::regex_replace(text, c, "\"settings\": { \"plugin_states\": { \"KonvoyTP\": { \"enabled\": true } } }", first);
    else if (std::regex_search(text, d))
      out = std::regex_replace(text, d, "\"settings\": {\n        \"plugin_states\": { \"KonvoyTP\": { \"enabled\": true } },", first);
  }
  ktpup::JVal check;
  if (out.empty() || !ktpup::ParseJson(out, check) || PluginState(check) != want) {
    err = "SPF ayar dosyası beklenmeyen biçimde; dokunulmadı";
    return false;
  }
  std::wstring bak = p.cfgFile + L".konvoytp-yedek";
  if (!Exists(bak)) CopyFileW(p.cfgFile.c_str(), bak.c_str(), TRUE);
  return WriteGameFile(p.cfgFile, out, err);
}

typedef std::function<void(const std::wstring&)> StepFn;

bool InstallMod(const std::wstring& game, std::string& err, const StepFn& step) {
  g_needAdmin = false;
  if (!IsGameDir(game)) {
    err = "oyun klasörü bulunamadı";
    return false;
  }
  if (IsGameRunning()) {
    err = "oyun açık; önce oyunu kapat";
    return false;
  }
  GamePaths p = PathsOf(game);
  MakeDirs(p.plugins);
  std::string have = Exists(p.spfDll) ? FileVersionOf(p.spfDll) : "";
  if (have.empty() || ktpup::CompareVersions(have, SPF_VERSION) < 0) {
    step(L"SPF Framework kuruluyor…");
    std::string dll;
    if (!PakGet("SPF/spf-framework.dll", dll)) {
      err = "SPF Framework paketin içinde yok";
      return false;
    }
    if (Exists(p.spfDll)) CopyFileW(p.spfDll.c_str(), (p.spfDll + L".eski").c_str(), FALSE);
    if (!WriteGameFile(p.spfDll, dll, err)) return false;
    MakeDirs(p.plugins + L"\\spfAssets\\localization");
    for (const std::string& n : PakNames("SPF/spfAssets/localization/")) {
      std::string d;
      if (PakGet(n, d) && !WriteGameFile(p.plugins + L"\\spfAssets\\localization\\" + Widen(n.substr(n.rfind('/') + 1)), d, err)) return false;
    }
    std::string lic;
    if (PakGet("SPF/LICENSE-SPF-Framework.txt", lic)) {
      MakeDirs(p.plugins + L"\\spfAssets\\licenses");
      WriteGameFile(p.plugins + L"\\spfAssets\\licenses\\SPF-Framework-LICENSE.txt", lic, err);
    }
    Log("SPF %s kuruldu (onceki: %s)", SPF_VERSION, have.empty() ? "-" : have.c_str());
  }
  MakeDirs(p.plugins + L"\\spfPlugins");
  step(L"KonvoyTP kopyalanıyor…");
  MakeDirs(p.ktpDir + L"\\localization");
  std::string dll;
  if (!PakGet("KonvoyTP/KonvoyTP.dll", dll)) {
    err = "KonvoyTP.dll paketin içinde yok";
    return false;
  }
  DeleteFileW((p.ktpDll + L".eski").c_str());
  DeleteFileW((p.ktpDll + L".yeni").c_str());
  if (!WriteGameFile(p.ktpDll, dll, err)) return false;
  for (const char* lang : {"tr", "en"}) {
    std::string d;
    if (PakGet(std::string("KonvoyTP/localization/") + lang + ".json", d) && !WriteGameFile(p.ktpDir + L"\\localization\\" + Widen(lang) + L".json", d, err)) return false;
  }
  step(L"SPF'de etkinleştiriliyor…");
  if (!SetPluginEnabled(p, true, err)) return false;
  Log("KonvoyTP %s kuruldu: %s", APP_VERSION, Narrow(game).c_str());
  return true;
}

bool UninstallMod(const std::wstring& game, std::string& err, bool& placesSaved) {
  placesSaved = false;
  if (IsGameRunning()) {
    err = "oyun açık; önce oyunu kapat";
    return false;
  }
  if (!IsGameDir(game)) return true;
  GamePaths p = PathsOf(game);
  if (Exists(p.ktpDir)) {
    std::wstring places = p.ktpDir + L"\\yerler.txt";
    if (Exists(places)) placesSaved = CopyFileW(places.c_str(), (DesktopDir() + L"\\KonvoyTP_yerler_yedek.txt").c_str(), FALSE) != 0;
    DeleteTree(p.ktpDir);
    if (Exists(p.ktpDll)) {
      err = "mod dosyaları silinemedi";
      return false;
    }
  }
  std::string e2;
  SetPluginEnabled(p, false, e2);
  Log("KonvoyTP kaldirildi: %s", Narrow(game).c_str());
  return true;
}

// ---- the app itself: copy to %LOCALAPPDATA%\Programs\KonvoyTP, shortcuts, Windows uninstall entry ----
bool MakeShortcut(const std::wstring& lnk, const std::wstring& target) {
  IShellLinkW* sl = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
  sl->SetPath(target.c_str());
  sl->SetWorkingDirectory(DirOf(target).c_str());
  sl->SetDescription(L"KonvoyTP: ETS2 konvoy ışınlanma modunu kur, güncelle, kaldır");
  sl->SetIconLocation(target.c_str(), 0);
  IPersistFile* pf = nullptr;
  HRESULT hr = sl->QueryInterface(IID_PPV_ARGS(&pf));
  if (SUCCEEDED(hr)) {
    hr = pf->Save(lnk.c_str(), TRUE);
    pf->Release();
  }
  sl->Release();
  return SUCCEEDED(hr);
}

void RegisterUninstall(const std::wstring& exe) {
  if (!TestRoot().empty()) {  // tests: leave the registry alone, just show that it would be registered
    ktpup::WriteFileAll(TestRoot() + L"\\kaldirma_kaydi.txt", Narrow(L"\"" + exe + L"\" --kaldir"));
    return;
  }
  HKEY k = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
  auto setS = [&](const wchar_t* n, const std::wstring& v) { RegSetValueExW(k, n, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t))); };
  auto setD = [&](const wchar_t* n, DWORD v) { RegSetValueExW(k, n, 0, REG_DWORD, (const BYTE*)&v, sizeof(v)); };
  setS(L"DisplayName", L"KonvoyTP (ETS2 konvoy modu)");
  setS(L"DisplayVersion", Widen(APP_VERSION));
  setS(L"Publisher", L"KonvoyTP");
  setS(L"DisplayIcon", exe);
  setS(L"UninstallString", L"\"" + exe + L"\" --kaldir");
  setS(L"InstallLocation", DirOf(exe));
  setS(L"URLInfoAbout", L"https://github.com/unsalable/KonvoyTP");
  setD(L"NoModify", 1);
  setD(L"NoRepair", 1);
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &fa)) setD(L"EstimatedSize", fa.nFileSizeLow / 1024);
  RegCloseKey(k);
}

bool RunningInstalled() { return SameFile(ExePath(), InstalledExe()); }

void InstallAppSelf() {
  std::wstring dst = InstalledExe();
  if (!RunningInstalled()) {
    MakeDirs(InstallDir());
    DeleteFileW((dst + L".eski").c_str());
    if (!CopyFileW(ExePath().c_str(), dst.c_str(), FALSE)) {
      MoveFileExW(dst.c_str(), (dst + L".eski").c_str(), MOVEFILE_REPLACE_EXISTING);  // an old copy may be running
      CopyFileW(ExePath().c_str(), dst.c_str(), FALSE);
    }
  }
  if (Exists(dst)) {
    MakeShortcut(DesktopLink(), dst);
    MakeShortcut(StartMenuLink(), dst);
    RegisterUninstall(dst);
  }
}

bool g_removeSelfOnExit = false;

void RemoveAppSelf() {
  DeleteFileW(DesktopLink().c_str());
  DeleteFileW(StartMenuLink().c_str());
  if (TestRoot().empty())
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
  else
    DeleteFileW((TestRoot() + L"\\kaldirma_kaydi.txt").c_str());
  if (RunningInstalled())
    g_removeSelfOnExit = true;
  else
    DeleteTree(InstallDir());
}

void RemoveSelfNow() {  // our own folder can only go once this process has exited
  std::wstring cmd = L"cmd.exe /c ping 127.0.0.1 -n 3 >nul & rmdir /s /q \"" + InstallDir() + L"\" & rmdir /s /q \"" + DataDir() + L"\"";
  STARTUPINFOW si;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi;
  if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, KnownFolder(FOLDERID_LocalAppData).c_str(), &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

// ---- self update ----
bool ReplaceSelf(const std::string& data, std::string& err) {
  std::wstring exe = ExePath(), fresh = exe + L".yeni", old = exe + L".eski";
  if (!ktpup::WriteFileAll(fresh, data)) {
    err = "yeni sürüm kaydedilemedi";
    return false;
  }
  DeleteFileW(old.c_str());
  if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(fresh.c_str());
    err = "uygulama dosyası değiştirilemedi";
    return false;
  }
  if (!MoveFileExW(fresh.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
    DeleteFileW(fresh.c_str());
    err = "uygulama dosyası değiştirilemedi";
    return false;
  }
  return true;
}

bool Launch(const std::wstring& exe, const std::wstring& args) {
  std::wstring cmd = L"\"" + exe + L"\" " + args;
  STARTUPINFOW si;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi;
  if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, DirOf(exe).c_str(), &si, &pi)) return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

void CleanupOldSelf() { DeleteFileW((ExePath() + L".eski").c_str()); }

// =====================================================================================
// Model shared between the UI and the worker threads
// =====================================================================================
enum Kind { K_NEUTRAL = 0, K_OK = 1, K_WARN = 2, K_ERR = 3 };

struct Model {
  std::wstring gameDir;
  bool gameFound = false;
  bool gameRunning = false;
  std::string modVer;  // "" = not installed
  std::string spfVer;  // "" = not installed
  std::wstring upd = L"Kontrol ediliyor…";
  int updKind = K_NEUTRAL;
  bool busy = false;
  std::wstring busyText;
  double progress = -1;
  std::wstring msg;
  int msgKind = K_NEUTRAL;
  bool pendingModUpdate = false;
  bool restartPending = false;
};

Model g_m;
std::mutex g_mx;
HWND g_wnd = nullptr;
bool g_appJustUpdated = false;  // started with --guncellendi (the previous version replaced itself with this one)
volatile LONG g_cancel = 0;
const UINT WM_APP_REFRESH = WM_APP + 1;
const UINT WM_APP_RESTART = WM_APP + 2;

Model Snapshot() {
  std::lock_guard<std::mutex> lk(g_mx);
  return g_m;
}

void Mutate(const std::function<void(Model&)>& f) {
  {
    std::lock_guard<std::mutex> lk(g_mx);
    f(g_m);
  }
  if (g_wnd) PostMessageW(g_wnd, WM_APP_REFRESH, 0, 0);
}

void RefreshGame() {
  std::wstring dir = Snapshot().gameDir;
  bool found = IsGameDir(dir);
  std::string mv, sv;
  if (found) {
    GamePaths p = PathsOf(dir);
    if (Exists(p.ktpDll)) mv = FileVersionOf(p.ktpDll);
    if (Exists(p.ktpDll) && mv.empty()) mv = "?";
    if (Exists(p.spfDll)) sv = FileVersionOf(p.spfDll);
    if (Exists(p.spfDll) && sv.empty()) sv = "?";
  }
  bool running = IsGameRunning();
  Mutate([&](Model& m) {
    m.gameFound = found;
    m.modVer = mv;
    m.spfVer = sv;
    m.gameRunning = running;
  });
}

void RunAsync(std::function<void()> f) {
  std::thread([f]() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    f();
    CoUninitialize();
  }).detach();
}

std::wstring W(const std::string& s) { return Widen(s); }

// The app carries the newest mod: bring an older installed copy up to date by itself.
void AutoModUpdate() {
  Model m = Snapshot();
  if (m.busy || !m.gameFound || m.modVer.empty() || m.modVer == "?" || ktpup::CompareVersions(m.modVer, APP_VERSION) >= 0) return;
  if (IsGameRunning()) {
    Mutate([](Model& x) {
      x.pendingModUpdate = true;
      x.msg = L"Mod, oyun kapanınca kendiliğinden güncellenecek.";
      x.msgKind = K_NEUTRAL;
    });
    return;
  }
  Mutate([](Model& x) {
    x.busy = true;
    x.busyText = L"Mod güncelleniyor…";
    x.pendingModUpdate = false;
  });
  std::string err;
  bool ok = InstallMod(m.gameDir, err, [](const std::wstring& s) { Mutate([&](Model& x) { x.busyText = s; }); });
  Mutate([&](Model& x) {
    x.busy = false;
    x.msg = ok ? (g_appJustUpdated ? L"KonvoyTP ve mod " : L"Mod ") + W(APP_VERSION) + L" sürümüne güncellendi." : L"Mod güncellenemedi: " + W(err);
    x.msgKind = ok ? K_OK : K_ERR;
  });
  RefreshGame();
}

void OnProgress(uint64_t got, uint64_t total, void*) {
  static DWORD last = 0;
  DWORD now = GetTickCount();
  if (total && got < total && now - last < 120) return;
  last = now;
  Mutate([&](Model& m) {
    m.progress = total ? (double)got / (double)total : -1;
    int pct = total ? (int)(100.0 * (double)got / (double)total) : 0;
    m.upd = L"Yeni sürüm indiriliyor… %" + std::to_wstring(pct);
  });
}

void UpdateWorker() {
  ktpup::Release rel;
  std::string err;
  if (!ktpup::FetchLatest(rel, err, &g_cancel)) {
    Log("guncelleme kontrolu: %s", err.c_str());
    Mutate([&](Model& m) {
      m.upd = L"Kontrol edilemedi (" + W(err) + L")";
      m.updKind = K_WARN;
    });
    AutoModUpdate();
    return;
  }
  std::string latest = ktpup::CleanVersion(rel.tag);
  if (ktpup::CompareVersions(latest, APP_VERSION) > 0) {
    const ktpup::Asset* a = rel.Find(ktpup::kAssetApp);
    if (a) {
      Log("yeni surum %s indiriliyor", latest.c_str());
      Mutate([&](Model& m) {
        m.upd = L"Yeni sürüm " + W(latest) + L" indiriliyor…";
        m.progress = 0;
      });
      std::string data;
      if (ktpup::DownloadAsset(*a, data, err, &g_cancel, OnProgress, nullptr) && ktpup::LooksLikePe64(data) && ReplaceSelf(data, err)) {
        Log("uygulama %s surumune guncellendi, yeniden aciliyor", latest.c_str());
        Mutate([&](Model& m) {
          m.upd = L"Güncellendi, yeniden açılıyor…";
          m.progress = -1;
          m.restartPending = true;
        });
        if (g_wnd) PostMessageW(g_wnd, WM_APP_RESTART, 0, 0);
        return;
      }
      Log("guncellenemedi: %s", err.c_str());
      Mutate([&](Model& m) {
        m.upd = L"Güncellenemedi (" + W(err) + L")";
        m.updKind = K_ERR;
        m.progress = -1;
      });
    } else {
      Mutate([&](Model& m) {
        m.upd = W(latest) + L" çıktı";
        m.updKind = K_WARN;
      });
    }
  } else {
    Mutate([&](Model& m) {
      m.upd = L"Güncel";
      m.updKind = K_OK;
    });
  }
  AutoModUpdate();
}

// =====================================================================================
// UI: Direct2D + DirectWrite
// =====================================================================================
namespace C {
const uint32_t Bg = 0xF5F5F7, Card = 0xFFFFFF, Line = 0xE3E3E8, Sep = 0xECECF0, Text = 0x1D1D1F, Text2 = 0x6E6E73, Text3 = 0xA1A1A6;
const uint32_t Accent = 0xFF9F0A, AccentHov = 0xFFAD33, AccentPress = 0xE88C00, OnAccent = 0x1C1404;
const uint32_t Btn2 = 0xE8E8ED, Btn2Hov = 0xDEDEE4, Btn2Press = 0xD3D3DA, Green = 0x34C759, Red = 0xE5342A, Orange = 0xFF9500, Blue = 0x0071E3;
}  // namespace C

const float kWinW = 480.0f, kWinH = 620.0f;

ID2D1Factory* g_d2 = nullptr;
IDWriteFactory* g_dw = nullptr;
ID2D1HwndRenderTarget* g_rt = nullptr;
ID2D1SolidColorBrush* g_br = nullptr;
ID2D1PathGeometry* g_cab = nullptr;
ID2D1PathGeometry* g_cabWin = nullptr;
IDWriteTextFormat *g_fTitle = nullptr, *g_fSub = nullptr, *g_fBody = nullptr, *g_fBodyB = nullptr, *g_fSmall = nullptr, *g_fBtn = nullptr, *g_fH1 = nullptr;
UINT g_dpi = 96;
int g_page = 0;  // 0 main, 1 "Nasıl çalışır?"
int g_hover = 0, g_press = 0;
bool g_tracking = false;

enum Id { ID_NONE = 0, ID_PRIMARY, ID_INFO, ID_REINSTALL, ID_UNINSTALL, ID_GAMEROW, ID_BACK, ID_LINK };
struct Hit {
  int id;
  D2D1_RECT_F r;
};
std::vector<Hit> g_hits;

D2D1_COLOR_F Col(uint32_t rgb, float a = 1.0f) { return D2D1::ColorF(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a); }

template <class T>
void SafeRelease(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

bool FontExists(const wchar_t* fam) {
  IDWriteFontCollection* fc = nullptr;
  UINT32 i = 0;
  BOOL ex = FALSE;
  if (SUCCEEDED(g_dw->GetSystemFontCollection(&fc)) && fc) {
    fc->FindFamilyName(fam, &i, &ex);
    fc->Release();
  }
  return ex != FALSE;
}

IDWriteTextFormat* MakeFormat(const wchar_t* fam, float size, DWRITE_FONT_WEIGHT w) {
  IDWriteTextFormat* f = nullptr;
  g_dw->CreateTextFormat(fam, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"tr-TR", &f);
  return f;
}

void CreateDeviceIndependent() {
  D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2);
  DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_dw));
  const wchar_t* text = FontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
  const wchar_t* disp = FontExists(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
  g_fTitle = MakeFormat(disp, 27.0f, DWRITE_FONT_WEIGHT_BOLD);
  g_fH1 = MakeFormat(disp, 24.0f, DWRITE_FONT_WEIGHT_BOLD);
  g_fSub = MakeFormat(text, 14.0f, DWRITE_FONT_WEIGHT_NORMAL);
  g_fBody = MakeFormat(text, 14.0f, DWRITE_FONT_WEIGHT_NORMAL);
  g_fBodyB = MakeFormat(text, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  g_fSmall = MakeFormat(text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL);
  g_fBtn = MakeFormat(text, 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
  // truck cab + window of the app icon, in a 256x256 design space
  auto poly = [](ID2D1PathGeometry** g, const D2D1_POINT_2F* pts, int n) {
    g_d2->CreatePathGeometry(g);
    ID2D1GeometrySink* s = nullptr;
    (*g)->Open(&s);
    s->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
    for (int i = 1; i < n; ++i) s->AddLine(pts[i]);
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    s->Release();
  };
  const D2D1_POINT_2F cab[] = {{158, 96}, {196, 96}, {222, 126}, {222, 156}, {158, 156}};
  const D2D1_POINT_2F win[] = {{168, 106}, {191, 106}, {209, 127}, {168, 127}};
  poly(&g_cab, cab, 5);
  poly(&g_cabWin, win, 4);
}

void DiscardTarget() {
  SafeRelease(g_br);
  SafeRelease(g_rt);
}

bool EnsureTarget() {
  if (g_rt) return true;
  RECT rc;
  GetClientRect(g_wnd, &rc);
  D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), (float)g_dpi, (float)g_dpi);
  if (FAILED(g_d2->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(g_wnd, D2D1::SizeU((UINT32)rc.right, (UINT32)rc.bottom)), &g_rt))) return false;
  g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
  g_rt->CreateSolidColorBrush(Col(0), &g_br);
  return true;
}

float Px() { return 96.0f / (float)g_dpi; }  // one physical pixel in DIPs

void Fill(const D2D1_RECT_F& r, uint32_t c, float a = 1.0f) {
  g_br->SetColor(Col(c, a));
  g_rt->FillRectangle(r, g_br);
}

void FillRound(const D2D1_RECT_F& r, float rad, uint32_t c, float a = 1.0f) {
  g_br->SetColor(Col(c, a));
  g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), g_br);
}

void StrokeRound(const D2D1_RECT_F& r, float rad, uint32_t c, float a = 1.0f) {
  float h = Px() * 0.5f;
  g_br->SetColor(Col(c, a));
  g_rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + h, r.top + h, r.right - h, r.bottom - h), rad, rad), g_br, Px());
}

void Dot(float x, float y, float rad, uint32_t c, float a = 1.0f) {
  g_br->SetColor(Col(c, a));
  g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), rad, rad), g_br);
}

enum Align { A_LEFT, A_CENTER, A_RIGHT };

IDWriteTextLayout* Layout(const std::wstring& s, IDWriteTextFormat* f, float w, float h, Align al, bool wrap, bool ellipsis, wchar_t delim) {
  IDWriteTextLayout* L = nullptr;
  if (FAILED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, w, h, &L))) return nullptr;
  L->SetTextAlignment(al == A_LEFT ? DWRITE_TEXT_ALIGNMENT_LEADING : al == A_CENTER ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING);
  L->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
  if (ellipsis) {
    DWRITE_TRIMMING t = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, (UINT32)delim, delim ? 1u : 0u};
    IDWriteInlineObject* sign = nullptr;
    g_dw->CreateEllipsisTrimmingSign(f, &sign);
    L->SetTrimming(&t, sign);
    SafeRelease(sign);
  }
  return L;
}

// draws text in r (vertically centered unless wrap) and returns its height
float Text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, uint32_t c, Align al = A_LEFT, bool wrap = false, float a = 1.0f, wchar_t delim = 0) {
  IDWriteTextLayout* L = Layout(s, f, r.right - r.left, r.bottom - r.top, al, wrap, !wrap, delim);
  if (!L) return 0;
  DWRITE_TEXT_METRICS m;
  L->GetMetrics(&m);
  float y = wrap ? r.top : r.top + (r.bottom - r.top - m.height) * 0.5f;
  g_br->SetColor(Col(c, a));
  g_rt->DrawTextLayout(D2D1::Point2F(r.left, y), L, g_br);
  L->Release();
  return m.height;
}

float TextHeight(const std::wstring& s, IDWriteTextFormat* f, float w) {
  IDWriteTextLayout* L = Layout(s, f, w, 1000.0f, A_LEFT, true, false, 0);
  if (!L) return 0;
  DWRITE_TEXT_METRICS m;
  L->GetMetrics(&m);
  L->Release();
  return m.height;
}

void AddHit(int id, const D2D1_RECT_F& r) { g_hits.push_back({id, r}); }

enum BtnKind { B_PRIMARY, B_SECONDARY, B_DANGER };

void Button(int id, const D2D1_RECT_F& r, const std::wstring& label, BtnKind kind, bool enabled) {
  bool hov = enabled && g_hover == id, prs = hov && g_press == id;
  uint32_t bg, fg;
  float bgA = 1.0f, fgA = 1.0f;
  if (kind == B_PRIMARY) {
    bg = prs ? C::AccentPress : hov ? C::AccentHov : C::Accent;
    fg = C::OnAccent;
    if (!enabled) {
      bgA = 0.42f;
      fgA = 0.6f;
    }
  } else {
    bg = prs ? C::Btn2Press : hov ? C::Btn2Hov : C::Btn2;
    fg = kind == B_DANGER ? C::Red : C::Text;
    if (!enabled) fgA = 0.4f;
  }
  FillRound(r, 12.0f, bg, bgA);
  Text(label, kind == B_PRIMARY ? g_fBtn : g_fBodyB, D2D1::RectF(r.left + 10, r.top, r.right - 10, r.bottom), fg, A_CENTER, false, fgA);
  if (enabled) AddHit(id, r);
}

void DrawAppIcon(float x, float y, float size) {
  float k = size / 256.0f;
  D2D1_MATRIX_3X2_F old;
  g_rt->GetTransform(&old);
  g_rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(x, y) * old);
  ID2D1GradientStopCollection* stops = nullptr;
  D2D1_GRADIENT_STOP st[2] = {{0.0f, Col(0xFFB84D)}, {1.0f, Col(0xFF8C00)}};
  ID2D1LinearGradientBrush* grad = nullptr;
  if (SUCCEEDED(g_rt->CreateGradientStopCollection(st, 2, &stops)) &&
      SUCCEEDED(g_rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 8), D2D1::Point2F(0, 248)), stops, &grad))) {
    g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(8, 8, 248, 248), 58, 58), grad);
  }
  SafeRelease(grad);
  SafeRelease(stops);
  g_br->SetColor(Col(0xFFFFFF));
  g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(36, 76, 152, 156), 10, 10), g_br);
  g_rt->FillGeometry(g_cab, g_br);
  g_rt->FillRectangle(D2D1::RectF(36, 150, 222, 162), g_br);
  for (float cx : {72.0f, 120.0f, 194.0f}) g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, 175), 19, 19), g_br);
  g_br->SetColor(Col(0xFF9F0A));
  g_rt->FillGeometry(g_cabWin, g_br);
  for (float cx : {72.0f, 120.0f, 194.0f}) g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, 175), 7, 7), g_br);
  g_rt->SetTransform(old);
}

struct Primary {
  std::wstring label;
  bool enabled;
  int action;  // 0 none, 1 pick folder, 2 install/update, 3 launch game
};

bool ModCurrent(const Model& m) { return !m.modVer.empty() && m.modVer != "?" && ktpup::CompareVersions(m.modVer, APP_VERSION) >= 0; }

Primary PrimaryFor(const Model& m) {
  if (m.busy) return {m.busyText.empty() ? L"Çalışıyor…" : m.busyText, false, 0};
  if (!m.gameFound) return {L"Oyun klasörünü seç", true, 1};
  if (m.gameRunning) return {ModCurrent(m) ? L"Oyun açık · iyi yolculuklar" : L"Kurmak için önce oyunu kapat", false, 0};
  if (m.modVer.empty()) return {L"Kur", true, 2};
  if (!ModCurrent(m)) return {L"Güncelle (" + W(APP_VERSION) + L")", true, 2};
  return {L"Oyunu aç", true, 3};
}

uint32_t KindColor(int k) { return k == K_OK ? C::Green : k == K_WARN ? C::Orange : k == K_ERR ? C::Red : C::Text2; }

void PaintMain(const Model& m) {
  const float L = 24.0f, R = kWinW - 24.0f;
  DrawAppIcon(L + 2, 28, 58);
  Text(L"KonvoyTP", g_fTitle, D2D1::RectF(L + 76, 28, R, 62), C::Text);
  Text(L"ETS2 konvoy ışınlanma modu", g_fSub, D2D1::RectF(L + 77, 62, R, 84), C::Text2);

  // status card
  float y = 112.0f;
  const float rowH = 52.0f;
  D2D1_RECT_F card = D2D1::RectF(L, y, R, y + rowH * 4);
  FillRound(card, 14.0f, C::Card);
  StrokeRound(card, 14.0f, C::Line);
  struct Row {
    const wchar_t* label;
    std::wstring value;
    uint32_t dot;
    uint32_t valCol;
  } rows[4];
  rows[0] = {L"Oyun", m.gameFound ? m.gameDir : L"Bulunamadı · tıkla, klasörü seç", m.gameFound ? (m.gameRunning ? C::Orange : C::Green) : C::Red,
             m.gameFound ? C::Text2 : C::Red};
  if (m.gameFound && m.gameRunning) rows[0].value = L"Açık · " + m.gameDir;
  if (m.modVer.empty())
    rows[1] = {L"Mod", L"Kurulu değil", C::Text3, C::Text2};
  else if (!ModCurrent(m))
    rows[1] = {L"Mod", L"Eski sürüm · " + W(m.modVer), C::Orange, C::Text2};
  else
    rows[1] = {L"Mod", L"Kurulu · " + W(m.modVer), C::Green, C::Text2};
  if (m.spfVer.empty())
    rows[2] = {L"SPF Framework", L"Modla birlikte kurulur", C::Text3, C::Text2};
  else
    rows[2] = {L"SPF Framework", W(m.spfVer), ktpup::CompareVersions(m.spfVer, SPF_VERSION) >= 0 ? C::Green : C::Orange, C::Text2};
  rows[3] = {L"Güncelleme", m.upd, KindColor(m.updKind) == C::Text2 ? C::Text3 : KindColor(m.updKind), m.updKind == K_ERR ? C::Red : C::Text2};
  for (int i = 0; i < 4; ++i) {
    float top = y + rowH * i;
    if (i > 0) Fill(D2D1::RectF(L + 40, top, R, top + Px()), C::Sep);
    bool hov = i == 0 && g_hover == ID_GAMEROW;
    Dot(L + 22, top + rowH * 0.5f, 4.5f, rows[i].dot);
    Text(rows[i].label, g_fBodyB, D2D1::RectF(L + 38, top, L + 170, top + rowH), C::Text);
    Text(rows[i].value, g_fBody, D2D1::RectF(L + 150, top, R - 16, top + rowH), hov ? C::Blue : rows[i].valCol, A_RIGHT, false, 1.0f, i == 0 ? L'\\' : 0);
    if (i == 0) AddHit(ID_GAMEROW, D2D1::RectF(L, top, R, top + rowH));
  }
  y = card.bottom + 14.0f;

  // progress (downloads / installs)
  if (m.progress >= 0.0) {
    FillRound(D2D1::RectF(L, y, R, y + 4), 2, C::Line);
    FillRound(D2D1::RectF(L, y, L + (R - L) * (float)std::min(1.0, m.progress), y + 4), 2, C::Accent);
  }
  y += 12.0f;

  Primary pr = PrimaryFor(m);
  Button(ID_PRIMARY, D2D1::RectF(L, y, R, y + 52), pr.label, B_PRIMARY, pr.enabled);
  y += 64.0f;

  bool installed = !m.modVer.empty();
  bool can = !m.busy && !m.gameRunning && m.gameFound;
  if (installed) {
    float w = (R - L - 16.0f) / 3.0f;
    Button(ID_INFO, D2D1::RectF(L, y, L + w, y + 42), L"Nasıl çalışır?", B_SECONDARY, true);
    Button(ID_REINSTALL, D2D1::RectF(L + w + 8, y, L + 2 * w + 8, y + 42), L"Yeniden kur", B_SECONDARY, can);
    Button(ID_UNINSTALL, D2D1::RectF(L + 2 * w + 16, y, R, y + 42), L"Kaldır", B_DANGER, can);
  } else {
    Button(ID_INFO, D2D1::RectF(L, y, R, y + 42), L"Nasıl çalışır?", B_SECONDARY, true);
  }
  y += 58.0f;

  if (!m.msg.empty()) Text(m.msg, g_fBody, D2D1::RectF(L + 8, y, R - 8, y + 80), KindColor(m.msgKind), A_CENTER, true);

  bool lh = g_hover == ID_LINK;
  D2D1_RECT_F link = D2D1::RectF(L, kWinH - 38, R, kWinH - 16);
  Text(L"Sürüm " + W(APP_VERSION) + L" · github.com/unsalable/KonvoyTP", g_fSmall, link, lh ? C::Blue : C::Text3, A_CENTER);
  AddHit(ID_LINK, link);
}

void PaintInfo() {
  const float L = 24.0f, R = kWinW - 24.0f;
  Button(ID_BACK, D2D1::RectF(L, 22, L + 96, 56), L"‹  Geri", B_SECONDARY, true);
  Text(L"Nasıl çalışır?", g_fH1, D2D1::RectF(L + 2, 70, R, 104), C::Text);
  struct Sec {
    const wchar_t* h;
    const wchar_t* b;
  } secs[] = {
      {L"Oyunda", L"Kamyondayken F8 menüyü açar. Shift+F8 menüyü açmadan seni yola çeker."},
      {L"Konvoy", L"Arkadaşların listede görünür. Git: arkadaşının arkasına ışınlanırsın. Getir: onu yanına çekersin (o izin verdiyse, "
                  L"varsayılan: sadece konvoyu kuran). Herkeste KonvoyTP kurulu olmalı."},
      {L"Kurtar", L"Yola çek · Kendi şeridine geç · Kamyonu düzelt · Geri sar (10 sn – 3 dk) · Son ışınlanmayı geri al."},
      {L"Güncelleme", L"Bu uygulama her açılışta yeni sürüm var mı bakar, varsa kendini ve modu günceller. Oyun da açılışta modun yeni "
                      L"sürümünü kendisi indirir."},
      {L"Bilmen gerekenler", L"Oyunun kendi ışınlanma komutunu kullanır. Resmi konvoy modu içindir, TruckersMP'de çalışmaz. Kaldırınca "
                             L"kayıtlı yerlerin masaüstüne yedeklenir."},
  };
  const float pad = 16.0f, inner = R - L - pad * 2;
  float heights[5], total = 0;
  for (int i = 0; i < 5; ++i) {
    heights[i] = 22.0f + TextHeight(secs[i].b, g_fBody, inner) + pad * 2 - 6.0f;
    total += heights[i];
  }
  float y = 116.0f;
  D2D1_RECT_F card = D2D1::RectF(L, y, R, y + total);
  FillRound(card, 14.0f, C::Card);
  StrokeRound(card, 14.0f, C::Line);
  for (int i = 0; i < 5; ++i) {
    if (i > 0) Fill(D2D1::RectF(L + pad, y, R, y + Px()), C::Sep);
    Text(secs[i].h, g_fBodyB, D2D1::RectF(L + pad, y + pad - 2, R - pad, y + pad + 20), C::Text);
    Text(secs[i].b, g_fBody, D2D1::RectF(L + pad, y + pad + 20, R - pad, y + heights[i]), C::Text2, A_LEFT, true);
    y += heights[i];
  }
}

void Render() {
  if (!EnsureTarget()) return;
  g_rt->BeginDraw();
  g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
  g_rt->Clear(Col(C::Bg));
  g_hits.clear();
  if (g_page == 0)
    PaintMain(Snapshot());
  else
    PaintInfo();
  if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) DiscardTarget();
}

int HitTest(float x, float y) {
  for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it)
    if (x >= it->r.left && x < it->r.right && y >= it->r.top && y < it->r.bottom) return it->id;
  return ID_NONE;
}

// =====================================================================================
// Actions
// =====================================================================================
std::wstring PickFolder() {
  std::wstring out;
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
  DWORD opt = 0;
  dlg->GetOptions(&opt);
  dlg->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dlg->SetTitle(L"Euro Truck Simulator 2 klasörünü seç");
  if (SUCCEEDED(dlg->Show(g_wnd))) {
    IShellItem* it = nullptr;
    if (SUCCEEDED(dlg->GetResult(&it)) && it) {
      PWSTR p = nullptr;
      if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) out = p;
      CoTaskMemFree(p);
      it->Release();
    }
  }
  dlg->Release();
  return out;
}

void ChooseGameFolder() {
  std::wstring sel = PickFolder();
  if (sel.empty()) return;
  std::wstring dir = NormalizeGameDir(sel);
  if (dir.empty()) {
    MessageBoxW(g_wnd,
                L"Bu klasörde Euro Truck Simulator 2 bulunamadı.\n\nSteam > Kütüphane > ETS2 > sağ tık > Yönet > Yerel dosyalara göz at ile açılan klasörü seç.",
                L"KonvoyTP", MB_OK | MB_ICONWARNING);
    return;
  }
  SaveGameDir(dir);
  Mutate([&](Model& m) {
    m.gameDir = dir;
    m.msg.clear();
  });
  RefreshGame();
  RunAsync(AutoModUpdate);
}

void RelaunchAsAdmin(const std::wstring& args) {
  std::wstring exe = ExePath();
  SHELLEXECUTEINFOW sei;
  memset(&sei, 0, sizeof(sei));
  sei.cbSize = sizeof(sei);
  sei.lpVerb = L"runas";
  sei.lpFile = exe.c_str();
  sei.lpParameters = args.c_str();
  sei.nShow = SW_SHOWNORMAL;
  sei.fMask = SEE_MASK_NOCLOSEPROCESS;
  if (ShellExecuteExW(&sei) && sei.hProcess) {
    WaitForSingleObject(sei.hProcess, INFINITE);
    CloseHandle(sei.hProcess);
  }
}

void StartInstall() {
  Model m = Snapshot();
  if (m.busy) return;
  Mutate([](Model& x) {
    x.busy = true;
    x.busyText = L"Kuruluyor…";
    x.msg.clear();
  });
  std::wstring dir = m.gameDir;
  RunAsync([dir]() {
    std::string err;
    bool ok = InstallMod(dir, err, [](const std::wstring& s) { Mutate([&](Model& x) { x.busyText = s; }); });
    if (!ok && g_needAdmin) {
      Mutate([](Model& x) { x.busyText = L"Yönetici izni bekleniyor…"; });
      RelaunchAsAdmin(L"--kur --uygulamasiz --oyun \"" + dir + L"\"");
      ok = Exists(PathsOf(dir).ktpDll) && ktpup::CompareVersions(FileVersionOf(PathsOf(dir).ktpDll), APP_VERSION) >= 0;
      if (ok) err.clear();
    }
    if (ok) InstallAppSelf();
    Mutate([&](Model& x) {
      x.busy = false;
      x.msg = ok ? L"Kuruldu! Oyunu aç, kamyondayken F8'e bas.\nArkadaşlarının da kurması gerekiyor." : L"Kurulamadı: " + W(err);
      x.msgKind = ok ? K_OK : K_ERR;
    });
    RefreshGame();
  });
}

void StartUninstall() {
  Model m = Snapshot();
  if (m.busy) return;
  if (MessageBoxW(g_wnd, L"KonvoyTP oyundan ve bu bilgisayardan kaldırılsın mı?\n\nKayıtlı yerlerin masaüstüne yedeklenir. SPF Framework yerinde kalır.",
                  L"KonvoyTP'yi kaldır", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
    return;
  Mutate([](Model& x) {
    x.busy = true;
    x.busyText = L"Kaldırılıyor…";
    x.msg.clear();
  });
  std::wstring dir = m.gameDir;
  RunAsync([dir]() {
    std::string err;
    bool saved = false;
    bool ok = UninstallMod(dir, err, saved);
    if (ok) RemoveAppSelf();
    Mutate([&](Model& x) {
      x.busy = false;
      x.msg = ok ? (saved ? L"Kaldırıldı. Kayıtlı yerlerin masaüstünde (KonvoyTP_yerler_yedek.txt)." : L"Kaldırıldı.") : L"Kaldırılamadı: " + W(err);
      x.msgKind = ok ? K_OK : K_ERR;
    });
    RefreshGame();
  });
}

void OnClick(int id) {
  Model m = Snapshot();
  switch (id) {
    case ID_PRIMARY: {
      Primary p = PrimaryFor(m);
      if (!p.enabled) break;
      if (p.action == 1) ChooseGameFolder();
      if (p.action == 2) StartInstall();
      if (p.action == 3) ShellExecuteW(g_wnd, L"open", L"steam://rungameid/227300", nullptr, nullptr, SW_SHOWNORMAL);
      break;
    }
    case ID_GAMEROW:
      if (!m.busy) ChooseGameFolder();
      break;
    case ID_REINSTALL:
      StartInstall();
      break;
    case ID_UNINSTALL:
      StartUninstall();
      break;
    case ID_INFO:
      g_page = 1;
      break;
    case ID_BACK:
      g_page = 0;
      break;
    case ID_LINK:
      ShellExecuteW(g_wnd, L"open", L"https://github.com/unsalable/KonvoyTP", nullptr, nullptr, SW_SHOWNORMAL);
      break;
  }
  g_hover = ID_NONE;
  InvalidateRect(g_wnd, nullptr, FALSE);
}

void OnTimer() {
  CleanupOldSelf();
  bool running = IsGameRunning();
  Model m = Snapshot();
  if (running != m.gameRunning) RefreshGame();
  if (m.pendingModUpdate && !running && !m.busy) {
    Mutate([](Model& x) { x.pendingModUpdate = false; });
    RunAsync(AutoModUpdate);
  }
  if (m.restartPending && !m.busy) PostMessageW(g_wnd, WM_APP_RESTART, 0, 0);
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_DPICHANGED: {
      g_dpi = HIWORD(wp);
      const RECT* r = (const RECT*)lp;
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      DiscardTarget();
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    }
    case WM_SIZE:
      if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(h, &ps);
      Render();
      EndPaint(h, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_MOUSEMOVE: {
      float x = GET_X_LPARAM(lp) * Px(), y = GET_Y_LPARAM(lp) * Px();
      int id = HitTest(x, y);
      if (id != g_hover) {
        g_hover = id;
        InvalidateRect(h, nullptr, FALSE);
      }
      if (!g_tracking) {
        TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        g_tracking = true;
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      g_tracking = false;
      if (g_hover) {
        g_hover = ID_NONE;
        InvalidateRect(h, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONDOWN:
      g_hover = g_press = HitTest(GET_X_LPARAM(lp) * Px(), GET_Y_LPARAM(lp) * Px());
      if (g_press) SetCapture(h);
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    case WM_LBUTTONUP: {
      int id = g_press;
      g_press = ID_NONE;
      if (GetCapture() == h) ReleaseCapture();
      InvalidateRect(h, nullptr, FALSE);
      if (id && id == HitTest(GET_X_LPARAM(lp) * Px(), GET_Y_LPARAM(lp) * Px())) OnClick(id);
      return 0;
    }
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        SetCursor(LoadCursor(nullptr, g_hover ? IDC_HAND : IDC_ARROW));
        return TRUE;
      }
      break;
    case WM_KEYDOWN:
      if (wp == VK_F1) OnClick(g_page == 0 ? ID_INFO : ID_BACK);
      if (wp == VK_RETURN) OnClick(g_page == 0 ? ID_PRIMARY : ID_BACK);
      if (wp == VK_ESCAPE) {
        if (g_page == 1)
          OnClick(ID_BACK);
        else
          PostMessageW(h, WM_CLOSE, 0, 0);
      }
      return 0;
    case WM_TIMER:
      OnTimer();
      return 0;
    case WM_APP + 1:  // WM_APP_REFRESH
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    case WM_APP + 2: {  // WM_APP_RESTART
      Model m = Snapshot();
      if (m.busy) return 0;  // OnTimer retries once the current job is done
      Mutate([](Model& x) { x.restartPending = false; });
      if (Launch(ExePath(), L"--guncellendi")) DestroyWindow(h);
      return 0;
    }
    case WM_CLOSE:
      if (Snapshot().busy) {
        MessageBeep(MB_ICONWARNING);
        return 0;
      }
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      InterlockedExchange(&g_cancel, 1);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

// =====================================================================================
// Command line
// =====================================================================================
struct Args {
  bool install = false, uninstall = false, silent = false, noApp = false, updated = false;
  std::wstring game;
};

Args ParseArgs() {
  Args a;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; argv && i < argc; ++i) {
    std::wstring s = argv[i];
    if (s == L"--kur") a.install = true;
    else if (s == L"--kaldir") a.uninstall = true;
    else if (s == L"--sessiz") a.silent = true;
    else if (s == L"--uygulamasiz") a.noApp = true;
    else if (s == L"--guncellendi") a.updated = true;
    else if (s == L"--oyun" && i + 1 < argc) a.game = argv[++i];
  }
  LocalFree(argv);
  return a;
}

std::wstring ResolveGameDir(const std::wstring& given) {
  if (!given.empty()) return NormalizeGameDir(given);
  std::wstring d = NormalizeGameDir(LoadSavedGameDir());
  if (d.empty()) d = FindGameDir();
  return d;
}

int RunCli(const Args& a) {
  std::wstring game = ResolveGameDir(a.game);
  std::string err;
  if (a.install) {
    bool ok = InstallMod(game, err, [](const std::wstring&) {});
    if (ok && !a.noApp) InstallAppSelf();
    if (ok && !game.empty()) SaveGameDir(game);
    Log("--kur: %s %s", ok ? "tamam" : "HATA", err.c_str());
    if (!a.silent) MessageBoxW(nullptr, ok ? L"KonvoyTP kuruldu." : (L"Kurulamadı: " + W(err)).c_str(), L"KonvoyTP", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
    return ok ? 0 : 1;
  }
  // --kaldir (also the Windows "Uygulamalar" uninstall entry)
  if (!a.silent && MessageBoxW(nullptr, L"KonvoyTP oyundan ve bu bilgisayardan kaldırılsın mı?\n\nKayıtlı yerlerin masaüstüne yedeklenir.", L"KonvoyTP'yi kaldır",
                               MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
    return 2;
  bool saved = false;
  bool ok = UninstallMod(game, err, saved);
  if (ok && !a.noApp) RemoveAppSelf();
  Log("--kaldir: %s %s", ok ? "tamam" : "HATA", err.c_str());
  if (!a.silent) MessageBoxW(nullptr, ok ? L"KonvoyTP kaldırıldı." : (L"Kaldırılamadı: " + W(err)).c_str(), L"KonvoyTP", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
  if (g_removeSelfOnExit) RemoveSelfNow();
  return ok ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Args args = ParseArgs();
  bool havePayload = LoadPayload();
  CleanupOldSelf();
  if (args.install || args.uninstall) {
    if (!havePayload && args.install) return 3;
    int rc = RunCli(args);
    CoUninitialize();
    return rc;
  }

  g_m.gameDir = ResolveGameDir(L"");
  if (!g_m.gameDir.empty()) SaveGameDir(g_m.gameDir);
  if (args.updated) {
    g_appJustUpdated = true;
    g_m.msg = L"KonvoyTP " + W(APP_VERSION) + L" sürümüne güncellendi.";
    g_m.msgKind = K_OK;
  }
  if (!havePayload) {
    g_m.msg = L"Uygulama dosyası eksik görünüyor; GitHub'dan yeniden indir.";
    g_m.msgKind = K_ERR;
  }

  CreateDeviceIndependent();
  WNDCLASSEXW wc;
  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hi;
  wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
  wc.hIconSm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = L"KonvoyTPApp";
  RegisterClassExW(&wc);

  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  POINT pt = {0, 0};
  HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi;
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(mon, &mi);
  UINT dpi = GetDpiForSystem();
  RECT r = {0, 0, MulDiv((int)kWinW, (int)dpi, 96), MulDiv((int)kWinH, (int)dpi, 96)};
  AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
  int w = r.right - r.left, ht = r.bottom - r.top;
  int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2, y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - ht) / 2;
  g_wnd = CreateWindowExW(0, wc.lpszClassName, L"KonvoyTP", style, x, y, w, ht, nullptr, nullptr, hi, nullptr);
  if (!g_wnd) return 1;
  g_dpi = GetDpiForWindow(g_wnd);
  if (g_dpi != dpi) {
    RECT r2 = {0, 0, MulDiv((int)kWinW, (int)g_dpi, 96), MulDiv((int)kWinH, (int)g_dpi, 96)};
    AdjustWindowRectExForDpi(&r2, style, FALSE, 0, g_dpi);
    SetWindowPos(g_wnd, nullptr, 0, 0, r2.right - r2.left, r2.bottom - r2.top, SWP_NOMOVE | SWP_NOZORDER);
  }
  COLORREF cap = RGB(0xF5, 0xF5, 0xF7), txt = RGB(0x1D, 0x1D, 0x1F);
  DwmSetWindowAttribute(g_wnd, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof(cap));
  DwmSetWindowAttribute(g_wnd, 36 /* DWMWA_TEXT_COLOR */, &txt, sizeof(txt));
  ShowWindow(g_wnd, show);
  UpdateWindow(g_wnd);
  SetTimer(g_wnd, 1, 2000, nullptr);

  RefreshGame();
  if (havePayload) RunAsync(UpdateWorker);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (g_removeSelfOnExit) RemoveSelfNow();
  CoUninitialize();
  return 0;
}

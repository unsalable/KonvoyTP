// KonvoyTP - GitHub release check, download and verification.
// Shared by the game plugin (self-update at game start) and the KonvoyTP app (update on open).
// Only Windows components are used: WinHTTP (TLS, proxy, redirects) and BCrypt (SHA-256).
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace ktpup {

// The one place releases come from.
static const wchar_t kLatestApi[] = L"https://api.github.com/repos/unsalable/KonvoyTP/releases/latest";
static const char kAssetDll[] = "KonvoyTP.dll";
static const char kAssetApp[] = "KonvoyTP_Kurulum.exe";

// ------------------------------------------------------------------------------------------
// Versions: "v1.3.0", "1.3", "1.3.0.0" -> numeric parts
// ------------------------------------------------------------------------------------------
inline std::vector<int> VersionParts(const std::string& v) {
  std::vector<int> r;
  size_t i = 0;
  while (i < v.size() && !std::isdigit((unsigned char)v[i])) ++i;
  while (i < v.size() && std::isdigit((unsigned char)v[i])) {
    int n = 0;
    while (i < v.size() && std::isdigit((unsigned char)v[i])) n = n * 10 + (v[i++] - '0');
    r.push_back(n);
    if (i < v.size() && v[i] == '.') ++i;
  }
  return r;
}

// <0: a older than b, 0: same, >0: a newer
inline int CompareVersions(const std::string& a, const std::string& b) {
  std::vector<int> x = VersionParts(a), y = VersionParts(b);
  size_t n = std::max(x.size(), y.size());
  for (size_t i = 0; i < n; ++i) {
    int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
    if (p != q) return p < q ? -1 : 1;
  }
  return 0;
}

// "v1.3.0" -> "1.3.0"
inline std::string CleanVersion(const std::string& v) {
  std::vector<int> p = VersionParts(v);
  std::string s;
  for (size_t i = 0; i < p.size() && i < 3; ++i) s += (i ? "." : "") + std::to_string(p[i]);
  return s.empty() ? v : s;
}

// ------------------------------------------------------------------------------------------
// Tiny JSON reader (enough for the GitHub API and SPF's settings file)
// ------------------------------------------------------------------------------------------
struct JVal {
  enum Kind { NUL, BOOL, NUM, STR, ARR, OBJ } k = NUL;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<JVal> a;
  std::vector<std::pair<std::string, JVal>> o;
  const JVal* Get(const char* key) const {
    if (k != OBJ) return nullptr;
    for (const auto& kv : o)
      if (kv.first == key) return &kv.second;
    return nullptr;
  }
  std::string Str(const char* key) const {
    const JVal* v = Get(key);
    return v && v->k == STR ? v->s : std::string();
  }
};

inline void AppendUtf8(std::string& out, unsigned cp) {
  if (cp < 0x80) {
    out += (char)cp;
  } else if (cp < 0x800) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

class JParser {
 public:
  JParser(const char* b, const char* e) : p_(b), e_(e) {}
  bool Parse(JVal& v) {
    if (!Val(v)) return false;
    Ws();
    return p_ == e_;
  }

 private:
  const char* p_;
  const char* e_;
  int depth_ = 0;
  void Ws() {
    while (p_ < e_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) ++p_;
  }
  bool Lit(const char* w) {
    size_t n = strlen(w);
    if ((size_t)(e_ - p_) >= n && memcmp(p_, w, n) == 0) {
      p_ += n;
      return true;
    }
    return false;
  }
  static int Hex4(const char* q) {
    int v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = q[i];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= c - '0';
      else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
      else return -1;
    }
    return v;
  }
  bool Str(std::string& out) {
    if (p_ >= e_ || *p_ != '"') return false;
    ++p_;
    while (p_ < e_ && *p_ != '"') {
      char c = *p_++;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (p_ >= e_) return false;
      char x = *p_++;
      switch (x) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          if (e_ - p_ < 4) return false;
          int cp = Hex4(p_);
          if (cp < 0) return false;
          p_ += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF && e_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
            int lo = Hex4(p_ + 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              p_ += 6;
            }
          }
          AppendUtf8(out, (unsigned)cp);
          break;
        }
        default: return false;
      }
    }
    if (p_ >= e_) return false;
    ++p_;
    return true;
  }
  bool Val(JVal& v) {
    if (++depth_ > 96) return false;
    Ws();
    bool ok = false;
    if (p_ >= e_) {
    } else if (*p_ == '{') {
      ++p_;
      v.k = JVal::OBJ;
      Ws();
      if (p_ < e_ && *p_ == '}') {
        ++p_;
        ok = true;
      } else {
        for (;;) {
          Ws();
          std::string key;
          if (!Str(key)) break;
          Ws();
          if (p_ >= e_ || *p_ != ':') break;
          ++p_;
          JVal c;
          if (!Val(c)) break;
          v.o.emplace_back(std::move(key), std::move(c));
          Ws();
          if (p_ < e_ && *p_ == ',') {
            ++p_;
            continue;
          }
          if (p_ < e_ && *p_ == '}') {
            ++p_;
            ok = true;
          }
          break;
        }
      }
    } else if (*p_ == '[') {
      ++p_;
      v.k = JVal::ARR;
      Ws();
      if (p_ < e_ && *p_ == ']') {
        ++p_;
        ok = true;
      } else {
        for (;;) {
          JVal c;
          if (!Val(c)) break;
          v.a.push_back(std::move(c));
          Ws();
          if (p_ < e_ && *p_ == ',') {
            ++p_;
            continue;
          }
          if (p_ < e_ && *p_ == ']') {
            ++p_;
            ok = true;
          }
          break;
        }
      }
    } else if (*p_ == '"') {
      v.k = JVal::STR;
      ok = Str(v.s);
    } else if (Lit("true")) {
      v.k = JVal::BOOL;
      v.b = true;
      ok = true;
    } else if (Lit("false")) {
      v.k = JVal::BOOL;
      ok = true;
    } else if (Lit("null")) {
      ok = true;
    } else {
      const char* q = p_;
      if (q < e_ && (*q == '-' || *q == '+')) ++q;
      while (q < e_ && (std::isdigit((unsigned char)*q) || *q == '.' || *q == 'e' || *q == 'E' || *q == '-' || *q == '+')) ++q;
      if (q > p_) {
        v.k = JVal::NUM;
        v.n = strtod(std::string(p_, q).c_str(), nullptr);
        p_ = q;
        ok = true;
      }
    }
    --depth_;
    return ok;
  }
};

inline bool ParseJson(const std::string& text, JVal& out) {
  size_t start = text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF ? 3 : 0;
  JParser ps(text.data() + start, text.data() + text.size());
  return ps.Parse(out);
}

// ------------------------------------------------------------------------------------------
// GitHub release model
// ------------------------------------------------------------------------------------------
struct Asset {
  std::string name, url, sha256;  // sha256: lowercase hex, from GitHub's "digest"
  uint64_t size = 0;
};

struct Release {
  std::string tag;
  std::vector<Asset> assets;
  const Asset* Find(const char* name) const {
    for (const auto& a : assets)
      if (a.name == name) return &a;
    return nullptr;
  }
};

inline bool ParseRelease(const std::string& json, Release& out) {
  JVal root;
  if (!ParseJson(json, root) || root.k != JVal::OBJ) return false;
  out.tag = root.Str("tag_name");
  const JVal* draft = root.Get("draft");
  if (draft && draft->k == JVal::BOOL && draft->b) return false;
  const JVal* as = root.Get("assets");
  if (as && as->k == JVal::ARR) {
    for (const JVal& a : as->a) {
      Asset x;
      x.name = a.Str("name");
      x.url = a.Str("browser_download_url");
      std::string d = a.Str("digest");
      if (d.rfind("sha256:", 0) == 0) {
        x.sha256 = d.substr(7);
        for (char& c : x.sha256) c = (char)std::tolower((unsigned char)c);
      }
      const JVal* sz = a.Get("size");
      if (sz && sz->k == JVal::NUM && sz->n > 0) x.size = (uint64_t)sz->n;
      if (!x.name.empty() && x.url.rfind("https://", 0) == 0) out.assets.push_back(x);
    }
  }
  return !out.tag.empty();
}

// ------------------------------------------------------------------------------------------
// Small helpers
// ------------------------------------------------------------------------------------------
inline std::wstring Widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}

inline std::string Narrow(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}

inline std::string Sha256Hex(const void* data, size_t n) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE h = nullptr;
  unsigned char out[32];
  std::string hex;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return hex;
  if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
    const unsigned char* p = (const unsigned char*)data;
    bool ok = true;
    while (n > 0 && ok) {
      ULONG chunk = (ULONG)std::min<size_t>(n, 1u << 30);
      ok = BCryptHashData(h, (PUCHAR)p, chunk, 0) == 0;
      p += chunk;
      n -= chunk;
    }
    if (ok && BCryptFinishHash(h, out, 32, 0) == 0) {
      static const char kHex[] = "0123456789abcdef";
      for (unsigned char c : out) {
        hex += kHex[c >> 4];
        hex += kHex[c & 15];
      }
    }
    BCryptDestroyHash(h);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  return hex;
}

inline bool ReadFileAll(const std::wstring& path, std::string& data) {
  data.clear();
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz{};
  bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart < (1ll << 31);
  if (ok) {
    data.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    ok = sz.QuadPart == 0 || (ReadFile(f, &data[0], (DWORD)sz.QuadPart, &got, nullptr) && got == (DWORD)sz.QuadPart);
  }
  CloseHandle(f);
  if (!ok) data.clear();
  return ok;
}

// Writes via a temporary file + rename, so a crash never leaves a half-written file behind.
inline bool WriteFileAll(const std::wstring& path, const std::string& data) {
  std::wstring tmp = path + L".tmp";
  HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD wr = 0;
  bool ok = data.empty() || (WriteFile(f, data.data(), (DWORD)data.size(), &wr, nullptr) && wr == (DWORD)data.size());
  ok = FlushFileBuffers(f) && ok;
  CloseHandle(f);
  if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
  if (!ok) DeleteFileW(tmp.c_str());
  return ok;
}

// x64 Windows PE (exe or dll)?
inline bool LooksLikePe64(const std::string& d) {
  if (d.size() < 0x200 || d[0] != 'M' || d[1] != 'Z') return false;
  uint32_t pe = 0;
  memcpy(&pe, d.data() + 0x3C, 4);
  if (pe + 6 > d.size() || memcmp(d.data() + pe, "PE\0\0", 4) != 0) return false;
  uint16_t machine = 0;
  memcpy(&machine, d.data() + pe + 4, 2);
  return machine == 0x8664;
}

// ------------------------------------------------------------------------------------------
// HTTP GET (TLS, follows GitHub's redirect to its download CDN)
// ------------------------------------------------------------------------------------------
typedef void (*ProgressFn)(uint64_t got, uint64_t total, void* ctx);

inline bool HttpGet(const std::wstring& url, std::string& body, std::string& err, volatile LONG* cancel = nullptr, uint64_t maxBytes = 64ull << 20,
                    ProgressFn prog = nullptr, void* ctx = nullptr, bool api = false) {
  body.clear();
  err.clear();
  URL_COMPONENTS uc;
  memset(&uc, 0, sizeof(uc));
  uc.dwStructSize = sizeof(uc);
  uc.dwHostNameLength = (DWORD)-1;
  uc.dwUrlPathLength = (DWORD)-1;
  uc.dwExtraInfoLength = (DWORD)-1;
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || !uc.lpszHostName) {
    err = "adres hatalı";
    return false;
  }
  std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
  std::wstring path = uc.lpszUrlPath ? std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) : L"/";
  if (uc.lpszExtraInfo) path += std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);

  HINTERNET ses = WinHttpOpen(L"KonvoyTP-guncelleyici", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!ses) ses = WinHttpOpen(L"KonvoyTP-guncelleyici", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!ses) {
    err = "internet bağlantısı açılamadı";
    return false;
  }
  WinHttpSetTimeouts(ses, 10000, 10000, 15000, 30000);
  DWORD protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 /* TLS 1.3 */;
  if (!WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos))) {
    protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos));
  }
  bool ok = false;
  HINTERNET con = WinHttpConnect(ses, host.c_str(), uc.nPort, 0);
  HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
  if (!req) {
    err = "sunucuya bağlanılamadı";
  } else {
    const wchar_t* hdr = api ? L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n" : L"Accept: application/octet-stream\r\n";
    if (!WinHttpSendRequest(req, hdr, (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(req, nullptr)) {
      DWORD e = GetLastError();
      err = e == ERROR_WINHTTP_NAME_NOT_RESOLVED || e == ERROR_WINHTTP_CANNOT_CONNECT ? "internet yok" : (e == ERROR_WINHTTP_TIMEOUT ? "zaman aşımı" : "bağlantı hatası " + std::to_string(e));
    } else {
      DWORD status = 0, len = sizeof(status);
      WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
      DWORD total = 0;
      len = sizeof(total);
      if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &total, &len, WINHTTP_NO_HEADER_INDEX))
        total = 0;
      if (status != 200) {
        err = "sunucu cevabı " + std::to_string(status);
      } else if (total && total > maxBytes) {
        err = "dosya beklenenden büyük";
      } else {
        if (total) body.reserve(total);
        std::vector<char> buf(256 * 1024);
        ok = true;
        for (;;) {
          if (cancel && *cancel) {
            ok = false;
            err = "iptal edildi";
            break;
          }
          DWORD avail = 0;
          if (!WinHttpQueryDataAvailable(req, &avail)) {
            ok = false;
            err = "indirme yarıda kesildi";
            break;
          }
          if (avail == 0) break;
          DWORD got = 0;
          if (!WinHttpReadData(req, buf.data(), std::min<DWORD>(avail, (DWORD)buf.size()), &got)) {
            ok = false;
            err = "indirme yarıda kesildi";
            break;
          }
          if (got == 0) break;
          body.append(buf.data(), got);
          if (body.size() > maxBytes) {
            ok = false;
            err = "dosya beklenenden büyük";
            break;
          }
          if (prog) prog(body.size(), total, ctx);
        }
        if (ok && total && body.size() != total) {
          ok = false;
          err = "indirme eksik";
        }
      }
    }
  }
  if (req) WinHttpCloseHandle(req);
  if (con) WinHttpCloseHandle(con);
  WinHttpCloseHandle(ses);
  if (!ok) body.clear();
  return ok;
}

inline bool FetchLatest(Release& out, std::string& err, volatile LONG* cancel = nullptr) {
  std::string json;
  if (!HttpGet(kLatestApi, json, err, cancel, 4ull << 20, nullptr, nullptr, true)) return false;
  if (!ParseRelease(json, out)) {
    err = "sürüm bilgisi okunamadı";
    return false;
  }
  return true;
}

// Downloads one release file and checks it against the size and SHA-256 GitHub reports for it.
inline bool DownloadAsset(const Asset& a, std::string& data, std::string& err, volatile LONG* cancel = nullptr, ProgressFn prog = nullptr, void* ctx = nullptr) {
  if (a.sha256.size() != 64) {
    err = "dosyanın doğrulama bilgisi yok";
    return false;
  }
  uint64_t limit = a.size ? a.size : (200ull << 20);
  if (!HttpGet(Widen(a.url), data, err, cancel, limit, prog, ctx)) return false;
  if (a.size && data.size() != a.size) {
    err = "indirilen dosyanın boyutu tutmuyor";
    data.clear();
    return false;
  }
  if (Sha256Hex(data.data(), data.size()) != a.sha256) {
    err = "indirilen dosya doğrulanamadı (SHA-256)";
    data.clear();
    return false;
  }
  return true;
}

}  // namespace ktpup

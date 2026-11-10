// ktp_pack: builds the payload that is embedded into the KonvoyTP app (plugin DLL, its texts, SPF Framework).
//   ktp_pack out.bin  name=path  ...  prefix/=@directory   (all files of a directory)
// Format: "KTPPAK1\0", u32 count, then per file: u16 nameLen, name (UTF-8, '/'), u32 rawSize, u32 storedSize,
//         u8 method (0 stored, 1 LZMS via the Windows Compression API), data.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <compressapi.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "cabinet.lib")

static bool ReadAll(const std::wstring& p, std::string& d) {
  FILE* f = _wfopen(p.c_str(), L"rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  d.resize((size_t)n);
  bool ok = n == 0 || fread(&d[0], 1, (size_t)n, f) == (size_t)n;
  fclose(f);
  return ok;
}

static std::wstring W(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  w.resize(wcslen(w.c_str()));
  return w;
}

static std::string U(const std::wstring& w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
  s.resize(strlen(s.c_str()));
  return s;
}

struct Item {
  std::string name;
  std::wstring path;
};

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: ktp_pack out.bin name=path ... prefix/=@dir\n");
    return 2;
  }
  std::vector<Item> items;
  for (int i = 2; i < argc; ++i) {
    std::wstring a = argv[i];
    size_t eq = a.find(L'=');
    if (eq == std::wstring::npos) return 2;
    std::string name = U(a.substr(0, eq));
    std::wstring src = a.substr(eq + 1);
    if (!src.empty() && src[0] == L'@') {
      std::wstring dir = src.substr(1);
      WIN32_FIND_DATAW fd;
      HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
      if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "missing dir %s\n", U(dir).c_str());
        return 1;
      }
      do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        items.push_back({name + U(fd.cFileName), dir + L"\\" + fd.cFileName});
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    } else {
      items.push_back({name, src});
    }
  }

  COMPRESSOR_HANDLE comp = nullptr;
  if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &comp)) {
    fprintf(stderr, "CreateCompressor failed\n");
    return 1;
  }
  std::string out("KTPPAK1\0", 8);
  uint32_t count = (uint32_t)items.size();
  out.append((const char*)&count, 4);
  uint64_t rawTotal = 0;
  for (const Item& it : items) {
    std::string d;
    if (!ReadAll(it.path, d)) {
      fprintf(stderr, "cannot read %s\n", U(it.path).c_str());
      return 1;
    }
    std::string stored;
    uint8_t method = 0;
    if (d.size() >= 4096) {
      SIZE_T need = 0;
      Compress(comp, d.data(), d.size(), nullptr, 0, &need);
      std::string c(need, '\0');
      SIZE_T got = 0;
      if (need && Compress(comp, d.data(), d.size(), &c[0], need, &got) && got < d.size()) {
        c.resize(got);
        stored.swap(c);
        method = 1;
      }
    }
    if (!method) stored = d;
    uint16_t nl = (uint16_t)it.name.size();
    uint32_t raw = (uint32_t)d.size(), st = (uint32_t)stored.size();
    out.append((const char*)&nl, 2);
    out += it.name;
    out.append((const char*)&raw, 4);
    out.append((const char*)&st, 4);
    out.append((const char*)&method, 1);
    out += stored;
    rawTotal += raw;
    printf("  %-48s %9u -> %9u\n", it.name.c_str(), raw, st);
  }
  CloseCompressor(comp);
  FILE* f = _wfopen(argv[1], L"wb");
  if (!f || fwrite(out.data(), 1, out.size(), f) != out.size()) {
    fprintf(stderr, "cannot write output\n");
    return 1;
  }
  fclose(f);
  printf("payload: %u files, %llu -> %zu bytes\n", count, (unsigned long long)rawTotal, out.size());
  return 0;
}

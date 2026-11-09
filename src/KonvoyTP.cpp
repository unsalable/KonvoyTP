// =====================================================================================
//  KonvoyTP - ETS2 Konvoy Isinlanma eklentisi (SPF-Framework plugin)
//
//  * Konvoydaki arkadasinin yanina isinlan, onu yanina cek (yonetici), herkesi topla.
//  * Devrilen / sikisan kamyonu duzelt, U donusu yap, "geri sar" ile yola don,
//    son isinlanmayi geri al.
//  * Kayitli yerlere ve koordinatlara isinlan.
//  (Sehir merkezine dogrudan isinlanma bilerek YOK: oyunda yola oturtan bir fonksiyon yok,
//   test edildiginde kamyon bina/havada kaliyordu.)
//
//  Nasil calisir?
//   - Isinlanma: oyunun kendi "cheat teleport X;Y;Z;A;E" komutu (SPF GameConsole API).
//     Oyunun isinlanma fonksiyonu konvoyu destekler (diger oyunculara yeni konumu bildirir).
//   - Arkadas konumlari: konvoy sohbeti uzerinden gizli "#KTP1 ..." mesajlari. Mod kurulu
//     oyuncularda bu mesajlar sohbette GORUNMEZ (yakalanip islenir).
//   - Yetki: "yanina cek" istegini kabul edip etmemeye her oyuncu kendi karar verir
//     (varsayilan: sadece konvoy sahibi = oyun icindeki oyuncu kimligi 1).
// =====================================================================================

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include <SPF_Plugin.h>
#include <SPF_Config_API.h>
#include <SPF_Formatting_API.h>
#include <SPF_GameConsole_API.h>
#include <SPF_Hooks_API.h>
#include <SPF_Icons.h>
#include <SPF_KeyBinds_API.h>
#include <SPF_Logger_API.h>
#include <SPF_Manifest_API.h>
#include <SPF_Telemetry_API.h>
#include <SPF_UI_API.h>

namespace KonvoyTP {

// -------------------------------------------------------------------------------------
// Game function types (ETS2 1.62, verified by disassembly)
//   AddToChat  : (chat_manager, uint16 sender_id, const char** text, uint8 type)   -> shows a line
//   ChatInput  : (chat_manager, const char** text, uint8 type, bool broadcast)     -> sends a line
// -------------------------------------------------------------------------------------
using AddToChat_t = void (*)(void* chat_manager, uint64_t sender_id, const char** pp_text, uint64_t message_type);
using ChatInput_t = void (*)(void* chat_manager, const char** pp_text, uint64_t message_type, uint64_t should_broadcast);

static const char kProto[] = "#KTP1 ";
static const size_t kProtoLen = 6;
static const double kPi = 3.14159265358979323846;
static const double kBehind = 25.0;  // metres behind a truck where we place another one

enum PullPolicy { PULL_HOST = 0, PULL_ALL = 1, PULL_NONE = 2 };

struct Spot {
  double x = 0, y = 0, z = 0;
  double h = 0;  // SDK heading, 0..1 (counter-clockwise, 0 = north)
  bool valid = false;
};

struct TrailPt {
  double t, x, y, z, h, odo;
  int seg;
  bool good;
};

struct Peer {
  std::string iid, name;
  uint16_t sender = 0;
  uint32_t flags = 0;  // 1: accepts pull from everyone, 2: accepts pull from host, 4: is host, 8: position valid
  Spot pos, back;
  double lastSeen = 0;
};

struct InMsg {
  uint16_t sender;
  std::string text;
};

struct Place {
  std::string name;
  Spot s;
};

struct TpJob {
  bool active = false;
  Spot dest;
  bool checkHeading = false;
  std::string label;     // success message
  std::string replyIid;  // send result to this player
  double nextTry = 0, deadline = 0;
  int tries = 0;
  bool devEnabledByUs = false;
  bool verifying = false;
  double verifyAt = 0;
  int fixAttempts = 0;
  Spot before;
  bool endsCrash = false;  // a recovery back onto the recorded road: forget the last crash afterwards
};

struct GotoReq {
  bool active = false;
  std::string iid;
  double deadline = 0;
};

struct Ctx {
  const SPF_Load_API* load = nullptr;
  const SPF_Core_API* core = nullptr;
  SPF_Logger_Handle* log = nullptr;
  SPF_Config_Handle* cfg = nullptr;
  SPF_KeyBinds_Handle* kb = nullptr;
  SPF_Telemetry_Handle* tel = nullptr;
  SPF_UI_API* ui = nullptr;
  SPF_Window_Handle* win = nullptr;

  // chat hooks
  AddToChat_t o_AddToChat = nullptr;
  ChatInput_t o_ChatInput = nullptr;
  SPF_Hook_Handle* hookDisplay = nullptr;
  SPF_Hook_Handle* hookInput = nullptr;
  void** ppGlobal = nullptr;
  uint32_t chatOffset = 0;
  // "call begin_loading_screen" inside the game's teleport cheat (skipped for nearby teleports)
  uint8_t* loadCallSite = nullptr;
  void* capturedChat = nullptr;
  void* lastChat = nullptr;
  double chatSince = -1;
  bool announced = false;

  std::mutex inMutex;
  std::vector<InMsg> inbox;

  // identity
  std::string iid;
  std::string autoName;
  int localId = -1;

  // settings
  char nameBuf[64] = "";
  int pullPolicy = PULL_HOST;
  bool restoreDev = true;
  int headingSign = 0;  // 0: not verified yet (+1 used), 1: verified, -1: mirrored

  // telemetry
  Spot me;
  double meSpeed = 0, mePitch = 0, meRoll = 0;
  bool paused = false;
  double lastTel = -1;
  Spot prevTel;
  double prevTelT = -1;
  std::deque<TrailPt> trail;
  double odo = 0;
  int seg = 0;
  bool jumpPending = false;
  // crash memory: "Yola çek" / "Kendi şeridine geç" prefer road points from before the last crash
  double crashT = -1e9;     // time of the last detected crash (skid/spin, roll-over, flying)
  double crashOdo = 0;      // odometer at that moment
  double calmUntil = -1;    // our own teleport just happened: ignore crash signals until then
  bool wheelInfo = false;   // the game reports wheel/ground contact (seen "on ground" at least once)
  int airReads = 0;         // consecutive reads with no wheel on the ground
  uint64_t offroadSubst = 0;  // bit i: substance i is off-road (grass, dirt, sand, mud, snow)
  double substCheckedAt = -100;

  // network
  std::map<std::string, Peer> peers;
  std::deque<std::string> outbox;
  double lastSend = -100;
  double lastQuery = -100;
  double pendingPAt = -1;

  TpJob job;
  GotoReq gotoReq;
  double lastJobEnd = -100;
  Spot undoSpot;  // where we were before the last successful teleport

  // saved places
  std::vector<Place> places;
  char placeName[64] = "";
  char coordBuf[128] = "";
  std::wstring placesPath;

  std::string lastError;
  bool wasVisible = false;
  double toggleAt = -1.0;     // menu key pressed while Shift was down (maybe the Shift+F8 chord)
  double roadKeyAt = -100.0;  // last "Yola çek" hotkey

  // our own toast notifications (drawn by a separate, non-interactive overlay window)
  struct Toast {
    std::string text;
    int kind;  // 0 info, 1 success, 2 warning, 3 error
    double t0, life;
  };
  std::deque<Toast> toasts;
  SPF_Window_Handle* toastWin = nullptr;
};

static Ctx g;

// =====================================================================================
// Small helpers
// =====================================================================================
#ifdef KTP_TEST
double g_testNow = 0.0;  // unit tests drive the clock
static double Now() { return g_testNow; }
#else
static double Now() {
  static const auto t0 = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
#endif

static std::string Fmt(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return buf;
}

static void Logf(SPF_LogLevel lvl, const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  const SPF_Logger_API* api = g.core ? g.core->logger : (g.load ? g.load->logger : nullptr);
  if (api && g.log) api->Log(g.log, lvl, buf);
}

static double Now();

// Shows one of our own pill-shaped notifications at the top of the screen (see RenderToasts).
static void Notify(SPF_NotificationType type, const std::string& msg) {
  Logf(SPF_LOG_INFO, "[bildirim] %s", msg.c_str());
  int kind = 0;
  if (type == SPF_NOTIFICATION_SUCCESS) kind = 1;
  else if (type == SPF_NOTIFICATION_WARNING) kind = 2;
  else if (type == SPF_NOTIFICATION_ERROR || type == SPF_NOTIFICATION_CRITICAL) kind = 3;
  g.toasts.push_back({msg, kind, Now(), kind == 3 ? 4.5 : (kind == 2 ? 3.5 : 2.6)});
  while (g.toasts.size() > 3) g.toasts.pop_front();
}

static std::string WideToUtf8(const wchar_t* w) {
  if (!w || !*w) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return "";
  std::string s(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

static double Dist(const Spot& a, double x, double y, double z) {
  double dx = a.x - x, dy = a.y - y, dz = a.z - z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}
static double Dist(const Spot& a, const Spot& b) { return Dist(a, b.x, b.y, b.z); }
static double Dist2D(const Spot& a, const Spot& b) {
  double dx = a.x - b.x, dz = a.z - b.z;
  return std::sqrt(dx * dx + dz * dz);
}

static double Wrap01(double h) {
  h = std::fmod(h, 1.0);
  if (h < 0) h += 1.0;
  return h;
}
// difference of two headings in [-0.5, 0.5)
static double HeadDiff(double a) {
  a = std::fmod(a, 1.0);
  if (a >= 0.5) a -= 1.0;
  if (a < -0.5) a += 1.0;
  return a;
}

// World coordinates are metres of the (scaled) game world. The game's own GPS / odometer show
// distances multiplied by the map scale (1:19 outside cities), so show the same kind of number.
static std::string DistStr(double worldMetres) {
  double m = worldMetres * 19.0;
  if (m < 1000.0) return Fmt("~%.0f m", m);
  if (m < 100000.0) return Fmt("~%.1f km", m / 1000.0);
  return Fmt("~%.0f km", m / 1000.0);
}

static std::string AgoStr(double s) {
  if (s < 3) return "şimdi";
  if (s < 60) return Fmt("%d sn önce", (int)s);
  if (s < 3600) return Fmt("%d dk önce", (int)(s / 60));
  return Fmt("%d sa önce", (int)(s / 3600));
}

// Keep names short, single line and free of protocol / markdown characters.
static std::string SanitizeName(const std::string& in) {
  std::string out;
  for (unsigned char c : in) {
    if (c < 0x20 || c == '#' || c == '|' || c == '*' || c == '_' || c == '`' || c == '[' || c == ']') c = ' ';
    if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
    out.push_back((char)c);
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  if (out.size() > 24) {
    size_t cut = 24;
    while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80) --cut;  // stay on a UTF-8 boundary
    out.resize(cut);
  }
  if (out.empty()) out = "Oyuncu";
  return out;
}

static std::vector<std::string> SplitWs(const std::string& s) {
  std::vector<std::string> v;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == ' ') ++i;
    if (i >= s.size()) break;
    size_t j = s.find(' ', i);
    if (j == std::string::npos) j = s.size();
    v.push_back(s.substr(i, j - i));
    i = j;
  }
  return v;
}

// =====================================================================================
// SEH-protected memory helpers (no C++ objects inside these functions!)
// =====================================================================================
static bool SafeReadPtr(const void* addr, void** out) {
  __try {
    *out = *(void* const*)addr;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *out = nullptr;
    return false;
  }
}

static int SafeCopyStr(const char* src, char* dst, int cap) {
  __try {
    int i = 0;
    for (; i < cap - 1 && src[i]; ++i) dst[i] = src[i];
    dst[i] = 0;
    return i;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    dst[0] = 0;
    return -1;
  }
}

// Temporarily replaces / restores the 5-byte "call rel32" that makes the teleport cheat show a
// loading screen. Only touched for the duration of one synchronous cheat call on the game thread.
static bool PatchBytes(uint8_t* at, const uint8_t* bytes, size_t n) {
  DWORD old = 0;
  if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
  __try {
    for (size_t i = 0; i < n; ++i) at[i] = bytes[i];
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    DWORD tmp = 0;
    VirtualProtect(at, n, old, &tmp);
    return false;
  }
  DWORD tmp = 0;
  VirtualProtect(at, n, old, &tmp);
  FlushInstructionCache(GetCurrentProcess(), at, n);
  return true;
}

static bool SafeSteamPersona(char* out, int cap) {
  out[0] = 0;
  __try {
    HMODULE h = GetModuleHandleW(L"steam_api64.dll");
    if (!h) return false;
    typedef void* (*GetFriends_t)();
    typedef const char* (*GetName_t)(void*);
    GetName_t getName = (GetName_t)GetProcAddress(h, "SteamAPI_ISteamFriends_GetPersonaName");
    if (!getName) return false;
    const char* versions[] = {"SteamAPI_SteamFriends_v017", "SteamAPI_SteamFriends_v018", "SteamAPI_SteamFriends_v016", "SteamAPI_SteamFriends_v015"};
    for (int i = 0; i < 4; ++i) {
      GetFriends_t f = (GetFriends_t)GetProcAddress(h, versions[i]);
      if (!f) continue;
      void* friends = f();
      if (!friends) continue;
      const char* n = getName(friends);
      if (n && *n) {
        int k = 0;
        for (; k < cap - 1 && n[k]; ++k) out[k] = n[k];
        out[k] = 0;
        return true;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out[0] = 0;
  }
  return false;
}

// =====================================================================================
// Convoy chat transport
// =====================================================================================
static void* ChatManager() {
  if (g.ppGlobal && g.chatOffset) {
    void* gm = nullptr;
    if (!SafeReadPtr(g.ppGlobal, &gm) || !gm) return nullptr;
    void* cm = nullptr;
    if (!SafeReadPtr((const char*)gm + g.chatOffset, &cm)) return nullptr;
    return cm;
  }
  return g.capturedChat;  // fallback when the global could not be located
}

static void Detour_AddToChat(void* cm, uint64_t sender, const char** pp, uint64_t type) {
  void* text = nullptr;
  if (pp && SafeReadPtr(pp, &text) && text) {
    char buf[256];
    int n = SafeCopyStr((const char*)text, buf, (int)sizeof(buf));
    if (n >= (int)kProtoLen && memcmp(buf, kProto, kProtoLen) == 0) {
      {
        std::lock_guard<std::mutex> lk(g.inMutex);
        if (g.inbox.size() < 256) g.inbox.push_back({(uint16_t)(sender & 0xFFFF), std::string(buf, (size_t)n)});
      }
      return;  // our protocol line: never shown in the chat window
    }
  }
  if (g.o_AddToChat) g.o_AddToChat(cm, sender, pp, type);
}

static void Detour_ChatInput(void* cm, const char** pp, uint64_t type, uint64_t broadcast) {
  if (cm) g.capturedChat = cm;
  if (g.o_ChatInput) g.o_ChatInput(cm, pp, type, broadcast);
}

static bool InConvoy() { return ChatManager() != nullptr && g.o_ChatInput != nullptr; }

static bool SendNow(const std::string& line) {
  void* cm = ChatManager();
  if (!cm || !g.o_ChatInput || line.size() >= 199) return false;
  const char* p = line.c_str();
  g.o_ChatInput(cm, &p, 1, 1);  // type 1 = normal player message, broadcast to the session
  return true;
}

static void Send(const std::string& body) {
  if (!InConvoy()) return;
  if (g.outbox.size() > 40) g.outbox.pop_front();
  g.outbox.push_back(std::string(kProto) + body);
}

static std::string SpotStr(const Spot& s) {
  if (!s.valid) return "- - - -";
  return Fmt("%.2f %.2f %.2f %.4f", s.x, s.y, s.z, Wrap01(s.h));
}

static Spot ParseSpot(const std::vector<std::string>& t, size_t i) {
  Spot s;
  if (t.size() < i + 4 || t[i] == "-") return s;
  s.x = strtod(t[i].c_str(), nullptr);
  s.y = strtod(t[i + 1].c_str(), nullptr);
  s.z = strtod(t[i + 2].c_str(), nullptr);
  s.h = Wrap01(strtod(t[i + 3].c_str(), nullptr));
  s.valid = std::isfinite(s.x) && std::isfinite(s.y) && std::isfinite(s.z) && !(s.x == 0 && s.y == 0 && s.z == 0);
  return s;
}

static std::string MyName() { return SanitizeName(g.nameBuf[0] ? std::string(g.nameBuf) : g.autoName); }

static uint32_t MyFlags() {
  uint32_t f = 0;
  if (g.pullPolicy == PULL_ALL) f |= 1;
  if (g.pullPolicy == PULL_HOST) f |= 2;
  if (g.localId == 1) f |= 4;
  if (g.me.valid) f |= 8;
  return f;
}

static Spot BehindPoint(double dist, bool allowFallback);

static void SendPresence() {
  Spot back = BehindPoint(kBehind, false);
  Send(Fmt("P %s %u ", g.iid.c_str(), MyFlags()) + SpotStr(g.me) + " " + SpotStr(back) + " " + MyName());
}

static void SendQuery() {
  if (!InConvoy()) return;
  Send(Fmt("Q %s", g.iid.c_str()));
  g.lastQuery = Now();
  g.pendingPAt = Now();  // also tell others where we are
}

static std::string PeerName(const std::string& iid) {
  auto it = g.peers.find(iid);
  return it != g.peers.end() ? it->second.name : std::string("Bir oyuncu");
}

// =====================================================================================
// Telemetry + trail ("breadcrumbs" of good on-road positions)
// =====================================================================================
// Which surface types (the game's "substances") are off-road. Re-read now and then; the list is tiny.
static void UpdateSurfaceTypes(const SPF_Telemetry_API* t, double now) {
  if (!t->Tel_GetCommonData || now - g.substCheckedAt < 10.0) return;
  g.substCheckedAt = now;
  static SPF_CommonData cd;  // ~4 KB, kept off the stack
  memset(&cd, 0, sizeof(cd));
  t->Tel_GetCommonData(g.tel, &cd, sizeof(cd));
  uint64_t mask = 0;
  uint32_t n = std::min<uint32_t>(cd.substance_count, 64);
  for (uint32_t i = 0; i < n; ++i) {
    char name[SPF_TELEMETRY_ID_MAX_SIZE];
    memcpy(name, cd.substances[i], sizeof(name));
    name[sizeof(name) - 1] = 0;
    for (char* c = name; *c; ++c) *c = (char)std::tolower((unsigned char)*c);
    if (strstr(name, "road")) continue;  // road, road_snow, ...
    if (strstr(name, "grass") || strstr(name, "dirt") || strstr(name, "sand") || strstr(name, "mud") || strstr(name, "snow"))
      mask |= 1ull << i;
  }
  g.offroadSubst = mask;
}

// A crash (skid/spin, roll-over, flying) just happened: the last moments before it are not safe road
// positions either (whatever we hit is still there), so they are dropped from the trail.
static void MarkCrash(double now, const char* why) {
  if (now - g.crashT > 3.0) Logf(SPF_LOG_INFO, "kaza algılandı (%s)", why);
  g.crashT = now;
  g.crashOdo = g.odo;
  for (auto it = g.trail.rbegin(); it != g.trail.rend(); ++it) {
    if (it->seg != g.seg) break;
    if (now - it->t > 1.2 && g.odo - it->odo > 8.0) break;
    it->good = false;
  }
}

static void ReadTelemetry(bool force) {
  double now = Now();
  if (!force && now - g.lastTel < 0.1) return;
  g.lastTel = now;
  const SPF_Telemetry_API* t = g.core ? g.core->telemetry : nullptr;
  if (!t || !g.tel) return;

  SPF_TruckData td;
  memset(&td, 0, sizeof(td));
  t->Tel_GetTruckData(g.tel, &td, sizeof(td));
  SPF_GameState gs;
  memset(&gs, 0, sizeof(gs));
  t->Tel_GetGameState(g.tel, &gs, sizeof(gs));
  g.paused = gs.paused;

  const SPF_DPlacement& wp = td.world_placement;
  Spot s;
  s.x = wp.position.x;
  s.y = wp.position.y;
  s.z = wp.position.z;
  s.h = Wrap01(wp.orientation.heading);
  s.valid = std::isfinite(s.x) && std::isfinite(s.z) && !(s.x == 0 && s.y == 0 && s.z == 0);
  g.me = s;
  g.meSpeed = td.speed;
  g.mePitch = wp.orientation.pitch;
  g.meRoll = wp.orientation.roll;
  if (!s.valid) return;

  // teleport / jump detection (> ~120 m/s apparent speed), and the direction the truck actually moved since
  // the previous read (~0.1 s ago) compared with where it points: the skid angle, in turns (0.25 = 90 deg)
  double readDiff = 0.0;
  if (g.prevTelT >= 0 && g.prevTel.valid) {
    double dt = std::max(0.05, now - g.prevTelT);
    double d = Dist(s, g.prevTel);
    if (d > 60.0 && d / dt > 120.0) {
      g.jumpPending = true;
    } else if (now - g.prevTelT < 0.5 && Dist2D(s, g.prevTel) > 0.5) {
      double mh = Wrap01(std::atan2(-(s.x - g.prevTel.x), -(s.z - g.prevTel.z)) / (2.0 * kPi));
      readDiff = std::fabs(HeadDiff(mh - s.h));
    }
  }
  g.prevTel = s;
  g.prevTelT = now;

  if (g.paused) return;
  UpdateSurfaceTypes(t, now);

  // ---- what the truck is doing right now ----
  // Every extra signal below is only used when the game actually provides it, so missing or odd data
  // can never stop the trail from being recorded.
  int onGround = 0, onDirt = 0;
  for (int i = 0; i < SPF_TELEMETRY_WHEEL_MAX_COUNT; ++i) {
    const SPF_WheelData& w = td.wheels[i];
    if (!w.on_ground) continue;
    ++onGround;
    if (w.substance < 64 && ((g.offroadSubst >> w.substance) & 1)) ++onDirt;
  }
  if (onGround > 0) g.wheelInfo = true;
  bool air = g.wheelInfo && onGround == 0;
  g.airReads = air ? g.airReads + 1 : 0;
  bool flying = g.airReads >= 3;  // a real flight, not a bump
  bool offRoad = onGround > 0 && onDirt * 2 > onGround;  // most wheels on grass / dirt / sand
  // skid angle from the truck's own velocity (vehicle space: -Z forward, +X right), in turns (0.25 = 90 deg);
  // trusted only when it agrees with the speedometer
  double spd = std::fabs(g.meSpeed);
  double fwd = -td.local_linear_velocity.z, lat = td.local_linear_velocity.x;
  double vh = std::sqrt(fwd * fwd + lat * lat);
  double slip = -1.0;
  if (vh > 1.5 && vh > spd * 0.5 && vh < spd * 2.0 + 1.0) slip = std::atan2(std::fabs(lat), fwd) / (2.0 * kPi);
  double tilt = std::max(std::fabs(g.mePitch), std::fabs(g.meRoll));
  bool forward = g.meSpeed > 3.0;  // reversing is not skidding
  // skidding right now (either signal); > ~40 deg while driving forward = a crash, not a manoeuvre
  double skid = std::max(readDiff, slip);
  const char* crash = nullptr;
  if (flying) crash = "havada";
  else if (tilt > 0.05) crash = "devrilme";
  else if (forward && skid > 0.11) crash = "savrulma";

  // ---- a trail sample every 3 m ----
  bool sample = false;
  double moveDiff = 0.0;
  if (g.jumpPending) {
    g.seg++;
    g.jumpPending = false;
    sample = true;
  } else if (g.trail.empty()) {
    sample = true;
  } else {
    const TrailPt& last = g.trail.back();
    double d = Dist(s, last.x, last.y, last.z);
    if (d >= 3.0) {
      sample = true;
      if (last.seg == g.seg) {
        g.odo += d;
        // direction of travel vs. where the truck points. Normal curves and lane changes stay well below
        // 10 deg between samples 3 m apart; more than ~16 deg = skidding, more than ~40 deg = spinning.
        double moveH = Wrap01(std::atan2(-(s.x - last.x), -(s.z - last.z)) / (2.0 * kPi));
        moveDiff = std::fabs(HeadDiff(moveH - s.h));
        if (!crash && forward && moveDiff > 0.11) crash = "savrulma";
      }
    }
  }
  if (crash && now >= g.calmUntil) MarkCrash(now, crash);
  if (!sample) return;
  bool good = g.meSpeed > 1.0 && tilt < 0.035 && moveDiff <= 0.045 && skid <= 0.045 && !air && !offRoad &&
              now - g.crashT > 0.5;
  g.trail.push_back({now, s.x, s.y, s.z, s.h, g.odo, g.seg, good});
  while (g.trail.size() > 4000) g.trail.pop_front();
}

// A point `dist` metres behind me along the road I came from (on-road, correct heading & height).
static Spot BehindPoint(double dist, bool allowFallback) {
  Spot r;
  if (!g.me.valid) return r;
  if (!g.trail.empty()) {
    const TrailPt& last = g.trail.back();
    double cur = g.odo + (last.seg == g.seg ? Dist(g.me, last.x, last.y, last.z) : 0.0);
    for (auto it = g.trail.rbegin(); it != g.trail.rend(); ++it) {
      if (it->seg != g.seg) break;
      if (cur - it->odo >= dist && it->good) {
        r.x = it->x;
        r.y = it->y + 0.3;
        r.z = it->z;
        r.h = it->h;
        r.valid = true;
        return r;
      }
    }
  }
  if (!allowFallback) return r;
  double th = g.me.h * 2.0 * kPi;  // forward = (-sin, 0, -cos)
  r.x = g.me.x + std::sin(th) * dist;
  r.z = g.me.z + std::cos(th) * dist;
  r.y = g.me.y + 1.0;
  r.h = g.me.h;
  r.valid = true;
  return r;
}

static Spot BehindOf(const Peer& p) {
  if (p.back.valid) return p.back;
  Spot r;
  if (!p.pos.valid) return r;
  double th = p.pos.h * 2.0 * kPi;
  r.x = p.pos.x + std::sin(th) * kBehind;
  r.z = p.pos.z + std::cos(th) * kBehind;
  r.y = p.pos.y + 1.0;
  r.h = p.pos.h;
  r.valid = true;
  return r;
}

// Last good on-road position that is at least `seconds` old (or the oldest one we have).
static bool RewindSpot(double seconds, Spot& out) {
  double target = Now() - seconds;
  const TrailPt* oldest = nullptr;
  for (auto it = g.trail.rbegin(); it != g.trail.rend(); ++it) {
    if (!it->good) continue;
    oldest = &*it;
    if (it->t <= target) break;
  }
  if (!oldest) return false;
  out.x = oldest->x;
  out.y = oldest->y + 0.3;
  out.z = oldest->z;
  out.h = oldest->h;
  out.valid = true;
  return true;
}

// =====================================================================================
// Settings & saved places
// =====================================================================================
static void LoadSettings() {
  const SPF_Config_API* c = g.load ? g.load->config : nullptr;
  if (!c || !g.cfg) return;
  c->Cfg_GetString(g.cfg, "settings.name", "", g.nameBuf, (int)sizeof(g.nameBuf));
  g.pullPolicy = (int)c->Cfg_GetInt32(g.cfg, "settings.pull_policy", PULL_HOST);
  if (g.pullPolicy < 0 || g.pullPolicy > 2) g.pullPolicy = PULL_HOST;
  g.restoreDev = c->Cfg_GetBool(g.cfg, "settings.restore_dev", true);
  g.headingSign = (int)c->Cfg_GetInt32(g.cfg, "settings.heading_sign", 0);
  if (g.headingSign != 1 && g.headingSign != -1) g.headingSign = 0;
}

// SPF calls OnSettingChanged synchronously from inside Cfg_Set*; without this guard the
// callback would reload the not-yet-written values and silently undo the change.
static bool g_savingSettings = false;

static void SaveSettings() {
  const SPF_Config_API* c = g.core ? g.core->config : (g.load ? g.load->config : nullptr);
  if (!c || !g.cfg) return;
  const std::string name = g.nameBuf;
  const int policy = g.pullPolicy, sign = g.headingSign;
  const bool restore = g.restoreDev;
  g_savingSettings = true;
  c->Cfg_SetString(g.cfg, "settings.name", name.c_str());
  c->Cfg_SetInt32(g.cfg, "settings.pull_policy", policy);
  c->Cfg_SetBool(g.cfg, "settings.restore_dev", restore);
  c->Cfg_SetInt32(g.cfg, "settings.heading_sign", sign);
  c->Cfg_Save(g.cfg);
  g_savingSettings = false;
}

static std::wstring DllDir() {
  HMODULE hm = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&DllDir, &hm);
  wchar_t path[MAX_PATH] = L"";
  GetModuleFileNameW(hm, path, MAX_PATH);
  std::wstring p(path);
  size_t s = p.find_last_of(L"\\/");
  return s == std::wstring::npos ? std::wstring(L".") : p.substr(0, s);
}

static void LoadPlaces() {
  g.places.clear();
  FILE* f = _wfopen(g.placesPath.c_str(), L"rb");
  if (!f) return;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    std::string l(line);
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
    if (l.size() >= 3 && (unsigned char)l[0] == 0xEF && (unsigned char)l[1] == 0xBB && (unsigned char)l[2] == 0xBF) l = l.substr(3);
    if (l.empty() || l[0] == '#') continue;
    std::vector<std::string> parts;
    size_t i = 0;
    while (true) {
      size_t j = l.find('|', i);
      parts.push_back(l.substr(i, j == std::string::npos ? std::string::npos : j - i));
      if (j == std::string::npos) break;
      i = j + 1;
    }
    if (parts.size() < 4) continue;
    Place p;
    p.name = SanitizeName(parts[0]);
    p.s.x = strtod(parts[1].c_str(), nullptr);
    p.s.y = strtod(parts[2].c_str(), nullptr);
    p.s.z = strtod(parts[3].c_str(), nullptr);
    p.s.h = parts.size() > 4 ? Wrap01(strtod(parts[4].c_str(), nullptr)) : 0.0;
    p.s.valid = true;
    g.places.push_back(p);
  }
  fclose(f);
}

static void SavePlaces() {
  FILE* f = _wfopen(g.placesPath.c_str(), L"wb");
  if (!f) {
    Notify(SPF_NOTIFICATION_ERROR, "yerler.txt yazılamadı.");
    return;
  }
  fputs("# KonvoyTP kayitli yerler: ad|x|y|z|yon(0-1)\n", f);
  for (const Place& p : g.places) fprintf(f, "%s|%.2f|%.2f|%.2f|%.4f\n", p.name.c_str(), p.s.x, p.s.y, p.s.z, p.s.h);
  fclose(f);
}

// =====================================================================================
// Teleport execution (cheat teleport via SPF GameConsole, executed on the game frame)
// =====================================================================================
static std::string TeleportCmd(const Spot& s) {
  int sign = g.headingSign == -1 ? -1 : 1;
  double a = sign * Wrap01(s.h) * 2.0 * kPi;
  if (a > kPi) a -= 2.0 * kPi;
  return Fmt("cheat teleport %.3f;%.3f;%.3f;%.5f;0", s.x, s.y, s.z, a);
}

static bool Exec(const char* cmd, char* err, int errSize) {
  const SPF_GameConsole_API* con = g.core ? g.core->console : nullptr;
  if (!con || !con->GCon_ExecuteCommandEx) {
    if (err && errSize > 0) snprintf(err, (size_t)errSize, "konsol API yok");
    return false;
  }
  if (err && errSize > 0) err[0] = 0;
  bool ok = con->GCon_ExecuteCommandEx(cmd, err, errSize) != 0;
  Logf(ok ? SPF_LOG_INFO : SPF_LOG_WARN, "komut: %s -> %s %s", cmd, ok ? "OK" : "HATA", (!ok && err) ? err : "");
  return ok;
}

// Runs "cheat teleport". Nearby destinations are already streamed in, so the game's loading screen is
// skipped (instant, like the developer-camera teleport). Far ones keep it so the area can load safely.
static bool ExecTeleport(const Spot& dest, char* err, int errSize) {
  std::string cmd = TeleportCmd(dest);
  uint8_t saved[5] = {};
  bool instant = g.loadCallSite && g.me.valid && Dist(g.me, dest) < 800.0 && g.loadCallSite[0] == 0xE8;
  if (instant) {
    memcpy(saved, g.loadCallSite, 5);
    static const uint8_t kNop5[5] = {0x0F, 0x1F, 0x44, 0x00, 0x00};  // 5-byte NOP
    instant = PatchBytes(g.loadCallSite, kNop5, 5);
  }
  bool ok = Exec(cmd.c_str(), err, errSize);
  if (instant) PatchBytes(g.loadCallSite, saved, 5);
  return ok;
}

static bool StartJob(const Spot& dest, const std::string& label, const std::string& replyIid, bool checkHeading) {
  if (g.job.active) {
    if (!replyIid.empty()) Send(Fmt("R %s %s ERR", g.iid.c_str(), replyIid.c_str()));
    Notify(SPF_NOTIFICATION_WARNING, "Zaten bir ışınlanma sürüyor, biraz bekle.");
    return false;
  }
  if (!g.me.valid) {
    if (!replyIid.empty()) Send(Fmt("R %s %s ERR", g.iid.c_str(), replyIid.c_str()));
    Notify(SPF_NOTIFICATION_ERROR, "Kamyonun konumu okunamadı. Sürüş ekranında (kamyonda) olmalısın.");
    return false;
  }
  if (!dest.valid || !std::isfinite(dest.x) || !std::isfinite(dest.y) || !std::isfinite(dest.z)) {
    if (!replyIid.empty()) Send(Fmt("R %s %s ERR", g.iid.c_str(), replyIid.c_str()));
    Notify(SPF_NOTIFICATION_ERROR, "Geçersiz hedef konum.");
    return false;
  }
  TpJob j;
  j.active = true;
  j.dest = dest;
  j.label = label;
  j.replyIid = replyIid;
  j.checkHeading = checkHeading;
  j.nextTry = std::max(Now(), g.lastJobEnd + 0.8);
  j.deadline = j.nextTry + 4.0;
  j.before = g.me;
  g.job = j;
  return true;
}

static void FinishJob(bool ok, const std::string& err) {
  TpJob& j = g.job;
  const SPF_GameConsole_API* con = g.core ? g.core->console : nullptr;
  if (j.devEnabledByUs && g.restoreDev && con) con->GCon_ExecuteCommand("g_developer 0");
  if (!j.replyIid.empty()) Send(Fmt("R %s %s %s", g.iid.c_str(), j.replyIid.c_str(), ok ? "OK" : "ERR"));
  if (ok) {
    if (j.before.valid && Dist(j.before, j.dest) > 5.0) g.undoSpot = j.before;
    if (j.endsCrash) g.crashT = -1e9;
    Notify(SPF_NOTIFICATION_SUCCESS, j.label);
    if (InConvoy()) g.pendingPAt = Now() + 1.0;  // tell friends our new position
  } else {
    g.lastError = err;
    Notify(SPF_NOTIFICATION_ERROR, "Işınlanma olmadı: " + err);
  }
  j.active = false;
  g.lastJobEnd = Now();
}

static void VerifyJob() {
  TpJob& j = g.job;
  ReadTelemetry(true);
  if (!g.me.valid) {
    FinishJob(true, "");
    return;
  }
  double moved = Dist(g.me, j.before);
  double toDest = Dist2D(g.me, j.dest);
  if (toDest > 60.0 && moved < 1.0 && Dist2D(j.before, j.dest) > 60.0) {
    FinishJob(false, "oyun ışınlanmayı uygulamadı");
    return;
  }
  // One-time heading convention check (only informative headings, i.e. not ~north/south).
  if (j.checkHeading && g.headingSign == 0 && j.fixAttempts == 0 && toDest < 30.0) {
    double req = Wrap01(j.dest.h), act = g.me.h;
    if (std::fabs(std::sin(2.0 * kPi * req)) > 0.35) {
      if (std::fabs(HeadDiff(act - req)) < 0.04) {
        g.headingSign = 1;
        SaveSettings();
        Logf(SPF_LOG_INFO, "yön kalibrasyonu: normal");
      } else if (std::fabs(HeadDiff(act + req)) < 0.04) {
        g.headingSign = -1;
        SaveSettings();
        Logf(SPF_LOG_WARN, "yön kalibrasyonu: ters - düzeltiliyor");
        j.fixAttempts = 1;
        char err[256];
        ExecTeleport(j.dest, err, (int)sizeof(err));
        g.calmUntil = Now() + 2.5;
        j.verifyAt = Now() + 0.6;
        return;
      }
    }
  }
  FinishJob(true, "");
}

static void ProcessJob() {
  TpJob& j = g.job;
  if (!j.active) return;
  double now = Now();
  const SPF_GameConsole_API* con = g.core ? g.core->console : nullptr;
  if (!con) {
    FinishJob(false, "SPF GameConsole kancası kapalı");
    return;
  }
  if (j.verifying) {
    if (now >= j.verifyAt) VerifyJob();
    return;
  }
  if (now < j.nextTry) return;
  if (now > j.deadline) {
    FinishJob(false, "geliştirici modu (g_developer) açılamadı");
    return;
  }
  if (!j.devEnabledByUs && !con->GCon_IsCheatsEnabled()) {
    con->GCon_EnableCheats();  // queued "g_developer 1"; executed by the game on the next frames
    j.devEnabledByUs = true;
    j.nextTry = now + 0.1;
    return;
  }
  char err[256] = "";
  bool ok = ExecTeleport(j.dest, err, (int)sizeof(err));
  if (!ok) {
    if (strstr(err, "disabled") && j.tries < 30) {  // g_developer not applied yet -> retry next frames
      j.tries++;
      j.nextTry = now + 0.1;
      return;
    }
    FinishJob(false, std::string("oyun reddetti (") + err + ")");
    return;
  }
  // the truck drops onto the spot: not a crash. A real move also starts a new trail segment.
  g.calmUntil = now + 2.5;
  if (!g.me.valid || Dist(g.me, j.dest) > 5.0) g.jumpPending = true;
  j.verifying = true;
  j.verifyAt = now + 0.6;
}

// =====================================================================================
// Actions
// =====================================================================================
static bool CanPull(const Peer& p) {
  if (p.flags & 1) return true;                       // accepts everyone
  if ((p.flags & 2) && g.localId == 1) return true;   // accepts host and I am the host
  return false;
}

static void StartGotoPeer(const Peer& p) {
  Spot d = BehindOf(p);
  if (!d.valid) {
    Notify(SPF_NOTIFICATION_ERROR, p.name + " konumunu henüz paylaşmadı (sürüş ekranında değil olabilir).");
    return;
  }
  StartJob(d, p.name + " yanına ışınlandın.", "", true);
}

static void ActionGotoPeer(const std::string& iid) {
  auto it = g.peers.find(iid);
  if (it == g.peers.end()) return;
  if (Now() - it->second.lastSeen < 2.0) {
    StartGotoPeer(it->second);
    return;
  }
  g.gotoReq.active = true;
  g.gotoReq.iid = iid;
  g.gotoReq.deadline = Now() + 3.5;
  SendQuery();
  Notify(SPF_NOTIFICATION_INFO, it->second.name + " konumu soruluyor...");
}

static void ActionBring(const std::vector<std::string>& iids) {
  int k = 0;
  for (const std::string& iid : iids) {
    auto it = g.peers.find(iid);
    if (it == g.peers.end() || !CanPull(it->second)) continue;
    ++k;
    Spot d = BehindPoint(kBehind * k, true);
    if (!d.valid) break;
    Send(Fmt("B %s %s ", g.iid.c_str(), iid.c_str()) + SpotStr(d));
  }
  if (k == 0)
    Notify(SPF_NOTIFICATION_WARNING, "Çekilebilecek oyuncu yok (izin vermiyorlar ya da konvoy sahibi değilsin).");
  else
    Notify(SPF_NOTIFICATION_INFO, Fmt("%d oyuncu yanına çağrıldı...", k));
}

static void ActionBringAll() {
  std::vector<std::string> v;
  for (auto& kv : g.peers)
    if (Now() - kv.second.lastSeen < 600 && CanPull(kv.second)) v.push_back(kv.first);
  ActionBring(v);
}

static void ActionUpright() {
  Spot d = g.me;
  d.y += 0.6;
  StartJob(d, "Kamyon düzeltildi.", "", true);
}

// ---- "Yola çek" / "Kendi şeridine geç" -------------------------------------------------
// Both use the trail of good road positions recorded while the player was driving normally
// (upright, moving forward, not skidding). Those points are always on the road, in the lane the
// player was driving in, with the correct direction - unlike the game's own F7 recovery, which
// gives up when the truck is no longer on a road item.
// Nearest good road point within maxDist. Shortly after a crash, points recorded before it come first: wherever
// the truck slid, flew or was driven to afterwards (wrong lane, a field), it goes back to where it drove properly.
static int NearestTrailIdx(double maxDist) {
  bool afterCrash = Now() - g.crashT < 120.0 && g.odo - g.crashOdo < 150.0;
  for (int pass = afterCrash ? 0 : 1; pass < 2; ++pass) {
    int best = -1;
    double bestD = maxDist;
    for (int i = (int)g.trail.size() - 1; i >= 0; --i) {
      const TrailPt& p = g.trail[(size_t)i];
      if (!p.good || (pass == 0 && p.t >= g.crashT)) continue;
      double d = Dist(g.me, p.x, p.y, p.z);
      if (d < bestD) {
        bestD = d;
        best = i;
      }
    }
    if (best >= 0) return best;
  }
  return -1;
}

static Spot TrailSpot(const TrailPt& p, double lift) {
  Spot s;
  s.x = p.x;
  s.y = p.y + lift;
  s.z = p.z;
  s.h = p.h;
  s.valid = true;
  return s;
}

// Yola çek: back onto the road, ~15 m behind the closest recorded road point (away from whatever we hit).
static void ActionRoad() {
  if (!g.me.valid) {
    Notify(SPF_NOTIFICATION_ERROR, "Kamyonun konumu okunamadı. Sürüş ekranında olmalısın.");
    return;
  }
  int n = NearestTrailIdx(400.0);
  if (n < 0) {
    Spot d = g.me;  // nothing recorded nearby yet: at least put the truck back on its wheels
    d.y += 0.6;
    StartJob(d, "Yakında kayıtlı yol yoktu; kamyon olduğu yerde düzeltildi.", "", true);
    return;
  }
  const TrailPt& closest = g.trail[(size_t)n];
  int b = n;
  for (int i = n; i >= 0; --i) {
    const TrailPt& p = g.trail[(size_t)i];
    if (p.seg != closest.seg) break;
    if (!p.good) continue;
    b = i;
    if (closest.odo - p.odo >= 15.0) break;
  }
  if (StartJob(TrailSpot(g.trail[(size_t)b], 0.3), "Yola çekildin.", "", true)) g.job.endsCrash = true;
}

// Kendi şeridine geç: back into the lane we were driving in, facing forward, at the closest point where we were
// still driving properly (after a crash: a moment before it, so not on top of whatever we hit).
static void ActionLane() {
  if (!g.me.valid) {
    Notify(SPF_NOTIFICATION_ERROR, "Kamyonun konumu okunamadı. Sürüş ekranında olmalısın.");
    return;
  }
  int n = NearestTrailIdx(80.0);
  if (n < 0) {
    Notify(SPF_NOTIFICATION_WARNING, "Yakında kayıtlı şerit yok. \"Yola çek\"i dene.");
    return;
  }
  const TrailPt& p = g.trail[(size_t)n];
  // already on that line, facing the same way? (samples are a few metres apart, so compare sideways offset)
  double th = p.h * 2.0 * kPi;
  double fx = -std::sin(th), fz = -std::cos(th);  // forward of the recorded point
  double ox = g.me.x - p.x, oz = g.me.z - p.z;
  double along = ox * fx + oz * fz, side = std::fabs(ox * fz - oz * fx);
  if (side < 1.2 && std::fabs(along) < 12.0 && std::fabs(g.me.y - p.y) < 2.0 && std::fabs(HeadDiff(g.me.h - p.h)) < 0.05) {
    Notify(SPF_NOTIFICATION_INFO, "Zaten kendi şeridindesin.");
    return;
  }
  if (StartJob(TrailSpot(p, 0.3), "Kendi şeridine geçtin.", "", true)) g.job.endsCrash = true;
}

static void OnHotkeyRoad() {
  g.roadKeyAt = Now();
  g.toggleAt = -1.0;  // Shift+F8 was the "Yola çek" chord, not the menu key
  if (!g.job.active) ActionRoad();
}

static void ActionRewind(int seconds) {
  Spot d;
  if (!RewindSpot(seconds, d)) {
    Notify(SPF_NOTIFICATION_WARNING, "Henüz kayıtlı yol konumu yok (biraz sür, sonra tekrar dene).");
    return;
  }
  if (StartJob(d, seconds < 60 ? Fmt("%d sn önceki konumuna döndün.", seconds) : Fmt("%d dk önceki konumuna döndün.", seconds / 60), "", true))
    g.job.endsCrash = true;
}

static void ActionUndo() {
  if (!g.undoSpot.valid) return;
  Spot d = g.undoSpot;
  d.y += 0.3;
  StartJob(d, "Son ışınlanmadan önceki yerine döndün.", "", true);
}

static void ActionCoords() {
  std::string s(g.coordBuf);
  for (char& ch : s)
    if (ch == ';' || ch == '\t') ch = ' ';
  std::vector<std::string> t = SplitWs(s);
  if (t.size() < 3) {
    Notify(SPF_NOTIFICATION_WARNING, "Koordinatı 'X Y Z' şeklinde yaz.");
    return;
  }
  Spot d;
  d.x = strtod(t[0].c_str(), nullptr);
  d.y = strtod(t[1].c_str(), nullptr);
  d.z = strtod(t[2].c_str(), nullptr);
  d.h = t.size() > 3 ? Wrap01(strtod(t[3].c_str(), nullptr)) : g.me.h;
  d.valid = true;
  StartJob(d, "Koordinata ışınlandın.", "", false);
}

// =====================================================================================
// Inbound protocol
// =====================================================================================
static void HandleMsg(uint16_t sender, const std::string& text) {
  std::vector<std::string> t = SplitWs(text);
  if (t.size() < 3) return;
  const std::string& kind = t[1];
  const std::string& from = t[2];
  if (from == g.iid) {  // our own line echoed back locally -> transport works, learn our player id
    if (g.localId != (int)sender) Logf(SPF_LOG_INFO, "konvoy bağlantısı doğrulandı (oyuncu kimliği %u%s)", sender, sender == 1 ? ", host" : "");
    g.localId = sender;
    return;
  }
  Logf(SPF_LOG_INFO, "gelen: %c from=%s sender=%u", kind.empty() ? '?' : kind[0], from.c_str(), sender);
  double now = Now();

  if (kind == "Q") {
    if (g.pendingPAt < 0) g.pendingPAt = now + 0.15 + (double)(rand() % 400) / 1000.0;
    return;
  }
  if (kind == "P" && t.size() >= 12) {
    for (auto it = g.peers.begin(); it != g.peers.end();) {  // same player restarted the game -> new id
      if (it->first != from && it->second.sender == sender)
        it = g.peers.erase(it);
      else
        ++it;
    }
    Peer& p = g.peers[from];
    p.iid = from;
    p.sender = sender;
    p.flags = (uint32_t)strtoul(t[3].c_str(), nullptr, 10);
    p.pos = ParseSpot(t, 4);
    p.back = ParseSpot(t, 8);
    std::string name;
    for (size_t i = 12; i < t.size(); ++i) name += (i > 12 ? " " : "") + t[i];
    p.name = SanitizeName(name);
    p.lastSeen = now;
    if (g.gotoReq.active && g.gotoReq.iid == from) {
      g.gotoReq.active = false;
      StartGotoPeer(p);
    }
    return;
  }
  if (kind == "B" && t.size() >= 8) {
    if (t[3] != g.iid && t[3] != "*") return;
    Spot d = ParseSpot(t, 4);
    std::string who = PeerName(from);
    bool allowed = g.pullPolicy == PULL_ALL || (g.pullPolicy == PULL_HOST && sender == 1);
    if (!allowed) {
      Send(Fmt("R %s %s NO", g.iid.c_str(), from.c_str()));
      Notify(SPF_NOTIFICATION_WARNING, who + " seni yanına çekmek istedi; ayarların izin vermediği için reddedildi.");
      return;
    }
    if (!d.valid) return;
    Notify(SPF_NOTIFICATION_INFO, who + " seni yanına çekiyor...");
    StartJob(d, who + " seni yanına çekti.", from, true);
    return;
  }
  if (kind == "R" && t.size() >= 5) {
    if (t[3] != g.iid) return;
    std::string who = PeerName(from);
    if (t[4] == "OK")
      Notify(SPF_NOTIFICATION_SUCCESS, who + " yanına geldi.");
    else if (t[4] == "NO")
      Notify(SPF_NOTIFICATION_WARNING, who + " çekilmeye izin vermiyor (kendi Ayarlar sekmesinden açabilir).");
    else
      Notify(SPF_NOTIFICATION_ERROR, who + " ışınlanamadı (oyunu izin vermedi).");
  }
}

// =====================================================================================
// UI - sade, Apple tarzi, kendi renk semasi.
// Sadece pencere aciksa cizilir; birkac dikdortgen + yazi, animasyon/golge yok (cok hafif).
// =====================================================================================
namespace T {
struct C {
  float r, g, b, a;
};
static const C kCard{0.110f, 0.110f, 0.118f, 0.96f};  // #1C1C1E kart
static const C kCell{0.173f, 0.173f, 0.180f, 1.0f};   // #2C2C2E hucre
static const C kCellHov{0.212f, 0.212f, 0.220f, 1.0f};
static const C kCellAct{0.255f, 0.255f, 0.267f, 1.0f};
static const C kSegSel{0.388f, 0.388f, 0.400f, 1.0f};  // #636366 secili segment
static const C kLine{1.0f, 1.0f, 1.0f, 0.07f};
static const C kText{0.961f, 0.961f, 0.969f, 1.0f};
static const C kText2{0.557f, 0.557f, 0.576f, 1.0f};  // #8E8E93
static const C kAccent{1.0f, 0.624f, 0.039f, 1.0f};   // #FF9F0A kehribar (marka rengi)
static const C kAccentHov{1.0f, 0.698f, 0.231f, 1.0f};
static const C kAccentAct{0.902f, 0.553f, 0.024f, 1.0f};
static const C kOnAccent{0.110f, 0.078f, 0.016f, 1.0f};
static const C kBlue{0.039f, 0.518f, 1.0f, 1.0f};    // #0A84FF
static const C kGray{0.388f, 0.388f, 0.400f, 1.0f};  // #636366
static const C kGreen{0.188f, 0.820f, 0.345f, 1.0f};  // #30D158
static const C kYellow{1.0f, 0.839f, 0.039f, 1.0f};   // #FFD60A
static const C kRed{1.0f, 0.271f, 0.227f, 1.0f};      // #FF453A
static const C kClear{0.0f, 0.0f, 0.0f, 0.0f};
static const float kW = 364.0f;  // icerik genisligi
}  // namespace T

static int g_tab = 0;

static uint32_t U32(const T::C& c, float alphaMul = 1.0f) {
  auto ch = [](float v) { return (uint32_t)(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
  return ch(c.r) | (ch(c.g) << 8) | (ch(c.b) << 16) | (ch(c.a * alphaMul) << 24);
}
static void PushCol(SPF_UI_API* ui, SPF_StyleColor idx, const T::C& c) { ui->UI_PushStyleColor(idx, c.r, c.g, c.b, c.a); }
static float TextW(SPF_UI_API* ui, const char* s) {
  float w = 0, h = 0;
  ui->UI_CalcTextSize(s, &w, &h);
  return w;
}
static void Cursor(SPF_UI_API* ui, float& x, float& y) { ui->UI_GetCursorScreenPos(&x, &y); }
static void Advance(SPF_UI_API* ui, float x, float y, float w = T::kW) {  // move layout cursor below custom-drawn content
  ui->UI_SetCursorScreenPos(x, y);
  ui->UI_Dummy(w, 0.0f);
}

enum BtnKind { BTN_PRIMARY, BTN_SECONDARY, BTN_GHOST, BTN_DANGER };

static bool Btn(SPF_UI_API* ui, const char* label, BtnKind kind, float w, float h) {
  T::C bg = T::kCellHov, hov = T::kCellAct, act = T::kSegSel, fg = T::kText;
  if (kind == BTN_PRIMARY) {
    bg = T::kAccent;
    hov = T::kAccentHov;
    act = T::kAccentAct;
    fg = T::kOnAccent;
  } else if (kind == BTN_GHOST) {
    bg = T::kClear;
    hov = T::kCellHov;
    act = T::kCellAct;
    fg = T::kText2;
  } else if (kind == BTN_DANGER) {
    bg = T::kClear;
    hov = {1.0f, 0.271f, 0.227f, 0.16f};
    act = {1.0f, 0.271f, 0.227f, 0.28f};
    fg = T::kRed;
  }
  PushCol(ui, SPF_COLOR_BUTTON, bg);
  PushCol(ui, SPF_COLOR_BUTTON_HOVERED, hov);
  PushCol(ui, SPF_COLOR_BUTTON_ACTIVE, act);
  PushCol(ui, SPF_COLOR_TEXT, fg);
  ui->UI_PushStyleVarFloat(SPF_STYLE_VAR_FRAME_ROUNDING, h > 0.0f ? h * 0.5f : 9.0f);
  bool r = ui->UI_Button(label, w, h);
  ui->UI_PopStyleVar(1);
  ui->UI_PopStyleColor(4);
  return r;
}

static void Caption(SPF_UI_API* ui, const char* text) {
  PushCol(ui, SPF_COLOR_TEXT, T::kText2);
  ui->UI_TextWrapped(text);
  ui->UI_PopStyleColor(1);
}

static void Section(SPF_UI_API* ui, const char* title) {
  float x, y;
  ui->UI_Dummy(0.0f, 2.0f);
  Cursor(ui, x, y);
  ui->UI_SetCursorScreenPos(x + 6.0f, y);
  PushCol(ui, SPF_COLOR_TEXT, T::kText2);
  ui->UI_Text(title);
  ui->UI_PopStyleColor(1);
}

// iOS "inset grouped" list row background (+ hairline separator between rows)
static void RowBg(SPF_UI_API* ui, float x, float y, float w, float h, bool first, bool last, const T::C& c) {
  SPF_DrawList_Handle dl = ui->UI_GetWindowDrawList();
  int f = (first ? SPF_DRAW_FLAG_ROUND_CORNERS_TOP : 0) | (last ? SPF_DRAW_FLAG_ROUND_CORNERS_BOTTOM : 0);
  if (!first && !last) f = SPF_DRAW_FLAG_ROUND_CORNERS_NONE;
  ui->UI_DrawList_AddRectFilled(dl, x, y, x + w, y + h, U32(c), 12.0f, (SPF_DrawFlags)f);
  if (!first) ui->UI_DrawList_AddLine(dl, x + 14.0f, y, x + w, y, U32(T::kLine), 1.0f);
}

// Segmented control (Apple style). Returns the selected index.
static int Segmented(SPF_UI_API* ui, const char* id, const char* const* labels, int n, int cur, float width, float h) {
  float x, y;
  Cursor(ui, x, y);
  SPF_DrawList_Handle dl = ui->UI_GetWindowDrawList();
  ui->UI_DrawList_AddRectFilled(dl, x, y, x + width, y + h, U32(T::kCell), 9.0f, SPF_DRAW_FLAG_ROUND_CORNERS_ALL);
  float segW = width / (float)n, lh = ui->UI_GetTextLineHeight();
  int result = cur;
  for (int i = 0; i < n; ++i) {
    float sx = x + segW * (float)i;
    ui->UI_SetCursorScreenPos(sx, y);
    ui->UI_PushID_Int(i);
    if (ui->UI_InvisibleButton(id, segW, h)) result = i;
    bool hov = ui->UI_IsItemHovered(SPF_HOVERED_FLAG_NONE);
    ui->UI_PopID();
    if (i == cur)
      ui->UI_DrawList_AddRectFilled(dl, sx + 2.0f, y + 2.0f, sx + segW - 2.0f, y + h - 2.0f, U32(T::kSegSel), 7.0f, SPF_DRAW_FLAG_ROUND_CORNERS_ALL);
    else if (hov)
      ui->UI_DrawList_AddRectFilled(dl, sx + 2.0f, y + 2.0f, sx + segW - 2.0f, y + h - 2.0f, U32(T::kCellHov), 7.0f, SPF_DRAW_FLAG_ROUND_CORNERS_ALL);
    float tw = TextW(ui, labels[i]);
    ui->UI_DrawList_AddText(dl, sx + (segW - tw) * 0.5f, y + (h - lh) * 0.5f, U32(i == cur ? T::kText : T::kText2), labels[i]);
  }
  Advance(ui, x, y + h, width);
  return result;
}

// iOS style switch
static bool Toggle(SPF_UI_API* ui, const char* id, bool* v, float x, float y) {
  const float w = 46.0f, h = 28.0f;
  ui->UI_SetCursorScreenPos(x, y);
  bool clicked = ui->UI_InvisibleButton(id, w, h);
  if (clicked) *v = !*v;
  SPF_DrawList_Handle dl = ui->UI_GetWindowDrawList();
  ui->UI_DrawList_AddRectFilled(dl, x, y, x + w, y + h, U32(*v ? T::kGreen : T::kCellAct), h * 0.5f, SPF_DRAW_FLAG_ROUND_CORNERS_ALL);
  float r = h * 0.5f - 3.0f, cx = *v ? x + w - h * 0.5f : x + h * 0.5f;
  ui->UI_DrawList_AddCircleFilled(dl, cx, y + h * 0.5f, r, 0xFFFFFFFFu, 24);
  return clicked;
}

// Big tappable row: colored icon tile + title + subtitle + chevron (like iOS Settings).
static bool ActionRow(SPF_UI_API* ui, const char* id, const char* icon, const T::C& tile, const char* title, const char* sub, float x, float& y, bool first,
                      bool last, bool enabled) {
  const float w = T::kW, h = 58.0f, lh = ui->UI_GetTextLineHeight();
  ui->UI_SetCursorScreenPos(x, y);
  ui->UI_BeginDisabled(!enabled);
  bool clicked = ui->UI_InvisibleButton(id, w, h);
  bool hov = enabled && ui->UI_IsItemHovered(SPF_HOVERED_FLAG_NONE);
  bool act = enabled && ui->UI_IsItemActive();
  ui->UI_EndDisabled();
  RowBg(ui, x, y, w, h, first, last, act ? T::kCellAct : (hov ? T::kCellHov : T::kCell));
  SPF_DrawList_Handle dl = ui->UI_GetWindowDrawList();
  float a = enabled ? 1.0f : 0.38f, s = 32.0f, ix = x + 12.0f, iy = y + (h - s) * 0.5f;
  ui->UI_DrawList_AddRectFilled(dl, ix, iy, ix + s, iy + s, U32(tile, a), 8.0f, SPF_DRAW_FLAG_ROUND_CORNERS_ALL);
  ui->UI_DrawList_AddText(dl, ix + (s - TextW(ui, icon)) * 0.5f, iy + (s - lh) * 0.5f, U32(T::kText, a), icon);
  float tx = ix + s + 12.0f, chevronX = x + w - 14.0f - TextW(ui, ICON_FA_CHEVRON_RIGHT);
  ui->UI_DrawList_PushClipRect(dl, tx, y, chevronX - 6.0f, y + h, true);
  ui->UI_DrawList_AddText(dl, tx, y + 9.0f, U32(T::kText, a), title);
  ui->UI_DrawList_AddText(dl, tx, y + 10.0f + lh, U32(T::kText2, a), sub);
  ui->UI_DrawList_PopClipRect(dl);
  ui->UI_DrawList_AddText(dl, chevronX, y + (h - lh) * 0.5f, U32(T::kText2, a * 0.7f), ICON_FA_CHEVRON_RIGHT);
  y += h;
  return clicked && enabled;
}

static void EmptyState(SPF_UI_API* ui, const char* icon, const char* title, const char* body) {
  float x, y, wx, wy;
  Cursor(ui, x, y);
  ui->UI_GetWindowPos(&wx, &wy);
  const float h = 118.0f, lh = ui->UI_GetTextLineHeight();
  RowBg(ui, x, y, T::kW, h, true, true, T::kCell);
  SPF_DrawList_Handle dl = ui->UI_GetWindowDrawList();
  ui->UI_DrawList_AddText(dl, x + (T::kW - TextW(ui, icon)) * 0.5f, y + 16.0f, U32(T::kText2), icon);
  ui->UI_DrawList_AddText(dl, x + (T::kW - TextW(ui, title)) * 0.5f, y + 22.0f + lh, U32(T::kText), title);
  float bw = TextW(ui, body);
  if (bw <= T::kW - 32.0f) {
    ui->UI_DrawList_AddText(dl, x + (T::kW - bw) * 0.5f, y + 28.0f + lh * 2.0f, U32(T::kText2), body);
  } else {
    ui->UI_SetCursorScreenPos(x + 16.0f, y + 28.0f + lh * 2.0f);
    ui->UI_PushTextWrapPos(x + T::kW - 16.0f - wx);
    Caption(ui, body);
    ui->UI_PopTextWrapPos();
  }
  Advance(ui, x, y + h);
}

// ---- Header: title, close button, status line --------------------------------------
static void Header(SPF_UI_API* ui) {
  float x, y;
  Cursor(ui, x, y);
  SPF_DrawList_Handle dl = ui->UI_GetWindowDrawList();
  SPF_Font_Handle bold = ui->UI_GetFont("bold");
  if (bold) ui->UI_PushFont(bold);
  float lh = ui->UI_GetTextLineHeight();
  ui->UI_DrawList_AddText(dl, x + 2.0f, y, U32(T::kAccent), ICON_FA_TRUCK);
  ui->UI_DrawList_AddText(dl, x + 2.0f + TextW(ui, ICON_FA_TRUCK) + 9.0f, y, U32(T::kText), "Konvoy Işınlanma");
  if (bold) ui->UI_PopFont();

  // close (x) button
  const float cs = 26.0f;
  float bx = x + T::kW - cs, by = y + (lh - cs) * 0.5f;
  ui->UI_SetCursorScreenPos(bx, by);
  bool close = ui->UI_InvisibleButton("##ktp_close", cs, cs);
  bool hov = ui->UI_IsItemHovered(SPF_HOVERED_FLAG_NONE);
  ui->UI_DrawList_AddCircleFilled(dl, bx + cs * 0.5f, by + cs * 0.5f, cs * 0.5f, U32(hov ? T::kCellAct : T::kCell), 24);
  float rlh = ui->UI_GetTextLineHeight();
  ui->UI_DrawList_AddText(dl, bx + (cs - TextW(ui, ICON_FA_XMARK)) * 0.5f, by + (cs - rlh) * 0.5f, U32(hov ? T::kText : T::kText2), ICON_FA_XMARK);
  if (close && g.win) ui->UI_SetVisibility(g.win, false);

  // status line
  bool inConvoy = InConvoy();
  T::C dot = T::kText2;
  std::string s;
  if (g.job.active) {
    dot = T::kAccent;
    s = "Işınlanıyor…";
  } else if (g.gotoReq.active) {
    dot = T::kAccent;
    s = "Arkadaşının konumu bekleniyor…";
  } else if (!g.me.valid) {
    s = "Kamyona geçmen bekleniyor";
  } else if (!inConvoy) {
    s = "Tek başına · konvoy yok";
  } else if (g.localId >= 0) {
    dot = T::kGreen;
    s = g.localId == 1 ? "Konvoyda · bağlantı tamam · Host" : "Konvoyda · bağlantı tamam";
  } else if (g.chatSince >= 0 && Now() - g.chatSince < 15.0) {
    dot = T::kYellow;
    s = "Konvoyda · bağlantı kontrol ediliyor…";
  } else {
    dot = T::kRed;
    s = "Konvoy sohbetine ulaşılamadı";
  }
  float sy = y + lh + 6.0f;
  ui->UI_DrawList_AddCircleFilled(dl, x + 7.0f, sy + rlh * 0.5f, 4.0f, U32(dot), 16);
  ui->UI_DrawList_AddText(dl, x + 18.0f, sy, U32(T::kText2), s.c_str());
  Advance(ui, x, sy + rlh + 4.0f);
}

// ---- Tabs ----------------------------------------------------------------------------
static void RenderConvoyTab(SPF_UI_API* ui) {
  double now = Now();
  if (!InConvoy()) {
    EmptyState(ui, ICON_FA_PEOPLE_GROUP, "Konvoyda değilsin", "Bir konvoya girince arkadaşların burada görünür.");
    ui->UI_Dummy(0.0f, 2.0f);
    Caption(ui, "Kurtar ve Yerler sekmeleri tek başınayken de çalışır.");
    return;
  }

  // section title + refresh on the right
  float x, y;
  ui->UI_Dummy(0.0f, 2.0f);
  Cursor(ui, x, y);
  PushCol(ui, SPF_COLOR_TEXT, T::kText2);
  ui->UI_SetCursorScreenPos(x + 6.0f, y + 4.0f);
  ui->UI_Text("ARKADAŞLAR");
  ui->UI_PopStyleColor(1);
  ui->UI_SetCursorScreenPos(x + T::kW - 96.0f, y);
  if (Btn(ui, ICON_FA_ARROWS_ROTATE " Yenile", BTN_GHOST, 96.0f, 28.0f)) SendQuery();
  Advance(ui, x, y + 32.0f);

  std::vector<const Peer*> list;
  for (auto& kv : g.peers) list.push_back(&kv.second);
  std::sort(list.begin(), list.end(), [](const Peer* a, const Peer* b) { return a->name < b->name; });

  if (list.empty()) {
    EmptyState(ui, ICON_FA_USER_GROUP, "Henüz kimse görünmüyor", "Arkadaşlarında da KonvoyTP kurulu olmalı.");
  } else {
    Cursor(ui, x, y);
    SPF_DrawList_Handle dl = ui->UI_GetWindowDrawList();
    const float h = 56.0f, lh = ui->UI_GetTextLineHeight();
    bool busy = g.job.active || g.gotoReq.active;
    for (size_t i = 0; i < list.size(); ++i) {
      const Peer* p = list[i];
      RowBg(ui, x, y, T::kW, h, i == 0, i + 1 == list.size(), T::kCell);
      bool pull = CanPull(*p);
      float bx = x + T::kW - 12.0f;
      ui->UI_PushID_Int((int)i);
      ui->UI_BeginDisabled(busy);
      if (pull) {
        bx -= 70.0f;
        ui->UI_SetCursorScreenPos(bx, y + (h - 30.0f) * 0.5f);
        if (Btn(ui, "Getir", BTN_PRIMARY, 70.0f, 30.0f)) ActionBring({p->iid});
        bx -= 8.0f;
      }
      bx -= 56.0f;
      ui->UI_SetCursorScreenPos(bx, y + (h - 30.0f) * 0.5f);
      if (Btn(ui, "Git", BTN_SECONDARY, 56.0f, 30.0f)) ActionGotoPeer(p->iid);
      ui->UI_EndDisabled();
      ui->UI_PopID();

      // name (+ crown for the host) and details, clipped before the buttons
      ui->UI_DrawList_PushClipRect(dl, x, y, bx - 8.0f, y + h, true);
      float tx = x + 14.0f, ty = y + 9.0f;
      if (p->flags & 4) {
        ui->UI_DrawList_AddText(dl, tx, ty, U32(T::kYellow), ICON_FA_CROWN);
        tx += TextW(ui, ICON_FA_CROWN) + 7.0f;
      }
      ui->UI_DrawList_AddText(dl, tx, ty, U32(T::kText), p->name.c_str());
      std::string sub = (g.me.valid && p->pos.valid) ? DistStr(Dist(g.me, p->pos)) : std::string("konum yok");
      sub += " · " + AgoStr(now - p->lastSeen);
      ui->UI_DrawList_AddText(dl, x + 14.0f, ty + lh + 2.0f, U32(T::kText2), sub.c_str());
      ui->UI_DrawList_PopClipRect(dl);
      y += h;
    }
    Advance(ui, x, y);
  }

  bool anyPull = false;
  for (auto& kv : g.peers)
    if (CanPull(kv.second)) anyPull = true;
  if (anyPull) {
    ui->UI_Dummy(0.0f, 4.0f);
    ui->UI_BeginDisabled(g.job.active);
    if (Btn(ui, ICON_FA_PEOPLE_GROUP "  Herkesi yanıma topla", BTN_PRIMARY, T::kW, 40.0f)) ActionBringAll();
    ui->UI_EndDisabled();
  }
  Caption(ui, g.localId == 1 ? "Konvoyun sahibisin: izin veren herkesi yanına çekebilirsin."
                             : "\"Git\" ile yanına gidersin. \"Getir\" sadece izin verenlerde çıkar.");
}

static void RenderRescueTab(SPF_UI_API* ui) {
  bool ok = !g.job.active && g.me.valid;
  float x, y;
  ui->UI_Dummy(0.0f, 2.0f);
  Cursor(ui, x, y);
  if (ActionRow(ui, "##ktp_road", ICON_FA_ROAD, T::kAccent, "Yola çek", "Yoldan çıktıysan yola geri koyar", x, y, true, false, ok)) ActionRoad();
  if (ActionRow(ui, "##ktp_lane", ICON_FA_ARROWS_LEFT_RIGHT, T::kBlue, "Kendi şeridine geç", "Karşı şeride düştüysen geri alır", x, y, false, false, ok))
    ActionLane();
  if (ActionRow(ui, "##ktp_up", ICON_FA_WRENCH, T::kGray, "Kamyonu düzelt", "Devrildiysen olduğun yerde düz koyar", x, y, false, false, ok)) ActionUpright();
  if (ActionRow(ui, "##ktp_undo", ICON_FA_ROTATE_LEFT, T::kGray, "Son ışınlanmayı geri al", "Önceki yerine döner", x, y, false, true, ok && g.undoSpot.valid))
    ActionUndo();
  Advance(ui, x, y);
  if (g.kb && g.core && g.core->keybinds && g.core->keybinds->Kbind_GetBindingDisplayName) {
    char key[64] = "";
    g.core->keybinds->Kbind_GetBindingDisplayName(g.kb, "Rescue.road", 0, key, (int)sizeof(key));
    if (key[0]) Caption(ui, (std::string("Kısayol: ") + key + " = Yola çek (menüyü açmadan)").c_str());
  }

  Section(ui, "GERİ SAR");
  static const char* kLabels[] = {"10 sn", "30 sn", "1 dk", "3 dk"};
  static const int kSecs[] = {10, 30, 60, 180};
  float bw = (T::kW - 3.0f * 8.0f) / 4.0f;
  ui->UI_BeginDisabled(!ok);
  for (int i = 0; i < 4; ++i) {
    if (i) ui->UI_SameLine(0.0f, 8.0f);
    if (Btn(ui, kLabels[i], BTN_SECONDARY, bw, 36.0f)) ActionRewind(kSecs[i]);
  }
  ui->UI_EndDisabled();
  Caption(ui, g.me.valid ? "Kazadan önce yolda düzgün gittiğin son noktaya döner." : "Önce kamyona geç.");
}

// ---- Toasts: our own notifications, drawn on top of the game (only while one is showing) ----
static void RenderToasts(SPF_UI_API* ui, void* /*user_data*/) {
  ui->UI_Dummy(1.0f, 1.0f);
  if (g.toasts.empty()) return;
  float vx = 0, vy = 0, vw = 0, vh = 0;
  ui->UI_GetMainViewportPos(&vx, &vy);
  ui->UI_GetMainViewportSize(&vw, &vh);
  SPF_DrawList_Handle dl = ui->UI_GetForegroundDrawList();
  SPF_Font_Handle font = ui->UI_GetFont("medium");
  if (font) ui->UI_PushFont(font);
  const float h = 42.0f, lh = ui->UI_GetTextLineHeight();
  double now = Now();
  float y = vy + 64.0f;
  for (const auto& t : g.toasts) {
    double age = now - t.t0, left = t.life - age;
    float a = (float)std::clamp(std::min(age / 0.15, left / 0.35), 0.0, 1.0);
    if (a <= 0.0f) continue;
    float slide = (float)(1.0 - std::min(1.0, age / 0.22)) * -10.0f;  // small drop-in
    const char* icon = ICON_FA_CIRCLE_INFO;
    T::C ic = T::kAccent;
    if (t.kind == 1) {
      icon = ICON_FA_CIRCLE_CHECK;
      ic = T::kGreen;
    } else if (t.kind == 2) {
      icon = ICON_FA_TRIANGLE_EXCLAMATION;
      ic = T::kYellow;
    } else if (t.kind == 3) {
      icon = ICON_FA_CIRCLE_XMARK;
      ic = T::kRed;
    }
    float iw = TextW(ui, icon), tw = TextW(ui, t.text.c_str());
    float w = 16.0f + iw + 10.0f + tw + 20.0f;
    float x = vx + (vw - w) * 0.5f, yy = y + slide;
    ui->UI_DrawList_AddRectFilled(dl, x, yy, x + w, yy + h, U32(T::kCard, a), h * 0.5f, SPF_DRAW_FLAG_ROUND_CORNERS_ALL);
    ui->UI_DrawList_AddRect(dl, x + 0.5f, yy + 0.5f, x + w - 0.5f, yy + h - 0.5f, U32(T::kLine, 1.6f * a), h * 0.5f, SPF_DRAW_FLAG_ROUND_CORNERS_ALL, 1.0f);
    ui->UI_DrawList_AddText(dl, x + 16.0f, yy + (h - lh) * 0.5f, U32(ic, a), icon);
    ui->UI_DrawList_AddText(dl, x + 16.0f + iw + 10.0f, yy + (h - lh) * 0.5f, U32(T::kText, a), t.text.c_str());
    y += h + 8.0f;
  }
  if (font) ui->UI_PopFont();
}

static void RenderPlacesTab(SPF_UI_API* ui) {
  ui->UI_Dummy(0.0f, 2.0f);
  ui->UI_SetNextItemWidth(T::kW - 104.0f);
  ui->UI_InputTextWithHint("##ktp_pname", "Yer adı (örn. Buluşma)", g.placeName, sizeof(g.placeName), SPF_INPUT_TEXT_FLAG_NONE);
  ui->UI_SameLine(0.0f, 8.0f);
  ui->UI_BeginDisabled(!g.me.valid);
  if (Btn(ui, "Kaydet", BTN_PRIMARY, 96.0f, 0.0f)) {
    Place p;
    p.name = SanitizeName(g.placeName[0] ? std::string(g.placeName) : Fmt("Yer %d", (int)g.places.size() + 1));
    p.s = g.me;
    g.places.push_back(p);
    SavePlaces();
    g.placeName[0] = 0;
    Notify(SPF_NOTIFICATION_SUCCESS, p.name + " kaydedildi.");
  }
  ui->UI_EndDisabled();
  ui->UI_Dummy(0.0f, 2.0f);

  if (g.places.empty()) {
    EmptyState(ui, ICON_FA_LOCATION_DOT, "Kayıtlı yer yok", "Bir yerdeyken adını yazıp Kaydet'e bas.");
  } else {
    float x, y;
    Cursor(ui, x, y);
    SPF_DrawList_Handle dl = ui->UI_GetWindowDrawList();
    const float h = 52.0f, lh = ui->UI_GetTextLineHeight();
    int remove = -1;
    for (size_t i = 0; i < g.places.size(); ++i) {
      const Place& p = g.places[i];
      RowBg(ui, x, y, T::kW, h, i == 0, i + 1 == g.places.size(), T::kCell);
      float bx = x + T::kW - 12.0f - 34.0f;
      ui->UI_PushID_Int((int)i);
      ui->UI_SetCursorScreenPos(bx, y + (h - 30.0f) * 0.5f);
      if (Btn(ui, ICON_FA_TRASH_CAN, BTN_DANGER, 34.0f, 30.0f)) remove = (int)i;
      bx -= 8.0f + 56.0f;
      ui->UI_SetCursorScreenPos(bx, y + (h - 30.0f) * 0.5f);
      ui->UI_BeginDisabled(g.job.active || !g.me.valid);
      if (Btn(ui, "Git", BTN_SECONDARY, 56.0f, 30.0f)) {
        Spot d = p.s;
        d.y += 0.3;
        StartJob(d, p.name + " konumuna ışınlandın.", "", true);
      }
      ui->UI_EndDisabled();
      ui->UI_PopID();
      ui->UI_DrawList_PushClipRect(dl, x, y, bx - 8.0f, y + h, true);
      ui->UI_DrawList_AddText(dl, x + 14.0f, y + 7.0f, U32(T::kText), p.name.c_str());
      ui->UI_DrawList_AddText(dl, x + 14.0f, y + 8.0f + lh, U32(T::kText2), g.me.valid ? DistStr(Dist(g.me, p.s)).c_str() : "?");
      ui->UI_DrawList_PopClipRect(dl);
      y += h;
    }
    Advance(ui, x, y);
    if (remove >= 0) {
      g.places.erase(g.places.begin() + remove);
      SavePlaces();
    }
  }

  Section(ui, "KOORDİNAT");
  ui->UI_SetNextItemWidth(T::kW - 72.0f);
  ui->UI_InputTextWithHint("##ktp_coord", "X Y Z", g.coordBuf, sizeof(g.coordBuf), SPF_INPUT_TEXT_FLAG_NONE);
  ui->UI_SameLine(0.0f, 8.0f);
  ui->UI_BeginDisabled(g.job.active || !g.me.valid);
  if (Btn(ui, "Git##coord", BTN_SECONDARY, 64.0f, 0.0f)) ActionCoords();
  ui->UI_EndDisabled();
  if (g.me.valid) {
    std::string cur = Fmt("%.1f %.1f %.1f", g.me.x, g.me.y, g.me.z);
    PushCol(ui, SPF_COLOR_TEXT, T::kText2);
    ui->UI_AlignTextToFramePadding();
    ui->UI_Text(("Konumun: " + cur).c_str());
    ui->UI_PopStyleColor(1);
    ui->UI_SameLine(0.0f, 6.0f);
    if (Btn(ui, ICON_FA_COPY " Kopyala", BTN_GHOST, 0.0f, 0.0f)) ui->UI_SetClipboardText(cur.c_str());
  }
}

static void RenderSettingsTab(SPF_UI_API* ui) {
  Section(ui, "GÖRÜNEN AD");
  ui->UI_SetNextItemWidth(T::kW);
  ui->UI_InputTextWithHint("##ktp_name", g.autoName.c_str(), g.nameBuf, sizeof(g.nameBuf), SPF_INPUT_TEXT_FLAG_NONE);
  if (ui->UI_IsItemDeactivatedAfterEdit()) {
    SaveSettings();
    if (InConvoy()) g.pendingPAt = Now();
  }

  Section(ui, "BENİ KİM YANINA ÇEKEBİLİR");
  static const char* kPol[] = {"Sadece host", "Herkes", "Kimse"};
  int pol = Segmented(ui, "##ktp_pol", kPol, 3, g.pullPolicy, T::kW, 32.0f);
  if (pol != g.pullPolicy) {
    g.pullPolicy = pol;
    SaveSettings();
    if (InConvoy()) g.pendingPAt = Now();
  }

  Section(ui, "DİĞER");
  float x, y;
  Cursor(ui, x, y);
  SPF_DrawList_Handle dl = ui->UI_GetWindowDrawList();
  const float h = 48.0f, lh = ui->UI_GetTextLineHeight();
  RowBg(ui, x, y, T::kW, h, true, false, T::kCell);
  ui->UI_DrawList_AddText(dl, x + 14.0f, y + (h - lh) * 0.5f, U32(T::kText), "Işınlanınca geliştirici modunu kapat");
  if (Toggle(ui, "##ktp_dev", &g.restoreDev, x + T::kW - 12.0f - 46.0f, y + (h - 28.0f) * 0.5f)) SaveSettings();
  y += h;
  // key binding rows: label, current key, "Değiştir"
  struct KeyRow {
    const char* label;
    const char* action;
    const char* fallback;
  };
  static const KeyRow kKeys[] = {{"Menü tuşu", "MainWindow.toggle", "F8"}, {"Yola çek tuşu", "Rescue.road", "Shift+F8"}};
  for (int k = 0; k < 2; ++k) {
    RowBg(ui, x, y, T::kW, h, false, k == 1, T::kCell);
    char key[64] = "";
    if (g.kb && g.core && g.core->keybinds && g.core->keybinds->Kbind_GetBindingDisplayName)
      g.core->keybinds->Kbind_GetBindingDisplayName(g.kb, kKeys[k].action, 0, key, (int)sizeof(key));
    ui->UI_DrawList_AddText(dl, x + 14.0f, y + (h - lh) * 0.5f, U32(T::kText), kKeys[k].label);
    std::string keyText = key[0] ? key : kKeys[k].fallback;
    float bxw = 92.0f;
    ui->UI_DrawList_AddText(dl, x + T::kW - 12.0f - bxw - 10.0f - TextW(ui, keyText.c_str()), y + (h - lh) * 0.5f, U32(T::kText2), keyText.c_str());
    ui->UI_SetCursorScreenPos(x + T::kW - 12.0f - bxw, y + (h - 30.0f) * 0.5f);
    ui->UI_PushID_Int(k);
    if (Btn(ui, "Değiştir", BTN_SECONDARY, bxw, 30.0f) && g.kb && g.core->keybinds->Kbind_OpenRebindPopup)
      g.core->keybinds->Kbind_OpenRebindPopup(g.kb, kKeys[k].action, 0);
    ui->UI_PopID();
    y += h;
  }
  Advance(ui, x, y);

  ui->UI_Dummy(0.0f, 2.0f);
  if (ui->UI_CollapsingHeader("Teşhis bilgisi", SPF_TREE_NODE_FLAG_NONE)) {
    const SPF_GameConsole_API* con = g.core ? g.core->console : nullptr;
    PushCol(ui, SPF_COLOR_TEXT, T::kText2);
    ui->UI_Text(Fmt("Sohbet kancaları: gelen %s, giden %s", g.o_AddToChat ? "tamam" : "YOK", g.o_ChatInput ? "tamam" : "YOK").c_str());
    ui->UI_Text(Fmt("Konvoy sohbeti: %s  (adres %s, ofset 0x%X)", ChatManager() ? "var" : "yok", g.ppGlobal ? "bulundu" : "YOK", g.chatOffset).c_str());
    ui->UI_Text(Fmt("Oyuncu kimliği: %d   Eklenti kimliği: %s", g.localId, g.iid.c_str()).c_str());
    ui->UI_Text(Fmt("Geliştirici modu: %s   Yön ayarı: %s", (con && con->GCon_IsCheatsEnabled && con->GCon_IsCheatsEnabled()) ? "açık" : "kapalı",
                    g.headingSign == 1 ? "doğrulandı" : (g.headingSign == -1 ? "ters" : "test edilmedi"))
                    .c_str());
    if (g.me.valid) ui->UI_Text(Fmt("Konum: %.1f %.1f %.1f   yön %.3f", g.me.x, g.me.y, g.me.z, g.me.h).c_str());
    ui->UI_PopStyleColor(1);
    if (!g.lastError.empty()) ui->UI_TextColored(T::kRed.r, T::kRed.g, T::kRed.b, 1.0f, ("Son hata: " + g.lastError).c_str());
  }
}

static void RenderMain(SPF_UI_API* ui, void* /*user_data*/) {
  // --- card background (covers the whole window; the window itself has no background/title bar)
  float wx, wy, ww, wh;
  ui->UI_GetWindowPos(&wx, &wy);
  ui->UI_GetWindowSize(&ww, &wh);
  SPF_DrawList_Handle dl = ui->UI_GetWindowDrawList();
  ui->UI_DrawList_PushClipRectFullScreen(dl);
  ui->UI_DrawList_AddRectFilled(dl, wx, wy, wx + ww, wy + wh, U32(T::kCard), 16.0f, SPF_DRAW_FLAG_ROUND_CORNERS_ALL);
  ui->UI_DrawList_AddRect(dl, wx + 0.5f, wy + 0.5f, wx + ww - 0.5f, wy + wh - 0.5f, U32(T::kLine, 1.6f), 16.0f, SPF_DRAW_FLAG_ROUND_CORNERS_ALL, 1.0f);
  ui->UI_DrawList_PopClipRect(dl);

  // --- local style (popped at the end)
  ui->UI_PushStyleVarVec2(SPF_STYLE_VAR_ITEM_SPACING, 8.0f, 8.0f);
  ui->UI_PushStyleVarVec2(SPF_STYLE_VAR_FRAME_PADDING, 12.0f, 7.0f);
  ui->UI_PushStyleVarFloat(SPF_STYLE_VAR_FRAME_ROUNDING, 9.0f);
  ui->UI_PushStyleVarFloat(SPF_STYLE_VAR_FRAME_BORDERSIZE, 0.0f);
  PushCol(ui, SPF_COLOR_TEXT, T::kText);
  PushCol(ui, SPF_COLOR_TEXT_DISABLED, T::kText2);
  PushCol(ui, SPF_COLOR_FRAME_BG, T::kCell);
  PushCol(ui, SPF_COLOR_FRAME_BG_HOVERED, T::kCellHov);
  PushCol(ui, SPF_COLOR_FRAME_BG_ACTIVE, T::kCellAct);
  PushCol(ui, SPF_COLOR_CHECK_MARK, T::kAccent);
  PushCol(ui, SPF_COLOR_TEXT_SELECTED_BG, {1.0f, 0.624f, 0.039f, 0.35f});
  PushCol(ui, SPF_COLOR_HEADER, T::kCell);
  PushCol(ui, SPF_COLOR_HEADER_HOVERED, T::kCellHov);
  PushCol(ui, SPF_COLOR_HEADER_ACTIVE, T::kCellAct);
  PushCol(ui, SPF_COLOR_BUTTON, T::kCellHov);
  PushCol(ui, SPF_COLOR_BUTTON_HOVERED, T::kCellAct);
  PushCol(ui, SPF_COLOR_BUTTON_ACTIVE, T::kSegSel);
  PushCol(ui, SPF_COLOR_BORDER, T::kClear);
  PushCol(ui, SPF_COLOR_NAV_HIGHLIGHT, T::kAccent);
  const int kColors = 15;

  // --- comfortable inner padding regardless of the framework's window padding
  float cx, cy;
  Cursor(ui, cx, cy);
  float padL = cx - wx, inset = std::max(0.0f, 16.0f - padL);
  if (inset > 0.0f) ui->UI_Indent(inset);
  ui->UI_Dummy(T::kW + inset, std::max(0.0f, 12.0f - (cy - wy) - 8.0f));

  Header(ui);
  static const char* kTabs[] = {"Konvoy", "Kurtar", "Yerler", "Ayarlar"};
  g_tab = Segmented(ui, "##ktp_tabs", kTabs, 4, g_tab, T::kW, 32.0f);
  switch (g_tab) {
    case 0:
      RenderConvoyTab(ui);
      break;
    case 1:
      RenderRescueTab(ui);
      break;
    case 2:
      RenderPlacesTab(ui);
      break;
    default:
      RenderSettingsTab(ui);
      break;
  }
  ui->UI_Dummy(0.0f, 4.0f);
  if (inset > 0.0f) ui->UI_Unindent(inset);

  ui->UI_PopStyleColor(kColors);
  ui->UI_PopStyleVar(4);
}

static void ToggleMainWindow() {
  if (!g.ui || !g.win) return;
  bool vis = g.ui->UI_IsVisible(g.win);
  g.ui->UI_SetVisibility(g.win, !vis);
}

static void OnToggleMainWindow() {
  // With Shift held this F8 may be part of the Shift+F8 "Yola çek" chord: decide a moment later (OnUpdate)
  if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
    g.toggleAt = Now();
    return;
  }
  ToggleMainWindow();
}

// =====================================================================================
// Lifecycle
// =====================================================================================
static void BuildManifest(SPF_Manifest_Builder_Handle* h, const SPF_Manifest_Builder_API* api) {
  api->Info_SetName(h, PLUGIN_NAME);
  api->Info_SetVersion(h, PLUGIN_VERSION);
  api->Info_SetMinFrameworkVersion(h, "1.2.7");
  api->Info_SetAuthor(h, "KonvoyTP");
  api->Info_SetDescriptionLiteral(h,
                                  "Konvoyda arkadaşının yanına ışınlan, onu yanına çek, kayıtlı yerlere git, "
                                  "devrilen kamyonu düzelt, geri sar. Menü tuşu: F8");

  api->Policy_SetAllowUserConfig(h, true);
  api->Policy_AddConfigurableSystem(h, "logging");
  api->Policy_AddConfigurableSystem(h, "localization");  // loads localization/<lang>.json before the window is created
  api->Policy_AddRequiredHook(h, "GameConsole");
  api->Policy_AddRequiredHook(h, "PrismStringResolver");
  api->Policy_AddRequiredHook(h, "PrismArrayResolver");

  // All settings are edited from the plugin's own "Ayarlar" tab, so they are hidden in the SPF settings window.
  api->Settings_SetJson(h, R"json({ "name": "", "pull_policy": 0, "restore_dev": true, "heading_sign": 0 })json");
  api->Meta_AddCustomSetting(h, "name", nullptr, nullptr, nullptr, nullptr, true);
  api->Meta_AddCustomSetting(h, "pull_policy", nullptr, nullptr, nullptr, nullptr, true);
  api->Meta_AddCustomSetting(h, "restore_dev", nullptr, nullptr, nullptr, nullptr, true);
  api->Meta_AddCustomSetting(h, "heading_sign", nullptr, nullptr, nullptr, nullptr, true);

  api->Defaults_SetLogging(h, "info", true);
  api->Defaults_SetLocalization(h, "en");
  api->Defaults_AddKeybind(h, "MainWindow", "toggle", "keyboard", "KEY_F8", "always");
  api->Defaults_AddWindow(h, "MainWindow", false, true, 60, 140, 580, 560, false, false);
  // Texts come from localization/<lang>.json next to the DLL.
  api->Meta_AddWindow(h, "MainWindow", "MainWindow.title", "MainWindow.desc");
  api->Meta_AddKeybind(h, "MainWindow", "toggle", "keybinds.toggle.title", "keybinds.toggle.desc");
  // "Yola çek" without opening the menu
  api->Defaults_AddKeybind(h, "Rescue", "road", "chord", "keyboard:KEY_LSHIFT+keyboard:KEY_F8", "always");
  api->Meta_AddKeybind(h, "Rescue", "road", "keybinds.road.title", "keybinds.road.desc");
  // our own notifications: a non-interactive overlay window, only visible while a toast is showing
  api->Defaults_AddWindow(h, "Toast", false, false, 0, 0, 8, 8, false, false);
  api->Meta_AddWindow(h, "Toast", "Toast.title", "Toast.desc");
}

static void OnLoad(const SPF_Load_API* load_api) {
  g.load = load_api;
  if (!load_api) return;
  if (load_api->logger) g.log = load_api->logger->Log_GetContext(PLUGIN_NAME);
  if (load_api->config) g.cfg = load_api->config->Cfg_GetContext(PLUGIN_NAME);
  LoadSettings();
}

static void OnActivated(const SPF_Core_API* core_api) {
  g.core = core_api;
  if (!core_api) return;

  // identity
  {
    std::random_device rd;
    std::mt19937 rng(rd() ^ (uint32_t)GetTickCount() ^ (uint32_t)GetCurrentProcessId());
    g.iid = Fmt("%08x", (unsigned)rng());
    srand((unsigned)rng());
    char persona[128];
    if (SafeSteamPersona(persona, (int)sizeof(persona))) g.autoName = SanitizeName(persona);
    if (g.autoName.empty() || g.autoName == "Oyuncu") {
      wchar_t wn[128] = L"";
      DWORD n = 128;
      if (GetUserNameW(wn, &n)) g.autoName = SanitizeName(WideToUtf8(wn));
    }
    if (g.autoName.empty()) g.autoName = "Oyuncu";
  }

  g.placesPath = DllDir() + L"\\yerler.txt";
  LoadPlaces();

  if (core_api->keybinds) {
    g.kb = core_api->keybinds->Kbind_GetContext(PLUGIN_NAME);
    if (g.kb) {
      core_api->keybinds->Kbind_Register(g.kb, "MainWindow.toggle", OnToggleMainWindow);
      core_api->keybinds->Kbind_Register(g.kb, "Rescue.road", OnHotkeyRoad);
    }
  }
  if (core_api->telemetry) g.tel = core_api->telemetry->Tel_GetContext(PLUGIN_NAME);

  SPF_Hooks_API* hk = core_api->hooks;
  if (hk) {
    g.hookDisplay = hk->Hook_Register(PLUGIN_NAME, "KTP_ChatDisplay", "KonvoyTP: konvoy sohbeti (gelen)", (void*)&Detour_AddToChat, (void**)&g.o_AddToChat,
                                      "str:@@mp_chat_system@@", true);
    g.hookInput = hk->Hook_Register(PLUGIN_NAME, "KTP_ChatInput", "KonvoyTP: konvoy sohbeti (giden)", (void*)&Detour_ChatInput, (void**)&g.o_ChatInput,
                                    "str:@@mp_chat_no_admin@@", true);
    uintptr_t x = hk->Hook_FindFunctionByString("@@mp_leaving_private@@", false, nullptr, 0);
    if (x) {
      uintptr_t mov = hk->Hook_FindBackward(x, 16, "48 8B [05-3D]");
      if (mov) g.ppGlobal = (void**)hk->Memory_GetRipAddress(mov, 3, 7);
      uintptr_t cmp = hk->Hook_FindPatternFrom("48 39 [81-86]", x, 32);
      if (cmp) {
        int32_t off = hk->Memory_ReadInt32(cmp + 3);
        if (off > 0 && off < 0x10000) g.chatOffset = (uint32_t)off;
      }
    }
    // Teleport cheat: "...; mov rcx,[game]; lea r8,[rbp+x]; xor r9d,r9d; mov qword [rbp+x],0; lea edx,[r9+1]; call begin_loading"
    uintptr_t tp = hk->Hook_FindFunctionByString("Use: teleport X;Y;Z or goto X;Y;Z;A;E", false, nullptr, 0);
    if (tp) {
      uintptr_t site = hk->Hook_FindPatternFrom("48 8B 0D ?? ?? ?? ?? 4C 8D 45 ?? 45 33 C9 48 C7 45 ?? 00 00 00 00 41 8D 51 01 E8", tp, 0x600);
      if (site && hk->Memory_ReadInt8(site + 26) == (int8_t)0xE8) g.loadCallSite = (uint8_t*)(site + 26);
    }
    Logf(SPF_LOG_INFO, "kancalar: gelen=%s giden=%s global=%p ofset=0x%X hizli_isinlanma=%s", g.hookDisplay ? "ok" : "yok", g.hookInput ? "ok" : "yok",
         (void*)g.ppGlobal, g.chatOffset, g.loadCallSite ? "ok" : "yok");
  }
  Logf(SPF_LOG_INFO, "KonvoyTP %s hazır. ad=%s kimlik=%s yerler=%u", PLUGIN_VERSION, g.autoName.c_str(), g.iid.c_str(), (unsigned)g.places.size());
}

static void OnRegisterUI(SPF_UI_API* ui_api) {
  g.ui = ui_api;
  if (!ui_api) return;
  // No title bar / background: the window is drawn as our own rounded card (see RenderMain).
  ui_api->UI_RegisterDrawCallbackWithFlags(PLUGIN_NAME, "MainWindow", RenderMain, nullptr,
                                           (SPF_WindowFlags)(SPF_WINDOW_FLAG_NO_TITLE_BAR | SPF_WINDOW_FLAG_NO_BACKGROUND | SPF_WINDOW_FLAG_NO_SCROLLBAR |
                                                             SPF_WINDOW_FLAG_NO_COLLAPSE | SPF_WINDOW_FLAG_ALWAYS_AUTO_RESIZE |
                                                             SPF_WINDOW_FLAG_NO_DOCKING));
  g.win = ui_api->UI_GetWindowHandle(PLUGIN_NAME, "MainWindow");
  ui_api->UI_RegisterDrawCallbackWithFlags(
      PLUGIN_NAME, "Toast", RenderToasts, nullptr,
      (SPF_WindowFlags)(SPF_WINDOW_FLAG_NO_TITLE_BAR | SPF_WINDOW_FLAG_NO_BACKGROUND | SPF_WINDOW_FLAG_NO_SCROLLBAR | SPF_WINDOW_FLAG_NO_COLLAPSE |
                        SPF_WINDOW_FLAG_NO_MOUSE_INPUTS | SPF_WINDOW_FLAG_NO_FOCUS_ON_APPEARING | SPF_WINDOW_FLAG_NO_BRING_TO_FRONT_ON_FOCUS |
                        SPF_WINDOW_FLAG_NO_NAV_INPUTS | SPF_WINDOW_FLAG_NO_NAV_FOCUS | SPF_WINDOW_FLAG_NO_DOCKING | SPF_WINDOW_FLAG_ALWAYS_AUTO_RESIZE));
  g.toastWin = ui_api->UI_GetWindowHandle(PLUGIN_NAME, "Toast");
}

static void OnUpdate() {
  if (!g.core) return;
  double now = Now();

  ReadTelemetry(false);

  // menu key pressed with Shift and no "Yola çek" chord followed: it really was the menu key
  if (g.toggleAt >= 0 && now - g.toggleAt > 0.25) {
    g.toggleAt = -1.0;
    if (now - g.roadKeyAt > 0.6) ToggleMainWindow();
  }

  // convoy session tracking
  void* cm = ChatManager();
  if (cm != g.lastChat) {
    g.lastChat = cm;
    g.peers.clear();
    g.outbox.clear();
    g.localId = -1;
    g.announced = false;
    g.gotoReq.active = false;
    g.chatSince = cm ? now : -1;
    if (cm) Logf(SPF_LOG_INFO, "konvoy sohbeti bulundu");
  }
  if (cm && !g.announced && g.chatSince >= 0 && now - g.chatSince > 4.0 && g.o_ChatInput) {
    g.announced = true;
    SendQuery();  // learn who is around (also tells them about us)
  }

  // inbound
  std::vector<InMsg> in;
  {
    std::lock_guard<std::mutex> lk(g.inMutex);
    in.swap(g.inbox);
  }
  for (const InMsg& m : in) HandleMsg(m.sender, m.text);

  // our presence reply
  if (g.pendingPAt >= 0 && now >= g.pendingPAt) {
    g.pendingPAt = -1;
    SendPresence();
  }

  // outbound (rate limited)
  if (!g.outbox.empty() && now - g.lastSend >= 0.25) {
    if (cm) SendNow(g.outbox.front());
    g.outbox.pop_front();
    g.lastSend = now;
  }

  // waiting for a friend's fresh position
  if (g.gotoReq.active && now > g.gotoReq.deadline) {
    g.gotoReq.active = false;
    auto it = g.peers.find(g.gotoReq.iid);
    if (it != g.peers.end() && (it->second.pos.valid || it->second.back.valid)) {
      Notify(SPF_NOTIFICATION_WARNING, it->second.name + " cevap vermedi, son bilinen konumuna gidiliyor.");
      StartGotoPeer(it->second);
    } else {
      Notify(SPF_NOTIFICATION_ERROR, "Arkadaşından cevap gelmedi.");
    }
  }

  ProcessJob();

  // forget players we have not heard from for 15 minutes
  for (auto it = g.peers.begin(); it != g.peers.end();) {
    if (now - it->second.lastSeen > 900)
      it = g.peers.erase(it);
    else
      ++it;
  }

  // opening the menu refreshes the friend list
  if (g.ui && g.win) {
    bool vis = g.ui->UI_IsVisible(g.win);
    if (vis && !g.wasVisible && InConvoy() && now - g.lastQuery > 5.0) SendQuery();
    g.wasVisible = vis;
  }

  // toasts: drop expired ones; the overlay window is only visible while something is showing
  g.toasts.erase(std::remove_if(g.toasts.begin(), g.toasts.end(), [now](const Ctx::Toast& t) { return now - t.t0 > t.life; }), g.toasts.end());
  if (g.ui && g.toastWin) {
    bool want = !g.toasts.empty();
    if (g.ui->UI_IsVisible(g.toastWin) != want) g.ui->UI_SetVisibility(g.toastWin, want);
  }
}

static void ForgetCrash() {
  g.crashT = -1e9;
  g.airReads = 0;
  g.calmUntil = Now() + 5.0;  // the truck is placed into the world: not a crash
  g.substCheckedAt = -100;    // re-read the surface list for this world
}

static void OnGameWorldReady() {
  g.trail.clear();
  ForgetCrash();
  g.undoSpot.valid = false;
}

static void OnWorldUnloaded() {
  g.trail.clear();
  ForgetCrash();
  g.peers.clear();
  g.undoSpot.valid = false;
  g.job.active = false;
  g.gotoReq.active = false;
  g.me.valid = false;
}

static void OnSettingChanged(SPF_Config_Handle* /*h*/, const char* /*keyPath*/) {
  if (!g_savingSettings) LoadSettings();
}

static void OnUnload() {
  // Trampolines are intentionally kept: if a detour is still reachable it must keep forwarding to the game.
  g.peers.clear();
  g.trail.clear();
  g.outbox.clear();
  g.job.active = false;
  g.core = nullptr;
  g.ui = nullptr;
  g.win = nullptr;
}

extern "C" {
SPF_PLUGIN_EXPORT bool SPF_GetManifestAPI(SPF_Manifest_API* out_api) {
  if (!out_api) return false;
  out_api->BuildManifest = BuildManifest;
  return true;
}

SPF_PLUGIN_EXPORT bool SPF_GetPlugin(SPF_Plugin_Exports* exports) {
  if (!exports) return false;
  exports->OnLoad = OnLoad;
  exports->OnActivated = OnActivated;
  exports->OnUnload = OnUnload;
  exports->OnUpdate = OnUpdate;
  exports->OnRegisterUI = OnRegisterUI;
  exports->OnSettingChanged = OnSettingChanged;
  exports->OnGameWorldReady = OnGameWorldReady;
  exports->OnWorldUnloaded = OnWorldUnloaded;
  return true;
}
}

}  // namespace KonvoyTP

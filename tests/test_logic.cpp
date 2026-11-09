// Offline tests for KonvoyTP's convoy logic: protocol, permissions, trail, teleport jobs.
// The real plugin source is compiled together with fake SPF APIs (telemetry, console, chat).
#define KTP_TEST
#define PLUGIN_NAME "KonvoyTP"
#define PLUGIN_VERSION "test"
#include "../src/KonvoyTP.cpp"

#include <iostream>

using namespace KonvoyTP;
namespace KonvoyTP {
extern double g_testNow;
}

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, msg)                                       \
  do {                                                         \
    if (cond) {                                                \
      ++g_pass;                                                \
    } else {                                                   \
      ++g_fail;                                                \
      std::cout << "  FAIL: " << msg << "  (" #cond ")\n"; \
    }                                                          \
  } while (0)

// ---------------- fake game state ----------------
static SPF_DPlacement g_truck{};
static float g_speed = 0;
static std::vector<std::string> g_cmds, g_sent, g_shown;
static uint16_t g_myId = 3;
static int g_cheatsOnAfter = 0;  // number of "disabled" answers before cheats work

static void Tel_GetTruckData(SPF_Telemetry_Handle*, SPF_TruckData* out, size_t) {
  memset(out, 0, sizeof(*out));
  out->world_placement = g_truck;
  out->speed = g_speed;
}
static void Tel_GetGameState(SPF_Telemetry_Handle*, SPF_GameState* out, size_t) { memset(out, 0, sizeof(*out)); }

static int Con_IsCheats() { return g_cheatsOnAfter <= 0 ? 1 : 0; }
static void Con_Enable() {}
static void Con_Exec(const char* c) { g_cmds.push_back(c); }
static int Con_ExecEx(const char* c, char* err, int n) {
  g_cmds.push_back(c);
  if (g_cheatsOnAfter > 0) {
    --g_cheatsOnAfter;
    if (err) snprintf(err, (size_t)n, "cheat commands are disabled (g_developer <= 0 or manager null)");
    return 0;
  }
  double x, y, z, a;
  if (sscanf(c, "cheat teleport %lf;%lf;%lf;%lf;", &x, &y, &z, &a) == 4) {
    g_truck.position = {x, y - 0.2, z};  // physics settles a little
    double h = a / (2.0 * kPi);
    if (h < 0) h += 1.0;
    g_truck.orientation.heading = h;
    return 1;
  }
  return 0;
}
static void Log(SPF_Logger_Handle*, SPF_LogLevel, const char* m) { std::cout << "    log: " << m << "\n"; }

// fake chat: sending echoes locally (like the game does) and records the broadcast
static void FakeShow(void*, uint64_t, const char** pp, uint64_t) { g_shown.push_back(*pp); }
static void FakeInput(void* cm, const char** pp, uint64_t type, uint64_t) {
  g_sent.push_back(*pp);
  Detour_AddToChat(cm, g_myId, pp, type);
}
static void Incoming(uint16_t sender, const std::string& text) {
  const char* p = text.c_str();
  Detour_AddToChat((void*)0x1, sender, &p, 1);
}
static void Tick(double dt = 0.1) {
  g_testNow += dt;
  OnUpdate();
}
static void RunUntilIdle(int maxTicks = 200) {
  for (int i = 0; i < 3; ++i) Tick(0.1);  // let OnUpdate drain the inbox first
  for (int i = 0; i < maxTicks && (g.job.active || !g.outbox.empty() || g.pendingPAt >= 0); ++i) Tick(0.1);
}
static bool SentStartsWith(const std::string& prefix) {
  for (auto& s : g_sent)
    if (s.rfind(prefix, 0) == 0) return true;
  return false;
}
static std::string LastCmd() { return g_cmds.empty() ? "" : g_cmds.back(); }

int main() {
  SetConsoleOutputCP(CP_UTF8);
  static SPF_Telemetry_API tel{};
  tel.Tel_GetTruckData = Tel_GetTruckData;
  tel.Tel_GetGameState = Tel_GetGameState;
  static SPF_GameConsole_API con{};
  con.GCon_IsCheatsEnabled = Con_IsCheats;
  con.GCon_EnableCheats = Con_Enable;
  con.GCon_ExecuteCommand = Con_Exec;
  con.GCon_ExecuteCommandEx = Con_ExecEx;
  static SPF_Logger_API lg{};
  lg.Log = Log;
  static SPF_Core_API core{};
  core.telemetry = &tel;
  core.console = &con;
  core.logger = &lg;
  g.core = &core;
  g.log = (SPF_Logger_Handle*)0x1;
  g.tel = (SPF_Telemetry_Handle*)0x1;
  g.iid = "me000001";
  g.autoName = "Melik";
  g.capturedChat = (void*)0x1;  // ChatManager() falls back to this when the global is not set
  g.o_ChatInput = FakeInput;
  g.o_AddToChat = FakeShow;

  std::cout << "1) normal chat passes through, protocol lines are hidden\n";
  Incoming(5, "selam millet");
  CHECK(g_shown.size() == 1 && g_shown[0] == "selam millet", "normal chat must be displayed");
  Incoming(5, "#KTP1 Q peer0001");
  CHECK(g_shown.size() == 1, "protocol line must not be displayed");

  std::cout << "2) joining a convoy: auto query, own echo verifies the link\n";
  g_truck.position = {1000.0, 10.0, 5000.0};
  g_truck.orientation.heading = 0.0;  // facing north (-Z)
  for (int i = 0; i < 60; ++i) Tick(0.1);
  CHECK(SentStartsWith("#KTP1 Q me000001"), "query sent after joining");
  CHECK(g.localId == g_myId, "local player id learned from own echo");
  CHECK(SentStartsWith("#KTP1 P me000001"), "presence sent (reply to the queued Q)");

  std::cout << "3) driving builds a trail: behind point and rewind\n";
  g_speed = 10.0f;
  for (int i = 0; i < 600; ++i) {  // 60 s at 10 m/s towards -Z
    g_truck.position.z -= 1.0;
    Tick(0.1);
  }
  g_speed = 0.0f;
  Spot b = BehindPoint(25.0, false);
  CHECK(b.valid, "behind point exists");
  CHECK(std::fabs(b.x - 1000.0) < 0.5 && std::fabs((b.z - g_truck.position.z) - 25.0) < 3.5, "behind point is ~25 m back on the road");
  Spot r;
  CHECK(RewindSpot(10.0, r) && std::fabs((r.z - g_truck.position.z) - 100.0) < 4.0, "rewind 10 s = ~100 m back");
  CHECK(RewindSpot(600.0, r) && r.valid, "rewind longer than trail returns the oldest point");

  std::cout << "4) presence message format and length\n";
  strcpy(g.nameBuf, "Çok Uzun İsimli Şoför Ağabey Gülümseyen");
  g_sent.clear();
  SendPresence();
  RunUntilIdle();
  CHECK(!g_sent.empty() && g_sent.back().size() < 199, "presence fits the 199 char chat limit");
  CHECK(g_sent.back().find("#KTP1 P me000001 ") == 0, "presence header");
  g.nameBuf[0] = 0;

  std::cout << "5) a friend's presence is parsed\n";
  Incoming(5, "#KTP1 P peer0001 2 100.00 10.00 200.00 0.2500 130.00 10.00 200.00 0.2500 Ahmet Yılmaz");
  Tick();
  CHECK(g.peers.count("peer0001") == 1, "peer stored");
  CHECK(g.peers["peer0001"].name == "Ahmet Yılmaz", "peer name with Turkish letters");
  CHECK(g.peers["peer0001"].back.valid && g.peers["peer0001"].sender == 5, "peer back point and sender id");

  std::cout << "6) 'Git' teleports behind the friend\n";
  g_cmds.clear();
  ActionGotoPeer("peer0001");
  RunUntilIdle();
  CHECK(LastCmd().rfind("cheat teleport 130.000;10.000;200.000;1.5708", 0) == 0, "teleport to friend's back point, facing his way: " + LastCmd());
  CHECK(std::fabs(g_truck.position.x - 130.0) < 0.01, "truck moved");
  CHECK(g.undoSpot.valid, "undo point stored");

  std::cout << "7) pull request from the host is accepted (default policy)\n";
  g_sent.clear();
  g_cmds.clear();
  Incoming(1, "#KTP1 P host0001 6 900.00 10.00 900.00 0.0000 - - - - Host Abi");
  Incoming(1, "#KTP1 B host0001 me000001 500.00 20.00 600.00 0.5000");
  RunUntilIdle();
  CHECK(LastCmd().rfind("cheat teleport 500.000;20.000;600.000;", 0) == 0, "pulled to the host: " + LastCmd());
  CHECK(SentStartsWith("#KTP1 R me000001 host0001 OK"), "OK reply sent to the host");

  std::cout << "8) pull request from a non-host is refused by default\n";
  g_sent.clear();
  g_cmds.clear();
  Incoming(7, "#KTP1 B rand0001 me000001 1.00 2.00 3.00 0.0000");
  RunUntilIdle();
  CHECK(g_cmds.empty(), "no teleport");
  CHECK(SentStartsWith("#KTP1 R me000001 rand0001 NO"), "NO reply sent");

  std::cout << "9) 'Herkes' policy accepts anyone; 'Kimse' refuses the host too\n";
  g.pullPolicy = PULL_ALL;
  g_cmds.clear();
  Incoming(7, "#KTP1 B rand0001 me000001 50.00 5.00 60.00 0.0000");
  RunUntilIdle();
  CHECK(LastCmd().rfind("cheat teleport 50.000;5.000;60.000;", 0) == 0, "accepted from anyone");
  g.pullPolicy = PULL_NONE;
  g_cmds.clear();
  g_sent.clear();
  Incoming(1, "#KTP1 B host0001 me000001 50.00 5.00 60.00 0.0000");
  RunUntilIdle();
  CHECK(g_cmds.empty() && SentStartsWith("#KTP1 R me000001 host0001 NO"), "refused even from host");
  g.pullPolicy = PULL_HOST;

  std::cout << "10) as the host: 'Getir' and 'Herkesi topla' send bring requests behind me\n";
  g.peers.erase("host0001");
  RunUntilIdle();  // drain echoes still queued from earlier steps
  g_myId = 1;      // from now on our own echo says: player id 1 = session host
  g.localId = 1;
  g_truck.position = {0.0, 0.0, 0.0};
  g.trail.clear();
  g_speed = 10.0f;
  for (int i = 0; i < 300; ++i) {
    g_truck.position.z -= 1.0;
    Tick(0.1);
  }
  g_speed = 0.0f;
  Incoming(8, "#KTP1 P peer0002 2 10.00 1.00 10.00 0.0000 - - - - Veli");
  Incoming(9, "#KTP1 P peer0003 0 10.00 1.00 10.00 0.0000 - - - - Kimseyi");  // accepts nobody
  Tick();
  g_sent.clear();
  ActionBringAll();
  RunUntilIdle();
  int bringCount = 0;
  for (auto& s : g_sent)
    if (s.rfind("#KTP1 B me000001 ", 0) == 0) {
      ++bringCount;
      CHECK(s.size() < 199, "bring line fits");
    }
  CHECK(bringCount == 2, "bring sent to the 2 peers that accept the host (peer0001, peer0002)");
  bool hasNobody = false;
  for (auto& s : g_sent)
    if (s.find(" peer0003 ") != std::string::npos) hasNobody = true;
  CHECK(!hasNobody, "nobody-policy peer is not pulled");

  std::cout << "11) developer mode: retried until enabled, then switched back off\n";
  g_cheatsOnAfter = 3;
  g_cmds.clear();
  ActionUpright();
  RunUntilIdle();
  CHECK(!g.job.active, "job finished");
  CHECK(std::find(g_cmds.begin(), g_cmds.end(), std::string("g_developer 0")) != g_cmds.end(), "g_developer restored to 0");
  int teleports = 0;
  for (auto& c : g_cmds)
    if (c.rfind("cheat teleport", 0) == 0) ++teleports;
  CHECK(teleports == 4, "3 refused attempts + 1 success");

  std::cout << "12) replies are shown, unknown/garbage lines are ignored safely\n";
  Incoming(5, "#KTP1 R peer0001 me000001 OK");
  Incoming(5, "#KTP1 B");
  Incoming(5, "#KTP1 P x");
  Incoming(5, "#KTP1 ZZZ q w e r t y");
  Tick();
  CHECK(true, "no crash on malformed input");

  std::cout << "\n" << g_pass << " passed, " << g_fail << " failed\n";
  return g_fail == 0 ? 0 : 1;
}

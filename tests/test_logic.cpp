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

// wheels / surface / velocity (tests 1-17 run without them, like a game that does not report them)
static int g_wheels = 0;
static bool g_onGround = true;
static int g_airTicks = 0;  // > 0: the truck is in the air for this many reads (dropped after a teleport)
static uint32_t g_subst = 1;
static SPF_FVector g_vel{};
static const char* kSubstances[] = {"static", "road", "road_snow", "dirt", "grass"};

static void Tel_GetTruckData(SPF_Telemetry_Handle*, SPF_TruckData* out, size_t) {
  memset(out, 0, sizeof(*out));
  out->world_placement = g_truck;
  out->speed = g_speed;
  out->local_linear_velocity = g_vel;
  bool og = g_onGround && g_airTicks <= 0;
  if (g_airTicks > 0) --g_airTicks;
  for (int i = 0; i < g_wheels; ++i) {
    out->wheels[i].on_ground = og;
    out->wheels[i].substance = g_subst;
  }
}
static void Tel_GetGameState(SPF_Telemetry_Handle*, SPF_GameState* out, size_t) { memset(out, 0, sizeof(*out)); }
static void Tel_GetCommonData(SPF_Telemetry_Handle*, SPF_CommonData* out, size_t) {
  memset(out, 0, sizeof(*out));
  for (int i = 0; i < 5; ++i) strcpy(out->substances[i], kSubstances[i]);
  out->substance_count = 5;
}
static int CountGood(double minX, double maxX) {
  int n = 0;
  for (auto& p : g.trail)
    if (p.good && p.x >= minX && p.x <= maxX) ++n;
  return n;
}

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
    if (g_wheels) g_airTicks = 4;  // dropped onto the spot
    return 1;
  }
  return 0;
}
static void Log(SPF_Logger_Handle*, SPF_LogLevel, const char* m) { std::cout << "    log: " << m << "\n"; }

// fake chat manager with the game's layout: +8 -> local player, player +0x48 = our convoy id (0xFFFF while joining)
alignas(16) static uint8_t g_fakePlayer[0x60];
alignas(16) static uint8_t g_fakeCm[0x40];
static void SetChatId(uint16_t id) { memcpy(g_fakePlayer + 0x48, &id, 2); }
static int g_badType = 0;  // lines sent with anything but "typed chat" (type 0, broadcast 1)

// fake chat: sending echoes locally with the id the game would use, and records the broadcast
static void FakeShow(void*, uint64_t, const char** pp, uint64_t) { g_shown.push_back(*pp); }
static void FakeInput(void* cm, const char** pp, uint64_t type, uint64_t broadcast) {
  g_sent.push_back(*pp);
  if (type != 0 || broadcast != 1) ++g_badType;
  uint16_t id = 0xFFFF;
  memcpy(&id, g_fakePlayer + 0x48, 2);
  Detour_AddToChat(cm, id, pp, type);
}
static void Incoming(uint16_t sender, const std::string& text) {
  const char* p = text.c_str();
  Detour_AddToChat(g_fakeCm, sender, &p, 0);
}
static int CountSent(const std::string& prefix) {
  int n = 0;
  for (auto& s : g_sent)
    if (s.rfind(prefix, 0) == 0) ++n;
  return n;
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
  tel.Tel_GetCommonData = Tel_GetCommonData;
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
  void* playerPtr = g_fakePlayer;
  memcpy(g_fakeCm + 8, &playerPtr, sizeof(playerPtr));
  SetChatId(0xFFFF);              // still joining the convoy
  g.capturedChat = g_fakeCm;      // ChatManager() falls back to this when the global is not set
  g.o_ChatInput = FakeInput;
  g.o_AddToChat = FakeShow;

  std::cout << "1) normal chat passes through, protocol lines are hidden\n";
  Incoming(5, "selam millet");
  CHECK(g_shown.size() == 1 && g_shown[0] == "selam millet", "normal chat must be displayed");
  Incoming(5, "#KTP1 Q peer0001");
  CHECK(g_shown.size() == 1, "protocol line must not be displayed");

  std::cout << "2) joining a convoy: waits for the player id, auto query, own echo verifies the link\n";
  g_truck.position = {1000.0, 10.0, 5000.0};
  g_truck.orientation.heading = 0.0;  // facing north (-Z)
  for (int i = 0; i < 100; ++i) Tick(0.1);  // 10 s while the game has not given us an id yet
  CHECK(g_sent.empty(), "nothing is sent while joining (the others' games would drop it)");
  SetChatId(g_myId);
  for (int i = 0; i < 40; ++i) Tick(0.1);
  CHECK(SentStartsWith("#KTP1 Q me000001"), "query sent once the player id is known");
  CHECK(g.localId == g_myId, "local player id learned from own echo");
  CHECK(SentStartsWith("#KTP1 P me000001"), "presence sent (reply to the queued Q)");
  CHECK(g_badType == 0, "lines go out exactly like typed chat (type 0, broadcast)");

  std::cout << "2b) nobody answered yet: it keeps asking by itself\n";
  int q0 = CountSent("#KTP1 Q me000001");
  for (int i = 0; i < 100; ++i) Tick(0.1);  // 10 s
  CHECK(CountSent("#KTP1 Q me000001") > q0, "asked again while the friend list is empty");

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
  SetChatId(1);
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

  std::cout << "13) 'Kendi şeridine geç' after a crash into the oncoming lane\n";
  RunUntilIdle();
  g.trail.clear();
  g_truck = SPF_DPlacement{};
  g_truck.position = {0.0, 0.0, 0.0};
  g_truck.orientation.heading = 0.0;  // driving north (-Z), our lane is x = 0
  g_speed = 15.0f;
  for (int i = 0; i < 300; ++i) {  // 300 m in our lane
    g_truck.position.z -= 1.0;
    Tick(0.1);
  }
  double zBeforeCrash = g_truck.position.z;
  for (int i = 0; i < 3; ++i) {  // skid diagonally into the oncoming lane, nose still north
    g_truck.position.x -= 1.5;
    g_truck.position.z -= 1.0;
    Tick(0.1);
  }
  g_speed = 0.0f;
  g_truck.orientation.heading = 0.42;  // spun around
  Tick(0.1);
  int skidGood = 0;
  for (auto& p : g.trail)
    if (p.x < -0.5 && p.good) ++skidGood;
  CHECK(skidGood == 0, "skidding points are not used as road positions");
  g_cmds.clear();
  ActionLane();
  RunUntilIdle();
  double lx = 0, ly = 0, lz = 0, la = 0;
  bool laneCmd = sscanf(LastCmd().c_str(), "cheat teleport %lf;%lf;%lf;%lf;", &lx, &ly, &lz, &la) == 4;
  CHECK(laneCmd && std::fabs(lx) < 0.01, "back to our lane (x = 0): " + LastCmd());
  CHECK(laneCmd && lz - zBeforeCrash > 4.0 && lz - zBeforeCrash < 25.0,
        "a short, safe bit before the crash point (not on top of what we hit)");
  CHECK(laneCmd && std::fabs(la) < 0.01, "facing the driving direction again");

  std::cout << "14) 'Kendi şeridine geç' when already in lane does nothing\n";
  g_cmds.clear();
  ActionLane();
  RunUntilIdle();
  CHECK(g_cmds.empty(), "no teleport when already in lane");

  std::cout << "15) 'Yola çek' from off-road puts the truck ~15 m behind on the road\n";
  g_truck.position.x = 30.0;  // flipped in a field next to the road
  g_truck.orientation.pitch = 0.2;
  g_truck.orientation.heading = 0.3;
  Tick(0.1);
  double zNear = g_truck.position.z;
  g_cmds.clear();
  ActionRoad();
  RunUntilIdle();
  double rx = 0, ry = 0, rz = 0, ra = 0;
  bool roadCmd = sscanf(LastCmd().c_str(), "cheat teleport %lf;%lf;%lf;%lf;", &rx, &ry, &rz, &ra) == 4;
  CHECK(roadCmd && std::fabs(rx) < 0.01 && std::fabs(ra) < 0.01, "on the road, facing forward: " + LastCmd());
  CHECK(roadCmd && (rz - zNear) > 10.0 && (rz - zNear) < 25.0, "a bit behind the nearest road point");
  g_truck.orientation.pitch = 0.0;

  std::cout << "16) 'Yola çek' with no recorded road falls back to putting the truck upright\n";
  g.trail.clear();
  g_truck.position = {5000.0, 50.0, 5000.0};
  Tick(0.1);
  g_cmds.clear();
  ActionRoad();
  RunUntilIdle();
  CHECK(LastCmd().rfind("cheat teleport 5000.000;50.600;5000.000;", 0) == 0, "upright in place: " + LastCmd());

  std::cout << "17) our toasts: max 3 at once, they disappear by themselves\n";
  for (int i = 0; i < 5; ++i) Notify(SPF_NOTIFICATION_SUCCESS, Fmt("bildirim %d", i));
  CHECK(g.toasts.size() == 3, "only the 3 newest toasts are kept");
  for (int i = 0; i < 60; ++i) Tick(0.1);
  CHECK(g.toasts.empty(), "toasts expire");

  auto parseCmd = [](double& x, double& y, double& z, double& a) {
    return sscanf(LastCmd().c_str(), "cheat teleport %lf;%lf;%lf;%lf;", &x, &y, &z, &a) == 4;
  };

  std::cout << "18) after a crash, driving on in the wrong lane does not fool 'Kendi şeridine geç'\n";
  RunUntilIdle();
  g.trail.clear();
  g_truck = SPF_DPlacement{};
  g_truck.position = {2000.0, 0.0, 0.0};
  g_speed = 15.0f;
  for (int i = 0; i < 200; ++i) {
    g_truck.position.z -= 1.0;
    Tick(0.1);
  }
  double zCrash = g_truck.position.z;
  for (int i = 0; i < 3; ++i) {  // pushed into the oncoming lane
    g_truck.position.x -= 1.5;
    g_truck.position.z -= 1.0;
    Tick(0.1);
  }
  for (int i = 0; i < 40; ++i) {  // ...and driven on there for 40 m
    g_truck.position.z -= 1.0;
    Tick(0.1);
  }
  g_speed = 0.0f;
  CHECK(CountGood(1994.0, 1996.0) > 5, "(the wrong-lane driving itself looks normal)");
  g_cmds.clear();
  ActionLane();
  RunUntilIdle();
  double ax = 0, ay = 0, az = 0, aa = 0;
  bool aOk = parseCmd(ax, ay, az, aa);
  CHECK(aOk && std::fabs(ax - 2000.0) < 0.01, "back to the lane we drove in before the crash: " + LastCmd());
  CHECK(aOk && az - zCrash > 4.0 && az - zCrash < 25.0, "just before the crash point");

  std::cout << "19) grass and dirt are never used as road positions\n";
  g.trail.clear();
  g_truck = SPF_DPlacement{};
  g_truck.position = {4000.0, 0.0, 0.0};
  g_wheels = 6;
  g_onGround = true;
  g_subst = 1;  // "road"
  g_speed = 10.0f;
  for (int i = 0; i < 100; ++i) {
    g_truck.position.z -= 1.0;
    Tick(0.1);
  }
  g_subst = 2;  // "road_snow" is still road
  for (int i = 0; i < 30; ++i) {
    g_truck.position.z -= 1.0;
    Tick(0.1);
  }
  CHECK(CountGood(3999.0, 4001.0) > 30, "asphalt (also snowy) is recorded");
  g_truck.orientation.heading = 0.75;  // turn east, off the road into a field
  g_subst = 4;                         // "grass"
  for (int i = 0; i < 40; ++i) {
    g_truck.position.x += 1.0;
    Tick(0.1);
  }
  g_subst = 3;  // "dirt"
  for (int i = 0; i < 20; ++i) {
    g_truck.position.x += 1.0;
    Tick(0.1);
  }
  g_speed = 0.0f;
  CHECK(CountGood(4002.0, 5000.0) == 0, "no road positions in the field");
  g_cmds.clear();
  ActionRoad();
  RunUntilIdle();
  double bx = 0, by = 0, bz = 0, ba = 0;
  bool bOk = parseCmd(bx, by, bz, ba);
  CHECK(bOk && std::fabs(bx - 4000.0) < 0.01 && std::fabs(ba) < 0.01, "'Yola çek' from the field goes to the road: " + LastCmd());
  g_subst = 1;

  std::cout << "20) flying through the air is a crash; dropping after our own teleport is not\n";
  g.trail.clear();
  g_truck = SPF_DPlacement{};
  g_truck.position = {6000.0, 0.0, 0.0};
  g_speed = 10.0f;
  for (int i = 0; i < 100; ++i) {
    g_truck.position.z -= 1.0;
    Tick(0.1);
  }
  g_onGround = false;
  for (int i = 0; i < 8; ++i) {  // launched
    g_truck.position.z -= 1.0;
    g_truck.position.y += 0.6;
    Tick(0.1);
  }
  CHECK(g_testNow - g.crashT < 1.0, "flight detected");
  int airGood = 0;
  for (auto& p : g.trail)
    if (p.good && p.y > 0.1) ++airGood;
  CHECK(airGood == 0, "positions in the air are not road positions");
  g_onGround = true;
  g_truck.position.y = 0.0;
  g_speed = 0.0f;
  for (int i = 0; i < 40; ++i) Tick(0.1);
  double crashBefore = g.crashT;
  ActionUpright();
  RunUntilIdle();
  CHECK(g.crashT == crashBefore, "our own teleport drop is not a crash");

  std::cout << "21) a sideways slide (from the truck's own velocity) is not a road position\n";
  g.trail.clear();
  g_truck = SPF_DPlacement{};
  g_truck.position = {8000.0, 0.0, 0.0};
  g_speed = 12.0f;
  g_vel = {0.0f, 0.0f, -12.0f};  // straight ahead
  for (int i = 0; i < 60; ++i) {
    g_truck.position.z -= 1.2;
    Tick(0.1);
  }
  size_t before = g.trail.size();
  CHECK(CountGood(7999.0, 8001.0) > 10, "normal driving is recorded");
  g_vel = {8.0f, 0.0f, -9.0f};  // sliding ~42 deg sideways
  for (int i = 0; i < 10; ++i) {
    g_truck.position.z -= 1.2;
    Tick(0.1);
  }
  int slideGood = 0;
  for (size_t i = before; i < g.trail.size(); ++i)
    if (g.trail[i].good) ++slideGood;
  CHECK(slideGood == 0, "slide samples are not road positions");
  CHECK(g_testNow - g.crashT < 1.0, "slide detected as a crash");
  g_vel = SPF_FVector{};
  g_speed = 0.0f;

  std::cout << "22) updater: versions, GitHub release JSON, SHA-256\n";
  CHECK(ktpup::CompareVersions("v1.3.0", "1.2.9") > 0 && ktpup::CompareVersions("1.3", "1.3.0.0") == 0 && ktpup::CompareVersions("1.2.10", "1.2.9") > 0,
        "version compare");
  CHECK(ktpup::CleanVersion("v1.4.2") == "1.4.2", "tag to version");
  {
    const char* json = R"({"tag_name":"v9.8.7","draft":false,"name":"KonvoyTP ç \"x\"","assets":[
        {"name":"KonvoyTP.dll","size":391168,"digest":"sha256:ABCDEF0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789",
         "browser_download_url":"https://github.com/unsalable/KonvoyTP/releases/download/v9.8.7/KonvoyTP.dll"},
        {"name":"KonvoyTP_Kurulum.exe","size":17336832,"digest":null,"browser_download_url":"https://github.com/x/y.exe"},
        {"name":"evil","size":1,"digest":"sha256:00","browser_download_url":"http://insecure.example/evil"}]})";
    ktpup::Release rel;
    bool ok = ktpup::ParseRelease(json, rel);
    CHECK(ok && rel.tag == "v9.8.7", "release tag parsed");
    const ktpup::Asset* dll = rel.Find("KonvoyTP.dll");
    CHECK(dll && dll->size == 391168 && dll->sha256 == "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789", "asset size + digest (lowercased)");
    const ktpup::Asset* app = rel.Find("KonvoyTP_Kurulum.exe");
    CHECK(app && app->sha256.empty(), "asset without digest has no hash");
    CHECK(!rel.Find("evil"), "non-https download urls are ignored");
    std::string data, err;
    CHECK(app && !ktpup::DownloadAsset(*app, data, err) && !err.empty(), "files without a digest are never downloaded");
    ktpup::Release draft;
    CHECK(!ktpup::ParseRelease(R"({"tag_name":"v1","draft":true,"assets":[]})", draft), "draft releases are ignored");
    ktpup::JVal v;
    CHECK(!ktpup::ParseJson("{\"a\": [1, 2,]}", v) && !ktpup::ParseJson("{\"a\": 1} x", v), "broken JSON is rejected");
  }
  CHECK(ktpup::Sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of 'abc'");
  {
    std::string notPe = "MZ";
    notPe.resize(0x400, '\0');
    CHECK(!ktpup::LooksLikePe64(notPe) && !ktpup::LooksLikePe64("hello"), "non-PE files are rejected");
    std::string self;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    CHECK(ktpup::ReadFileAll(exe, self) && ktpup::LooksLikePe64(self), "a real x64 exe is accepted");
  }

  // Live check against GitHub (only when asked: set KTP_CANLI=1). Installs the latest release's KonvoyTP.dll over a
  // dummy file in a temp folder, exactly like the plugin does at game start.
  if (GetEnvironmentVariableA("KTP_CANLI", nullptr, 0) > 0) {
    std::cout << "23) live: the plugin's self-update against GitHub\n";
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"ktp_selfupdate_test";
    CreateDirectoryW(dir.c_str(), nullptr);
    ktpup::WriteFileAll(dir + L"\\KonvoyTP.dll", "eski");
    volatile LONG cancel = 0;
    RunUpdateCheck(dir, "0.0.1", &cancel);
    std::cout << "    durum: " << UpdateState() << "\n";
    std::string got;
    CHECK(ktpup::ReadFileAll(dir + L"\\KonvoyTP.dll", got) && ktpup::LooksLikePe64(got) && got.find("SPF_GetPlugin") != std::string::npos,
          "new KonvoyTP.dll installed in place of the old one");
    std::string old;
    CHECK(ktpup::ReadFileAll(dir + L"\\KonvoyTP.dll.eski", old) && old == "eski", "previous DLL kept as KonvoyTP.dll.eski");
    RunUpdateCheck(dir, "999.0.0", &cancel);
    CHECK(UpdateState() == "güncel", "nothing to do when already newest");
  }

  std::cout << "\n" << g_pass << " passed, " << g_fail << " failed\n";
  return g_fail == 0 ? 0 : 1;
}

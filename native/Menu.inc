// Mod menu (Phase 1 rows only), included by LostCrownVRNative.cpp. Runs on the game's main thread; the XR thread draws
// the snapshot (xr\MenuDraw.cpp). Opens with both sticks held 0.5 s on an Xbox pad, or Insert. While it is open the
// game is paused (time scale 0) and its Unity Input System actions are switched off, so it gets no pad or keyboard
// input; they come back once every key and button has been released after closing (AI Limit VR's way).
// From Ori WotW VR's and AI Limit VR's menus (same author, MIT).

namespace menu {

enum Kind { Button, Value };
struct Row { const char* label; Kind kind; };
struct Tab { const char* name; std::vector<Row> rows; };
static std::vector<Tab> g_tabs;
static bool g_open = false, g_block = false;
static int g_tab = 0, g_row = 0;
static int g_rowPerTab[3] = {0, 0, 0};
static double g_bothSticksSince = -1;
static bool g_comboUsed = false;
static bool g_lastKeyboard = false;
static uint32_t g_padReads = 0;
static int g_labKey = 0; static double g_labKeyUntil = 0;  // lab: a key held for 0.15 s by command

// The pad, read by the menu itself through XInput (the game reads it through Unity's Input System).
struct XState { DWORD packet; uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; };
typedef DWORD(WINAPI* XInputGetState_t)(DWORD, XState*);
static XInputGetState_t g_xget;
enum { PadUp = 0x0001, PadDown = 0x0002, PadLeft = 0x0004, PadRight = 0x0008, PadStart = 0x0010, PadBack = 0x0020,
       PadL3 = 0x0040, PadR3 = 0x0080, PadLB = 0x0100, PadRB = 0x0200, PadA = 0x1000, PadB = 0x2000 };
static bool ReadPad(uint16_t& b, int16_t& lx, int16_t& ly) {
    if (!g_xget) {
        HMODULE x = LoadLibraryA("xinput1_4.dll");
        g_xget = x ? (XInputGetState_t)GetProcAddress(x, "XInputGetState") : nullptr;
        Log("INPUT XInput %s", g_xget ? "loaded (xinput1_4.dll): the menu reads the pad itself" : "NOT loaded: the menu works with the keyboard only");
        if (!g_xget) g_xget = (XInputGetState_t)1;
    }
    if (g_xget == (XInputGetState_t)1) return false;
    b = 0; lx = ly = 0;
    for (DWORD i = 0; i < 4; i++) {
        XState s{};
        if (g_xget(i, &s) == 0) { g_padReads++; b |= s.buttons; if (abs(s.lx) > abs(lx)) lx = s.lx; if (abs(s.ly) > abs(ly)) ly = s.ly; }
    }
    return true;
}

static void Build() {
    g_tabs = {
        {"STAGE", {{"Stage distance", Value}, {"Stage size", Value}, {"Depth", Value}, {"Pop-out", Value}, {"Edge match", Value}, {"Recenter", Button}}},
        {"PICTURE", {{"Resolution", Value}, {"Sharpen", Value}, {"Picture", Value}, {"Surround", Value}}},
        {"MENU", {{"Menu size", Value}, {"Menu height", Value}, {"Menu distance", Value}, {"Reset to defaults", Button}, {"Close", Button}}},
    };
}

static int NextEyeWidth() {
    float span = g_set.tanSpan > 0.5f ? g_set.tanSpan : 2.2f;
    int w = ((int)(lcxr::RecommendedWidth() * (g_set.stageWidth / g_set.stageDistance) / span * g_set.resolution + 8) / 16) * 16;
    return w < 640 ? 640 : w > 8192 ? 8192 : w;
}

static const char* SurroundName(int s) { return s == 1 ? "Green" : s == 2 ? (lcxr::GetState() == lcxr::Running && !lcxr::CanSeeThrough() ? "Transparent (not offered, black)" : "Transparent") : "Black"; }
static const char* PictureName(int p) { return p == 1 ? "Steady" : p == 2 ? "Floating Panel" : "Auto"; }

static void ValueText(const std::string& l, char* out, size_t n) {
    if (l == "Stage distance") sprintf_s(out, n, "%.2f m", g_set.stageDistance);
    else if (l == "Stage size") sprintf_s(out, n, "%.2f m", g_set.stageWidth);
    else if (l == "Depth") sprintf_s(out, n, "%d%%", (int)lroundf(g_set.depth * 100));
    else if (l == "Resolution") sprintf_s(out, n, "%d%%", (int)lroundf(g_set.resolution * 100));
    else if (l == "Surround") strcpy_s(out, n, SurroundName(g_set.surround));
    else if (l == "Pop-out") sprintf_s(out, n, "%d%%", (int)lroundf(g_set.popOut * 100));
    else if (l == "Sharpen") sprintf_s(out, n, "%d%%", (int)lroundf(g_set.sharpen * 100));
    else if (l == "Edge match") strcpy_s(out, n, g_set.edgeMask ? "On" : "Off");
    else if (l == "Picture") {
        if (g_set.picture == 0 && lcxr::LayerInUse() >= 0) sprintf_s(out, n, "Auto (%s)", lcxr::LayerInUse() == 1 ? "Steady" : "Floating Panel");
        else strcpy_s(out, n, PictureName(g_set.picture));
    }
    else if (l == "Menu size") sprintf_s(out, n, "%d%%", (int)lroundf(g_set.menuSize * 100));
    else if (l == "Menu height") sprintf_s(out, n, "%+.2f m", g_set.menuHeight);
    else if (l == "Menu distance") sprintf_s(out, n, "%.2f m", g_set.menuDistance);
    else out[0] = 0;
}

static void Tip(const std::string& l, char* out, size_t n) {
    if (l == "Stage distance") strcpy_s(out, n, "How far away the stage floats. Closer fills more of your view. The game's HUD sits on the stage and moves with it.");
    else if (l == "Stage size") strcpy_s(out, n, "How wide the stage is, in metres. Bigger fills more of your view.");
    else if (l == "Depth") strcpy_s(out, n, "How strong the 3D is. Higher makes the world feel deeper and smaller. If far backgrounds strain your eyes, lower it or raise Pop-out.");
    else if (l == "Edge match") strcpy_s(out, n, g_set.edgeMask ? "On: at the left and right edges of the stage both eyes end on the same part of the world, near or far. What only one eye could see is hidden." : "Off: at the stage edges one eye sees a little more of the world than the other.");
    else if (l == "Recenter") strcpy_s(out, n, "Puts the stage and this menu straight in front of where you are looking now. Your headset's own recenter does the same.");
    else if (l == "Resolution") {
        int nw = NextEyeWidth();
        sprintf_s(out, n, "Sharpness of the game picture. 100%% matches your headset's pixel density across the stage. Running now: %dx%d. Next launch: %dx%d (takes effect when you start the game again).",
                  g_eyeW, g_eyeH, nw, ((int)(nw * (float)g_hudH / g_hudW) + 1) & ~1);
    }
    else if (l == "Surround") strcpy_s(out, n, lcxr::GetState() == lcxr::Running && !lcxr::CanSeeThrough()
        ? "What you see around the stage. Black is the original look. Green is for Virtual Desktop's chroma-key passthrough. Transparent is not offered by your headset software, so it shows black."
        : "What you see around the stage. Black is the original look. Green is for Virtual Desktop's chroma-key passthrough. Transparent asks your headset software to show your room.");
    else if (l == "Pop-out") strcpy_s(out, n, "Brings the Prince and more of the scene out of the stage toward you. It also eases far backgrounds. 0% keeps the Prince on the stage.");
    else if (l == "Sharpen") strcpy_s(out, n, "Crisper edges. Too high looks harsh. 0% turns it off.");
    else if (l == "Picture") sprintf_s(out, n, "Auto picks for your headset software: Steady on Virtual Desktop, Floating Panel on everything else. Steady draws the stage into each eye's view and stays still. Floating Panel can look a little sharper, but Virtual Desktop squashes and shears it when you turn your head.%s%s%s",
                  lcxr::RuntimeName()[0] ? " Detected: " : "", lcxr::RuntimeName(), lcxr::RuntimeName()[0] ? "." : "");
    else if (l == "Menu size") strcpy_s(out, n, "Makes this menu bigger or smaller. The stage does not change.");
    else if (l == "Menu height") strcpy_s(out, n, "Moves this menu up or down from eye height. 0 is eye height.");
    else if (l == "Menu distance") strcpy_s(out, n, "How far this menu floats in front of you. The stage does not move.");
    else if (l == "Reset to defaults") strcpy_s(out, n, "Puts every setting on all three tabs back to how it started.");
    else if (l == "Close") strcpy_s(out, n, "Closes this menu and goes back to the game. Holding both sticks or pressing Insert also opens and closes it.");
    else out[0] = 0;
}

static float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static bool SetF(float& f, float v, float lo, float hi) { v = Clamp(roundf(v * 100) / 100, lo, hi); if (fabsf(v - f) < 0.0001f) return false; f = v; return true; }
static void LogSettings(const char* why) {
    Log("MENU %s: stage %.2f m wide at %.2f m, depth %.2f, pop-out %.2f, edge match %d, resolution %.2f, sharpen %.2f, picture %d, surround %d, menu %.2f / %+.2f m / %.2f m",
        why, g_set.stageWidth, g_set.stageDistance, g_set.depth, g_set.popOut, g_set.edgeMask, g_set.resolution, g_set.sharpen, g_set.picture, g_set.surround, g_set.menuSize, g_set.menuHeight, g_set.menuDistance);
}
static bool Change(const std::string& l, int d) {
    bool c = false;
    if (l == "Stage distance") { c = SetF(g_set.stageDistance, g_set.stageDistance + d * 0.05f, 0.5f, 10.f); if (c) lcxr::SetStage({g_set.stageWidth, g_set.stageDistance}); }
    else if (l == "Stage size") { c = SetF(g_set.stageWidth, g_set.stageWidth + d * 0.05f, 0.5f, 10.f); if (c) lcxr::SetStage({g_set.stageWidth, g_set.stageDistance}); }
    else if (l == "Depth") c = SetF(g_set.depth, g_set.depth + d * 0.05f, 0.f, 3.f);
    else if (l == "Resolution") c = SetF(g_set.resolution, g_set.resolution + d * 0.1f, 0.5f, 1.5f);
    else if (l == "Edge match") { g_set.edgeMask = !g_set.edgeMask; c = true; }
    else if (l == "Surround") { g_set.surround = (g_set.surround + 3 + d) % 3; lcxr::SetSurround(g_set.surround); c = true; }
    else if (l == "Pop-out") c = SetF(g_set.popOut, g_set.popOut + d * 0.05f, 0.f, 1.5f);
    else if (l == "Sharpen") { c = SetF(g_set.sharpen, g_set.sharpen + d * 0.1f, 0.f, 1.f); lcxr::SetSharpen(g_set.sharpen); }
    else if (l == "Picture") { g_set.picture = (g_set.picture + 3 + d) % 3; lcxr::SetPicture(g_set.picture); c = true; }
    else if (l == "Menu size") c = SetF(g_set.menuSize, g_set.menuSize + d * 0.05f, 0.6f, 1.4f);
    else if (l == "Menu height") c = SetF(g_set.menuHeight, g_set.menuHeight + d * 0.05f, -0.6f, 0.6f);
    else if (l == "Menu distance") c = SetF(g_set.menuDistance, g_set.menuDistance + d * 0.05f, 0.3f, 2.f);
    if (c) { SaveSettings(); LogSettings(l.c_str()); }
    return c;
}
static void SetOpen(bool open, const char* how);
static void Press(const std::string& l) {
    if (l == "Recenter") { lcxr::Recenter(); Log("MENU recenter"); }
    else if (l == "Reset to defaults") {
        Settings keep = g_set;
        g_set = kDefaults;
        g_set.tanSpan = keep.tanSpan; g_set.labCommands = keep.labCommands;
        SaveSettings(); lcxr::SetStage({g_set.stageWidth, g_set.stageDistance}); lcxr::SetPicture(g_set.picture); lcxr::SetSharpen(g_set.sharpen); lcxr::SetEdgeFade(g_set.edgeFade); lcxr::SetSurround(g_set.surround); LogSettings("reset to defaults");
    }
    else if (l == "Close") SetOpen(false, "Close row");
}

static void Snapshot() {
    lcxr::MenuSnapshot s;
    s.visible = g_open;
    s.problem = lcxr::GetState() == lcxr::Failed;
    s.tabCount = (int)g_tabs.size(); s.tab = g_tab;
    for (int i = 0; i < s.tabCount; i++) strcpy_s(s.tabs[i], g_tabs[i].name);
    const Tab& t = g_tabs[g_tab];
    s.rowCount = (int)t.rows.size(); s.row = g_row;
    for (int i = 0; i < s.rowCount; i++) {
        strcpy_s(s.rows[i].label, t.rows[i].label);
        ValueText(t.rows[i].label, s.rows[i].value, sizeof s.rows[i].value);
        s.rows[i].button = t.rows[i].kind == Button;
    }
    if (s.problem) sprintf_s(s.explain, "OpenXR error %d. Check that your headset software (SteamVR, Virtual Desktop) is running and set as the OpenXR runtime, then start the game again. The log LostCrownVR.log in the game folder has the details.", lcxr::LastError());
    else Tip(t.rows[g_row].label, s.explain, sizeof s.explain);
    strcpy_s(s.hint, g_lastKeyboard ? "Page Up Down: tabs     Arrow keys: choose and change     Enter: select     Insert or Esc: close"
                                    : "LB RB: tabs     Up Down: choose     Left Right: change     A: select     B: close");
    lcxr::Views v; lcxr::GetViews(&v);
    sprintf_s(s.readout, "Headset %.0f Hz  \xC2\xB7  eye %d x %d  \xC2\xB7  headset asks %d wide  \xC2\xB7  %s",
              v.periodSec > 0 ? 1.0 / v.periodSec : 0.0, g_eyeW, g_eyeH, lcxr::RecommendedWidth(), lcxr::RuntimeName()[0] ? lcxr::RuntimeName() : "no OpenXR");
    sprintf_s(s.version, "Prince of Persia VR %s (build %d)", MOD_VERSION, MOD_BUILD);
    s.size = g_set.menuSize; s.height = g_set.menuHeight; s.distance = g_set.menuDistance;
    menuui::Update(s);
    lcxr::SetMenu(s);
}

// Repeat for held directions: first step at once, then after 0.4 s every 0.12 s.
struct Repeat { double since = -1, last = 0; bool Step(bool held, double now) {
    if (!held) { since = -1; return false; }
    if (since < 0) { since = now; last = now; return true; }
    if (now - since > 0.4 && now - last > 0.12) { last = now; return true; }
    return false; } };
static Repeat g_rUp, g_rDown, g_rLeft, g_rRight;
static bool g_prevA, g_prevB, g_prevLB, g_prevRB, g_prevIns;

static bool OurWindowFocused() {
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
static bool Key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0 || (g_labKey == vk && Now() < g_labKeyUntil); }

// The game's Input System actions are switched off while the menu blocks input, and the same ones back on afterwards.
static std::vector<Il2CppObject*> g_offActions;
static void BlockGameInput(bool block) {
    if (block) {
        Il2CppObject* list = P<Il2CppObject* (*)(MethodInfo*)>(mListEnabledActions)(mListEnabledActions);
        g_offActions.clear();
        if (list) {
            Il2CppArray* items = Fld<Il2CppArray*>(list, oListItems); int n = Fld<int>(list, oListSize);
            for (int i = 0; i < n; i++) { Il2CppObject* a = (Il2CppObject*)items->data[i]; Keep(a); g_offActions.push_back(a); }
            for (auto a : g_offActions) P<void (*)(Il2CppObject*, MethodInfo*)>(mActionDisable)(a, mActionDisable);
        }
        Log("INPUT %zu game input actions switched off while the menu is open", g_offActions.size());
    } else {
        for (auto a : g_offActions) P<void (*)(Il2CppObject*, MethodInfo*)>(mActionEnable)(a, mActionEnable);
        Log("INPUT %zu game input actions switched back on", g_offActions.size());
        g_offActions.clear();
    }
}
// The game pauses while the menu is open (time scale 0), as in Ori VR; put back on close.
static float g_savedScale = -1;
static void PauseGame(bool pause) {
    if (pause) { g_savedScale = SF(mTimeScale); SetSF(mSetTimeScale, 0.0f); Log("MENU game paused (time scale was %.2f)", g_savedScale); }
    else if (g_savedScale >= 0) { SetSF(mSetTimeScale, g_savedScale); Log("MENU game resumed (time scale %.2f)", g_savedScale); g_savedScale = -1; }
}
static void SetOpen(bool open, const char* how) {
    if (open == g_open) return;
    g_open = open;
    PauseGame(open);
    Log("MENU %s (%s)", open ? "opened" : "closed", how);
}

static void Update(double now) {
    if (g_tabs.empty()) Build();
    bool kb = OurWindowFocused();
    uint16_t b = 0; int16_t lx = 0, ly = 0;
    ReadPad(b, lx, ly);
    bool ins = (kb || g_labKey) && Key(VK_INSERT);
    bool both = (b & PadL3) && (b & PadR3);
    if (both) { if (g_bothSticksSince < 0) g_bothSticksSince = now; if (!g_comboUsed && now - g_bothSticksSince > 0.5) { g_comboUsed = true; g_lastKeyboard = false; SetOpen(!g_open, "both sticks"); } }
    else { g_bothSticksSince = -1; g_comboUsed = false; }
    if (ins && !g_prevIns) { g_lastKeyboard = true; SetOpen(!g_open, "Insert"); }
    g_prevIns = ins;
    bool k = kb || g_labKey;
    bool keys = k && (Key(VK_UP) || Key(VK_DOWN) || Key(VK_LEFT) || Key(VK_RIGHT) || Key(VK_RETURN) || Key(VK_ESCAPE) || Key(VK_BACK) || Key(VK_PRIOR) || Key(VK_NEXT) || ins);
    bool anyHeld = b != 0 || abs(ly) > 8000 || abs(lx) > 8000 || keys;
    // After closing, the game gets input back only once every key and button has been up for 0.15 s.
    static double releasedSince = -1;
    if (g_open && !g_block) { g_block = true; BlockGameInput(true); }
    if (g_open) releasedSince = -1;
    else if (g_block) {
        if (anyHeld) releasedSince = -1;
        else if (releasedSince < 0) releasedSince = now;
        else if (now - releasedSince > 0.15) { g_block = false; BlockGameInput(false); Log("MENU input back to the game"); }
    }
    if (g_open) {
        if (keys) g_lastKeyboard = true; else if (b || abs(ly) > 8000 || abs(lx) > 8000) g_lastKeyboard = false;
        const Tab& t = g_tabs[g_tab];
        bool up = (b & PadUp) || ly > 16000 || (k && Key(VK_UP));
        bool down = (b & PadDown) || ly < -16000 || (k && Key(VK_DOWN));
        bool left = (b & PadLeft) || lx < -16000 || (k && Key(VK_LEFT));
        bool right = (b & PadRight) || lx > 16000 || (k && Key(VK_RIGHT));
        bool a = (b & PadA) || (k && Key(VK_RETURN));
        bool back = (b & PadB) || (k && (Key(VK_ESCAPE) || Key(VK_BACK)));
        bool lb = (b & PadLB) || (k && Key(VK_PRIOR)), rb = (b & PadRB) || (k && Key(VK_NEXT));
        int rows = (int)t.rows.size();
        if (g_rUp.Step(up, now)) g_row = (g_row + rows - 1) % rows;
        if (g_rDown.Step(down, now)) g_row = (g_row + 1) % rows;
        const Row& r = t.rows[g_row];
        if (r.kind == Value) {
            if (g_rLeft.Step(left, now)) Change(r.label, -1);
            if (g_rRight.Step(right, now)) Change(r.label, +1);
        } else { g_rLeft.Step(false, now); g_rRight.Step(false, now); }
        if (a && !g_prevA && r.kind == Button) Press(r.label);
        if (back && !g_prevB) SetOpen(false, "back");
        if ((lb && !g_prevLB) || (rb && !g_prevRB)) {
            g_rowPerTab[g_tab] = g_row;
            g_tab = (g_tab + (int)g_tabs.size() + (rb && !g_prevRB ? 1 : -1)) % (int)g_tabs.size();
            g_row = g_rowPerTab[g_tab];
        }
        g_prevA = a; g_prevB = back; g_prevLB = lb; g_prevRB = rb;
    } else {
        g_prevA = (b & PadA) || (k && Key(VK_RETURN)); g_prevB = (b & PadB) || (k && (Key(VK_ESCAPE) || Key(VK_BACK)));
        g_prevLB = (b & PadLB) || (k && Key(VK_PRIOR)); g_prevRB = (b & PadRB) || (k && Key(VK_NEXT));
    }
    Snapshot();
}

}  // namespace menu

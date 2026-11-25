// Stylized mod menu (build 10): drawn by the game itself with Unity UI, in the game's own TextMeshPro font and menu
// sprites found among its loaded assets at run time (nothing of the game is copied or shipped). Included by
// LostCrownVRNative.cpp before Menu.inc. Menu.inc keeps the rows, tabs, input and explanations; this file only draws.
//
// A hidden canvas (Screen Space - Camera) on its own camera renders into a 1400x787 texture, only while the menu is open;
// after the frame's render it is copied into the XR side's shared menu texture (lcxr::ExternalMenuShare), which the XR
// thread shows on the menu layer. Layout as in build 9's menu (MenuDraw.cpp), so look, not behaviour, changes.
// If anything is missing, the XR side's own drawing (Jost font) is used as before.

namespace menuui {

struct Vec2 { float x, y; };
static const int kW = 1400, kH = 787, kLayer = 31;
static bool g_tried = false, g_ok = false, g_visible = false;
static bool g_enabled = true;  // lab: "menuui 0" goes back to the XR side's own drawing
static Il2CppObject *g_cam, *g_rt, *g_canvasGo;
static Il2CppObject* g_font;       // the game's TMP_FontAsset (text)
static Il2CppObject* g_titleFont;  // and its title font
static Il2CppObject *g_sprHeader, *g_sprFooter;
static std::string g_titleFontName = "?", g_headerName = "-", g_footerName = "-", g_arrowName = "-";
static Il2CppObject *g_sprPanel, *g_sprBar, *g_sprArrow;  // the game's sprites (may be null: plain colour then)
static std::string g_fontName = "?", g_panelName = "-", g_barName = "-";

static MethodInfo *mSetLayer, *mSetParent, *mGetTransform, *mGoTransform, *mGoSetActive;
static MethodInfo *mAnchorMin, *mAnchorMax, *mPivot, *mAnchoredPos, *mSizeDelta;
static MethodInfo *mImgSprite, *mImgType, *mGraphicColor, *mRaycast;
static MethodInfo *mTmpText, *mTmpFont, *mTmpSize, *mTmpAlign, *mTmpWrap, *mTmpStyle, *mTmpSpacing, *mTmpColor, *mTmpPrefW;
static MethodInfo *mCanvasCam, *mCanvasPlane, *mCanvasModeSet, *mCanvasOrder;
static void *kImage, *kTmpUgui, *kCanvasK, *kRect;

struct El { Il2CppObject* go = nullptr; Il2CppObject* comp = nullptr; std::string text; bool active = true; bool isText = false; };
static El g_bg, g_frame, g_title, g_version, g_tabHi, g_tabs[6], g_lb, g_rb, g_rowHi, g_rowLab[8], g_rowVal[8], g_arrL, g_arrR,
          g_explain, g_rule, g_hint, g_readout;

static void SetActive(El& e, bool on) {
    if (e.go && e.active != on) { e.active = on; P<void (*)(Il2CppObject*, bool, MethodInfo*)>(mGoSetActive)(e.go, on, mGoSetActive); }
}
static void Place(El& e, float x, float y, float w, float h) {  // top-left origin, pixels
    Il2CppObject* rt = GetObj(mGetTransform, e.comp);
    auto v2 = [&](MethodInfo* m, Vec2 v) { P<void (*)(Il2CppObject*, Vec2, MethodInfo*)>(m)(rt, v, m); };
    v2(mAnchorMin, {0, 1}); v2(mAnchorMax, {0, 1}); v2(mPivot, {0, 1});
    v2(mAnchoredPos, {x, -y}); v2(mSizeDelta, {w, h});
}
// TextMeshPro keeps its own text colour (TMP_Text.color), separate from the UI base colour.
static void Colour(El& e, Color c) { MethodInfo* m = (e.isText && mTmpColor) ? mTmpColor : mGraphicColor; P<void (*)(Il2CppObject*, Color, MethodInfo*)>(m)(e.comp, c, m); }
static El Make(const char* name, void* klass) {
    El e;
    e.go = NewGameObject(name);
    P<void (*)(Il2CppObject*, int, MethodInfo*)>(mSetLayer)(e.go, kLayer, mSetLayer);
    e.comp = AddComp(e.go, klass);
    if (g_canvasGo) P<void (*)(Il2CppObject*, Il2CppObject*, bool, MethodInfo*)>(mSetParent)(GetObj(mGoTransform, e.go), GetObj(mGoTransform, g_canvasGo), false, mSetParent);
    if (mRaycast) P<void (*)(Il2CppObject*, bool, MethodInfo*)>(mRaycast)(e.comp, false, mRaycast);
    return e;
}
static El Image(const char* name, Il2CppObject* sprite, Color c, bool sliced) {
    El e = Make(name, kImage);
    if (sprite) { SetIO(mImgSprite, e.comp, sprite); SetII(mImgType, e.comp, sliced ? 1 : 0); }
    Colour(e, c);
    return e;
}
static El Text(const char* name, float size, Color c, int align, bool title = false) {
    El e = Make(name, kTmpUgui); e.isText = true;
    Il2CppObject* f = title && g_titleFont ? g_titleFont : g_font;
    if (f) SetIO(mTmpFont, e.comp, f);
    SetIF(mTmpSize, e.comp, size);
    SetII(mTmpAlign, e.comp, align);
    if (mTmpWrap) SetIB(mTmpWrap, e.comp, false);
    Colour(e, c);
    return e;
}
static void SetText(El& e, const char* s) {
    if (e.text == s) return;
    e.text = s;
    SetIO(mTmpText, e.comp, il2cpp_string_new(s));
}
enum { AlignLeft = 513, AlignCenter = 514, AlignRight = 516, AlignTopCenter = 258 };

// The game's font and sprites, by name (uiprobe.txt lists what is loaded). First match wins.
static const char* kFontNames[] = {"AvenirNextLTPro-Medium SDF", "AvenirNextLTPro-Regular SDF"};
static const char* kTitleFontNames[] = {"ContaxPro75Bold SDF", "AvenirNextLTPro-Medium SDF"};
static const char* kPanelSprites[] = {"Menu_BG_4K_UI"};
static const char* kHeaderSprites[] = {"Menu_Header_BG_4K_UI"};
static const char* kFooterSprites[] = {"Menu_Footer_BG_4K_UI"};
static const char* kArrowSprites[] = {"SelectArrow_4K_UI"};
static const char* kBarSprites[] = {"Button_Bg_Selected_4K_UI"};
static Il2CppObject* FindByName(void* klass, const char* const* names, int n, std::string& got) {
    MethodInfo* findAll = FindMethod(FindClass("UnityEngine", "Resources"), "FindObjectsOfTypeAll", 1);
    if (!findAll || !klass) return nullptr;
    Il2CppArray* a = P<Il2CppArray* (*)(Il2CppObject*, MethodInfo*)>(findAll)(TypeOf(klass), findAll);
    for (int k = 0; k < n; k++)
        for (uintptr_t i = 0; a && i < a->max_length; i++) {
            Il2CppObject* o = (Il2CppObject*)a->data[i];
            if (o && Name(o) == names[k]) { got = names[k]; return o; }
        }
    return nullptr;
}

static bool Build() {
    kImage = FindClass("UnityEngine.UI", "Image"); kTmpUgui = FindClass("TMPro", "TextMeshProUGUI"); kCanvasK = FindClass("UnityEngine", "Canvas");
    kRect = FindClass("UnityEngine", "RectTransform");
    void* kGo = FindClass("UnityEngine", "GameObject"); void* kTr = FindClass("UnityEngine", "Transform"); void* kComp = FindClass("UnityEngine", "Component");
    void* kGraphic = FindClass("UnityEngine.UI", "Graphic"); void* kTmpText = FindClass("TMPro", "TMP_Text");
    if (!kImage || !kTmpUgui || !kCanvasK || !kRect || !kGraphic || !kTmpText) { Log("MENUUI classes missing: the mod draws the menu"); return false; }
    mSetLayer = FindMethod(kGo, "set_layer", 1); mGoSetActive = FindMethod(kGo, "SetActive", 1);
    mSetParent = FindMethod(kTr, "SetParent", 2); mGetTransform = FindMethod(kComp, "get_transform", 0);
    mGoTransform = FindMethod(kGo, "get_transform", 0);
    mAnchorMin = FindMethod(kRect, "set_anchorMin", 1); mAnchorMax = FindMethod(kRect, "set_anchorMax", 1); mPivot = FindMethod(kRect, "set_pivot", 1);
    mAnchoredPos = FindMethod(kRect, "set_anchoredPosition", 1); mSizeDelta = FindMethod(kRect, "set_sizeDelta", 1);
    mImgSprite = FindMethod(kImage, "set_sprite", 1); mImgType = FindMethod(kImage, "set_type", 1);
    mGraphicColor = FindMethod(kGraphic, "set_color", 1); mRaycast = FindMethod(kGraphic, "set_raycastTarget", 1);
    mTmpText = FindMethod(kTmpText, "set_text", 1); mTmpFont = FindMethod(kTmpText, "set_font", 1); mTmpSize = FindMethod(kTmpText, "set_fontSize", 1);
    mTmpAlign = FindMethod(kTmpText, "set_alignment", 1); mTmpColor = FindMethod(kTmpText, "set_color", 1); mTmpPrefW = FindMethod(kTmpText, "get_preferredWidth", 0); mTmpWrap = FindMethod(kTmpText, "set_enableWordWrapping", 1);
    mCanvasModeSet = FindMethod(kCanvasK, "set_renderMode", 1); mCanvasCam = FindMethod(kCanvasK, "set_worldCamera", 1);
    mCanvasPlane = FindMethod(kCanvasK, "set_planeDistance", 1); mCanvasOrder = FindMethod(kCanvasK, "set_sortingOrder", 1);
    if (!mSetLayer || !mGoSetActive || !mSetParent || !mGetTransform || !mGoTransform || !mAnchorMin || !mAnchorMax || !mPivot || !mAnchoredPos || !mSizeDelta ||
        !mImgSprite || !mImgType || !mGraphicColor || !mTmpText || !mTmpFont || !mTmpSize || !mTmpAlign || !mCanvasModeSet || !mCanvasCam || !mCanvasPlane) {
        Log("MENUUI methods missing: the mod draws the menu"); return false;
    }
    g_font = FindByName(FindClass("TMPro", "TMP_FontAsset"), kFontNames, sizeof kFontNames / sizeof *kFontNames, g_fontName);
    void* kSprite = FindClass("UnityEngine", "Sprite");
    g_sprPanel = FindByName(kSprite, kPanelSprites, sizeof kPanelSprites / sizeof *kPanelSprites, g_panelName);
    g_sprBar = FindByName(kSprite, kBarSprites, sizeof kBarSprites / sizeof *kBarSprites, g_barName);
    g_titleFont = FindByName(FindClass("TMPro", "TMP_FontAsset"), kTitleFontNames, sizeof kTitleFontNames / sizeof *kTitleFontNames, g_titleFontName);
    g_sprHeader = FindByName(kSprite, kHeaderSprites, 1, g_headerName);
    g_sprFooter = FindByName(kSprite, kFooterSprites, 1, g_footerName);
    g_sprArrow = FindByName(kSprite, kArrowSprites, 1, g_arrowName);
    if (!g_font) Log("MENUUI the game's menu font is not loaded yet (TextMeshPro's default font used)");

    // Camera and texture, like the HUD camera: orthographic, plain renderer, transparent clear, only the menu's layer.
    g_rt = MakeRT(kW, kH, "LCVR_Menu");
    Il2CppObject* camGo = NewGameObject("LCVR_MenuCam");
    g_cam = AddComp(camGo, kCamera); Il2CppObject* d = AddComp(camGo, kUrpCam);
    SetII(mCamSetClearFlags, g_cam, 2);
    P<void (*)(Il2CppObject*, Color, MethodInfo*)>(mCamSetBg)(g_cam, Color{0, 0, 0, 0}, mCamSetBg);
    SetII(mCamSetMask, g_cam, 1 << kLayer);
    P<void (*)(Il2CppObject*, float, MethodInfo*)>(mCamSetDepth)(g_cam, 60, mCamSetDepth);
    SetIO(mCamSetTarget, g_cam, g_rt);
    if (d) {
        if (oUrpRenderer >= 0) Fld<int>(d, oUrpRenderer) = 0;
        if (oUrpPost >= 0) Fld<bool>(d, oUrpPost) = false;
        if (oUrpAA >= 0) Fld<int>(d, oUrpAA) = 0;
        if (oUrpShadows >= 0) Fld<bool>(d, oUrpShadows) = false;
        if (oUrpVolMask >= 0) Fld<int>(d, oUrpVolMask) = 0;
    }
    P<void (*)(Il2CppObject*, bool, MethodInfo*)>(mCamSetOrtho)(g_cam, true, mCamSetOrtho);
    SetIF(mCamSetOrthoSize, g_cam, kH * 0.5f);
    SetIF(mCamSetNear, g_cam, 0.5f); SetIF(mCamSetFar, g_cam, 2.0f);
    P<void (*)(Il2CppObject*, Vec3, Quat, MethodInfo*)>(mTrSetPosRot)(Transform(g_cam), Vec3{kW * 0.5f, kH * 0.5f - 200000.0f, -1.0f}, Quat{0, 0, 0, 1}, mTrSetPosRot);
    SetIB(mBehSetEnabled, g_cam, false);

    g_canvasGo = NewGameObject("LCVR_MenuCanvas");
    P<void (*)(Il2CppObject*, int, MethodInfo*)>(mSetLayer)(g_canvasGo, kLayer, mSetLayer);
    Il2CppObject* canvas = AddComp(g_canvasGo, kCanvasK);
    SetII(mCanvasModeSet, canvas, 1 /*ScreenSpaceCamera*/); SetIO(mCanvasCam, canvas, g_cam); SetIF(mCanvasPlane, canvas, 1.0f);

    const Color white{0.96f, 0.96f, 1.0f, 1}, value{0.55f, 0.62f, 1.0f, 1}, dim{0.78f, 0.78f, 0.86f, 1}, faint{0.55f, 0.55f, 0.68f, 1},
                gold{1.0f, 0.84f, 0.36f, 1}, panel{0.04f, 0.03f, 0.12f, 0.96f}, bar{0.16f, 0.16f, 0.62f, 0.95f}, barDim{0.12f, 0.12f, 0.40f, 0.85f};
    g_bg = Image("LCVR_M_bg", g_sprPanel, g_sprPanel ? Color{1, 1, 1, 1} : panel, false); Place(g_bg, 0, 0, kW, kH);
    if (g_sprHeader) { El hd = Image("LCVR_M_head", g_sprHeader, Color{1, 1, 1, 1}, false); Place(hd, 0, 0, kW, 187); }
    if (g_sprFooter) { El ft = Image("LCVR_M_foot", g_sprFooter, Color{0.10f, 0.07f, 0.28f, 0.85f}, false); Place(ft, 0, kH - 134, kW, 84); }  // the game tints this white band
    g_frame = Image("LCVR_M_frame", nullptr, Color{0.35f, 0.30f, 0.85f, g_sprPanel ? 0.0f : 0.6f}, false); Place(g_frame, 0, kH - 133, kW, 2);
    g_tabHi = Image("LCVR_M_tabhi", g_sprBar, g_sprBar ? Color{1, 1, 1, 0.9f} : barDim, false);
    g_rowHi = Image("LCVR_M_rowhi", g_sprBar, g_sprBar ? Color{1, 1, 1, 1} : bar, false);
    g_title = Text("LCVR_M_title", 64, white, AlignLeft, true); Place(g_title, 60, 20, 800, 80);
    g_version = Text("LCVR_M_ver", 28, faint, AlignRight); Place(g_version, kW - 650, 30, 600, 40);
    for (int i = 0; i < 6; i++) g_tabs[i] = Text("LCVR_M_tab", 40, faint, AlignCenter, true);
    g_lb = Text("LCVR_M_lb", 28, faint, AlignLeft); Place(g_lb, 46, 116, 80, 50); SetText(g_lb, "LB");
    g_rb = Text("LCVR_M_rb", 28, faint, AlignRight); Place(g_rb, kW - 126, 116, 80, 50); SetText(g_rb, "RB");
    for (int i = 0; i < 8; i++) { g_rowLab[i] = Text("LCVR_M_lab", 40, dim, AlignLeft); g_rowVal[i] = Text("LCVR_M_val", 40, value, AlignRight); }
    if (g_sprArrow) {  // the game's own arrow; the left one mirrored
        g_arrL = Image("LCVR_M_arl", g_sprArrow, Color{1, 1, 1, 1}, false); g_arrR = Image("LCVR_M_arr", g_sprArrow, Color{1, 1, 1, 1}, false);
        MethodInfo* mScale = FindMethod(FindClass("UnityEngine", "Transform"), "set_localScale", 1);
        if (mScale) P<void (*)(Il2CppObject*, Vec3, MethodInfo*)>(mScale)(GetObj(mGetTransform, g_arrL.comp), Vec3{-1, 1, 1}, mScale);
    } else { g_arrL = Text("LCVR_M_arl", 40, gold, AlignCenter); g_arrR = Text("LCVR_M_arr", 40, gold, AlignCenter); }
    g_explain = Text("LCVR_M_exp", 30, white, AlignTopCenter); if (mTmpWrap) SetIB(mTmpWrap, g_explain.comp, true);
    Place(g_explain, 130, kH - 252, kW - 260, 120);
    g_rule = Image("LCVR_M_rule", nullptr, Color{0.35f, 0.30f, 0.85f, 0.5f}, false); Place(g_rule, 0, kH - 132, kW, 2);
    g_hint = Text("LCVR_M_hint", 30, dim, AlignCenter); Place(g_hint, 0, kH - 116, kW, 44);
    g_readout = Text("LCVR_M_read", 25, faint, AlignCenter); Place(g_readout, 0, kH - 72, kW, 40);
    Log("MENUUI built: text font '%s', title font '%s', sprites panel '%s' header '%s' footer '%s' bar '%s' arrow '%s', %dx%d texture on layer %d",
        g_fontName.c_str(), g_titleFontName.c_str(), g_panelName.c_str(), g_headerName.c_str(), g_footerName.c_str(), g_barName.c_str(), g_arrowName.c_str(), kW, kH, kLayer);
    return true;
}

// Main thread, every frame the menu snapshot is made.
static void Update(lcxr::MenuSnapshot& s) {
    if (!s.visible || !g_enabled) {
        if (g_visible && g_cam) { SetIB(mBehSetEnabled, g_cam, false); g_visible = false; }
        return;
    }
    if (!g_tried) { g_tried = true; g_ok = Build(); }
    if (!g_ok || !lcxr::ExternalMenuShare()) return;  // the XR side makes the shared texture the first time it shows the menu
    if (!g_visible) { SetIB(mBehSetEnabled, g_cam, true); g_visible = true; }
    s.external = true;
    SetText(g_title, s.problem ? "VR DID NOT START" : "VR SETTINGS");
    SetText(g_version, s.version);
    float tabY = 116, gap = 330;
    for (int i = 0; i < 6; i++) {
        SetActive(g_tabs[i], i < s.tabCount);
        if (i >= s.tabCount) continue;
        float cx = kW * 0.5f + (i - (s.tabCount - 1) * 0.5f) * gap;
        Place(g_tabs[i], cx - 160, tabY - 4, 320, 56);
        SetText(g_tabs[i], s.tabs[i]);
        Colour(g_tabs[i], i == s.tab ? Color{0.96f, 0.96f, 1, 1} : Color{0.55f, 0.55f, 0.68f, 1});
        if (i == s.tab) Place(g_tabHi, cx - 140, tabY - 2, 280, 52);
    }
    float x0 = 180, x1 = kW - 180, ry = 205, step = 54;
    for (int i = 0; i < 8; i++) {
        bool show = i < s.rowCount;
        SetActive(g_rowLab[i], show); SetActive(g_rowVal[i], show);
        if (!show) continue;
        bool on = i == s.row;
        Place(g_rowLab[i], x0, ry + i * step, 700, 50);
        SetText(g_rowLab[i], s.rows[i].label);
        Colour(g_rowLab[i], on ? Color{0.96f, 0.96f, 1, 1} : Color{0.78f, 0.78f, 0.86f, 1});
        float vr = (on && !s.rows[i].button) ? x1 - 30 : x1;
        Place(g_rowVal[i], vr - 500, ry + i * step, 500, 50);
        SetText(g_rowVal[i], s.rows[i].value);
        Colour(g_rowVal[i], on ? Color{0.96f, 0.96f, 1, 1} : Color{0.55f, 0.62f, 1.0f, 1});
        if (on) Place(g_rowHi, x0 - 60, ry + i * step - 2, (x1 - x0) + 120, 52);
    }
    bool arrows = s.row < s.rowCount && !s.rows[s.row].button && s.rows[s.row].value[0];
    SetActive(g_arrL, arrows); SetActive(g_arrR, arrows);
    if (arrows) {
        // the left arrow sits just before the value: TMP's preferred width is not read back here, so it goes at a fixed
        // place left of the value column, as the value is right-aligned
        float vw = mTmpPrefW ? P<float (*)(Il2CppObject*, MethodInfo*)>(mTmpPrefW)(g_rowVal[s.row].comp, mTmpPrefW) : 200.0f;
        if (vw <= 0 || vw > 500) vw = 200.0f;
        Place(g_arrL, x1 - 30 - vw - 54, ry + s.row * step + 1, 48, 48); Place(g_arrR, x1 + 2, ry + s.row * step + 1, 48, 48);
        if (!g_sprArrow) { SetText(g_arrL, "<"); SetText(g_arrR, ">"); }
    }
    SetText(g_explain, s.explain);
    SetText(g_hint, s.hint);
    SetText(g_readout, s.readout);
}

// Render thread: the menu texture into the XR side's shared texture (key 0 in, key 1 out; never waits).
static void* g_menuTexNative;
static ID3D11Texture2D* g_shared; static IDXGIKeyedMutex* g_sharedMx; static void* g_sharedFor;
static bool g_copyLogged = false;
static void __stdcall OnCopy(int) {
    void* h = lcxr::ExternalMenuShare();
    ID3D11Texture2D* src = (ID3D11Texture2D*)g_menuTexNative;
    if (!h || !src) return;
    if (g_sharedFor != h) {
        ID3D11Device* dev = nullptr; src->GetDevice(&dev);
        if (g_shared) { g_shared->Release(); g_shared = nullptr; } if (g_sharedMx) { g_sharedMx->Release(); g_sharedMx = nullptr; }
        if (FAILED(dev->OpenSharedResource(h, __uuidof(ID3D11Texture2D), (void**)&g_shared))) { dev->Release(); Log("MENUUI could not open the shared menu texture"); g_sharedFor = h; return; }
        g_shared->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&g_sharedMx);
        dev->Release(); g_sharedFor = h;
    }
    if (!g_shared || !g_sharedMx) return;
    if (g_sharedMx->AcquireSync(0, 0) != S_OK) return;
    ID3D11Device* dev = nullptr; src->GetDevice(&dev); ID3D11DeviceContext* ctx = nullptr; dev->GetImmediateContext(&ctx);
    ctx->CopyResource(g_shared, src);
    ctx->Release(); dev->Release();
    g_sharedMx->ReleaseSync(1);
    if (!g_copyLogged) { g_copyLogged = true; Log("MENUUI first menu picture handed to the XR side"); }
}
// Main thread, after the frame's render: queue the copy while the menu is shown.
static void AfterRender() {
    if (!g_visible || !g_rt) return;
    g_menuTexNative = TexPtr(g_rt);
    g_issue((void*)&OnCopy, 300);
}

}  // namespace menuui

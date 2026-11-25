// Prince of Persia: The Lost Crown VR, native mod (build 1, Phase 1, route B: no BepInEx).
// Loaded by the winhttp.dll proxy, which hands this DLL one call on the game's main thread once IL2CPP runs (the game's
// IL2CPP exports are Denuvo wrappers and are never hooked or patched). Everything is found by IL2CPP name. One game
// method is hooked: URP's UniversalRenderPipeline.Render(ScriptableRenderContext, List<Camera>), in the game's plain
// code section (checked at run time).
// The diorama (Ori VR's design): a flat rectangle fixed in the room (the stage) shows what the flat game shows on its
// gameplay plane; each eye looks through it from its own place. Two eye cameras copied from the game camera render in
// the same frame, posed in the frame's first Render call (after every game update), so both eyes show the same tick.
// The game's HUD (Screen Space Overlay canvases) is drawn by the mod's HUD camera into its own texture, which lies
// exactly over the stage. OpenXR runs on its own thread (xr\LcXR.cpp).
// Ported from Ori WotW VR and AI Limit VR (same author, MIT). MinHook (TsudaKageyu, BSD-2) is compiled in.
#include <windows.h>
#include <d3d11.h>
#include <timeapi.h>
#include <mmsystem.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <set>
#include <mutex>
#include <atomic>
#include <d3dcompiler.h>
#include <fstream>
#include <sstream>
#include "MinHook.h"
#include "../xr/LcXR.h"

#define MOD_VERSION "1.0"
#define MOD_BUILD 10

// ---------------------------------------------------------------- log
static std::wstring g_gameDir;
static std::mutex g_logMx;
static FILE* g_log = nullptr;
static LARGE_INTEGER g_qpf, g_qp0;
static double Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return double(t.QuadPart - g_qp0.QuadPart) / g_qpf.QuadPart; }
static void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMx);
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

// ---------------------------------------------------------------- settings (LostCrownVR\LostCrownVR.ini)
struct Settings {
    float stageWidth = 1.85f, stageDistance = 1.0f, depth = 1.5f, resolution = 1.0f, tanSpan = 0;
    float popOut = 1.0f;        // 0 keeps the player's depth on the stage; higher brings more of the scene toward you (Ori VR; menu STAGE tab, build 8)
    int surround = 0;           // 0 Black, 1 Green (Virtual Desktop chroma key), 2 Transparent (menu PICTURE tab, build 9; Ori WotW VR)
    float sharpen = 1.0f;       // sharpen pass strength, 0 off (menu: PICTURE tab, build 6; Ori WotW VR's default)
    int picture = 0;            // 0 Auto (Steady on Virtual Desktop, Panel elsewhere), 1 Steady, 2 Panel (menu: PICTURE tab, build 5)
    float menuSize = 1.0f, menuHeight = 0.0f, menuDistance = 0.8f;
    int labCommands = 0;        // [Lab] Commands=1: test commands from vrlab_command.txt (lab only)
    // Build 2, cutscenes (a build 1 headset test): where the eyes converge and how fast that follows.
    int planeSource = 1;        // 0 always the player (build 1), 1 cutscenes: what the active camera looks at, else hold
    float cutsceneDepth = 1.0f; // depth in cutscenes, times Depth
    float smoothSec = 0.25f;    // convergence distance follows over this time (camera cuts jump at once)
    // Edge match (after build 2: one eye saw slightly further at the stage edges). Cuphead VR's fix: each eye's
    // outer edge is blacked out by how far content at the frame depth shifts between the eyes, so the stage frame
    // floats at that depth and both eyes see the same picture at the edges.
    // Build 7: the menu's Edge match is now a fade (edgeFade). The strip stays as a lab test only (edgeMatch, off):
    // on 2026-11-24 the content at the stage edges was behind the plane at the top and in front at the bottom in the same
    // frame, so any strip was wrong somewhere; in headset tests build 5's strip made the edges worse.
    int edgeMatch = 0;
    // Both eyes fade to black over this share of the stage width at each side edge (the same in both eyes, so no band
    // only one eye sees). 0 off. Tests: 6% cut the difference between the eyes near the edges by about 42%.
    float edgeFade = 0.0f;      // lab only since build 10 ("edgefade")
    int edgeMask = 1;           // build 10: each eye shows only the world both eyes see (menu STAGE tab, Edge match On / Off)
    // Frame depth, times the gameplay plane's distance. Behind the plane (> 1): the inner edges are trimmed (the right
    // eye's left edge, the left eye's right edge), for the background seen past the stage edge. In front (< 1): the outer
    // edges, for foreground in front of the stage (Cuphead VR's case). In the prologue, at the stage edges the right eye
    // saw 46 px of 3888 more on the left and the left eye more on the right: background, behind the plane.
    float edgeDepth = 2.0f;
};
static const Settings kDefaults;
static Settings g_set;
static std::wstring IniPath() { return g_gameDir + L"LostCrownVR\\LostCrownVR.ini"; }
static float IniF(const wchar_t* sec, const wchar_t* key, float def) {
    wchar_t b[64], d[64]; swprintf_s(d, L"%g", def);
    GetPrivateProfileStringW(sec, key, d, b, 64, IniPath().c_str()); return (float)_wtof(b);
}
static void IniW(const wchar_t* sec, const wchar_t* key, float v) {
    wchar_t b[64]; swprintf_s(b, L"%g", v); WritePrivateProfileStringW(sec, key, b, IniPath().c_str());
}
static void LoadSettings() {
    g_set.stageWidth = IniF(L"Stage", L"Width", kDefaults.stageWidth);
    g_set.stageDistance = IniF(L"Stage", L"Distance", kDefaults.stageDistance);
    g_set.depth = IniF(L"Stage", L"Depth", kDefaults.depth);
    g_set.resolution = IniF(L"VR", L"Resolution", kDefaults.resolution);
    g_set.picture = (int)IniF(L"VR", L"Picture", 0);
    g_set.sharpen = IniF(L"VR", L"Sharpen", kDefaults.sharpen);
    g_set.popOut = IniF(L"Stage", L"PopOut", kDefaults.popOut);
    g_set.surround = (int)IniF(L"VR", L"Surround", 0);
    g_set.tanSpan = IniF(L"VR", L"EyeSpanLastRun", 0);
    g_set.menuSize = IniF(L"Menu", L"Size", kDefaults.menuSize);
    g_set.menuHeight = IniF(L"Menu", L"Height", kDefaults.menuHeight);
    g_set.menuDistance = IniF(L"Menu", L"Distance", kDefaults.menuDistance);
    g_set.labCommands = (int)IniF(L"Lab", L"Commands", 0);
    g_set.planeSource = (int)IniF(L"Stage", L"CutscenePlane", 1);
    g_set.cutsceneDepth = IniF(L"Stage", L"CutsceneDepth", 1.0f);
    g_set.smoothSec = IniF(L"Stage", L"PlaneSmoothing", 0.25f);
    g_set.edgeMatch = (int)IniF(L"Lab", L"EdgeStrip", 0);  // the old [Stage] EdgeMatch key is no longer read
    g_set.edgeFade = 0;  // the fade is no longer a setting
    g_set.edgeMask = (int)IniF(L"Stage", L"EdgeMask", 1);
    g_set.edgeDepth = IniF(L"Stage", L"EdgeDepth", 2.0f);
}
static void SaveSettings() {
    IniW(L"Stage", L"Width", g_set.stageWidth); IniW(L"Stage", L"Distance", g_set.stageDistance); IniW(L"Stage", L"Depth", g_set.depth);
    IniW(L"VR", L"Resolution", g_set.resolution); IniW(L"VR", L"Picture", (float)g_set.picture); IniW(L"VR", L"Sharpen", g_set.sharpen); IniW(L"Stage", L"PopOut", g_set.popOut); IniW(L"VR", L"Surround", (float)g_set.surround);
    IniW(L"Menu", L"Size", g_set.menuSize); IniW(L"Menu", L"Height", g_set.menuHeight); IniW(L"Menu", L"Distance", g_set.menuDistance);
    IniW(L"Stage", L"CutscenePlane", (float)g_set.planeSource); IniW(L"Stage", L"CutsceneDepth", g_set.cutsceneDepth); IniW(L"Stage", L"PlaneSmoothing", g_set.smoothSec);
    IniW(L"Stage", L"EdgeMask", (float)g_set.edgeMask); IniW(L"Stage", L"EdgeDepth", g_set.edgeDepth);
}

// ---------------------------------------------------------------- il2cpp
struct Il2CppObject { void* klass; void* monitor; };
struct Il2CppArray { void* klass; void* monitor; void* bounds; uintptr_t max_length; void* data[1]; };
struct MethodInfo { void* methodPointer; };
struct Vec3 { float x, y, z; };
struct Quat { float x, y, z, w; };
struct Color { float r, g, b, a; };
struct Mat4 { float m[16]; };  // Unity column-major: element (row r, col c) at m[c*4+r]
static float& M(Mat4& a, int r, int c) { return a.m[c * 4 + r]; }

#define IL2CPP_FN(ret, name, args) typedef ret(*name##_t) args; static name##_t name;
IL2CPP_FN(void*, il2cpp_domain_get, ())
IL2CPP_FN(void**, il2cpp_domain_get_assemblies, (void*, size_t*))
IL2CPP_FN(void*, il2cpp_assembly_get_image, (void*))
IL2CPP_FN(void*, il2cpp_class_from_name, (void*, const char*, const char*))
IL2CPP_FN(MethodInfo*, il2cpp_class_get_methods, (void*, void**))
IL2CPP_FN(const char*, il2cpp_method_get_name, (MethodInfo*))
IL2CPP_FN(uint32_t, il2cpp_method_get_param_count, (MethodInfo*))
IL2CPP_FN(void*, il2cpp_method_get_param, (MethodInfo*, uint32_t))
IL2CPP_FN(char*, il2cpp_type_get_name, (void*))
IL2CPP_FN(void*, il2cpp_resolve_icall, (const char*))
IL2CPP_FN(Il2CppObject*, il2cpp_object_new, (void*))
IL2CPP_FN(void*, il2cpp_class_get_field_from_name, (void*, const char*))
IL2CPP_FN(size_t, il2cpp_field_get_offset, (void*))
IL2CPP_FN(void, il2cpp_field_static_get_value, (void*, void*))
IL2CPP_FN(uint32_t, il2cpp_gchandle_new, (Il2CppObject*, bool))
IL2CPP_FN(void*, il2cpp_class_get_parent, (void*))
IL2CPP_FN(void, il2cpp_free, (void*))
IL2CPP_FN(void*, il2cpp_class_get_type, (void*))
IL2CPP_FN(Il2CppObject*, il2cpp_type_get_object, (void*))
IL2CPP_FN(void*, il2cpp_object_get_class, (Il2CppObject*))
IL2CPP_FN(const char*, il2cpp_class_get_name, (void*))
IL2CPP_FN(Il2CppObject*, il2cpp_string_new, (const char*))

static HMODULE g_ga;
static bool BindIl2cpp() {
#define B(n) n = (n##_t)GetProcAddress(g_ga, #n); if (!n) { Log("IL2CPP export missing: " #n); return false; }
    B(il2cpp_domain_get) B(il2cpp_domain_get_assemblies) B(il2cpp_assembly_get_image) B(il2cpp_class_from_name)
    B(il2cpp_class_get_methods) B(il2cpp_method_get_name) B(il2cpp_method_get_param_count) B(il2cpp_method_get_param)
    B(il2cpp_type_get_name) B(il2cpp_resolve_icall) B(il2cpp_object_new) B(il2cpp_class_get_field_from_name) B(il2cpp_field_get_offset)
    B(il2cpp_field_static_get_value) B(il2cpp_gchandle_new) B(il2cpp_class_get_parent) B(il2cpp_free)
    B(il2cpp_class_get_type) B(il2cpp_type_get_object) B(il2cpp_object_get_class) B(il2cpp_class_get_name) B(il2cpp_string_new)
#undef B
    return true;
}
static void* FindClass(const char* ns, const char* name) {
    size_t n = 0;
    void** as = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &n);
    for (size_t i = 0; i < n; i++) { void* k = il2cpp_class_from_name(il2cpp_assembly_get_image(as[i]), ns, name); if (k) return k; }
    return nullptr;
}
static MethodInfo* FindMethod(void* klass, const char* name, int argc, const char* types = nullptr) {
    for (void* k = klass; k; k = il2cpp_class_get_parent(k)) {
        void* it = nullptr;
        while (MethodInfo* m = il2cpp_class_get_methods(k, &it)) {
            if (strcmp(il2cpp_method_get_name(m), name) != 0) continue;
            if (argc >= 0 && (int)il2cpp_method_get_param_count(m) != argc) continue;
            if (types) {
                std::string sig;
                for (int i = 0; i < argc; i++) { char* tn = il2cpp_type_get_name(il2cpp_method_get_param(m, i)); if (i) sig += ","; sig += tn; il2cpp_free(tn); }
                if (sig != types) continue;
            }
            return m;
        }
    }
    return nullptr;
}
static int g_missing = 0;
static MethodInfo* Need(void* klass, const char* kname, const char* name, int argc, const char* types = nullptr) {
    MethodInfo* m = klass ? FindMethod(klass, name, argc, types) : nullptr;
    if (!m) { Log("HOOKMAP %s.%s(%d) NOT FOUND", kname, name, argc); g_missing++; }
    return m;
}
static MethodInfo* Opt(void* klass, const char* name, int argc) { return klass ? FindMethod(klass, name, argc) : nullptr; }
static int Field(void* klass, const char* kname, const char* name, bool required = true) {
    void* f = klass ? il2cpp_class_get_field_from_name(klass, name) : nullptr;
    int off = f ? (int)il2cpp_field_get_offset(f) : -1;
    if (!f) { Log("HOOKMAP field %s.%s %s", kname, name, required ? "NOT FOUND" : "not found (optional)"); if (required) g_missing++; }
    return off;
}
template <class F> F P(MethodInfo* m) { return (F)m->methodPointer; }
template <class T> static T& Fld(Il2CppObject* o, int off) { return *(T*)((uint8_t*)o + off); }
static Il2CppObject* TypeOf(void* klass) { return il2cpp_type_get_object(il2cpp_class_get_type(klass)); }
static const char* ClassName(Il2CppObject* o) { return o ? il2cpp_class_get_name(il2cpp_object_get_class(o)) : "null"; }
static Il2CppObject* Keep(Il2CppObject* o) { if (o) il2cpp_gchandle_new(o, false); return o; }

// ---------------------------------------------------------------- game API, found by name in Setup
static void *kCamera, *kUrpCam, *kCanvas, *kGameObject, *kCamSys;
static MethodInfo *mObjName, *mObjFindAll, *mObjDontDestroy, *mObjAlive, *mSetHideFlags, *mCompTransform, *mCompGet, *mGoCtor, *mGoAdd,
    *mTrPos, *mTrFwd, *mTrSetPosRot, *mBehSetEnabled, *mCamGetTarget, *mCamSetTarget, *mCamCopyFrom, *mCamGetDepth, *mCamSetDepth,
    *mCamGetMask, *mCamSetMask, *mCamGetW2C, *mCamSetW2C, *mCamGetProj, *mCamSetProj, *mCamSetOrtho, *mCamSetOrthoSize,
    *mCamSetNear, *mCamSetFar, *mCamSetClearFlags, *mCamSetBg, *mRTCtor, *mRTCreate, *mTexNative,
    *mCanvasMode, *mCanvasSetMode, *mCanvasSetCam, *mCanvasGetCam, *mCanvasSetPlane, *mCanvasRoot,
    *mTimeTime, *mTimeScale, *mSetTimeScale, *mTimeFixedDelta, *mSetFixedDelta, *mTimeMaxDelta, *mTimeFrame,
    *mVSync, *mSetVSync, *mTargetFps, *mSetTargetFps, *mScreenW, *mScreenH, *mPrefsGetInt, *mFrlEnable, *mFrlRate,
    *mVolMgrInstance, *mListEnabledActions, *mActionEnable, *mActionDisable, *mRender;
static int oCsCamera = -1, oCsUiCamera = -1, oCsDefault = -1, oCsBrain = -1;
static MethodInfo* mBrainActive;
static void* kVcamBase;
static int oCsFollow = -1, oUrpRenderer = -1, oUrpPost = -1, oUrpAA = -1, oUrpAAQ = -1, oUrpShadows = -1, oUrpVolMask = -1, oUrpVolTrig = -1,
    oUrpDepthOpt = -1, oUrpColorOpt = -1, oUrpStopNaN = -1, oUrpDither = -1, oVolMgrVolumes = -1, oVolProfile = -1, oProfComps = -1, oCompActive = -1;
static const int oListItems = 0x10, oListSize = 0x18;
typedef void (*IssuePluginEvent_t)(void* cb, int id);
static IssuePluginEvent_t g_issue;
typedef void* (*TexNativeIcall_t)(Il2CppObject*);
static TexNativeIcall_t g_texNativeIcall;

static Il2CppObject* GetObj(MethodInfo* m, Il2CppObject* o) { return P<Il2CppObject* (*)(Il2CppObject*, MethodInfo*)>(m)(o, m); }
static float SF(MethodInfo* m) { return P<float (*)(MethodInfo*)>(m)(m); }
static int SI(MethodInfo* m) { return P<int (*)(MethodInfo*)>(m)(m); }
static bool SB(MethodInfo* m) { return P<bool (*)(MethodInfo*)>(m)(m); }
static void SetSI(MethodInfo* m, int v) { P<void (*)(int, MethodInfo*)>(m)(v, m); }
static void SetSF(MethodInfo* m, float v) { P<void (*)(float, MethodInfo*)>(m)(v, m); }
static float IF(MethodInfo* m, Il2CppObject* o) { return P<float (*)(Il2CppObject*, MethodInfo*)>(m)(o, m); }
static int II(MethodInfo* m, Il2CppObject* o) { return P<int (*)(Il2CppObject*, MethodInfo*)>(m)(o, m); }
static bool IB(MethodInfo* m, Il2CppObject* o) { return P<bool (*)(Il2CppObject*, MethodInfo*)>(m)(o, m); }
static void SetIB(MethodInfo* m, Il2CppObject* o, bool v) { P<void (*)(Il2CppObject*, bool, MethodInfo*)>(m)(o, v, m); }
static void SetII(MethodInfo* m, Il2CppObject* o, int v) { P<void (*)(Il2CppObject*, int, MethodInfo*)>(m)(o, v, m); }
static void SetIF(MethodInfo* m, Il2CppObject* o, float v) { P<void (*)(Il2CppObject*, float, MethodInfo*)>(m)(o, v, m); }
static void SetIO(MethodInfo* m, Il2CppObject* o, Il2CppObject* v) { P<void (*)(Il2CppObject*, Il2CppObject*, MethodInfo*)>(m)(o, v, m); }
static Mat4 GetM(MethodInfo* m, Il2CppObject* o) { return P<Mat4 (*)(Il2CppObject*, MethodInfo*)>(m)(o, m); }
static void SetM(MethodInfo* m, Il2CppObject* o, Mat4 v) { P<void (*)(Il2CppObject*, Mat4, MethodInfo*)>(m)(o, v, m); }
static bool Alive(Il2CppObject* o) { return o && P<bool (*)(Il2CppObject*, MethodInfo*)>(mObjAlive)(o, mObjAlive); }

struct Il2CppStr { void* k; void* m; int32_t len; wchar_t c[1]; };
static std::string Str(Il2CppObject* so) {
    if (!so) return "null";
    Il2CppStr* st = (Il2CppStr*)so; std::string o;
    for (int i = 0; i < st->len && i < 120; i++) o += st->c[i] < 128 ? (char)st->c[i] : '?';
    return o;
}
static std::string Name(Il2CppObject* o) { return o ? Str(GetObj(mObjName, o)) : "null"; }
static Il2CppObject* Transform(Il2CppObject* c) { return GetObj(mCompTransform, c); }
static Vec3 TrPos(Il2CppObject* t) { return P<Vec3 (*)(Il2CppObject*, MethodInfo*)>(mTrPos)(t, mTrPos); }
static Vec3 TrFwd(Il2CppObject* t) { return P<Vec3 (*)(Il2CppObject*, MethodInfo*)>(mTrFwd)(t, mTrFwd); }
static Il2CppObject* GetComp(Il2CppObject* c, void* klass) { return P<Il2CppObject* (*)(Il2CppObject*, Il2CppObject*, MethodInfo*)>(mCompGet)(c, TypeOf(klass), mCompGet); }
static Il2CppArray* FindAll(void* klass) { return P<Il2CppArray* (*)(Il2CppObject*, MethodInfo*)>(mObjFindAll)(TypeOf(klass), mObjFindAll); }
static void* TexPtr(Il2CppObject* t) { return mTexNative ? P<void* (*)(Il2CppObject*, MethodInfo*)>(mTexNative)(t, mTexNative) : g_texNativeIcall(t); }
static int PrefInt(const char* key, int def) { return P<int (*)(Il2CppObject*, int, MethodInfo*)>(mPrefsGetInt)(il2cpp_string_new(key), def, mPrefsGetInt); }

static Il2CppObject* CameraSystem() {
    static void* f = nullptr;
    if (!f && kCamSys) { void* par = il2cpp_class_get_parent(kCamSys); f = par ? il2cpp_class_get_field_from_name(par, "m_instance") : nullptr; }
    Il2CppObject* o = nullptr;
    if (f) il2cpp_field_static_get_value(f, &o);
    return o;
}

static Il2CppObject* MakeRT(int w, int h, const char* name) {
    Il2CppObject* rt = Keep(il2cpp_object_new(FindClass("UnityEngine", "RenderTexture")));
    P<void (*)(Il2CppObject*, int, int, int, int, int, MethodInfo*)>(mRTCtor)(rt, w, h, 24, 0 /*ARGB32*/, 2 /*sRGB*/, mRTCtor);
    P<bool (*)(Il2CppObject*, MethodInfo*)>(mRTCreate)(rt, mRTCreate);
    // HideFlags.DontUnloadUnusedAsset: a game's Resources.UnloadUnusedAssets destroyed eye textures in Ori WotW VR.
    if (mSetHideFlags) P<void (*)(Il2CppObject*, int, MethodInfo*)>(mSetHideFlags)(rt, 32, mSetHideFlags);
    Log("EYES texture %s %dx%d ARGB32 sRGB", name, w, h);
    return rt;
}
static Il2CppObject* NewGameObject(const char* name) {
    Il2CppObject* go = Keep(il2cpp_object_new(kGameObject));
    P<void (*)(Il2CppObject*, Il2CppObject*, MethodInfo*)>(mGoCtor)(go, il2cpp_string_new(name), mGoCtor);
    P<void (*)(Il2CppObject*, MethodInfo*)>(mObjDontDestroy)(go, mObjDontDestroy);
    return go;
}
static Il2CppObject* AddComp(Il2CppObject* go, void* klass) { return Keep(P<Il2CppObject* (*)(Il2CppObject*, Il2CppObject*, MethodInfo*)>(mGoAdd)(go, TypeOf(klass), mGoAdd)); }

// ---------------------------------------------------------------- lab snapshots (render thread readback, raw files)
struct Snap { ID3D11Texture2D* tex; std::string name; };
static std::mutex g_snapMx;
static std::vector<Snap> g_snapQueue;
static void WriteSnap(ID3D11Texture2D* src, const std::string& name) {
    ID3D11Device* dev = nullptr; src->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr; dev->GetImmediateContext(&ctx);
    D3D11_TEXTURE2D_DESC d; src->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st))) {
        ctx->CopyResource(st, src);
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &map))) {
            std::wstring dir = g_gameDir + L"LostCrownVR\\snaps\\";
            CreateDirectoryW(dir.c_str(), nullptr);
            FILE* f = _wfopen((dir + std::wstring(name.begin(), name.end()) + L".raw").c_str(), L"wb");
            if (f) {
                uint32_t hdr[4] = {d.Width, d.Height, (uint32_t)d.Format, 4};
                fwrite(hdr, 4, 4, f);
                for (UINT y = 0; y < d.Height; y++) fwrite((uint8_t*)map.pData + y * map.RowPitch, 1, d.Width * 4, f);
                fclose(f);
            }
            ctx->Unmap(st, 0);
            Log("SNAP %s %ux%u format %d written", name.c_str(), d.Width, d.Height, (int)d.Format);
        }
        st->Release();
    }
    ctx->Release(); dev->Release();
}
// Lab probe: the eye textures' own depth buffers, read back on the render thread (is the scene's depth there, or does
// URP render into its own targets and copy colour only?).
typedef void* (*NativeDepthIcall_t)(Il2CppObject*);
static NativeDepthIcall_t g_rtDepthIcall;
static void* g_probeDepth[2];
static void __stdcall OnDepthProbe(int) {
    for (int e = 0; e < 2; e++) {
        ID3D11Resource* res = (ID3D11Resource*)g_probeDepth[e];
        if (!res) { Log("DEPTHPROBE eye %d: no native depth pointer", e); continue; }
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) || !tex) { Log("DEPTHPROBE eye %d: pointer %p is not a 2D texture", e, res); continue; }
        D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
        Log("DEPTHPROBE eye %d: %ux%u format %d bind 0x%X samples %u mips %u", e, d.Width, d.Height, (int)d.Format, d.BindFlags, d.SampleDesc.Count, d.MipLevels);
        ID3D11Device* dev = nullptr; tex->GetDevice(&dev); ID3D11DeviceContext* ctx = nullptr; dev->GetImmediateContext(&ctx);
        D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
        ID3D11Texture2D* st = nullptr;
        if (d.SampleDesc.Count == 1 && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st))) {
            ctx->CopyResource(st, tex);
            D3D11_MAPPED_SUBRESOURCE map;
            if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &map))) {
                // format 19 (R32G8X24): 8 bytes a texel, the first 4 a float depth (Unity: reversed, near 1, far 0)
                UINT bpt = d.Format == DXGI_FORMAT_R32G8X24_TYPELESS ? 8 : 4;
                float mn = 1e9f, mx = -1e9f; uint32_t zeros = 0, n = 0;
                for (UINT y = 0; y < d.Height; y += 8) for (UINT x = 0; x < d.Width; x += 8) {
                    const uint8_t* px = (uint8_t*)map.pData + y * map.RowPitch + x * bpt;
                    float v = bpt == 8 ? *(const float*)px : (float)(*(const uint32_t*)px & 0xFFFFFF) / 16777215.0f; n++;
                    if (v < mn) mn = v; if (v > mx) mx = v; if (v == 0) zeros++;
                }
                Log("DEPTHPROBE eye %d: depth min %.6f max %.6f, exactly 0 in %u of %u samples (%u bytes a texel)", e, mn, mx, zeros, n, bpt);
                std::wstring dir = g_gameDir + L"LostCrownVR\\snaps\\"; CreateDirectoryW(dir.c_str(), nullptr);
                FILE* f = _wfopen((dir + (e ? L"depth_R.raw" : L"depth_L.raw")).c_str(), L"wb");
                if (f) { uint32_t hdr[4] = {d.Width, d.Height, (uint32_t)d.Format, bpt}; fwrite(hdr, 4, 4, f);
                    for (UINT y = 0; y < d.Height; y++) fwrite((uint8_t*)map.pData + y * map.RowPitch, 1, d.Width * bpt, f); fclose(f); }
                ctx->Unmap(st, 0);
            }
            st->Release();
        } else Log("DEPTHPROBE eye %d: could not read back", e);
        ctx->Release(); dev->Release(); tex->Release();
    }
}
static bool g_wantDepthProbe = false;
static void UiProbe();
static void __stdcall OnSnapEvent(int) {
    std::vector<Snap> q;
    { std::lock_guard<std::mutex> lk(g_snapMx); q.swap(g_snapQueue); }
    for (auto& s : q) WriteSnap(s.tex, s.name);
}

// ---------------------------------------------------------------- GPU time of the eye renders (not under Virtual Desktop)
struct GpuSlot { ID3D11Query *disjoint = nullptr, *t0 = nullptr, *t1 = nullptr; bool pending = false; };
static GpuSlot g_gpu[6]; static int g_gpuW = 0;
static std::mutex g_gpuMx; static double g_gpuSum = 0, g_gpuMax = 0; static uint32_t g_gpuN = 0;
static ID3D11Device* g_gameDev; static ID3D11DeviceContext* g_gameCtx;
static bool g_gpuTiming = false;
static void __stdcall OnGpuEvent(int id) {
    if (!g_gameCtx) return;
    if (!g_gpu[0].disjoint) {
        D3D11_QUERY_DESC dj{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, ts{D3D11_QUERY_TIMESTAMP, 0};
        for (auto& g : g_gpu) { g_gameDev->CreateQuery(&dj, &g.disjoint); g_gameDev->CreateQuery(&ts, &g.t0); g_gameDev->CreateQuery(&ts, &g.t1); }
    }
    GpuSlot& g = g_gpu[g_gpuW];
    if (id == 10) {
        if (g.pending) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 a, b;
            if (g_gameCtx->GetData(g.disjoint, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                g_gameCtx->GetData(g.t0, &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                g_gameCtx->GetData(g.t1, &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && !dj.Disjoint && dj.Frequency) {
                double ms = (double)(b - a) * 1000.0 / dj.Frequency;
                std::lock_guard<std::mutex> lk(g_gpuMx); g_gpuSum += ms; if (ms > g_gpuMax) g_gpuMax = ms; g_gpuN++;
            }
        }
        g_gameCtx->Begin(g.disjoint); g_gameCtx->End(g.t0);
    } else {
        g_gameCtx->End(g.t1); g_gameCtx->End(g.disjoint); g.pending = true;
        g_gpuW = (g_gpuW + 1) % 6;
    }
}

// ---------------------------------------------------------------- state
static bool g_vrOn = true;           // lab: "vr 0" stops eye rendering (flat game)
static bool g_flatToo = false;       // lab: "flat 1" keeps the game's own flat render while VR runs
static bool g_wantSnap = false, g_eyeCheck = false, g_hudPanel = true, g_useCamSys = true;
// Layers the HUD camera draws: all. The game's root UI canvas is on layer 0, not UI; the HUD camera sits far below the
// level with a 1.5 unit depth range, so no level geometry is in its view.
static int g_hudMask = 0x7FFFFFFF;
static int g_lastFrame = -1, g_framesSeen = 0, g_posedFrame = -1;
static Il2CppObject *g_eyeCam[2], *g_eyeRT[2], *g_hudCam, *g_hudRT, *g_gameCam;
static int g_eyeW = 0, g_eyeH = 0, g_hudW = 1920, g_hudH = 1080;
static uint64_t g_snapCount = 0;
static uint32_t g_renderCalls = 0, g_eyePairs = 0, g_flatSkipped = 0, g_webBlocked = 0;
static bool g_vrStarted = false;
static double g_vrAskedAt = -1;
static lcxr::Views g_usedViews;
static double g_capNext = 0, g_capWaitSum = 0, g_capWaitMax = 0, g_hookSum = 0, g_hookMax = 0, g_lastFrameT = 0, g_frameMax = 0;
static uint32_t g_capped = 0, g_late = 0;
static double g_bestPeriod = 0, g_capPeriod = 0, g_lowSince = -1;
static double g_scanMax = 0, g_capThisFrame = 0;
static int g_gameCamSeen = -10;
static float g_edgeTrim = 0;
static bool g_menuFlat = true;
static Il2CppObject* g_lastVcam = nullptr;
static float g_fovNow = 0;
static bool g_inCutscene = false;
static float g_gameplayDist = 0, g_smoothDist = 0;


#include "MenuUI.inc"
#include "Menu.inc"

// ---------------------------------------------------------------- the game's pacing: VSync, frame limit, physics step
// The mod paces the game (its own cap at the headset rate): VSync and the frame limit off, checked every frame, since the
// game's own options and its FrameRateLimiter can set them again. The game sets its physics step (Time.fixedDeltaTime)
// to the display's refresh rate (1/144 on a 144 Hz display); in VR it is held at the headset's period, so every
// frame gets the same number of physics steps.
static float g_gameFixed = 0;
static void LogGameSet(const char* when) {
    Log("GAMESET %s: vSyncCount %d, targetFrameRate %d, fixedDeltaTime %.5f (game's own %.5f), maximumDeltaTime %.3f, timeScale %.2f, screen %dx%d, "
        "FrameRateLimiter.Enable %d LimitedFrameRate %d, prefs AA %d DOF %d Shadow %d",
        when, SI(mVSync), SI(mTargetFps), SF(mTimeFixedDelta), g_gameFixed, SF(mTimeMaxDelta), SF(mTimeScale), SI(mScreenW), SI(mScreenH),
        mFrlEnable ? (int)SB(mFrlEnable) : -1, mFrlRate ? SI(mFrlRate) : -1, PrefInt("RendererManager_AAMode2", -1), PrefInt("RendererManager_DOF", -1), PrefInt("RendererManager_Shadow", -1));
}
static void KeepPacing() {
    bool changed = false;
    if (SI(mVSync) != 0) { SetSI(mSetVSync, 0); changed = true; }
    if (SI(mTargetFps) != -1) { SetSI(mSetTargetFps, -1); changed = true; }
    float fixedNow = SF(mTimeFixedDelta);
    if (g_capPeriod > 0) {
        float want = (float)g_capPeriod;
        if (fabsf(fixedNow - want) > 1e-6f) {
            if (g_gameFixed == 0 || fabsf(fixedNow - g_gameFixed) > 1e-6f) g_gameFixed = fixedNow;
            SetSF(mSetFixedDelta, want);
            changed = true;
        }
    }
    static uint32_t logged = 0;
    if (changed && logged++ < 20) LogGameSet("VR pacing set (VSync off, frame limit off, physics step = headset period)");
}

// ---------------------------------------------------------------- frame cap at the headset's real rate (Ori WotW VR)
static void Cap(double periodSec) {
    double now = Now();
    if (periodSec < 1.0 / 250 || periodSec > 1.0 / 30) periodSec = g_bestPeriod;
    if (periodSec <= 0) return;
    if (g_bestPeriod == 0 || periodSec < g_bestPeriod * 0.98) { g_bestPeriod = periodSec; g_capPeriod = periodSec; g_lowSince = -1; Log("CAP headset rate %.1f Hz: the mod caps the game there", 1.0 / periodSec); }
    else if (periodSec > g_bestPeriod * 1.02) {
        double ratio = periodSec / g_bestPeriod;
        bool smoothing = fabs(ratio - 2) < 0.05 || fabs(ratio - 1.5) < 0.05 || fabs(ratio - 3) < 0.05;
        if (g_lowSince < 0) g_lowSince = now;
        if (!smoothing && now - g_lowSince > 20 && fabs(g_capPeriod - periodSec) > 1e-5) { g_capPeriod = periodSec; g_bestPeriod = periodSec; Log("CAP the headset now runs at %.1f Hz steadily: cap follows", 1.0 / periodSec); }
    } else g_lowSince = -1;
    if (g_capPeriod <= 0) return;
    if (g_capNext == 0 || now - g_capNext > g_capPeriod) g_capNext = now;
    double t0 = now;
    while (Now() < g_capNext) { if (g_capNext - Now() > 0.0015) Sleep(1); else YieldProcessor(); }
    double w = (Now() - t0) * 1000;
    g_capWaitSum += w; if (w > g_capWaitMax) g_capWaitMax = w; g_capped++; g_capThisFrame = w;
    g_capNext += g_capPeriod;
}

// ---------------------------------------------------------------- VR-unfriendly effects off
// Bloom, vignette, depth of field, chromatic aberration, motion blur, film grain and screen-space AO, as Volume
// overrides (the game's and Alkawa's own). Found through VolumeManager.instance.m_Volumes every 2 s (cheap); new
// volumes from level loads are caught there.
static const char* kOff[] = {"AlkawaBloom", "Bloom", "AlkawaVignette", "Vignette", "AlkawaChromaticAberration", "ChromaticAberration",
                             "DepthOfField", "MotionBlur", "FilmGrain", "ScreenSpaceAmbientOcclusion", "AlkawaSSAO"};
static std::set<Il2CppObject*> g_offComps;
static uint32_t g_effectsOff = 0;
static void EffectsOff() {
    double t0 = Now();
    Il2CppObject* vm = P<Il2CppObject* (*)(MethodInfo*)>(mVolMgrInstance)(mVolMgrInstance);
    Il2CppObject* vl = vm ? Fld<Il2CppObject*>(vm, oVolMgrVolumes) : nullptr;
    Il2CppArray* vols = vl ? Fld<Il2CppArray*>(vl, oListItems) : nullptr; int nv = vl ? Fld<int>(vl, oListSize) : 0;
    for (int i = 0; vols && i < nv; i++) {
        Il2CppObject* v = (Il2CppObject*)vols->data[i]; if (!v) continue;
        Il2CppObject* prof = Fld<Il2CppObject*>(v, oVolProfile); if (!prof) continue;
        Il2CppObject* list = Fld<Il2CppObject*>(prof, oProfComps); if (!list) continue;
        Il2CppArray* items = Fld<Il2CppArray*>(list, oListItems); int n = Fld<int>(list, oListSize);
        for (int k = 0; k < n; k++) {
            Il2CppObject* c = (Il2CppObject*)items->data[k]; if (!c) continue;
            const char* cn = ClassName(c);
            for (const char* off : kOff) if (!strcmp(cn, off) && Fld<bool>(c, oCompActive)) {
                Fld<bool>(c, oCompActive) = false; g_effectsOff++;
                if (!g_offComps.count(c)) { g_offComps.insert(c); if (g_offComps.size() <= 40) Log("EFFECTS %s off in volume '%s' (VR-unfriendly)", cn, Name(v).c_str()); }
            }
        }
    }
    double ms = (Now() - t0) * 1000; if (ms > g_scanMax) g_scanMax = ms;
}

// ---------------------------------------------------------------- HUD: the game's overlay canvases drawn into a texture
static void MakeHud() {
    g_hudRT = MakeRT(g_hudW, g_hudH, "LCVR_HUD");
    Il2CppObject* go = NewGameObject("LCVR_HudCam");
    g_hudCam = AddComp(go, kCamera); Il2CppObject* d = AddComp(go, kUrpCam);
    SetII(mCamSetClearFlags, g_hudCam, 2 /*SolidColor*/);
    P<void (*)(Il2CppObject*, Color, MethodInfo*)>(mCamSetBg)(g_hudCam, Color{0, 0, 0, 0}, mCamSetBg);
    SetII(mCamSetMask, g_hudCam, g_hudMask);
    P<void (*)(Il2CppObject*, float, MethodInfo*)>(mCamSetDepth)(g_hudCam, 50, mCamSetDepth);
    SetIO(mCamSetTarget, g_hudCam, g_hudRT);
    if (d) {  // the plain renderer, no post-processing, no AA, no shadows
        if (oUrpRenderer >= 0) Fld<int>(d, oUrpRenderer) = 0;
        if (oUrpPost >= 0) Fld<bool>(d, oUrpPost) = false;
        if (oUrpAA >= 0) Fld<int>(d, oUrpAA) = 0;
        if (oUrpShadows >= 0) Fld<bool>(d, oUrpShadows) = false;
        if (oUrpVolMask >= 0) Fld<int>(d, oUrpVolMask) = 0;
    }
    // Orthographic, 1 unit = 1 screen pixel, so canvas elements the game places by screen pixel land where it meant
    // (AI Limit VR). Far below the level (y - 100000), so no level geometry is in its view.
    P<void (*)(Il2CppObject*, bool, MethodInfo*)>(mCamSetOrtho)(g_hudCam, true, mCamSetOrtho);
    SetIF(mCamSetOrthoSize, g_hudCam, g_hudH * 0.5f);
    SetIF(mCamSetNear, g_hudCam, 0.5f); SetIF(mCamSetFar, g_hudCam, 2.0f);
    P<void (*)(Il2CppObject*, Vec3, Quat, MethodInfo*)>(mTrSetPosRot)(Transform(g_hudCam), Vec3{g_hudW * 0.5f, g_hudH * 0.5f - 100000.0f, -1.0f}, Quat{0, 0, 0, 1}, mTrSetPosRot);
    Log("HUD camera %dx%d made: orthographic, layers 0x%X, transparent clear", g_hudW, g_hudH, g_hudMask);
}
static bool g_uiCamMove = true;
static Il2CppObject* UiCamera() {
    Il2CppObject* cs = oCsUiCamera >= 0 ? CameraSystem() : nullptr;
    Il2CppObject* u = cs ? Fld<Il2CppObject*>(cs, oCsUiCamera) : nullptr;
    return (u && Alive(u)) ? u : nullptr;
}
static void MoveCanvases() {
    double t0 = Now();
    Il2CppArray* cvs = FindAll(kCanvas);
    for (uintptr_t i = 0; cvs && i < cvs->max_length; i++) {
        Il2CppObject* c = (Il2CppObject*)cvs->data[i]; if (!c || !IB(mCanvasRoot, c)) continue;
        int mode = II(mCanvasMode, c);
        if (mode == 0) { SetII(mCanvasSetMode, c, 1); SetIO(mCanvasSetCam, c, g_hudCam); SetIF(mCanvasSetPlane, c, 1.0f); Log("HUD canvas '%s' moved to the HUD camera", Name(c).c_str()); }
        else if (mode == 1 && g_uiCamMove && UiCamera() && GetObj(mCanvasGetCam, c) == UiCamera()) { SetIO(mCanvasSetCam, c, g_hudCam); SetIF(mCanvasSetPlane, c, 1.0f); Log("HUD canvas '%s' (drawn by the game's UICamera) moved to the HUD camera", Name(c).c_str()); }
        else if (mode == 1 && g_gameCam && GetObj(mCanvasGetCam, c) == g_gameCam) { SetIO(mCanvasSetCam, c, g_hudCam); SetIF(mCanvasSetPlane, c, 1.0f); Log("HUD canvas '%s' (drawn by the game camera) moved to the HUD camera", Name(c).c_str()); }
    }
    double ms = (Now() - t0) * 1000; if (ms > g_scanMax) g_scanMax = ms;
}

// ---------------------------------------------------------------- periodic log block (every 5 s)
static double g_statT0 = -1; static float g_statGame0 = 0; static int g_statFrames0 = 0;
static void Periodic() {
    double t = Now();
    float game = SF(mTimeTime); int frame = SI(mTimeFrame);
    if (g_statT0 < 0) { g_statT0 = t; g_statGame0 = game; g_statFrames0 = frame; return; }
    double dt = t - g_statT0;
    if (dt < 5.0) return;
    int frames = frame - g_statFrames0;
    double speed = (game - g_statGame0) / dt;
    lcxr::Views v; lcxr::GetViews(&v);
    double hz = v.periodSec > 0 ? 1.0 / v.periodSec : 0;
    Log("SPEED %.1f s: game speed %.3f, %.1f frames/s, headset %.1f Hz (cap %.1f Hz), timeScale %.3f, physics step %.2f ms, maxDelta %.3f",
        dt, speed, frames / dt, hz, g_capPeriod > 0 ? 1.0 / g_capPeriod : 0, SF(mTimeScale), SF(mTimeFixedDelta) * 1000, SF(mTimeMaxDelta));
    if (g_vrStarted && fabs(speed - 1.0) > 0.05 && SF(mTimeScale) > 0.99) Log("SPEED WARNING: game time runs at %.2f of real time", speed);
    { std::lock_guard<std::mutex> lk(g_gpuMx);
      if (g_gpuTiming) Log("GPU both eye renders avg %.2f ms max %.2f ms (%u frames measured)", g_gpuN ? g_gpuSum / g_gpuN : 0, g_gpuMax, g_gpuN);
      g_gpuSum = g_gpuMax = 0; g_gpuN = 0; }
    double frameAvg = frames ? dt * 1000 / frames : 0, waitAvg = g_capped ? g_capWaitSum / g_capped : 0;
    Log("PERF frame avg %.2f ms max %.2f ms, CPU (frame minus cap wait) avg %.2f ms, cap wait avg %.2f max %.2f ms, mod hook CPU avg %.3f max %.3f ms, scan max %.2f ms, %u eye pairs, %u render calls, flat skipped %u, late frames %u",
        frameAvg, g_frameMax, frameAvg - waitAvg, waitAvg, g_capWaitMax, frames ? g_hookSum / frames : 0, g_hookMax, g_scanMax, g_eyePairs, g_renderCalls, g_flatSkipped, g_late);
    lcxr::Stats s; lcxr::TakeStats(&s);
    Log("XRSTAT state %d session %d focused %d, xr frames %u (no new image %u, layers sent %u), queued %u copied %u skipped busy %u no slot %u, copy max %.2f ms, xrWaitFrame avg %.2f max %.2f ms, xrEndFrame avg %.2f max %.2f ms, layer %d",
        lcxr::GetState(), v.sessionState, v.focused, s.xrFrames, s.xrNoNewImage, s.layersSent, s.queued, s.copied, s.skippedBusy, s.skippedNoSlots, s.copyMsMax,
        s.waitMsAvg, s.waitMsMax, s.endMsAvg, s.endMsMax, lcxr::LayerInUse());
    Log("INPUT pad reads %u, menu open %d, blocking the game %d | EFFECTS %zu overrides held off (%u switches) | OFFLINE %u web requests blocked | EDGE trim %.4f of the width each side (match %d, frame depth %.2f)",
        menu::g_padReads, menu::g_open, menu::g_block, g_offComps.size(), g_effectsOff, g_webBlocked, g_edgeTrim, g_set.edgeMatch, g_set.edgeDepth);
    menu::g_padReads = 0; g_effectsOff = 0;
    g_capWaitSum = g_capWaitMax = g_hookSum = g_hookMax = g_frameMax = g_scanMax = 0; g_capped = g_eyePairs = g_renderCalls = g_flatSkipped = g_late = 0;
    g_statT0 = t; g_statGame0 = game; g_statFrames0 = frame;
}

// Lab: every Input System action asset the game has loaded, and the keyboard keys it binds (is Insert free?).
static void LogBindings() {
    void* kAsset = FindClass("UnityEngine.InputSystem", "InputActionAsset");
    MethodInfo* mAll = FindMethod(FindClass("UnityEngine", "Resources"), "FindObjectsOfTypeAll", 1, "System.Type");
    MethodInfo* mJson = kAsset ? FindMethod(kAsset, "ToJson", 0) : nullptr;
    if (!kAsset || !mAll || !mJson) { Log("BINDINGS not available (InputActionAsset %p FindObjectsOfTypeAll %p ToJson %p)", kAsset, mAll, mJson); return; }
    Il2CppArray* all = P<Il2CppArray* (*)(Il2CppObject*, MethodInfo*)>(mAll)(TypeOf(kAsset), mAll);
    int insertHits = 0;
    for (uintptr_t i = 0; all && i < all->max_length; i++) {
        Il2CppObject* a = (Il2CppObject*)all->data[i];
        Il2CppObject* js = GetObj(mJson, a);
        Il2CppStr* st = (Il2CppStr*)js; std::string t;
        for (int k = 0; st && k < st->len; k++) t += st->c[k] < 128 ? (char)st->c[k] : '?';
        std::set<std::string> keys;
        for (size_t at = t.find("<Keyboard>/"); at != std::string::npos; at = t.find("<Keyboard>/", at + 1)) {
            size_t e = t.find_first_of("\",}", at); keys.insert(t.substr(at + 11, e == std::string::npos ? 20 : e - at - 11));
        }
        std::string list; for (auto& k : keys) list += k + " ";
        bool ins = keys.count("insert") > 0; if (ins) insertHits++;
        Log("BINDINGS asset '%s': %zu keyboard keys%s: %s", Name(a).c_str(), keys.size(), ins ? " (INSERT USED)" : "", list.c_str());
    }
    Log("BINDINGS %d asset(s), Insert used in %d", all ? (int)all->max_length : 0, insertHits);
}

static void PollCommands() {
    std::wstring path = g_gameDir + L"vrlab_command.txt";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    std::ifstream f(path);
    std::stringstream ss; ss << f.rdbuf(); f.close();
    DeleteFileW(path.c_str());
    std::string line;
    while (std::getline(ss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        Log("CMD %s", line.c_str());
        std::istringstream ls(line); std::string c; ls >> c;
        if (c == "snap") g_wantSnap = true;
        else if (c == "eyecheck") { int n; ls >> n; g_eyeCheck = n != 0; }
        else if (c == "vr") { int n; ls >> n; g_vrOn = n != 0; }
        else if (c == "flat") { int n; ls >> n; g_flatToo = n != 0; }
        else if (c == "hud") { int n; ls >> n; g_hudPanel = n != 0; lcxr::SetHud(g_hudPanel); }
        else if (c == "hudmask") { ls >> std::hex >> g_hudMask; if (g_hudCam) SetII(mCamSetMask, g_hudCam, g_hudMask); Log("HUD layers 0x%X", g_hudMask); }
        else if (c == "recenter") lcxr::Recenter();
        else if (c == "depth") { ls >> g_set.depth; }
        else if (c == "stage") { ls >> g_set.stageWidth >> g_set.stageDistance; lcxr::SetStage({g_set.stageWidth, g_set.stageDistance}); }
        else if (c == "gamesettings") LogGameSet("asked");
        else if (c == "camsys") { int n; ls >> n; g_useCamSys = n != 0; Log("CMD game camera from CameraSystem %s", g_useCamSys ? "on" : "off (build 1: first screen camera)"); }
        else if (c == "bindings") LogBindings();
        else if (c == "edge") { ls >> g_set.edgeMatch; Log("CMD edge match %d", g_set.edgeMatch); }
        else if (c == "edgemask") { ls >> g_set.edgeMask; Log("CMD edge mask %d", g_set.edgeMask); }
        else if (c == "edgefade") { float f; ls >> f; g_set.edgeFade = f; lcxr::SetEdgeFade(f); Log("CMD edge fade %.3f of the stage width each side", f); }
        else if (c == "edgedepth") { ls >> g_set.edgeDepth; Log("CMD edge match frame depth %.2f x the plane", g_set.edgeDepth); }
        else if (c == "menuflat") { int n; ls >> n; g_menuFlat = n != 0; }
        else if (c == "uicanvas") { int n; ls >> n; g_uiCamMove = n != 0; }
        else if (c == "uiprobe") UiProbe();
        else if (c == "menuui") { int v; ls >> v; menuui::g_enabled = v != 0; Log("CMD game-drawn menu %s", v ? "on" : "off"); }
        else if (c == "depthprobe") g_wantDepthProbe = true;
        else if (c == "surround") { ls >> g_set.surround; lcxr::SetSurround(g_set.surround); Log("CMD surround %d (0 Black, 1 Green, 2 Transparent)", g_set.surround); }
        else if (c == "popout") { ls >> g_set.popOut; Log("CMD pop-out %.2f", g_set.popOut); }
        else if (c == "sharpen") { ls >> g_set.sharpen; lcxr::SetSharpen(g_set.sharpen); Log("CMD sharpen %.2f", g_set.sharpen); }
        else if (c == "picture") { ls >> g_set.picture; lcxr::SetPicture(g_set.picture); Log("CMD picture %d (0 Auto, 1 Steady, 2 Panel)", g_set.picture); }
        else if (c == "hudquad") { int n; ls >> n; lcxr::SetHudQuad(n != 0); }
        else if (c == "plane") { ls >> g_set.planeSource; Log("CMD convergence source %d (0 player always, 1 cutscene camera's target)", g_set.planeSource); }
        else if (c == "cutdepth") { ls >> g_set.cutsceneDepth; Log("CMD cutscene depth %.2f", g_set.cutsceneDepth); }
        else if (c == "smooth") { ls >> g_set.smoothSec; Log("CMD convergence smoothing %.2f s", g_set.smoothSec); }
        else if (c == "camstate") Log("CAMSTATE cutscene %d, active camera '%s', convergence %.2f (gameplay %.2f), game camera '%s'", (int)g_inCutscene, (g_lastVcam && Alive(g_lastVcam)) ? Name(g_lastVcam).c_str() : "none", g_smoothDist, g_gameplayDist, Name(g_gameCam).c_str());
        else if (c == "menu") menu::SetOpen(!menu::g_open, "lab");
        else if (c == "xrsnap") { static int k = 0; char pth[MAX_PATH]; sprintf_s(pth, "%lsLostCrownVR\\snaps\\xr%d", g_gameDir.c_str(), ++k); lcxr::SnapComposed(pth); Log("XRSNAP xr%d, edge trim %.4f of the width each side", k, g_edgeTrim); }
        else if (c == "key") { int vk; ls >> std::hex >> vk; menu::g_labKey = vk; menu::g_labKeyUntil = Now() + 0.15; }
        else Log("CMD unknown");
    }
}

// ---------------------------------------------------------------- eyes
static void CopyGameCamera(Il2CppObject* game) {
    Il2CppObject* gd = GetComp(game, kUrpCam);
    for (int e = 0; e < 2; e++) {
        P<void (*)(Il2CppObject*, Il2CppObject*, MethodInfo*)>(mCamCopyFrom)(g_eyeCam[e], game, mCamCopyFrom);
        SetIO(mCamSetTarget, g_eyeCam[e], g_eyeRT[e]);
        P<void (*)(Il2CppObject*, float, MethodInfo*)>(mCamSetDepth)(g_eyeCam[e], IF(mCamGetDepth, game) - 2 + e, mCamSetDepth);
        Il2CppObject* d = GetComp(g_eyeCam[e], kUrpCam);
        if (gd && d) {
            if (oUrpRenderer >= 0) Fld<int>(d, oUrpRenderer) = Fld<int>(gd, oUrpRenderer);
            if (oUrpPost >= 0) Fld<bool>(d, oUrpPost) = Fld<bool>(gd, oUrpPost);
            if (oUrpAA >= 0) Fld<int>(d, oUrpAA) = Fld<int>(gd, oUrpAA);
            if (oUrpAAQ >= 0) Fld<int>(d, oUrpAAQ) = Fld<int>(gd, oUrpAAQ);
            if (oUrpShadows >= 0) Fld<bool>(d, oUrpShadows) = Fld<bool>(gd, oUrpShadows);
            if (oUrpVolMask >= 0) Fld<int>(d, oUrpVolMask) = Fld<int>(gd, oUrpVolMask);
            if (oUrpVolTrig >= 0) Fld<Il2CppObject*>(d, oUrpVolTrig) = Transform(game);
            if (oUrpDepthOpt >= 0) Fld<int>(d, oUrpDepthOpt) = Fld<int>(gd, oUrpDepthOpt);
            if (oUrpColorOpt >= 0) Fld<int>(d, oUrpColorOpt) = Fld<int>(gd, oUrpColorOpt);
            if (oUrpStopNaN >= 0) Fld<bool>(d, oUrpStopNaN) = Fld<bool>(gd, oUrpStopNaN);
            if (oUrpDither >= 0) Fld<bool>(d, oUrpDither) = Fld<bool>(gd, oUrpDither);
        }
    }
    Log("EYES settings copied from game camera '%s' (URP data %s)", Name(game).c_str(), gd ? "copied" : "missing");
}
static void MakeEyes(Il2CppObject* game) {
    g_eyeW = lcxr::EyeWidth(); g_eyeH = lcxr::EyeHeight();
    for (int e = 0; e < 2; e++) {
        g_eyeRT[e] = MakeRT(g_eyeW, g_eyeH, e ? "LCVR_R" : "LCVR_L");
        Il2CppObject* go = NewGameObject(e ? "LCVR_EyeR" : "LCVR_EyeL");
        g_eyeCam[e] = AddComp(go, kCamera); AddComp(go, kUrpCam);
    }
    CopyGameCamera(game);
    if (!g_gameDev) { ID3D11Texture2D* t = (ID3D11Texture2D*)TexPtr(g_eyeRT[0]); if (t) { t->GetDevice(&g_gameDev); g_gameDev->GetImmediateContext(&g_gameCtx); } }
}
// Where the eyes converge (the depth that sits on the stage). Gameplay: the player (CameraSystem.m_followTarget).
// Build 2: when the active Cinemachine camera is not the gameplay one (cutscenes), the depth of what it looks at
// (its LookAt, else its Follow), else the last gameplay distance held. The result is smoothed so the depth never
// jumps while the camera moves; a camera cut (a big jump in camera position) takes the new distance at once.
static float DepthAlong(Il2CppObject* cam, Il2CppObject* target) {
    if (!target || !Alive(target)) return 0;
    Il2CppObject* ct = Transform(cam); Vec3 cp = TrPos(ct), f = TrFwd(ct), tp = TrPos(target);
    return (tp.x - cp.x) * f.x + (tp.y - cp.y) * f.y + (tp.z - cp.z) * f.z;
}
static Vec3 g_lastCamPos{0, 0, 0};
static double g_lastPlaneT = 0;
static uint32_t g_vcamChanges = 0;
// The brain's active camera can be a blend (a plain C# object, not a Unity object): only a real virtual camera is
// asked for its name or targets (build 2's first test run crashed asking a blend for its name).
static bool IsVcam(Il2CppObject* o) {
    if (!o || !kVcamBase) return false;
    for (void* k = il2cpp_object_get_class(o); k; k = il2cpp_class_get_parent(k)) if (k == kVcamBase) return true;
    return false;
}
static float PlaneDistance(Il2CppObject* cam, Mat4 view) {
    Il2CppObject* cs = CameraSystem();
    Il2CppObject* follow = cs && oCsFollow >= 0 ? Fld<Il2CppObject*>(cs, oCsFollow) : nullptr;
    Il2CppObject* def = cs && oCsDefault >= 0 ? Fld<Il2CppObject*>(cs, oCsDefault) : nullptr;
    Il2CppObject* brain = cs && oCsBrain >= 0 ? Fld<Il2CppObject*>(cs, oCsBrain) : nullptr;
    Il2CppObject* vcamAny = (brain && Alive(brain) && mBrainActive) ? GetObj(mBrainActive, brain) : nullptr;
    Il2CppObject* vcam = (IsVcam(vcamAny) && Alive(vcamAny)) ? vcamAny : nullptr;
    bool blending = vcamAny && !vcam;
    float dPlayer = DepthAlong(cam, follow);
    bool cutscene = (vcam && def && vcam != def) || (blending && g_inCutscene);
    float d = 0; const char* src = "player";
    float dLook = 0;
    if (cutscene && g_set.planeSource == 1) {
        static void* lookK = nullptr; static MethodInfo *mLook = nullptr, *mFol = nullptr;
        void* k = il2cpp_object_get_class(vcam);
        if (k != lookK) { lookK = k; mLook = FindMethod(k, "get_LookAt", 0); mFol = FindMethod(k, "get_Follow", 0); }
        Il2CppObject* t = mLook ? GetObj(mLook, vcam) : nullptr;
        if (!t && mFol) t = GetObj(mFol, vcam);
        dLook = DepthAlong(cam, t);
        if (dLook > 0.3f) { d = dLook; src = "cutscene camera's target"; }
        else if (g_gameplayDist > 0) { d = g_gameplayDist; src = "held (cutscene camera has no target in front)"; }
        else { d = dPlayer; src = "player"; }
    } else {
        d = dPlayer;
        if (d > 0.5f && !cutscene) g_gameplayDist = d;
    }
    // In cutscenes the convergence stays within 0.3x to 3x the gameplay distance: the eye gap in game units grows with
    // it, and a far one gave very different eye pictures (a build 1 headset test).
    if (cutscene && g_gameplayDist > 0) { float lo = 0.3f * g_gameplayDist, hi = 3.0f * g_gameplayDist; d = d < lo ? lo : d > hi ? hi : d; }
    // The player behind or level with the camera for a moment (a build 2 headset log: depth -3.2): hold the
    // last gameplay distance instead of jumping to the world origin (103 units there).
    if (d < 0.5f && g_gameplayDist > 0) { d = g_gameplayDist; src = "held (player not in front of the camera)"; }
    if (d < 0.5f) { d = fabsf(M(view, 2, 3)); src = "world origin (no player)"; }
    if (d < 0.5f) d = 10.0f;
    if (vcam != g_lastVcam && !blending) {
        g_lastVcam = vcam; g_vcamChanges++;
        if (g_vcamChanges <= 200)
            Log("CAMERA active camera '%s' (%s)%s: fov %.1f, player depth %.2f, target depth %.2f, converging on %s at %.2f",
                vcam ? Name(vcam).c_str() : "none", vcam ? ClassName(vcam) : (vcamAny ? ClassName(vcamAny) : "-"), cutscene ? " CUTSCENE" : "", g_fovNow,
                dPlayer, dLook, src, d);
    }
    g_inCutscene = cutscene;
    // Smoothing, with cuts taken at once.
    Il2CppObject* ct = Transform(cam); Vec3 cp = TrPos(ct);
    double now = Now();
    float jump = sqrtf((cp.x - g_lastCamPos.x) * (cp.x - g_lastCamPos.x) + (cp.y - g_lastCamPos.y) * (cp.y - g_lastCamPos.y) + (cp.z - g_lastCamPos.z) * (cp.z - g_lastCamPos.z));
    double dt = g_lastPlaneT > 0 ? now - g_lastPlaneT : 1;
    g_lastPlaneT = now; g_lastCamPos = cp;
    if (g_smoothDist <= 0 || g_set.smoothSec <= 0 || jump > 0.25f * g_smoothDist || dt > 0.5) g_smoothDist = d;
    else { float a = (float)(dt / g_set.smoothSec); if (a > 1) a = 1; g_smoothDist += (d - g_smoothDist) * a; }
    return g_smoothDist;
}
static float g_planeDist = 0;
static float g_maskP00[2], g_maskP02[2], g_maskP22, g_maskP23, g_maskEx[2], g_maskEs[2]; static bool g_maskFlat = false;
// Both eyes from the game camera's view this frame plus each eye's place relative to the recenter point (Ori VR's
// diorama maths): each eye sees the game camera's own rectangle on the gameplay plane, from where it is.
static void PoseEyes(Il2CppObject* game, int frame) {
    lcxr::Views v; lcxr::GetViews(&v);
    g_usedViews = v;
    Mat4 view = GetM(mCamGetW2C, game), proj = GetM(mCamGetProj, game);
    g_fovNow = 2 * atanf(1 / M(proj, 1, 1)) * 57.29578f;
    float dist = PlaneDistance(game, view); g_planeDist = dist;
    float planeW = 2 * dist / M(proj, 0, 0), planeH = 2 * dist / M(proj, 1, 1);
    float upm = planeW / g_set.stageWidth;  // game units per metre on the stage
    // Menu cameras (the character screen's 'SargonCamera', the pause menu's UICamera): when the game draws with a camera
    // that is not CameraSystem's gameplay camera, both eyes see it from the centre (flat on the stage), since the depth
    // maths assume the gameplay plane (83 units for the character screen). Lab: "menuflat 0".
    Il2CppObject* csys = (oCsCamera >= 0) ? CameraSystem() : nullptr;
    Il2CppObject* sysCam = csys ? Fld<Il2CppObject*>(csys, oCsCamera) : nullptr;
    bool menuCam = g_menuFlat && sysCam && game != sysCam;
    static bool wasMenu = false;
    if (menuCam != wasMenu) { wasMenu = menuCam; Log("EYES %s", menuCam ? "menu camera: both eyes from the centre (flat)" : "gameplay camera: stereo"); }
    float eyeX[2] = {0, 0};
    for (int e = 0; e < 2; e++) {
        float lx, ly, lz;
        if (g_eyeCheck || menuCam) { lx = ly = lz = 0; }
        else if (v.valid && v.recenters) { lx = v.local[e][0]; ly = v.local[e][1]; lz = v.local[e][2]; }
        else { lx = e ? 0.032f : -0.032f; ly = lz = 0; }
        float depth = g_set.depth * (g_inCutscene ? g_set.cutsceneDepth : 1.0f);
        float x = lx * upm * depth, y = ly * upm * depth;
        eyeX[e] = x;
        float eyeDist = dist + lz * upm; if (eyeDist < dist * 0.25f) eyeDist = dist * 0.25f;
        Mat4 ve = view, pe = proj;
        M(ve, 0, 3) -= x; M(ve, 1, 3) -= y; M(ve, 2, 3) -= (eyeDist - dist);
        // Pop-out (Ori VR, Ori WotW VR): the depth planeZ behind the player sits on the stage instead of the player, so
        // the player and everything nearer come toward you; the flat view from the middle is unchanged because the
        // rectangle grows with the distance. Not in menu cameras (flat).
        float planeZ = menuCam ? 0.0f : g_set.popOut * dist, grow = (dist + planeZ) / dist;
        float wz = planeW * grow, hz = planeH * grow, dz = eyeDist + planeZ;
        M(pe, 0, 0) = 2 * dz / wz; M(pe, 0, 2) = -2 * x / wz;
        M(pe, 1, 1) = 2 * dz / hz; M(pe, 1, 2) = -2 * y / hz;
        g_maskP00[e] = M(pe, 0, 0); g_maskP02[e] = M(pe, 0, 2); g_maskP22 = M(pe, 2, 2); g_maskP23 = M(pe, 2, 3);
        g_maskEx[e] = x; g_maskEs[e] = eyeDist - dist;
        Il2CppObject* c = g_eyeCam[e];
        SetII(mCamSetMask, c, II(mCamGetMask, game));
        SetM(mCamSetW2C, c, ve); SetM(mCamSetProj, c, pe);
    }
    // Edge match: the stereo part of the eye gap only (head movement shows the same extra strip to both eyes; Cuphead VR
    // 1.01). Content at depth z = edgeDepth x dist shifts by gap x (dist/z - 1) on the plane between the eyes.
    float gap = eyeX[1] - eyeX[0];
    float trim = 0;
    if (g_set.edgeMatch && gap > 0 && g_set.edgeDepth > 0.05f && fabsf(g_set.edgeDepth - 1.0f) > 0.01f) {
        trim = gap * fabsf(1.0f / g_set.edgeDepth - 1.0f) / planeW;
        if (trim > 0.15f) trim = 0.15f;
    }
    g_maskFlat = menuCam || g_eyeCheck;
    g_edgeTrim = trim;
    bool behind = g_set.edgeDepth > 1.0f;
    float trims[4] = {behind ? 0 : trim, behind ? trim : 0, behind ? trim : 0, behind ? 0 : trim};
    lcxr::SetEdgeTrim(trims);
    g_posedFrame = frame;
}

#include "EdgeMask.inc"
// URP renders each camera with RenderCameraStack; after an eye camera, its depth is copied for the edge mask.
typedef void (*Stack_t)(void* ctx, Il2CppObject* cam, MethodInfo* m);
static Stack_t oStack;
static void hkStack(void* ctx, Il2CppObject* cam, MethodInfo* m) {
    oStack(ctx, cam, m);
    if (!g_set.edgeMask || !cam) return;
    if (cam == g_eyeCam[0]) edgemask::AfterCamera(0); else if (cam == g_eyeCam[1]) edgemask::AfterCamera(1);
}

// Lab probe for the stylized menu: every loaded TextMeshPro font asset and every sprite (name, texture, size), written to
// LostCrownVR\snaps\uiprobe.txt. Nothing is changed or copied; the names pick what the menu loads at run time.
static void UiProbe() {
    MethodInfo* findAll = FindMethod(FindClass("UnityEngine", "Resources"), "FindObjectsOfTypeAll", 1);
    if (!findAll) { Log("UIPROBE Resources.FindObjectsOfTypeAll not found"); return; }
    auto all = [&](void* k) -> Il2CppArray* { return k ? P<Il2CppArray* (*)(Il2CppObject*, MethodInfo*)>(findAll)(TypeOf(k), findAll) : nullptr; };
    std::wstring path = g_gameDir + L"LostCrownVR\\snaps\\uiprobe.txt";
    FILE* f = _wfopen(path.c_str(), L"w"); if (!f) return;
    void* kFont = FindClass("TMPro", "TMP_FontAsset");
    Il2CppArray* fonts = all(kFont);
    fprintf(f, "TMP_FontAsset %d\n", fonts ? (int)fonts->max_length : -1);
    for (uintptr_t i = 0; fonts && i < fonts->max_length; i++) fprintf(f, "  font %s\n", Name((Il2CppObject*)fonts->data[i]).c_str());
    void* kSprite = FindClass("UnityEngine", "Sprite");
    MethodInfo* sprTex = FindMethod(kSprite, "get_texture", 0);
    void* kTex = FindClass("UnityEngine", "Texture");
    MethodInfo* tw = FindMethod(kTex, "get_width", 0); MethodInfo* th = FindMethod(kTex, "get_height", 0);
    Il2CppArray* spr = all(kSprite);
    fprintf(f, "Sprite %d\n", spr ? (int)spr->max_length : -1);
    for (uintptr_t i = 0; spr && i < spr->max_length; i++) {
        Il2CppObject* sp = (Il2CppObject*)spr->data[i];
        Il2CppObject* t = sprTex ? GetObj(sprTex, sp) : nullptr;
        int w = (t && tw) ? P<int (*)(Il2CppObject*, MethodInfo*)>(tw)(t, tw) : 0, h = (t && th) ? P<int (*)(Il2CppObject*, MethodInfo*)>(th)(t, th) : 0;
        fprintf(f, "  sprite %s | texture %s %dx%d\n", Name(sp).c_str(), t ? Name(t).c_str() : "-", w, h);
    }
    fclose(f);
    Log("UIPROBE %d font assets, %d sprites written to uiprobe.txt", fonts ? (int)fonts->max_length : -1, spr ? (int)spr->max_length : -1);
}

// ---------------------------------------------------------------- the render hook
typedef void (*Render_t)(Il2CppObject* self, void* ctx, Il2CppObject* cams, MethodInfo* m);
static Render_t oRender;

static void StartVr() {
    g_vrStarted = true;
    LogGameSet("before VR");
    g_hudW = SI(mScreenW); g_hudH = SI(mScreenH);
    if (g_hudW < 320 || g_hudH < 200) { g_hudW = 1920; g_hudH = 1080; }
    lcxr::Configure({g_set.stageWidth, g_set.stageDistance}, g_set.picture, g_set.resolution, g_set.tanSpan, (float)g_hudW / g_hudH, g_hudW, g_hudH);
    lcxr::SetSharpen(g_set.sharpen);
    lcxr::SetEdgeFade(g_set.edgeFade);
    lcxr::SetSurround(g_set.surround);
    if (g_set.labCommands) {  // lab only: start-failure tests ([Lab] RuntimeJson, NoRuntimeFallback in the ini)
        char rt[MAX_PATH] = ""; wchar_t wrt[MAX_PATH] = L"";
        GetPrivateProfileStringW(L"Lab", L"RuntimeJson", L"", wrt, MAX_PATH, IniPath().c_str());  // one path string (build 4's lab
        WideCharToMultiByte(CP_ACP, 0, wrt, -1, rt, MAX_PATH, nullptr, nullptr);              // read mixed two temporaries: crash)
        bool noFb = IniF(L"Lab", L"NoRuntimeFallback", 0) != 0;
        if (rt[0]) lcxr::SetLabRuntime(rt, noFb);
    }
    lcxr::Start();
    Log("VR starting: stage %.2f m wide at %.2f m, depth %.2f, resolution %.2f, picture %d, screen %dx%d", g_set.stageWidth, g_set.stageDistance, g_set.depth, g_set.resolution, g_set.picture, g_hudW, g_hudH);
}

static void FrameStart(int frame) {
    g_framesSeen++;
    double now = Now();
    if (g_lastFrameT > 0) { double ft = (now - g_lastFrameT) * 1000; if (ft > g_frameMax) g_frameMax = ft; if (g_capPeriod > 0 && ft > g_capPeriod * 1500) g_late++; }
    g_lastFrameT = now;
    if (g_set.labCommands && (g_framesSeen % 15) == 0) PollCommands();
    menu::Update(now);
    Periodic();
    // VR starts once the game has drawn for a few seconds (title screen), so the window and its size are settled.
    if (!g_vrStarted && g_vrOn && g_gameCam) { if (g_vrAskedAt < 0) g_vrAskedAt = now; else if (now - g_vrAskedAt > 3) StartVr(); }
    lcxr::Views v; lcxr::GetViews(&v);
    bool running = g_vrOn && lcxr::GetState() == lcxr::Running;
    if (running) {
        if ((g_framesSeen % 144) == 0) EffectsOff();
        if (g_hudCam && (g_framesSeen % 72) == 0) MoveCanvases();
        if (!g_gpuTiming && g_gameCtx && lcxr::RuntimeName()[0] && strstr(lcxr::RuntimeName(), "VirtualDesktop") == nullptr) g_gpuTiming = true;
        Cap(v.periodSec);
        KeepPacing();
    }
    static float savedSpan = 0;
    float span = lcxr::EyeTanSpan();
    if (span > 0.5f && g_eyeW && fabsf(span - savedSpan) > 0.01f) { savedSpan = span; g_set.tanSpan = span; IniW(L"VR", L"EyeSpanLastRun", span);
        if (g_eyeW) Log("RENDERSIZE check: eye image %dx%d, the stage needs %.0f px wide at the headset's density x resolution %.2f (headset span %.3f, recommended %dx%d)",
            g_eyeW, g_eyeH, lcxr::RecommendedWidth() * (g_set.stageWidth / g_set.stageDistance) / span * g_set.resolution, g_set.resolution, span,
            lcxr::RecommendedWidth(), lcxr::RecommendedHeight()); }
    // Eyes posed here, in the frame's first Render call: Unity renders the eye cameras (they have target textures) in
    // their own call before the game camera's call.
    if (running && g_eyeCam[0] && g_gameCam && g_gameCamSeen >= frame - 1 && Alive(g_gameCam)) PoseEyes(g_gameCam, frame);
}

static void hkRender(Il2CppObject* self, void* ctx, Il2CppObject* cams, MethodInfo* m) {
    double t0 = Now();
    g_renderCalls++;
    int frame = SI(mTimeFrame);
    if (frame != g_lastFrame) { g_lastFrame = frame; FrameStart(frame); }
    if (!cams) { oRender(self, ctx, cams, m); return; }
    Il2CppArray* items = Fld<Il2CppArray*>(cams, oListItems); int n = Fld<int>(cams, oListSize);
    Il2CppObject* game = nullptr; int gameIdx = -1; bool hasEyes = false;
    Il2CppObject* fallback = nullptr; int fallbackIdx = -1;
    static int sysCamSeen = -10;
    // The gameplay camera is CameraSystem.m_camera ('Brain') whenever the game has one: build 1 took the first screen
    // camera of a call, which during the pause menu was the game's 'UICamera' (a build 1 headset log).
    Il2CppObject* cs = (g_useCamSys && oCsCamera >= 0) ? CameraSystem() : nullptr;
    Il2CppObject* sysCam = cs ? Fld<Il2CppObject*>(cs, oCsCamera) : nullptr;
    if (sysCam && !Alive(sysCam)) sysCam = nullptr;
    for (int i = 0; i < n; i++) {
        Il2CppObject* c = (Il2CppObject*)items->data[i];
        if (!c) continue;
        if (c == g_eyeCam[0] || c == g_eyeCam[1]) { hasEyes = true; continue; }
        if (c == g_hudCam) continue;
        if (GetObj(mCamGetTarget, c)) continue;
        if (c == sysCam) { game = c; gameIdx = i; continue; }
        if (!fallback && c != UiCamera()) { fallback = c; fallbackIdx = i; }
    }
    // CameraSystem's camera when it renders in this call; otherwise the first screen camera that is not the game's
    // UICamera (title, loading). A camera is only posed from while it renders (build 2's first runs crashed posing from
    // the title camera the level load had destroyed).
    if (!game && fallback && !(sysCam && sysCamSeen >= SI(mTimeFrame) - 1)) { game = fallback; gameIdx = fallbackIdx; }
    if (game && game == sysCam) sysCamSeen = SI(mTimeFrame);
    if (game) g_gameCamSeen = SI(mTimeFrame);
    if (game && game != g_gameCam) {
        g_gameCam = game;
        Log("RENDER game camera is now '%s'", Name(game).c_str());
        if (g_eyeCam[0]) CopyGameCamera(game);
    }
    bool running = g_vrOn && lcxr::GetState() == lcxr::Running && lcxr::EyeWidth() > 0;
    if (running && game && !g_eyeCam[0]) { MakeEyes(game); MakeHud(); MoveCanvases(); EffectsOff(); }
    if (g_eyeCam[0] && (!Alive(g_eyeRT[0]) || !Alive(g_eyeRT[1]) || !Alive(g_eyeCam[0]) || !Alive(g_eyeCam[1]))) {
        Log("EYES eye cameras or textures were destroyed by the game: made again");
        g_eyeCam[0] = g_eyeCam[1] = g_eyeRT[0] = g_eyeRT[1] = nullptr;
        if (game) MakeEyes(game);
    }
    static bool eyesOn = false;
    if (g_eyeCam[0] && eyesOn != running) {
        eyesOn = running;
        for (int e = 0; e < 2; e++) SetIB(mBehSetEnabled, g_eyeCam[e], running);
        if (g_hudCam) SetIB(mBehSetEnabled, g_hudCam, running);
        Log("EYES %s", running ? "on" : "off");
    }
    // Leave the game's own flat render out while VR runs (saves a full render).
    bool removed = false;
    if (running && gameIdx >= 0 && !g_flatToo) {
        for (int i = gameIdx; i < n - 1; i++) items->data[i] = items->data[i + 1];
        Fld<int>(cams, oListSize) = n - 1; removed = true; g_flatSkipped++;
    }
    if (hasEyes && g_gpuTiming) g_issue((void*)&OnGpuEvent, 10);
    double mid = Now();
    oRender(self, ctx, cams, m);
    double t1 = Now();
    if (removed) { for (int i = n - 1; i > gameIdx; i--) items->data[i] = items->data[i - 1]; items->data[gameIdx] = game; Fld<int>(cams, oListSize) = n; }
    if (hasEyes && running) {
        if (g_gpuTiming) g_issue((void*)&OnGpuEvent, 11);
        void* l = TexPtr(g_eyeRT[0]); void* r = TexPtr(g_eyeRT[1]); void* h = (g_hudRT && Alive(g_hudRT)) ? TexPtr(g_hudRT) : nullptr;
        if (g_set.edgeMask && !g_maskFlat) {
            edgemask::Params mp{};
            for (int e = 0; e < 2; e++) { mp.p00[e] = g_maskP00[e]; mp.p02[e] = g_maskP02[e]; mp.ex[e] = g_maskEx[e]; mp.es[e] = g_maskEs[e]; }
            mp.p22 = g_maskP22; mp.p23 = g_maskP23; mp.on = 1;
            mp.colour[0] = 0; mp.colour[1] = g_set.surround == 1 ? 1.0f : 0; mp.colour[2] = 0; mp.colour[3] = g_set.surround == 2 ? 0 : 1.0f;
            edgemask::Issue(frame, mp, g_eyeRT[0], g_eyeRT[1], l, r);
        }
        lcxr::QueueFrame(l, r, h, g_usedViews, (uint32_t)frame);
        g_issue(lcxr::RenderEventFunc(), 1);
        g_eyePairs++;
        menuui::AfterRender();
        if (g_wantDepthProbe) {
            g_wantDepthProbe = false;
            if (!g_rtDepthIcall) g_rtDepthIcall = (NativeDepthIcall_t)il2cpp_resolve_icall("UnityEngine.RenderTexture::GetNativeDepthBufferPtr");
            Log("DEPTHPROBE icall %s", g_rtDepthIcall ? "found" : "NOT FOUND");
            for (int e = 0; e < 2; e++) g_probeDepth[e] = g_rtDepthIcall ? g_rtDepthIcall(g_eyeRT[e]) : nullptr;
            {
                typedef int (*PropId_t)(Il2CppObject*); typedef Il2CppObject* (*GetGlobTex_t)(int);
                static PropId_t propId = (PropId_t)il2cpp_resolve_icall("UnityEngine.Shader::PropertyToID");
                static GetGlobTex_t getGlob = (GetGlobTex_t)il2cpp_resolve_icall("UnityEngine.Shader::GetGlobalTextureImpl");
                Log("DEPTHPROBE Shader.PropertyToID %s, GetGlobalTextureImpl %s", propId ? "found" : "NOT FOUND", getGlob ? "found" : "NOT FOUND");
                const char* names[] = {"_CameraDepthTexture", "_CameraDepthAttachment", "_CameraColorTexture", "_CameraOpaqueTexture", "_CameraColorAttachmentA"};
                for (const char* nm : names) {
                    Il2CppObject* t = (propId && getGlob) ? getGlob(propId(il2cpp_string_new(nm))) : nullptr;
                    Log("DEPTHPROBE global %s: %s%s", nm, t ? Name(t).c_str() : "none", t ? "" : "");
                }
            }
            g_issue((void*)&OnDepthProbe, 3);
        }
        if (g_wantSnap) {
            g_wantSnap = false; g_snapCount++;
            const lcxr::Views& v = g_usedViews;
            Log("SNAP %llu frame %d: eyes posed in frame %d, eyecheck %d, depth %.2f, plane %.2f, head L (%.3f %.3f %.3f) R (%.3f %.3f %.3f)",
                (unsigned long long)g_snapCount, frame, g_posedFrame, g_eyeCheck, g_set.depth, g_planeDist,
                v.local[0][0], v.local[0][1], v.local[0][2], v.local[1][0], v.local[1][1], v.local[1][2]);
            std::lock_guard<std::mutex> lk(g_snapMx);
            char nm[64];
            sprintf_s(nm, "s%llu_f%d_L", (unsigned long long)g_snapCount, frame); g_snapQueue.push_back({(ID3D11Texture2D*)l, nm});
            sprintf_s(nm, "s%llu_f%d_R", (unsigned long long)g_snapCount, frame); g_snapQueue.push_back({(ID3D11Texture2D*)r, nm});
            if (h) { sprintf_s(nm, "s%llu_f%d_HUD", (unsigned long long)g_snapCount, frame); g_snapQueue.push_back({(ID3D11Texture2D*)h, nm}); }
            g_issue((void*)&OnSnapEvent, 2);
        }
    }
    double ms = ((mid - t0) + (Now() - t1)) * 1000 - g_capThisFrame;  // the mod's own work, without the cap's wait
    g_capThisFrame = 0;
    g_hookSum += ms; if (ms > g_hookMax) g_hookMax = ms;
}

// ---------------------------------------------------------------- offline: the game's web requests (Ori WotW VR)
struct Il2CppString { void* klass; void* monitor; int32_t length; wchar_t chars[1]; };
typedef void* (*BeginWebRequest_t)(Il2CppObject*);
typedef Il2CppString* (*GetUrl_t)(Il2CppObject*);
typedef void (*SetUrl_t)(Il2CppObject*, Il2CppObject*);
static BeginWebRequest_t oBeginWebRequest; static GetUrl_t g_getUrl; static SetUrl_t g_setUrl;
static void* hkBeginWebRequest(Il2CppObject* req) {
    Il2CppString* u = g_getUrl ? g_getUrl(req) : nullptr;
    if (u && u->length > 4) {
        std::string url; for (int i = 0; i < u->length && i < 200; i++) url += (char)u->chars[i];
        std::string low = url; for (auto& ch : low) ch = (char)tolower(ch);
        if ((low.rfind("http://", 0) == 0 || low.rfind("https://", 0) == 0) && low.find("127.0.0.1:9/") == std::string::npos) {
            g_webBlocked++;
            if (g_webBlocked <= 5 || (g_webBlocked % 50) == 0) { size_t q = url.find('?'); Log("OFFLINE blocked web request %u: %s", g_webBlocked, url.substr(0, q).c_str()); }
            g_setUrl(req, il2cpp_string_new("http://127.0.0.1:9/blocked-by-lost-crown-vr-mod"));
        }
    }
    return oBeginWebRequest(req);
}
static void HookWebRequests() {
    g_getUrl = (GetUrl_t)il2cpp_resolve_icall("UnityEngine.Networking.UnityWebRequest::GetUrl");
    g_setUrl = (SetUrl_t)il2cpp_resolve_icall("UnityEngine.Networking.UnityWebRequest::SetUrl");
    void* f = il2cpp_resolve_icall("UnityEngine.Networking.UnityWebRequest::BeginWebRequest");
    if (!f || !g_getUrl || !g_setUrl) { Log("OFFLINE web request icalls not found: UnityWebRequest calls not blocked"); return; }
    MH_STATUS a = MH_CreateHook(f, (void*)&hkBeginWebRequest, (void**)&oBeginWebRequest), b = MH_EnableHook(f);
    Log("HOOK icall UnityWebRequest::BeginWebRequest (offline: blocked and logged) %s. Ubisoft Connect's own connection is outside the game's code and not blocked", (a == MH_OK && b == MH_OK) ? "in place" : "FAILED");
}

// ---------------------------------------------------------------- clean exit (Ori WotW VR)
typedef void(WINAPI* ExitProcess_t)(UINT);
static ExitProcess_t oExitProcess;
static volatile LONG g_exiting = 0;
static void ShutdownForExit(const char* how) {
    if (InterlockedExchange(&g_exiting, 1)) return;
    Log("EXIT %s: shutting VR down", how);
    g_vrOn = false;
    lcxr::Shutdown();
    MH_DisableHook(MH_ALL_HOOKS);
    Log("EXIT hooks off");
}
static void WINAPI hkExitProcess(UINT code) { ShutdownForExit("ExitProcess"); oExitProcess(code); }
typedef BOOL(WINAPI* TerminateProcess_t)(HANDLE, UINT);
static TerminateProcess_t oTerminateProcess;
static BOOL WINAPI hkTerminateProcess(HANDLE h, UINT code) {
    if (h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId()) ShutdownForExit("TerminateProcess");
    return oTerminateProcess(h, code);
}
static void HookExit() {
    void* ep = (void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ExitProcess");
    MH_STATUS a = MH_CreateHook(ep, (void*)&hkExitProcess, (void**)&oExitProcess), b = MH_EnableHook(ep);
    Log("HOOK ExitProcess (clean VR shutdown) %s", (a == MH_OK && b == MH_OK) ? "in place" : "FAILED");
    void* tp = (void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "TerminateProcess");
    MH_STATUS c = MH_CreateHook(tp, (void*)&hkTerminateProcess, (void**)&oTerminateProcess), d = MH_EnableHook(tp);
    Log("HOOK TerminateProcess (clean VR shutdown) %s", (c == MH_OK && d == MH_OK) ? "in place" : "FAILED");
}

// ---------------------------------------------------------------- setup on the main thread
static bool InPlainCode(void* p, char* section, size_t secLen) {
    auto base = (uint8_t*)g_ga;
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t rva = (uint8_t*)p - base;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (rva >= sec->VirtualAddress && rva < sec->VirtualAddress + sec->Misc.VirtualSize) {
            memcpy(section, sec->Name, 8); section[8] = 0;
            bool denuvo = !strcmp(section, ".ecode") || !strcmp(section, ".arch") || !strcmp(section, ".xcode");
            return !denuvo && (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE);
        }
    }
    strcpy_s(section, secLen, "none");
    return false;
}
static bool Hook(MethodInfo* m, void* detour, void** orig, const char* name) {
    if (!m) { Log("HOOK %s NOT FOUND", name); return false; }
    char sec[16];
    bool ok = InPlainCode(m->methodPointer, sec, sizeof(sec));
    uintptr_t rva = (uint8_t*)m->methodPointer - (uint8_t*)g_ga;
    if (!ok) { Log("HOOK %s refused: code at rva 0x%llX is in section '%s', not the plain code section", name, (unsigned long long)rva, sec); return false; }
    MH_STATUS a = MH_CreateHook(m->methodPointer, detour, orig), b = MH_EnableHook(m->methodPointer);
    Log("HOOK %s %s (rva 0x%llX, section '%s')", name, (a == MH_OK && b == MH_OK) ? "in place" : "FAILED", (unsigned long long)rva, sec);
    return a == MH_OK && b == MH_OK;
}

static double g_t0;
static void Setup() {
    Log("IL2CPP ready, setup on the main thread %.2f s after load", Now() - g_t0);
    { double a = Now(); for (int i = 0; i < 1000; i++) il2cpp_domain_get(); Log("COST one call through the game's Denuvo-wrapped IL2CPP export: %.3f us", (Now() - a) * 1000.0); }
    kCamera = FindClass("UnityEngine", "Camera");
    kUrpCam = FindClass("UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData");
    kCanvas = FindClass("UnityEngine", "Canvas");
    kGameObject = FindClass("UnityEngine", "GameObject");
    kCamSys = FindClass("Alkawa.Gameplay", "CameraSystem");
    void* kObj = FindClass("UnityEngine", "Object"); void* kComp = FindClass("UnityEngine", "Component"); void* kTr = FindClass("UnityEngine", "Transform");
    mObjName = Need(kObj, "Object", "get_name", 0); mObjFindAll = Need(kObj, "Object", "FindObjectsOfType", 1, "System.Type");
    mObjDontDestroy = Need(kObj, "Object", "DontDestroyOnLoad", 1); mObjAlive = Need(kObj, "Object", "op_Implicit", 1);
    mSetHideFlags = Need(kObj, "Object", "set_hideFlags", 1);
    mCompTransform = Need(kComp, "Component", "get_transform", 0); mCompGet = Need(kComp, "Component", "GetComponent", 1, "System.Type");
    mGoCtor = Need(kGameObject, "GameObject", ".ctor", 1, "System.String"); mGoAdd = Need(kGameObject, "GameObject", "AddComponent", 1, "System.Type");
    mTrPos = Need(kTr, "Transform", "get_position", 0); mTrFwd = Need(kTr, "Transform", "get_forward", 0);
    mTrSetPosRot = Need(kTr, "Transform", "SetPositionAndRotation", 2);
    mBehSetEnabled = Need(FindClass("UnityEngine", "Behaviour"), "Behaviour", "set_enabled", 1);
    mCamGetTarget = Need(kCamera, "Camera", "get_targetTexture", 0); mCamSetTarget = Need(kCamera, "Camera", "set_targetTexture", 1);
    mCamCopyFrom = Need(kCamera, "Camera", "CopyFrom", 1); mCamGetDepth = Need(kCamera, "Camera", "get_depth", 0); mCamSetDepth = Need(kCamera, "Camera", "set_depth", 1);
    mCamGetMask = Need(kCamera, "Camera", "get_cullingMask", 0); mCamSetMask = Need(kCamera, "Camera", "set_cullingMask", 1);
    mCamGetW2C = Need(kCamera, "Camera", "get_worldToCameraMatrix", 0); mCamSetW2C = Need(kCamera, "Camera", "set_worldToCameraMatrix", 1);
    mCamGetProj = Need(kCamera, "Camera", "get_projectionMatrix", 0); mCamSetProj = Need(kCamera, "Camera", "set_projectionMatrix", 1);
    mCamSetOrtho = Need(kCamera, "Camera", "set_orthographic", 1); mCamSetOrthoSize = Need(kCamera, "Camera", "set_orthographicSize", 1);
    mCamSetNear = Need(kCamera, "Camera", "set_nearClipPlane", 1); mCamSetFar = Need(kCamera, "Camera", "set_farClipPlane", 1);
    mCamSetClearFlags = Need(kCamera, "Camera", "set_clearFlags", 1); mCamSetBg = Need(kCamera, "Camera", "set_backgroundColor", 1);
    void* kRT = FindClass("UnityEngine", "RenderTexture");
    mRTCtor = Need(kRT, "RenderTexture", ".ctor", 5, "System.Int32,System.Int32,System.Int32,UnityEngine.RenderTextureFormat,UnityEngine.RenderTextureReadWrite");
    mRTCreate = Need(kRT, "RenderTexture", "Create", 0);
    mTexNative = FindMethod(FindClass("UnityEngine", "Texture"), "GetNativeTexturePtr", 0);
    g_texNativeIcall = (TexNativeIcall_t)il2cpp_resolve_icall("UnityEngine.Texture::GetNativeTexturePtr");
    Log("HOOKMAP Texture.GetNativeTexturePtr method %s, icall %s", mTexNative ? "found" : "stripped", g_texNativeIcall ? "found" : "NOT FOUND");
    if (!mTexNative && !g_texNativeIcall) g_missing++;
    mCanvasMode = Need(kCanvas, "Canvas", "get_renderMode", 0); mCanvasSetMode = Need(kCanvas, "Canvas", "set_renderMode", 1);
    mCanvasSetCam = Need(kCanvas, "Canvas", "set_worldCamera", 1); mCanvasGetCam = Need(kCanvas, "Canvas", "get_worldCamera", 0);
    mCanvasSetPlane = Need(kCanvas, "Canvas", "set_planeDistance", 1); mCanvasRoot = Need(kCanvas, "Canvas", "get_isRootCanvas", 0);
    void* kTime = FindClass("UnityEngine", "Time");
    mTimeTime = Need(kTime, "Time", "get_time", 0); mTimeScale = Need(kTime, "Time", "get_timeScale", 0); mSetTimeScale = Need(kTime, "Time", "set_timeScale", 1);
    mTimeFixedDelta = Need(kTime, "Time", "get_fixedDeltaTime", 0); mSetFixedDelta = Need(kTime, "Time", "set_fixedDeltaTime", 1);
    mTimeMaxDelta = Need(kTime, "Time", "get_maximumDeltaTime", 0); mTimeFrame = Need(kTime, "Time", "get_frameCount", 0);
    void* kQS = FindClass("UnityEngine", "QualitySettings");
    mVSync = Need(kQS, "QualitySettings", "get_vSyncCount", 0); mSetVSync = Need(kQS, "QualitySettings", "set_vSyncCount", 1);
    void* kApp = FindClass("UnityEngine", "Application");
    mTargetFps = Need(kApp, "Application", "get_targetFrameRate", 0); mSetTargetFps = Need(kApp, "Application", "set_targetFrameRate", 1);
    void* kScreen = FindClass("UnityEngine", "Screen");
    mScreenW = Need(kScreen, "Screen", "get_width", 0); mScreenH = Need(kScreen, "Screen", "get_height", 0);
    mPrefsGetInt = Need(FindClass("UnityEngine", "PlayerPrefs"), "PlayerPrefs", "GetInt", 2);
    void* kFrl = FindClass("Alkawa.Engine.Gfx", "FrameRateLimiter");
    mFrlEnable = Opt(kFrl, "get_Enable", 0); mFrlRate = Opt(kFrl, "get_LimitedFrameRate", 0);
    oCsFollow = Field(kCamSys, "CameraSystem", "m_followTarget", false);
    oCsCamera = Field(kCamSys, "CameraSystem", "m_camera", false); oCsUiCamera = Field(kCamSys, "CameraSystem", "m_uiCamera", false);
    oCsDefault = Field(kCamSys, "CameraSystem", "m_defaultCamera", false); oCsBrain = Field(kCamSys, "CameraSystem", "m_brain", false);
    mBrainActive = Opt(FindClass("Cinemachine", "CinemachineBrain"), "get_ActiveVirtualCamera", 0);
    kVcamBase = FindClass("Cinemachine", "CinemachineVirtualCameraBase");
    Log("HOOKMAP CinemachineBrain.get_ActiveVirtualCamera %s", mBrainActive ? "found" : "not found (cutscene plane follows the player)");
    oUrpRenderer = Field(kUrpCam, "UACD", "m_RendererIndex"); oUrpPost = Field(kUrpCam, "UACD", "m_RenderPostProcessing");
    oUrpAA = Field(kUrpCam, "UACD", "m_Antialiasing"); oUrpAAQ = Field(kUrpCam, "UACD", "m_AntialiasingQuality");
    oUrpShadows = Field(kUrpCam, "UACD", "m_RenderShadows"); oUrpVolMask = Field(kUrpCam, "UACD", "m_VolumeLayerMask");
    oUrpVolTrig = Field(kUrpCam, "UACD", "m_VolumeTrigger"); oUrpDepthOpt = Field(kUrpCam, "UACD", "m_RequiresDepthTextureOption");
    oUrpColorOpt = Field(kUrpCam, "UACD", "m_RequiresOpaqueTextureOption"); oUrpStopNaN = Field(kUrpCam, "UACD", "m_StopNaN");
    oUrpDither = Field(kUrpCam, "UACD", "m_Dithering");
    void* kVM = FindClass("UnityEngine.Rendering", "VolumeManager");
    mVolMgrInstance = Need(kVM, "VolumeManager", "get_instance", 0); oVolMgrVolumes = Field(kVM, "VolumeManager", "m_Volumes");
    oVolProfile = Field(FindClass("UnityEngine.Rendering", "Volume"), "Volume", "sharedProfile");
    oProfComps = Field(FindClass("UnityEngine.Rendering", "VolumeProfile"), "VolumeProfile", "components");
    oCompActive = Field(FindClass("UnityEngine.Rendering", "VolumeComponent"), "VolumeComponent", "active");
    void* kIS = FindClass("UnityEngine.InputSystem", "InputSystem");
    mListEnabledActions = Need(kIS, "InputSystem", "ListEnabledActions", 0);
    void* kAct = FindClass("UnityEngine.InputSystem", "InputAction");
    mActionEnable = Need(kAct, "InputAction", "Enable", 0); mActionDisable = Need(kAct, "InputAction", "Disable", 0);
    g_issue = (IssuePluginEvent_t)il2cpp_resolve_icall("UnityEngine.GL::GLIssuePluginEvent");
    Log("HOOKMAP icall GL::GLIssuePluginEvent %s", g_issue ? "found" : "NOT FOUND");
    void* kPipe = FindClass("UnityEngine.Rendering.Universal", "UniversalRenderPipeline");
    mRender = Need(kPipe, "UniversalRenderPipeline", "Render", 2, "UnityEngine.Rendering.ScriptableRenderContext,System.Collections.Generic.List<UnityEngine.Camera>");
    Log("HOOKMAP %d required item(s) not found", g_missing);
    if (g_missing || !g_issue || !mRender) { g_vrOn = false; Log("VR stays off: names missing (the game may have been updated)"); return; }
    if (!Hook(mRender, (void*)&hkRender, (void**)&oRender, "UniversalRenderPipeline.Render(List<Camera>)")) { g_vrOn = false; return; }
    Hook(Need(kPipe, "UniversalRenderPipeline", "RenderCameraStack", 2), (void*)&hkStack, (void**)&oStack, "UniversalRenderPipeline.RenderCameraStack");
    HookWebRequests();
    HookExit();
}

// The proxy calls this on the main thread from its il2cpp_runtime_invoke pass-through, until setup has run once.
typedef void (*SetCb_t)(void (*)());
static SetCb_t g_setCb;
static void OnMainThread() {
    static bool done = false;
    if (done) return;
    done = true;
    g_setCb(nullptr);
    Setup();
}

static DWORD WINAPI InitThread(void*) {
    g_t0 = Now();
    HMODULE proxy = GetModuleHandleW((g_gameDir + L"winhttp.dll").c_str());
    g_setCb = proxy ? (SetCb_t)GetProcAddress(proxy, "LCVR_SetMainThreadCallback") : nullptr;
    if (!g_setCb) { Log("START the game folder's winhttp.dll is not this mod's (no LCVR_SetMainThreadCallback): VR off. Reinstall the mod."); return 0; }
    while (!(g_ga = GetModuleHandleW(L"GameAssembly.dll"))) Sleep(1);
    MH_Initialize();
    if (!BindIl2cpp()) return 0;
    g_setCb(&OnMainThread);
    Log("START waiting for the main thread (GameAssembly seen after %.3f s)", Now() - g_t0);
    return 0;
}

// ---------------------------------------------------------------- crash logging
// A crash must leave its place in LostCrownVR.log: module and offset of the fault and of return addresses on the
// stack (first 4 fatal-looking exceptions). IL2CPP turns some access violations into NullReferenceExceptions, so a
// logged line is not always a crash.
static std::string ModOff(uintptr_t a) {
    HMODULE m = nullptr; char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
        GetModuleFileNameA(m, name, MAX_PATH); const char* b = strrchr(name, '\\'); char o[MAX_PATH + 32];
        sprintf_s(o, "%s+0x%llx", b ? b + 1 : name, (unsigned long long)(a - (uintptr_t)m)); return o;
    }
    char o[32]; sprintf_s(o, "0x%llx", (unsigned long long)a); return o;
}
static volatile LONG g_crashLogged = 0;
static bool SafeRead(uintptr_t* p, uintptr_t* out) { __try { *out = *p; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW && code != 0xC0000374) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_crashLogged) > 4) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t at = (uintptr_t)e->ExceptionRecord->ExceptionAddress;
    std::string line = "CRASH? exception " + std::to_string(code) + " at " + ModOff(at) + " thread " + std::to_string(GetCurrentThreadId()) + "; stack:";
    uintptr_t* sp = (uintptr_t*)e->ContextRecord->Rsp; int found = 0;
    for (int i = 0; i < 256 && found < 12; i++) {
        uintptr_t v = 0; if (!SafeRead(sp + i, &v)) break;
        HMODULE m = nullptr;
        if (v > 0x10000 && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)v, &m) && m) { line += " " + ModOff(v); found++; }
    }
    Log("%s", line.c_str());
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        QueryPerformanceFrequency(&g_qpf); QueryPerformanceCounter(&g_qp0);
        timeBeginPeriod(1);
        wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
        g_gameDir = p; g_gameDir = g_gameDir.substr(0, g_gameDir.find_last_of(L"\\/") + 1);
        g_log = _wfopen((g_gameDir + L"LostCrownVR.log").c_str(), L"w");
        LoadSettings(); SaveSettings();
        Log("LostCrownVR %s (build %d), built " __DATE__ " " __TIME__, MOD_VERSION, MOD_BUILD);
        Log("SETTINGS cutscene plane %d, cutscene depth %.2f, plane smoothing %.2f s", g_set.planeSource, g_set.cutsceneDepth, g_set.smoothSec);
        Log("SETTINGS stage %.2f m wide at %.2f m, depth %.2f, resolution %.2f, picture %d, eye span last run %.3f, menu %.2f / %+.2f m / %.2f m, lab commands %s",
            g_set.stageWidth, g_set.stageDistance, g_set.depth, g_set.resolution, g_set.picture, g_set.tanSpan, g_set.menuSize, g_set.menuHeight, g_set.menuDistance,
            g_set.labCommands ? "ON" : "off");
        lcxr::SetLogger(&Log);
        AddVectoredExceptionHandler(0, CrashHandler);
        CloseHandle(CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr));
    }
    return TRUE;
}

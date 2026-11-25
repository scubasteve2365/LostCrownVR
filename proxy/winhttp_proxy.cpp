// winhttp.dll proxy for Prince of Persia: The Lost Crown VR.
// UnityPlayer.dll imports winhttp.dll, so Windows loads this file from the game folder first.
// Every export jumps to the real System32 winhttp.dll (thunks.asm). Then:
//  1. redirects UnityPlayer.dll's GetProcAddress import (its import table, the same place BepInEx's Doorstop hooks), so
//     when UnityPlayer asks GameAssembly.dll for il2cpp_init and il2cpp_runtime_invoke it gets small pass-throughs here.
//     They call the game's real exports unchanged: this game's IL2CPP exports are Denuvo wrappers, and nothing in
//     GameAssembly.dll is patched. The pass-through gives the mod one call on the game's main thread once IL2CPP runs.
//  2. chain-loads winhttp_chain.dll if present (another mod's winhttp.dll renamed, for example BepInEx 6 Doorstop),
//  3. loads LostCrownVR\LostCrownVRNative.dll on a new thread (never LoadLibrary inside DllMain's loader lock).
// From AI Limit VR and Ori WotW VR (same author, MIT); the import-table idea is Doorstop's (BepInEx, LGPL, idea only).
#include <windows.h>
#include <string>
#include <atomic>

extern "C" UINT64 g_real[];
static const char* kNames[] = {
#include "names.inc"
};

static std::wstring GameDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

static void Log(const char* msg) {
    std::wstring path = GameDir() + L"LostCrownVR_proxy.log";
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t; GetLocalTime(&t);
    char line[600];
    int n = wsprintfA(line, "%02d:%02d:%02d.%03d %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    DWORD w; WriteFile(h, line, n, &w, nullptr);
    CloseHandle(h);
}

// ---------------------------------------------------------------- main-thread entry for the mod
typedef int (*Il2cppInit_t)(const char*);
typedef void* (*RuntimeInvoke_t)(void*, void*, void**, void**);
typedef void (*MainThreadFn_t)();
static Il2cppInit_t g_realInit;
static RuntimeInvoke_t g_realInvoke;
static std::atomic<MainThreadFn_t> g_onMain{nullptr};  // set by LostCrownVRNative.dll (LCVR_SetMainThreadCallback)
static std::atomic<int> g_il2cppReady{0};
static DWORD g_mainThread;

static int PassInit(const char* domain) {
    int r = g_realInit(domain);
    g_mainThread = GetCurrentThreadId();
    g_il2cppReady = 1;
    Log("PROXY il2cpp_init returned on the main thread");
    return r;
}
static void* PassInvoke(void* m, void* obj, void** params, void** exc) {
    // Cheap test on every call; the mod's callback runs on the main thread only, and it clears itself when done.
    MainThreadFn_t f = g_onMain.load(std::memory_order_relaxed);
    if (f && g_il2cppReady && GetCurrentThreadId() == g_mainThread) f();
    return g_realInvoke(m, obj, params, exc);
}
extern "C" __declspec(dllexport) void LCVR_SetMainThreadCallback(MainThreadFn_t f) { g_onMain = f; }
extern "C" __declspec(dllexport) int LCVR_Il2cppReady() { return g_il2cppReady; }

typedef FARPROC(WINAPI* GetProcAddress_t)(HMODULE, LPCSTR);
static GetProcAddress_t g_realGPA;
static ULONG_PTR* g_slot;  // the UnityPlayer import slot we redirected
static FARPROC WINAPI PassGetProcAddress(HMODULE mod, LPCSTR name) {
    FARPROC p = g_realGPA(mod, name);
    if (!p || ((ULONG_PTR)name >> 16) == 0) return p;  // ordinals pass through
    if (!g_realInit && strcmp(name, "il2cpp_init") == 0) { g_realInit = (Il2cppInit_t)p; Log("PROXY il2cpp_init found by UnityPlayer, pass-through given"); return (FARPROC)&PassInit; }
    if (!g_realInvoke && strcmp(name, "il2cpp_runtime_invoke") == 0) { g_realInvoke = (RuntimeInvoke_t)p; Log("PROXY il2cpp_runtime_invoke found by UnityPlayer, pass-through given"); return (FARPROC)&PassInvoke; }
    return p;
}

// Replace GetProcAddress in UnityPlayer.dll's import table (kernel32 import). Returns the number of entries changed.
static int RedirectImport(HMODULE mod) {
    auto base = (BYTE*)mod;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    FARPROC target = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcAddress");
    int changed = 0;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        auto thunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; thunk->u1.Function; thunk++) {
            if ((FARPROC)thunk->u1.Function != target) continue;
            DWORD old;
            if (VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                g_realGPA = (GetProcAddress_t)target;
                thunk->u1.Function = (ULONG_PTR)&PassGetProcAddress;
                g_slot = &thunk->u1.Function;
                VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
                changed++;
            }
        }
    }
    return changed;
}

static DWORD WINAPI LoadThread(void*) {
    std::wstring chain = GameDir() + L"winhttp_chain.dll";
    if (GetFileAttributesW(chain.c_str()) != INVALID_FILE_ATTRIBUTES) {
        HMODULE c = LoadLibraryW(chain.c_str());
        Log(c ? "PROXY chain-loaded winhttp_chain.dll" : "PROXY winhttp_chain.dll found but failed to load");
        // Another loader (BepInEx's Doorstop, for one) redirects the same UnityPlayer import. If it replaced ours, its
        // hook becomes the next step and ours goes back in front, so both still see UnityPlayer's lookups.
        if (g_slot && *g_slot != (ULONG_PTR)&PassGetProcAddress) {
            DWORD old;
            if (VirtualProtect(g_slot, sizeof(void*), PAGE_READWRITE, &old)) {
                g_realGPA = (GetProcAddress_t)*g_slot;
                *g_slot = (ULONG_PTR)&PassGetProcAddress;
                VirtualProtect(g_slot, sizeof(void*), old, &old);
                Log("PROXY the chained DLL had redirected UnityPlayer's GetProcAddress too: it now runs after this mod's pass-through");
            }
        }
    } else {
        Log("PROXY no winhttp_chain.dll (fine)");
    }
    std::wstring dir = GameDir() + L"LostCrownVR\\";
    SetDllDirectoryW(dir.c_str());
    HMODULE m = LoadLibraryW((dir + L"LostCrownVRNative.dll").c_str());
    char msg[200];
    wsprintfA(msg, m ? "PROXY loaded LostCrownVRNative.dll" : "PROXY LostCrownVRNative.dll failed to load, error %lu", GetLastError());
    Log(msg);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        HMODULE real = LoadLibraryW((std::wstring(sys) + L"\\winhttp.dll").c_str());
        int missing = 0;
        for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
            g_real[i] = real ? (UINT64)GetProcAddress(real, kNames[i]) : 0;
            if (!g_real[i]) missing++;
        }
        HMODULE up = GetModuleHandleW(L"UnityPlayer.dll");
        int redirected = up ? RedirectImport(up) : 0;
        char msg[200];
        wsprintfA(msg, "PROXY winhttp proxy attached, real winhttp %s, %d of %d exports missing; UnityPlayer %s, GetProcAddress import redirected %d",
                  real ? "loaded" : "NOT loaded", missing, (int)(sizeof(kNames) / sizeof(kNames[0])), up ? "found" : "NOT found", redirected);
        Log(msg);
        CloseHandle(CreateThread(nullptr, 0, LoadThread, nullptr, 0, nullptr));
    }
    return TRUE;
}

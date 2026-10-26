// w3_photo_addon.cpp
// Milestone 1 (ReShade add-on version): press F9, save the current frame to W3Photos\photo_<tick>.png
//
// UNTESTED SKELETON. Build as x64 DLL, name it w3_photo_capture.addon64,
// put it next to witcher3.exe (bin\x64_dx12) with ReShade 6.8+ (full add-on support) installed.
// Dependencies: ReShade add-on SDK headers (crosire/reshade, "include" folder), stb_image_write.h
//
// API calls to double-check against your reshade.hpp version:
//   effect_runtime::get_screenshot_width_and_height, effect_runtime::capture_screenshot,
//   addon_event::reshade_present (signature: void(effect_runtime*))

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <imgui.h>        // must come before reshade.hpp so ReShade's ImGui function table is used
#include <reshade.hpp>

#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>
#include <set>
#include <algorithm>
#include <string>
#include <objbase.h>
#include <shlobj.h>
#include <knownfolders.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

extern "C" __declspec(dllexport) const char* NAME = "Witcher 3 Photo Capture";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Saves a screenshot on F9 (milestone 1 of the magic camera mod).";

// ---------- logging ----------
// g_dir = folder containing this .addon64 file (ends with a backslash). All our files go here,
// because the game can change its working directory after startup.
static char g_dir[MAX_PATH] = "";

static DWORD WINAPI WatcherThread(LPVOID);
static volatile LONG g_watcherStarted = 0;
static volatile LONG g_stop = 0;

// GPU objects waiting to be destroyed. They are kept alive for a few frames so draws that are
// still in flight do not use a freed texture.
struct DeadGpu {
    reshade::api::device* dev;
    reshade::api::resource res;
    reshade::api::resource_view view;
    unsigned long long dieAtFrame;
};
static std::vector<DeadGpu> g_dead;
static unsigned long long g_frameCount = 0;

static bool g_viewerOpen = false;
static volatile LONG g_logTextures = 0;   // logging of newly created textures (F8, texlog.now file, or overlay button)
static unsigned long long g_logUntil = 0; // tick count when timed logging ends (0 = manual, stays on)
static unsigned long long g_resSeen = 0;  // how many resources of any kind were created while logging was on

static void StartTextureLog(unsigned long long ms) {
    g_resSeen = 0;
    g_logUntil = GetTickCount64() + ms;
    InterlockedExchange(&g_logTextures, 1);
    Log("texture logging ON for %llu seconds", ms / 1000);
}
static void ToggleViewer(reshade::api::effect_runtime* runtime);

static void Log(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf); OutputDebugStringA("\n");
    char logPath[MAX_PATH * 2];
    snprintf(logPath, sizeof(logPath), "%sw3_photo_capture.log", g_dir);
    if (FILE* f = fopen(logPath, "a")) { fprintf(f, "%s\n", buf); fclose(f); }
}

// ---------- capture ----------
static void Capture(reshade::api::effect_runtime* runtime) {
    Log("F9 pressed: calling get_screenshot_width_and_height");
    uint32_t w = 0, h = 0;
    runtime->get_screenshot_width_and_height(&w, &h);
    Log("size reported: %u x %u", w, h);
    if (w == 0 || h == 0 || w > 16384 || h > 16384) { Log("capture: bad size, stopping (possible ABI mismatch)"); return; }

    std::vector<uint8_t> px((size_t)w * h * 4);   // 8-bit RGBA
    Log("calling capture_screenshot");
    const bool captured = runtime->capture_screenshot(px.data());
    Log("capture_screenshot returned %d", (int)captured);
    if (!captured) { Log("capture: capture_screenshot failed (HDR back buffer?)"); return; }

    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;   // make sure alpha is opaque

    char folder[MAX_PATH * 2];
    snprintf(folder, sizeof(folder), "%sW3Photos", g_dir);
    CreateDirectoryA(folder, nullptr);
    char path[MAX_PATH * 2];
    snprintf(path, sizeof(path), "%s\\photo_%llu.png", folder, (unsigned long long)GetTickCount64());

    if (stbi_write_png(path, (int)w, (int)h, 4, px.data(), (int)w * 4)) {
        char full[MAX_PATH];
        GetFullPathNameA(path, MAX_PATH, full, nullptr);
        Log("saved %s (%ux%u)", full, w, h);
    } else {
        Log("capture: PNG write failed");
    }
}

// Called by ReShade once per frame after effects are rendered.
static void RunCapture(reshade::api::effect_runtime* runtime) {
    Capture(runtime);
}

static void OnReshadePresent(reshade::api::effect_runtime* runtime) {
    static bool wasDown = false;
    static unsigned long long frame = 0;
    ++frame;
    g_frameCount = frame;

    // destroy textures that were replaced at least a few frames ago
    for (size_t i = 0; i < g_dead.size();) {
        if (frame >= g_dead[i].dieAtFrame) {
            if (g_dead[i].view.handle) g_dead[i].dev->destroy_resource_view(g_dead[i].view);
            if (g_dead[i].res.handle)  g_dead[i].dev->destroy_resource(g_dead[i].res);
            g_dead.erase(g_dead.begin() + i);
        } else {
            ++i;
        }
    }

    // Start the folder watcher from the render thread instead of DllMain (avoids loader-lock hazards).
    // Build with -DDISABLE_WATCHER to turn it off for troubleshooting.
#ifndef DISABLE_WATCHER
    if (frame == 1 && InterlockedCompareExchange(&g_watcherStarted, 1, 0) == 0)
        CreateThread(nullptr, 0, WatcherThread, nullptr, 0, nullptr);
#endif

    // heartbeat: proves this callback is actually being called
    if (frame == 1 || frame % 1800 == 0) Log("present callback alive, frame %llu", frame);

    // trigger 1: F9 key (edge-detected: one press = one photo)
    const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (down && !wasDown) { Log("F9 detected"); RunCapture(runtime); }
    wasDown = down;

    // F10: show / hide the full-screen photo viewer
    static bool viewerKeyWasDown = false;
    const bool viewerKey = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (viewerKey && !viewerKeyWasDown) ToggleViewer(runtime);
    viewerKeyWasDown = viewerKey;

    // F8: start / stop logging every large texture the game creates (used to identify a painting's texture)
    static bool logKeyWasDown = false;
    const bool logKey = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (logKey && !logKeyWasDown) {
        g_logUntil = 0;   // manual mode: stays on until F8 is pressed again
        g_resSeen = 0;
        InterlockedExchange(&g_logTextures, g_logTextures ? 0 : 1);
        Log("texture logging %s", g_logTextures ? "ON" : "OFF");
    }
    logKeyWasDown = logKey;

    // timed logging ends by itself
    if (g_logTextures && g_logUntil != 0 && GetTickCount64() >= g_logUntil) {
        InterlockedExchange(&g_logTextures, 0);
        Log("texture logging OFF (timer); %llu resources were created while it was on", g_resSeen);
    }

    // key-independent trigger: create a file named texlog.now next to the add-on
    char texTrigger[MAX_PATH * 2];
    snprintf(texTrigger, sizeof(texTrigger), "%stexlog.now", g_dir);
    if (frame % 30 == 0 && GetFileAttributesA(texTrigger) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA(texTrigger);
        StartTextureLog(30000);
    }

    // trigger 2: create a file named capture.now in the log's folder (works even if key input doesn't)
    char triggerPath[MAX_PATH * 2];
    snprintf(triggerPath, sizeof(triggerPath), "%scapture.now", g_dir);
    if (frame % 30 == 0 && GetFileAttributesA(triggerPath) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA(triggerPath);
        Log("capture.now found");
        RunCapture(runtime);
    }
}

// ---------- photo mode screenshot watcher ----------
// The game's photo mode saves PNGs to Documents\The Witcher 3\screenshots.
// A background thread notices each new file, waits until it is fully written,
// decodes it, and keeps the newest one in g_latest (RGBA8) for the texture step.

struct Photo {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    std::wstring path;
    unsigned long long id = 0;   // increments with every new photo
};

static SRWLOCK g_photoLock = SRWLOCK_INIT;
static Photo g_latest;
static volatile LONG g_photoCounter = 0;

static std::string Narrow(const std::wstring& ws) {
    char buf[1024];
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
    return n > 0 ? std::string(buf) : std::string("?");
}

static std::wstring ScreenshotDir() {
    PWSTR docs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) || !docs) return L"";
    std::wstring dir = std::wstring(docs) + L"\\The Witcher 3\\screenshots";
    CoTaskMemFree(docs);
    return dir;
}

static bool FileSize(const std::wstring& path, ULONGLONG& size) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return false;
    size = ((ULONGLONG)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    return true;
}

// true once the file size has stopped changing and the file can be opened
static bool WaitUntilWritten(const std::wstring& path) {
    for (int i = 0; i < 15 && !g_stop; ++i) {
        ULONGLONG a = 0, b = 0;
        if (!FileSize(path, a)) { Sleep(200); continue; }
        Sleep(250);
        if (!FileSize(path, b)) continue;
        if (a == b && a > 0) {
            if (FILE* f = _wfopen(path.c_str(), L"rb")) { fclose(f); return true; }
        }
    }
    return false;
}

static void LoadPhoto(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) { Log("watcher: cannot open %s", Narrow(path).c_str()); return; }
    int w = 0, h = 0, comp = 0;
    unsigned char* data = stbi_load_from_file(f, &w, &h, &comp, 4);
    fclose(f);
    if (!data) { Log("watcher: decode failed for %s: %s", Narrow(path).c_str(), stbi_failure_reason()); return; }

    Photo p;
    p.w = w; p.h = h; p.path = path;
    p.rgba.assign(data, data + (size_t)w * h * 4);
    stbi_image_free(data);
    p.id = (unsigned long long)InterlockedIncrement(&g_photoCounter);
    const unsigned long long newId = p.id;
    AcquireSRWLockExclusive(&g_photoLock);
    g_latest = std::move(p);
    ReleaseSRWLockExclusive(&g_photoLock);
    Log("watcher: new photo #%llu loaded (%dx%d) %s", newId, w, h, Narrow(path).c_str());
}

static void ScanFolder(const std::wstring& dir, std::set<std::wstring>& seen, bool initial) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.png").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        if (seen.count(name)) continue;
        if (initial) { seen.insert(name); continue; }   // existing photos are not "new"
        const std::wstring full = dir + L"\\" + name;
        if (WaitUntilWritten(full)) { seen.insert(name); LoadPhoto(full); }
    } while (FindNextFileW(h, &fd) && !g_stop);
    FindClose(h);
}

static DWORD WINAPI WatcherThread(LPVOID) {
    const std::wstring dir = ScreenshotDir();
    if (dir.empty()) { Log("watcher: could not resolve Documents folder"); return 0; }
    Log("watcher: watching %s", Narrow(dir).c_str());

    std::set<std::wstring> seen;
    ScanFolder(dir, seen, true);
    Log("watcher: %zu existing png files ignored", seen.size());

    while (!g_stop) {
        ScanFolder(dir, seen, false);
        Sleep(500);
    }
    return 0;
}

// ---------- photo picker overlay ----------
// Open the ReShade overlay (Home key by default) and find the "Witcher 3 Photos" window.
// It lists the PNGs in the screenshots folder, newest first. Clicking one decodes it, center-crops it
// to the painting's aspect ratio, resizes it to PAINT_W x PAINT_H, uploads it to a GPU texture and
// shows it as a preview. g_paintingRGBA holds the same pixels for the later painting-texture step.

static const uint32_t PAINT_W = 1024;
static const uint32_t PAINT_H = 576;

struct PhotoItem {
    std::wstring path;
    std::string name;
    FILETIME time;
};

static std::vector<PhotoItem> g_list;
static bool g_listDirty = true;
static std::wstring g_selectedPath;
static std::vector<uint8_t> g_paintingRGBA;                 // PAINT_W x PAINT_H, RGBA8
static reshade::api::resource g_res = { 0 };
static reshade::api::resource_view g_view = { 0 };

static void RefreshList() {
    g_list.clear();
    const std::wstring dir = ScreenshotDir();
    if (dir.empty()) return;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.png").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        PhotoItem it;
        it.path = dir + L"\\" + fd.cFileName;
        it.name = Narrow(fd.cFileName);
        it.time = fd.ftLastWriteTime;
        g_list.push_back(it);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(g_list.begin(), g_list.end(), [](const PhotoItem& a, const PhotoItem& b) {
        return CompareFileTime(&a.time, &b.time) > 0;   // newest first
    });
}

// Center-crop src to the destination aspect ratio, then bilinear-resample to dw x dh (RGBA8).
static void ResizeCrop(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh) {
    const double srcAspect = (double)sw / sh;
    const double dstAspect = (double)dw / dh;
    double cx = 0, cy = 0, cw = sw, ch = sh;
    if (srcAspect > dstAspect) { cw = sh * dstAspect; cx = (sw - cw) / 2.0; }
    else                       { ch = sw / dstAspect; cy = (sh - ch) / 2.0; }

    for (int y = 0; y < dh; ++y) {
        double fy = cy + (y + 0.5) * ch / dh - 0.5;
        int y0 = (int)fy; if (fy < 0) { y0 = 0; fy = 0; }
        int y1 = std::min(y0 + 1, sh - 1);
        y0 = std::min(std::max(y0, 0), sh - 1);
        const double ty = fy - y0;
        for (int x = 0; x < dw; ++x) {
            double fx = cx + (x + 0.5) * cw / dw - 0.5;
            int x0 = (int)fx; if (fx < 0) { x0 = 0; fx = 0; }
            int x1 = std::min(x0 + 1, sw - 1);
            x0 = std::min(std::max(x0, 0), sw - 1);
            const double tx = fx - x0;
            for (int c = 0; c < 4; ++c) {
                const double p00 = src[((size_t)y0 * sw + x0) * 4 + c];
                const double p10 = src[((size_t)y0 * sw + x1) * 4 + c];
                const double p01 = src[((size_t)y1 * sw + x0) * 4 + c];
                const double p11 = src[((size_t)y1 * sw + x1) * 4 + c];
                const double v = (p00 * (1 - tx) + p10 * tx) * (1 - ty) + (p01 * (1 - tx) + p11 * tx) * ty;
                dst[((size_t)y * dw + x) * 4 + c] = (uint8_t)(v + 0.5);
            }
        }
    }
}

// The selected photo's path is stored next to the add-on so it survives restarts.
static std::wstring Widen(const std::string& s) {
    wchar_t buf[1024];
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, buf, 1024);
    return n > 0 ? std::wstring(buf) : std::wstring();
}

static std::string SelectedFilePath() { return std::string(g_dir) + "selected_photo.txt"; }

static void SaveSelectedPath(const std::wstring& p) {
    if (FILE* f = fopen(SelectedFilePath().c_str(), "wb")) {
        const std::string s = Narrow(p);
        fwrite(s.data(), 1, s.size(), f);
        fclose(f);
    }
}

static std::wstring LoadSelectedPath() {
    std::wstring out;
    if (FILE* f = fopen(SelectedFilePath().c_str(), "rb")) {
        char buf[2048];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        out = Widen(buf);
    }
    return out;
}

static void SelectPhoto(reshade::api::effect_runtime* runtime, const PhotoItem& item) {
    FILE* f = _wfopen(item.path.c_str(), L"rb");
    if (!f) { Log("picker: cannot open %s", item.name.c_str()); return; }
    int w = 0, h = 0, comp = 0;
    unsigned char* data = stbi_load_from_file(f, &w, &h, &comp, 4);
    fclose(f);
    if (!data) { Log("picker: decode failed for %s: %s", item.name.c_str(), stbi_failure_reason()); return; }

    std::vector<uint8_t> out((size_t)PAINT_W * PAINT_H * 4);
    ResizeCrop(data, w, h, out.data(), (int)PAINT_W, (int)PAINT_H);
    stbi_image_free(data);

    reshade::api::device* dev = runtime->get_device();
    reshade::api::subresource_data init = {};
    init.data = out.data();
    init.row_pitch = PAINT_W * 4;
    init.slice_pitch = PAINT_W * PAINT_H * 4;

    const reshade::api::resource_desc desc(
        PAINT_W, PAINT_H, 1, 1,
        reshade::api::format::r8g8b8a8_unorm, 1,
        reshade::api::memory_heap::gpu_only,
        reshade::api::resource_usage::shader_resource | reshade::api::resource_usage::copy_dest);

    reshade::api::resource res = { 0 };
    reshade::api::resource_view view = { 0 };
    if (!dev->create_resource(desc, &init, reshade::api::resource_usage::shader_resource, &res)) {
        Log("picker: create_resource failed");
        return;
    }
    if (!dev->create_resource_view(res, reshade::api::resource_usage::shader_resource,
                                   reshade::api::resource_view_desc(reshade::api::format::r8g8b8a8_unorm), &view)) {
        Log("picker: create_resource_view failed");
        dev->destroy_resource(res);
        return;
    }

    // retire the previous texture a few frames from now instead of destroying it immediately
    if (g_view.handle || g_res.handle) {
        DeadGpu d = { dev, g_res, g_view, g_frameCount + 10 };
        g_dead.push_back(d);
    }
    g_res = res;
    g_view = view;
    g_selectedPath = item.path;
    g_paintingRGBA = std::move(out);
    SaveSelectedPath(item.path);
    Log("picker: selected %s (%dx%d -> %ux%u)", item.name.c_str(), w, h, PAINT_W, PAINT_H);
}

static void DrawOverlay(reshade::api::effect_runtime* runtime) {
    static LONG lastCounter = -1;
    if (g_listDirty || lastCounter != g_photoCounter) {
        RefreshList();
        lastCounter = g_photoCounter;
        g_listDirty = false;
    }

    ImGui::TextUnformatted("Photo mode screenshots (newest first)");
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) g_listDirty = true;

    ImGui::BeginChild("w3photolist", ImVec2(0, 150), true);
    for (size_t i = 0; i < g_list.size(); ++i) {
        const bool selected = (g_list[i].path == g_selectedPath);
        if (ImGui::Selectable(g_list[i].name.c_str(), selected)) SelectPhoto(runtime, g_list[i]);
    }
    if (g_list.empty()) ImGui::TextUnformatted("No PNG files found in the screenshots folder.");
    ImGui::EndChild();

    if (g_view.handle != 0) {
        ImGui::Text("Painting image (%ux%u):", PAINT_W, PAINT_H);
        ImGui::Image((ImTextureID)g_view.handle, ImVec2(480, 270));
    } else {
        ImGui::TextUnformatted("Click a photo above to preview it.");
    }

    ImGui::Separator();
    if (ImGui::Button("Log new textures for 30 seconds")) StartTextureLog(30000);
    ImGui::SameLine();
    ImGui::TextUnformatted(g_logTextures ? "(logging...)" : "");
}

// ---------- full-screen photo viewer ----------
// Opened and closed with F10 for now. Later the in-game paper item will open it instead.
// It is drawn from the reshade_overlay event, so it shows even when the ReShade overlay is closed.
// (While the ReShade overlay is closed ImGui gets no mouse input, so the viewer is closed with the key.)

static void ToggleViewer(reshade::api::effect_runtime* runtime) {
    if (g_viewerOpen) { g_viewerOpen = false; Log("viewer: closed"); return; }

    if (g_view.handle == 0) {
        const std::wstring p = LoadSelectedPath();
        if (p.empty()) { Log("viewer: no photo selected yet (pick one in the Witcher 3 Photos window)"); return; }
        PhotoItem it;
        it.path = p;
        it.name = Narrow(p);
        it.time.dwLowDateTime = 0;
        it.time.dwHighDateTime = 0;
        SelectPhoto(runtime, it);
        if (g_view.handle == 0) return;
    }
    g_viewerOpen = true;
    Log("viewer: opened");
}

static void OnReshadeOverlay(reshade::api::effect_runtime*) {
    if (!g_viewerOpen || g_view.handle == 0) return;

    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    float w = screen.x * 0.70f;
    float h = w * (float)PAINT_H / (float)PAINT_W;
    if (h > screen.y * 0.70f) {
        h = screen.y * 0.70f;
        w = h * (float)PAINT_W / (float)PAINT_H;
    }
    const float pad = 16.0f;

    ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w + pad * 2, h + pad * 2 + 28.0f), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::Begin("##w3photoviewer", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
    ImGui::Image((ImTextureID)g_view.handle, ImVec2(w, h));
    ImGui::TextUnformatted("Press F10 to close");
    ImGui::End();
    ImGui::PopStyleVar();
}

// ---------- texture finder (discovery tool) ----------
// Press F8, do the thing in the game (for example open a vanilla painting from the inventory), press F8 again.
// Every 2D texture of 64x64 or larger created in between is written to the log with its size and format.

static void OnInitResource(reshade::api::device*, const reshade::api::resource_desc& desc,
                           const reshade::api::subresource_data*, reshade::api::resource_usage,
                           reshade::api::resource res) {
    if (!g_logTextures) return;
    ++g_resSeen;
    if (desc.type != reshade::api::resource_type::texture_2d) return;
    if (desc.texture.width < 64 || desc.texture.height < 64) return;

    Log("tex: %ux%u format=%u mips=%u layers=%u heap=%u usage=0x%x handle=%llx",
        (unsigned)desc.texture.width, (unsigned)desc.texture.height,
        (unsigned)desc.texture.format, (unsigned)desc.texture.levels,
        (unsigned)desc.texture.depth_or_layers, (unsigned)desc.heap,
        (unsigned)desc.usage, (unsigned long long)res.handle);
}

// ---------- add-on entry point ----------
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        {
            char me[MAX_PATH] = "";
            GetModuleFileNameA(module, me, MAX_PATH);
            if (char* slash = strrchr(me, '\\')) { slash[1] = 0; strncpy(g_dir, me, MAX_PATH - 1); }
        }
        if (!reshade::register_addon(module)) return FALSE;
        reshade::register_event<reshade::addon_event::reshade_present>(OnReshadePresent);
        reshade::register_overlay("Witcher 3 Photos", DrawOverlay);
        reshade::register_event<reshade::addon_event::reshade_overlay>(OnReshadeOverlay);
        reshade::register_event<reshade::addon_event::init_resource>(OnInitResource);
        Log("w3_photo_capture add-on registered (build %s %s, dir %s)", __DATE__, __TIME__, g_dir);
        break;
    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);   // thread exits on its own; do not join inside DllMain
        reshade::unregister_addon(module);
        break;
    }
    return TRUE;
}

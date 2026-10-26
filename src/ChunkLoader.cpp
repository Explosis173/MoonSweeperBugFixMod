// Moon Sweepers "ChunkLoader" mod.
//
// The game only fully simulates things near an "observer" (players, vehicles):
//   - MoonDepotConstruction.Update deactivates machines/docks further than 425 m,
//   - MoonConveyorNetwork.Update does the same for belts,
//   - MoonRobotVacuum only moves within 400 m, and far robots fall back to a
//     coarse simulation (StepFarRobots, 50% efficiency, every 2 s).
// This mod adds invisible anchor transforms to MoonWorld.observers at every
// cluster of machines/belts (and optionally at every active robot), so those
// areas behave exactly as if a player was standing there.
//
// It also spreads launched dust capsules over all capsule catchers: the game
// sends every capsule to the nearest catcher, which then queues them 9 s apart
// (MoonWorld.ReserveCatch) while other catchers sit idle.
//
// Loaded as a version.dll proxy; talks to the game only through the public
// il2cpp C API exported by GameAssembly.dll, and hooks MoonWorld.Update so all
// Unity calls happen on the main thread.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <unordered_map>
#include <algorithm>

// ---- version.dll forwarding -------------------------------------------------
#pragma comment(linker, "/export:GetFileVersionInfoA=C:\\Windows\\System32\\version.GetFileVersionInfoA")
#pragma comment(linker, "/export:GetFileVersionInfoByHandle=C:\\Windows\\System32\\version.GetFileVersionInfoByHandle")
#pragma comment(linker, "/export:GetFileVersionInfoExA=C:\\Windows\\System32\\version.GetFileVersionInfoExA")
#pragma comment(linker, "/export:GetFileVersionInfoExW=C:\\Windows\\System32\\version.GetFileVersionInfoExW")
#pragma comment(linker, "/export:GetFileVersionInfoSizeA=C:\\Windows\\System32\\version.GetFileVersionInfoSizeA")
#pragma comment(linker, "/export:GetFileVersionInfoSizeExA=C:\\Windows\\System32\\version.GetFileVersionInfoSizeExA")
#pragma comment(linker, "/export:GetFileVersionInfoSizeExW=C:\\Windows\\System32\\version.GetFileVersionInfoSizeExW")
#pragma comment(linker, "/export:GetFileVersionInfoSizeW=C:\\Windows\\System32\\version.GetFileVersionInfoSizeW")
#pragma comment(linker, "/export:GetFileVersionInfoW=C:\\Windows\\System32\\version.GetFileVersionInfoW")
#pragma comment(linker, "/export:VerFindFileA=C:\\Windows\\System32\\version.VerFindFileA")
#pragma comment(linker, "/export:VerFindFileW=C:\\Windows\\System32\\version.VerFindFileW")
#pragma comment(linker, "/export:VerInstallFileA=C:\\Windows\\System32\\version.VerInstallFileA")
#pragma comment(linker, "/export:VerInstallFileW=C:\\Windows\\System32\\version.VerInstallFileW")
#pragma comment(linker, "/export:VerLanguageNameA=C:\\Windows\\System32\\version.VerLanguageNameA")
#pragma comment(linker, "/export:VerLanguageNameW=C:\\Windows\\System32\\version.VerLanguageNameW")
#pragma comment(linker, "/export:VerQueryValueA=C:\\Windows\\System32\\version.VerQueryValueA")
#pragma comment(linker, "/export:VerQueryValueW=C:\\Windows\\System32\\version.VerQueryValueW")

// ---- il2cpp API -------------------------------------------------------------
typedef void Il2CppObject;
typedef void Il2CppClass;
typedef void Il2CppImage;
typedef void Il2CppAssembly;
typedef void Il2CppDomain;
typedef void FieldInfo;
typedef void MethodInfo;
// Unity 6 il2cpp: GC handles are pointer-sized, not uint32.
typedef void* GCHandle;

#define IL2CPP_API(ret, name, args) typedef ret (*name##_t) args; static name##_t name;
IL2CPP_API(Il2CppDomain*, il2cpp_domain_get, ())
IL2CPP_API(const Il2CppAssembly**, il2cpp_domain_get_assemblies, (const Il2CppDomain*, size_t*))
IL2CPP_API(const Il2CppImage*, il2cpp_assembly_get_image, (const Il2CppAssembly*))
IL2CPP_API(Il2CppClass*, il2cpp_class_from_name, (const Il2CppImage*, const char*, const char*))
IL2CPP_API(FieldInfo*, il2cpp_class_get_field_from_name, (Il2CppClass*, const char*))
IL2CPP_API(size_t, il2cpp_field_get_offset, (FieldInfo*))
IL2CPP_API(void, il2cpp_field_static_get_value, (FieldInfo*, void*))
IL2CPP_API(void, il2cpp_field_set_value, (Il2CppObject*, FieldInfo*, void*))
IL2CPP_API(const MethodInfo*, il2cpp_class_get_method_from_name, (Il2CppClass*, const char*, int))
IL2CPP_API(Il2CppObject*, il2cpp_runtime_invoke, (const MethodInfo*, void*, void**, Il2CppObject**))
IL2CPP_API(Il2CppObject*, il2cpp_object_new, (const Il2CppClass*))
IL2CPP_API(Il2CppObject*, il2cpp_string_new, (const char*))
IL2CPP_API(Il2CppObject*, il2cpp_array_new, (Il2CppClass*, uintptr_t))
IL2CPP_API(Il2CppObject*, il2cpp_array_new_specific, (Il2CppClass*, uintptr_t))
IL2CPP_API(Il2CppClass*, il2cpp_object_get_class, (Il2CppObject*))
IL2CPP_API(void*, il2cpp_object_unbox, (Il2CppObject*))
IL2CPP_API(GCHandle, il2cpp_gchandle_new, (Il2CppObject*, bool))
IL2CPP_API(Il2CppObject*, il2cpp_gchandle_get_target, (GCHandle))
IL2CPP_API(void, il2cpp_gchandle_free, (GCHandle))
IL2CPP_API(void*, il2cpp_thread_attach, (Il2CppDomain*))
IL2CPP_API(void, il2cpp_runtime_class_init, (Il2CppClass*))
IL2CPP_API(void, il2cpp_gc_wbarrier_set_field, (Il2CppObject*, void**, void*))
IL2CPP_API(const Il2CppImage*, il2cpp_get_corlib, ())

// Il2CppArray: klass, monitor, bounds, max_length, then data.
static const size_t kArrayLength = 0x18;
static const size_t kArrayData = 0x20;

// ---- config / log -----------------------------------------------------------
static char g_dir[MAX_PATH];
static FILE* g_log;

struct Config {
    bool enabled = true;
    float cellSize = 300.0f;     // machines closer than this share one anchor
    int maxAnchors = 64;         // cap on anchor points (performance)
    bool includeBelts = true;    // also anchor belt ends
    bool followRobots = true;    // make every active robot an observer too
    float refreshSeconds = 5.0f; // how often anchors are recomputed
    int toggleKey = VK_F8;
    bool balanceCapsules = true; // send capsules to the catcher that can take them first
    float fillPenalty = 60.0f;   // seconds a completely full catcher store counts as extra wait
} g_cfg;

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

static void LoadConfig() {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\ChunkLoader.ini", g_dir);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = fopen(path, "w");
        if (f) {
            fputs("[ChunkLoader]\n"
                  "; 1 = an, 0 = aus (im Spiel mit F8 umschaltbar)\n"
                  "Enabled=1\n"
                  "; Maschinen naeher als so viele Meter teilen sich einen Anker\n"
                  "CellSize=300\n"
                  "; Maximale Anzahl Anker (mehr = mehr geladen, aber mehr Leistung noetig)\n"
                  "MaxAnchors=64\n"
                  "; Auch Foerderbaender verankern\n"
                  "IncludeBelts=1\n"
                  "; Jeder aktive Roboter haelt seine Umgebung selbst geladen\n"
                  "FollowRobots=1\n"
                  "; Wie oft (Sekunden) die Anker neu berechnet werden\n"
                  "RefreshSeconds=5\n"
                  "; Virtual-Key-Code der Umschalttaste (119 = F8)\n"
                  "ToggleKey=119\n"
                  "; Kapseln auf alle Empfaenger verteilen statt immer zum naechsten\n"
                  "BalanceCapsules=1\n"
                  "; Wie viele Sekunden Wartezeit ein voller Empfaenger-Speicher zaehlt\n"
                  "FillPenaltySeconds=60\n", f);
            fclose(f);
        }
    }
    char buf[64];
    g_cfg.enabled = GetPrivateProfileIntA("ChunkLoader", "Enabled", 1, path) != 0;
    GetPrivateProfileStringA("ChunkLoader", "CellSize", "300", buf, sizeof(buf), path);
    g_cfg.cellSize = std::max(50.0f, (float)atof(buf));
    g_cfg.maxAnchors = std::clamp((int)GetPrivateProfileIntA("ChunkLoader", "MaxAnchors", 64, path), 1, 512);
    g_cfg.includeBelts = GetPrivateProfileIntA("ChunkLoader", "IncludeBelts", 1, path) != 0;
    g_cfg.followRobots = GetPrivateProfileIntA("ChunkLoader", "FollowRobots", 1, path) != 0;
    GetPrivateProfileStringA("ChunkLoader", "RefreshSeconds", "5", buf, sizeof(buf), path);
    g_cfg.refreshSeconds = std::max(0.5f, (float)atof(buf));
    g_cfg.toggleKey = GetPrivateProfileIntA("ChunkLoader", "ToggleKey", VK_F8, path);
    g_cfg.balanceCapsules = GetPrivateProfileIntA("ChunkLoader", "BalanceCapsules", 1, path) != 0;
    GetPrivateProfileStringA("ChunkLoader", "FillPenaltySeconds", "60", buf, sizeof(buf), path);
    g_cfg.fillPenalty = std::max(0.0f, (float)atof(buf));
    Log("Config: Enabled=%d CellSize=%.0f MaxAnchors=%d IncludeBelts=%d FollowRobots=%d RefreshSeconds=%.1f ToggleKey=%d "
        "BalanceCapsules=%d FillPenaltySeconds=%.0f",
        g_cfg.enabled, g_cfg.cellSize, g_cfg.maxAnchors, g_cfg.includeBelts, g_cfg.followRobots,
        g_cfg.refreshSeconds, g_cfg.toggleKey, g_cfg.balanceCapsules, g_cfg.fillPenalty);
}

// Where Refresh got to, for the fault report.
static volatile int g_stage;

static void ModuleOffset(DWORD64 addr, char* out, size_t n) {
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m)
        GetModuleFileNameA(m, name, MAX_PATH);
    const char* base = strrchr(name, '\\');
    snprintf(out, n, "%s+0x%llx", base ? base + 1 : name, (unsigned long long)(addr - (DWORD64)m));
}

static int LogFault(EXCEPTION_POINTERS* ep, const char* where) {
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    char at[MAX_PATH + 32];
    ModuleOffset((DWORD64)r->ExceptionAddress, at, sizeof(at));
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        Log("Fehler in %s: 0x%08X bei %s, Stufe %d, %s 0x%llx.", where, r->ExceptionCode, at, g_stage,
            r->ExceptionInformation[0] ? "schreibt" : "liest", (unsigned long long)r->ExceptionInformation[1]);
    else
        Log("Fehler in %s: 0x%08X bei %s, Stufe %d.", where, r->ExceptionCode, at, g_stage);
    CONTEXT c = *ep->ContextRecord;
    for (int i = 0; i < 12 && c.Rip; i++) {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
        if (!fe) { c.Rip = *(DWORD64*)c.Rsp; c.Rsp += 8; }
        else {
            void* hd = nullptr; DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fe, &c, &hd, &frame, nullptr);
        }
        if (!c.Rip) break;
        ModuleOffset(c.Rip, at, sizeof(at));
        Log("    aus %s", at);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---- resolved game metadata -------------------------------------------------
struct Vec3 { float x, y, z; };

static Il2CppClass* k_MoonWorld;
static Il2CppClass* k_RobotVacuum;
static Il2CppClass* k_GameObject;
static Il2CppClass* k_Component;
static Il2CppClass* k_Transform;
static Il2CppClass* k_UObject;

static FieldInfo* f_observers;
static FieldInfo* f_depots;
static FieldInfo* f_conveyors;
static FieldInfo* f_robotsActive;
static size_t o_depotId, o_depotKind, o_depotX, o_beltStart, o_beltEnd, o_cachedPtr;

static const MethodInfo* m_GoCtor;
static const MethodInfo* m_GetTransform;
static const MethodInfo* m_GetPosition;
static const MethodInfo* m_SetPosition;

template <class T> static T Read(const void* obj, size_t off) { return *(const T*)((const char*)obj + off); }

static Il2CppClass* FindClass(const char* ns, const char* name) {
    size_t n = 0;
    const Il2CppAssembly** asms = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &n);
    for (size_t i = 0; i < n; i++) {
        Il2CppClass* k = il2cpp_class_from_name(il2cpp_assembly_get_image(asms[i]), ns, name);
        if (k) return k;
    }
    return nullptr;
}

static size_t FieldOffset(Il2CppClass* k, const char* name, size_t fallback) {
    FieldInfo* f = k ? il2cpp_class_get_field_from_name(k, name) : nullptr;
    return f ? il2cpp_field_get_offset(f) : fallback;
}

static bool Alive(Il2CppObject* unityObj) {
    return unityObj && Read<void*>(unityObj, o_cachedPtr) != nullptr;
}

static Il2CppObject* Invoke(const MethodInfo* m, void* obj, void** args) {
    Il2CppObject* exc = nullptr;
    Il2CppObject* r = il2cpp_runtime_invoke(m, obj, args, &exc);
    return exc ? nullptr : r;
}

static Il2CppObject* TransformOf(Il2CppObject* component) {
    return Alive(component) ? Invoke(m_GetTransform, component, nullptr) : nullptr;
}

static bool PositionOf(Il2CppObject* transform, Vec3* out) {
    Il2CppObject* boxed = Alive(transform) ? Invoke(m_GetPosition, transform, nullptr) : nullptr;
    if (!boxed) return false;
    *out = *(Vec3*)il2cpp_object_unbox(boxed);
    return true;
}

// Calls fn(value) for every value of a Dictionary<int, RefType>.
template <class F> static void ForEachDictValue(Il2CppObject* dict, F fn) {
    if (!dict) return;
    Il2CppClass* dk = il2cpp_object_get_class(dict);
    Il2CppObject* entries = Read<Il2CppObject*>(dict, FieldOffset(dk, "_entries", 0x18));
    int count = Read<int>(dict, FieldOffset(dk, "_count", 0x20));
    if (!entries) return;
    int len = (int)Read<uintptr_t>(entries, kArrayLength);
    count = std::min(count, len);
    // Entry<int, ref>: hashCode, next, key (ints), value (pointer) = 24 bytes.
    const char* data = (const char*)entries + kArrayData;
    for (int i = 0; i < count; i++) {
        const char* e = data + (size_t)i * 24;
        if (Read<int>(e, 4) < -1) continue;  // free slot
        Il2CppObject* v = Read<Il2CppObject*>(e, 16);
        if (v) fn(v);
    }
}

static bool ResolveMetadata() {
    k_MoonWorld = FindClass("MoonCleaners", "MoonWorld");
    k_RobotVacuum = FindClass("MoonCleaners", "MoonRobotVacuum");
    k_GameObject = FindClass("UnityEngine", "GameObject");
    k_Component = FindClass("UnityEngine", "Component");
    k_Transform = FindClass("UnityEngine", "Transform");
    k_UObject = FindClass("UnityEngine", "Object");
    Il2CppClass* depot = FindClass("MoonCleaners", "MoonDepotSnapshot");
    Il2CppClass* belt = FindClass("MoonCleaners", "MoonConveyorSnapshot");
    if (!k_MoonWorld || !k_GameObject || !k_Component || !k_Transform || !k_UObject || !depot) {
        Log("FEHLER: Spielklassen nicht gefunden (MoonWorld=%p GameObject=%p).", k_MoonWorld, k_GameObject);
        return false;
    }
    f_observers = il2cpp_class_get_field_from_name(k_MoonWorld, "observers");
    f_depots = il2cpp_class_get_field_from_name(k_MoonWorld, "depots");
    f_conveyors = il2cpp_class_get_field_from_name(k_MoonWorld, "conveyors");
    f_robotsActive = k_RobotVacuum ? il2cpp_class_get_field_from_name(k_RobotVacuum, "active") : nullptr;
    o_depotId = FieldOffset(depot, "id", 0x10);
    o_depotKind = FieldOffset(depot, "kind", 0x14);
    o_depotX = FieldOffset(depot, "x", 0x20);
    o_beltStart = FieldOffset(belt, "start", 0x20);
    o_beltEnd = FieldOffset(belt, "end", 0x38);
    o_cachedPtr = FieldOffset(k_UObject, "m_CachedPtr", 0x10);

    m_GoCtor = il2cpp_class_get_method_from_name(k_GameObject, ".ctor", 1);
    m_GetTransform = il2cpp_class_get_method_from_name(k_Component, "get_transform", 0);
    m_GetPosition = il2cpp_class_get_method_from_name(k_Transform, "get_position", 0);
    m_SetPosition = il2cpp_class_get_method_from_name(k_Transform, "set_position", 1);
    if (!f_observers || !f_depots || !m_GoCtor || !m_GetTransform || !m_GetPosition || !m_SetPosition) {
        Log("FEHLER: Felder/Methoden fehlen (observers=%p depots=%p ctor=%p).", f_observers, f_depots, m_GoCtor);
        return false;
    }
    Log("Metadaten ok. observers@0x%zx depots@0x%zx conveyors=%p robots=%p",
        il2cpp_field_get_offset(f_observers), il2cpp_field_get_offset(f_depots), f_conveyors, f_robotsActive);
    return true;
}

// ---- anchor logic (runs on the main thread inside MoonWorld.Update) ----------
static Il2CppObject* g_world;            // world the anchors belong to
static std::vector<GCHandle> g_anchors;  // gc handles of anchor Transforms
static std::vector<GCHandle> g_mine;     // gc handles of everything we added last time
static double g_nextRefresh;
static bool g_keyWasDown;

static Il2CppObject* NewAnchor(int index) {
    char name[48];
    snprintf(name, sizeof(name), "ChunkLoaderAnchor_%d", index);
    Il2CppObject* go = il2cpp_object_new(k_GameObject);
    void* args[1] = { il2cpp_string_new(name) };
    Il2CppObject* exc = nullptr;
    il2cpp_runtime_invoke(m_GoCtor, go, args, &exc);
    if (exc) return nullptr;
    Il2CppObject* tr = Invoke(il2cpp_class_get_method_from_name(k_GameObject, "get_transform", 0), go, nullptr);
    return tr;
}

static void SetObservers(Il2CppObject* world, const std::vector<Il2CppObject*>& extra) {
    g_stage = 80;
    Il2CppObject* current = Read<Il2CppObject*>(world, il2cpp_field_get_offset(f_observers));

    // Keep the game's own observers (player, vehicles, crew) in their order,
    // drop whatever we added last time, append the new extras.
    std::vector<Il2CppObject*> mine;
    for (GCHandle h : g_mine) mine.push_back(il2cpp_gchandle_get_target(h));
    std::vector<Il2CppObject*> list;
    if (current) {
        int n = (int)Read<uintptr_t>(current, kArrayLength);
        for (int i = 0; i < n; i++) {
            Il2CppObject* t = Read<Il2CppObject*>(current, kArrayData + i * sizeof(void*));
            if (t && std::find(mine.begin(), mine.end(), t) == mine.end()) list.push_back(t);
        }
    }
    g_stage = 81;
    size_t gameCount = list.size();
    for (Il2CppObject* t : extra)
        if (std::find(list.begin(), list.end(), t) == list.end()) list.push_back(t);

    // Nothing changed: leave the array alone.
    if (current && (size_t)Read<uintptr_t>(current, kArrayLength) == list.size()) {
        bool same = true;
        for (size_t i = 0; same && i < list.size(); i++)
            same = Read<Il2CppObject*>(current, kArrayData + i * sizeof(void*)) == list[i];
        if (same) return;
    }

    g_stage = 82;
    // Same array class the game uses for observers.
    Il2CppObject* arr = current ? il2cpp_array_new_specific(il2cpp_object_get_class(current), list.size())
                                : il2cpp_array_new(k_Transform, list.size());
    // Keep the array alive ourselves too, independent of the GC seeing the field store.
    static GCHandle arrHandle;
    GCHandle newHandle = il2cpp_gchandle_new(arr, false);
    for (size_t i = 0; i < list.size(); i++) {
        void** slot = (void**)((char*)arr + kArrayData + i * sizeof(void*));
        il2cpp_gc_wbarrier_set_field(arr, slot, list[i]);
    }
    // The game runs the incremental GC: stores into managed objects must go
    // through the write barrier, or a freshly allocated array can be collected
    // while the world still points at it.
    il2cpp_gc_wbarrier_set_field(world, (void**)((char*)world + il2cpp_field_get_offset(f_observers)), arr);
    if (arrHandle) il2cpp_gchandle_free(arrHandle);
    arrHandle = newHandle;

    g_stage = 83;
    for (GCHandle h : g_mine) il2cpp_gchandle_free(h);
    g_mine.clear();
    for (size_t i = gameCount; i < list.size(); i++) g_mine.push_back(il2cpp_gchandle_new(list[i], false));
}

struct Cell { double sx = 0, sy = 0, sz = 0; int count = 0; bool important = false; };

static void Refresh(Il2CppObject* world) {
    g_stage = 10;
    if (world != g_world) {  // new save / scene: start over
        g_world = world;
        g_anchors.clear();
        g_mine.clear();
        Log("Neue Spielwelt erkannt (%p).", world);
    }
    if (!g_cfg.enabled) {
        if (!g_mine.empty()) { SetObservers(world, {}); Log("Deaktiviert: Anker entfernt."); }
        return;
    }

    g_stage = 20;
    Vec3 origin{ 0, 0, 0 };
    PositionOf(TransformOf(world), &origin);

    // Cluster machines (and belt ends) into cells.
    std::unordered_map<int64_t, Cell> cells;
    auto add = [&](float x, float y, float z, bool important) {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return;
        int64_t cx = (int64_t)floorf(x / g_cfg.cellSize), cy = (int64_t)floorf(y / g_cfg.cellSize),
                cz = (int64_t)floorf(z / g_cfg.cellSize);
        int64_t key = ((cx & 0x1FFFFF) << 42) | ((cy & 0x1FFFFF) << 21) | (cz & 0x1FFFFF);
        Cell& c = cells[key];
        c.sx += x; c.sy += y; c.sz += z; c.count++;
        c.important |= important;
    };
    g_stage = 30;
    int depotCount = 0, beltCount = 0;
    ForEachDictValue(Read<Il2CppObject*>(world, il2cpp_field_get_offset(f_depots)), [&](Il2CppObject* d) {
        int kind = Read<int>(d, o_depotKind);
        Vec3 p = Read<Vec3>(d, o_depotX);
        // 91/92 robot docks, 99 Joonas, 100 capsule catcher, 10 packager, 93-97 power/processing
        bool important = kind == 91 || kind == 92 || kind == 99 || kind == 100 || kind == 10 || (kind >= 93 && kind <= 97);
        add(origin.x + p.x, origin.y + p.y, origin.z + p.z, important);
        depotCount++;
    });
    g_stage = 40;
    if (g_cfg.includeBelts && f_conveyors) {
        ForEachDictValue(Read<Il2CppObject*>(world, il2cpp_field_get_offset(f_conveyors)), [&](Il2CppObject* b) {
            Vec3 s = Read<Vec3>(b, o_beltStart), e = Read<Vec3>(b, o_beltEnd);
            add(origin.x + s.x, origin.y + s.y, origin.z + s.z, true);
            add(origin.x + e.x, origin.y + e.y, origin.z + e.z, true);
            beltCount++;
        });
    }

    g_stage = 70;
    int robots = 0;
    if (g_cfg.followRobots && f_robotsActive) {
        il2cpp_runtime_class_init(k_RobotVacuum);
        Il2CppObject* list = nullptr;
        il2cpp_field_static_get_value(f_robotsActive, &list);
        if (list) {
            Il2CppClass* lk = il2cpp_object_get_class(list);
            Il2CppObject* items = Read<Il2CppObject*>(list, FieldOffset(lk, "_items", 0x10));
            int size = Read<int>(list, FieldOffset(lk, "_size", 0x18));
            for (int i = 0; items && i < size; i++) {
                g_stage = 7000 + i;
                Il2CppObject* r = Read<Il2CppObject*>(items, kArrayData + i * sizeof(void*));
                Il2CppObject* tr = TransformOf(r);
                // Anchor at the robot's position, never the robot itself: every
                // observer within 400 m becomes an obstacle in GatherMovers, so a
                // robot listed as observer keeps dodging its own collider.
                Vec3 rp;
                if (PositionOf(tr, &rp)) { add(rp.x, rp.y, rp.z, true); robots++; }
            }
        }
    }

    g_stage = 50;
    std::vector<Cell> sorted;
    for (auto& kv : cells) sorted.push_back(kv.second);
    std::sort(sorted.begin(), sorted.end(), [](const Cell& a, const Cell& b) {
        if (a.important != b.important) return a.important;
        return a.count > b.count;
    });
    if ((int)sorted.size() > g_cfg.maxAnchors) sorted.resize(g_cfg.maxAnchors);

    // Make sure we have enough live anchor objects, then move them into place.
    std::vector<Il2CppObject*> extra;
    for (size_t i = 0; i < sorted.size(); i++) {
        g_stage = 600 + (int)i;
        Il2CppObject* tr = i < g_anchors.size() ? il2cpp_gchandle_get_target(g_anchors[i]) : nullptr;
        if (!Alive(tr)) {
            tr = NewAnchor((int)i);
            if (!tr) break;
            GCHandle h = il2cpp_gchandle_new(tr, false);
            if (i < g_anchors.size()) g_anchors[i] = h; else g_anchors.push_back(h);
        }
        const Cell& c = sorted[i];
        Vec3 p{ (float)(c.sx / c.count), (float)(c.sy / c.count), (float)(c.sz / c.count) };
        void* args[1] = { &p };
        Invoke(m_SetPosition, tr, args);
        extra.push_back(tr);
    }

    SetObservers(world, extra);
    g_stage = 90;
    static int lastLogged = -1;
    int sig = (int)sorted.size() * 1000 + robots;
    if (sig != lastLogged) {
        lastLogged = sig;
        Log("%d Anker aktiv (aus %d Gebaeuden, %d Baendern) + %d Roboter.", (int)sorted.size(), depotCount, beltCount, robots);
    }
}

// ---- hook -------------------------------------------------------------------
typedef void (*Update_t)(Il2CppObject*, const MethodInfo*);
static Update_t g_origUpdate;

static void HookedUpdate(Il2CppObject* self, const MethodInfo* mi) {
    bool down = (GetAsyncKeyState(g_cfg.toggleKey) & 0x8000) != 0;
    DWORD fgPid = 0;
    if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &fgPid);
    if (down && !g_keyWasDown && fgPid == GetCurrentProcessId()) {
        g_cfg.enabled = !g_cfg.enabled;
        g_nextRefresh = 0;
        Log("Per Taste %s.", g_cfg.enabled ? "AKTIVIERT" : "DEAKTIVIERT");
    }
    g_keyWasDown = down;

    double now = GetTickCount64() / 1000.0;
    if (self && now >= g_nextRefresh) {
        g_nextRefresh = now + g_cfg.refreshSeconds;
        __try { Refresh(self); }
        __except (LogFault(GetExceptionInformation(), "Refresh")) { Log("Mod pausiert 60 s."); g_nextRefresh = now + 60; }
    }
    g_origUpdate(self, mi);
}

// ---- capsule catcher balancing ----------------------------------------------
// MoonWorld.FindCatcher(from, out id, out mouth, out up) picks the nearest catcher
// that is not full. The launcher then books the arrival with
// ReserveCatch(id, now + flight), which returns max(that, CatchFreeAt(id)) and
// pushes the catcher's next free slot 9 s further. Among the catchers the game
// would accept, we pick the one that would catch the capsule first, plus a
// penalty for a filling store.
static const int kCatcherKind = 100;        // MoonWorld.CapsuleCatcherKind
static const double kCatcherStoreKg = 100;  // MoonWorld.CatcherStoreKg

typedef bool (*FindCatcher_t)(Il2CppObject*, Vec3*, int*, Vec3*, Vec3*, const MethodInfo*);
static FindCatcher_t g_origFindCatcher;
static bool InstallHook(void* target, void* hook, void** original);
struct GameFn { const MethodInfo* mi = nullptr; void* code = nullptr; };
static GameFn fn_CatcherFull, fn_CatcherMouth, fn_CatcherKg, fn_CatchFreeAt, fn_FlightSeconds, fn_Time;

static bool ResolveFn(GameFn& f, Il2CppClass* k, const char* name, int args) {
    f.mi = k ? il2cpp_class_get_method_from_name(k, name, args) : nullptr;
    f.code = f.mi ? *(void**)f.mi : nullptr;
    if (!f.code) Log("FEHLER: Methode %s fehlt.", name);
    return f.code != nullptr;
}

static bool PickCatcher(Il2CppObject* world, const Vec3* from, int* id, Vec3* mouth, Vec3* up) {
    float now = ((float (*)(const MethodInfo*))fn_Time.code)(fn_Time.mi);
    float best = INFINITY, nearestDist = INFINITY;
    int bestId = 0, nearestId = 0;
    Vec3 bestMouth{}, bestUp{};
    ForEachDictValue(Read<Il2CppObject*>(world, il2cpp_field_get_offset(f_depots)), [&](Il2CppObject* d) {
        if (Read<int>(d, o_depotKind) != kCatcherKind) return;
        if (((bool (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*))fn_CatcherFull.code)(world, d, fn_CatcherFull.mi)) return;
        int cid = Read<int>(d, o_depotId);
        Vec3 m{}, u{};
        if (!((bool (*)(Il2CppObject*, int, Vec3*, Vec3*, const MethodInfo*))fn_CatcherMouth.code)(world, cid, &m, &u, fn_CatcherMouth.mi)) return;
        Vec3 a = *from, b = m;  // by-value Vector3 args are passed as pointers to copies
        float flight = ((float (*)(Vec3*, Vec3*, const MethodInfo*))fn_FlightSeconds.code)(&a, &b, fn_FlightSeconds.mi);
        float freeAt = ((float (*)(Il2CppObject*, int, const MethodInfo*))fn_CatchFreeAt.code)(world, cid, fn_CatchFreeAt.mi);
        double kg = ((double (*)(Il2CppObject*, int, const MethodInfo*))fn_CatcherKg.code)(world, cid, fn_CatcherKg.mi);
        float t = std::max(now + flight, freeAt) + (float)(std::clamp(kg / kCatcherStoreKg, 0.0, 1.0) * g_cfg.fillPenalty);
        float dx = m.x - from->x, dy = m.y - from->y, dz = m.z - from->z;
        float dist = dx * dx + dy * dy + dz * dz;
        if (dist < nearestDist) { nearestDist = dist; nearestId = cid; }
        if (t < best || (t == best && cid < bestId)) { best = t; bestId = cid; bestMouth = m; bestUp = u; }
    });
    if (bestId <= 0) return false;
    *id = bestId; *mouth = bestMouth; *up = bestUp;
    static int lastLogged, logged;
    if (bestId != lastLogged && logged < 200) {
        lastLogged = bestId; logged++;
        Log("Kapsel-Ziel: Empfaenger %d (naechster waere %d), Ankunft in %.1f s.", bestId, nearestId, best - now);
    }
    return true;
}

static bool HookedFindCatcher(Il2CppObject* self, Vec3* from, int* id, Vec3* mouth, Vec3* up, const MethodInfo* mi) {
    static bool failed;
    if (g_cfg.balanceCapsules && !failed && self && from && id && mouth && up) {
        __try { if (PickCatcher(self, from, id, mouth, up)) return true; }
        __except (LogFault(GetExceptionInformation(), "Kapsel-Verteilung")) { failed = true; Log("Kapsel-Verteilung aus, nutze Spiel-Logik."); }
    }
    return g_origFindCatcher(self, from, id, mouth, up, mi);
}

static void InstallCatcherHook() {
    Il2CppClass* capsule = FindClass("MoonCleaners", "MoonLaunchCapsule");
    Il2CppClass* time = FindClass("UnityEngine", "Time");
    const MethodInfo* find = il2cpp_class_get_method_from_name(k_MoonWorld, "FindCatcher", 4);
    void* code = find ? *(void**)find : nullptr;
    if (!code || !ResolveFn(fn_CatcherFull, k_MoonWorld, "CatcherFull", 1) || !ResolveFn(fn_CatcherMouth, k_MoonWorld, "CatcherMouth", 3) ||
        !ResolveFn(fn_CatcherKg, k_MoonWorld, "CatcherKilograms", 1) || !ResolveFn(fn_CatchFreeAt, k_MoonWorld, "CatchFreeAt", 1) ||
        !ResolveFn(fn_FlightSeconds, capsule, "FlightSeconds", 2) || !ResolveFn(fn_Time, time, "get_time", 0)) {
        Log("Kapsel-Verteilung nicht verfuegbar (FindCatcher=%p).", code);
        return;
    }
    if (!InstallHook(code, (void*)&HookedFindCatcher, (void**)&g_origFindCatcher)) { Log("Kapsel-Verteilung: Hook fehlgeschlagen."); return; }
    Log("Hook auf MoonWorld.FindCatcher installiert (%p): Kapseln werden verteilt.", code);
}

// Length of one x64 prologue instruction we know how to relocate, or 0.
static int InsnLen(const uint8_t* p) {
    int rex = 0;
    if ((p[0] & 0xF0) == 0x40) rex = 1;
    uint8_t op = p[rex];
    if (op >= 0x50 && op <= 0x57) return rex + 1;  // push r
    if (rex && (op == 0x89 || op == 0x8B)) {      // mov r/m, r
        uint8_t modrm = p[rex + 1];
        int mod = modrm >> 6, rm = modrm & 7;
        if (mod == 3) return rex + 2;
        if (mod == 0 && rm == 5) return 0;          // rip-relative: refuse
        int len = rex + 2 + (rm == 4 ? 1 : 0);
        if (mod == 1) len += 1; else if (mod == 2) len += 4;
        return len;
    }
    if (rex && op == 0x83 && p[rex + 1] == 0xEC) return rex + 3;  // sub rsp, imm8
    if (rex && op == 0x81 && p[rex + 1] == 0xEC) return rex + 6;  // sub rsp, imm32
    return 0;
}

static bool InstallHook(void* target, void* hook, void** original) {
    uint8_t* t = (uint8_t*)target;
    int len = 0;
    while (len < 14) {
        int l = InsnLen(t + len);
        if (!l) { Log("FEHLER: unbekannter Funktionsanfang (%02X %02X %02X) bei +%d.", t[len], t[len + 1], t[len + 2], len); return false; }
        len += l;
    }
    uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    memcpy(tramp, t, len);
    uint8_t jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    *(uint64_t*)(jmp + 6) = (uint64_t)(t + len);
    memcpy(tramp + len, jmp, 14);

    DWORD old;
    if (!VirtualProtect(t, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(uint64_t*)(jmp + 6) = (uint64_t)hook;
    memcpy(t, jmp, 14);
    for (int i = 14; i < len; i++) t[i] = 0x90;
    VirtualProtect(t, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, len);
    *original = tramp;
    return true;
}

// ---- startup ----------------------------------------------------------------
static bool ResolveApi(HMODULE ga) {
#define RESOLVE(name) name = (name##_t)GetProcAddress(ga, #name); if (!name) { Log("FEHLER: Export %s fehlt.", #name); return false; }
    RESOLVE(il2cpp_domain_get) RESOLVE(il2cpp_domain_get_assemblies) RESOLVE(il2cpp_assembly_get_image)
    RESOLVE(il2cpp_class_from_name) RESOLVE(il2cpp_class_get_field_from_name) RESOLVE(il2cpp_field_get_offset)
    RESOLVE(il2cpp_field_static_get_value) RESOLVE(il2cpp_field_set_value) RESOLVE(il2cpp_class_get_method_from_name)
    RESOLVE(il2cpp_runtime_invoke) RESOLVE(il2cpp_object_new) RESOLVE(il2cpp_string_new) RESOLVE(il2cpp_array_new) RESOLVE(il2cpp_array_new_specific)
    RESOLVE(il2cpp_object_get_class) RESOLVE(il2cpp_object_unbox) RESOLVE(il2cpp_gchandle_new)
    RESOLVE(il2cpp_gchandle_get_target) RESOLVE(il2cpp_gchandle_free) RESOLVE(il2cpp_thread_attach) RESOLVE(il2cpp_runtime_class_init)
    RESOLVE(il2cpp_gc_wbarrier_set_field) RESOLVE(il2cpp_get_corlib)
#undef RESOLVE
    return true;
}

static DWORD WINAPI Worker(LPVOID) {
    HMODULE ga = nullptr;
    for (int i = 0; i < 1200 && !(ga = GetModuleHandleA("GameAssembly.dll")); i++) Sleep(50);
    if (!ga) { Log("FEHLER: GameAssembly.dll wurde nicht geladen."); return 0; }
    if (!ResolveApi(ga)) return 0;

    // Wait for il2cpp to finish initialising (corlib is set during il2cpp_init).
    for (int i = 0; i < 1200 && !il2cpp_get_corlib(); i++) Sleep(50);
    if (!il2cpp_get_corlib()) { Log("FEHLER: il2cpp startet nicht."); return 0; }
    Sleep(1500);
    il2cpp_thread_attach(il2cpp_domain_get());

    if (!ResolveMetadata()) return 0;
    const MethodInfo* update = il2cpp_class_get_method_from_name(k_MoonWorld, "Update", 0);
    void* code = update ? *(void**)update : nullptr;  // MethodInfo::methodPointer
    if (!code) { Log("FEHLER: MoonWorld.Update nicht gefunden."); return 0; }
    if (!InstallHook(code, (void*)&HookedUpdate, (void**)&g_origUpdate)) { Log("FEHLER: Hook fehlgeschlagen, Mod inaktiv."); return 0; }
    Log("Hook auf MoonWorld.Update installiert (%p). Bereit.", code);
    InstallCatcherHook();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        GetModuleFileNameA(nullptr, g_dir, MAX_PATH);
        // Only the game itself; UnityCrashHandler64.exe also loads version.dll.
        const char* exe = strrchr(g_dir, '\\');
        if (!exe || _stricmp(exe + 1, "MoonSweepers.exe") != 0) return TRUE;
        if (char* s = strrchr(g_dir, '\\')) *s = 0;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\ChunkLoader.log", g_dir);
        g_log = fopen(path, "w");
        Log("Moon Sweepers ChunkLoader 1.2.2 geladen.");
        LoadConfig();
        CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }
    return TRUE;
}

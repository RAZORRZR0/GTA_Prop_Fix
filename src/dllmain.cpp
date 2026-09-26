#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <share.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../minhook/MinHook.h"

// =======================================================================
// GTA SA Definitive Edition - Prop Physics Fix v0.3.2
//
// Restores the original 2004 RenderWare prop behaviour (reference:
// gta-reversed CObject / CPhysical / BreakObject_c).
//
// 1. Dislodged props (lamp posts, traffic lights, signs, bins, hydrants,
//    fences that uproot, ...): DE's CObject::SetIsStatic(false) keeps the
//    original RenderWare CPhysical simulation running, but also fires the
//    Blueprint event `Dislodged`, which hands the actor to UE PhysX (which has
//    no world collision). The plugin suppresses that hand-off, flags the actor
//    with bEntityUpdatePosition and copies the RenderWare matrix to the actor
//    after every collision step, so the prop moves exactly like the original.
//    Street lights switch off when tilted, like CEntity::ProcessLightsForEntity.
// 2. Breakable props (colDamageEffect BREAKABLE / glass): the original
//    BreakObject_c code is stubbed in DE ("Unimplemented BREAKABLES") and the
//    pieces are UE PhysX bodies. The plugin turns the Blueprint's tiny floor
//    into the original infinite ground plane (DE passes the original ground
//    probe as floorTransform; glass gets the same probe here), lets only the
//    fragments collide with it, and hides the fragments when the Blueprint
//    would drop them through the map (original fragments fade after ~5 s).
// 3. Correct PhysX substepping offsets (UPhysicsSettings 0x12C/0x130/0x134).
//
// Settings: GTA_Prop_Fix.ini next to the .asi (see README).
// =======================================================================

// ----------------------------------------------------------------------
// Configuration
// ----------------------------------------------------------------------
enum DebrisMode { DEBRIS_ORIGINAL = 0, DEBRIS_KEEP = 1, DEBRIS_STOCK = 2 };

static struct Config {
    bool  rwDislodgedProps = true;
    bool  lightsOffWhenTilted = true;
    bool  uprootStreetLights = true;    // true = fall over whole (RW physics); false = shatter into DE pieces
    float piecesDisappearSeconds = 6.0f; // broken pieces are removed after this (0 = never)
    float fallenPolesDisappearSeconds = 6.0f; // street lights knocked over whole (StreetLightsFallWhole=1)
    float knockedPropsDisappearSeconds = 0.0f; // any other prop knocked loose (0 = stays, original)
    bool  fallenPolesBlockCars = true;  // original: cars ignore fallen lamp posts
    bool  keepPushable = true;          // settled knocked props stay pushable (not re-rooted)
    float fragmentAngularDamping = 1.5f;
    float minColCoverage = 0.6f; // RenderWare col height / visual height needed to trust RW physics
    float minShapeCoverage = 0.7f; // solid col shapes must fill this much of the col box height
    bool  addSpheres = true;       // generate spheres for triangle-mesh props so RW can land them in one piece
    bool  fixDebrisFloor = true;
    int   debrisMode = DEBRIS_ORIGINAL;
    float debrisPlaneSizeM = 30.0f;
    bool  debrisCCD = true;
    bool  originalPieceMotion = true; // original BreakObject_c motion for broken pieces
    float piecesFadeSeconds = 2.55f;  // original: alpha = FramesToLive*2 -> fades over the last 127.5 steps
    bool  substepping = true;
    int   logLevel = 1; // 0 off, 1 prop events (throttled) + stats, 2 verbose
} g_cfg;

static char g_dir[MAX_PATH];
static char g_iniPath[MAX_PATH];
static bool g_iniFound = false;

static void LoadConfig(HMODULE self) {
    GetModuleFileNameA(self, g_dir, MAX_PATH);
    char* slash = strrchr(g_dir, '\\');
    if (slash) slash[1] = '\0';
    const char* ini = g_iniPath;
    snprintf(g_iniPath, sizeof(g_iniPath), "%sGTA_Prop_Fix.ini", g_dir);
    g_iniFound = GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES;
    g_cfg.rwDislodgedProps    = GetPrivateProfileIntA("Props", "RenderWarePhysicsForDislodgedProps", 1, ini) != 0;
    g_cfg.lightsOffWhenTilted = GetPrivateProfileIntA("Props", "StreetLightsOffWhenTilted", 1, ini) != 0;
    g_cfg.uprootStreetLights  = GetPrivateProfileIntA("Props", "StreetLightsFallWhole", 1, ini) != 0;
    {
        char buf[32];
        GetPrivateProfileStringA("Debris", "PiecesDisappearSeconds", "6", buf, sizeof(buf), ini);
        g_cfg.piecesDisappearSeconds = (float)atof(buf);
        GetPrivateProfileStringA("Props", "FallenPolesDisappearSeconds", "6", buf, sizeof(buf), ini);
        g_cfg.fallenPolesDisappearSeconds = fmaxf(0.0f, (float)atof(buf));
        GetPrivateProfileStringA("Props", "KnockedPropsDisappearSeconds", "0", buf, sizeof(buf), ini);
        g_cfg.knockedPropsDisappearSeconds = fmaxf(0.0f, (float)atof(buf));
        if (g_cfg.piecesDisappearSeconds < 0.0f) g_cfg.piecesDisappearSeconds = 0.0f;
    }
    g_cfg.fallenPolesBlockCars = GetPrivateProfileIntA("Props", "FallenPolesBlockCars", 1, ini) != 0;
    g_cfg.keepPushable        = GetPrivateProfileIntA("Props", "KnockedPropsStayPushable", 1, ini) != 0;
    {
        char buf[32];
        GetPrivateProfileStringA("Debris", "FragmentAngularDamping", "1.5", buf, sizeof(buf), ini);
        g_cfg.fragmentAngularDamping = (float)atof(buf);
    }
    g_cfg.minColCoverage      = GetPrivateProfileIntA("Props", "MinCollisionCoveragePercent", 60, ini) / 100.0f;
    g_cfg.addSpheres          = GetPrivateProfileIntA("Props", "GenerateCollisionSpheres", 1, ini) != 0;
    g_cfg.minShapeCoverage    = GetPrivateProfileIntA("Props", "MinShapeCoveragePercent", 70, ini) / 100.0f;
    g_cfg.fixDebrisFloor      = GetPrivateProfileIntA("Debris", "OriginalGroundPlane", 1, ini) != 0;
    g_cfg.debrisMode          = GetPrivateProfileIntA("Debris", "Lifetime", DEBRIS_ORIGINAL, ini);
    g_cfg.debrisPlaneSizeM    = (float)GetPrivateProfileIntA("Debris", "GroundPlaneSizeMeters", 30, ini);
    g_cfg.originalPieceMotion = GetPrivateProfileIntA("Debris", "OriginalPieceMotion", 1, ini) != 0;
    {
        char buf[32];
        GetPrivateProfileStringA("Debris", "PiecesFadeSeconds", "2.55", buf, sizeof(buf), ini);
        g_cfg.piecesFadeSeconds = fmaxf(0.0f, (float)atof(buf));
    }
    g_cfg.debrisCCD           = GetPrivateProfileIntA("Debris", "ContinuousCollision", 1, ini) != 0;
    g_cfg.substepping         = GetPrivateProfileIntA("PhysX", "Substepping", 1, ini) != 0;
    g_cfg.logLevel            = GetPrivateProfileIntA("Debug", "Log", 1, ini);
    if (g_cfg.debrisMode < DEBRIS_ORIGINAL || g_cfg.debrisMode > DEBRIS_STOCK) g_cfg.debrisMode = DEBRIS_ORIGINAL;
    if (g_cfg.debrisPlaneSizeM < 1.0f) g_cfg.debrisPlaneSizeM = 30.0f;
}

// ---- Logging: one file per session; next to the .asi, else %LOCALAPPDATA%, else %TEMP%.
static SRWLOCK g_logLock = SRWLOCK_INIT;
static FILE* g_logFile = nullptr;
static char g_logPath[MAX_PATH];
static DWORD g_logStart = 0;

static void OpenLog() {
    if (g_cfg.logLevel <= 0) return;
    char candidates[3][MAX_PATH] = {};
    snprintf(candidates[0], MAX_PATH, "%sGTA_Prop_Fix.log", g_dir);
    char env[MAX_PATH];
    if (GetEnvironmentVariableA("LOCALAPPDATA", env, MAX_PATH)) {
        char dir[MAX_PATH];
        snprintf(dir, MAX_PATH, "%s\\GTA_Prop_Fix", env);
        CreateDirectoryA(dir, nullptr);
        snprintf(candidates[1], MAX_PATH, "%s\\GTA_Prop_Fix.log", dir);
    }
    if (GetTempPathA(MAX_PATH, env)) snprintf(candidates[2], MAX_PATH, "%sGTA_Prop_Fix.log", env);
    for (auto& c : candidates) {
        if (!c[0]) continue;
        g_logFile = _fsopen(c, "w", _SH_DENYWR);
        if (g_logFile) { strcpy_s(g_logPath, c); break; }
    }
    g_logStart = GetTickCount();
    char msg[MAX_PATH + 64];
    snprintf(msg, sizeof(msg), "[GTA_Prop_Fix] log: %s\n", g_logFile ? g_logPath : "(no writable location)");
    OutputDebugStringA(msg);
}

static void Log(int level, const char* fmt, ...) {
    if (level > g_cfg.logLevel || !g_logFile) return;
    char line[1024];
    const DWORD ms = GetTickCount() - g_logStart;
    int n = snprintf(line, sizeof(line), "[%5lu.%03lu] ", ms / 1000, ms % 1000);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);
    AcquireSRWLockExclusive(&g_logLock);
    fputs(line, g_logFile);
    fputc('\n', g_logFile);
    fflush(g_logFile);
    ReleaseSRWLockExclusive(&g_logLock);
}

// Same (key, kind) event is logged at most once per `ms`; suppressed repeats are counted.
static struct { const void* key; int kind; DWORD tick; } g_throttle[256];
static volatile LONG g_suppressed = 0;
static bool ShouldLog(const void* key, int kind, DWORD ms = 3000) {
    if (g_cfg.logLevel <= 0 || !g_logFile) return false;
    const DWORD now = GetTickCount();
    const size_t slot = (((uintptr_t)key >> 4) ^ (uintptr_t)kind * 2654435761u) % 256;
    auto& t = g_throttle[slot];
    if (t.key == key && t.kind == kind && now - t.tick < ms) { InterlockedIncrement(&g_suppressed); return false; }
    t = { key, kind, now };
    return true;
}

// Event counters, written to the log as a periodic summary.
static struct Stats {
    volatile LONG dislodged, dislodgeStock, rested, uprootDamage, uprootGlass, breakKept, glassBreak,
        setupBroken, floors, floorsMissing, removeHide, removeKeep, removeStock, lightsOff, syncs, removedProps;
} g_stats;
#define STAT(x) InterlockedIncrement(&g_stats.x)

// ----------------------------------------------------------------------
// Signature scanning (SanAndreas.exe, Steam/RGL build with UE 4.26 Gameface)
// ----------------------------------------------------------------------
static uint8_t* g_textBegin = nullptr;
static size_t   g_textSize = 0;

static uint8_t* g_moduleBase = nullptr; // tests may point this at a manually mapped SanAndreas.exe

static bool InitTextSection() {
    uint8_t* base = g_moduleBase ? g_moduleBase : (uint8_t*)GetModuleHandleA(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 5) == 0) {
            g_textBegin = base + sec->VirtualAddress;
            g_textSize = sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

// Returns the unique match of `pattern` ("48 8D ?? ..."), or nullptr if absent/ambiguous.
static uint8_t* FindUnique(const char* pattern) {
    uint8_t bytes[128];
    bool mask[128];
    size_t n = 0;
    for (const char* p = pattern; *p && n < sizeof(bytes);) {
        if (*p == ' ') { ++p; continue; }
        if (p[0] == '?') { mask[n] = false; bytes[n++] = 0; p += (p[1] == '?') ? 2 : 1; continue; }
        char hex[3] = { p[0], p[1], 0 };
        bytes[n] = (uint8_t)strtoul(hex, nullptr, 16);
        mask[n++] = true;
        p += 2;
    }
    if (!n || !g_textBegin) return nullptr;
    uint8_t* found = nullptr;
    uint8_t* end = g_textBegin + g_textSize - n;
    for (uint8_t* cur = g_textBegin; cur <= end; ++cur) {
        if (cur[0] != bytes[0]) continue;
        size_t k = 1;
        while (k < n && (!mask[k] || cur[k] == bytes[k])) ++k;
        if (k == n) {
            if (found) return nullptr; // ambiguous
            found = cur;
        }
    }
    return found;
}

static uint8_t* RipTarget(uint8_t* insn, size_t dispOffset, size_t insnLen) {
    if (!insn) return nullptr;
    return insn + insnLen + *(int32_t*)(insn + dispOffset);
}

// ----------------------------------------------------------------------
// Unreal Engine 4.26 layouts (verified against the Dumper-7 SDK of this build)
// ----------------------------------------------------------------------
namespace UE {
constexpr size_t Obj_Class = 0x10;
constexpr size_t Obj_Index = 0x0C;
constexpr size_t Obj_Name = 0x18;
constexpr size_t Struct_Super = 0x40;
constexpr size_t Actor_RootComponent = 0x130;
constexpr size_t IPLMapActor_Flags = 0x2B0;      // bit 2 = bEntityUpdatePosition
constexpr uint8_t EntityUpdatePositionBit = 0x04;
constexpr size_t StaticMeshComp_StaticMesh = 0x480;
constexpr size_t PhysSettings_bSubstepping = 0x12C;
constexpr size_t PhysSettings_MaxSubstepDeltaTime = 0x130;
constexpr size_t PhysSettings_MaxSubsteps = 0x134;
}

// RenderWare/GTA entity layout in the x64 DE build (from IDA of CObject::SetIsStatic,
// CObject::ObjectDamage and the actor sync routine).
namespace GTA {
constexpr size_t Entity_Matrix = 0x18;     // CMatrix* (right, fwd, up, pos; 16-byte rows)
constexpr size_t Entity_Actor = 0x20;      // linked UE actor (UObject*)
constexpr size_t Entity_Flags = 0x30;      // bit 2 = bIsStatic
constexpr size_t Entity_Type = 0x6A;       // low 3 bits: 4 = object
constexpr size_t Entity_ModelIndex = 0x3A;    // int16 m_nModelIndex
constexpr size_t Physical_MoveSpeed = 0x7C;   // CVector m_vecMoveSpeed (+ m_vecTurnSpeed at 0x88)
constexpr uint32_t PhysFlag_SmashedRemoved = 0x800000; // set by DE's "smash completely" (ObjectDamage case 0x14)
constexpr uint32_t Flag_IsVisible = 0x80;
constexpr size_t VTable_UpdateActorVisibility = 0x98;
constexpr size_t VTable_DeleteRwObject = 0x40;
constexpr size_t ColModel_Data = 0x30;          // CColModel::m_pColData (CCollisionData*)
constexpr size_t ColData_Spheres = 0x08;        // CColSphere* (20 B each)
constexpr size_t ColData_TrianglePlanes = 0x30; // CColTrianglePlane* (allocated lazily by the game)
constexpr size_t Physical_MovingListNode = 0xF0; // CPtrNode* m_pMovingList (null = not simulated)
constexpr size_t Entity_ColDamageEffect = 0x1C0; // uint8 m_nColDamageEffect
constexpr size_t Entity_ObjectFlags = 0x1BC;   // bit 8 = bIsLampPost
constexpr uint32_t ObjFlag_IsLampPost = 0x100;
constexpr size_t Entity_ObjectInfo = 0x1DC;    // CObjectInfo* (unaligned in the x64 layout)
constexpr size_t Entity_PhysicalFlags = 0x78; // bit 2 = bDisableCollisionForce
constexpr uint32_t PhysFlag_DisableCollisionForce = 0x04;
constexpr size_t ObjectInfo_UprootLimit = 0x14;
constexpr size_t ObjectInfo_ColDamageMult = 0x18;
constexpr size_t ObjectInfo_BreakVelocity = 0x3C;     // CVector m_vecBreakVelocity (after the 8-byte fx pointer at 0x30, smash mult at 0x38)
constexpr size_t ObjectInfo_BreakVelocityRand = 0x48; // float m_fBreakVelocityRand
constexpr size_t VTable_SetIsStatic = 0x28;
constexpr uint32_t Flag_UsesCollision = 0x01;
constexpr uint32_t Flag_IsStatic = 0x04;
constexpr uint8_t Type_Object = 4;
constexpr uint8_t ColDamage_Breakable = 200, ColDamage_BreakableRemoved = 202;
// eWeaponType values DE passes to ObjectDamage for physical impacts.
constexpr int Weapon_RammedByCar = 49, Weapon_RunOverByCar = 50, Weapon_Explosion = 51, Weapon_Collision = 55;
}

#pragma pack(push, 1)
struct FNamePool {
    uint8_t Lock[8];
    uint32_t CurrentBlock;
    uint32_t CurrentByteCursor;
    uint8_t* Blocks[0x2000];
};
struct FName { int32_t Index; int32_t Number; };
struct FVec { float X, Y, Z; };
struct alignas(16) FQuat { float X, Y, Z, W; };
struct alignas(16) FTransform { FQuat Rotation; float Translation[4]; float Scale3D[4]; };
struct SetupBrokenParms {
    FVec impulseSrc;
    FVec impulseVelocity;
    uint8_t Pad[8];
    FTransform floorTransform;
    bool ReturnValue;
};
#pragma pack(pop)
static_assert(offsetof(SetupBrokenParms, floorTransform) == 0x20, "SetupBroken layout");
static_assert(offsetof(SetupBrokenParms, ReturnValue) == 0x50, "SetupBroken layout");

// ----------------------------------------------------------------------
// Game function pointers
// ----------------------------------------------------------------------
typedef void (*ProcessEvent_Fn)(void* obj, void* func, void* parms);
typedef void* (*FindFunctionByName_Fn)(void* cls, FName name, int includeSuper);
typedef void (*SetIsStatic_Fn)(void* entity, bool isStatic);
typedef void (*SyncActor_Fn)(void* entity);
typedef uintptr_t (*PhysicalStep_Fn)(void* entity);
typedef uintptr_t (*GlassCollision_Fn)(void* entity, float damage, float* speed, float* pos);
typedef uintptr_t (*GroundProbe_Fn)(void* entity, float* outPos, float* outNormal);
typedef void (*ObjectDamage_Fn)(void* entity, float damage, float* fxOrigin, float* fxDir, void* damager, int weaponType);
typedef void (*AddToMovingList_Fn)(void* entity);
typedef const float* (*GetColModel_Fn)(int modelIndex); // CColModel* (bounding box first)

static FNamePool* g_names = nullptr;
static uint8_t* g_objects = nullptr; // TUObjectArray
static ProcessEvent_Fn       o_ProcessEvent = nullptr;
static FindFunctionByName_Fn g_FindFunctionByName = nullptr;
static SetIsStatic_Fn        o_SetIsStatic = nullptr;
static SyncActor_Fn          g_SyncActor = nullptr;
static PhysicalStep_Fn       o_ProcessCollision = nullptr;
static PhysicalStep_Fn       o_ProcessShift = nullptr;
static GlassCollision_Fn     o_GlassCollision = nullptr;
static GroundProbe_Fn        g_GroundProbe = nullptr;
static ObjectDamage_Fn       o_ObjectDamage = nullptr;
static AddToMovingList_Fn    g_AddToMovingList = nullptr;
static GetColModel_Fn        g_GetColModel = nullptr;

// ----------------------------------------------------------------------
// FName lookup
// ----------------------------------------------------------------------
enum NameId {
    N_DynamicIPLMapActor, N_Dislodged, N_EntityLinked, N_SetupBroken, N_RemoveFloor, N_HideBroken,
    N_GetPhysicsFloor, N_GetPhysicsFloorC, N_GetBrokenMesh, N_SetLights, N_SetMobility,
    N_K2_DetachFromComponent, N_K2_SetWorldTransform, N_SetCollisionObjectType, N_SetCollisionEnabled,
    N_SetCollisionResponseToAllChannels, N_SetCollisionResponseToChannel, N_IgnoreComponentWhenMoving,
    N_SetAllUseCCD, N_SetStaticMesh, N_Breakable_Floor, N_PhysicsSettings, N_Default__PhysicsSettings,
    N_StreetLightMapActor, N_GetCollisionObjectType, N_GetActorBounds, N_SetAngularDamping, N_SetHiddenInGame, N_SetSimulatePhysics,
    N_GetSocketLocation, N_GetSocketQuaternion, N_SetPhysicsLinearVelocity, N_SetPhysicsAngularVelocityInDegrees,
    N_GetNumBones, N_GetBoneName, N_IsSimulatingPhysics, N_SetEnableBodyGravity, N_PutRigidBodyToSleep, N_HideBoneByName,
    N_BreakConstraint, N_GetPhysicsLinearVelocity, N_SetAlpha, N_GetClosestPointOnCollision, N_GetMaterial, N_GetNumMaterials,
    N_FindConstraintBoneName,
    N_COUNT
};
static const char* kNames[N_COUNT] = {
    "DynamicIPLMapActor", "Dislodged", "EntityLinked", "SetupBroken", "RemoveFloor", "HideBroken",
    "GetPhysicsFloor", "GetPhysicsFloorC", "GetBrokenMesh", "SetLights", "SetMobility",
    "K2_DetachFromComponent", "K2_SetWorldTransform", "SetCollisionObjectType", "SetCollisionEnabled",
    "SetCollisionResponseToAllChannels", "SetCollisionResponseToChannel", "IgnoreComponentWhenMoving",
    "SetAllUseCCD", "SetStaticMesh", "Breakable_Floor", "PhysicsSettings", "Default__PhysicsSettings",
    "StreetLightMapActor", "GetCollisionObjectType", "GetActorBounds", "SetAngularDamping", "SetHiddenInGame", "SetSimulatePhysics",
    "GetSocketLocation", "GetSocketQuaternion", "SetPhysicsLinearVelocity", "SetPhysicsAngularVelocityInDegrees",
    "GetNumBones", "GetBoneName", "IsSimulatingPhysics", "SetEnableBodyGravity", "PutRigidBodyToSleep", "HideBoneByName",
    "BreakConstraint", "GetPhysicsLinearVelocity", "SetAlpha", "GetClosestPointOnCollision", "GetMaterial", "GetNumMaterials",
    "FindConstraintBoneName",
};
static volatile LONG g_nameIdx[N_COUNT];

static int32_t ScanNamePool(const char* s) {
    const size_t n = strlen(s);
    __try {
        const uint32_t lastBlock = g_names->CurrentBlock;
        for (uint32_t b = 0; b <= lastBlock && b < 0x2000; ++b) {
            const uint8_t* blk = g_names->Blocks[b];
            if (!blk) continue;
            const uint32_t limit = (b == lastBlock) ? g_names->CurrentByteCursor : 0x20000;
            uint32_t off = 0;
            while (off + 2 <= limit) {
                const uint16_t hdr = *(const uint16_t*)(blk + off);
                const uint32_t len = hdr >> 6;
                const bool wide = hdr & 1;
                if (len == 0) break;
                if (!wide && len == n && _strnicmp((const char*)blk + off + 2, s, n) == 0) // FNames are case-insensitive
                    return (int32_t)((b << 16) | (off >> 1));
                off += (2 + len * (wide ? 2 : 1) + 1) & ~1u;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}

static bool ResolveNames() {
    bool all = true;
    for (int i = 0; i < N_COUNT; ++i) {
        if (g_nameIdx[i] >= 0) continue;
        int32_t idx = ScanNamePool(kNames[i]);
        if (idx >= 0) {
            InterlockedExchange(&g_nameIdx[i], idx);
            Log(2, "name %s = 0x%X", kNames[i], idx);
        } else {
            all = false;
        }
    }
    return all;
}

static inline int32_t NameOf(void* obj) { return *(int32_t*)((uint8_t*)obj + UE::Obj_Name); }

// Finds a UFunction on the object's class (including overrides in subclasses).
static void* FindFunc(void* obj, NameId id) {
    if (!obj || g_nameIdx[id] < 0) return nullptr;
    __try {
        void* cls = *(void**)((uint8_t*)obj + UE::Obj_Class);
        if (!cls) return nullptr;
        return g_FindFunctionByName(cls, FName{ g_nameIdx[id], 0 }, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Calls `func` by name on `obj`. `parms` must be at least as large as the UFunction's ParmsSize.
static bool Call(void* obj, NameId id, void* parms) {
    void* fn = FindFunc(obj, id);
    if (!fn) return false;
    uint8_t scratch[0x200] = {};
    o_ProcessEvent(obj, fn, parms ? parms : scratch);
    return true;
}

// Prop kind by walking SuperStruct names; cached per class (pointer + name, so a reused address is re-checked).
enum PropKind : uint8_t { PROP_NONE = 0, PROP_DYNAMIC = 1, PROP_STREETLIGHT = 2 };
static struct { void* cls; int32_t name; uint8_t kind; } g_classCache[512];
static int g_classCacheNext = 0; // game thread only

static uint8_t PropKindOf(void* actor) {
    if (!actor || g_nameIdx[N_DynamicIPLMapActor] < 0 || g_nameIdx[N_StreetLightMapActor] < 0) return PROP_NONE;
    __try {
        void* cls = *(void**)((uint8_t*)actor + UE::Obj_Class);
        if (!cls) return PROP_NONE;
        const int32_t clsName = NameOf(cls);
        for (auto& e : g_classCache)
            if (e.cls == cls && e.name == clsName) return e.kind;
        uint8_t kind = PROP_NONE;
        for (void* s = cls; s; s = *(void**)((uint8_t*)s + UE::Struct_Super)) {
            const int32_t n = NameOf(s);
            if (n == g_nameIdx[N_StreetLightMapActor]) kind |= PROP_STREETLIGHT;
            if (n == g_nameIdx[N_DynamicIPLMapActor]) { kind |= PROP_DYNAMIC; break; }
        }
        g_classCache[g_classCacheNext] = { cls, clsName, kind };
        g_classCacheNext = (g_classCacheNext + 1) % (int)(sizeof(g_classCache) / sizeof(g_classCache[0]));
        return kind;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return PROP_NONE;
    }
}

static bool IsDynamicPropActor(void* actor) { return (PropKindOf(actor) & PROP_DYNAMIC) != 0; }
static bool IsStreetLightActor(void* actor) { return PropKindOf(actor) == (PROP_DYNAMIC | PROP_STREETLIGHT); }

// ----------------------------------------------------------------------
// Small pointer sets (game thread only)
// ----------------------------------------------------------------------
template <int N>
struct PtrSet {
    void* items[N] = {};
    int count = 0;
    int Find(void* p) const { for (int i = 0; i < count; ++i) if (items[i] == p) return i; return -1; }
    bool Has(void* p) const { return Find(p) >= 0; }
    void Add(void* p) { if (!Has(p)) { if (count == N) Remove(items[0]); items[count++] = p; } }
    void Remove(void* p) { int i = Find(p); if (i >= 0) items[i] = items[--count]; }
};
static PtrSet<256> g_lightsOff; // street light actors whose lights we switched off

// Entities the plugin handed back to RenderWare, with the actor's original bEntityUpdatePosition.
struct Sample { DWORD t; float z, bottom, upZ, speed; };
static struct Dislodged {
    void* entity; void* actor; bool hadUpdateFlag; bool warnedSink; bool resting; float startZ; float groundZ;
    float bb[6]; float minBottom; float startSolid; float last[4]; DWORD tick; int model; Sample ring[8]; int ringPos;
} g_dislodged[1024];
static int g_dislodgedCount = 0;

static int FindDislodged(void* entity) {
    for (int i = 0; i < g_dislodgedCount; ++i) if (g_dislodged[i].entity == entity) return i;
    return -1;
}
static void RemoveDislodgedAt(int i) { g_dislodged[i] = g_dislodged[--g_dislodgedCount]; }
static Dislodged* AddDislodged(void* entity, void* actor, bool hadFlag) {
    const int i = FindDislodged(entity);
    if (i >= 0) { g_dislodged[i].actor = actor; return &g_dislodged[i]; } // keep the first-seen original flag
    if (g_dislodgedCount == (int)(sizeof(g_dislodged) / sizeof(g_dislodged[0]))) RemoveDislodgedAt(0);
    Dislodged& d = g_dislodged[g_dislodgedCount++];
    memset(&d, 0, sizeof(d));
    d.entity = entity; d.actor = actor; d.hadUpdateFlag = hadFlag; d.tick = GetTickCount();
    return &d;
}

// ----------------------------------------------------------------------
// Diagnostics: readable descriptions of props for the log
// ----------------------------------------------------------------------
// Raw field reads only are guarded by SEH; engine calls are never wrapped, so their faults are not hidden.
static void* ReadPtr(void* base, size_t off) {
    __try {
        return *(void**)((uint8_t*)base + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static void NameToStr(int32_t idx, char* out, size_t cap) {
    strcpy_s(out, cap, "?");
    if (!g_names || idx < 0) return;
    __try {
        const uint8_t* blk = g_names->Blocks[(uint32_t)idx >> 16];
        if (!blk) return;
        const uint8_t* e = blk + (size_t)(idx & 0xFFFF) * 2;
        const uint16_t hdr = *(const uint16_t*)e;
        size_t len = hdr >> 6;
        if (len >= cap) len = cap - 1;
        for (size_t i = 0; i < len; ++i) out[i] = (hdr & 1) ? (char)((const uint16_t*)(e + 2))[i] : (char)e[2 + i];
        out[len] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

struct PropInfo {
    int model = -1, effect = -1;
    float uproot = 0, mult = 0, x = 0, y = 0, z = 0, upZ = 1;
    float colMinZ = 0, colMaxZ = 0; bool hasCol = false;
    char cls[96] = "?";
};

static void ReadPropInfo(void* entity, void* actor, PropInfo& pi) {
    __try {
        uint8_t* e = (uint8_t*)entity;
        pi.model = *(int16_t*)(e + GTA::Entity_ModelIndex);
        pi.effect = e[GTA::Entity_ColDamageEffect];
        if (uint8_t* info = *(uint8_t**)(e + GTA::Entity_ObjectInfo)) {
            pi.uproot = *(float*)(info + GTA::ObjectInfo_UprootLimit);
            pi.mult = *(float*)(info + GTA::ObjectInfo_ColDamageMult);
        }
        if (const float* m = *(const float**)(e + GTA::Entity_Matrix)) {
            pi.upZ = m[10]; pi.x = m[12]; pi.y = m[13]; pi.z = m[14];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (g_GetColModel && pi.model >= 0) {
        const float* bb = g_GetColModel(pi.model); // CColModel: CBoundingBox { min, max } first
        __try {
            if (bb) { pi.colMinZ = bb[2]; pi.colMaxZ = bb[5]; pi.hasCol = pi.colMaxZ > pi.colMinZ; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (void* cls = actor ? ReadPtr(actor, UE::Obj_Class) : nullptr) NameToStr(NameOf(cls), pi.cls, sizeof(pi.cls));
}

#define PROPFMT "%s model=%d eff=%d uproot=%.0f mult=%.2f pos=(%.1f %.1f %.1f) up.z=%.2f"
#define PROPARGS(pi) (pi).cls, (pi).model, (pi).effect, (pi).uproot, (pi).mult, (pi).x, (pi).y, (pi).z, (pi).upZ

// Visual height of the actor (metres), from AActor::GetActorBounds.
static float ActorHeightM(void* actor) {
    alignas(8) uint8_t parms[0x20] = {}; // bOnlyCollidingComponents=false, Origin, BoxExtent, bIncludeFromChildActors
    if (!Call(actor, N_GetActorBounds, parms)) return 0.0f;
    return *(float*)(parms + 0x18) * 2.0f / 100.0f; // BoxExtent.Z (cm, half) -> full height in m
}

// RenderWare can only simulate a prop the way the original did if its RW collision covers the whole
// prop. Some DE poles keep only a stub at the base (the prop then hangs from its base under the road
// and cars pass through it), so compare the RW col height with the visual height, per model.
static int8_t g_colVerdict[32768]; // 0 unknown, 1 RW ok, 2 RW collision incomplete

// Vertical extent actually covered by solid RW collision shapes (model space). The bounding box can span
// the whole prop while the spheres/boxes/triangles only exist at the base (e.g. BP_MTraffic1_C), so merge
// the z-ranges of every primitive. Layout from DE's CCollision line test (0x1410415E0):
// CColModel+0x30 -> CCollisionData { u16 nSpheres, nBoxes, nTris; ...; +0x08 spheres (20 B: c.xyz, r),
// +0x10 boxes (28 B: min.xyz, max.xyz), +0x20 float3 vertices, +0x28 tris (16 B: 3 x int32 index) }.
struct ColShapes { int spheres = 0, boxes = 0, tris = 0; float covered = 0, lo = 0, hi = 0; bool hasData = false; };
static float g_ivals[8192][2];

static int CmpIval(const void* a, const void* b) {
    const float d = ((const float*)a)[0] - ((const float*)b)[0];
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

static int CollectColIntervals(const float* colModel, ColShapes& cs) {
    int n = 0;
    const int cap = (int)(sizeof(g_ivals) / sizeof(g_ivals[0]));
    __try {
        const uint8_t* data = *(const uint8_t* const*)((const uint8_t*)colModel + GTA::ColModel_Data);
        if (!data) return 0;
        cs.hasData = true;
        cs.spheres = *(const uint16_t*)(data + 0);
        cs.boxes = *(const uint16_t*)(data + 2);
        cs.tris = *(const uint16_t*)(data + 4);
        const float* sp = *(const float* const*)(data + 0x08);
        const float* bx = *(const float* const*)(data + 0x10);
        const float* vt = *(const float* const*)(data + 0x20);
        const int32_t* tr = *(const int32_t* const*)(data + 0x28);
        for (int i = 0; sp && i < cs.spheres && n < cap; ++i, ++n) {
            const float* s = sp + i * 5;
            g_ivals[n][0] = s[2] - s[3]; g_ivals[n][1] = s[2] + s[3];
        }
        for (int i = 0; bx && i < cs.boxes && n < cap; ++i, ++n) {
            const float* b = bx + i * 7;
            g_ivals[n][0] = b[2]; g_ivals[n][1] = b[5];
        }
        for (int i = 0; vt && tr && i < cs.tris && n < cap; ++i, ++n) {
            const int32_t* t = tr + i * 4;
            const float z0 = vt[t[0] * 3 + 2], z1 = vt[t[1] * 3 + 2], z2 = vt[t[2] * 3 + 2];
            g_ivals[n][0] = fminf(z0, fminf(z1, z2)); g_ivals[n][1] = fmaxf(z0, fmaxf(z1, z2));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cs.hasData = false;
        return 0;
    }
    return n;
}

static ColShapes MeasureColShapes(const float* colModel) {
    ColShapes cs;
    if (!colModel) return cs;
    const int n = CollectColIntervals(colModel, cs);
    if (!n) return cs;
    qsort(g_ivals, n, sizeof(g_ivals[0]), CmpIval);
    float curLo = g_ivals[0][0], curHi = g_ivals[0][1];
    cs.lo = curLo; cs.hi = curHi;
    for (int i = 1; i < n; ++i) {
        cs.hi = fmaxf(cs.hi, g_ivals[i][1]);
        if (g_ivals[i][0] <= curHi + 0.05f) { curHi = fmaxf(curHi, g_ivals[i][1]); continue; }
        cs.covered += curHi - curLo;
        curLo = g_ivals[i][0]; curHi = g_ivals[i][1];
    }
    cs.covered += curHi - curLo;
    return cs;
}

// RenderWare's CCollision::ProcessColModels only tests a MOVING object's spheres (and lines) against the
// world; the object's triangles are only tested against the other side's spheres (cars), and the world has
// none. So a prop that falls in one piece needs spheres along its whole length. Measure the sphere span.
struct SphereSpan { int count = 0; float lo = 0, hi = 0; };
static SphereSpan MeasureSpheres(const float* colModel) {
    SphereSpan s;
    __try {
        const uint8_t* data = *(const uint8_t* const*)((const uint8_t*)colModel + GTA::ColModel_Data);
        if (!data) return s;
        const int n = *(const uint16_t*)data;
        const float* sp = *(const float* const*)(data + 0x08);
        for (int i = 0; sp && i < n; ++i) {
            const float lo = sp[i * 5 + 2] - sp[i * 5 + 3], hi = sp[i * 5 + 2] + sp[i * 5 + 3];
            s.lo = s.count ? fminf(s.lo, lo) : lo;
            s.hi = s.count ? fmaxf(s.hi, hi) : hi;
            ++s.count;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return s;
}

// ---- Collision augmentation: spheres generated along a triangle-mesh prop, served only to props the
// plugin simulates (CEntity::GetColModel hook). The game's collision data is never modified: the copy's
// header is refreshed from the game's data on every call, so streamed/reloaded collision stays valid.
constexpr int kAugMaxSpheres = 64;
struct AugCol {
    alignas(16) uint8_t colModel[0x38];  // CColModel copy (bbox, bounding sphere, flags, colData -> data)
    alignas(16) uint8_t data[0x60];      // CCollisionData header copy (spheres -> spheres below)
    float spheres[kAugMaxSpheres * 5];   // CColSphere: centre xyz, radius, surface
    int nSpheres;
    float radius;
};
static AugCol* g_aug[32768];
static float g_pts[16384][3];

static int GatherColPoints(const float* colModel, uint32_t& surface) {
    int n = 0;
    const int cap = (int)(sizeof(g_pts) / sizeof(g_pts[0]));
    auto add = [&](float x, float y, float z) { if (n < cap) { g_pts[n][0] = x; g_pts[n][1] = y; g_pts[n][2] = z; ++n; } };
    __try {
        const uint8_t* data = *(const uint8_t* const*)((const uint8_t*)colModel + GTA::ColModel_Data);
        if (!data) return 0;
        const int ns = *(const uint16_t*)(data + 0), nb = *(const uint16_t*)(data + 2), nt = *(const uint16_t*)(data + 4);
        const float* sp = *(const float* const*)(data + 0x08);
        const float* bx = *(const float* const*)(data + 0x10);
        const float* vt = *(const float* const*)(data + 0x20);
        const int32_t* tr = *(const int32_t* const*)(data + 0x28);
        for (int i = 0; sp && i < ns; ++i) { add(sp[i * 5], sp[i * 5 + 1], sp[i * 5 + 2]); surface = *(const uint32_t*)(sp + i * 5 + 4); }
        for (int i = 0; bx && i < nb; ++i) { // sample boxes every <=0.25 m, both faces included
            const float* b = bx + i * 7;
            int st[3];
            for (int k = 0; k < 3; ++k) st[k] = (int)fminf(40.0f, fmaxf(1.0f, ceilf((b[k + 3] - b[k]) / 0.25f)));
            for (int ix = 0; ix <= st[0]; ++ix)
                for (int iy = 0; iy <= st[1]; ++iy)
                    for (int iz = 0; iz <= st[2]; ++iz)
                        add(b[0] + (b[3] - b[0]) * ix / st[0], b[1] + (b[4] - b[1]) * iy / st[1], b[2] + (b[5] - b[2]) * iz / st[2]);
        }
        for (int i = 0; vt && tr && i < nt; ++i) { // sample triangle surfaces every ~0.25 m
            const float* a = vt + tr[i * 4] * 3; const float* b = vt + tr[i * 4 + 1] * 3; const float* c = vt + tr[i * 4 + 2] * 3;
            if (!sp || !ns) surface = *(const uint32_t*)(tr + i * 4 + 3);
            const float lab = sqrtf((b[0]-a[0])*(b[0]-a[0]) + (b[1]-a[1])*(b[1]-a[1]) + (b[2]-a[2])*(b[2]-a[2]));
            const float lac = sqrtf((c[0]-a[0])*(c[0]-a[0]) + (c[1]-a[1])*(c[1]-a[1]) + (c[2]-a[2])*(c[2]-a[2]));
            const int steps = (int)fminf(40.0f, fmaxf(1.0f, fmaxf(lab, lac) / 0.25f));
            for (int u = 0; u <= steps; ++u)
                for (int v = 0; u + v <= steps; ++v) {
                    const float fu = (float)u / steps, fv = (float)v / steps;
                    add(a[0] + fu * (b[0] - a[0]) + fv * (c[0] - a[0]), a[1] + fu * (b[1] - a[1]) + fv * (c[1] - a[1]),
                        a[2] + fu * (b[2] - a[2]) + fv * (c[2] - a[2]));
                }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return n;
}

// Slice the collision surface into 0.5 m height slabs. A thin slab (pole) gets one sphere on its axis sized
// to its thickness; a wide slab (lamp arm, signal gantry) is voxelised into 0.5 m cells, one sphere each.
static AugCol* BuildAugCol(const float* colModel) {
    uint32_t surface = 0;
    const int n = GatherColPoints(colModel, surface);
    if (n < 4) return nullptr;
    auto* a = (AugCol*)VirtualAlloc(nullptr, sizeof(AugCol), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); // never freed
    if (!a) return nullptr;
    float zlo = g_pts[0][2], zhi = g_pts[0][2];
    for (int i = 1; i < n; ++i) { zlo = fminf(zlo, g_pts[i][2]); zhi = fmaxf(zhi, g_pts[i][2]); }
    for (float slab = 0.5f; slab <= 4.0f; slab *= 1.4f) {
        int ns = 0;
        bool overflow = false;
        float rmax = 0.0f;
        for (float z0 = zlo; z0 <= zhi + 1e-3f && !overflow; z0 += slab) {
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, sz = 0; int c = 0;
            for (int i = 0; i < n; ++i) {
                if (g_pts[i][2] < z0 || g_pts[i][2] >= z0 + slab) continue;
                x0 = fminf(x0, g_pts[i][0]); x1 = fmaxf(x1, g_pts[i][0]);
                y0 = fminf(y0, g_pts[i][1]); y1 = fmaxf(y1, g_pts[i][1]); sz += g_pts[i][2]; ++c;
            }
            if (!c) continue;
            const float w = fmaxf(x1 - x0, y1 - y0);
            if (w <= 1.0f) { // thin: one sphere on the axis
                if (ns == kAugMaxSpheres) { overflow = true; break; }
                float* s = a->spheres + ns++ * 5;
                s[0] = (x0 + x1) * 0.5f; s[1] = (y0 + y1) * 0.5f; s[2] = sz / c;
                s[3] = fminf(0.5f, fmaxf(0.1f, w * 0.5f));
                *(uint32_t*)(s + 4) = surface;
                rmax = fmaxf(rmax, s[3]);
                continue;
            }
            const float cell = fmaxf(0.5f, slab);
            for (int i = 0; i < n && !overflow; ++i) { // wide: voxel cells within the slab
                if (g_pts[i][2] < z0 || g_pts[i][2] >= z0 + slab) continue;
                const float cx = floorf(g_pts[i][0] / cell), cy = floorf(g_pts[i][1] / cell);
                int k = 0;
                for (; k < ns; ++k) {
                    float* s = a->spheres + k * 5;
                    if (s[2] >= z0 && s[2] < z0 + slab && floorf(s[0] / cell) == cx && floorf(s[1] / cell) == cy) break;
                }
                if (k < ns) continue;
                if (ns == kAugMaxSpheres) { overflow = true; break; }
                float* s = a->spheres + ns++ * 5;
                s[0] = (cx + 0.5f) * cell; s[1] = (cy + 0.5f) * cell; s[2] = z0 + slab * 0.5f; s[3] = fminf(0.35f, cell * 0.5f);
                *(uint32_t*)(s + 4) = surface;
                rmax = fmaxf(rmax, s[3]);
            }
        }
        if (overflow) continue;
        a->nSpheres = ns;
        a->radius = rmax;
        return a;
    }
    VirtualFree(a, 0, MEM_RELEASE);
    return nullptr;
}

// Returns our extended copy of `colModel` (header refreshed from the game's current data), or null.
static const void* RefreshAugCol(AugCol* a, const uint8_t* colModel) {
    const uint8_t* src = *(const uint8_t* const*)(colModel + GTA::ColModel_Data);
    if (!src) return nullptr;
    void* ourPlanes = *(void**)(a->data + GTA::ColData_TrianglePlanes); // allocated by the game for our copy
    memcpy(a->colModel, colModel, sizeof(a->colModel));
    memcpy(a->data, src, sizeof(a->data));
    *(void**)(a->colModel + GTA::ColModel_Data) = a->data;
    *(void**)(a->data + GTA::ColData_TrianglePlanes) = ourPlanes; // never share the game's plane cache entry
    *(uint16_t*)(a->data + 0) = (uint16_t)a->nSpheres;
    *(float**)(a->data + GTA::ColData_Spheres) = a->spheres;
    return a->colModel;
}

typedef const void* (*EntityColModel_Fn)(void* entity);
static EntityColModel_Fn o_EntityColModel = nullptr;
static int FindDislodged(void* entity);

// DE's collision code mostly inlines CEntity::GetColModel as modelInfos[model]->GetColModel() (vtable +0x28,
// shared implementation 0x140D2BE90). It has no entity argument, so the plugin's ProcessCollision /
// ProcessShift hooks record which tracked prop is being simulated on this thread.
typedef const void* (*MIColModel_Fn)(void* modelInfo);
static MIColModel_Fn o_MIColModel = nullptr;
static void** g_modelInfos = nullptr; // CModelInfo::ms_modelInfoPtrs
static thread_local void* t_simEntity = nullptr;

static const void* Hooked_MIColModel(void* modelInfo) {
    const void* cm = o_MIColModel(modelInfo);
    void* e = t_simEntity;
    if (!e || !cm || !g_modelInfos) return cm;
    const int model = *(const int16_t*)((const uint8_t*)e + GTA::Entity_ModelIndex);
    if (model < 0 || model >= 32768 || !g_aug[model] || g_modelInfos[model] != modelInfo) return cm;
    const void* aug = RefreshAugCol(g_aug[model], (const uint8_t*)cm);
    return aug ? aug : cm;
}

static const void* Hooked_EntityColModel(void* entity) {
    const void* cm = o_EntityColModel(entity);
    if (!cm || !entity) return cm;
    const int model = *(const int16_t*)((const uint8_t*)entity + GTA::Entity_ModelIndex);
    if (model < 0 || model >= 32768 || !g_aug[model]) return cm;
    if ((*((const uint8_t*)entity + GTA::Entity_Type) & 7) != GTA::Type_Object || FindDislodged(entity) < 0) return cm;
    const void* aug = RefreshAugCol(g_aug[model], (const uint8_t*)cm);
    return aug ? aug : cm;
}

static bool RwCollisionCovers(void* entity, void* actor) {
    PropInfo pi;
    ReadPropInfo(entity, actor, pi);
    if (pi.model < 0 || pi.model >= 32768 || !g_GetColModel) return true;
    if (g_colVerdict[pi.model]) return g_colVerdict[pi.model] == 1;
    const float visH = ActorHeightM(actor);
    const float boxH = pi.hasCol ? pi.colMaxZ - pi.colMinZ : 0.0f;
    const float* colModel = g_GetColModel(pi.model);
    const ColShapes cs = MeasureColShapes(colModel);
    const SphereSpan ss = MeasureSpheres(colModel);
    // 1) the col box spans enough of the visible prop, 2) solid shapes fill that box and reach its top,
    // 3) spheres (the only shapes RW collides with the world) span it too, or we can generate them.
    const bool boxOk = visH < 1.5f || boxH >= g_cfg.minColCoverage * visH;
    const bool shapesOk = cs.hasData && boxH > 0.0f && cs.covered >= g_cfg.minShapeCoverage * boxH &&
                          cs.hi >= pi.colMinZ + g_cfg.minShapeCoverage * boxH;
    const bool spheresOk = boxH < 1.5f || (ss.count >= 2 && ss.hi - ss.lo >= g_cfg.minShapeCoverage * boxH);
    const char* verdict = "RenderWare physics";
    bool ok = boxOk && (visH < 1.5f || shapesOk);
    if (ok && !spheresOk) {
        if (!g_cfg.addSpheres) { ok = false; verdict = "too few spheres for RW ground collision: keep DE behaviour"; }
        else if (!g_aug[pi.model] && !(g_aug[pi.model] = BuildAugCol(colModel))) { ok = false; verdict = "sphere generation failed: keep DE behaviour"; }
        else verdict = "RenderWare physics + generated spheres";
    } else if (!ok) {
        verdict = !cs.hasData ? "no RW collision data: keep DE behaviour" : "RW collision incomplete: keep DE behaviour";
    }
    if (pi.upZ > 0.9f) g_colVerdict[pi.model] = ok ? 1 : 2; // only cache measurements taken upright
    if (ShouldLog(actor, 8, 10000))
        Log(1, "col-check " PROPFMT " box z=%.2f..%.2f (%.2fm) visual=%.2fm box/visual=%d%% | shapes s=%d b=%d t=%d "
               "solid z=%.2f..%.2f covered=%d%% | spheres z=%.2f..%.2f%s -> %s",
            PROPARGS(pi), pi.colMinZ, pi.colMaxZ, boxH, visH, visH > 0 ? (int)(100 * boxH / visH) : -1, cs.spheres,
            cs.boxes, cs.tris, cs.lo, cs.hi, boxH > 0 ? (int)(100 * cs.covered / boxH) : -1, ss.lo, ss.hi,
            g_aug[pi.model] ? " (generated)" : "", verdict);
    if (g_aug[pi.model] && ShouldLog(g_aug[pi.model], 11, 600000))
        Log(1, "  generated %d spheres r=%.2f for model %d", g_aug[pi.model]->nSpheres, g_aug[pi.model]->radius, pi.model);
    return ok;
}

// Last impact on an object (from ObjectDamage), reused when the plugin has to break a prop itself.
struct HitCtx { void* entity; float damage; float pos[3]; float speed[3]; };
static thread_local HitCtx t_hit = {};
static thread_local const char* t_path = "other"; // which native path is breaking/uprooting (for the log)
static thread_local void* t_breakEntity = nullptr;  // RW object being broken (for its object.dat break data)

// ----------------------------------------------------------------------
// 1. Dislodged props follow the RenderWare simulation
// ----------------------------------------------------------------------
static thread_local void* t_suppressDislodgedFor = nullptr;

static void* EntityActor(void* entity) { return ReadPtr(entity, GTA::Entity_Actor); }

static bool IsObjectEntity(void* entity) {
    return (*((uint8_t*)entity + GTA::Entity_Type) & 7) == GTA::Type_Object;
}

static void MakeRootMovable(void* actor) {
    void* root = ReadPtr(actor, UE::Actor_RootComponent);
    if (!root) return;
    uint8_t parms[8] = { 2 }; // EComponentMobility::Movable
    Call(root, N_SetMobility, parms);
}

static void UpdateStreetLight(void* entity, void* actor) {
    if (!g_cfg.lightsOffWhenTilted || g_lightsOff.Has(actor) || !IsStreetLightActor(actor)) return;
    const float* m = (const float*)ReadPtr(entity, GTA::Entity_Matrix);
    // CEntity::ProcessLightsForEntity: no lights when up.z < 0.96
    if (!m || m[10] >= 0.96f) return;
    uint8_t parms[8] = { 0, 0 }; // bLightsOn = false, bForceForEditor = false
    if (Call(actor, N_SetLights, parms)) {
        g_lightsOff.Add(actor);
        STAT(lightsOff);
        Log(2, "lights off: actor %p up.z=%.2f", actor, m[10]);
    }
}

// Returns the actor if `entity` is a dislodged prop that should follow RenderWare right now.
static void* DislodgedActor(void* entity) {
    if (FindDislodged(entity) < 0) return nullptr;
    __try {
        if (!IsObjectEntity(entity)) return nullptr;
        if (*(uint32_t*)((uint8_t*)entity + GTA::Entity_Flags) & GTA::Flag_IsStatic) return nullptr;
        void* actor = *(void**)((uint8_t*)entity + GTA::Entity_Actor);
        if (!actor || !IsDynamicPropActor(actor)) return nullptr;
        if (!(*((uint8_t*)actor + UE::IPLMapActor_Flags) & UE::EntityUpdatePositionBit)) return nullptr;
        return actor;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Lowest world z of the RW collision box: pos.z + sum over the three axes of the lower corner term.
static float ColBottomZ(const float* m, const float* bb) {
    float z = m[14];
    const float axisZ[3] = { m[2], m[6], m[10] }; // right.z, forward.z, up.z
    for (int k = 0; k < 3; ++k) z += fminf(bb[k] * axisZ[k], bb[k + 3] * axisZ[k]);
    return z;
}

// Lowest world z of the solid RW collision shapes (spheres, box corners, triangle vertices).
static float ExactColBottomZ(const float* colModel, const float* m) {
    float lo = 1e9f;
    auto wz = [m](float x, float y, float z) { return m[14] + x * m[2] + y * m[6] + z * m[10]; };
    __try {
        const uint8_t* data = *(const uint8_t* const*)((const uint8_t*)colModel + GTA::ColModel_Data);
        if (!data) return lo;
        const int ns = *(const uint16_t*)(data + 0), nb = *(const uint16_t*)(data + 2), nt = *(const uint16_t*)(data + 4);
        const float* sp = *(const float* const*)(data + 0x08);
        const float* bx = *(const float* const*)(data + 0x10);
        const float* vt = *(const float* const*)(data + 0x20);
        const int32_t* tr = *(const int32_t* const*)(data + 0x28);
        for (int i = 0; sp && i < ns; ++i) lo = fminf(lo, wz(sp[i * 5], sp[i * 5 + 1], sp[i * 5 + 2]) - sp[i * 5 + 3]);
        for (int i = 0; bx && i < nb; ++i) {
            const float* b = bx + i * 7;
            for (int c = 0; c < 8; ++c) lo = fminf(lo, wz(b[(c & 1) ? 3 : 0], b[(c & 2) ? 4 : 1], b[(c & 4) ? 5 : 2]));
        }
        for (int i = 0; vt && tr && i < nt; ++i)
            for (int k = 0; k < 3; ++k) {
                const float* v = vt + tr[i * 4 + k] * 3;
                lo = fminf(lo, wz(v[0], v[1], v[2]));
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return lo;
}

static void SyncDislodged(void* entity) {
    void* actor = DislodgedActor(entity);
    if (!actor) return;
    const int i = FindDislodged(entity);
    const float* m = (const float*)ReadPtr(entity, GTA::Entity_Matrix);
    if (i < 0 || !m) return;
    Dislodged& d = g_dislodged[i];
    // Props kept pushable at rest are processed every frame: only move the actor when the pose changed.
    if (d.resting && m[12] == d.last[0] && m[13] == d.last[1] && m[14] == d.last[2] && m[10] == d.last[3]) return;
    d.last[0] = m[12]; d.last[1] = m[13]; d.last[2] = m[14]; d.last[3] = m[10];
    g_SyncActor(entity);
    STAT(syncs);
    UpdateStreetLight(entity, actor);

    // Fall-through detector: bottom of the RW collision box against the ground under the prop.
    const float bottom = ColBottomZ(m, d.bb);
    if (bottom < d.minBottom) d.minBottom = bottom;
    const DWORD now = GetTickCount();
    Sample& prev = d.ring[(d.ringPos + 7) % 8];
    if (now - prev.t >= 100) {
        const float* v = (const float*)((uint8_t*)entity + GTA::Physical_MoveSpeed);
        d.ring[d.ringPos] = { now, m[14], bottom, m[10], sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) * 50.0f };
        d.ringPos = (d.ringPos + 1) % 8;
    }
    // Cheap bounding-box test first; confirm with the solid shapes (a lamp arm makes the box corner dip).
    float solidBottom = bottom;
    if (!d.warnedSink && bottom < d.groundZ - 0.75f && g_GetColModel)
        if (const float* cm = g_GetColModel(d.model)) solidBottom = ExactColBottomZ(cm, m);
    // Foundations are often modelled below the pavement: only warn below the lower of ground and start.
    const float floorZ = fminf(d.groundZ, d.startSolid);
    if (!d.warnedSink && bottom < floorZ - 0.75f && solidBottom < floorZ - 0.5f && solidBottom < 1e8f) {
        d.warnedSink = true;
        PropInfo pi;
        ReadPropInfo(entity, actor, pi);
        Log(1, "WARNING below ground: " PROPFMT " solidBottom=%.2f ground=%.2f (%.2fm under) after %lums; last samples "
               "(ms z boxBottom up.z speed m/s):", PROPARGS(pi), solidBottom, d.groundZ, d.groundZ - solidBottom, now - d.tick);
        for (int k = 0; k < 8; ++k) {
            const Sample& s = d.ring[(d.ringPos + k) % 8];
            if (s.t) Log(1, "    %5lu %7.2f %7.2f %5.2f %5.1f", s.t - d.tick, s.z, s.bottom, s.upZ, s.speed);
        }
    }
}

static void UprootEntity(void* entity) {
    auto setIsStatic = *(SetIsStatic_Fn*)(*(uint8_t**)entity + GTA::VTable_SetIsStatic);
    setIsStatic(entity, false); // -> Hooked_SetIsStatic -> RenderWare physics
    g_AddToMovingList(entity);  // CWorld::Process only simulates the moving list
}

static void ForceDEBreak(void* entity); // defined with the glass hook

static void StopTracking(void* entity, int i) {
    const Dislodged d = g_dislodged[i];
    RemoveDislodgedAt(i);
    if (!d.hadUpdateFlag && EntityActor(entity) == d.actor) {
        __try {
            *((uint8_t*)d.actor + UE::IPLMapActor_Flags) &= (uint8_t)~UE::EntityUpdatePositionBit;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

static void Hooked_SetIsStatic(void* entity, bool isStatic) {
    if (isStatic) {
        const int i = entity ? FindDislodged(entity) : -1;
        if (i < 0) { o_SetIsStatic(entity, isStatic); return; }
        Dislodged& d = g_dislodged[i];
        // A re-created object at a reused address (CObject::Init) is not in the moving list: forget the entry.
        const bool live = EntityActor(entity) == d.actor && ReadPtr(entity, GTA::Physical_MovingListNode) != nullptr;
        if (live && g_cfg.keepPushable) {
            // CObject::ProcessControl wants to freeze the settled prop. Leave it non-static (still in the
            // moving list, no gravity step while settled) so any car or ped touching it pushes it again,
            // instead of it turning back into a rooted prop with its full uproot limit.
            SyncDislodged(entity);
            if (!d.resting) {
                d.resting = true;
                STAT(rested);
                const float* m = (const float*)ReadPtr(entity, GTA::Entity_Matrix);
                PropInfo pi;
                ReadPropInfo(entity, d.actor, pi);
                const float* cm = g_GetColModel ? g_GetColModel(d.model) : nullptr;
                const float solid = m && cm ? ExactColBottomZ(cm, m) : 0.0f;
                Log(1, "settled (kept pushable): " PROPFMT " after %lums solidBottom=%.2f ground=%.2f (%s)", PROPARGS(pi),
                    GetTickCount() - d.tick, solid, d.groundZ,
                    solid < d.groundZ - 0.5f ? "UNDER GROUND" : solid > d.groundZ + 1.0f ? "above ground" : "on ground");
            }
            return;
        }
        if (live) SyncDislodged(entity); // final resting pose
        StopTracking(entity, i);
        o_SetIsStatic(entity, isStatic);
        if (live) {
            STAT(rested);
            PropInfo pi;
            ReadPropInfo(entity, EntityActor(entity), pi);
            Log(1, "rested (static): " PROPFMT, PROPARGS(pi));
        }
        return;
    }

    void* actor = nullptr;
    const bool dislodge = g_cfg.rwDislodgedProps && entity && IsObjectEntity(entity) &&
                          (actor = EntityActor(entity)) != nullptr && IsDynamicPropActor(actor);
    if (!dislodge) { o_SetIsStatic(entity, isStatic); return; }

    const int known = FindDislodged(entity);
    if (known >= 0 && g_dislodged[known].actor == actor) {
        // Already following RenderWare (buoyancy, or a settled prop being pushed again).
        t_suppressDislodgedFor = actor;
        o_SetIsStatic(entity, isStatic);
        t_suppressDislodgedFor = nullptr;
        if (g_dislodged[known].resting) {
            g_dislodged[known].resting = false;
            Log(2, "pushed again: entity %p", entity);
        }
        return;
    }
    if (!g_cfg.uprootStreetLights && IsStreetLightActor(actor)) {
        // 2004 behaviour: these poles are breakable (object.dat CDEff 200) and shatter before they can uproot.
        PropInfo pi;
        ReadPropInfo(entity, actor, pi);
        if (pi.effect == GTA::ColDamage_Breakable || pi.effect == GTA::ColDamage_BreakableRemoved) {
            STAT(breakKept);
            Log(1, "dislodge->shatter (original breakable): " PROPFMT, PROPARGS(pi));
            ForceDEBreak(entity);
            return;
        }
    }
    if (!RwCollisionCovers(entity, actor)) {
        PropInfo pi;
        ReadPropInfo(entity, actor, pi);
        if (IsStreetLightActor(actor)) {
            // RenderWare would drop this pole through the road: break it the DE way instead.
            STAT(breakKept);
            Log(1, "dislodge->DE break (RW col incomplete): " PROPFMT, PROPARGS(pi));
            ForceDEBreak(entity);
            return; // stays static; the Blueprint shows the broken pole
        }
        STAT(dislodgeStock);
        Log(1, "dislodge->stock DE (RW col incomplete): " PROPFMT, PROPARGS(pi));
        o_SetIsStatic(entity, isStatic);
        return;
    }

    t_suppressDislodgedFor = actor;
    o_SetIsStatic(entity, isStatic);
    t_suppressDislodgedFor = nullptr;

    bool hadFlag = false;
    __try {
        uint8_t& flags = *((uint8_t*)actor + UE::IPLMapActor_Flags);
        hadFlag = (flags & UE::EntityUpdatePositionBit) != 0;
        flags |= UE::EntityUpdatePositionBit;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    PropInfo pi;
    ReadPropInfo(entity, actor, pi);
    Dislodged* d = AddDislodged(entity, actor, hadFlag);
    d->model = pi.model;
    d->startZ = pi.z;
    if (const float* bb = g_GetColModel ? g_GetColModel(pi.model) : nullptr) memcpy(d->bb, bb, sizeof(d->bb));
    d->groundZ = -1000.0f;
    if (ReadPtr(entity, GTA::Entity_Matrix)) {
        float gp[3] = {}, gn[3] = { 0, 0, 1 };
        g_GroundProbe(entity, gp, gn); // same vertical probe the original BreakObject_c used
        d->groundZ = gp[2];
    }
    if (const float* m = (const float*)ReadPtr(entity, GTA::Entity_Matrix)) {
        d->minBottom = ColBottomZ(m, d->bb);
        const float* cm = g_GetColModel ? g_GetColModel(pi.model) : nullptr;
        d->startSolid = cm ? ExactColBottomZ(cm, m) : d->minBottom;
    }
    if (g_cfg.fallenPolesBlockCars) { // original CVehicle/CPhysical ignore fallen lamp posts (IsFallenLampPost)
        __try { *(uint32_t*)((uint8_t*)entity + GTA::Entity_ObjectFlags) &= ~GTA::ObjFlag_IsLampPost; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    MakeRootMovable(actor);
    SyncDislodged(entity);
    STAT(dislodged);
    if (ShouldLog(entity, 1))
        Log(1, "dislodged->RenderWare via %s: " PROPFMT " colBottom=%.2f ground=%.2f", t_path, PROPARGS(pi), d->minBottom,
            d->groundZ);
}

// Removes a knocked-over prop exactly like DE's "smash completely" (ObjectDamage case 0x14): no collision,
// invisible, static (CWorld::Process then drops it from the moving list), speeds cleared, UE actor hidden,
// RW object deleted. It comes back when the area streams in again, as smashed props do.
static void RemoveKnockedProp(void* entity, int i) {
    const Dislodged d = g_dislodged[i];
    StopTracking(entity, i);
    uint8_t* e = (uint8_t*)entity;
    *(uint32_t*)(e + GTA::Entity_PhysicalFlags) |= GTA::PhysFlag_SmashedRemoved;
    uint32_t& flags = *(uint32_t*)(e + GTA::Entity_Flags);
    flags = (flags & ~(GTA::Flag_UsesCollision | GTA::Flag_IsStatic | GTA::Flag_IsVisible)) | GTA::Flag_IsStatic;
    memset(e + GTA::Physical_MoveSpeed, 0, 24);
    void** vt = *(void***)entity;
    ((void (*)(void*))vt[GTA::VTable_UpdateActorVisibility / 8])(entity);
    ((void (*)(void*))vt[GTA::VTable_DeleteRwObject / 8])(entity);
    STAT(removedProps);
    Log(1, "removed knocked prop after %lums: model %d", GetTickCount() - d.tick, d.model);
}

static float DisappearSecondsFor(const Dislodged& d) {
    return IsStreetLightActor(d.actor) ? g_cfg.fallenPolesDisappearSeconds : g_cfg.knockedPropsDisappearSeconds;
}

static uintptr_t Hooked_ProcessCollision(void* entity) {
    const bool tracked = g_cfg.rwDislodgedProps && FindDislodged(entity) >= 0;
    void* prev = t_simEntity;
    if (tracked) t_simEntity = entity; // collision for this prop uses its generated spheres
    uintptr_t r = o_ProcessCollision(entity);
    t_simEntity = prev;
    if (!tracked) return r;
    SyncDislodged(entity);
    const int i = FindDislodged(entity);
    if (i >= 0) {
        const float secs = DisappearSecondsFor(g_dislodged[i]);
        if (secs > 0.0f && GetTickCount() - g_dislodged[i].tick >= (DWORD)(secs * 1000.0f) &&
            EntityActor(entity) == g_dislodged[i].actor)
            RemoveKnockedProp(entity, i);
    }
    return r;
}

static uintptr_t Hooked_ProcessShift(void* entity) {
    const bool tracked = g_cfg.rwDislodgedProps && FindDislodged(entity) >= 0;
    void* prev = t_simEntity;
    if (tracked) t_simEntity = entity;
    uintptr_t r = o_ProcessShift(entity);
    t_simEntity = prev;
    if (tracked) SyncDislodged(entity);
    return r;
}

// Street-light props that RenderWare may take over (config, locks and collision permitting).
static bool CanUprootStreetLight(void* entity, void* actor) {
    if (!g_cfg.rwDislodgedProps || !g_cfg.uprootStreetLights || !actor || !IsStreetLightActor(actor)) return false;
    __try {
        uint8_t* e = (uint8_t*)entity;
        if (!(*(uint32_t*)(e + GTA::Entity_Flags) & GTA::Flag_UsesCollision)) return false;
        if (*(uint32_t*)(e + GTA::Entity_PhysicalFlags) & GTA::PhysFlag_DisableCollisionForce) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return RwCollisionCovers(entity, actor);
}

// Original lamp posts / traffic lights are not breakable: they are uprooted and simulated by
// CPhysical (gta-reversed Physical.cpp: SetIsStatic(false) + AddToMovingList). DE marks them BREAKABLE
// and swaps in a PhysX bending pole. For street-light props every ObjectDamage runs as colDamageEffect
// NONE (as in the original), and physical impacts that DE would have broken the pole with uproot it.
static void Hooked_ObjectDamage(void* entity, float damage, float* fxOrigin, float* fxDir, void* damager, int weaponType) {
    t_hit = { entity, damage, {}, {} };
    if (fxOrigin) memcpy(t_hit.pos, fxOrigin, sizeof(t_hit.pos));
    if (damager) {
        __try { memcpy(t_hit.speed, (uint8_t*)damager + GTA::Physical_MoveSpeed, sizeof(t_hit.speed)); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    uint8_t effect = 0;
    float mult = 1.0f;
    void* actor = EntityActor(entity);
    __try {
        effect = ((uint8_t*)entity)[GTA::Entity_ColDamageEffect];
        if (uint8_t* info = *(uint8_t**)((uint8_t*)entity + GTA::Entity_ObjectInfo))
            mult = *(float*)(info + GTA::ObjectInfo_ColDamageMult);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    const bool breakable = effect == GTA::ColDamage_Breakable || effect == GTA::ColDamage_BreakableRemoved;
    const bool convert = breakable && CanUprootStreetLight(entity, actor);

    if (g_cfg.logLevel >= 2 && actor && IsDynamicPropActor(actor) && ShouldLog(entity, 3, 1000)) {
        PropInfo pi;
        ReadPropInfo(entity, actor, pi);
        Log(2, "damage " PROPFMT " dmg=%.1f weapon=%d convert=%d", PROPARGS(pi), damage, weaponType, convert);
    }

    const char* prevPath = t_path;
    t_path = "ObjectDamage";
    if (!convert) {
        void* prevBreak = t_breakEntity;
        t_breakEntity = entity;
        o_ObjectDamage(entity, damage, fxOrigin, fxDir, damager, weaponType);
        t_breakEntity = prevBreak;
        t_path = prevPath;
        return;
    }

    uint8_t* effectPtr = (uint8_t*)entity + GTA::Entity_ColDamageEffect;
    *effectPtr = 0; // COL_DAMAGE_EFFECT_NONE
    void* prevBreak = t_breakEntity;
    t_breakEntity = entity;
    o_ObjectDamage(entity, damage, fxOrigin, fxDir, damager, weaponType);
    t_breakEntity = prevBreak;
    *effectPtr = effect;

    // Like the original, collisions only uproot the pole when they beat its uproot limit: DE's own test in
    // CPhysical::ApplyCollision does that right after this call and passes the car's momentum to the pole.
    // Explosions never reach that test, and a 9999 limit means "never uproot", so handle those here.
    float uprootLimit = 0.0f;
    if (uint8_t* info = (uint8_t*)ReadPtr(entity, GTA::Entity_ObjectInfo)) uprootLimit = *(float*)(info + GTA::ObjectInfo_UprootLimit);
    const bool selfUproot = weaponType == GTA::Weapon_Explosion || uprootLimit >= 9999.0f;
    const uint32_t flags = *(uint32_t*)((uint8_t*)entity + GTA::Entity_Flags);
    if (selfUproot && (flags & GTA::Flag_IsStatic) && damage * mult > 150.0f) {
        UprootEntity(entity);
        STAT(uprootDamage);
    }
    if (ShouldLog(entity, 10, 1000)) {
        PropInfo pi;
        ReadPropInfo(entity, actor, pi);
        Log((flags & GTA::Flag_IsStatic) ? 1 : 2, "street light hit (no shatter): " PROPFMT " dmg=%.0f weapon=%d -> %s", PROPARGS(pi), damage, weaponType,
            selfUproot && damage * mult > 150.0f ? "uprooted by plugin"
                                                 : "RenderWare decides (uproot if impact > limit)");
    }
    t_path = prevPath;
}

// ----------------------------------------------------------------------
// 2. Breakable fragments: original ground plane and lifetime
// ----------------------------------------------------------------------
struct GroundStash { void* actor; float pos[3]; float normal[3]; };
static thread_local GroundStash t_glassGround = {};

// DE routes many props (lamp posts included) through the glass code: CGlass::WindowRespondsToCollision
// calls the Blueprint SetupBroken directly, bypassing ObjectDamage. Street lights that RenderWare can
// simulate are uprooted here instead, exactly where the original game uprooted them.
static uintptr_t Hooked_GlassCollision(void* entity, float damage, float* speed, float* pos) {
    void* actor = entity ? EntityActor(entity) : nullptr;
    if (entity && CanUprootStreetLight(entity, actor)) {
        const bool isStatic = (*(uint32_t*)((uint8_t*)entity + GTA::Entity_Flags) & GTA::Flag_IsStatic) != 0;
        const char* prevPath = t_path;
        t_path = "glass";
        if (isStatic) {
            UprootEntity(entity);
            STAT(uprootGlass);
        }
        t_path = prevPath;
        return 0; // never shatter a street light that RenderWare is simulating
    }

    t_glassGround.actor = nullptr;
    // The probe dereferences the entity matrix without a null check (DE bug), so check it here.
    if (g_cfg.fixDebrisFloor && entity && ReadPtr(entity, GTA::Entity_Matrix)) {
        float p[3] = {}, nrm[3] = { 0, 0, 1 };
        g_GroundProbe(entity, p, nrm); // BreakObject_c::Init ground probe
        t_glassGround = { actor, { p[0], p[1], p[2] }, { nrm[0], nrm[1], nrm[2] } };
    }
    STAT(glassBreak);
    if (actor && IsDynamicPropActor(actor) && ShouldLog(entity, 4)) {
        PropInfo pi;
        ReadPropInfo(entity, actor, pi);
        Log(1, "glass-break (DE) " PROPFMT " dmg=%.1f", PROPARGS(pi), damage);
    }
    const char* prevPath = t_path;
    t_path = "glass";
    void* prevBreak = t_breakEntity;
    t_breakEntity = entity;
    uintptr_t r = o_GlassCollision(entity, damage, speed, pos);
    t_breakEntity = prevBreak;
    t_path = prevPath;
    t_glassGround.actor = nullptr;
    return r;
}

// Breaks a prop through DE's own glass/breakable path (Blueprint SetupBroken + our ground plane).
static void ForceDEBreak(void* entity) {
    float pos[3] = {}, speed[3] = {}, damage = 100.0f;
    if (t_hit.entity == entity) {
        memcpy(pos, t_hit.pos, sizeof(pos));
        memcpy(speed, t_hit.speed, sizeof(speed));
        damage = t_hit.damage;
    } else if (const float* m = (const float*)ReadPtr(entity, GTA::Entity_Matrix)) {
        pos[0] = m[12]; pos[1] = m[13]; pos[2] = m[14];
    }
    t_glassGround.actor = nullptr;
    if (g_cfg.fixDebrisFloor && ReadPtr(entity, GTA::Entity_Matrix)) {
        float p[3] = {}, nrm[3] = { 0, 0, 1 };
        g_GroundProbe(entity, p, nrm);
        t_glassGround = { EntityActor(entity), { p[0], p[1], p[2] }, { nrm[0], nrm[1], nrm[2] } };
    }
    const char* prevPath = t_path;
    t_path = "forced-break";
    void* prevBreak = t_breakEntity;
    t_breakEntity = entity;
    o_GlassCollision(entity, damage, speed, pos);
    t_breakEntity = prevBreak;
    t_path = prevPath;
    t_glassGround.actor = nullptr;
}

// FQuat::FindBetweenNormals(+Z, n)
static FQuat QuatFromUp(float nx, float ny, float nz) {
    float w = 1.0f + nz;
    FQuat q;
    if (w < 1e-6f) { q = { 1, 0, 0, 0 }; return q; }
    q = { -ny, nx, 0.0f, w };
    float len = sqrtf(q.X * q.X + q.Y * q.Y + q.W * q.W);
    q.X /= len; q.Y /= len; q.W /= len;
    return q;
}

static FVec RotateUp(const FQuat& q) {
    // q * (0,0,1)
    return { 2.0f * (q.X * q.Z + q.W * q.Y), 2.0f * (q.Y * q.Z - q.W * q.X), 1.0f - 2.0f * (q.X * q.X + q.Y * q.Y) };
}

static void PatchGlassFloor(void* actor, SetupBrokenParms* p) {
    if (!t_glassGround.actor || t_glassGround.actor != actor) return;
    float* t = p->floorTransform.Translation;
    if (fabsf(t[0]) > 1.0f || fabsf(t[1]) > 1.0f) return; // DE already supplied a floor
    // RenderWare -> Unreal: metres to centimetres, Y flipped (as DE's ObjectDamage does).
    t[0] = t_glassGround.pos[0] * 100.0f;
    t[1] = t_glassGround.pos[1] * -100.0f;
    t[2] = t_glassGround.pos[2] * 100.0f;
    t[3] = 0.0f;
    p->floorTransform.Rotation = QuatFromUp(t_glassGround.normal[0], -t_glassGround.normal[1], t_glassGround.normal[2]);
    float* s = p->floorTransform.Scale3D;
    s[0] = s[1] = s[2] = 1.0f; s[3] = 0.0f;
    Log(2, "glass floor: actor %p ground (%.1f %.1f %.1f)", actor, t[0], t[1], t[2]);
}

static void* GetOutComponent(void* actor, NameId id) {
    uint8_t parms[0x40] = {};
    if (!Call(actor, id, parms)) return nullptr;
    return *(void**)parms;
}

static void* g_breakableFloorMesh = nullptr;

static void FindBreakableFloorMesh() {
    if (g_breakableFloorMesh || !g_objects || g_nameIdx[N_Breakable_Floor] < 0) return;
    __try {
        uint8_t** chunks = *(uint8_t***)g_objects;
        const int32_t num = *(int32_t*)(g_objects + 0x14);
        for (int32_t i = 0; i < num; ++i) {
            uint8_t* chunk = chunks[i / 65536];
            if (!chunk) continue;
            void* obj = *(void**)(chunk + (i % 65536) * 0x18);
            if (!obj || NameOf(obj) != g_nameIdx[N_Breakable_Floor]) continue;
            void* cls = *(void**)((uint8_t*)obj + UE::Obj_Class);
            if (cls && NameOf(cls) == ScanNamePool("StaticMesh")) { g_breakableFloorMesh = obj; return; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void ChannelResponse(void* comp, uint8_t channel, uint8_t response) {
    uint8_t parms[8] = { channel, response };
    Call(comp, N_SetCollisionResponseToChannel, parms);
}

static void SetupGroundPlane(void* actor, const SetupBrokenParms* p) {
    void* floor = GetOutComponent(actor, N_GetPhysicsFloor);
    if (!floor) floor = GetOutComponent(actor, N_GetPhysicsFloorC);
    void* broken = GetOutComponent(actor, N_GetBrokenMesh);

    // Ground point and normal: DE's floorTransform is the original BreakObject_c ground probe.
    FQuat rot = p->floorTransform.Rotation;
    float gx = p->floorTransform.Translation[0], gy = p->floorTransform.Translation[1], gz = p->floorTransform.Translation[2];
    if (fabsf(gx) < 1.0f && fabsf(gy) < 1.0f) {
        // No ground supplied: fall back to the impact point (legacy v0.0.x behaviour).
        gx = p->impulseSrc.X; gy = p->impulseSrc.Y; gz = p->impulseSrc.Z - 80.0f;
        rot = { 0, 0, 0, 1 };
    }
    const float rlen = sqrtf(rot.X * rot.X + rot.Y * rot.Y + rot.Z * rot.Z + rot.W * rot.W);
    if (rlen < 0.5f) rot = { 0, 0, 0, 1 };

    if (floor) {
        void* mesh = ReadPtr(floor, UE::StaticMeshComp_StaticMesh);
        if (!mesh) {
            FindBreakableFloorMesh();
            if (g_breakableFloorMesh) {
                uint8_t parms[0x10] = {};
                *(void**)parms = g_breakableFloorMesh;
                Call(floor, N_SetStaticMesh, parms);
            }
        }
        uint8_t mob[8] = { 2 }; // Movable
        Call(floor, N_SetMobility, mob);
        uint8_t det[8] = { 1, 1, 1, 0 }; // KeepWorld x3
        Call(floor, N_K2_DetachFromComponent, det);

        // 20 cm thick slab whose top face lies on the original ground plane.
        const FVec up = RotateUp(rot);
        alignas(16) uint8_t xf[0xD0] = {};
        FTransform* t = (FTransform*)xf;
        t->Rotation = rot;
        t->Translation[0] = gx - up.X * 10.0f;
        t->Translation[1] = gy - up.Y * 10.0f;
        t->Translation[2] = gz - up.Z * 10.0f;
        t->Scale3D[0] = g_cfg.debrisPlaneSizeM;
        t->Scale3D[1] = g_cfg.debrisPlaneSizeM;
        t->Scale3D[2] = 0.2f;
        xf[0xC0] = 1; // bTeleport
        Call(floor, N_K2_SetWorldTransform, xf);

        // Like the original BreakObject_c plane, only fragments touch it: physics only (invisible to
        // traces), ignore everything, block physics bodies, destructibles and the fragments' own type.
        uint8_t type[8] = { (uint8_t)(g_cfg.originalPieceMotion ? 7 : 0) }; // ECC_Destructible for original pieces, else WorldStatic
        Call(floor, N_SetCollisionObjectType, type);
        uint8_t en[8] = { 2 };   // ECollisionEnabled::PhysicsOnly
        Call(floor, N_SetCollisionEnabled, en);
        uint8_t all[8] = { 0 };  // Ignore
        Call(floor, N_SetCollisionResponseToAllChannels, all);
        ChannelResponse(floor, 5, 2); // PhysicsBody -> Block
        ChannelResponse(floor, 7, 2); // Destructible -> Block
        if (broken) {
            uint8_t ot[8] = { 0xFF };
            if (Call(broken, N_GetCollisionObjectType, ot) && ot[0] != 0xFF && ot[0] != 2 && ot[0] != 6)
                ChannelResponse(floor, ot[0], 2); // fragments' own object type (never Pawn/Vehicle)
            uint8_t ign[0x10] = {};
            *(void**)ign = broken; // bShouldIgnore = false
            Call(floor, N_IgnoreComponentWhenMoving, ign);
        }
    }

    if (broken && g_cfg.debrisCCD) {
        uint8_t ccd[8] = { 1 };
        Call(broken, N_SetAllUseCCD, ccd);
    }
    // Original BreakObject_c pieces stop rotating when they land; PhysX pieces keep spinning without damping.
    if (broken && g_cfg.fragmentAngularDamping > 0.0f && !g_cfg.originalPieceMotion) {
        uint8_t damp[8] = {};
        *(float*)damp = g_cfg.fragmentAngularDamping;
        Call(broken, N_SetAngularDamping, damp);
    }
    uint8_t fragType[8] = { 0xFF };
    if (broken) Call(broken, N_GetCollisionObjectType, fragType);
    if (floor) STAT(floors); else STAT(floorsMissing);
    if (ShouldLog(actor, 5)) {
        char cls[96] = "?";
        if (void* c = ReadPtr(actor, UE::Obj_Class)) NameToStr(NameOf(c), cls, sizeof(cls));
        Log(1, "ground-plane %s: floor=%s fragments=%s fragType=%d ground=(%.0f %.0f %.0f)cm up.z=%.2f", cls,
            floor ? "yes" : "MISSING", broken ? "yes" : "none", fragType[0], gx, gy, gz, RotateUp(rot).Z);
    }
}

// ----------------------------------------------------------------------
// Broken pieces disappear after a configurable delay (original BreakObject_c pieces faded after
// 256-288 frames). Timed actors are validated through GObjects before they are touched.
// ----------------------------------------------------------------------
struct PieceTimer { void* actor; int32_t index; int32_t serial; DWORD due; };
static PieceTimer g_pieceTimers[512];
static int g_pieceTimerCount = 0;
static DWORD g_gameThread = 0;
static bool g_inPieceTimers = false;

static uint8_t* ObjectItem(int32_t index) {
    if (!g_objects || index < 0) return nullptr;
    __try {
        uint8_t** chunks = *(uint8_t***)g_objects;
        if (index >= *(int32_t*)(g_objects + 0x14)) return nullptr;
        uint8_t* chunk = chunks[index / 65536];
        return chunk ? chunk + (index % 65536) * 0x18 : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Same UObject still at that slot, same serial, not being destroyed (PendingKill | Unreachable).
static bool UObjectAlive(void* obj, int32_t index, int32_t serial) {
    uint8_t* item = ObjectItem(index);
    if (!item) return false;
    __try {
        return *(void**)item == obj && *(int32_t*)(item + 0x10) == serial && !(*(int32_t*)(item + 8) & ((1 << 29) | (1 << 28)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void SchedulePieces(void* actor) {
    if (g_cfg.debrisMode != DEBRIS_ORIGINAL || g_cfg.piecesDisappearSeconds <= 0.0f) return;
    int32_t index = -1, serial = 0;
    __try { index = *(int32_t*)((uint8_t*)actor + UE::Obj_Index); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    uint8_t* item = ObjectItem(index);
    if (!item || ReadPtr(item, 0) != actor) return;
    serial = *(int32_t*)(item + 0x10);
    g_gameThread = GetCurrentThreadId();
    const DWORD due = GetTickCount() + (DWORD)(g_cfg.piecesDisappearSeconds * 1000.0f);
    for (int i = 0; i < g_pieceTimerCount; ++i)
        if (g_pieceTimers[i].actor == actor) { g_pieceTimers[i] = { actor, index, serial, due }; return; }
    if (g_pieceTimerCount == (int)(sizeof(g_pieceTimers) / sizeof(g_pieceTimers[0]))) g_pieceTimers[0] = g_pieceTimers[--g_pieceTimerCount];
    g_pieceTimers[g_pieceTimerCount++] = { actor, index, serial, due };
}

static void CancelPieces(void* actor) {
    for (int i = 0; i < g_pieceTimerCount; ++i)
        if (g_pieceTimers[i].actor == actor) { g_pieceTimers[i] = g_pieceTimers[--g_pieceTimerCount]; return; }
}

static void ExpirePieces(void* actor) {
    // Blueprint HideBroken where the class has it; otherwise hide the broken mesh directly.
    const bool hidden = Call(actor, N_HideBroken, nullptr);
    if (!hidden) {
        if (void* broken = GetOutComponent(actor, N_GetBrokenMesh)) {
            uint8_t sim[8] = { 0 };
            Call(broken, N_SetSimulatePhysics, sim);
            uint8_t col[8] = { 0 }; // NoCollision
            Call(broken, N_SetCollisionEnabled, col);
            uint8_t hide[8] = { 1, 1 }; // NewHidden, bPropagateToChildren
            Call(broken, N_SetHiddenInGame, hide);
        }
    }
    Call(actor, N_RemoveFloor, nullptr); // drop the ground plane with the pieces (unhooked call)
    STAT(removeHide);
    if (ShouldLog(actor, 12, 1000)) {
        char cls[96] = "?";
        if (void* c = ReadPtr(actor, UE::Obj_Class)) NameToStr(NameOf(c), cls, sizeof(cls));
        Log(1, "pieces removed after %.1fs: %s (%s)", g_cfg.piecesDisappearSeconds, cls, hidden ? "HideBroken" : "mesh hidden");
    }
}

static void ProcessPieceTimers() {
    g_inPieceTimers = true;
    const DWORD now = GetTickCount();
    for (int i = 0; i < g_pieceTimerCount;) {
        PieceTimer t = g_pieceTimers[i];
        if ((LONG)(now - t.due) < 0) { ++i; continue; }
        g_pieceTimers[i] = g_pieceTimers[--g_pieceTimerCount];
        if (UObjectAlive(t.actor, t.index, t.serial)) ExpirePieces(t.actor);
    }
    g_inPieceTimers = false;
}

// ----------------------------------------------------------------------
// Original piece system (gta-reversed BreakObject_c, 0x59D190-0x59E750) driving DE's piece bodies.
// The original drew RenderWare triangles (BreakablePlugin); DE renders through Unreal, so its broken
// mesh bones are the visible pieces. Each physics body becomes one original break group:
//   SetGroupData: v = object.dat break velocity + rand(+-r) per axis, spin 3..6 deg/step, random axis
//   Update:       v.z -= step/125 (2 g); spin for 5 steps, then turn the flattest axis to the ground normal
//                 at angle*step/20; DoCollision against the probed ground plane
//   DoCollisionResponse: reflect with 0.85, random +-0.05*step, keep speed, scale 0.8; stop below 0.05
//   Lifetime:     each group lives on its own and is hidden when it runs out
//   Render:       alpha = FramesToLive*2 (fade over the last 127.5 steps)
// Ground contact is measured on the piece's real collision (GetClosestPointOnCollision), not its bone:
// DE's piece bones pivot anywhere on the piece.
// RenderWare units: metres, 1 step = 1/50 s. Unreal: centimetres, Y mirrored.
// ----------------------------------------------------------------------
struct PieceGroup {
    FName bone;
    float vel[3];      // RenderWare axes, m/step
    float axis[3];     // spin axis (Unreal axes)
    float rotSpeed;    // degrees per step
    float life;        // steps left
    float cmdVn;       // velocity sent into the ground plane last update (cm/s, <0 = towards it)
    float sinkSpeed;   // fade: cm/s down through the ground so the piece is under it when its life ends
    int8_t flat;       // local axis (0 x, 1 y, 2 z) the piece is thinnest along; -1 = unmeasured
    bool stopped, hidden;
};
struct PieceSet {
    void* actor; int32_t actorIdx, actorSerial;
    void* mesh;  int32_t meshIdx, meshSerial;
    float ground[3];   // plane point (Unreal cm)
    float normal[3];   // plane normal (Unreal)
    float steps;       // steps since the break (m_FramesActive)
    float alpha;       // last AGTAActor::SetAlpha value
    float nextTrace;   // diagnostics: next step count to log the first groups at (0 = off)
    bool sinking;      // fade started: the pieces ignore the ground plane and slide under the ground
    int n;
    PieceGroup g[48];
};
static PieceSet* g_pieceSets[64];
static int g_pieceSetCount = 0;
static DWORD g_pieceLastTick = 0;
static uint32_t g_rng = 0x2545F491u;
static float Rand01() { g_rng = g_rng * 1664525u + 1013904223u; return (g_rng >> 8) * (1.0f / 16777216.0f); }
static float RandRange(float a, float b) { return a + (b - a) * Rand01(); }

static bool ObjAliveBySlot(void* obj, int32_t idx, int32_t serial) { return obj && UObjectAlive(obj, idx, serial); }
static bool SlotOf(void* obj, int32_t& idx, int32_t& serial) {
    idx = -1; serial = 0;
    __try { idx = *(int32_t*)((uint8_t*)obj + UE::Obj_Index); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    uint8_t* item = ObjectItem(idx);
    if (!item || ReadPtr(item, 0) != obj) return false;
    serial = *(int32_t*)(item + 0x10);
    return true;
}

struct BoneParms { FName bone; uint8_t rest[0x38]; };
static bool BoneLocation(void* mesh, FName bone, float out[3]) {
    alignas(16) uint8_t p[0x40] = {};
    *(FName*)p = bone;
    if (!Call(mesh, N_GetSocketLocation, p)) return false;
    memcpy(out, p + 0x08, 12);
    return true;
}
static bool BoneQuat(void* mesh, FName bone, FQuat& q) {
    alignas(16) uint8_t p[0x40] = {};
    *(FName*)p = bone;
    if (!Call(mesh, N_GetSocketQuaternion, p)) return false;
    memcpy(&q, p + 0x10, 16);
    return true;
}
static void SetBoneVelocity(void* mesh, FName bone, const float v[3], NameId fn) {
    alignas(16) uint8_t p[0x40] = {};
    memcpy(p, v, 12);           // NewVel / NewAngVel
    p[0x0C] = 0;                // bAddToCurrent
    *(FName*)(p + 0x10) = bone; // BoneName
    Call(mesh, fn, p);
}
static void BoneCall(void* mesh, NameId fn, FName bone) {
    alignas(16) uint8_t p[0x40] = {};
    *(FName*)p = bone;
    Call(mesh, fn, p);
}
static void BodyGravity(void* mesh, FName bone, bool on) {
    alignas(8) uint8_t p[0x10] = {};
    p[0] = on; *(FName*)(p + 4) = bone; // bEnableGravity, BoneName
    Call(mesh, N_SetEnableBodyGravity, p);
}

// Rotates `v` by quaternion q.
static void QuatRotate(const FQuat& q, const float v[3], float out[3]) {
    const float tx = 2 * (q.Y * v[2] - q.Z * v[1]), ty = 2 * (q.Z * v[0] - q.X * v[2]), tz = 2 * (q.X * v[1] - q.Y * v[0]);
    out[0] = v[0] + q.W * tx + (q.Y * tz - q.Z * ty);
    out[1] = v[1] + q.W * ty + (q.Z * tx - q.X * tz);
    out[2] = v[2] + q.W * tz + (q.X * ty - q.Y * tx);
}

// Closest point on the piece's collision to `pt`. False when the body has no usable shape (returns -1).
static bool ClosestOnPiece(void* mesh, FName bone, const float pt[3], float out[3]) {
    alignas(16) uint8_t p[0x30] = {};
    memcpy(p, pt, 12);                                       // Point
    *(FName*)(p + 0x18) = bone;                              // BoneName
    if (!Call(mesh, N_GetClosestPointOnCollision, p) || *(float*)(p + 0x20) <= 0.0f) return false;
    memcpy(out, p + 0x0C, 12);                               // OutPointOnBody
    return true;
}

// Height (cm) above the ground plane of the piece's lowest (dir = -1) or highest (dir = +1) collision point.
// The closest point on the body to a point 1 km beyond it along the normal is its extreme point that way
// (error <= size^2 / 2 km, 3 cm for an 8 m pole).
static bool PieceExtent(const PieceSet* s, FName bone, float dir, float& h) {
    float loc[3], pt[3], q[3];
    if (!BoneLocation(s->mesh, bone, loc)) return false;
    for (int k = 0; k < 3; ++k) pt[k] = loc[k] + s->normal[k] * dir * 100000.0f;
    if (!ClosestOnPiece(s->mesh, bone, pt, q)) return false;
    h = (q[0] - s->ground[0]) * s->normal[0] + (q[1] - s->ground[1]) * s->normal[1] + (q[2] - s->ground[2]) * s->normal[2];
    return true;
}

// BreakObject_c::CalcGroupCenter: the local axis along which the piece is thinnest (its flat face normal),
// measured on the piece's collision. An upright pole piece gets a horizontal axis, so it lies down.
static int8_t FlattestAxis(void* mesh, FName bone) {
    float loc[3];
    FQuat q;
    if (!BoneLocation(mesh, bone, loc) || !BoneQuat(mesh, bone, q)) return -1;
    int8_t best = -1;
    float bestSize = 1e9f;
    for (int8_t k = 0; k < 3; ++k) {
        float a[3] = {}, w[3];
        a[k] = 1.0f;
        QuatRotate(q, a, w);
        float size = 0.0f;
        for (float sgn = -1.0f; sgn <= 1.0f; sgn += 2.0f) {
            float pt[3], c[3];
            for (int j = 0; j < 3; ++j) pt[j] = loc[j] + w[j] * sgn * 100000.0f;
            if (!ClosestOnPiece(mesh, bone, pt, c)) return -1;
            size += sgn * ((c[0] - loc[0]) * w[0] + (c[1] - loc[1]) * w[1] + (c[2] - loc[2]) * w[2]);
        }
        if (size < bestSize) { bestSize = size; best = k; }
    }
    return best;
}

// SetupBroken just ran: take over the Blueprint's pieces. Returns false when the prop has no pieces to drive.
static bool InitOriginalPieces(void* actor, const SetupBrokenParms* p) {
    if (!g_cfg.originalPieceMotion) return false;
    void* mesh = GetOutComponent(actor, N_GetBrokenMesh);
    if (!mesh) return false;
    auto* s = (PieceSet*)calloc(1, sizeof(PieceSet));
    if (!s) return false;
    s->alpha = 1.0f;
    s->actor = actor; s->mesh = mesh;
    if (!SlotOf(actor, s->actorIdx, s->actorSerial) || !SlotOf(mesh, s->meshIdx, s->meshSerial)) { free(s); return false; }

    // Ground plane = DE's floorTransform (the original BreakObject_c::Init vertical probe).
    FQuat rot = p->floorTransform.Rotation;
    s->ground[0] = p->floorTransform.Translation[0]; s->ground[1] = p->floorTransform.Translation[1]; s->ground[2] = p->floorTransform.Translation[2];
    if (fabsf(s->ground[0]) < 1.0f && fabsf(s->ground[1]) < 1.0f) {
        s->ground[0] = p->impulseSrc.X; s->ground[1] = p->impulseSrc.Y; s->ground[2] = p->impulseSrc.Z - 80.0f;
        rot = { 0, 0, 0, 1 };
    }
    if (rot.X * rot.X + rot.Y * rot.Y + rot.Z * rot.Z + rot.W * rot.W < 0.25f) rot = { 0, 0, 0, 1 };
    const FVec up = RotateUp(rot);
    s->normal[0] = up.X; s->normal[1] = up.Y; s->normal[2] = up.Z;

    // object.dat break data of the RW object being broken (m_vecBreakVelocity, m_fBreakVelocityRand).
    float bv[3] = { 0.0f, 0.0f, 0.1f }, br = 0.07f; // lamp post / traffic light values
    if (t_breakEntity && EntityActor(t_breakEntity) == actor) {
        if (uint8_t* info = (uint8_t*)ReadPtr(t_breakEntity, GTA::Entity_ObjectInfo)) {
            __try {
                memcpy(bv, info + GTA::ObjectInfo_BreakVelocity, 12);
                br = *(float*)(info + GTA::ObjectInfo_BreakVelocityRand);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    // Independent pieces like the original groups: break every joint of the physics asset by its own joint
    // name. Breaking only joints named after the simulated bones left DE's lamp post pieces jointed together.
    int joints = 0;
    for (int c = 0; c < 256; ++c) {
        alignas(8) uint8_t fc[0x10] = {};
        *(int32_t*)fc = c;
        if (!Call(mesh, N_FindConstraintBoneName, fc)) break;
        const FName joint = *(FName*)(fc + 4);
        if (joint.Index == 0 && joint.Number == 0) break; // NAME_None: past the last constraint
        alignas(8) uint8_t bc[0x30] = {};
        *(FName*)(bc + 0x18) = joint;                     // zero impulse: the algorithm sets the velocity
        Call(mesh, N_BreakConstraint, bc);
        ++joints;
    }

    alignas(8) uint8_t nb[0x10] = {};
    Call(mesh, N_GetNumBones, nb);
    const int numBones = *(int32_t*)nb;
    const float life = fmaxf(0.5f, g_cfg.piecesDisappearSeconds > 0.0f ? g_cfg.piecesDisappearSeconds : 5.12f) * 50.0f;
    for (int i = 0; i < numBones && s->n < (int)(sizeof(s->g) / sizeof(s->g[0])); ++i) {
        alignas(8) uint8_t bn[0x10] = {};
        *(int32_t*)bn = i;
        Call(mesh, N_GetBoneName, bn);
        const FName bone = *(FName*)(bn + 4);
        alignas(8) uint8_t sim[0x10] = {};
        *(FName*)sim = bone;
        Call(mesh, N_IsSimulatingPhysics, sim);
        if (!sim[8]) continue; // bone without its own physics body
        PieceGroup& g = s->g[s->n++];
        g.bone = bone;
        for (int k = 0; k < 3; ++k) g.vel[k] = bv[k] + (br != 0.0f ? RandRange(-br, br) : 0.0f);
        g.rotSpeed = RandRange(3.0f, 6.0f);
        float ax[3] = { RandRange(-1, 1), RandRange(-1, 1), RandRange(-1, 1) };
        const float len = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
        for (int k = 0; k < 3; ++k) g.axis[k] = len > 1e-4f ? ax[k] / len : (k == 2 ? 1.0f : 0.0f);
        g.life = life + RandRange(0.0f, 32.0f); // 256 + rand(32) frames in the original
        g.flat = FlattestAxis(mesh, bone);
        BodyGravity(mesh, bone, false);         // gravity comes from the algorithm
    }
    if (!s->n) { free(s); return false; }
    // Original pieces only ever touched their ground plane: ignore cars, peds, props and each other; block only
    // the plugin's ground planes (object type Destructible, see SetupGroundPlane).
    uint8_t ignoreAll[8] = { 0 };
    Call(mesh, N_SetCollisionResponseToAllChannels, ignoreAll);
    ChannelResponse(mesh, 7, 2);
    if (g_pieceSetCount == (int)(sizeof(g_pieceSets) / sizeof(g_pieceSets[0]))) { free(g_pieceSets[0]); g_pieceSets[0] = g_pieceSets[--g_pieceSetCount]; }
    g_pieceSets[g_pieceSetCount++] = s;
    g_gameThread = GetCurrentThreadId();
    // Diagnostics for the log: can the real collision be measured, where do the pieces start, which materials
    // (the fade relies on them reading AGTAActor::SetAlpha's Custom Primitive Data 6).
    int measured = 0;
    float lowest = 1e9f, highest = -1e9f;
    for (int i = 0; i < s->n; ++i) {
        float h;
        if (PieceExtent(s, s->g[i].bone, -1.0f, h)) { ++measured; lowest = fminf(lowest, h); highest = fmaxf(highest, h); }
    }
    Log(1, "original pieces: %d groups, %d joints broken, break velocity (%.2f %.2f %.2f) +-%.2f m/step, ground z=%.0fcm, "
           "contact sensor=%s (%d/%d), piece bottoms %.0f..%.0fcm above ground", s->n, joints, bv[0], bv[1], bv[2], br, s->ground[2],
        measured == s->n ? "collision" : measured ? "mixed" : "velocity", measured, s->n, measured ? lowest : 0.0f, measured ? highest : 0.0f);
    if (ShouldLog(ReadPtr(actor, UE::Obj_Class), 8, 60000)) {
        s->nextTrace = 10.0f; // trace the first groups of this class for a few seconds
        alignas(8) uint8_t nm[0x10] = {};
        Call(mesh, N_GetNumMaterials, nm);
        char line[512] = "", mat[96];
        for (int i = 0; i < *(int32_t*)nm && i < 6; ++i) {
            alignas(8) uint8_t gm[0x10] = {};
            *(int32_t*)gm = i;
            Call(mesh, N_GetMaterial, gm);
            void* m = *(void**)(gm + 8);
            if (m) NameToStr(NameOf(m), mat, sizeof(mat)); else strcpy_s(mat, "none");
            strcat_s(line, mat); strcat_s(line, " ");
        }
        Log(1, "  piece materials (%d): %s", *(int32_t*)nm, line);
    }
    return true;
}

// Fade start (BreakObject_c::Render fades over the last 127.5 steps). DE's prop materials do not show
// AGTAActor::SetAlpha, so the pieces also stop touching the ground plane and slide under the ground. HideBoneByName
// also hides a bone's children, so every piece is under the ground by the time the first one hides.
static void StartSinking(PieceSet* s) {
    s->sinking = true;
    ChannelResponse(s->mesh, 7, 0); // ignore the ground plane (the only thing the pieces touch)
    float deepest = 0.0f, minLife = 1e9f;
    for (int i = 0; i < s->n; ++i) if (!s->g[i].hidden) minLife = fminf(minLife, s->g[i].life);
    for (int i = 0; i < s->n; ++i) {
        PieceGroup& g = s->g[i];
        if (g.hidden) continue;
        float top;
        if (!PieceExtent(s, g.bone, 1.0f, top)) { // unknown size: assume it reaches 1 m above its pivot
            float loc[3] = {};
            BoneLocation(s->mesh, g.bone, loc);
            top = (loc[0] - s->ground[0]) * s->normal[0] + (loc[1] - s->ground[1]) * s->normal[1] + (loc[2] - s->ground[2]) * s->normal[2] + 100.0f;
        }
        const float depth = fmaxf(top, 0.0f) + 5.0f;
        deepest = fmaxf(deepest, depth);
        g.sinkSpeed = depth / fmaxf(minLife, 1.0f) * 50.0f; // cm per remaining step -> cm/s
        if (g.stopped) BodyGravity(s->mesh, g.bone, false); // the sink speed alone moves it
    }
    Log(s->nextTrace > 0.0f ? 1 : 2, "  fade: %d groups sink under the ground (up to %.0fcm) over %.1fs", s->n, deepest,
        g_cfg.piecesFadeSeconds);
}

// One BreakObject_c::Update for a set; returns false when every group is gone.
static bool UpdatePieceSet(PieceSet* s, float step) {
    const float nRW[3] = { s->normal[0], -s->normal[1], s->normal[2] };
    const float fadeSteps = g_cfg.piecesFadeSeconds > 0.0f ? fmaxf(1.0f, g_cfg.piecesFadeSeconds * 50.0f) : 0.0f;
    if (!s->sinking && fadeSteps > 0.0f) {
        float minLife = 1e9f;
        for (int i = 0; i < s->n; ++i) if (!s->g[i].hidden) minLife = fminf(minLife, s->g[i].life);
        if (minLife <= fadeSteps) StartSinking(s);
    }
    const bool trace = s->nextTrace > 0.0f && s->steps >= s->nextTrace;
    if (trace) s->nextTrace = s->nextTrace < 200.0f ? s->nextTrace * 2.0f : 0.0f;
    bool anyLeft = false;
    for (int i = 0; i < s->n; ++i) {
        PieceGroup& g = s->g[i];
        if (g.hidden) continue;
        g.life -= step;
        if (g.life <= 0.0f) { // out of frames: faded out
            const float zero[3] = {};
            SetBoneVelocity(s->mesh, g.bone, zero, N_SetPhysicsLinearVelocity);
            SetBoneVelocity(s->mesh, g.bone, zero, N_SetPhysicsAngularVelocityInDegrees);
            alignas(8) uint8_t hb[0x10] = {};
            *(FName*)hb = g.bone; hb[8] = 0; // PBO_None
            Call(s->mesh, N_HideBoneByName, hb);
            BoneCall(s->mesh, N_PutRigidBodyToSleep, g.bone);
            g.hidden = true;
            continue;
        }
        anyLeft = true;
        if (s->sinking) {
            const float down[3] = { -s->normal[0] * g.sinkSpeed, -s->normal[1] * g.sinkSpeed, -s->normal[2] * g.sinkSpeed };
            const float zero[3] = {};
            SetBoneVelocity(s->mesh, g.bone, down, N_SetPhysicsLinearVelocity);
            SetBoneVelocity(s->mesh, g.bone, zero, N_SetPhysicsAngularVelocityInDegrees);
            continue;
        }

        // Lowest point of the piece's real collision above the ground plane.
        if (g.stopped && !(trace && i < 4)) continue;
        float h = 0.0f;
        const bool measured = PieceExtent(s, g.bone, -1.0f, h);
        if (trace && i < 4)
            Log(1, "  t=%.1fs group %d: bottom %s%.0fcm above ground, vel (%.3f %.3f %.3f) m/step%s", s->steps / 50.0f, i,
                measured ? "" : "unmeasured ", h, g.vel[0], g.vel[1], g.vel[2], g.stopped ? ", stopped" : "");
        if (g.stopped) continue;
        g.vel[2] -= step / 125.0f; // gravity
        float ang[3] = {};
        if (s->steps < 5.0f) {     // initial tumble
            for (int k = 0; k < 3; ++k) ang[k] = g.axis[k] * g.rotSpeed * 50.0f;
        } else {                    // turn the piece's flat face (thinnest axis) towards the ground normal
            FQuat q;
            if (BoneQuat(s->mesh, g.bone, q)) {
                float best[3] = {}, bestDot = -1.0f;
                for (int8_t k = 0; k < 3; ++k) {
                    if (g.flat >= 0 && k != g.flat) continue; // unmeasured: the axis nearest the normal
                    float a[3] = {}, w[3];
                    a[k] = 1.0f;
                    QuatRotate(q, a, w);
                    float d = w[0] * s->normal[0] + w[1] * s->normal[1] + w[2] * s->normal[2];
                    if (fabsf(d) > bestDot) {
                        bestDot = fabsf(d);
                        for (int j = 0; j < 3; ++j) best[j] = d < 0 ? -w[j] : w[j];
                    }
                }
                const float angle = acosf(fminf(1.0f, bestDot));
                if (angle > 0.01f) {
                    float c[3] = { best[1] * s->normal[2] - best[2] * s->normal[1], best[2] * s->normal[0] - best[0] * s->normal[2],
                                   best[0] * s->normal[1] - best[1] * s->normal[0] };
                    const float cl = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
                    const float degPerSec = angle * 57.29578f * 50.0f / 20.0f; // angle*step/20 per step
                    if (cl > 1e-5f) for (int k = 0; k < 3; ++k) ang[k] = c[k] / cl * degPerSec;
                }
            }
        }

        // DoCollision: the piece reaches the ground plane during this update. Without a measurable
        // collision shape, fall back to the plane as sensor: PhysX stopped most of the speed sent into it.
        const float vn = g.vel[0] * nRW[0] + g.vel[1] * nRW[1] + g.vel[2] * nRW[2];
        bool contact = false;
        if (measured) {
            contact = h + vn * step * 100.0f <= 1.0f;
        } else if (g.cmdVn < -30.0f) {
            alignas(8) uint8_t lv[0x20] = {};
            *(FName*)lv = g.bone;
            if (Call(s->mesh, N_GetPhysicsLinearVelocity, lv)) {
                const float* a = (const float*)(lv + 8);
                const float actualVn = a[0] * s->normal[0] + a[1] * s->normal[1] + a[2] * s->normal[2];
                contact = actualVn > g.cmdVn * 0.5f;
            }
        }
        if (contact && vn < 0.0f) { // DoCollisionResponse
            float nv[3];
            for (int k = 0; k < 3; ++k) nv[k] = g.vel[k] - nRW[k] * (vn * 0.85f * 2.0f);
            const float speed = sqrtf(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
            float r[3] = { RandRange(-1, 1), RandRange(-1, 1), RandRange(-1, 1) };
            const float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
            for (int k = 0; k < 3; ++k) nv[k] += rl > 1e-4f ? r[k] / rl * step * 0.05f : 0.0f;
            const float nl = sqrtf(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
            for (int k = 0; k < 3; ++k) g.vel[k] = nl > 1e-6f ? nv[k] / nl * speed * 0.8f : 0.0f;
            g.rotSpeed = 0.0f;
            if (speed < 0.05f) { // m_bStoppedMoving
                // PhysX lays the piece down on the plane (a tilted piece tips over flat instead of resting
                // on one corner) and sleeps it; nothing can be left hanging in the air.
                g.stopped = true;
                g.cmdVn = 0.0f;
                const float zero[3] = {};
                SetBoneVelocity(s->mesh, g.bone, zero, N_SetPhysicsLinearVelocity);
                SetBoneVelocity(s->mesh, g.bone, zero, N_SetPhysicsAngularVelocityInDegrees);
                BodyGravity(s->mesh, g.bone, true);
                continue;
            }
        }
        const float vUE[3] = { g.vel[0] * 5000.0f, -g.vel[1] * 5000.0f, g.vel[2] * 5000.0f }; // m/step -> cm/s
        g.cmdVn = vUE[0] * s->normal[0] + vUE[1] * s->normal[1] + vUE[2] * s->normal[2];
        SetBoneVelocity(s->mesh, g.bone, vUE, N_SetPhysicsLinearVelocity);
        SetBoneVelocity(s->mesh, g.bone, ang, N_SetPhysicsAngularVelocityInDegrees);
    }
    s->steps += step;

    // BreakObject_c::Render: alpha = FramesToLive * 2 (opaque until the last ~128 steps). DE fades a whole
    // actor through AGTAActor::SetAlpha (0..1); the pieces of one break die within 32 steps of each other.
    float maxLife = 0.0f;
    for (int i = 0; i < s->n; ++i) if (!s->g[i].hidden) maxLife = fmaxf(maxLife, s->g[i].life);
    const float alpha = fadeSteps > 0.0f ? fminf(1.0f, fmaxf(0.0f, maxLife / fadeSteps)) : 1.0f;
    if (anyLeft && fabsf(alpha - s->alpha) >= 0.01f) {
        alignas(8) uint8_t pa[0x10] = {};
        *(float*)pa = alpha;
        if (Call(s->actor, N_SetAlpha, pa)) s->alpha = alpha;
    }
    return anyLeft;
}

static bool g_inPieceTick = false;
static void TickOriginalPieces() {
    const DWORD now = GetTickCount();
    const DWORD elapsed = now - g_pieceLastTick;
    if (elapsed < 8) return; // at most ~120 updates per second
    g_pieceLastTick = now;
    const float step = fminf((float)elapsed, 100.0f) * 0.05f; // ms -> 1/50 s steps (capped)
    g_inPieceTick = true;
    for (int i = 0; i < g_pieceSetCount;) {
        PieceSet* s = g_pieceSets[i];
        const bool alive = ObjAliveBySlot(s->actor, s->actorIdx, s->actorSerial) && ObjAliveBySlot(s->mesh, s->meshIdx, s->meshSerial);
        const bool left = alive && UpdatePieceSet(s, step);
        if (left) { ++i; continue; }
        if (alive) {
            Call(s->actor, N_RemoveFloor, nullptr); // every group gone: drop the ground plane too
            alignas(8) uint8_t pa[0x10] = {};
            *(float*)pa = 1.0f;                     // back to opaque for when the prop is restored
            Call(s->actor, N_SetAlpha, pa);
            STAT(removeHide);
            Log(1, "original pieces finished (%d groups) after %.1fs", s->n, s->steps / 50.0f);
        }
        free(s);
        g_pieceSets[i] = g_pieceSets[--g_pieceSetCount];
    }
    g_inPieceTick = false;
}

static void CancelPieceSets(void* actor) {
    for (int i = 0; i < g_pieceSetCount; ++i)
        if (g_pieceSets[i]->actor == actor) {
            if (g_pieceSets[i]->alpha < 1.0f) {
                alignas(8) uint8_t pa[0x10] = {};
                *(float*)pa = 1.0f;
                Call(actor, N_SetAlpha, pa);
            }
            free(g_pieceSets[i]);
            g_pieceSets[i] = g_pieceSets[--g_pieceSetCount];
            return;
        }
}

// ----------------------------------------------------------------------
// ProcessEvent hook
// ----------------------------------------------------------------------
static void Hooked_ProcessEvent(void* obj, void* func, void* parms) {
    if ((g_pieceTimerCount || g_pieceSetCount) && GetCurrentThreadId() == g_gameThread) {
        if (g_pieceTimerCount && !g_inPieceTimers) ProcessPieceTimers();
        if (g_pieceSetCount && !g_inPieceTick) TickOriginalPieces();
    }
    if (!obj || !func) { o_ProcessEvent(obj, func, parms); return; }
    const int32_t name = NameOf(func);

    if (name == g_nameIdx[N_Dislodged]) {
        if (t_suppressDislodgedFor == obj) return; // RenderWare keeps simulating the prop; no UE PhysX hand-off
        if (IsDynamicPropActor(obj) && ShouldLog(obj, 6)) {
            char cls[96] = "?";
            if (void* c = ReadPtr(obj, UE::Obj_Class)) NameToStr(NameOf(c), cls, sizeof(cls));
            Log(1, "Dislodged event reached the Blueprint (UE PhysX) for %s via %s", cls, t_path);
        }
    }

    if (name == g_nameIdx[N_SetupBroken] && parms) {
        auto* p = (SetupBrokenParms*)parms;
        if (g_cfg.fixDebrisFloor) PatchGlassFloor(obj, p);
        o_ProcessEvent(obj, func, parms);
        STAT(setupBroken);
        if (ShouldLog(obj, 7)) {
            char cls[96] = "?";
            if (void* c = ReadPtr(obj, UE::Obj_Class)) NameToStr(NameOf(c), cls, sizeof(cls));
            Log(1, "broken (DE Blueprint) %s via %s: ok=%d impulse=(%.0f %.0f %.0f)%s", cls, t_path, p->ReturnValue,
                p->impulseVelocity.X, p->impulseVelocity.Y, p->impulseVelocity.Z,
                IsStreetLightActor(obj) ? "  [street light shattered]" : "");
        }
        if (g_cfg.fixDebrisFloor && p->ReturnValue && IsDynamicPropActor(obj)) SetupGroundPlane(obj, p);
        if (p->ReturnValue && IsDynamicPropActor(obj) && !InitOriginalPieces(obj, p)) SchedulePieces(obj);
        return;
    }

    if (name == g_nameIdx[N_RemoveFloor] && g_cfg.fixDebrisFloor) {
        Log(2, "RemoveFloor on %p -> %s", obj, g_cfg.debrisMode == DEBRIS_KEEP ? "kept" : g_cfg.debrisMode == DEBRIS_ORIGINAL ? "timed" : "stock");
        if (g_cfg.debrisMode == DEBRIS_KEEP) { STAT(removeKeep); return; }
        if (g_cfg.debrisMode == DEBRIS_ORIGINAL) {
            // The Blueprint's own 6 s timer would drop the pieces through the map: the plugin's timer
            // removes them (and then the floor) instead. Without a timer, fall back to hiding them now.
            if (g_cfg.piecesDisappearSeconds > 0.0f || g_cfg.originalPieceMotion) { STAT(removeKeep); return; }
            Call(obj, N_HideBroken, nullptr);
            STAT(removeHide);
        } else {
            STAT(removeStock);
        }
        o_ProcessEvent(obj, func, parms);
        return;
    }

    if (name == g_nameIdx[N_EntityLinked]) { g_lightsOff.Remove(obj); CancelPieces(obj); CancelPieceSets(obj); }
    // A fallen street light stays dark when DE switches lights on at dusk.
    if (name == g_nameIdx[N_SetLights] && parms && *(uint8_t*)parms && g_lightsOff.Has(obj)) *(uint8_t*)parms = 0;

    o_ProcessEvent(obj, func, parms);
}

// ----------------------------------------------------------------------
// Background initialisation: names and PhysX settings
// ----------------------------------------------------------------------
static bool ConfigureSubstepping() {
    if (!g_objects || g_nameIdx[N_Default__PhysicsSettings] < 0 || g_nameIdx[N_PhysicsSettings] < 0) return false;
    __try {
        uint8_t** chunks = *(uint8_t***)g_objects;
        const int32_t num = *(int32_t*)(g_objects + 0x14);
        for (int32_t i = 0; i < num; ++i) {
            uint8_t* chunk = chunks[i / 65536];
            if (!chunk) continue;
            uint8_t* obj = *(uint8_t**)(chunk + (i % 65536) * 0x18);
            if (!obj || NameOf(obj) != g_nameIdx[N_Default__PhysicsSettings]) continue;
            void* cls = *(void**)(obj + UE::Obj_Class);
            if (!cls || NameOf(cls) != g_nameIdx[N_PhysicsSettings]) continue;
            obj[UE::PhysSettings_bSubstepping] = 1;
            *(float*)(obj + UE::PhysSettings_MaxSubstepDeltaTime) = 1.0f / 60.0f;
            *(int32_t*)(obj + UE::PhysSettings_MaxSubsteps) = 4;
            Log(1, "PhysicsSettings %p: substepping enabled", obj);
            return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static void LogStats() {
    static Stats last = {};
    Stats now;
    memcpy(&now, (const void*)&g_stats, sizeof(now));
    now.syncs = last.syncs; // per-frame counter alone does not trigger a summary
    if (!memcmp(&now, &last, sizeof(now))) return;
    now.syncs = g_stats.syncs;
    Log(1, "stats: dislodged->RW=%ld dislodge->stock=%ld rested=%ld uproot(damage)=%ld uproot(glass)=%ld "
           "DE-break-kept=%ld glass-break=%ld SetupBroken=%ld planes=%ld planes-missing=%ld "
           "RemoveFloor(hide/keep/stock)=%ld/%ld/%ld removedProps=%ld lightsOff=%ld syncs=%ld tracked=%d log-suppressed=%ld",
        now.dislodged, now.dislodgeStock, now.rested, now.uprootDamage, now.uprootGlass, now.breakKept, now.glassBreak,
        now.setupBroken, now.floors, now.floorsMissing, now.removeHide, now.removeKeep, now.removeStock, now.removedProps, now.lightsOff,
        now.syncs, g_dislodgedCount, g_suppressed);
    last = now;
}

static DWORD WINAPI InitThread(LPVOID) {
    bool names = false, physx = !g_cfg.substepping;
    for (int i = 0; i < 600 && !(names && physx); ++i) { // up to 10 minutes
        Sleep(1000);
        if (!names) names = ResolveNames();
        if (!physx) physx = ConfigureSubstepping();
    }
    Log(1, "init done: names=%s substepping=%s", names ? "ok" : "MISSING", physx ? "ok" : "not applied");
    for (int i = 0; i < N_COUNT; ++i)
        if (g_nameIdx[i] < 0) Log(1, "  unresolved name: %s", kNames[i]);
    while (g_cfg.logLevel > 0 && g_logFile) { // periodic summary, only when something changed
        Sleep(30000);
        LogStats();
    }
    return 0;
}

// ----------------------------------------------------------------------
// Entry
// ----------------------------------------------------------------------
struct Sig { const char* name; const char* pattern; void** out; };

static bool Install() {
    if (!InitTextSection()) return false;
    void *pe = nullptr, *ffn = nullptr, *sis = nullptr, *sync = nullptr, *pc = nullptr, *ps = nullptr, *glass = nullptr,
         *probe = nullptr, *damage = nullptr, *moving = nullptr, *colModel = nullptr, *entColModel = nullptr, *gnamesRef = nullptr, *gobjRef = nullptr;
    const Sig sigs[] = {
        { "ProcessEvent", "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC F0 00 00 00 48 8D 6C 24 30", &pe },
        { "UClass::FindFunctionByName", "44 89 44 24 18 53 55 57 41 55 48 83 EC 48 8B 81 38 01 00 00 45 33 ED 48 8B DA", &ffn },
        { "CObject::SetIsStatic", "40 53 48 83 EC 20 83 61 30 FB 48 8B D9 0F B6 C2 C1 E0 02 09 41 30 84 D2", &sis },
        { "CEntity::SyncActor", "48 89 5C 24 10 57 48 83 EC 70 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 60 48 8B 79 20 48 8B D9", &sync },
        { "CPhysical::ProcessCollision", "40 55 57 41 54 48 8D AC 24 50 FF FF FF 48 81 EC B0 01 00 00 8B 41 78 45 33 E4", &pc },
        { "CPhysical::ProcessShift", "40 55 53 57 48 8D 6C 24 90 48 81 EC 70 01 00 00 48 8B 01 48 8D 54 24 38", &ps },
        { "CGlass::WindowRespondsToCollision", "4C 8B DC 55 53 57 41 57 49 8D AB 08 FF FF FF 48 81 EC D8 01 00 00 45 0F 29 8B 78 FF FF FF", &glass },
        { "BreakObject ground probe", "4C 8B DC 49 89 5B 20 55 56 57 48 81 EC 10 01 00 00 41 0F 29 73 D8 41 0F 29 7B C8", &probe },
        { "CObject::ObjectDamage", "4C 8B DC 55 56 57 41 54 41 55 49 8D AB C8 FE FF FF 48 81 EC 10 02 00 00", &damage },
        { "CPhysical::AddToMovingList", "40 53 48 83 EC 20 48 83 B9 F0 00 00 00 00 48 8B D9 75 ?? F7 41 30 00 00 04 00", &moving },
        { "CModelInfo::GetColModel", "48 83 EC 28 48 63 C1 48 8D 0D ?? ?? ?? ?? 48 8B 0C C1 48 85 C9 75 28 65 48 8B 04 25 58 00 00 00", &colModel },
        { "CEntity::GetColModel", "40 53 48 83 EC 30 0F B6 41 6A 48 8B D9 24 07 3C 02 75 20 48 0F BE 81 F7 06 00 00", &entColModel },
        { "GNames ref", "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 01 8B 80 70 07 01 00", &gnamesRef },
        { "GObjects ref", "48 8D 05 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? E8 03 00 00 48 8D 0D ?? ?? ?? ??", &gobjRef },
    };
    bool ok = true;
    for (const Sig& s : sigs) {
        *s.out = FindUnique(s.pattern);
        Log(*s.out ? 2 : 1, "sig %-36s %p%s", s.name, *s.out, *s.out ? "" : "  <-- NOT FOUND");
        if (!*s.out) ok = false;
    }
    if (!ok) {
        Log(1, "unsupported SanAndreas.exe build: plugin disabled (game runs unmodded)");
        return false;
    }
    g_names = (FNamePool*)RipTarget((uint8_t*)gnamesRef, 3, 7);
    g_objects = RipTarget((uint8_t*)gobjRef, 3, 7);
    g_FindFunctionByName = (FindFunctionByName_Fn)ffn;
    g_SyncActor = (SyncActor_Fn)sync;
    g_GroundProbe = (GroundProbe_Fn)probe;
    g_AddToMovingList = (AddToMovingList_Fn)moving;
    g_GetColModel = (GetColModel_Fn)colModel;
    g_modelInfos = (void**)RipTarget((uint8_t*)colModel + 7, 3, 7); // lea rcx, CModelInfo::ms_modelInfoPtrs
    void* miColModel = FindUnique("48 8B 41 30 C3 CC CC CC CC"); // CBaseModelInfo::GetColModel (optional)
    Log(miColModel ? 2 : 1, "sig %-36s %p%s", "CBaseModelInfo::GetColModel", miColModel, miColModel ? "" : "  <-- not found: generated spheres disabled");

    if (MH_Initialize() != MH_OK) return false;
    ok = MH_CreateHook(pe, (void*)&Hooked_ProcessEvent, (void**)&o_ProcessEvent) == MH_OK &&
         MH_CreateHook(sis, (void*)&Hooked_SetIsStatic, (void**)&o_SetIsStatic) == MH_OK &&
         MH_CreateHook(pc, (void*)&Hooked_ProcessCollision, (void**)&o_ProcessCollision) == MH_OK &&
         MH_CreateHook(ps, (void*)&Hooked_ProcessShift, (void**)&o_ProcessShift) == MH_OK &&
         MH_CreateHook(glass, (void*)&Hooked_GlassCollision, (void**)&o_GlassCollision) == MH_OK &&
         MH_CreateHook(damage, (void*)&Hooked_ObjectDamage, (void**)&o_ObjectDamage) == MH_OK &&
         MH_CreateHook(entColModel, (void*)&Hooked_EntityColModel, (void**)&o_EntityColModel) == MH_OK &&
         (!miColModel || MH_CreateHook(miColModel, (void*)&Hooked_MIColModel, (void**)&o_MIColModel) == MH_OK);
    if (!ok || MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        Log(1, "hook installation failed");
        MH_Uninitialize();
        return false;
    }
    Log(1, "hooks installed (%d signatures found)", (int)(sizeof(sigs) / sizeof(sigs[0])));
    return true;
}

#ifndef PROPFIX_TEST
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        for (int i = 0; i < N_COUNT; ++i) g_nameIdx[i] = -1;
        LoadConfig(hModule);
        OpenLog();
        char exe[MAX_PATH] = "?";
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        Log(1, "GTA_Prop_Fix v0.3.2 | exe %s", exe);
        Log(1, "ini %s (%s) | log level %d", g_iniPath, g_iniFound ? "found" : "NOT FOUND, using defaults", g_cfg.logLevel);
        Log(1, "config: rwProps=%d streetLightsFallWhole=%d piecesDisappear=%.1fs polesDisappear=%.1fs propsDisappear=%.1fs polesBlockCars=%d stayPushable=%d fragDamping=%.2f minColCoverage=%d%% minShapeCoverage=%d%% addSpheres=%d lightsOff=%d groundPlane=%d lifetime=%d "
               "planeSize=%.0fm ccd=%d substepping=%d originalPieces=%d fade=%.2fs",
            g_cfg.rwDislodgedProps, g_cfg.uprootStreetLights, g_cfg.piecesDisappearSeconds, g_cfg.fallenPolesDisappearSeconds, g_cfg.knockedPropsDisappearSeconds, g_cfg.fallenPolesBlockCars, g_cfg.keepPushable, g_cfg.fragmentAngularDamping, (int)(g_cfg.minColCoverage * 100), (int)(g_cfg.minShapeCoverage * 100), g_cfg.addSpheres, g_cfg.lightsOffWhenTilted,
            g_cfg.fixDebrisFloor, g_cfg.debrisMode, g_cfg.debrisPlaneSizeM, g_cfg.debrisCCD, g_cfg.substepping, g_cfg.originalPieceMotion, g_cfg.piecesFadeSeconds);
        if (Install()) {
            HANDLE h = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
            if (h) CloseHandle(h);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }
    return TRUE;
}
#endif

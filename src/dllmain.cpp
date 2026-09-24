#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "../minhook/MinHook.h"

// =======================================================================
// GTA SA Definitive Edition - Street Props Physics Fix v0.0.1
//
// Dual-Layer Physics Solution:
// 1. NEUTRALIZE ARTIFICIAL EXPLOSIONS & PROPELLER SPINNING:
//    - Intercept and DROP all calls to AddRandomOutwardVelocityToAllBodies
//    - Clamp vertical launch impulses (Impulse.Z = 0) in SetupBroken and
//      AddImpulseAtLocationForAllBodiesBelow so poles tip over naturally
//      at bumper height instead of rocketing into the sky.
//    - Natural linear damping (0.05) and angular damping (0.40).
// 2. DYNAMIC VEHICLE COLLIDER (NO MORE GHOSTING THROUGH CARS):
//    - When a car crashes into a prop, the unbroken 'Mesh' component is
//      re-purposed as an invisible, solid 3D PhysX vehicle box (2.2m x 4.8m x 1.4m).
//    - Positioned at the vehicle's footprint and kinematically moved along
//      the vehicle's velocity vector across the 2.5s impact window.
//    - Falling poles collide with and roll over the car hood and roof!
// 3. FLUSH GROUND COLLIDER (NO FALLING THROUGH ROAD):
//    - 150m x 150m x 0.2m solid barrier flush with the road (contactZ).
//    - RemoveFloor permanently blocked.
// =======================================================================

// RVAs for SanAndreas.exe
static const uintptr_t RVA_ProcessEvent = 0x01C7B6B0;
static const uintptr_t RVA_GNames       = 0x0570CDC0;
static const uintptr_t RVA_GObjects      = 0x05086380;

static FILE* g_LogFile = nullptr;

static void Log(const char* fmt, ...) {
    if (!g_LogFile) return;
    va_list args;
    va_start(args, fmt);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_LogFile, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    vfprintf(g_LogFile, fmt, args);
    fflush(g_LogFile);
    va_end(args);
}

// ===== UE4 Structures =====
#pragma pack(push, 1)
struct FNameEntryHeader {
    uint16_t bIsWide : 1;
    uint16_t BitPad_0_1 : 5;
    uint16_t Len : 10;
};
struct FNamePool {
    uint8_t Pad_0[8];
    uint32_t CurrentBlock;
    uint32_t CurrentByteCursor;
    uint8_t* Blocks[0x2000];
};
struct FUObjectItem {
    void* Object;
    int32_t Flags;
    int32_t ClusterRootIndex;
    int32_t SerialNumber;
    int32_t Pad;
};
struct TUObjectArray {
    FUObjectItem** Objects;
    uint8_t Pad_8[8];
    int32_t MaxElements;
    int32_t NumElements;
    int32_t MaxChunks;
    int32_t NumChunks;
};
#pragma pack(pop)

static FNamePool* g_Names = nullptr;
static TUObjectArray* g_Objects = nullptr;

static bool GetFNameString(int32_t index, char* outBuf, size_t maxLen) {
    if (!g_Names || index < 0 || !outBuf || maxLen == 0) return false;
    uint32_t chunkIdx = (uint32_t)index >> 16;
    uint32_t inChunk  = (uint32_t)index & 0xFFFF;
    if (chunkIdx > g_Names->CurrentBlock) return false;
    if (chunkIdx == g_Names->CurrentBlock && inChunk > g_Names->CurrentByteCursor) return false;
    uint8_t* block = g_Names->Blocks[chunkIdx];
    if (!block) return false;
    uint8_t* entryPtr = block + (inChunk * 2);
    FNameEntryHeader* header = (FNameEntryHeader*)entryPtr;
    uint16_t len = header->Len;
    if (len == 0) return false;
    if (header->bIsWide) {
        wchar_t* wideName = (wchar_t*)(entryPtr + 2);
        int converted = WideCharToMultiByte(CP_UTF8, 0, wideName, len, outBuf, (int)maxLen - 1, nullptr, nullptr);
        if (converted > 0) { outBuf[converted] = '\0'; return true; }
        return false;
    } else {
        char* ansiName = (char*)(entryPtr + 2);
        size_t toCopy = len < maxLen - 1 ? len : maxLen - 1;
        memcpy(outBuf, ansiName, toCopy);
        outBuf[toCopy] = '\0';
        return true;
    }
}

// ===== ProcessEvent Hook =====
typedef void (*ProcessEvent_Fn)(void* Context, void* Function, void* Parms);
static ProcessEvent_Fn pOriginalProcessEvent = nullptr;

static int32_t s_RemoveFloorIdx = -1;
static int32_t s_SetupBrokenIdx = -1;
static int32_t s_AddRandomOutwardVelocityIdx = -1;
static int32_t s_AddImpulseAtLocationIdx = -1;

static uint64_t s_BlockedRemoveFloorCount = 0;
static uint64_t s_BlockedAddRandomCount = 0;
static uint64_t s_TunedImpulseCount = 0;
static uint64_t s_PatchedSetupBrokenCount = 0;

// ===== Engine UFunction Pointers =====
static void* g_fnSetMobility = nullptr;                     // USceneComponent::SetMobility
static void* g_fnK2_DetachFromComponent = nullptr;          // USceneComponent::K2_DetachFromComponent
static void* g_fnSetAbsolute = nullptr;                     // USceneComponent::SetAbsolute
static void* g_fnSetWorldLocationAndRotation = nullptr;     // USceneComponent::K2_SetWorldLocationAndRotation
static void* g_fnSetWorldScale3D = nullptr;                 // USceneComponent::SetWorldScale3D
static void* g_fnGetComponentLocation = nullptr;           // USceneComponent::K2_GetComponentLocation
static void* g_fnSetCollisionObjectType = nullptr;          // UPrimitiveComponent::SetCollisionObjectType
static void* g_fnSetCollisionEnabled = nullptr;             // UPrimitiveComponent::SetCollisionEnabled
static void* g_fnSetCollisionResponseToAllChannels = nullptr; // UPrimitiveComponent::SetCollisionResponseToAllChannels
static void* g_fnSetCollisionResponseToChannel = nullptr;   // UPrimitiveComponent::SetCollisionResponseToChannel
static void* g_fnIgnoreComponentWhenMoving = nullptr;       // UPrimitiveComponent::IgnoreComponentWhenMoving
static void* g_fnClearMoveIgnoreComponents = nullptr;       // UPrimitiveComponent::ClearMoveIgnoreComponents
static void* g_fnSetAllUseCCD = nullptr;                    // UPrimitiveComponent::SetAllUseCCD
static void* g_fnSetStaticMesh = nullptr;                   // UStaticMeshComponent::SetStaticMesh
static void* g_fnSetLinearDamping = nullptr;                // UPrimitiveComponent::SetLinearDamping
static void* g_fnSetAngularDamping = nullptr;               // UPrimitiveComponent::SetAngularDamping
static void* g_meshBreakableFloor = nullptr;                // StaticMesh Breakable_Floor
static volatile bool g_bFunctionsResolved = false;

// ===== Parameter Structs =====
#pragma pack(push, 1)
struct Parms_SetMobility {
    uint8_t NewMobility; // 0x0000(0x0001) 2 = Movable
};
struct Parms_K2_DetachFromComponent {
    uint8_t LocationRule; // 0x0000(0x0001) 1 = KeepWorld
    uint8_t RotationRule; // 0x0001(0x0001) 1 = KeepWorld
    uint8_t ScaleRule;    // 0x0002(0x0001) 1 = KeepWorld
    bool bCallModify;     // 0x0003(0x0001)
};
struct Parms_SetAbsolute {
    bool bNewAbsoluteLocation; // 0x0000(0x0001)
    bool bNewAbsoluteRotation; // 0x0001(0x0001)
    bool bNewAbsoluteScale;    // 0x0002(0x0001)
};
struct Parms_SetWorldLocationAndRotation {
    float NewLocation[3];          // 0x0000(0x000C)
    float NewRotation[3];          // 0x000C(0x000C)
    bool bSweep;                   // 0x0018(0x0001)
    uint8_t Pad_19[3];
    uint8_t SweepHitResult[0x8C];  // 0x001C(0x008C)
    bool bTeleport;                // 0x00A8(0x0001)
    uint8_t Pad_A9[3];
};
struct Parms_SetWorldScale3D {
    float NewScale[3]; // 0x0000(0x000C)
};
struct Parms_GetComponentLocation {
    float ReturnValue[3]; // 0x0000(0x000C)
};
struct Parms_SetCollisionObjectType {
    uint8_t Channel; // 0x0000(0x0001) 0 = ECC_WorldStatic, 1 = ECC_WorldDynamic
};
struct Parms_SetCollisionEnabled {
    uint8_t NewType; // 0x0000(0x0001) 3 = QueryAndPhysics
};
struct Parms_SetCollisionResponseToAllChannels {
    uint8_t NewResponse; // 0x0000(0x0001) 2 = ECR_Block
};
struct Parms_SetCollisionResponseToChannel {
    uint8_t Channel;     // 0x0000(0x0001)
    uint8_t NewResponse; // 0x0001(0x0001)
};
struct Parms_IgnoreComponentWhenMoving {
    void* Component; // 0x0000(0x0008)
    bool bShouldIgnore; // 0x0008(0x0001)
};
struct Parms_SetAllUseCCD {
    bool InUseCCD; // 0x0000(0x0001)
};
struct Parms_SetStaticMesh {
    void* NewMesh; // 0x0000(0x0008)
    bool ReturnValue; // 0x0008(0x0001)
};
struct Parms_SetLinearDamping {
    float InDamping; // 0x0000(0x0004)
};
struct Parms_SetAngularDamping {
    float InDamping; // 0x0000(0x0004)
};
#pragma pack(pop)

// ===== Active Dynamic Vehicle Collider Tracker =====
struct ActiveCarCollider {
    void* Comp;
    float PosX, PosY, PosZ;
    float VelX, VelY;
    float Yaw;
    DWORD StartTime;
    DWORD LastUpdateTime;
    float DurationSec;
    bool Active;
};
static const int MAX_CAR_COLLIDERS = 8;
static ActiveCarCollider g_CarColliders[MAX_CAR_COLLIDERS] = {};

static void RegisterCarCollider(void* comp, float x, float y, float z, float vx, float vy, float yaw, float duration) {
    if (!comp) return;
    DWORD now = GetTickCount();
    for (int i = 0; i < MAX_CAR_COLLIDERS; ++i) {
        if (!g_CarColliders[i].Active) {
            g_CarColliders[i].Comp = comp;
            g_CarColliders[i].PosX = x;
            g_CarColliders[i].PosY = y;
            g_CarColliders[i].PosZ = z;
            g_CarColliders[i].VelX = vx;
            g_CarColliders[i].VelY = vy;
            g_CarColliders[i].Yaw = yaw;
            g_CarColliders[i].StartTime = now;
            g_CarColliders[i].LastUpdateTime = now;
            g_CarColliders[i].DurationSec = duration;
            g_CarColliders[i].Active = true;
            return;
        }
    }
    g_CarColliders[0].Comp = comp;
    g_CarColliders[0].PosX = x;
    g_CarColliders[0].PosY = y;
    g_CarColliders[0].PosZ = z;
    g_CarColliders[0].VelX = vx;
    g_CarColliders[0].VelY = vy;
    g_CarColliders[0].Yaw = yaw;
    g_CarColliders[0].StartTime = now;
    g_CarColliders[0].LastUpdateTime = now;
    g_CarColliders[0].DurationSec = duration;
    g_CarColliders[0].Active = true;
}

static void UpdateActiveCarColliders() {
    if (!g_fnSetWorldLocationAndRotation || !pOriginalProcessEvent) return;
    DWORD now = GetTickCount();

    for (int i = 0; i < MAX_CAR_COLLIDERS; ++i) {
        if (!g_CarColliders[i].Active || !g_CarColliders[i].Comp) continue;

        float elapsed = (now - g_CarColliders[i].StartTime) / 1000.0f;
        if (elapsed >= g_CarColliders[i].DurationSec) {
            if (g_fnSetCollisionEnabled) {
                Parms_SetCollisionEnabled colParms = { 0 }; // NoCollision
                pOriginalProcessEvent(g_CarColliders[i].Comp, g_fnSetCollisionEnabled, &colParms);
            }
            g_CarColliders[i].Active = false;
            g_CarColliders[i].Comp = nullptr;
            continue;
        }

        DWORD deltaMs = now - g_CarColliders[i].LastUpdateTime;
        if (deltaMs < 16) continue;

        float dt = deltaMs / 1000.0f;
        g_CarColliders[i].LastUpdateTime = now;
        g_CarColliders[i].PosX += g_CarColliders[i].VelX * dt;
        g_CarColliders[i].PosY += g_CarColliders[i].VelY * dt;

        Parms_SetWorldLocationAndRotation locRot{};
        locRot.NewLocation[0] = g_CarColliders[i].PosX;
        locRot.NewLocation[1] = g_CarColliders[i].PosY;
        locRot.NewLocation[2] = g_CarColliders[i].PosZ;
        locRot.NewRotation[0] = 0.0f;
        locRot.NewRotation[1] = g_CarColliders[i].Yaw;
        locRot.NewRotation[2] = 0.0f;
        locRot.bSweep = false;
        locRot.bTeleport = false; // KINEMATIC SWEEP: Physically deflects dynamic rigid bodies!
        pOriginalProcessEvent(g_CarColliders[i].Comp, g_fnSetWorldLocationAndRotation, &locRot);
    }
}

// ===== Engine Function Resolution =====
static void ResolveEngineFunctions() {
    if (g_bFunctionsResolved || !g_Objects) return;

    for (int32_t i = 0; i < g_Objects->NumElements; ++i) {
        int32_t chunkIdx = i / 65536;
        int32_t inChunk  = i % 65536;
        if (chunkIdx >= g_Objects->NumChunks || !g_Objects->Objects[chunkIdx]) continue;
        FUObjectItem* item = &g_Objects->Objects[chunkIdx][inChunk];
        if (!item || !item->Object) continue;
        void* obj = item->Object;

        int32_t nameIdx = *(int32_t*)((uintptr_t)obj + 0x18);
        char nameBuf[128];
        if (!GetFNameString(nameIdx, nameBuf, sizeof(nameBuf))) continue;

        if (!g_fnSetMobility && strcmp(nameBuf, "SetMobility") == 0) {
            g_fnSetMobility = obj;
            Log("[LightPoleFix]   Found SetMobility at 0x%p\n", obj);
        } else if (!g_fnK2_DetachFromComponent && strcmp(nameBuf, "K2_DetachFromComponent") == 0) {
            g_fnK2_DetachFromComponent = obj;
            Log("[LightPoleFix]   Found K2_DetachFromComponent at 0x%p\n", obj);
        } else if (!g_fnSetAbsolute && strcmp(nameBuf, "SetAbsolute") == 0) {
            g_fnSetAbsolute = obj;
            Log("[LightPoleFix]   Found SetAbsolute at 0x%p\n", obj);
        } else if (!g_fnSetWorldLocationAndRotation && strcmp(nameBuf, "K2_SetWorldLocationAndRotation") == 0) {
            g_fnSetWorldLocationAndRotation = obj;
            Log("[LightPoleFix]   Found K2_SetWorldLocationAndRotation at 0x%p\n", obj);
        } else if (!g_fnSetWorldScale3D && strcmp(nameBuf, "SetWorldScale3D") == 0) {
            g_fnSetWorldScale3D = obj;
            Log("[LightPoleFix]   Found SetWorldScale3D at 0x%p\n", obj);
        } else if (!g_fnGetComponentLocation && strcmp(nameBuf, "K2_GetComponentLocation") == 0) {
            g_fnGetComponentLocation = obj;
            Log("[LightPoleFix]   Found K2_GetComponentLocation at 0x%p\n", obj);
        } else if (!g_fnSetCollisionObjectType && strcmp(nameBuf, "SetCollisionObjectType") == 0) {
            g_fnSetCollisionObjectType = obj;
            Log("[LightPoleFix]   Found SetCollisionObjectType at 0x%p\n", obj);
        } else if (!g_fnSetCollisionEnabled && strcmp(nameBuf, "SetCollisionEnabled") == 0) {
            g_fnSetCollisionEnabled = obj;
            Log("[LightPoleFix]   Found SetCollisionEnabled at 0x%p\n", obj);
        } else if (!g_fnSetCollisionResponseToAllChannels && strcmp(nameBuf, "SetCollisionResponseToAllChannels") == 0) {
            g_fnSetCollisionResponseToAllChannels = obj;
            Log("[LightPoleFix]   Found SetCollisionResponseToAllChannels at 0x%p\n", obj);
        } else if (!g_fnSetCollisionResponseToChannel && strcmp(nameBuf, "SetCollisionResponseToChannel") == 0) {
            g_fnSetCollisionResponseToChannel = obj;
            Log("[LightPoleFix]   Found SetCollisionResponseToChannel at 0x%p\n", obj);
        } else if (!g_fnIgnoreComponentWhenMoving && strcmp(nameBuf, "IgnoreComponentWhenMoving") == 0) {
            g_fnIgnoreComponentWhenMoving = obj;
            Log("[LightPoleFix]   Found IgnoreComponentWhenMoving at 0x%p\n", obj);
        } else if (!g_fnClearMoveIgnoreComponents && strcmp(nameBuf, "ClearMoveIgnoreComponents") == 0) {
            g_fnClearMoveIgnoreComponents = obj;
            Log("[LightPoleFix]   Found ClearMoveIgnoreComponents at 0x%p\n", obj);
        } else if (!g_fnSetAllUseCCD && strcmp(nameBuf, "SetAllUseCCD") == 0) {
            g_fnSetAllUseCCD = obj;
            Log("[LightPoleFix]   Found SetAllUseCCD at 0x%p\n", obj);
        } else if (!g_fnSetStaticMesh && strcmp(nameBuf, "SetStaticMesh") == 0) {
            g_fnSetStaticMesh = obj;
            Log("[LightPoleFix]   Found SetStaticMesh at 0x%p\n", obj);
        } else if (!g_fnSetLinearDamping && strcmp(nameBuf, "SetLinearDamping") == 0) {
            g_fnSetLinearDamping = obj;
            Log("[LightPoleFix]   Found SetLinearDamping at 0x%p\n", obj);
        } else if (!g_fnSetAngularDamping && strcmp(nameBuf, "SetAngularDamping") == 0) {
            g_fnSetAngularDamping = obj;
            Log("[LightPoleFix]   Found SetAngularDamping at 0x%p\n", obj);
        } else if (!g_meshBreakableFloor && strcmp(nameBuf, "Breakable_Floor") == 0) {
            void* uclass = *(void**)((uintptr_t)obj + 0x10);
            if (uclass) {
                char clsName[64];
                if (GetFNameString(*(int32_t*)((uintptr_t)uclass + 0x18), clsName, sizeof(clsName))) {
                    if (strcmp(clsName, "StaticMesh") == 0) {
                        g_meshBreakableFloor = obj;
                        Log("[LightPoleFix]   Found StaticMesh Breakable_Floor at 0x%p\n", obj);
                    }
                }
            }
        }
    }

    if (g_fnSetMobility && g_fnSetWorldLocationAndRotation && g_fnSetCollisionObjectType) {
        g_bFunctionsResolved = true;
        Log("[LightPoleFix] All engine physics functions successfully resolved!\n");
    }
}

// ===== Set Collision Channel Response Helper =====
static void SetChannelResponse(void* comp, uint8_t channel, uint8_t response) {
    if (!comp || !g_fnSetCollisionResponseToChannel) return;
    Parms_SetCollisionResponseToChannel p = { channel, response };
    pOriginalProcessEvent(comp, g_fnSetCollisionResponseToChannel, &p);
}

// ===== Component Resolver =====
static void ResolveComponents(void* Context, void** outFloor, void** outBrokenMesh, void** outMesh) {
    *outFloor = nullptr;
    *outBrokenMesh = nullptr;
    *outMesh = nullptr;
    if (!Context || !g_Objects) return;

    for (int32_t i = 0; i < g_Objects->NumElements; ++i) {
        int32_t chunkIdx = i / 65536;
        int32_t inChunk  = i % 65536;
        if (chunkIdx >= g_Objects->NumChunks || !g_Objects->Objects[chunkIdx]) continue;
        FUObjectItem* item = &g_Objects->Objects[chunkIdx][inChunk];
        if (!item || !item->Object) continue;
        void* obj = item->Object;

        __try {
            if (*(void**)((uintptr_t)obj + 0x20) != Context) continue;

            char objName[128] = "";
            char clsName[128] = "";
            GetFNameString(*(int32_t*)((uintptr_t)obj + 0x18), objName, sizeof(objName));
            void* uclass = *(void**)((uintptr_t)obj + 0x10);
            if (uclass) {
                GetFNameString(*(int32_t*)((uintptr_t)uclass + 0x18), clsName, sizeof(clsName));
            }

            if (strstr(clsName, "Component") == nullptr) continue;

            Log("  [Actor Component] '%s' (Class '%s') at 0x%p\n", objName, clsName, obj);

            if (!*outFloor && (strcmp(objName, "PhysicsFloor") == 0 || strstr(objName, "Floor") != nullptr)) {
                *outFloor = obj;
            }

            if (!*outBrokenMesh && (strstr(objName, "Broken") != nullptr || strstr(clsName, "SkeletalMeshComponent") != nullptr)) {
                *outBrokenMesh = obj;
            }

            if (!*outMesh && strcmp(objName, "Mesh") == 0 && strstr(clsName, "StaticMeshComponent") != nullptr) {
                *outMesh = obj;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// ===== Core Physics Fix on SetupBroken =====
static void FixBrokenProp(void* Context, void* Parms) {
    if (!Context || !Parms) return;

    __try {
        char actorName[128] = "Unknown";
        char actorClassName[128] = "Unknown";
        GetFNameString(*(int32_t*)((uintptr_t)Context + 0x18), actorName, sizeof(actorName));
        void* actorClass = *(void**)((uintptr_t)Context + 0x10);
        if (actorClass) GetFNameString(*(int32_t*)((uintptr_t)actorClass + 0x18), actorClassName, sizeof(actorClassName));
        Log("[LightPoleFix] Broken Prop Actor: '%s' (Class '%s') at 0x%p\n", actorName, actorClassName, Context);

        float* pImpulseSrc = (float*)((uintptr_t)Parms + 0x00);
        float* pImpulseVel = (float*)((uintptr_t)Parms + 0x0C);
        float* pFloorLoc   = (float*)((uintptr_t)Parms + 0x30);

        float contactX = pFloorLoc[0];
        float contactY = pFloorLoc[1];
        float contactZ = pFloorLoc[2];

        // Fallback if floor location is near (0,0,0)
        if (fabs(contactX) < 1.0f && fabs(contactY) < 1.0f) {
            contactX = pImpulseSrc[0];
            contactY = pImpulseSrc[1];
            contactZ = pImpulseSrc[2] - 80.0f;
        }

        float vx = pImpulseVel[0];
        float vy = pImpulseVel[1];
        float vz = pImpulseVel[2];
        float speed = sqrtf(vx * vx + vy * vy);

        Log("[LightPoleFix] Target Ground Contact: (%.2f, %.2f, %.2f)\n", contactX, contactY, contactZ);
        Log("[LightPoleFix] Impact Velocity: (%.2f, %.2f, %.2f), HorizSpeed = %.1f cm/s (%.1f km/h)\n",
            vx, vy, vz, speed, speed * 0.036f);

        void* floorComp = nullptr;
        void* brokenMeshComp = nullptr;
        void* meshComp = nullptr;
        ResolveComponents(Context, &floorComp, &brokenMeshComp, &meshComp);

        Log("[LightPoleFix]   floorComp   = 0x%p\n", floorComp);
        Log("[LightPoleFix]   brokenMesh  = 0x%p\n", brokenMeshComp);
        Log("[LightPoleFix]   meshComp    = 0x%p\n", meshComp);

        // ==============================================================
        // 1. CONFIGURE & TELEPORT THE GROUND FLOOR COLLIDER
        // ==============================================================
        if (floorComp) {
            void* curMesh = *(void**)((uintptr_t)floorComp + 0x0480);
            if (!curMesh && g_meshBreakableFloor && g_fnSetStaticMesh) {
                Parms_SetStaticMesh meshParms = { g_meshBreakableFloor, false };
                pOriginalProcessEvent(floorComp, g_fnSetStaticMesh, &meshParms);
                Log("[LightPoleFix]   Assigned Breakable_Floor StaticMesh to PhysicsFloor!\n");
            }

            if (g_fnSetMobility) {
                Parms_SetMobility mobParms = { 2 }; // Movable
                pOriginalProcessEvent(floorComp, g_fnSetMobility, &mobParms);
            }

            if (g_fnK2_DetachFromComponent) {
                Parms_K2_DetachFromComponent detParms = { 1, 1, 1, false }; // KeepWorld
                pOriginalProcessEvent(floorComp, g_fnK2_DetachFromComponent, &detParms);
            }

            if (g_fnSetAbsolute) {
                Parms_SetAbsolute absParms = { true, true, true };
                pOriginalProcessEvent(floorComp, g_fnSetAbsolute, &absParms);
            }

            // Flush ground barrier (Scale.Z = 0.2f -> thickness 20cm, half-height 10cm)
            if (g_fnSetWorldLocationAndRotation) {
                Parms_SetWorldLocationAndRotation locRotParms{};
                locRotParms.NewLocation[0] = contactX;
                locRotParms.NewLocation[1] = contactY;
                locRotParms.NewLocation[2] = contactZ - 10.0f; // Flush with road
                locRotParms.NewRotation[0] = 0.0f;
                locRotParms.NewRotation[1] = 0.0f;
                locRotParms.NewRotation[2] = 0.0f;
                locRotParms.bSweep = false;
                locRotParms.bTeleport = true;
                pOriginalProcessEvent(floorComp, g_fnSetWorldLocationAndRotation, &locRotParms);
                Log("[LightPoleFix]   -> Teleported Floor (Top Surface Flush at Z=%.2f)!\n", contactZ);
            }

            // Expand to 150m x 150m x 0.2m ground barrier
            if (g_fnSetWorldScale3D) {
                Parms_SetWorldScale3D scaleParms{};
                scaleParms.NewScale[0] = 150.0f;
                scaleParms.NewScale[1] = 150.0f;
                scaleParms.NewScale[2] = 0.2f;
                pOriginalProcessEvent(floorComp, g_fnSetWorldScale3D, &scaleParms);
                Log("[LightPoleFix]   -> SetWorldScale3D(150, 150, 0.2) SUCCESS!\n");
            }

            if (g_fnSetCollisionObjectType) {
                Parms_SetCollisionObjectType typeParms = { 0 }; // ECC_WorldStatic
                pOriginalProcessEvent(floorComp, g_fnSetCollisionObjectType, &typeParms);
            }

            if (g_fnSetCollisionEnabled) {
                Parms_SetCollisionEnabled colParms = { 3 }; // QueryAndPhysics
                pOriginalProcessEvent(floorComp, g_fnSetCollisionEnabled, &colParms);
            }

            // Block physics bodies, ignore vehicles/pawns/camera
            if (g_fnSetCollisionResponseToAllChannels) {
                Parms_SetCollisionResponseToAllChannels respAll = { 2 }; // ECR_Block
                pOriginalProcessEvent(floorComp, g_fnSetCollisionResponseToAllChannels, &respAll);
            }
            SetChannelResponse(floorComp, 2, 0); // ECC_Pawn -> ECR_Ignore
            SetChannelResponse(floorComp, 3, 0); // ECC_Visibility -> ECR_Ignore
            SetChannelResponse(floorComp, 4, 0); // ECC_Camera -> ECR_Ignore
            SetChannelResponse(floorComp, 6, 0); // ECC_Vehicle -> ECR_Ignore
            SetChannelResponse(floorComp, 5, 2); // ECC_PhysicsBody -> ECR_Block
            SetChannelResponse(floorComp, 7, 2); // ECC_Destructible -> ECR_Block

            if (g_fnClearMoveIgnoreComponents) {
                pOriginalProcessEvent(floorComp, g_fnClearMoveIgnoreComponents, nullptr);
            }
            if (brokenMeshComp && g_fnIgnoreComponentWhenMoving) {
                Parms_IgnoreComponentWhenMoving ign = { brokenMeshComp, false };
                pOriginalProcessEvent(floorComp, g_fnIgnoreComponentWhenMoving, &ign);
            }

            *(float*)((uintptr_t)floorComp + 0x02C0 + 0x00A8) = 100.0f; // MaxDepenetrationVelocity = 1.0 m/s
        }

        // ==============================================================
        // 2. DYNAMIC VEHICLE COLLIDER BOX (PREVENTS GHOSTING THROUGH CAR)
        // ==============================================================
        if (meshComp && g_meshBreakableFloor) {
            // Assign solid box mesh
            if (g_fnSetStaticMesh) {
                Parms_SetStaticMesh meshParms = { g_meshBreakableFloor, false };
                pOriginalProcessEvent(meshComp, g_fnSetStaticMesh, &meshParms);
            }

            if (g_fnSetMobility) {
                Parms_SetMobility mobParms = { 2 }; // Movable
                pOriginalProcessEvent(meshComp, g_fnSetMobility, &mobParms);
            }

            if (g_fnK2_DetachFromComponent) {
                Parms_K2_DetachFromComponent detParms = { 1, 1, 1, false }; // KeepWorld
                pOriginalProcessEvent(meshComp, g_fnK2_DetachFromComponent, &detParms);
            }

            if (g_fnSetAbsolute) {
                Parms_SetAbsolute absParms = { true, true, true };
                pOriginalProcessEvent(meshComp, g_fnSetAbsolute, &absParms);
            }

            // Scale to car proportions: Width 2.2m, Length 4.8m, Height 1.4m
            // Breakable_Floor unit cube is 100x100x100 cm (half-extent 50cm).
            // Scale: (2.2, 4.8, 1.4) -> 220cm wide, 480cm long, 140cm tall box.
            if (g_fnSetWorldScale3D) {
                Parms_SetWorldScale3D scaleParms{};
                scaleParms.NewScale[0] = 2.2f;
                scaleParms.NewScale[1] = 4.8f;
                scaleParms.NewScale[2] = 1.4f;
                pOriginalProcessEvent(meshComp, g_fnSetWorldScale3D, &scaleParms);
            }

            if (g_fnSetCollisionObjectType) {
                Parms_SetCollisionObjectType typeParms = { 1 }; // ECC_WorldDynamic
                pOriginalProcessEvent(meshComp, g_fnSetCollisionObjectType, &typeParms);
            }

            if (g_fnSetCollisionEnabled) {
                Parms_SetCollisionEnabled colParms = { 3 }; // QueryAndPhysics
                pOriginalProcessEvent(meshComp, g_fnSetCollisionEnabled, &colParms);
            }

            // Ignore everything except PhysicsBody (the falling pole)
            if (g_fnSetCollisionResponseToAllChannels) {
                Parms_SetCollisionResponseToAllChannels respAll = { 0 }; // ECR_Ignore
                pOriginalProcessEvent(meshComp, g_fnSetCollisionResponseToAllChannels, &respAll);
            }
            SetChannelResponse(meshComp, 5, 2); // ECC_PhysicsBody -> ECR_Block
            SetChannelResponse(meshComp, 7, 2); // ECC_Destructible -> ECR_Block

            // Calculate car position and orientation from impact velocity
            float dirX = 0.0f;
            float dirY = 0.0f;
            float yawDeg = 0.0f;
            if (speed > 50.0f) {
                dirX = vx / speed;
                dirY = vy / speed;
                yawDeg = atan2f(dirY, dirX) * 57.2957795f;
            }

            // Center of car is ~2.0m behind the front bumper contact point
            float carX = contactX - dirX * 200.0f;
            float carY = contactY - dirY * 200.0f;
            float carZ = contactZ + 70.0f; // Half-height = 70cm, bottom touches road, top at 140cm

            if (g_fnSetWorldLocationAndRotation) {
                Parms_SetWorldLocationAndRotation locRot{};
                locRot.NewLocation[0] = carX;
                locRot.NewLocation[1] = carY;
                locRot.NewLocation[2] = carZ;
                locRot.NewRotation[0] = 0.0f;
                locRot.NewRotation[1] = yawDeg;
                locRot.NewRotation[2] = 0.0f;
                locRot.bSweep = false;
                locRot.bTeleport = true;
                pOriginalProcessEvent(meshComp, g_fnSetWorldLocationAndRotation, &locRot);
                Log("[LightPoleFix]   -> Car Collider Box spawned at (%.2f, %.2f, %.2f), Yaw=%.1f!\n",
                    carX, carY, carZ, yawDeg);
            }

            // Register for kinematic movement over next 2.5 seconds
            RegisterCarCollider(meshComp, carX, carY, carZ, vx, vy, yawDeg, 2.5f);
            Log("[LightPoleFix]   -> Registered dynamic car collider tracking for 2.5s!\n");
        }

        // ==============================================================
        // 3. CONFIGURE BROKEN MESH: NATURAL GRAVITY & BELIEVABLE DAMPING
        // ==============================================================
        if (brokenMeshComp) {
            if (g_fnSetCollisionEnabled) {
                Parms_SetCollisionEnabled colParms = { 3 }; // QueryAndPhysics
                pOriginalProcessEvent(brokenMeshComp, g_fnSetCollisionEnabled, &colParms);
            }

            if (g_fnClearMoveIgnoreComponents) {
                pOriginalProcessEvent(brokenMeshComp, g_fnClearMoveIgnoreComponents, nullptr);
            }
            if (floorComp && g_fnIgnoreComponentWhenMoving) {
                Parms_IgnoreComponentWhenMoving ign = { floorComp, false };
                pOriginalProcessEvent(brokenMeshComp, g_fnIgnoreComponentWhenMoving, &ign);
            }
            if (meshComp && g_fnIgnoreComponentWhenMoving) {
                Parms_IgnoreComponentWhenMoving ign = { meshComp, false };
                pOriginalProcessEvent(brokenMeshComp, g_fnIgnoreComponentWhenMoving, &ign);
            }

            // Continuous Collision Detection (CCD)
            if (g_fnSetAllUseCCD) {
                Parms_SetAllUseCCD ccdParms = { true };
                pOriginalProcessEvent(brokenMeshComp, g_fnSetAllUseCCD, &ccdParms);
            }

            // NATURAL LINEAR DAMPING (0.05f = full gravity! No slow-motion floating!)
            if (g_fnSetLinearDamping) {
                Parms_SetLinearDamping linDamp = { 0.05f };
                pOriginalProcessEvent(brokenMeshComp, g_fnSetLinearDamping, &linDamp);
                Log("[LightPoleFix]   -> Natural LinearDamping (0.05f) applied!\n");
            }

            // GENTLE ANGULAR DAMPING (0.40f = natural tumble, eliminates infinite spinning)
            if (g_fnSetAngularDamping) {
                Parms_SetAngularDamping angDamp = { 0.40f };
                pOriginalProcessEvent(brokenMeshComp, g_fnSetAngularDamping, &angDamp);
                Log("[LightPoleFix]   -> Natural AngularDamping (0.40f) applied!\n");
            }

            // Cap depenetration velocity to prevent violent launches
            *(float*)((uintptr_t)brokenMeshComp + 0x02C0 + 0x00A8) = 150.0f;

            // Enable Sensitive Sleep Family so resting debris comes to a clean stop on the ground
            *(uint8_t*)((uintptr_t)brokenMeshComp + 0x02C0 + 0x0059) = 1;

            Log("[LightPoleFix]   -> Natural physics simulation active on BrokenMesh!\n");
        }

    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[LightPoleFix] Exception in FixBrokenProp!\n");
    }
}

// ===== ProcessEvent Hook =====
static void Hooked_ProcessEvent(void* Context, void* Function, void* Parms) {
    if (!Function || !Context) {
        pOriginalProcessEvent(Context, Function, Parms);
        return;
    }

    // Always update active car kinematic colliders on the game thread
    UpdateActiveCarColliders();

    int32_t funcNameIdx = *(int32_t*)((uintptr_t)Function + 0x18);

    // 1. Block RemoveFloor calls permanently
    if (funcNameIdx == s_RemoveFloorIdx && s_RemoveFloorIdx != -1) {
        s_BlockedRemoveFloorCount++;
        if (s_BlockedRemoveFloorCount <= 5 || (s_BlockedRemoveFloorCount % 20 == 0)) {
            Log("[LightPoleFix] Blocked RemoveFloor #%llu! Floor collision preserved.\n", s_BlockedRemoveFloorCount);
        }
        return;
    }

    // 2. Block AddRandomOutwardVelocityToAllBodies permanently (kills artificial explosions)
    if (funcNameIdx == s_AddRandomOutwardVelocityIdx && s_AddRandomOutwardVelocityIdx != -1) {
        s_BlockedAddRandomCount++;
        if (s_BlockedAddRandomCount <= 10 || (s_BlockedAddRandomCount % 20 == 0)) {
            Log("[LightPoleFix] Blocked AddRandomOutwardVelocityToAllBodies #%llu! Artificial explosion neutralized.\n",
                s_BlockedAddRandomCount);
        }
        return; // DROP CALL!
    }

    // 3. Tame AddImpulseAtLocationForAllBodiesBelow (kills skyward launch)
    if (funcNameIdx == s_AddImpulseAtLocationIdx && s_AddImpulseAtLocationIdx != -1) {
        s_TunedImpulseCount++;
        if (Parms) {
            float* pImpulse = (float*)((uintptr_t)Parms + 0x00);
            pImpulse[2] = 0.0f; // Zero out vertical launch
            float horiz = sqrtf(pImpulse[0] * pImpulse[0] + pImpulse[1] * pImpulse[1]);
            if (horiz > 600.0f) {
                float s = 600.0f / horiz;
                pImpulse[0] *= s;
                pImpulse[1] *= s;
            }
        }
        pOriginalProcessEvent(Context, Function, Parms);
        return;
    }

    // 4. Discover FName indices
    if (s_RemoveFloorIdx == -1 || s_SetupBrokenIdx == -1 ||
        s_AddRandomOutwardVelocityIdx == -1 || s_AddImpulseAtLocationIdx == -1) {
        char nameBuf[256];
        if (GetFNameString(funcNameIdx, nameBuf, sizeof(nameBuf))) {
            if (strcmp(nameBuf, "RemoveFloor") == 0) {
                s_RemoveFloorIdx = funcNameIdx;
                s_BlockedRemoveFloorCount++;
                Log("[LightPoleFix] Discovered RemoveFloor (FName=%d). Blocked.\n", funcNameIdx);
                return;
            } else if (strcmp(nameBuf, "SetupBroken") == 0) {
                s_SetupBrokenIdx = funcNameIdx;
                Log("[LightPoleFix] Discovered SetupBroken (FName=%d).\n", funcNameIdx);
            } else if (strcmp(nameBuf, "AddRandomOutwardVelocityToAllBodies") == 0) {
                s_AddRandomOutwardVelocityIdx = funcNameIdx;
                s_BlockedAddRandomCount++;
                Log("[LightPoleFix] Discovered AddRandomOutwardVelocityToAllBodies (FName=%d). Blocked.\n", funcNameIdx);
                return;
            } else if (strcmp(nameBuf, "AddImpulseAtLocationForAllBodiesBelow") == 0) {
                s_AddImpulseAtLocationIdx = funcNameIdx;
                Log("[LightPoleFix] Discovered AddImpulseAtLocationForAllBodiesBelow (FName=%d).\n", funcNameIdx);
            }
        }
    }

    // 5. Intercept SetupBroken
    if (funcNameIdx == s_SetupBrokenIdx && s_SetupBrokenIdx != -1) {
        s_PatchedSetupBrokenCount++;
        Log("\n[LightPoleFix] >>> INTERCEPTED SetupBroken #%llu (Actor=0x%p, Func=0x%p) <<<\n",
            s_PatchedSetupBrokenCount, Context, Function);

        ResolveEngineFunctions();

        // Tame the initial impulse before original game logic receives it!
        if (Parms) {
            float* pImpulseVel = (float*)((uintptr_t)Parms + 0x0C);
            Log("[LightPoleFix]   Original ImpulseVelocity: (%.2f, %.2f, %.2f)\n",
                pImpulseVel[0], pImpulseVel[1], pImpulseVel[2]);

            // Eliminate skyward rocket launch
            pImpulseVel[2] = 0.0f;

            // Cap excessive horizontal kicks to realistic bumper shove (<= 800 cm/s = 28 km/h)
            float horizSpeed = sqrtf(pImpulseVel[0] * pImpulseVel[0] + pImpulseVel[1] * pImpulseVel[1]);
            if (horizSpeed > 800.0f) {
                float s = 800.0f / horizSpeed;
                pImpulseVel[0] *= s;
                pImpulseVel[1] *= s;
            }
            Log("[LightPoleFix]   Tuned ImpulseVelocity: (%.2f, %.2f, %.2f)\n",
                pImpulseVel[0], pImpulseVel[1], pImpulseVel[2]);
        }

        // Run game logic (spawns BrokenMesh with tuned horizontal-only impulse)
        pOriginalProcessEvent(Context, Function, Parms);

        // Apply ground collision, car kinematic collider, and natural physics tuning
        FixBrokenProp(Context, Parms);

        Log("[LightPoleFix] <<< SetupBroken #%llu complete! Natural physics active. >>>\n\n",
            s_PatchedSetupBrokenCount);
        return;
    }

    // Default: pass through
    pOriginalProcessEvent(Context, Function, Parms);
}

// Background worker: Configure PhysX substepping
static DWORD WINAPI PhysicsInitThread(LPVOID lpParam) {
    Log("[LightPoleFix] Background worker started. Waiting for UPhysicsSettings in GObjects...\n");

    bool substeppingConfigured = false;
    for (int attempts = 0; attempts < 60 && !substeppingConfigured; ++attempts) {
        Sleep(1000);
        if (!g_Objects || g_Objects->NumElements <= 0) continue;

        ResolveEngineFunctions();

        __try {
            for (int32_t i = 0; i < g_Objects->NumElements; ++i) {
                int32_t chunkIdx = i / 65536;
                int32_t inChunk  = i % 65536;
                if (chunkIdx >= g_Objects->NumChunks || !g_Objects->Objects[chunkIdx]) continue;
                FUObjectItem* item = &g_Objects->Objects[chunkIdx][inChunk];
                if (!item || !item->Object) continue;
                void* obj = item->Object;

                int32_t nameIdx = *(int32_t*)((uintptr_t)obj + 0x18);
                char nameBuf[128];
                if (GetFNameString(nameIdx, nameBuf, sizeof(nameBuf))) {
                    if (strstr(nameBuf, "PhysicsSettings") != nullptr) {
                        void* uclass = *(void**)((uintptr_t)obj + 0x10);
                        if (uclass) {
                            char clsName[64];
                            if (GetFNameString(*(int32_t*)((uintptr_t)uclass + 0x18), clsName, sizeof(clsName))) {
                                if (strstr(clsName, "PhysicsSettings") != nullptr) {
                                    *(uint8_t*)((uintptr_t)obj + 0x0038) = 1;      // bSubstepping = true
                                    *(int32_t*)((uintptr_t)obj + 0x0040) = 4;      // MaxSubsteps = 4
                                    *(float*)((uintptr_t)obj + 0x0044)   = 0.0167f;// MaxSubstepDeltaTime = 1/60s
                                    substeppingConfigured = true;
                                    Log("[LightPoleFix] Successfully enabled PhysX substepping in %s (MaxSubsteps=4, MaxDelta=0.0167)!\n", nameBuf);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[LightPoleFix] Exception while scanning for UPhysicsSettings.\n");
        }
    }

    return 0;
}

// ===== DllMain =====
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        char logPath[MAX_PATH];
        GetModuleFileNameA(hModule, logPath, MAX_PATH);
        char* lastSlash = strrchr(logPath, '\\');
        if (lastSlash) *(lastSlash + 1) = '\0';
        strcat_s(logPath, "GTA_Prop_Fix.log");
        fopen_s(&g_LogFile, logPath, "w");

        Log("============================================================\n");
        Log("GTA SA Definitive Edition - Street Props Physics Fix v0.0.1\n");
        Log("============================================================\n");

        uintptr_t imageBase = (uintptr_t)GetModuleHandleA(NULL);
        Log("[LightPoleFix] Target ImageBase: 0x%p\n", (void*)imageBase);

        g_Names = (FNamePool*)(imageBase + RVA_GNames);
        g_Objects = (TUObjectArray*)(imageBase + RVA_GObjects);
        Log("[LightPoleFix] GNames at 0x%p, GObjects at 0x%p\n", g_Names, g_Objects);

        uintptr_t targetProcessEvent = imageBase + RVA_ProcessEvent;
        Log("[LightPoleFix] Target UObject::ProcessEvent: 0x%p\n", (void*)targetProcessEvent);

        if (MH_Initialize() != MH_OK) {
            Log("[LightPoleFix] ERROR: MH_Initialize failed!\n");
            return TRUE;
        }

        if (MH_CreateHook((LPVOID)targetProcessEvent, (LPVOID)&Hooked_ProcessEvent, (LPVOID*)&pOriginalProcessEvent) != MH_OK) {
            Log("[LightPoleFix] ERROR: MH_CreateHook failed!\n");
            return TRUE;
        }

        if (MH_EnableHook((LPVOID)targetProcessEvent) != MH_OK) {
            Log("[LightPoleFix] ERROR: MH_EnableHook failed!\n");
            return TRUE;
        }

        Log("[LightPoleFix] Hook successfully installed on UObject::ProcessEvent!\n");
        Log("[LightPoleFix] Ready: Anti-explosion, car collision, and flush ground active.\n");

        CreateThread(NULL, 0, PhysicsInitThread, NULL, 0, NULL);
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        if (g_LogFile) {
            Log("[LightPoleFix] Plugin unloaded cleanly.\n");
            fclose(g_LogFile);
            g_LogFile = nullptr;
        }
    }
    return TRUE;
}

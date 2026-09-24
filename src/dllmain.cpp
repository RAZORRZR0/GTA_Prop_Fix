#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "../minhook/MinHook.h"

// =======================================================================
// GTA SA Definitive Edition - Street Props Ground & Vehicle Collision Fix v0.0.2
//
// Dual-Layer Architecture:
// 1. FLUSH GROUND COLLIDER (100% Solid WorldStatic Barrier):
//    - Blocks RemoveFloor permanently.
//    - Transforms PhysicsFloor into a 150m x 150m x 0.2m flush road barrier.
//    - Decoupled from actor hierarchy (KeepWorld).
// 2. STANDALONE DYNAMIC VEHICLE COLLIDER (Phase 1):
//    - Spawns an independent AActor with Owner = nullptr (no same-actor
//      PhysX collision suppression).
//    - Shapes it to car dimensions (2.2m x 4.8m x 1.4m), ECC_WorldDynamic,
//      blocking ECC_PhysicsBody.
//    - Kinematically translates it along the vehicle's velocity vector for
//      2.5 seconds at 60 Hz so falling props bounce off the hood and roof!
//    - Cleanly destroyed via K2_DestroyActor when the car clears the crash zone.
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

// ===== Engine UFunction Pointers =====
static void* g_fnSetMobility = nullptr;                     // USceneComponent::SetMobility
static void* g_fnK2_DetachFromComponent = nullptr;          // USceneComponent::K2_DetachFromComponent
static void* g_fnSetAbsolute = nullptr;                     // USceneComponent::SetAbsolute
static void* g_fnSetWorldLocationAndRotation = nullptr;     // USceneComponent::K2_SetWorldLocationAndRotation
static void* g_fnSetWorldScale3D = nullptr;                 // USceneComponent::SetWorldScale3D
static void* g_fnSetCollisionObjectType = nullptr;          // UPrimitiveComponent::SetCollisionObjectType
static void* g_fnSetCollisionEnabled = nullptr;             // UPrimitiveComponent::SetCollisionEnabled
static void* g_fnSetCollisionResponseToAllChannels = nullptr; // UPrimitiveComponent::SetCollisionResponseToAllChannels
static void* g_fnSetCollisionResponseToChannel = nullptr;   // UPrimitiveComponent::SetCollisionResponseToChannel
static void* g_fnIgnoreComponentWhenMoving = nullptr;       // UPrimitiveComponent::IgnoreComponentWhenMoving
static void* g_fnClearMoveIgnoreComponents = nullptr;       // UPrimitiveComponent::ClearMoveIgnoreComponents
static void* g_fnSetAllUseCCD = nullptr;                    // UPrimitiveComponent::SetAllUseCCD
static void* g_fnSetStaticMesh = nullptr;                   // UStaticMeshComponent::SetStaticMesh
static void* g_meshBreakableFloor = nullptr;                // StaticMesh Breakable_Floor

// Spawning functions (UGameplayStatics / AActor)
static void* g_fnBeginDeferredActorSpawn = nullptr;         // UGameplayStatics::BeginDeferredActorSpawnFromClass
static void* g_fnFinishSpawningActor = nullptr;             // UGameplayStatics::FinishSpawningActor
static void* g_fnK2_DestroyActor = nullptr;                 // AActor::K2_DestroyActor
static void* g_classStaticMeshActor = nullptr;              // UClass StaticMeshActor

static volatile bool g_bFunctionsResolved = false;

// ===== Parameter Structs =====
#pragma pack(push, 1)
struct Parms_SetMobility {
    uint8_t NewMobility; // 2 = Movable
};
struct Parms_K2_DetachFromComponent {
    uint8_t LocationRule; // 1 = KeepWorld
    uint8_t RotationRule; // 1 = KeepWorld
    uint8_t ScaleRule;    // 1 = KeepWorld
    bool bCallModify;
};
struct Parms_SetAbsolute {
    bool bNewAbsoluteLocation;
    bool bNewAbsoluteRotation;
    bool bNewAbsoluteScale;
};
struct Parms_SetWorldLocationAndRotation {
    float NewLocation[3];
    float NewRotation[3];
    bool bSweep;
    uint8_t Pad_19[3];
    uint8_t SweepHitResult[0x8C];
    bool bTeleport;
    uint8_t Pad_A9[3];
};
struct Parms_SetWorldScale3D {
    float NewScale[3];
};
struct Parms_SetCollisionObjectType {
    uint8_t Channel; // 0 = ECC_WorldStatic, 1 = ECC_WorldDynamic
};
struct Parms_SetCollisionEnabled {
    uint8_t NewType; // 3 = QueryAndPhysics
};
struct Parms_SetCollisionResponseToAllChannels {
    uint8_t NewResponse; // 0 = ECR_Ignore, 2 = ECR_Block
};
struct Parms_SetCollisionResponseToChannel {
    uint8_t Channel;
    uint8_t NewResponse;
};
struct Parms_IgnoreComponentWhenMoving {
    void* Component;
    bool bShouldIgnore;
};
struct Parms_SetAllUseCCD {
    bool InUseCCD;
};
struct Parms_SetStaticMesh {
    void* NewMesh;
    bool ReturnValue;
};

// Spawning structs
struct Parms_BeginDeferredActorSpawnFromClass {
    void* WorldContextObject;      // 0x00
    void* ActorClass;              // 0x08
    float SpawnTransform_Rot[4];   // 0x10 (X, Y, Z, W)
    float SpawnTransform_Trans[3]; // 0x20 (X, Y, Z)
    float SpawnTransform_Scale[3]; // 0x2C (X, Y, Z)
    uint32_t Pad_38;
    uint32_t Pad_3C;
    uint8_t CollisionHandlingOverride; // 0x40 (1 = AlwaysSpawn)
    uint8_t Pad_41[7];
    void* Owner;                   // 0x48 (nullptr)
    void* ReturnValue;             // 0x50 (AActor*)
};
struct Parms_FinishSpawningActor {
    void* Actor;                   // 0x00
    uint8_t Pad_08[8];
    float SpawnTransform_Rot[4];   // 0x10
    float SpawnTransform_Trans[3]; // 0x20
    float SpawnTransform_Scale[3]; // 0x2C
    uint32_t Pad_38;
    uint32_t Pad_3C;
    void* ReturnValue;             // 0x40
};
#pragma pack(pop)

// ===== Active Standalone Vehicle Collider Tracker =====
struct ActiveVehicleActor {
    void* Actor;
    void* TargetComp;
    float PosX, PosY, PosZ;
    float VelX, VelY;
    float Yaw;
    DWORD StartTime;
    DWORD LastUpdateTime;
    float DurationSec;
    bool Active;
};
static const int MAX_VEHICLE_ACTORS = 4;
static ActiveVehicleActor g_VehicleActors[MAX_VEHICLE_ACTORS] = {};

static void UpdateActiveVehicleActors() {
    if (!g_fnSetWorldLocationAndRotation || !pOriginalProcessEvent) return;
    DWORD now = GetTickCount();

    for (int i = 0; i < MAX_VEHICLE_ACTORS; ++i) {
        if (!g_VehicleActors[i].Active || !g_VehicleActors[i].Actor) continue;

        float elapsed = (now - g_VehicleActors[i].StartTime) / 1000.0f;
        if (elapsed >= g_VehicleActors[i].DurationSec) {
            // Clean up standalone vehicle actor
            if (g_fnK2_DestroyActor) {
                pOriginalProcessEvent(g_VehicleActors[i].Actor, g_fnK2_DestroyActor, nullptr);
                Log("[GTA_Prop_Fix] Destroyed Standalone Vehicle Actor at 0x%p (elapsed=%.1fs).\n",
                    g_VehicleActors[i].Actor, elapsed);
            }
            g_VehicleActors[i].Active = false;
            g_VehicleActors[i].Actor = nullptr;
            g_VehicleActors[i].TargetComp = nullptr;
            continue;
        }

        DWORD deltaMs = now - g_VehicleActors[i].LastUpdateTime;
        if (deltaMs < 16) continue;

        float dt = deltaMs / 1000.0f;
        g_VehicleActors[i].LastUpdateTime = now;
        g_VehicleActors[i].PosX += g_VehicleActors[i].VelX * dt;
        g_VehicleActors[i].PosY += g_VehicleActors[i].VelY * dt;

        void* target = g_VehicleActors[i].TargetComp ? g_VehicleActors[i].TargetComp : g_VehicleActors[i].Actor;
        Parms_SetWorldLocationAndRotation locRot{};
        locRot.NewLocation[0] = g_VehicleActors[i].PosX;
        locRot.NewLocation[1] = g_VehicleActors[i].PosY;
        locRot.NewLocation[2] = g_VehicleActors[i].PosZ;
        locRot.NewRotation[0] = 0.0f;
        locRot.NewRotation[1] = g_VehicleActors[i].Yaw;
        locRot.NewRotation[2] = 0.0f;
        locRot.bSweep = false;
        locRot.bTeleport = false; // Kinematic push: physically deflects dynamic rigid bodies!
        pOriginalProcessEvent(target, g_fnSetWorldLocationAndRotation, &locRot);
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
            Log("[GTA_Prop_Fix]   Found SetMobility at 0x%p\n", obj);
        } else if (!g_fnK2_DetachFromComponent && strcmp(nameBuf, "K2_DetachFromComponent") == 0) {
            g_fnK2_DetachFromComponent = obj;
            Log("[GTA_Prop_Fix]   Found K2_DetachFromComponent at 0x%p\n", obj);
        } else if (!g_fnSetAbsolute && strcmp(nameBuf, "SetAbsolute") == 0) {
            g_fnSetAbsolute = obj;
            Log("[GTA_Prop_Fix]   Found SetAbsolute at 0x%p\n", obj);
        } else if (!g_fnSetWorldLocationAndRotation && strcmp(nameBuf, "K2_SetWorldLocationAndRotation") == 0) {
            g_fnSetWorldLocationAndRotation = obj;
            Log("[GTA_Prop_Fix]   Found K2_SetWorldLocationAndRotation at 0x%p\n", obj);
        } else if (!g_fnSetWorldScale3D && strcmp(nameBuf, "SetWorldScale3D") == 0) {
            g_fnSetWorldScale3D = obj;
            Log("[GTA_Prop_Fix]   Found SetWorldScale3D at 0x%p\n", obj);
        } else if (!g_fnSetCollisionObjectType && strcmp(nameBuf, "SetCollisionObjectType") == 0) {
            g_fnSetCollisionObjectType = obj;
            Log("[GTA_Prop_Fix]   Found SetCollisionObjectType at 0x%p\n", obj);
        } else if (!g_fnSetCollisionEnabled && strcmp(nameBuf, "SetCollisionEnabled") == 0) {
            g_fnSetCollisionEnabled = obj;
            Log("[GTA_Prop_Fix]   Found SetCollisionEnabled at 0x%p\n", obj);
        } else if (!g_fnSetCollisionResponseToAllChannels && strcmp(nameBuf, "SetCollisionResponseToAllChannels") == 0) {
            g_fnSetCollisionResponseToAllChannels = obj;
            Log("[GTA_Prop_Fix]   Found SetCollisionResponseToAllChannels at 0x%p\n", obj);
        } else if (!g_fnSetCollisionResponseToChannel && strcmp(nameBuf, "SetCollisionResponseToChannel") == 0) {
            g_fnSetCollisionResponseToChannel = obj;
            Log("[GTA_Prop_Fix]   Found SetCollisionResponseToChannel at 0x%p\n", obj);
        } else if (!g_fnIgnoreComponentWhenMoving && strcmp(nameBuf, "IgnoreComponentWhenMoving") == 0) {
            g_fnIgnoreComponentWhenMoving = obj;
            Log("[GTA_Prop_Fix]   Found IgnoreComponentWhenMoving at 0x%p\n", obj);
        } else if (!g_fnClearMoveIgnoreComponents && strcmp(nameBuf, "ClearMoveIgnoreComponents") == 0) {
            g_fnClearMoveIgnoreComponents = obj;
            Log("[GTA_Prop_Fix]   Found ClearMoveIgnoreComponents at 0x%p\n", obj);
        } else if (!g_fnSetAllUseCCD && strcmp(nameBuf, "SetAllUseCCD") == 0) {
            g_fnSetAllUseCCD = obj;
            Log("[GTA_Prop_Fix]   Found SetAllUseCCD at 0x%p\n", obj);
        } else if (!g_fnSetStaticMesh && strcmp(nameBuf, "SetStaticMesh") == 0) {
            g_fnSetStaticMesh = obj;
            Log("[GTA_Prop_Fix]   Found SetStaticMesh at 0x%p\n", obj);
        } else if (!g_fnBeginDeferredActorSpawn && strcmp(nameBuf, "BeginDeferredActorSpawnFromClass") == 0) {
            g_fnBeginDeferredActorSpawn = obj;
            Log("[GTA_Prop_Fix]   Found BeginDeferredActorSpawnFromClass at 0x%p\n", obj);
        } else if (!g_fnFinishSpawningActor && strcmp(nameBuf, "FinishSpawningActor") == 0) {
            g_fnFinishSpawningActor = obj;
            Log("[GTA_Prop_Fix]   Found FinishSpawningActor at 0x%p\n", obj);
        } else if (!g_fnK2_DestroyActor && strcmp(nameBuf, "K2_DestroyActor") == 0) {
            g_fnK2_DestroyActor = obj;
            Log("[GTA_Prop_Fix]   Found K2_DestroyActor at 0x%p\n", obj);
        } else if (!g_classStaticMeshActor && strcmp(nameBuf, "StaticMeshActor") == 0) {
            void* uclass = *(void**)((uintptr_t)obj + 0x10);
            if (uclass) {
                char clsName[64];
                if (GetFNameString(*(int32_t*)((uintptr_t)uclass + 0x18), clsName, sizeof(clsName))) {
                    if (strcmp(clsName, "Class") == 0) {
                        g_classStaticMeshActor = obj;
                        Log("[GTA_Prop_Fix]   Found UClass StaticMeshActor at 0x%p\n", obj);
                    }
                }
            }
        } else if (!g_meshBreakableFloor && strcmp(nameBuf, "Breakable_Floor") == 0) {
            void* uclass = *(void**)((uintptr_t)obj + 0x10);
            if (uclass) {
                char clsName[64];
                if (GetFNameString(*(int32_t*)((uintptr_t)uclass + 0x18), clsName, sizeof(clsName))) {
                    if (strcmp(clsName, "StaticMesh") == 0) {
                        g_meshBreakableFloor = obj;
                        Log("[GTA_Prop_Fix]   Found StaticMesh Breakable_Floor at 0x%p\n", obj);
                    }
                }
            }
        }
    }

    if (g_fnSetMobility && g_fnSetWorldLocationAndRotation && g_fnSetCollisionObjectType) {
        g_bFunctionsResolved = true;
        Log("[GTA_Prop_Fix] Engine functions successfully resolved!\n");
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

// ===== Standalone Vehicle PhysX Collider Spawner =====
static void SpawnStandaloneVehicleCollider(void* Context, float contactX, float contactY, float contactZ, float vx, float vy, float speed) {
    if (!Context || !g_fnBeginDeferredActorSpawn || !g_fnFinishSpawningActor) return;

    void* actorClass = g_classStaticMeshActor;
    if (!actorClass) actorClass = *(void**)((uintptr_t)Context + 0x10); // Fallback: Breakable Actor class
    if (!actorClass) return;

    float dirX = 0.0f;
    float dirY = 0.0f;
    float yawDeg = 0.0f;
    if (speed > 50.0f) {
        dirX = vx / speed;
        dirY = vy / speed;
        yawDeg = atan2f(dirY, dirX) * 57.2957795f;
    }

    // Vehicle center is ~2.0m behind front bumper impact
    float carX = contactX - dirX * 200.0f;
    float carY = contactY - dirY * 200.0f;
    float carZ = contactZ + 70.0f; // Half-height = 70cm, bottom at road, top at 140cm

    Parms_BeginDeferredActorSpawnFromClass beginParms{};
    beginParms.WorldContextObject = Context;
    beginParms.ActorClass = actorClass;
    beginParms.SpawnTransform_Rot[0] = 0.0f;
    beginParms.SpawnTransform_Rot[1] = 0.0f;
    beginParms.SpawnTransform_Rot[2] = 0.0f;
    beginParms.SpawnTransform_Rot[3] = 1.0f;
    beginParms.SpawnTransform_Trans[0] = carX;
    beginParms.SpawnTransform_Trans[1] = carY;
    beginParms.SpawnTransform_Trans[2] = carZ;
    beginParms.SpawnTransform_Scale[0] = 1.0f;
    beginParms.SpawnTransform_Scale[1] = 1.0f;
    beginParms.SpawnTransform_Scale[2] = 1.0f;
    beginParms.CollisionHandlingOverride = 1; // AlwaysSpawn
    beginParms.Owner = nullptr;               // STANDALONE INDEPENDENT ACTOR!

    pOriginalProcessEvent(Context, g_fnBeginDeferredActorSpawn, &beginParms);
    void* newActor = beginParms.ReturnValue;
    if (!newActor) {
        Log("[GTA_Prop_Fix] BeginDeferredActorSpawnFromClass returned null!\n");
        return;
    }

    Parms_FinishSpawningActor finishParms{};
    finishParms.Actor = newActor;
    finishParms.SpawnTransform_Rot[0] = 0.0f;
    finishParms.SpawnTransform_Rot[1] = 0.0f;
    finishParms.SpawnTransform_Rot[2] = 0.0f;
    finishParms.SpawnTransform_Rot[3] = 1.0f;
    finishParms.SpawnTransform_Trans[0] = carX;
    finishParms.SpawnTransform_Trans[1] = carY;
    finishParms.SpawnTransform_Trans[2] = carZ;
    finishParms.SpawnTransform_Scale[0] = 1.0f;
    finishParms.SpawnTransform_Scale[1] = 1.0f;
    finishParms.SpawnTransform_Scale[2] = 1.0f;
    pOriginalProcessEvent(Context, g_fnFinishSpawningActor, &finishParms);

    Log("[GTA_Prop_Fix] Spawned Standalone Vehicle Actor at 0x%p, Pos=(%.2f, %.2f, %.2f)!\n",
        newActor, carX, carY, carZ);

    // Resolve component on new actor
    void* newFloor = nullptr;
    void* newBroken = nullptr;
    void* newMesh = nullptr;
    ResolveComponents(newActor, &newFloor, &newBroken, &newMesh);

    void* targetComp = newFloor ? newFloor : (newMesh ? newMesh : nullptr);
    if (!targetComp) {
        targetComp = *(void**)((uintptr_t)newActor + 0x0130); // AActor::RootComponent
    }

    if (targetComp) {
        if (g_meshBreakableFloor && g_fnSetStaticMesh) {
            Parms_SetStaticMesh meshParms = { g_meshBreakableFloor, false };
            pOriginalProcessEvent(targetComp, g_fnSetStaticMesh, &meshParms);
        }
        if (g_fnSetMobility) {
            Parms_SetMobility mob = { 2 }; // Movable
            pOriginalProcessEvent(targetComp, g_fnSetMobility, &mob);
        }
        if (g_fnSetWorldScale3D) {
            Parms_SetWorldScale3D scale{};
            scale.NewScale[0] = 2.2f;
            scale.NewScale[1] = 4.8f;
            scale.NewScale[2] = 1.4f;
            pOriginalProcessEvent(targetComp, g_fnSetWorldScale3D, &scale);
        }
        if (g_fnSetCollisionObjectType) {
            Parms_SetCollisionObjectType type = { 1 }; // ECC_WorldDynamic
            pOriginalProcessEvent(targetComp, g_fnSetCollisionObjectType, &type);
        }
        if (g_fnSetCollisionEnabled) {
            Parms_SetCollisionEnabled col = { 3 }; // QueryAndPhysics
            pOriginalProcessEvent(targetComp, g_fnSetCollisionEnabled, &col);
        }
        if (g_fnSetCollisionResponseToAllChannels) {
            Parms_SetCollisionResponseToAllChannels resp = { 0 }; // ECR_Ignore
            pOriginalProcessEvent(targetComp, g_fnSetCollisionResponseToAllChannels, &resp);
        }
        SetChannelResponse(targetComp, 5, 2); // ECC_PhysicsBody -> ECR_Block
        SetChannelResponse(targetComp, 7, 2); // ECC_Destructible -> ECR_Block

        Log("[GTA_Prop_Fix] Configured Vehicle Box Component at 0x%p (Scale: 2.2 x 4.8 x 1.4)!\n", targetComp);
    }

    // Register active vehicle actor
    for (int i = 0; i < MAX_VEHICLE_ACTORS; ++i) {
        if (!g_VehicleActors[i].Active) {
            g_VehicleActors[i].Actor = newActor;
            g_VehicleActors[i].TargetComp = targetComp;
            g_VehicleActors[i].PosX = carX;
            g_VehicleActors[i].PosY = carY;
            g_VehicleActors[i].PosZ = carZ;
            g_VehicleActors[i].VelX = vx;
            g_VehicleActors[i].VelY = vy;
            g_VehicleActors[i].Yaw = yawDeg;
            DWORD now = GetTickCount();
            g_VehicleActors[i].StartTime = now;
            g_VehicleActors[i].LastUpdateTime = now;
            g_VehicleActors[i].DurationSec = 2.5f;
            g_VehicleActors[i].Active = true;
            Log("[GTA_Prop_Fix] Standalone Vehicle Actor registered for tracking (Slot %d, 2.5s).\n", i);
            return;
        }
    }
}

// ===== Core Physics Fix on SetupBroken =====
static void FixBrokenProp(void* Context, void* Parms) {
    if (!Context || !Parms) return;

    __try {
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

        Log("[GTA_Prop_Fix] Impact: Contact=(%.2f, %.2f, %.2f), Speed=%.1f km/h\n",
            contactX, contactY, contactZ, speed * 0.036f);

        void* floorComp = nullptr;
        void* brokenMeshComp = nullptr;
        void* meshComp = nullptr;
        ResolveComponents(Context, &floorComp, &brokenMeshComp, &meshComp);

        // 1. Configure & Teleport the Flush Ground Floor Collider
        if (floorComp) {
            void* curMesh = *(void**)((uintptr_t)floorComp + 0x0480);
            if (!curMesh && g_meshBreakableFloor && g_fnSetStaticMesh) {
                Parms_SetStaticMesh meshParms = { g_meshBreakableFloor, false };
                pOriginalProcessEvent(floorComp, g_fnSetStaticMesh, &meshParms);
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
                Log("[GTA_Prop_Fix] Ground Floor teleported flush at Z=%.2f\n", contactZ);
            }

            // Expand to 150m x 150m x 0.2m ground barrier
            if (g_fnSetWorldScale3D) {
                Parms_SetWorldScale3D scaleParms{};
                scaleParms.NewScale[0] = 150.0f;
                scaleParms.NewScale[1] = 150.0f;
                scaleParms.NewScale[2] = 0.2f;
                pOriginalProcessEvent(floorComp, g_fnSetWorldScale3D, &scaleParms);
            }

            if (g_fnSetCollisionObjectType) {
                Parms_SetCollisionObjectType typeParms = { 0 }; // ECC_WorldStatic
                pOriginalProcessEvent(floorComp, g_fnSetCollisionObjectType, &typeParms);
            }

            if (g_fnSetCollisionEnabled) {
                Parms_SetCollisionEnabled colParms = { 3 }; // QueryAndPhysics
                pOriginalProcessEvent(floorComp, g_fnSetCollisionEnabled, &colParms);
            }

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

        // 2. Configure BrokenMesh
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

            if (g_fnSetAllUseCCD) {
                Parms_SetAllUseCCD ccdParms = { true };
                pOriginalProcessEvent(brokenMeshComp, g_fnSetAllUseCCD, &ccdParms);
            }

            *(float*)((uintptr_t)brokenMeshComp + 0x02C0 + 0x00A8) = 150.0f;
            *(uint8_t*)((uintptr_t)brokenMeshComp + 0x02C0 + 0x0059) = 1; // Sensitive Sleep Family
        }

        // 3. Spawn Standalone Vehicle PhysX Collider
        if (speed > 50.0f) {
            SpawnStandaloneVehicleCollider(Context, contactX, contactY, contactZ, vx, vy, speed);
        }

    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[GTA_Prop_Fix] Exception in FixBrokenProp!\n");
    }
}

// ===== ProcessEvent Hook =====
static void Hooked_ProcessEvent(void* Context, void* Function, void* Parms) {
    if (!Function || !Context) {
        pOriginalProcessEvent(Context, Function, Parms);
        return;
    }

    // Advance standalone vehicle kinematic actors on the game thread
    UpdateActiveVehicleActors();

    int32_t funcNameIdx = *(int32_t*)((uintptr_t)Function + 0x18);

    // Block RemoveFloor calls permanently
    if (funcNameIdx == s_RemoveFloorIdx && s_RemoveFloorIdx != -1) {
        return;
    }

    // Discover FName indices
    if (s_RemoveFloorIdx == -1 || s_SetupBrokenIdx == -1) {
        char nameBuf[256];
        if (GetFNameString(funcNameIdx, nameBuf, sizeof(nameBuf))) {
            if (strcmp(nameBuf, "RemoveFloor") == 0) {
                s_RemoveFloorIdx = funcNameIdx;
                Log("[GTA_Prop_Fix] Discovered RemoveFloor (FName=%d). Blocked.\n", funcNameIdx);
                return;
            } else if (strcmp(nameBuf, "SetupBroken") == 0) {
                s_SetupBrokenIdx = funcNameIdx;
                Log("[GTA_Prop_Fix] Discovered SetupBroken (FName=%d).\n", funcNameIdx);
            }
        }
    }

    // Intercept SetupBroken
    if (funcNameIdx == s_SetupBrokenIdx && s_SetupBrokenIdx != -1) {
        Log("\n[GTA_Prop_Fix] >>> Intercepted SetupBroken (Context=0x%p) <<<\n", Context);
        ResolveEngineFunctions();
        pOriginalProcessEvent(Context, Function, Parms);
        FixBrokenProp(Context, Parms);
        return;
    }

    // Default: pass through
    pOriginalProcessEvent(Context, Function, Parms);
}

// Background worker: Configure PhysX substepping
static DWORD WINAPI PhysicsInitThread(LPVOID lpParam) {
    Log("[GTA_Prop_Fix] Background worker started. Waiting for UPhysicsSettings in GObjects...\n");

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
                                    Log("[GTA_Prop_Fix] Enabled PhysX substepping in %s (MaxSubsteps=4, MaxDelta=0.0167)!\n", nameBuf);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[GTA_Prop_Fix] Exception in PhysicsInitThread.\n");
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
        Log("GTA SA Definitive Edition - Street Props Ground & Vehicle Fix v0.0.2\n");
        Log("============================================================\n");

        uintptr_t imageBase = (uintptr_t)GetModuleHandleA(NULL);
        Log("[GTA_Prop_Fix] Target ImageBase: 0x%p\n", (void*)imageBase);

        g_Names = (FNamePool*)(imageBase + RVA_GNames);
        g_Objects = (TUObjectArray*)(imageBase + RVA_GObjects);

        uintptr_t targetProcessEvent = imageBase + RVA_ProcessEvent;

        if (MH_Initialize() != MH_OK) {
            Log("[GTA_Prop_Fix] ERROR: MH_Initialize failed!\n");
            return TRUE;
        }

        if (MH_CreateHook((LPVOID)targetProcessEvent, (LPVOID)&Hooked_ProcessEvent, (LPVOID*)&pOriginalProcessEvent) != MH_OK) {
            Log("[GTA_Prop_Fix] ERROR: MH_CreateHook failed!\n");
            return TRUE;
        }

        if (MH_EnableHook((LPVOID)targetProcessEvent) != MH_OK) {
            Log("[GTA_Prop_Fix] ERROR: MH_EnableHook failed!\n");
            return TRUE;
        }

        Log("[GTA_Prop_Fix] Hook successfully installed on UObject::ProcessEvent!\n");
        Log("[GTA_Prop_Fix] Ready: Flush ground barrier and standalone vehicle collider active.\n");

        CreateThread(NULL, 0, PhysicsInitThread, NULL, 0, NULL);
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        if (g_LogFile) {
            Log("[GTA_Prop_Fix] Plugin unloaded cleanly.\n");
            fclose(g_LogFile);
            g_LogFile = nullptr;
        }
    }
    return TRUE;
}

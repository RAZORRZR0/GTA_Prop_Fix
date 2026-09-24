#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "../minhook/MinHook.h"

// =======================================================================
// GTA SA Definitive Edition - Street Props Ground Fix v0.0.4
//
// Core Principle:
// - PRESERVE 100% OF BASE GAME PHYSICS: Keep stock game velocity, speed,
//   impulses, damping, and vehicle collision intact.
// - FIX THE ONLY BUG: Prevent props from falling through the world.
//   1. Block RemoveFloor permanently so the game never destroys ground collision.
//   2. Transform PhysicsFloor into a massive 150m x 150m x 0.2m flush pavement
//      barrier at road level (contactZ).
//   3. Decouple PhysicsFloor from the actor hierarchy to break PhysX same-actor
//      collision suppression.
//   4. Enable Continuous Collision Detection (CCD) and PhysX substepping.
// =======================================================================

// RVAs for SanAndreas.exe
static const uintptr_t RVA_ProcessEvent = 0x01C7B6B0;
static const uintptr_t RVA_GNames       = 0x0570CDC0;
static const uintptr_t RVA_GObjects      = 0x05086380;

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
    uint8_t Channel; // 0 = ECC_WorldStatic
};
struct Parms_SetCollisionEnabled {
    uint8_t NewType; // 3 = QueryAndPhysics
};
struct Parms_SetCollisionResponseToAllChannels {
    uint8_t NewResponse; // 2 = ECR_Block
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
#pragma pack(pop)

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
        } else if (!g_fnK2_DetachFromComponent && strcmp(nameBuf, "K2_DetachFromComponent") == 0) {
            g_fnK2_DetachFromComponent = obj;
        } else if (!g_fnSetAbsolute && strcmp(nameBuf, "SetAbsolute") == 0) {
            g_fnSetAbsolute = obj;
        } else if (!g_fnSetWorldLocationAndRotation && strcmp(nameBuf, "K2_SetWorldLocationAndRotation") == 0) {
            g_fnSetWorldLocationAndRotation = obj;
        } else if (!g_fnSetWorldScale3D && strcmp(nameBuf, "SetWorldScale3D") == 0) {
            g_fnSetWorldScale3D = obj;
        } else if (!g_fnSetCollisionObjectType && strcmp(nameBuf, "SetCollisionObjectType") == 0) {
            g_fnSetCollisionObjectType = obj;
        } else if (!g_fnSetCollisionEnabled && strcmp(nameBuf, "SetCollisionEnabled") == 0) {
            g_fnSetCollisionEnabled = obj;
        } else if (!g_fnSetCollisionResponseToAllChannels && strcmp(nameBuf, "SetCollisionResponseToAllChannels") == 0) {
            g_fnSetCollisionResponseToAllChannels = obj;
        } else if (!g_fnSetCollisionResponseToChannel && strcmp(nameBuf, "SetCollisionResponseToChannel") == 0) {
            g_fnSetCollisionResponseToChannel = obj;
        } else if (!g_fnIgnoreComponentWhenMoving && strcmp(nameBuf, "IgnoreComponentWhenMoving") == 0) {
            g_fnIgnoreComponentWhenMoving = obj;
        } else if (!g_fnClearMoveIgnoreComponents && strcmp(nameBuf, "ClearMoveIgnoreComponents") == 0) {
            g_fnClearMoveIgnoreComponents = obj;
        } else if (!g_fnSetAllUseCCD && strcmp(nameBuf, "SetAllUseCCD") == 0) {
            g_fnSetAllUseCCD = obj;
        } else if (!g_fnSetStaticMesh && strcmp(nameBuf, "SetStaticMesh") == 0) {
            g_fnSetStaticMesh = obj;
        } else if (!g_meshBreakableFloor && strcmp(nameBuf, "Breakable_Floor") == 0) {
            void* uclass = *(void**)((uintptr_t)obj + 0x10);
            if (uclass) {
                char clsName[64];
                if (GetFNameString(*(int32_t*)((uintptr_t)uclass + 0x18), clsName, sizeof(clsName))) {
                    if (strcmp(clsName, "StaticMesh") == 0) {
                        g_meshBreakableFloor = obj;
                    }
                }
            }
        }
    }

    if (g_fnSetMobility && g_fnSetWorldLocationAndRotation && g_fnSetCollisionObjectType) {
        g_bFunctionsResolved = true;
    }
}

// ===== Set Collision Channel Response Helper =====
static void SetChannelResponse(void* comp, uint8_t channel, uint8_t response) {
    if (!comp || !g_fnSetCollisionResponseToChannel) return;
    Parms_SetCollisionResponseToChannel p = { channel, response };
    pOriginalProcessEvent(comp, g_fnSetCollisionResponseToChannel, &p);
}

// ===== Component Resolver =====
static void ResolveComponents(void* Context, void** outFloor, void** outBrokenMesh) {
    *outFloor = nullptr;
    *outBrokenMesh = nullptr;
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
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// ===== Core Ground Fix on SetupBroken =====
static void FixBrokenProp(void* Context, void* Parms) {
    if (!Context || !Parms) return;

    __try {
        float* pImpulseSrc = (float*)((uintptr_t)Parms + 0x00);
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

        void* floorComp = nullptr;
        void* brokenMeshComp = nullptr;
        ResolveComponents(Context, &floorComp, &brokenMeshComp);

        // Configure & Teleport the Ground Floor Collider (Solid Pavement)
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
            // Center at (contactZ - 10.0f) so top surface is exactly at road level contactZ
            if (g_fnSetWorldLocationAndRotation) {
                Parms_SetWorldLocationAndRotation locRotParms{};
                locRotParms.NewLocation[0] = contactX;
                locRotParms.NewLocation[1] = contactY;
                locRotParms.NewLocation[2] = contactZ - 10.0f;
                locRotParms.NewRotation[0] = 0.0f;
                locRotParms.NewRotation[1] = 0.0f;
                locRotParms.NewRotation[2] = 0.0f;
                locRotParms.bSweep = false;
                locRotParms.bTeleport = true;
                pOriginalProcessEvent(floorComp, g_fnSetWorldLocationAndRotation, &locRotParms);
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

        // Enable CCD on BrokenMesh so it never tunnels through the floor
        if (brokenMeshComp) {
            if (g_fnSetCollisionEnabled) {
                Parms_SetCollisionEnabled colParms = { 3 }; // QueryAndPhysics
                pOriginalProcessEvent(brokenMeshComp, g_fnSetCollisionEnabled, &colParms);
            }
            if (floorComp && g_fnIgnoreComponentWhenMoving) {
                Parms_IgnoreComponentWhenMoving ign = { floorComp, false };
                pOriginalProcessEvent(brokenMeshComp, g_fnIgnoreComponentWhenMoving, &ign);
            }
            if (g_fnSetAllUseCCD) {
                Parms_SetAllUseCCD ccdParms = { true };
                pOriginalProcessEvent(brokenMeshComp, g_fnSetAllUseCCD, &ccdParms);
            }
            *(float*)((uintptr_t)brokenMeshComp + 0x02C0 + 0x00A8) = 100.0f;
        }

    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ===== ProcessEvent Hook =====
static void Hooked_ProcessEvent(void* Context, void* Function, void* Parms) {
    if (!Function || !Context) {
        pOriginalProcessEvent(Context, Function, Parms);
        return;
    }

    int32_t funcNameIdx = *(int32_t*)((uintptr_t)Function + 0x18);

    // Block RemoveFloor calls permanently so the ground collider is never destroyed
    if (funcNameIdx == s_RemoveFloorIdx && s_RemoveFloorIdx != -1) {
        return;
    }

    // Discover FName indices
    if (s_RemoveFloorIdx == -1 || s_SetupBrokenIdx == -1) {
        char nameBuf[256];
        if (GetFNameString(funcNameIdx, nameBuf, sizeof(nameBuf))) {
            if (strcmp(nameBuf, "RemoveFloor") == 0) {
                s_RemoveFloorIdx = funcNameIdx;
                return;
            } else if (strcmp(nameBuf, "SetupBroken") == 0) {
                s_SetupBrokenIdx = funcNameIdx;
            }
        }
    }

    // Intercept SetupBroken
    if (funcNameIdx == s_SetupBrokenIdx && s_SetupBrokenIdx != -1) {
        ResolveEngineFunctions();
        // 1. Let native game logic run completely untouched (stock speed, stock velocity, stock impulses!)
        pOriginalProcessEvent(Context, Function, Parms);
        // 2. Teleport the flush ground floor barrier under the debris
        FixBrokenProp(Context, Parms);
        return;
    }

    // Default: pass through
    pOriginalProcessEvent(Context, Function, Parms);
}

// Background worker: Configure PhysX substepping
static DWORD WINAPI PhysicsInitThread(LPVOID lpParam) {
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
                if (!GetFNameString(nameIdx, nameBuf, sizeof(nameBuf))) continue;

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
                                break;
                            }
                        }
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    return 0;
}

// ===== DllMain =====
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        uintptr_t imageBase = (uintptr_t)GetModuleHandleA(NULL);
        g_Names = (FNamePool*)(imageBase + RVA_GNames);
        g_Objects = (TUObjectArray*)(imageBase + RVA_GObjects);

        uintptr_t targetProcessEvent = imageBase + RVA_ProcessEvent;

        if (MH_Initialize() != MH_OK) return TRUE;
        if (MH_CreateHook((LPVOID)targetProcessEvent, (LPVOID)&Hooked_ProcessEvent, (LPVOID*)&pOriginalProcessEvent) != MH_OK) return TRUE;
        if (MH_EnableHook((LPVOID)targetProcessEvent) != MH_OK) return TRUE;

        CreateThread(NULL, 0, PhysicsInitThread, NULL, 0, NULL);
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }
    return TRUE;
}

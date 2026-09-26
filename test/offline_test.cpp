// Offline checks for GTA_Prop_Fix (no game required).
//  1. Plugin logic against synthetic UE/RenderWare objects (fake engine functions).
//  2. Signature scan + MinHook installation against a manually mapped SanAndreas.exe.
// Usage: offline_test.exe <path to SanAndreas.exe>
#define PROPFIX_TEST
#include "../src/dllmain.cpp"

#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
        else { printf("ok   %s\n", #cond); }                                        \
    } while (0)

// ---------------------------------------------------------------- fake engine
struct FakeObj { uint8_t mem[0x400]; };
static FakeObj* NewObj(void* cls, int32_t name) {
    auto* o = new FakeObj();
    memset(o->mem, 0, sizeof(o->mem));
    *(void**)(o->mem + UE::Obj_Class) = cls;
    *(int32_t*)(o->mem + UE::Obj_Name) = name;
    return o;
}

struct CallRec { void* obj; int32_t name; uint8_t parms[0xD0]; };
static std::vector<CallRec> g_calls;
static FakeObj* g_funcs[N_COUNT];
static int g_syncCount = 0;
static void* g_fakeFloor = nullptr;   // returned by GetPhysicsFloor
static void* g_fakeBroken = nullptr;  // returned by GetBrokenMesh
static uint8_t g_brokenObjectType = 5; // returned by GetCollisionObjectType

static void Fake_ProcessEvent(void* obj, void* func, void* parms) {
    const int32_t n = NameOf(func);
    if (n == g_nameIdx[N_GetPhysicsFloor]) *(void**)parms = g_fakeFloor;
    if (n == g_nameIdx[N_GetBrokenMesh]) *(void**)parms = g_fakeBroken;
    if (n == g_nameIdx[N_GetCollisionObjectType]) *(uint8_t*)parms = g_brokenObjectType;
    if (n == g_nameIdx[N_GetActorBounds]) *(float*)((uint8_t*)parms + 0x18) = 300.0f; // every fake prop is 6 m tall
    // Fake broken mesh: 3 bones (FName 7000+i); bone 1 has no physics body.
    if (n == g_nameIdx[N_GetNumBones]) *(int32_t*)parms = 3;
    if (n == g_nameIdx[N_GetBoneName]) *(FName*)((uint8_t*)parms + 4) = FName{ 7000 + *(int32_t*)parms, 0 };
    if (n == g_nameIdx[N_IsSimulatingPhysics]) ((uint8_t*)parms)[8] = ((FName*)parms)->Index != 7001;
    // Physics asset joints, named after the child bones 1 and 2 (bone 1 has no body); then NAME_None.
    if (n == g_nameIdx[N_FindConstraintBoneName]) {
        const int c = *(int32_t*)parms;
        *(FName*)((uint8_t*)parms + 4) = FName{ c < 2 ? 7001 + c : 0, 0 };
    }
    if (n == g_nameIdx[N_GetSocketLocation]) {
        extern float g_boneZ[3];
        const int b = ((FName*)parms)->Index - 7000;
        float* out = (float*)((uint8_t*)parms + 8);
        out[0] = 0; out[1] = 0; out[2] = (b >= 0 && b < 3) ? g_boneZ[b] : 0;
    }
    if (n == g_nameIdx[N_GetSocketQuaternion]) { float* q = (float*)((uint8_t*)parms + 0x10); q[0] = q[1] = q[2] = 0; q[3] = 1; }
    if (n == g_nameIdx[N_GetClosestPointOnCollision]) { // lowest point for a query from below, highest from above
        extern float g_boneZ[3], g_bottomZ[3], g_topZ[3];
        extern bool g_noShape[3];
        uint8_t* p = (uint8_t*)parms;
        const int b = ((FName*)(p + 0x18))->Index - 7000;
        const float* pt = (const float*)p;
        float* out = (float*)(p + 0x0C);
        if (b < 0 || b >= 3 || g_noShape[b]) { memcpy(out, pt, 12); *(float*)(p + 0x20) = -1.0f; }
        else { out[0] = 0; out[1] = 0; out[2] = pt[2] < g_boneZ[b] ? g_bottomZ[b] : g_topZ[b]; *(float*)(p + 0x20) = 1000.0f; }
    }
    {
        extern float g_cmdVel[3][3], g_forcedVel[3][3];
        extern bool g_forceVel[3];
        const int b = parms ? ((FName*)parms)->Index - 7000 : -1;
        if (parms && n == g_nameIdx[N_SetPhysicsLinearVelocity]) {
            const int bb = ((FName*)((uint8_t*)parms + 0x10))->Index - 7000;
            if (bb >= 0 && bb < 3) memcpy(g_cmdVel[bb], parms, 12);
        }
        if (n == g_nameIdx[N_GetPhysicsLinearVelocity] && b >= 0 && b < 3)
            memcpy((uint8_t*)parms + 8, g_forceVel[b] ? g_forcedVel[b] : g_cmdVel[b], 12);
    }
    CallRec rec{ obj, n, {} };
    const bool wide = n == g_nameIdx[N_SetPhysicsLinearVelocity] || n == g_nameIdx[N_SetPhysicsAngularVelocityInDegrees] ||
                      n == g_nameIdx[N_BreakConstraint];
    if (parms) memcpy(rec.parms, parms, n == g_nameIdx[N_K2_SetWorldTransform] ? 0xD0 : wide ? 0x20 : 0x10);
    g_calls.push_back(rec);
}
static void* Fake_FindFunctionByName(void*, FName name, int) {
    for (int i = 0; i < N_COUNT; ++i)
        if (g_nameIdx[i] == name.Index) return g_funcs[i];
    return nullptr;
}
static void Fake_SyncActor(void*) { ++g_syncCount; }

// Mimics DE CObject::SetIsStatic: flag update, then Dislodged event on the actor.
static void Fake_SetIsStatic(void* entity, bool isStatic) {
    uint32_t& flags = *(uint32_t*)((uint8_t*)entity + GTA::Entity_Flags);
    flags = (flags & ~GTA::Flag_IsStatic) | (isStatic ? GTA::Flag_IsStatic : 0);
    if (!isStatic) {
        void* actor = *(void**)((uint8_t*)entity + GTA::Entity_Actor);
        Hooked_ProcessEvent(actor, g_funcs[N_Dislodged], nullptr);
    }
}
static uintptr_t Fake_PhysicalStep(void*) { return 7; }
static int g_movingListAdds = 0;
static void Fake_AddToMovingList(void*) { ++g_movingListAdds; }

// RenderWare col models (DE x64 layout). Model 0: 6 m pole with solid boxes along it. Model 200: same
// 6 m bounding box but solid shapes only at the base (the BP_MTraffic1_C case). Model 201: short box.
struct FakeColData { uint16_t nSpheres, nBoxes, nTris; uint8_t pad[2]; float* spheres; float* boxes; void* lines;
                     float* verts; int32_t* tris; };
struct FakeColModel { float bb[6]; float sphere[4]; uint8_t slot, flags, pad[6]; FakeColData* data; };
static_assert(offsetof(FakeColModel, data) == GTA::ColModel_Data, "col model layout");
static_assert(offsetof(FakeColData, boxes) == 0x10 && offsetof(FakeColData, verts) == 0x20, "col data layout");
static float g_poleBoxes[2 * 7] = { -0.2f, -0.2f, 0.0f, 0.2f, 0.2f, 3.1f, 0, -0.2f, -0.2f, 3.0f, 0.2f, 0.2f, 6.0f, 0 };
static float g_stubSphere[5] = { 0, 0, 0.3f, 0.3f, 0 };
static float g_stubVerts[9] = { 0, 0, 0.0f, 1, 0, 0.2f, 0, 1, 0.5f };
static int32_t g_stubTri[4] = { 0, 1, 2, 0 };
static FakeColData g_poleData = { 0, 2, 0, {}, nullptr, g_poleBoxes, nullptr, nullptr, nullptr };
static FakeColData g_stubData = { 1, 0, 1, {}, g_stubSphere, nullptr, nullptr, g_stubVerts, g_stubTri };
static FakeColModel g_colFull = { { -0.3f, -0.3f, 0.0f, 0.3f, 0.3f, 6.0f }, {}, 0, 0, {}, &g_poleData };
static FakeColModel g_colStub = { { -0.3f, -0.3f, 0.0f, 0.3f, 0.3f, 6.0f }, {}, 0, 0, {}, &g_stubData };
static FakeColModel g_colShort = { { -0.3f, -0.3f, 0.0f, 0.3f, 0.3f, 0.4f }, {}, 0, 0, {}, &g_stubData };
static const float* Fake_GetColModel(int model) {
    return (const float*)(model == 200 ? &g_colStub : model == 201 ? &g_colShort : &g_colFull);
}

static const void* Fake_EntityColModel(void* entity) {
    return Fake_GetColModel(*(int16_t*)((uint8_t*)entity + GTA::Entity_ModelIndex));
}

static int g_glassCalls = 0;
static float g_glassPos[3];
static uintptr_t Fake_GlassCollision(void* entity, float, float*, float* pos) {
    ++g_glassCalls;
    memcpy(g_glassPos, pos, sizeof(g_glassPos));
    *(uint32_t*)((uint8_t*)entity + GTA::Entity_Flags) &= ~GTA::Flag_UsesCollision; // what DE's CGlass does
    return 1;
}
static uintptr_t Fake_GroundProbe(void*, float* p, float* n) { p[0] = p[1] = p[2] = 0; n[0] = n[1] = 0; n[2] = 1; return 0; }

static int g_damageCalls = 0;
static uint8_t g_effectSeen = 0xFF;
static void Fake_ObjectDamage(void* entity, float, float*, float*, void*, int) {
    ++g_damageCalls;
    g_effectSeen = *((uint8_t*)entity + GTA::Entity_ColDamageEffect);
}

static bool Called(void* obj, NameId id) {
    for (auto& c : g_calls) if (c.obj == obj && c.name == g_nameIdx[id]) return true;
    return false;
}
static int IndexOfCall(void* obj, NameId id) {
    for (size_t i = 0; i < g_calls.size(); ++i)
        if (g_calls[i].obj == obj && g_calls[i].name == g_nameIdx[id]) return (int)i;
    return -1;
}
static int CountCalls(void* obj, NameId id) {
    int c = 0;
    for (auto& r : g_calls) if (r.obj == obj && r.name == g_nameIdx[id]) ++c;
    return c;
}
// Last SetCollisionResponseToChannel response for `channel` on `obj`, or -1.
static int ResponseFor(void* obj, uint8_t channel) {
    int r = -1;
    for (auto& c : g_calls)
        if (c.obj == obj && c.name == g_nameIdx[N_SetCollisionResponseToChannel] && c.parms[0] == channel) r = c.parms[1];
    return r;
}

// Fake FNamePool with ANSI and wide entries.
static FNamePool g_pool;
static uint8_t g_block0[0x20000];
static int32_t AddName(uint32_t& cursor, const char* s, bool wide = false) {
    const uint32_t len = (uint32_t)strlen(s);
    const int32_t idx = (int32_t)(cursor >> 1);
    *(uint16_t*)(g_block0 + cursor) = (uint16_t)((len << 6) | (wide ? 1 : 0));
    if (wide) for (uint32_t i = 0; i < len; ++i) *(uint16_t*)(g_block0 + cursor + 2 + i * 2) = (uint16_t)s[i];
    else memcpy(g_block0 + cursor + 2, s, len);
    cursor += (2 + len * (wide ? 2 : 1) + 1) & ~1u;
    return idx;
}

static void TestLogic() {
    printf("== synthetic logic ==\n");
    // Name pool
    memset(&g_pool, 0, sizeof(g_pool));
    uint32_t cursor = 0;
    AddName(cursor, "None");
    AddName(cursor, "Dislodged", /*wide*/ true); // wide duplicate must be skipped
    int32_t expected[N_COUNT];
    for (int i = 0; i < N_COUNT; ++i) expected[i] = AddName(cursor, kNames[i]);
    g_pool.CurrentBlock = 0;
    g_pool.CurrentByteCursor = cursor;
    g_pool.Blocks[0] = g_block0;
    g_names = &g_pool;
    for (int i = 0; i < N_COUNT; ++i) g_nameIdx[i] = -1;
    ResolveNames();
    bool allMatch = true;
    for (int i = 0; i < N_COUNT; ++i) allMatch &= g_nameIdx[i] == expected[i];
    CHECK(allMatch);

    // Most of these tests cover the "fall in one piece" mode; shatter mode (default) is tested below.
    g_cfg.uprootStreetLights = true;

    // Fake engine
    o_ProcessEvent = Fake_ProcessEvent;
    g_FindFunctionByName = Fake_FindFunctionByName;
    g_SyncActor = Fake_SyncActor;
    o_SetIsStatic = Fake_SetIsStatic;
    o_ProcessCollision = Fake_PhysicalStep;
    o_ProcessShift = Fake_PhysicalStep;
    g_AddToMovingList = Fake_AddToMovingList;
    g_GetColModel = Fake_GetColModel;
    o_GlassCollision = Fake_GlassCollision;
    g_GroundProbe = Fake_GroundProbe;
    o_EntityColModel = Fake_EntityColModel;
    for (int i = 0; i < N_COUNT; ++i) g_funcs[i] = NewObj(nullptr, g_nameIdx[i]);

    // Class chain: BP_Streetlight_C -> StreetLightMapActor -> DynamicIPLMapActor -> Actor
    FakeObj* clsActor = NewObj(nullptr, 9001);
    FakeObj* clsDyn = NewObj(nullptr, g_nameIdx[N_DynamicIPLMapActor]);
    FakeObj* clsLight = NewObj(nullptr, g_nameIdx[N_StreetLightMapActor]);
    FakeObj* clsBp = NewObj(nullptr, 9003);
    *(void**)(clsDyn->mem + UE::Struct_Super) = clsActor;
    *(void**)(clsLight->mem + UE::Struct_Super) = clsDyn;
    *(void**)(clsBp->mem + UE::Struct_Super) = clsLight;
    FakeObj* clsCar = NewObj(nullptr, 9004);
    *(void**)(clsCar->mem + UE::Struct_Super) = clsActor;

    FakeObj* actor = NewObj(clsBp, 9100);
    FakeObj* root = NewObj(clsActor, 9101);
    *(void**)(actor->mem + UE::Actor_RootComponent) = root;
    FakeObj* carActor = NewObj(clsCar, 9102);
    CHECK(!IsDynamicPropActor(carActor));

    // RenderWare entity (object) linked to the actor, upright matrix.
    alignas(16) float matrix[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 10,20,30,1 };
    FakeObj* entity = NewObj(nullptr, 0);
    *(float**)(entity->mem + GTA::Entity_Matrix) = matrix;
    *(void**)(entity->mem + GTA::Entity_Actor) = actor;
    *(uint32_t*)(entity->mem + GTA::Entity_Flags) = GTA::Flag_IsStatic;
    entity->mem[GTA::Entity_Type] = GTA::Type_Object;

    // Dislodge: Blueprint hand-off suppressed, actor follows RenderWare.
    g_calls.clear();
    Hooked_SetIsStatic(entity, false);
    CHECK(!Called(actor, N_Dislodged));
    CHECK((actor->mem[UE::IPLMapActor_Flags] & UE::EntityUpdatePositionBit) != 0);
    CHECK(Called(root, N_SetMobility));
    CHECK(!Called(actor, N_SetLights)); // still upright

    // Physics steps sync the actor; tilted street light switches off once.
    g_calls.clear(); g_syncCount = 0;
    Hooked_ProcessCollision(entity);
    matrix[10] = 0.5f; // up.z
    Hooked_ProcessShift(entity);
    Hooked_ProcessCollision(entity);
    CHECK(g_syncCount == 3);
    CHECK(CountCalls(actor, N_SetLights) == 1);
    // DE switching lights on at dusk is overridden for the fallen pole.
    uint8_t lightsOn[8] = { 1, 0 };
    Hooked_ProcessEvent(actor, g_funcs[N_SetLights], lightsOn);
    CHECK(lightsOn[0] == 0);
    // Re-link restores light handling.
    Hooked_ProcessEvent(actor, g_funcs[N_EntityLinked], nullptr);
    lightsOn[0] = 1;
    Hooked_ProcessEvent(actor, g_funcs[N_SetLights], lightsOn);
    CHECK(lightsOn[0] == 1);

    // Repeated SetIsStatic(false) (e.g. buoyancy every frame) does not redo the setup.
    g_calls.clear();
    Hooked_SetIsStatic(entity, false);
    CHECK(!Called(actor, N_Dislodged) && !Called(root, N_SetMobility));

    // Settling while still in the moving list: kept non-static and pushable, actor still follows RW.
    uint8_t movingNode[8] = {};
    *(void**)(entity->mem + GTA::Physical_MovingListNode) = movingNode;
    g_syncCount = 0;
    Hooked_SetIsStatic(entity, true);
    CHECK((*(uint32_t*)(entity->mem + GTA::Entity_Flags) & GTA::Flag_IsStatic) == 0); // not re-rooted
    Hooked_SetIsStatic(entity, true);   // called every settled frame
    Hooked_ProcessCollision(entity);    // pose unchanged -> no actor update
    const int syncsWhileSettled = g_syncCount;
    matrix[12] += 0.5f;                 // a car pushes it
    Hooked_ProcessCollision(entity);
    CHECK(g_syncCount == syncsWhileSettled + 1);
    // Config off (original): comes to rest -> static, flag restored, no more syncs.
    g_cfg.keepPushable = false;
    g_syncCount = 0;
    Hooked_SetIsStatic(entity, true);
    const int restSyncs = g_syncCount; // 0: the actor already has this pose
    CHECK((*(uint32_t*)(entity->mem + GTA::Entity_Flags) & GTA::Flag_IsStatic) != 0);
    CHECK((actor->mem[UE::IPLMapActor_Flags] & UE::EntityUpdatePositionBit) == 0);
    Hooked_ProcessCollision(entity);
    CHECK(g_syncCount == restSyncs);
    g_cfg.keepPushable = true;
    // A re-created object at the same address (not in the moving list) is not kept pushable.
    *(uint32_t*)(entity->mem + GTA::Entity_Flags) = GTA::Flag_IsStatic;
    Hooked_SetIsStatic(entity, false);
    *(void**)(entity->mem + GTA::Physical_MovingListNode) = nullptr;
    Hooked_SetIsStatic(entity, true);
    CHECK(FindDislodged(entity) < 0 && (*(uint32_t*)(entity->mem + GTA::Entity_Flags) & GTA::Flag_IsStatic));

    // Config off -> stock behaviour.
    g_cfg.rwDislodgedProps = false;
    g_calls.clear();
    *(uint32_t*)(entity->mem + GTA::Entity_Flags) = GTA::Flag_IsStatic;
    Hooked_SetIsStatic(entity, false);
    CHECK(Called(actor, N_Dislodged));
    g_cfg.rwDislodgedProps = true;

    // RemoveFloor lifetime modes.
    g_calls.clear();
    g_cfg.debrisMode = DEBRIS_ORIGINAL;
    Hooked_ProcessEvent(actor, g_funcs[N_RemoveFloor], nullptr); // timer active: held back
    CHECK(!Called(actor, N_HideBroken) && !Called(actor, N_RemoveFloor));
    g_cfg.piecesDisappearSeconds = 0.0f;                           // no timer, no original pieces:
    g_cfg.originalPieceMotion = false;                             // hide at the Blueprint's call
    Hooked_ProcessEvent(actor, g_funcs[N_RemoveFloor], nullptr);
    g_cfg.originalPieceMotion = true;
    CHECK(Called(actor, N_HideBroken) && Called(actor, N_RemoveFloor));
    g_cfg.piecesDisappearSeconds = 6.0f;
    g_calls.clear();
    g_cfg.debrisMode = DEBRIS_KEEP;
    Hooked_ProcessEvent(actor, g_funcs[N_RemoveFloor], nullptr);
    CHECK(g_calls.empty());
    g_calls.clear();
    g_cfg.debrisMode = DEBRIS_STOCK;
    Hooked_ProcessEvent(actor, g_funcs[N_RemoveFloor], nullptr);
    CHECK(Called(actor, N_RemoveFloor) && !Called(actor, N_HideBroken));
    g_cfg.debrisMode = DEBRIS_ORIGINAL;

    // Ground plane maths.
    const float n[3] = { 0.3f, -0.2f, 0.9327379f };
    const FQuat q = QuatFromUp(n[0], n[1], n[2]);
    FVec up = RotateUp(q);
    CHECK(fabsf(up.X - n[0]) < 1e-4f && fabsf(up.Y - n[1]) < 1e-4f && fabsf(up.Z - n[2]) < 1e-4f);

    // Glass: constant floorTransform replaced by the RenderWare ground probe (m -> cm, Y flipped).
    alignas(16) SetupBrokenParms parms = {};
    parms.floorTransform.Rotation = { 0, 0, 0, 1 };
    t_glassGround = { actor, { 1.5f, -2.0f, 3.25f }, { 0, 0, 1 } };
    PatchGlassFloor(actor, &parms);
    CHECK(parms.floorTransform.Translation[0] == 150.0f && parms.floorTransform.Translation[1] == 200.0f &&
          parms.floorTransform.Translation[2] == 325.0f);
    // A floor already supplied by DE (ObjectDamage path) is left alone.
    parms.floorTransform.Translation[0] = 5000.0f;
    t_glassGround.pos[2] = 99.0f;
    PatchGlassFloor(actor, &parms);
    CHECK(parms.floorTransform.Translation[2] == 325.0f);
    t_glassGround.actor = nullptr;

    // SetupBroken passes through to the Blueprint and then builds the plane.
    FakeObj* floor = NewObj(clsActor, 9200);
    FakeObj* brokenMesh = NewObj(clsActor, 9201);
    g_fakeFloor = floor; g_fakeBroken = brokenMesh;
    parms.ReturnValue = true;
    g_calls.clear();
    Hooked_ProcessEvent(actor, g_funcs[N_SetupBroken], &parms);
    CHECK(IndexOfCall(actor, N_SetupBroken) == 0);
    {
        const int xi = IndexOfCall(floor, N_K2_SetWorldTransform);
        CHECK(xi >= 0);
        if (xi >= 0) {
            const FTransform* t = (const FTransform*)g_calls[xi].parms;
            CHECK(t->Translation[0] == 5000.0f && t->Translation[2] == 325.0f - 10.0f); // top face on ground
            CHECK(t->Scale3D[2] == 0.2f); // plane thickness that matches the -10 cm offset
            CHECK(g_calls[xi].parms[0xC0] == 1); // bTeleport
        }
        const int ci = IndexOfCall(floor, N_SetCollisionEnabled);
        CHECK(ci >= 0 && g_calls[ci].parms[0] == 2); // PhysicsOnly
        const int ai = IndexOfCall(floor, N_SetCollisionResponseToAllChannels);
        CHECK(ai >= 0 && g_calls[ai].parms[0] == 0); // Ignore all
        CHECK(ResponseFor(floor, 5) == 2 && ResponseFor(floor, 7) == 2);
        CHECK(ResponseFor(floor, 0) == -1 && ResponseFor(floor, 1) == -1 && ResponseFor(floor, 6) == -1);
        CHECK(Called(floor, N_SetMobility) && Called(floor, N_K2_DetachFromComponent));
        CHECK(Called(brokenMesh, N_SetAllUseCCD));
    }
    // Fragments of a custom object type get that channel blocked too; Vehicle is never blocked.
    g_brokenObjectType = 1; g_calls.clear();
    Hooked_ProcessEvent(actor, g_funcs[N_SetupBroken], &parms);
    CHECK(ResponseFor(floor, 1) == 2);
    g_brokenObjectType = 6; g_calls.clear();
    Hooked_ProcessEvent(actor, g_funcs[N_SetupBroken], &parms);
    CHECK(ResponseFor(floor, 6) == -1);
    g_brokenObjectType = 5;
    // Blueprint reports failure -> no floor is built.
    parms.ReturnValue = false; g_calls.clear();
    Hooked_ProcessEvent(actor, g_funcs[N_SetupBroken], &parms);
    CHECK(Called(actor, N_SetupBroken) && !Called(actor, N_GetPhysicsFloor));

    // Class cache: same UClass address reused by a different class is re-evaluated.
    FakeObj* clsReuse = NewObj(nullptr, 9500);
    *(void**)(clsReuse->mem + UE::Struct_Super) = clsDyn;
    FakeObj* reuseActor = NewObj(clsReuse, 9501);
    CHECK(IsDynamicPropActor(reuseActor));
    *(int32_t*)(clsReuse->mem + UE::Obj_Name) = 9502;
    *(void**)(clsReuse->mem + UE::Struct_Super) = clsActor;
    CHECK(!IsDynamicPropActor(reuseActor));

    // Street lights: DE's BREAKABLE effect becomes "uproot like the original".
    printf("-- ObjectDamage --\n");
    o_ObjectDamage = Fake_ObjectDamage;
    alignas(16) uint8_t objectInfo[0x40] = {};
    *(float*)(objectInfo + GTA::ObjectInfo_ColDamageMult) = 1.0f;
    void* vtable[16] = {};
    vtable[GTA::VTable_SetIsStatic / 8] = (void*)&Hooked_SetIsStatic;
    FakeObj* lamp = NewObj(nullptr, 0);
    *(void***)lamp->mem = vtable;
    *(void**)(lamp->mem + GTA::Entity_Actor) = actor;
    *(uint8_t**)(lamp->mem + GTA::Entity_ObjectInfo) = objectInfo;
    *(float**)(lamp->mem + GTA::Entity_Matrix) = matrix;
    lamp->mem[GTA::Entity_Type] = GTA::Type_Object;
    lamp->mem[GTA::Entity_ColDamageEffect] = GTA::ColDamage_Breakable;
    *(uint32_t*)(lamp->mem + GTA::Entity_Flags) = GTA::Flag_IsStatic | GTA::Flag_UsesCollision;
    auto resetLampFlags = [](FakeObj* e) {
        *(uint32_t*)(e->mem + GTA::Entity_Flags) = GTA::Flag_IsStatic | GTA::Flag_UsesCollision;
    };

    g_damageCalls = 0; g_movingListAdds = 0;
    Hooked_ObjectDamage(lamp, 100.0f, nullptr, nullptr, nullptr, GTA::Weapon_Collision); // below threshold
    CHECK(g_damageCalls == 1 && g_effectSeen == 0);
    CHECK(lamp->mem[GTA::Entity_ColDamageEffect] == GTA::ColDamage_Breakable); // restored

    // A car impact is left to RenderWare's own uproot test (limit 240 here): no early uproot, no shatter.
    *(float*)(objectInfo + GTA::ObjectInfo_UprootLimit) = 240.0f;
    Hooked_ObjectDamage(lamp, 400.0f, nullptr, nullptr, nullptr, GTA::Weapon_Collision);
    CHECK((*(uint32_t*)(lamp->mem + GTA::Entity_Flags) & GTA::Flag_IsStatic) != 0);
    // Explosions (no ApplyCollision uproot test) and "never uproot" limits are uprooted by the plugin.
    resetLampFlags(lamp);
    Hooked_ObjectDamage(lamp, 400.0f, nullptr, nullptr, nullptr, GTA::Weapon_Explosion);
    CHECK((*(uint32_t*)(lamp->mem + GTA::Entity_Flags) & GTA::Flag_IsStatic) == 0 && g_movingListAdds == 1);
    Hooked_SetIsStatic(lamp, true);
    resetLampFlags(lamp);
    *(float*)(objectInfo + GTA::ObjectInfo_UprootLimit) = 9999.0f;
    Hooked_ObjectDamage(lamp, 400.0f, nullptr, nullptr, nullptr, GTA::Weapon_Collision);
    CHECK((*(uint32_t*)(lamp->mem + GTA::Entity_Flags) & GTA::Flag_IsStatic) == 0 && g_movingListAdds == 2);
    Hooked_SetIsStatic(lamp, true);
    *(float*)(objectInfo + GTA::ObjectInfo_UprootLimit) = 240.0f;

    // Other weapons never break a street light (original colDamageEffect NONE) and do not uproot it.
    const int weapons[] = { 0, 22, 34, 37, 54 };
    bool neverBroken = true;
    for (int w : weapons) {
        *(uint32_t*)(lamp->mem + GTA::Entity_Flags) = GTA::Flag_IsStatic | GTA::Flag_UsesCollision;
        Hooked_ObjectDamage(lamp, 10000.0f, nullptr, nullptr, nullptr, w);
        neverBroken &= g_effectSeen == 0 && (*(uint32_t*)(lamp->mem + GTA::Entity_Flags) & GTA::Flag_IsStatic);
    }
    CHECK(neverBroken);

    // Script-locked poles (bDisableCollisionForce) are not uprooted.
    *(uint32_t*)(lamp->mem + GTA::Entity_PhysicalFlags) = GTA::PhysFlag_DisableCollisionForce;
    Hooked_ObjectDamage(lamp, 400.0f, nullptr, nullptr, nullptr, GTA::Weapon_Explosion);
    CHECK((*(uint32_t*)(lamp->mem + GTA::Entity_Flags) & GTA::Flag_IsStatic) != 0);
    *(uint32_t*)(lamp->mem + GTA::Entity_PhysicalFlags) = 0;

    // Non-street-light breakables (fences, boxes) keep DE's fragments.
    FakeObj* box = NewObj(nullptr, 0);
    *(void**)(box->mem + GTA::Entity_Actor) = carActor;
    box->mem[GTA::Entity_Type] = GTA::Type_Object;
    box->mem[GTA::Entity_ColDamageEffect] = GTA::ColDamage_Breakable;
    *(uint32_t*)(box->mem + GTA::Entity_Flags) = GTA::Flag_IsStatic | GTA::Flag_UsesCollision;
    Hooked_ObjectDamage(box, 400.0f, nullptr, nullptr, nullptr, GTA::Weapon_Collision);
    CHECK(g_effectSeen == GTA::ColDamage_Breakable);

    // ---- Street lights DE routes through the glass code (CGlass::WindowRespondsToCollision).
    printf("-- glass path / collision coverage --\n");
    auto resetLamp = [&](FakeObj* e, int16_t model) {
        *(int16_t*)(e->mem + GTA::Entity_ModelIndex) = model;
        *(uint32_t*)(e->mem + GTA::Entity_Flags) = GTA::Flag_IsStatic | GTA::Flag_UsesCollision;
        *(uint32_t*)(e->mem + GTA::Entity_PhysicalFlags) = 0;
        matrix[10] = 1.0f;
    };
    resetLamp(lamp, 0);
    g_glassCalls = 0; g_movingListAdds = 0; g_calls.clear();
    float spd[3] = {}, hit[3] = { 1, 2, 3 };
    Hooked_GlassCollision(lamp, 500.0f, spd, hit);
    CHECK(g_glassCalls == 0);                                                        // not shattered
    CHECK((*(uint32_t*)(lamp->mem + GTA::Entity_Flags) & GTA::Flag_IsStatic) == 0); // uprooted
    CHECK(g_movingListAdds == 1 && FindDislodged(lamp) >= 0 && !Called(actor, N_Dislodged));
    Hooked_GlassCollision(lamp, 500.0f, spd, hit); // already moving: still never shattered
    CHECK(g_glassCalls == 0);
    Hooked_SetIsStatic(lamp, true);

    // Model 200 has collision only at its base: RenderWare must not take it over.
    FakeObj* stub = NewObj(nullptr, 0);
    *(void***)stub->mem = vtable;
    *(void**)(stub->mem + GTA::Entity_Actor) = actor;
    *(uint8_t**)(stub->mem + GTA::Entity_ObjectInfo) = objectInfo;
    *(float**)(stub->mem + GTA::Entity_Matrix) = matrix;
    stub->mem[GTA::Entity_Type] = GTA::Type_Object;
    stub->mem[GTA::Entity_ColDamageEffect] = GTA::ColDamage_Breakable;
    resetLamp(stub, 200);
    {
        const ColShapes full = MeasureColShapes(Fake_GetColModel(0));
        CHECK(fabsf(full.covered - 6.0f) < 1e-3f && full.hi == 6.0f); // overlapping boxes merged, not double-counted
        FakeObj* shortPole = NewObj(nullptr, 0);
        *(float**)(shortPole->mem + GTA::Entity_Matrix) = matrix;
        *(int16_t*)(shortPole->mem + GTA::Entity_ModelIndex) = 201;
        CHECK(!RwCollisionCovers(shortPole, actor)); // box itself far shorter than the visible prop

        // Model 0 is boxes only (no spheres): RenderWare could not land it, so spheres are generated.
        CHECK(g_aug[0] != nullptr);
        if (g_aug[0]) {
            const AugCol* a = g_aug[0];
            float lo = 1e9f, hi = -1e9f;
            for (int k = 0; k < a->nSpheres; ++k) {
                lo = fminf(lo, a->spheres[k * 5 + 2] - a->spheres[k * 5 + 3]);
                hi = fmaxf(hi, a->spheres[k * 5 + 2] + a->spheres[k * 5 + 3]);
                CHECK(fabsf(a->spheres[k * 5]) < 0.01f && fabsf(a->spheres[k * 5 + 1]) < 0.01f && a->spheres[k * 5 + 3] <= 0.21f); // on the axis, pole-thick
            }
            printf("   generated %d spheres, z %.2f..%.2f\n", a->nSpheres, lo, hi);
            CHECK(a->nSpheres >= 10 && a->nSpheres <= 16 && lo >= -0.25f && lo <= 0.1f && hi >= 5.9f && hi <= 6.3f);
            // Served only to props the plugin simulates, with the game's data refreshed and our sphere list.
            const bool trackedLamp = FindDislodged(lamp) >= 0;
            FakeObj* other = NewObj(nullptr, 0);
            other->mem[GTA::Entity_Type] = GTA::Type_Object;
            CHECK(Hooked_EntityColModel(other) == Fake_GetColModel(0));
            Hooked_SetIsStatic(lamp, false);
            const uint8_t* served = (const uint8_t*)Hooked_EntityColModel(lamp);
            const uint8_t* data = *(uint8_t* const*)(served + GTA::ColModel_Data);
            CHECK(*(const uint16_t*)data == a->nSpheres && *(float* const*)(data + GTA::ColData_Spheres) == a->spheres);
            CHECK(*(const uint16_t*)(data + 2) == 2 && *(float* const*)(data + 0x10) == g_poleBoxes); // game's boxes
            CHECK(memcmp(served, Fake_GetColModel(0), 0x30) == 0);                                  // same bbox
            *(void**)(g_aug[0]->data + GTA::ColData_TrianglePlanes) = (void*)0x1234;               // planes the game made
            Hooked_EntityColModel(lamp);
            CHECK(*(void**)(g_aug[0]->data + GTA::ColData_TrianglePlanes) == (void*)0x1234);
            *(void**)(g_aug[0]->data + GTA::ColData_TrianglePlanes) = nullptr;
            if (!trackedLamp) { resetLamp(lamp, 0); }
        }
    }
    // glass path: DE shatters it (with our ground probe), no uproot
    g_glassCalls = 0; g_movingListAdds = 0;
    Hooked_GlassCollision(stub, 500.0f, spd, hit);
    CHECK(g_glassCalls == 1 && g_movingListAdds == 0 && FindDislodged(stub) < 0);
    // ObjectDamage path: DE's breakable effect is kept
    resetLamp(stub, 200);
    Hooked_ObjectDamage(stub, 400.0f, hit, nullptr, nullptr, GTA::Weapon_Collision);
    CHECK(g_effectSeen == GTA::ColDamage_Breakable);
    // DE's own uproot (SetIsStatic(false)): broken the DE way at the last impact point, stays static
    resetLamp(stub, 200);
    g_glassCalls = 0; g_calls.clear();
    t_hit = { stub, 321.0f, { 7, 8, 9 }, {} };
    Hooked_SetIsStatic(stub, false);
    CHECK(g_glassCalls == 1 && g_glassPos[0] == 7 && g_glassPos[2] == 9);
    CHECK((*(uint32_t*)(stub->mem + GTA::Entity_Flags) & GTA::Flag_IsStatic) != 0);
    CHECK(FindDislodged(stub) < 0 && !Called(actor, N_Dislodged));
    // A non-street-light prop with incomplete collision keeps stock DE behaviour (Blueprint Dislodged).
    FakeObj* clsBin = NewObj(nullptr, 9600);
    *(void**)(clsBin->mem + UE::Struct_Super) = clsDyn;
    FakeObj* binActor = NewObj(clsBin, 9601);
    FakeObj* bin = NewObj(nullptr, 0);
    *(void**)(bin->mem + GTA::Entity_Actor) = binActor;
    *(float**)(bin->mem + GTA::Entity_Matrix) = matrix;
    bin->mem[GTA::Entity_Type] = GTA::Type_Object;
    resetLamp(bin, 200);
    g_calls.clear(); g_glassCalls = 0;
    Hooked_SetIsStatic(bin, false);
    CHECK(Called(binActor, N_Dislodged) && g_glassCalls == 0 && FindDislodged(bin) < 0);
}

// Generated spheres reach the inlined model-info getter while (and only while) the tracked prop is being
// simulated; a fallen pole is removed like DE's "smash completely" after FallenPolesDisappearSeconds.
static uint8_t g_fakeModelInfo[0x40];
static const void* g_seenCol = nullptr;
static const void* Fake_MIColModel(void*) { return Fake_GetColModel(0); }
static uintptr_t Fake_CollisionAsksModelInfo(void*) { g_seenCol = Hooked_MIColModel(g_fakeModelInfo); return 7; }
static int g_visCalls = 0, g_deleteCalls = 0;
static void Fake_UpdateVisibility(void*) { ++g_visCalls; }
static void Fake_DeleteRwObject(void*) { ++g_deleteCalls; }

static void TestModelInfoHookAndRemoval() {
    printf("== model-info col hook + fallen pole removal ==\n");
    g_cfg.uprootStreetLights = true;
    static void* modelInfos[1] = { g_fakeModelInfo };
    g_modelInfos = modelInfos;
    o_MIColModel = Fake_MIColModel;
    o_ProcessCollision = Fake_CollisionAsksModelInfo;

    FakeObj* clsDyn = NewObj(nullptr, g_nameIdx[N_DynamicIPLMapActor]);
    FakeObj* clsLight = NewObj(nullptr, g_nameIdx[N_StreetLightMapActor]);
    *(void**)(clsLight->mem + UE::Struct_Super) = clsDyn;
    FakeObj* light = NewObj(clsLight, 9800);
    alignas(16) float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,10,1 };
    void* vt[32] = {};
    vt[GTA::VTable_SetIsStatic / 8] = (void*)&Hooked_SetIsStatic;
    vt[GTA::VTable_UpdateActorVisibility / 8] = (void*)&Fake_UpdateVisibility;
    vt[GTA::VTable_DeleteRwObject / 8] = (void*)&Fake_DeleteRwObject;
    FakeObj* pole = NewObj(nullptr, 0);
    *(void***)pole->mem = vt;
    *(void**)(pole->mem + GTA::Entity_Actor) = light;
    *(float**)(pole->mem + GTA::Entity_Matrix) = m;
    pole->mem[GTA::Entity_Type] = GTA::Type_Object;
    *(uint32_t*)(pole->mem + GTA::Entity_Flags) = GTA::Flag_IsStatic | GTA::Flag_UsesCollision | GTA::Flag_IsVisible;

    // Not tracked: the model-info getter returns the game's collision.
    g_seenCol = nullptr;
    Hooked_ProcessCollision(pole);
    CHECK(g_seenCol == Fake_GetColModel(0));
    // Tracked: during its own collision step it gets the extended copy; outside it, the original.
    Hooked_SetIsStatic(pole, false);
    g_cfg.fallenPolesDisappearSeconds = 0.0f;
    g_seenCol = nullptr;
    Hooked_ProcessCollision(pole);
    CHECK(g_seenCol == g_aug[0]->colModel);
    CHECK(Hooked_MIColModel(g_fakeModelInfo) == Fake_GetColModel(0));
    // Removal after the delay: DE smash sequence, tracking dropped, the actor flag restored.
    g_cfg.fallenPolesDisappearSeconds = 0.05f;
    Sleep(80);
    Hooked_ProcessCollision(pole);
    const uint32_t f = *(uint32_t*)(pole->mem + GTA::Entity_Flags);
    CHECK(FindDislodged(pole) < 0);
    CHECK((f & GTA::Flag_IsStatic) && !(f & GTA::Flag_UsesCollision) && !(f & GTA::Flag_IsVisible));
    CHECK(*(uint32_t*)(pole->mem + GTA::Entity_PhysicalFlags) & GTA::PhysFlag_SmashedRemoved);
    CHECK(g_visCalls == 1 && g_deleteCalls == 1);
    g_cfg.fallenPolesDisappearSeconds = 6.0f;
    g_modelInfos = nullptr;
    o_ProcessCollision = Fake_PhysicalStep;
}

float g_boneZ[3] = { 200.0f, 200.0f, 200.0f };
float g_bottomZ[3] = { 100.0f, 100.0f, 100.0f }, g_topZ[3] = { 145.0f, 145.0f, 145.0f }; // piece collision extent (cm)
bool g_noShape[3] = {};
float g_cmdVel[3][3] = {}, g_forcedVel[3][3] = {};
bool g_forceVel[3] = {};

// Last SetEnableBodyGravity value sent for a bone (-1 = none).
static int LastBodyGravity(void* mesh, int bone) {
    for (size_t i = g_calls.size(); i-- > 0;) {
        const CallRec& c = g_calls[i];
        if (c.obj == mesh && c.name == g_nameIdx[N_SetEnableBodyGravity] && ((const FName*)(c.parms + 4))->Index == 7000 + bone)
            return c.parms[0];
    }
    return -1;
}

// Last linear (or angular) velocity set for a bone (Unreal cm/s or deg/s), from the recorded calls.
static bool LastBoneVelocity(void* mesh, int bone, float out[3], NameId fn = N_SetPhysicsLinearVelocity) {
    for (size_t i = g_calls.size(); i-- > 0;) {
        const CallRec& c = g_calls[i];
        if (c.obj == mesh && c.name == g_nameIdx[fn] && ((const FName*)(c.parms + 0x10))->Index == 7000 + bone) {
            memcpy(out, c.parms, 12);
            return true;
        }
    }
    return false;
}
static int CountBoneCalls(void* mesh, NameId id, int bone) {
    int c = 0;
    for (auto& r : g_calls) if (r.obj == mesh && r.name == g_nameIdx[id] && ((const FName*)r.parms)->Index == 7000 + bone) ++c;
    return c;
}

// Original BreakObject_c motion on DE's pieces.
static void TestOriginalPieces() {
    printf("== original BreakObject_c pieces ==\n");
    static uint8_t items[2 * 0x18] = {};
    static uint8_t* chunkTable[1] = { items };
    static uint8_t objArray[0x20] = {};
    *(uint8_t***)objArray = chunkTable;
    *(int32_t*)(objArray + 0x14) = 2;
    g_objects = objArray;
    FakeObj* clsDyn = NewObj(nullptr, g_nameIdx[N_DynamicIPLMapActor]);
    FakeObj* clsComp = NewObj(nullptr, 9950);
    FakeObj* actor = NewObj(clsDyn, 9900);
    FakeObj* mesh = NewObj(clsComp, 9901);
    FakeObj* objs[2] = { actor, mesh };
    for (int i = 0; i < 2; ++i) {
        *(int32_t*)(objs[i]->mem + UE::Obj_Index) = i;
        *(void**)(items + i * 0x18) = objs[i];
        *(int32_t*)(items + i * 0x18 + 0x10) = 50 + i;
    }
    g_fakeBroken = mesh; g_fakeFloor = nullptr;
    g_cfg.originalPieceMotion = true;
    g_cfg.debrisMode = DEBRIS_ORIGINAL;
    g_cfg.piecesDisappearSeconds = 6.0f;

    // RW object being broken: object.dat break velocity (0,0,0.1) m/step, no random part (deterministic).
    alignas(16) uint8_t info[0x60] = {};
    const float bv[3] = { 0.0f, 0.0f, 0.1f };
    memcpy(info + GTA::ObjectInfo_BreakVelocity, bv, 12);
    FakeObj* ent = NewObj(nullptr, 0);
    *(void**)(ent->mem + GTA::Entity_Actor) = actor;
    *(uint8_t**)(ent->mem + GTA::Entity_ObjectInfo) = info;
    t_breakEntity = ent;

    alignas(16) SetupBrokenParms parms = {};
    parms.ReturnValue = true;
    parms.floorTransform.Rotation = { 0, 0, 0, 1 };
    parms.floorTransform.Translation[0] = 5000.0f; // ground plane at z = 0, normal +Z
    g_calls.clear();
    Hooked_ProcessEvent(actor, g_funcs[N_SetupBroken], &parms);
    t_breakEntity = nullptr;
    CHECK(g_pieceSetCount == 1 && g_pieceSets[0]->n == 2); // bone 1 has no body
    int gravityOff = 0;
    bool joint1 = false, joint2 = false;
    for (auto& r : g_calls) {
        if (r.obj == mesh && r.name == g_nameIdx[N_SetEnableBodyGravity] && r.parms[0] == 0) ++gravityOff;
        if (r.obj == mesh && r.name == g_nameIdx[N_BreakConstraint]) {
            joint1 |= ((const FName*)(r.parms + 0x18))->Index == 7001;
            joint2 |= ((const FName*)(r.parms + 0x18))->Index == 7002;
        }
    }
    // Every joint of the asset is broken, also the one named after a bone without a body (lamppost regression:
    // v0.3.2 only broke joints named after simulated bones and the pieces stayed jointed together).
    CHECK(gravityOff == 2 && joint1 && joint2);
    {   // pieces ignore everything but the ground planes (Destructible)
        const int all = IndexOfCall(mesh, N_SetCollisionResponseToAllChannels);
        CHECK(all >= 0 && g_calls[all].parms[0] == 0 && ResponseFor(mesh, 7) == 2 && ResponseFor(mesh, 6) == -1);
    }

    PieceSet* s = g_pieceSets[0];
    // One 1/50 s step in flight: v.z = 0.1 - 1/125 = 0.092 m/step -> 460 cm/s; tumbling for the first 5 steps.
    g_calls.clear();
    g_pieceLastTick = GetTickCount() - 20;
    TickOriginalPieces();
    float v[3] = {};
    CHECK(LastBoneVelocity(mesh, 0, v) && fabsf(v[2] - 460.0f) < 0.5f && fabsf(v[0]) < 1e-3f);
    // After the tumble the piece turns its thinnest axis (here X: 0 cm wide, 45 cm tall) to the ground normal,
    // like CalcGroupCenter: an upright piece lies down (90 deg * 50 / 20 = 225 deg/s about -Y), never stays upright.
    s->steps = 5.0f;
    g_calls.clear();
    g_pieceLastTick = GetTickCount() - 20;
    TickOriginalPieces();
    CHECK(s->g[0].flat == 0 && LastBoneVelocity(mesh, 0, v, N_SetPhysicsAngularVelocityInDegrees) && fabsf(v[1] + 225.0f) < 1.0f);
    // Collision sensor. Lamppost regression: in the air (lowest point 150 cm up) PhysX reports the piece
    // slowed down (joint, damping or timing): that is no landing. It keeps falling, never stops in mid-air.
    s->g[0].vel[0] = 0; s->g[0].vel[1] = 0; s->g[0].vel[2] = -0.2f; s->g[0].cmdVn = -1000.0f; // v0.3.1 took this as a landing
    g_bottomZ[0] = 150.0f;
    g_forceVel[0] = true; g_forcedVel[0][0] = g_forcedVel[0][1] = g_forcedVel[0][2] = 0;
    g_calls.clear();
    g_pieceLastTick = GetTickCount() - 20;
    TickOriginalPieces();
    CHECK(LastBoneVelocity(mesh, 0, v) && fabsf(v[2] - (-0.208f * 5000.0f)) < 0.5f && !s->g[0].stopped);
    // Ground reached this update (lowest point 2 cm up, falling 21.6 cm):
    // v = -0.208 -> -0.216 after gravity -> reflected x0.85 -> +0.1512 -> x0.8 = 0.12096 m/step (604.8 cm/s).
    g_bottomZ[0] = 2.0f;
    g_calls.clear();
    g_pieceLastTick = GetTickCount() - 20;
    TickOriginalPieces();
    const bool bounced = LastBoneVelocity(mesh, 0, v);
    float speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    CHECK(bounced && fabsf(speed - 604.8f) < 1.0f && v[2] > 500.0f);
    // Slow landing: stops; PhysX gravity lays it flat on the plane instead of freezing it where it is.
    s->g[0].vel[2] = -0.01f;
    g_bottomZ[0] = 0.5f;
    g_calls.clear();
    g_pieceLastTick = GetTickCount() - 20;
    TickOriginalPieces();
    CHECK(s->g[0].stopped && LastBodyGravity(mesh, 0) == 1 && CountBoneCalls(mesh, N_PutRigidBodyToSleep, 0) == 0);

    // Velocity fallback when the body has no measurable shape: the plane is the sensor.
    s->g[0].stopped = false;
    g_noShape[0] = true;
    s->g[0].vel[0] = 0; s->g[0].vel[1] = 0; s->g[0].vel[2] = -0.2f; s->g[0].cmdVn = -1000.0f;
    g_forceVel[0] = false; g_cmdVel[0][0] = 0; g_cmdVel[0][1] = 0; g_cmdVel[0][2] = -1000.0f;
    g_calls.clear();
    g_pieceLastTick = GetTickCount() - 20;
    TickOriginalPieces();
    CHECK(LastBoneVelocity(mesh, 0, v) && fabsf(v[2] - (-0.208f * 5000.0f)) < 0.5f && !s->g[0].stopped);
    g_forceVel[0] = true; // PhysX stopped the -1040 cm/s we sent: contact, same bounce
    g_calls.clear();
    g_pieceLastTick = GetTickCount() - 20;
    TickOriginalPieces();
    const bool bouncedFallback = LastBoneVelocity(mesh, 0, v);
    speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    CHECK(bouncedFallback && fabsf(speed - 604.8f) < 1.0f && v[2] > 500.0f);
    s->g[0].vel[2] = -0.01f; s->g[0].cmdVn = -50.0f;
    g_calls.clear();
    g_pieceLastTick = GetTickCount() - 20;
    TickOriginalPieces();
    CHECK(s->g[0].stopped && LastBodyGravity(mesh, 0) == 1);
    g_forceVel[0] = false; g_noShape[0] = false;
    // Fade (BreakObject_c::Render alpha = FramesToLive*2): opaque above 127.5 steps, then linear via SetAlpha.
    s->g[0].life = 300.0f; s->g[1].life = 250.0f;
    g_calls.clear();
    g_pieceLastTick = GetTickCount() - 20;
    TickOriginalPieces();
    CHECK(!Called(actor, N_SetAlpha) && !s->sinking);
    s->g[0].life = 65.0f; s->g[1].life = 60.0f; // -> 64 steps left after this update: alpha 64/127.5
    g_topZ[0] = 45.0f;                          // piece 0 reaches 45 cm above the ground
    g_calls.clear();
    g_pieceLastTick = GetTickCount() - 20;
    TickOriginalPieces();
    {
        const int ai = IndexOfCall(actor, N_SetAlpha);
        CHECK(ai >= 0 && fabsf(*(float*)g_calls[ai].parms - 64.0f / 127.5f) < 0.01f);
    }
    // ...and the pieces slide under the ground: plane ignored, piece 0 sinks (45 + 5) cm over the set's shortest
    // life (60 steps), so no piece is still above the ground when the first bone (and its children) hides.
    CHECK(s->sinking && ResponseFor(mesh, 7) == 0 && LastBodyGravity(mesh, 0) == 0);
    CHECK(LastBoneVelocity(mesh, 0, v) && fabsf(v[2] - (-50.0f / 60.0f * 50.0f)) < 0.1f && fabsf(v[0]) < 1e-3f);
    // Lifetime: each group hides its own bone; when all are gone the ground plane goes too.
    s->g[0].life = 0.5f; s->g[1].life = 5.0f;
    g_calls.clear();
    g_pieceLastTick = GetTickCount() - 20;
    TickOriginalPieces();
    CHECK(CountBoneCalls(mesh, N_HideBoneByName, 0) == 1 && CountBoneCalls(mesh, N_HideBoneByName, 2) == 0);
    s->g[1].life = 0.5f;
    g_pieceLastTick = GetTickCount() - 20;
    TickOriginalPieces();
    g_pieceLastTick = GetTickCount() - 20;
    TickOriginalPieces();
    CHECK(Called(actor, N_RemoveFloor) && g_pieceSetCount == 0);
    {   // alpha back to opaque after the pieces are gone (the prop can be restored later)
        int ai = -1;
        for (size_t k = 0; k < g_calls.size(); ++k)
            if (g_calls[k].obj == actor && g_calls[k].name == g_nameIdx[N_SetAlpha]) ai = (int)k; // last one
        CHECK(ai > IndexOfCall(actor, N_RemoveFloor) && *(float*)g_calls[ai].parms == 1.0f);
    }
    // A destroyed actor's set is dropped without touching it.
    Hooked_ProcessEvent(actor, g_funcs[N_SetupBroken], &parms);
    CHECK(g_pieceSetCount == 1);
    *(int32_t*)(items + 8) = 1 << 29; // PendingKill
    g_calls.clear();
    g_pieceLastTick = GetTickCount() - 20;
    TickOriginalPieces();
    CHECK(g_pieceSetCount == 0 && g_calls.empty());
    g_objects = nullptr;
    g_fakeBroken = nullptr;
    for (int i = 0; i < 3; ++i) { g_bottomZ[i] = 100.0f; g_topZ[i] = 145.0f; }
}

// Default mode: street lights shatter like the 2004 game, and broken pieces disappear after N seconds.
static void TestShatterAndTimers() {
    printf("== shatter mode + piece timers ==\n");
    g_cfg.uprootStreetLights = false;
    // Fake GObjects with 3 slots: FUObjectItem { Object, Flags, ClusterRoot, Serial, pad } = 0x18 bytes.
    static uint8_t items[3 * 0x18] = {};
    static uint8_t* chunkTable[1] = { items };
    static uint8_t objArray[0x20] = {};
    *(uint8_t***)objArray = chunkTable;
    *(int32_t*)(objArray + 0x14) = 3;
    g_objects = objArray;

    FakeObj* clsDyn = NewObj(nullptr, g_nameIdx[N_DynamicIPLMapActor]);
    FakeObj* clsLight = NewObj(nullptr, g_nameIdx[N_StreetLightMapActor]);
    *(void**)(clsLight->mem + UE::Struct_Super) = clsDyn;
    FakeObj* light = NewObj(clsLight, 9700);
    FakeObj* light2 = NewObj(clsLight, 9701);
    FakeObj* dead = NewObj(clsLight, 9702);
    FakeObj* objs[3] = { light, light2, dead };
    for (int i = 0; i < 3; ++i) {
        *(int32_t*)(objs[i]->mem + UE::Obj_Index) = i;
        *(void**)(items + i * 0x18) = objs[i];
        *(int32_t*)(items + i * 0x18 + 0x10) = 100 + i; // serial
    }
    g_fakeFloor = nullptr; g_fakeBroken = nullptr;

    // A standing street light that DE's ApplyCollision would uproot is shattered instead (stays static).
    alignas(16) float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,10,1 };
    FakeObj* pole = NewObj(nullptr, 0);
    *(void**)(pole->mem + GTA::Entity_Actor) = light;
    *(float**)(pole->mem + GTA::Entity_Matrix) = m;
    pole->mem[GTA::Entity_Type] = GTA::Type_Object;
    pole->mem[GTA::Entity_ColDamageEffect] = GTA::ColDamage_Breakable;
    *(uint32_t*)(pole->mem + GTA::Entity_Flags) = GTA::Flag_IsStatic | GTA::Flag_UsesCollision;
    g_glassCalls = 0; g_calls.clear();
    Hooked_SetIsStatic(pole, false);
    CHECK(g_glassCalls == 1 && FindDislodged(pole) < 0);
    CHECK((*(uint32_t*)(pole->mem + GTA::Entity_Flags) & GTA::Flag_IsStatic) != 0);
    // ObjectDamage keeps DE's breakable effect (no conversion) and the glass path breaks too.
    *(uint32_t*)(pole->mem + GTA::Entity_Flags) = GTA::Flag_IsStatic | GTA::Flag_UsesCollision;
    Hooked_ObjectDamage(pole, 400.0f, nullptr, nullptr, nullptr, GTA::Weapon_Collision);
    CHECK(g_effectSeen == GTA::ColDamage_Breakable);
    float spd[3] = {}, hit[3] = {};
    g_glassCalls = 0;
    Hooked_GlassCollision(pole, 500.0f, spd, hit);
    CHECK(g_glassCalls == 1);

    // SetupBroken schedules the pieces; the Blueprint's own RemoveFloor is held back until then.
    g_cfg.debrisMode = DEBRIS_ORIGINAL;
    g_cfg.piecesDisappearSeconds = 0.05f;
    alignas(16) SetupBrokenParms parms = {};
    parms.ReturnValue = true;
    Hooked_ProcessEvent(light, g_funcs[N_SetupBroken], &parms);
    Hooked_ProcessEvent(light2, g_funcs[N_SetupBroken], &parms);
    Hooked_ProcessEvent(dead, g_funcs[N_SetupBroken], &parms);
    // light2 is re-linked (restored) before the timer fires; `dead` is being destroyed.
    Hooked_ProcessEvent(light2, g_funcs[N_EntityLinked], nullptr);
    *(int32_t*)(items + 2 * 0x18 + 8) = 1 << 29; // PendingKill
    Sleep(80);
    g_calls.clear();
    Hooked_ProcessEvent(light, g_funcs[N_SetLights], nullptr); // any event on the game thread runs the timers
    CHECK(Called(light, N_HideBroken) && Called(light, N_RemoveFloor));
    CHECK(!Called(light2, N_HideBroken) && !Called(dead, N_HideBroken));
    // A reused slot (different serial) is not touched either.
    Hooked_ProcessEvent(light, g_funcs[N_SetupBroken], &parms);
    *(int32_t*)(items + 0x10) = 999;
    Sleep(80);
    g_calls.clear();
    Hooked_ProcessEvent(light2, g_funcs[N_SetLights], nullptr);
    CHECK(!Called(light, N_HideBroken));
    g_objects = nullptr;
    g_cfg.piecesDisappearSeconds = 6.0f;
}

// ---------------------------------------------------------------- real binary
static uint8_t* MapImage(const char* path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return nullptr;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> file(size);
    fread(file.data(), 1, size, f);
    fclose(f);
    auto* dos = (IMAGE_DOS_HEADER*)file.data();
    auto* nt = (IMAGE_NT_HEADERS64*)(file.data() + dos->e_lfanew);
    auto* img = (uint8_t*)VirtualAlloc(nullptr, nt->OptionalHeader.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!img) return nullptr;
    memcpy(img, file.data(), nt->OptionalHeader.SizeOfHeaders);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        memcpy(img + sec->VirtualAddress, file.data() + sec->PointerToRawData,
               min(sec->SizeOfRawData, sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData));
    return img;
}

static void TestBinary(const char* exePath) {
    printf("== signatures + hooks on %s ==\n", exePath);
    uint8_t* img = MapImage(exePath);
    CHECK(img != nullptr);
    if (!img) return;
    g_moduleBase = img;
    const bool installed = Install();
    CHECK(installed);
    if (!installed) return;
    CHECK((uint8_t*)g_names == img + 0x570CDC0);
    CHECK(g_objects == img + 0x5086380);
    CHECK((uint8_t*)g_FindFunctionByName == img + 0x1B18550);
    CHECK((uint8_t*)g_SyncActor == img + 0x112E610);
    CHECK((uint8_t*)g_GroundProbe == img + 0x120F290);
    CHECK((uint8_t*)g_AddToMovingList == img + 0x115C0F0);
    CHECK((uint8_t*)g_GetColModel == img + 0x120BC80);
    CHECK((uint8_t*)g_modelInfos == img + 0x5241FE0);
    CHECK(img[0xD2BE90] == 0xE9); // CBaseModelInfo::GetColModel hooked
    const uint32_t hooked[] = { 0x1C7B6B0, 0x12139F0, 0x115E370, 0x115F150, 0x1274040, 0x12142F0 };
    bool jumps = true;
    for (uint32_t rva : hooked) jumps &= img[rva] == 0xE9;
    CHECK(jumps);
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestLogic();
    TestShatterAndTimers();
    TestModelInfoHookAndRemoval();
    TestOriginalPieces();
    if (argc > 1) TestBinary(argv[1]);
    printf(g_failures ? "\n%d FAILED\n" : "\nALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}

# GTA_Prop_Fix (v0.3.2)

An x64 ASI plugin for **GTA: San Andreas – The Definitive Edition** that makes map props (lamp posts, traffic lights, signs, bins, hydrants, fences, boxes, and so on) move like they did in the original 2004 RenderWare game. The behaviour is modelled on [gta-reversed](https://github.com/gta-reversed/gta-reversed) (`CObject`, `CPhysical`, `BreakObject_c`).

---

## The Problem

In the Definitive Edition, Grove Street Games used a hybrid engine architecture:
1. **Classic 2004 RenderWare Engine:** Runs vehicle handling, player movement, world physics, and map geometry using legacy `.col` collision trees in native C++.
2. **Unreal Engine 4:** Runs purely as a graphics renderer.

When a streetlight or fence breaks, the game hands the fractured mesh (`BrokenMesh`) over to **Unreal Engine 4 PhysX** rigid-body simulation. However:
- **PhysX runs in an empty void:** Unreal Engine has no PhysX collision meshes for the city roads, terrain, or buildings (those belong exclusively to the RenderWare engine).
- **Falling Through Ground:** Because there is no terrain collision in PhysX, broken props fell straight through the road and vanished into the void. Grove Street Games attempted a crude workaround (`PhysicsFloor`), but despawned it after a few seconds (`RemoveFloor`), causing resting props to immediately drop through the pavement into the void.

---

## Features

### 1. Knocked-over props use the original RenderWare physics
DE still runs the original `CObject`/`CPhysical` simulation after a prop is knocked loose (`CObject::SetIsStatic(false)`). The only change is that it also fires the Blueprint event `Dislodged`, which hands the actor to UE PhysX, and PhysX has no world collision. The plugin:
- suppresses that hand-off for every `DynamicIPLMapActor` prop;
- copies the RenderWare matrix to the actor after each collision/shift step, so the prop collides with roads, buildings and cars using its `object.dat` mass, uproot limit and elasticity;
- switches street lights off once they tilt (`up.z < 0.96`, as in `CEntity::ProcessLightsForEntity`);
- takes a prop over only when its RenderWare collision box covers at least 60% of its visible height (`MinCollisionCoveragePercent`) **and** its solid collision shapes (spheres, boxes, triangles) fill at least 70% of that box (`MinShapeCoveragePercent`). Some DE poles only have collision at the base. RenderWare physics would leave those hanging from the base under the road, so street lights of that kind are broken the DE way (on the ground plane below), and other props keep stock behaviour.

RenderWare lands a falling prop only on its collision **spheres**: `CCollision::ProcessColModels` tests a moving object's spheres against the world, never its triangles. Some DE props (e.g. `BP_MTraffic1_C`: 1 sphere, 48 triangles) would therefore sink through the road except at the base. For these, the plugin generates spheres along the collision mesh: one per 0.5 m slab on the pole's axis, and voxel spheres for wide parts like arms. It serves them only while that prop is being simulated, through hooks on `CEntity::GetColModel` and on the model-info getter that DE's inlined collision code calls (`0x140D2BE90`). The game's collision data is never modified (`GenerateCollisionSpheres`).

A pole that fell over whole disappears after `FallenPolesDisappearSeconds` (default 6). Other knocked-loose props use `KnockedPropsDisappearSeconds` (default 0, stays). Removal copies DE's own "smash completely" (`ObjectDamage` case `0x14`), and the prop comes back when the area streams in again.

Two defaults differ from the original, because a fallen pole that cars can't touch feels broken:
- `FallenPolesBlockCars=1`: in the original, cars pass through a lamp post once it lies down (`IsFallenLampPost`). With this on, fallen poles stay solid for cars and peds.
- `KnockedPropsStayPushable=1`: in the original, a settled prop is rooted again after a few seconds (`CObject::ProcessControl` → `SetIsStatic(true)`) and only moves if hit harder than its uproot limit. With this on, a knocked prop stays pushable by any car or ped.

Set either to 0 for exact original behaviour. Car impacts uproot a pole only when they beat its uproot limit (DE's own `ApplyCollision` test), which also passes the car's momentum to the pole. Broken pieces get angular damping (`FragmentAngularDamping=1.5`) so they stop spinning once they land, like the original fragments.

### 2. Lamp posts and traffic lights shatter like the 2004 game
The game's own original data (`OriginalData/GTASA/data/object.dat` in `pakchunk0`) gives every lamp post and traffic light damage effect 200 (breakable), with an uproot limit of 240–280. The original `CPhysical::ApplyCollision` runs `CObject::ObjectDamage` first and breaks the prop once `damage × CDMult > 150`, before the uproot check can run. So the 2004 game shattered these poles; it did not knock them over whole. With `StreetLightsFallWhole=0` the plugin keeps that behaviour: every native path that would uproot a breakable street light instead breaks it (DE's `SetupBroken` pieces, on the ground plane below). That covers `ObjectDamage`, the glass code (`CGlass::WindowRespondsToCollision`) and `SetIsStatic(false)`. The default is `StreetLightsFallWhole=1`: they fall over in one piece with RenderWare physics (see section 1), because DE's broken-pole pieces behave nothing like the original fragments. Fallen street lights stay dark, even when DE switches lights on at dusk.

### 3. Breakable fragments land on the original ground plane
DE disabled the original `BreakObject_c` code ("Unimplemented BREAKABLES") and simulates fragments in PhysX. The plugin:
- turns the Blueprint's 1 m `PhysicsFloor` into a 30 m plane (configurable) at the ground point and normal that DE already probes, like `BreakObject_c::Init` (glass gets the same probe);
- makes the plane physics-only and invisible to traces, and lets it block only physics bodies, destructibles and the fragments' own collision type, never peds, cars or the camera. CCD is enabled on the fragments.
- removes the pieces after `PiecesDisappearSeconds` (default 6; the original pieces faded after 256–288 frames). It uses the Blueprint's `HideBroken` where the class has one, and otherwise hides the broken mesh and turns off its physics and collision. The ground plane goes with them. The Blueprint's own 6 s `RemoveFloor`, which would drop the pieces through the map, is held back until then. Before touching an actor, the plugin checks in `GObjects` that it is still the same live object (slot, serial, not pending kill). `Lifetime=1` keeps pieces forever; `Lifetime=2` is stock DE.

### 3b. The original piece system (gta-reversed `BreakObject_c`)
The Definitive Edition removed `BreakObject_c` ("Unimplemented BREAKABLES: BreakObject_c::Init") and uses PhysX pieces instead. With `OriginalPieceMotion=1` (default), the plugin runs the original piece algorithm on those pieces, taking each physics body of the prop's broken mesh as one original break group:
- **start** (`SetGroupData`): velocity = the object's own object.dat break velocity (`CObjectInfo+0x3C`, e.g. lamp posts `0,0,0.1` m/step) plus random ± its break-velocity scatter (`+0x48`). Each piece spins at 3–6° per step about a random axis. DE's joints between the pieces are broken (`BreakConstraint`) so they move independently, like the original groups.
- **flight** (`Update`): gravity `timeStep/125` per step (2 g, as in the original). After 5 steps the piece stops tumbling and turns its flattest face to the ground normal at `angle·timeStep/20`.
- **ground** (`DoCollision` / `DoCollisionResponse`): against the original probed ground plane, velocity is reflected with 0.85, scattered by a random `0.05·timeStep`, kept at the same speed and scaled by 0.8. Below 0.05 m per step the piece stops and PhysX gravity takes over, so a tilted piece tips over flat on the plane instead of resting on one corner. DE's piece bones pivot anywhere on the piece, so contact is measured on the piece's real collision: `GetClosestPointOnCollision` from a point 1 km below returns the piece's lowest point, and the piece bounces when that point reaches the plane during the update. A piece in the air can therefore never be mistaken for a landed one. Only a body without a measurable shape falls back to reading PhysX: if PhysX stopped most of the speed sent into the plane, the piece landed.
- **collision**: like the original, pieces touch only their ground plane. They ignore all channels except `Destructible`, the object type of the plugin's planes, so cars, peds, props and other pieces pass through them.
- **fade** (`Render`: `alpha = FramesToLive × 2`): opaque until the last 127.5 steps (`PiecesFadeSeconds=2.55`), then faded through DE's own `AGTAActor::SetAlpha` (0..1). IDA shows that it writes Custom Primitive Data 6 on the actor's mesh components, and DE itself uses it only to fade cars and peds in and out; the prop materials do not show it in game. So when the fade starts, the pieces also stop touching the ground plane and slide under the ground. Each piece sinks at its own speed, measured from its highest collision point, so it is fully below the ground when its life ends. Alpha returns to 1 when the pieces are gone.
- **lifetime**: each piece hides on its own (`HideBoneByName`) after `PiecesDisappearSeconds` (+ random 0–32 steps, like the original's 256–288 frames). When all are gone, the ground plane goes too.

What cannot come back: the original drew RenderWare triangles grouped by material (`SetBreakInfo` / `Render`), and the Definitive Edition renders through Unreal. So the pieces keep DE's own piece meshes. The smoke and spark particles on impact are not reproduced either. Units: RenderWare metres at 1 step = 1/50 s, converted to Unreal centimetres per second with Y mirrored.

### 4. PhysX substepping
Sets `UPhysicsSettings` `bSubstepping` / `MaxSubstepDeltaTime` / `MaxSubsteps` at their real offsets (0x12C / 0x130 / 0x134).

### Settings
`GTA_Prop_Fix.ini` goes next to the `.asi`. Every feature can be turned off there.

### Log
Logging is on by default (`Log=1`). The log goes to `GTA_Prop_Fix.log` next to the `.asi`, or to `%LOCALAPPDATA%\GTA_Prop_Fix\GTA_Prop_Fix.log` when the game folder is read-only. Each session starts a new file. Every prop event is logged with the prop's class, model id, damage effect, uproot limit and position:
- `dislodged->RenderWare`, `rested` (with the lowest z reached), and `WARNING sinking` (the prop fell more than 2 m below where it started);
- `col-check` (RenderWare collision height against visible height, and the verdict);
- `glass-break`, `broken (DE Blueprint)` (with the native path that caused it), `ground-plane`;
- a `stats:` summary every 30 s, written only when something changed.

Repeats of the same event on the same prop are throttled. `Log=2` adds every damage call, light switch and `RemoveFloor`.

### Build detection
Every address is found by a byte signature. If any signature is missing, the plugin installs nothing, so the game runs unmodded instead of crashing. The signatures were verified on the current Steam/RGL `SanAndreas.exe`. The Day-1 build is not supported.

---

## Installation

1. Install an ASI Loader for Definitive Edition (e.g., [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) `dxgi.dll` x64).
2. Download or build `GTA_Prop_Fix.asi`.
3. Copy `GTA_Prop_Fix.asi` and `GTA_Prop_Fix.ini` into:
   ```text
   <GameRoot>\Gameface\Binaries\Win64\
   ```
4. Launch the game.

Offline test (logic plus signatures/hooks against a copy of the exe): `test\run_test.bat path\to\SanAndreas.exe`.

---

## Building from Source

### Prerequisites
- Windows 10 / 11 (x64)
- **Visual Studio 2022** (or VS 2019 / MSVC Build Tools) with the **C++ Desktop Development** workload.

### Build Steps
1. Clone the repository:
   ```bash
   git clone https://github.com/RAZORRZR0/GTA_Prop_Fix.git
   cd GTA_Prop_Fix
   ```
2. Run `build.bat`:
   ```cmd
   build.bat
   ```
3. The compiled ASI binary will be created at:
   ```text
   bin\GTA_Prop_Fix.asi
   ```

---

## Project Structure

```text
GTA_Prop_Fix/
├── .gitignore
├── LICENSE
├── README.md
├── build.bat
├── bin/
│   └── GTA_Prop_Fix.asi
├── minhook/
│   ├── MinHook.h
│   ├── buffer.c
│   ├── buffer.h
│   ├── hook.c
│   ├── trampoline.c
│   ├── trampoline.h
│   └── hde/
│       ├── hde64.c
│       ├── hde64.h
│       ├── pstdint.h
│       └── table64.h
└── src/
    └── dllmain.cpp
```

---

## Compatibility

- **Game:** *Grand Theft Auto: San Andreas – The Definitive Edition* (UE4.26 Gameface x64 build).
- Tested on latest Steam and Rockstar Games Launcher releases.

---

## Credits & Dependencies

- Built with [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu (BSD 2-Clause License).
- Developed by razor.

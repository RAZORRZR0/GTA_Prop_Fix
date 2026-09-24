# GTA_Prop_Fix (v0.0.1)

A lightweight x64 ASI plugin for **Grand Theft Auto: The Trilogy – The Definitive Edition** (specifically targeting *GTA: San Andreas – The Definitive Edition*) that fixes broken streetlights, traffic poles, fences, and dynamic props from falling through the world, ghosting through vehicles, or exploding with chaotic propeller-like physics.

---

## The Problem

In the Definitive Edition, Grove Street Games used a hybrid engine architecture:
1. **Classic 2004 RenderWare Engine:** Runs vehicle handling, player movement, world physics, and map geometry using legacy `.col` collision trees in native C++.
2. **Unreal Engine 4:** Runs purely as a graphics renderer.

When a streetlight or fence breaks, the game hands the fractured mesh (`BrokenMesh`) over to **Unreal Engine 4 PhysX** rigid-body simulation. However:
- **PhysX runs in an empty void:** Unreal Engine has no PhysX collision meshes for the city roads, buildings, or vehicles.
- **Falling Through Ground:** Because there is no terrain collision in PhysX, broken props fell straight through the road and vanished into the void. Grove Street Games attempted a crude workaround (`PhysicsFloor`), but despawned it after a few seconds (`RemoveFloor`), causing resting props to drop through the world.
- **Ghosting Through Vehicles:** Because vehicles have no PhysX collision hulls in UE4, broken lampposts fell straight through car hoods, windshields, and roofs.
- **Unnatural Propeller Spinning & Skyward Catapults:** The game explicitly injected artificial outward velocity vectors (`AddRandomOutwardVelocityToAllBodies`) and large upward launch impulses, causing fences to violently explode outward and light poles to windmill into the sky before floating down in slow motion.

---

## Features

### 1. Zero-Penetration Ground Barrier (No Falling Through the World)
- Intercepts and permanently blocks `RemoveFloor` calls.
- Dynamically scales and positions a $150\text{m} \times 150\text{m} \times 0.2\text{m}$ solid ground barrier centered at `contactZ - 10.0 cm`, keeping its top face perfectly flush with the road.
- Debris rests solidly on the pavement and never falls through the map.

### 2. Natural Rigid-Body Physics & Tipping (No Propeller Spin or Rocket Launch)
- **Neutralized Velocity Injections:** Intercepts and drops calls to `AddRandomOutwardVelocityToAllBodies`, preventing chaotic explosive disassembly.
- **Tamed Upward Launch:** Zeros out vertical launch impulses (`Impulse.Z = 0.0`) in `SetupBroken` and `AddImpulseAtLocationForAllBodiesBelow`.
- **Realistic Bumper Tipping:** Preserves and moderates horizontal bumper momentum ($\le 800\text{ cm/s}$), shoving the base of the pole forward while the top lags behind from inertia, tipping over naturally like real steel/concrete posts.
- **Natural Damping:** Configures natural linear damping (`0.05`) and angular damping (`0.40`) so objects fall with full $9.8\text{ m/s}^2$ gravitational acceleration and settle cleanly.

### 3. Dynamic Kinematic Vehicle Collider (Cars Deflect Falling Props)
- Re-purposes the prop's idle unbroken `Mesh` component into an invisible solid 3D PhysX collision box ($2.2\text{m wide} \times 4.8\text{m long} \times 1.4\text{m tall}$).
- Positions the collider over the vehicle footprint at impact and kinematically translates it along the car's forward velocity vector for $2.5\text{ seconds}$ on the game thread.
- **Result:** Falling poles physically bounce, roll, and deflect off your car's hood and roof instead of ghosting through your vehicle.

### 4. Engine PhysX Substepping
- Enables substepping on `PhysicsSettingsCore` at runtime (`MaxSubsteps = 4`, `MaxDelta = 0.0167s`), stabilizing high-speed physics solver calculations.

---

## Installation

1. Install an ASI Loader for Definitive Edition (e.g., [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) `dxgi.dll` x64).
2. Download or build `GTA_Prop_Fix.asi`.
3. Copy `GTA_Prop_Fix.asi` into:
   ```text
   <GameRoot>\Gameface\Binaries\Win64\
   ```
4. Launch the game. A log file named `GTA_Prop_Fix.log` will be generated in the same folder to confirm initialization.

---

## Building from Source

### Prerequisites
- Windows 10 / 11 (x64)
- **Visual Studio 2022** (or VS 2019 / MSVC Build Tools) with the **C++ Desktop Development** workload.

### Build Steps
1. Clone the repository:
   ```bash
   git clone https://github.com/your-username/GTA_Prop_Fix.git
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

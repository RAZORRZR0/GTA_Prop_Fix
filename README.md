# GTA_Prop_Fix (v0.0.1)

A lightweight x64 ASI plugin for **Grand Theft Auto: The Trilogy – The Definitive Edition** (specifically targeting *GTA: San Andreas – The Definitive Edition*) that fixes broken streetlights, traffic poles, fences, and dynamic street props from falling through the road and vanishing into the void.

---

## The Problem

In the Definitive Edition, Grove Street Games used a hybrid engine architecture:
1. **Classic 2004 RenderWare Engine:** Runs vehicle handling, player movement, world physics, and map geometry using legacy `.col` collision trees in native C++.
2. **Unreal Engine 4:** Runs purely as a graphics renderer.

When a streetlight or fence breaks, the game hands the fractured mesh (`BrokenMesh`) over to **Unreal Engine 4 PhysX** rigid-body simulation. However:
- **PhysX runs in an empty void:** Unreal Engine has no PhysX collision geometry for the city roads or terrain.
- **Falling Through Ground:** Because there is no terrain collision in PhysX, broken props fell straight through the road and vanished into the void.
- Grove Street Games attempted a crude workaround (`PhysicsFloor`), but despawned it after a few seconds (`RemoveFloor`), causing resting props to drop through the world and disappear forever.

---

## Features

### 1. Zero-Penetration Ground Barrier (No Falling Through the World)
- **Blocked Floor Removal:** Intercepts and permanently blocks `RemoveFloor` calls, ensuring the solid ground collision is never deleted.
- **Flush Road Barrier:** Dynamically scales and positions a $150\text{m} \times 150\text{m} \times 0.2\text{m}$ solid ground barrier centered at `contactZ - 10.0 cm`, keeping its top face perfectly flush with the road surface.
- **Detached Collision:** Detaches the floor component from the actor (`KeepWorld`) to break PhysX same-actor collision suppression.
- Broken poles, traffic lights, and fences now solidly collide and rest on the pavement without falling through the map.

### 2. Engine PhysX Substepping
- Enables substepping on `Default__PhysicsSettingsCore` at runtime (`MaxSubsteps = 4`, `MaxDelta = 0.0167s`), eliminating collision tunneling during impacts.

---

## Installation

1. Install an ASI Loader for Definitive Edition (e.g., [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) `dxgi.dll` x64).
2. Download or build `GTA_Prop_Fix.asi`.
3. Copy `GTA_Prop_Fix.asi` into:
   ```text
   <GameRoot>\Gameface\Binaries\Win64\
   ```
4. Launch the game. A log file named `GTA_Prop_Fix.log` will be generated in the same directory to confirm initialization.

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

# eviction-helper

A D3D12 application for testing VRAM eviction behavior on Windows. Allocates a configurable amount of GPU memory to influence the memory budget of other applications.

## Features

- Allocates offscreen render targets to consume VRAM (0-16 GB configurable)
- Renders to all allocated targets every frame to keep memory resident
- Sets resource residency priority to HIGH
- Displays real-time DXGI video memory statistics via ImGui
- Runs at fixed 30 FPS
- **Shared memory interface** for control from external applications

## Building

Requires Visual Studio 2022 with C++ workload.

1. Open `EvictionHelper.sln` in Visual Studio 2022
2. Build Release x64 configuration
3. Run `bin\Release\EvictionHelper.exe`

Or build from command line:
```batch
"C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe" EvictionHelper.sln -p:Configuration=Release -p:Platform=x64
```

## Usage

### Standalone
Run `EvictionHelper.exe` and use the slider to set target VRAM usage. The application will allocate 2048x2048 RGBA8 render targets until the target is reached.

### Controlled from another application
Include `src/eviction_helper_shared.h` in your project and use the shared memory interface:

```cpp
#include "eviction_helper_shared.h"

EvictionHelperSharedMemory sharedMem;
if (EvictionHelper_OpenSharedMemory(&sharedMem)) {
    // Set target VRAM usage (in megabytes)
    sharedMem.pData->TargetVRAMUsageMB = 4096; // 4 GB

    // Read current state
    printf("Current VRAM usage: %llu bytes\n", sharedMem.pData->LocalCurrentUsage);
    printf("VRAM budget: %llu bytes\n", sharedMem.pData->LocalBudget);
    printf("Allocated: %llu bytes in %u render targets\n",
           sharedMem.pData->CurrentVRAMAllocationBytes,
           sharedMem.pData->AllocatedRenderTargetCount);

    // Verify app is running by checking frame counter changes
    uint64_t lastFrame = sharedMem.pData->FrameCount;
    Sleep(100);
    bool isRunning = (sharedMem.pData->FrameCount != lastFrame);

    // Request shutdown
    sharedMem.pData->RequestShutdown = 1;

    EvictionHelper_CloseSharedMemory(&sharedMem);
}
```

## Shared Memory Structure

Name: `Local\EvictionHelperSharedMemory`

```cpp
struct EvictionHelperSharedData
{
    // Input
    int TargetVRAMUsageMB;              // Set desired VRAM allocation in MB

    // Output - Allocation state
    uint64_t CurrentVRAMAllocationBytes;
    uint32_t AllocatedRenderTargetCount;

    // Output - Local (VRAM) memory info
    uint64_t LocalBudget;
    uint64_t LocalCurrentUsage;
    uint64_t LocalAvailableForReservation;
    uint64_t LocalCurrentReservation;

    // Output - Non-local (system) memory info
    uint64_t NonLocalBudget;
    uint64_t NonLocalCurrentUsage;
    uint64_t NonLocalAvailableForReservation;
    uint64_t NonLocalCurrentReservation;

    // Status
    uint32_t IsRunning;                 // 1 while app is running
    uint32_t RequestShutdown;           // Set to 1 to request exit
    uint64_t FrameCount;                // Increments each frame
};
```

## Dependencies

- Windows 10/11
- DirectX 12 compatible GPU
- [Dear ImGui](https://github.com/ocornut/imgui) (included)

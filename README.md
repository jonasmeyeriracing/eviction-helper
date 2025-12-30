# eviction-helper

A D3D12 application for testing VRAM eviction behavior on Windows. Allocates a configurable amount of GPU memory to influence the memory budget of other applications.

## Features

- Allocates offscreen render targets to consume VRAM (0-16 GB configurable)
- **Active VRAM**: Rendered to every frame to keep memory resident
- **Unused VRAM**: Allocated but not rendered to (tests eviction of idle resources)
- **Configurable residency priority** (Minimum/Low/Normal/High/Maximum) for:
  - Active VRAM allocations
  - Unused VRAM allocations
  - D3D12 Heaps
- Priority changes apply to existing allocations in real-time
- Displays real-time DXGI video memory statistics via ImGui
- Shows memory breakdown by priority level
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
Run `EvictionHelper.exe` and use the sliders to set target VRAM usage for both active and unused memory. The application allocates 2048x2048 RGBA8 render targets until the targets are reached. Use the priority dropdowns to control residency priority for each memory type.

### Controlled from another application
Include `src/eviction_helper_shared.h` in your project and use the shared memory interface:

```cpp
#include "eviction_helper_shared.h"

EvictionHelperSharedMemory sharedMem;
if (EvictionHelper_OpenSharedMemory(&sharedMem)) {
    // Set target VRAM usage (in megabytes)
    sharedMem.pData->TargetVRAMUsageMB = 4096;        // 4 GB active VRAM
    sharedMem.pData->TargetUnusedVRAMUsageMB = 2048;  // 2 GB unused VRAM

    // Set residency priorities (0=Minimum, 1=Low, 2=Normal, 3=High, 4=Maximum)
    sharedMem.pData->ActiveVRAMPriority = EVICTION_HELPER_PRIORITY_HIGH;
    sharedMem.pData->UnusedVRAMPriority = EVICTION_HELPER_PRIORITY_MINIMUM;
    sharedMem.pData->HeapPriority = EVICTION_HELPER_PRIORITY_NORMAL;

    // Read current state
    printf("Current VRAM usage: %llu bytes\n", sharedMem.pData->LocalCurrentUsage);
    printf("VRAM budget: %llu bytes\n", sharedMem.pData->LocalBudget);
    printf("Active: %llu bytes in %u render targets\n",
           sharedMem.pData->CurrentVRAMAllocationBytes,
           sharedMem.pData->AllocatedRenderTargetCount);
    printf("Unused: %llu bytes in %u render targets\n",
           sharedMem.pData->CurrentUnusedVRAMAllocationBytes,
           sharedMem.pData->AllocatedUnusedRenderTargetCount);

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
// Priority values (maps to D3D12_RESIDENCY_PRIORITY)
#define EVICTION_HELPER_PRIORITY_MINIMUM  0
#define EVICTION_HELPER_PRIORITY_LOW      1
#define EVICTION_HELPER_PRIORITY_NORMAL   2
#define EVICTION_HELPER_PRIORITY_HIGH     3
#define EVICTION_HELPER_PRIORITY_MAXIMUM  4

struct EvictionHelperSharedData
{
    // Input - Memory targets
    int TargetVRAMUsageMB;              // Active VRAM allocation in MB (rendered each frame)
    int TargetUnusedVRAMUsageMB;        // Unused VRAM allocation in MB (allocated but idle)

    // Input - Residency priorities (0-4, see EVICTION_HELPER_PRIORITY_*)
    int ActiveVRAMPriority;             // Priority for active VRAM (default: LOW)
    int UnusedVRAMPriority;             // Priority for unused VRAM (default: MINIMUM)
    int HeapPriority;                   // Priority for D3D12 heaps (default: NORMAL)

    // Output - Active allocation state
    uint64_t CurrentVRAMAllocationBytes;
    uint32_t AllocatedRenderTargetCount;

    // Output - Unused allocation state
    uint64_t CurrentUnusedVRAMAllocationBytes;
    uint32_t AllocatedUnusedRenderTargetCount;

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

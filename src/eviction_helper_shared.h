#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

// Shared memory name - use this to open from other processes
#define EVICTION_HELPER_SHARED_MEMORY_NAME "Local\\EvictionHelperSharedMemory"

// Priority values (maps to D3D12_RESIDENCY_PRIORITY)
// 0 = MINIMUM, 1 = LOW, 2 = NORMAL, 3 = HIGH, 4 = MAXIMUM
#define EVICTION_HELPER_PRIORITY_MINIMUM  0
#define EVICTION_HELPER_PRIORITY_LOW      1
#define EVICTION_HELPER_PRIORITY_NORMAL   2
#define EVICTION_HELPER_PRIORITY_HIGH     3
#define EVICTION_HELPER_PRIORITY_MAXIMUM  4

// Shared data structure between eviction-helper and controlling applications
struct EvictionHelperSharedData
{
    // Input: Set this from the controlling application (in megabytes)
    int TargetVRAMUsageMB;          // Memory that is actively used (rendered to each frame)
    int TargetUnusedVRAMUsageMB;    // Memory that is allocated but not used

    // Input: Residency priority for each memory type (0-4, see EVICTION_HELPER_PRIORITY_*)
    int ActiveVRAMPriority;         // Priority for active VRAM (default: HIGH)
    int UnusedVRAMPriority;         // Priority for unused VRAM (default: NORMAL)

    // Input: D3D12 Heap allocation flags
    int Allocate512MBHeap;          // Set to 1 to allocate a 512 MB heap
    int Allocate1GBHeap;            // Set to 1 to allocate a 1 GB heap

    // Output: Current allocation state (actively used)
    uint64_t CurrentVRAMAllocationBytes;
    uint32_t AllocatedRenderTargetCount;
    uint32_t _padding0;

    // Output: Current allocation state (unused/idle)
    uint64_t CurrentUnusedVRAMAllocationBytes;
    uint32_t AllocatedUnusedRenderTargetCount;
    uint32_t _padding2;

    // Output: Current heap allocation
    uint64_t CurrentHeapAllocationBytes;

    // Output: DXGI_QUERY_VIDEO_MEMORY_INFO for local (VRAM) memory
    uint64_t LocalBudget;
    uint64_t LocalCurrentUsage;
    uint64_t LocalAvailableForReservation;
    uint64_t LocalCurrentReservation;

    // Output: DXGI_QUERY_VIDEO_MEMORY_INFO for non-local (system) memory
    uint64_t NonLocalBudget;
    uint64_t NonLocalCurrentUsage;
    uint64_t NonLocalAvailableForReservation;
    uint64_t NonLocalCurrentReservation;

    // Status flags
    uint32_t IsRunning;         // Set to 1 while eviction-helper is running
    uint32_t RequestShutdown;   // Set to 1 from controller to request shutdown

    // Frame counter - increments each frame, use to verify app is running
    uint64_t FrameCount;
};

// Helper struct for managing shared memory handle and pointer
struct EvictionHelperSharedMemory
{
    HANDLE hMapFile;
    EvictionHelperSharedData* pData;
};

// Create shared memory (call from eviction-helper)
// Returns true on success, false on failure
inline bool EvictionHelper_CreateSharedMemory(EvictionHelperSharedMemory* outSharedMem)
{
    if (!outSharedMem) return false;

    outSharedMem->hMapFile = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        sizeof(EvictionHelperSharedData),
        EVICTION_HELPER_SHARED_MEMORY_NAME
    );

    if (outSharedMem->hMapFile == NULL)
    {
        return false;
    }

    outSharedMem->pData = (EvictionHelperSharedData*)MapViewOfFile(
        outSharedMem->hMapFile,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        sizeof(EvictionHelperSharedData)
    );

    if (outSharedMem->pData == NULL)
    {
        CloseHandle(outSharedMem->hMapFile);
        outSharedMem->hMapFile = NULL;
        return false;
    }

    // Zero initialize
    memset(outSharedMem->pData, 0, sizeof(EvictionHelperSharedData));
    return true;
}

// Open existing shared memory (call from controlling application)
// Returns true on success, false on failure (e.g., eviction-helper not running)
inline bool EvictionHelper_OpenSharedMemory(EvictionHelperSharedMemory* outSharedMem)
{
    if (!outSharedMem) return false;

    outSharedMem->hMapFile = OpenFileMappingA(
        FILE_MAP_ALL_ACCESS,
        FALSE,
        EVICTION_HELPER_SHARED_MEMORY_NAME
    );

    if (outSharedMem->hMapFile == NULL)
    {
        return false;
    }

    outSharedMem->pData = (EvictionHelperSharedData*)MapViewOfFile(
        outSharedMem->hMapFile,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        sizeof(EvictionHelperSharedData)
    );

    if (outSharedMem->pData == NULL)
    {
        CloseHandle(outSharedMem->hMapFile);
        outSharedMem->hMapFile = NULL;
        return false;
    }

    return true;
}

// Close shared memory (call from both eviction-helper and controlling application)
inline void EvictionHelper_CloseSharedMemory(EvictionHelperSharedMemory* sharedMem)
{
    if (!sharedMem) return;

    if (sharedMem->pData)
    {
        UnmapViewOfFile(sharedMem->pData);
        sharedMem->pData = NULL;
    }

    if (sharedMem->hMapFile)
    {
        CloseHandle(sharedMem->hMapFile);
        sharedMem->hMapFile = NULL;
    }
}

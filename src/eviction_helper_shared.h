#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

// Shared memory name - use this to open from other processes
#define EVICTION_HELPER_SHARED_MEMORY_NAME "Local\\EvictionHelperSharedMemory"

// Shared data structure between eviction-helper and controlling applications
struct EvictionHelperSharedData
{
    // Input: Set this from the controlling application (in megabytes)
    int TargetVRAMUsageMB;
    int _padding1;

    // Output: Current allocation state
    uint64_t CurrentVRAMAllocationBytes;
    uint32_t AllocatedRenderTargetCount;
    uint32_t _padding0;

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

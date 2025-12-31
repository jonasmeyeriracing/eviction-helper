#pragma once

#include "eviction_helper_shared.h"
#include "imgui.h"

// Priority names for ImGui combo boxes
inline const char* EvictionHelper_PriorityNames[] = { "Minimum", "Low", "Normal", "High", "Maximum" };

// Render the Eviction Helper ImGui UI contents (without Begin/End)
// Call this between ImGui::Begin() and ImGui::End() to render the UI
// This function can be called from any application that has access to the shared memory
// Parameters:
//   data - Pointer to shared memory data (required)
//   additionalHeapAllocation - Optional: bytes allocated in D3D12 heaps (for display purposes)
inline void EvictionHelper_RenderImGui(EvictionHelperSharedData* data)
{
	if (!data)
		return;

	ImGui::SeparatorText("Active VRAM (rendered each frame):");
	ImGui::Combo("Active Priority", &data->ActiveVRAMPriority, EvictionHelper_PriorityNames, IM_ARRAYSIZE(EvictionHelper_PriorityNames));
	ImGui::SliderInt("Active MB", &data->TargetVRAMUsageMB, 0, 32 << 10, "%d MB");

	ImGui::SeparatorText("Unused VRAM (allocated but idle):");
	ImGui::Combo("Unused Priority", &data->UnusedVRAMPriority, EvictionHelper_PriorityNames, IM_ARRAYSIZE(EvictionHelper_PriorityNames));
	ImGui::SliderInt("Unused MB", &data->TargetUnusedVRAMUsageMB, 0, 32 << 10, "%d MB");
	bool alloc512MB = data->Allocate512MBHeap != 0;
	bool alloc1GB = data->Allocate1GBHeap != 0;
	if (ImGui::Checkbox("Allocate 512 MB Heap", &alloc512MB))
		data->Allocate512MBHeap = alloc512MB ? 1 : 0;
	if (ImGui::Checkbox("Allocate 1 GB Heap", &alloc1GB))
		data->Allocate1GBHeap = alloc1GB ? 1 : 0;

	ImGui::SeparatorText("Memory Usage");
	uint64_t heapAllocation = data->CurrentHeapAllocationBytes;
	uint64_t totalMemory = data->CurrentVRAMAllocationBytes + data->CurrentUnusedVRAMAllocationBytes + heapAllocation;
	ImGui::Text("Active Render Targets: %u", data->AllocatedRenderTargetCount);
	ImGui::Text("Active VRAM: %.2f GB", data->CurrentVRAMAllocationBytes / (1024.0 * 1024.0 * 1024.0));
	ImGui::Text("Unused Render Targets: %u", data->AllocatedUnusedRenderTargetCount);
	ImGui::Text("Unused VRAM: %.2f GB", data->CurrentUnusedVRAMAllocationBytes / (1024.0 * 1024.0 * 1024.0));
	if (heapAllocation > 0)
	{
		ImGui::Text("Unused Heaps: %.2f GB", heapAllocation / (1024.0 * 1024.0 * 1024.0));
	}
	ImGui::Text("Total VRAM Usage: %.2f GB", totalMemory / (1024.0 * 1024.0 * 1024.0));

	// Calculate memory by priority level
	uint64_t memoryByPriority[5] = { 0, 0, 0, 0, 0 };
	int activePri = data->ActiveVRAMPriority;
	int unusedPri = data->UnusedVRAMPriority;
	if (activePri >= 0 && activePri <= 4)
		memoryByPriority[activePri] += data->CurrentVRAMAllocationBytes;
	if (unusedPri >= 0 && unusedPri <= 4)
	{
		memoryByPriority[unusedPri] += data->CurrentUnusedVRAMAllocationBytes;
		memoryByPriority[unusedPri] += heapAllocation;
	}

	ImGui::SeparatorText("Memory by Priority");
	for (int i = 0; i < 5; i++)
	{
		if (memoryByPriority[i] > 0)
		{
			ImGui::Text("  %s: %.2f GB", EvictionHelper_PriorityNames[i], memoryByPriority[i] / (1024.0 * 1024.0 * 1024.0));
		}
	}

	ImGui::SeparatorText("Video Memory Info");
	ImGui::Text("Local:");
	ImGui::Text("  Budget: %.2f GB", data->LocalBudget / (1024.0 * 1024.0 * 1024.0));
	ImGui::Text("  Current Usage: %.2f GB", data->LocalCurrentUsage / (1024.0 * 1024.0 * 1024.0));
	ImGui::Text("  Available for Reservation: %.2f GB", data->LocalAvailableForReservation / (1024.0 * 1024.0 * 1024.0));
	ImGui::Text("  Current Reservation: %.2f GB", data->LocalCurrentReservation / (1024.0 * 1024.0 * 1024.0));

	ImGui::Text("Non-Local:");
	ImGui::Text("  Budget: %.2f GB", data->NonLocalBudget / (1024.0 * 1024.0 * 1024.0));
	ImGui::Text("  Current Usage: %.2f GB", data->NonLocalCurrentUsage / (1024.0 * 1024.0 * 1024.0));
	ImGui::Text("  Available for Reservation: %.2f GB", data->NonLocalAvailableForReservation / (1024.0 * 1024.0 * 1024.0));
	ImGui::Text("  Current Reservation: %.2f GB", data->NonLocalCurrentReservation / (1024.0 * 1024.0 * 1024.0));
}

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <vector>
#include <string>
#include <chrono>
#include <algorithm>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include "eviction_helper_shared.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// Window dimensions
constexpr UINT WINDOW_WIDTH = 1024;
constexpr UINT WINDOW_HEIGHT = 720;
constexpr UINT NUM_FRAMES = 2;

// Forward declarations
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Global state
struct FrameContext {
    ComPtr<ID3D12CommandAllocator> CommandAllocator;
    UINT64 FenceValue;
};

// D3D12 objects
ComPtr<ID3D12Device1> g_Device;
ComPtr<ID3D12CommandQueue> g_CommandQueue;
ComPtr<IDXGISwapChain3> g_SwapChain;
ComPtr<ID3D12DescriptorHeap> g_RtvHeap;
ComPtr<ID3D12DescriptorHeap> g_SrvHeap;
ComPtr<ID3D12Resource> g_RenderTargets[NUM_FRAMES];
ComPtr<ID3D12GraphicsCommandList> g_CommandList;
ComPtr<ID3D12Fence> g_Fence;
ComPtr<ID3D12RootSignature> g_RootSignature;
ComPtr<ID3D12PipelineState> g_PipelineState;
ComPtr<ID3D12Resource> g_VertexBuffer;
D3D12_VERTEX_BUFFER_VIEW g_VertexBufferView;

FrameContext g_FrameContext[NUM_FRAMES];
UINT g_FrameIndex = 0;
HANDLE g_FenceEvent = nullptr;
UINT64 g_FenceValue = 0;
UINT g_RtvDescriptorSize = 0;

// VRAM management
struct VRAMRenderTarget {
    ComPtr<ID3D12Resource> Resource;
    D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle;
};

std::vector<VRAMRenderTarget> g_VRAMRenderTargets;
ComPtr<ID3D12DescriptorHeap> g_VRAMRtvHeap;
constexpr UINT64 RENDER_TARGET_SIZE_MB = 64; // Each RT is 64MB (2048x2048 RGBA8)
constexpr UINT RT_WIDTH = 2048;
constexpr UINT RT_HEIGHT = 2048;

// Shared memory for inter-process communication
EvictionHelperSharedMemory g_SharedMem = {};

// Adapter for memory queries
ComPtr<IDXGIAdapter3> g_Adapter;

// Timing
constexpr double TARGET_FRAME_TIME_MS = 1000.0 / 30.0; // 30 FPS

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
void WaitForLastSubmittedFrame();
void WaitForGpu();
FrameContext* WaitForNextFrameResources();
void CreateTrianglePipeline();
void AllocateVRAMRenderTargets(UINT64 targetBytes);
void RenderToAllVRAMTargets();
void QueryMemoryInfo();

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    // Create shared memory for inter-process communication
    if (!EvictionHelper_CreateSharedMemory(&g_SharedMem)) {
        MessageBoxA(NULL, "Failed to create shared memory", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    g_SharedMem.pData->IsRunning = 1;

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"EvictionHelperClass";
    RegisterClassExW(&wc);

    // Create window
    RECT rc = { 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hWnd = CreateWindowW(
        L"EvictionHelperClass",
        L"VRAM Eviction Helper",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!CreateDeviceD3D(hWnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hWnd);

    // Setup DX12 backend using new API
    ImGui_ImplDX12_InitInfo init_info = {};
    init_info.Device = g_Device.Get();
    init_info.CommandQueue = g_CommandQueue.Get();
    init_info.NumFramesInFlight = NUM_FRAMES;
    init_info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    init_info.SrvDescriptorHeap = g_SrvHeap.Get();
    init_info.LegacySingleSrvCpuDescriptor = g_SrvHeap->GetCPUDescriptorHandleForHeapStart();
    init_info.LegacySingleSrvGpuDescriptor = g_SrvHeap->GetGPUDescriptorHandleForHeapStart();
    ImGui_ImplDX12_Init(&init_info);

    // Main loop
    MSG msg = {};
    bool running = true;
    auto lastFrameTime = std::chrono::high_resolution_clock::now();

    while (running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                running = false;
            }
        }

        if (!running) break;

        // Check for shutdown request from shared memory
        if (g_SharedMem.pData->RequestShutdown) {
            running = false;
            break;
        }

        // Frame timing for 30 FPS cap
        auto currentTime = std::chrono::high_resolution_clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(currentTime - lastFrameTime).count();

        if (elapsedMs < TARGET_FRAME_TIME_MS) {
            DWORD sleepTime = static_cast<DWORD>(TARGET_FRAME_TIME_MS - elapsedMs);
            if (sleepTime > 0) {
                Sleep(sleepTime);
            }
            continue;
        }
        lastFrameTime = std::chrono::high_resolution_clock::now();

        // Query memory info and update shared memory
        QueryMemoryInfo();

        // Update VRAM allocation based on shared memory target (MB -> bytes)
        UINT64 targetBytes = static_cast<UINT64>(g_SharedMem.pData->TargetVRAMUsageMB) * 1024ULL * 1024ULL;
        if (targetBytes != g_SharedMem.pData->CurrentVRAMAllocationBytes) {
            AllocateVRAMRenderTargets(targetBytes);
        }

        // Start ImGui frame
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // ImGui window
        ImGui::Begin("VRAM Eviction Helper", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::Text("Target VRAM Usage:");
        ImGui::SliderInt("MB", &g_SharedMem.pData->TargetVRAMUsageMB, 0, 16384, "%d MB");

        ImGui::Separator();
        ImGui::Text("Allocated Render Targets: %u", g_SharedMem.pData->AllocatedRenderTargetCount);
        ImGui::Text("Allocated VRAM: %.2f GB", g_SharedMem.pData->CurrentVRAMAllocationBytes / (1024.0 * 1024.0 * 1024.0));

        ImGui::Separator();
        ImGui::Text("Video Memory Info (Local/VRAM):");
        ImGui::Text("  Budget: %.2f GB", g_SharedMem.pData->LocalBudget / (1024.0 * 1024.0 * 1024.0));
        ImGui::Text("  Current Usage: %.2f GB", g_SharedMem.pData->LocalCurrentUsage / (1024.0 * 1024.0 * 1024.0));
        ImGui::Text("  Available for Reservation: %.2f GB", g_SharedMem.pData->LocalAvailableForReservation / (1024.0 * 1024.0 * 1024.0));
        ImGui::Text("  Current Reservation: %.2f GB", g_SharedMem.pData->LocalCurrentReservation / (1024.0 * 1024.0 * 1024.0));

        ImGui::Separator();
        ImGui::Text("Video Memory Info (Non-Local/System):");
        ImGui::Text("  Budget: %.2f GB", g_SharedMem.pData->NonLocalBudget / (1024.0 * 1024.0 * 1024.0));
        ImGui::Text("  Current Usage: %.2f GB", g_SharedMem.pData->NonLocalCurrentUsage / (1024.0 * 1024.0 * 1024.0));
        ImGui::Text("  Available for Reservation: %.2f GB", g_SharedMem.pData->NonLocalAvailableForReservation / (1024.0 * 1024.0 * 1024.0));
        ImGui::Text("  Current Reservation: %.2f GB", g_SharedMem.pData->NonLocalCurrentReservation / (1024.0 * 1024.0 * 1024.0));

        ImGui::Separator();
        ImGui::Text("Frame Rate: 30 FPS (fixed)");
        ImGui::Text("Shared Memory: Active");

        ImGui::End();

        ImGui::Render();

        // Render
        FrameContext* frameCtx = WaitForNextFrameResources();
        UINT backBufferIdx = g_SwapChain->GetCurrentBackBufferIndex();

        frameCtx->CommandAllocator->Reset();
        g_CommandList->Reset(frameCtx->CommandAllocator.Get(), g_PipelineState.Get());

        // Render to all VRAM targets to keep them resident
        RenderToAllVRAMTargets();

        // Transition back buffer to render target
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = g_RenderTargets[backBufferIdx].Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_CommandList->ResourceBarrier(1, &barrier);

        // Clear and set render target
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_RtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtvHandle.ptr += backBufferIdx * g_RtvDescriptorSize;

        const float clearColor[] = { 0.1f, 0.1f, 0.2f, 1.0f };
        g_CommandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        g_CommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        // Set viewport and scissor
        D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)WINDOW_WIDTH, (float)WINDOW_HEIGHT, 0.0f, 1.0f };
        D3D12_RECT scissor = { 0, 0, (LONG)WINDOW_WIDTH, (LONG)WINDOW_HEIGHT };
        g_CommandList->RSSetViewports(1, &viewport);
        g_CommandList->RSSetScissorRects(1, &scissor);

        // Draw triangle
        g_CommandList->SetGraphicsRootSignature(g_RootSignature.Get());
        g_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_CommandList->IASetVertexBuffers(0, 1, &g_VertexBufferView);
        g_CommandList->DrawInstanced(3, 1, 0, 0);

        // Render ImGui
        ID3D12DescriptorHeap* heaps[] = { g_SrvHeap.Get() };
        g_CommandList->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_CommandList.Get());

        // Transition back buffer to present
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_CommandList->ResourceBarrier(1, &barrier);

        g_CommandList->Close();

        // Execute
        ID3D12CommandList* cmdLists[] = { g_CommandList.Get() };
        g_CommandQueue->ExecuteCommandLists(1, cmdLists);

        // Present
        g_SwapChain->Present(0, 0);

        // Signal fence
        UINT64 fenceValue = g_FenceValue++;
        g_CommandQueue->Signal(g_Fence.Get(), fenceValue);
        frameCtx->FenceValue = fenceValue;

        // Increment frame counter for external monitoring
        g_SharedMem.pData->FrameCount++;
    }

    WaitForGpu();

    // Cleanup
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    g_VRAMRenderTargets.clear();
    g_VRAMRtvHeap.Reset();
    CleanupDeviceD3D();

    // Cleanup shared memory
    if (g_SharedMem.pData) {
        g_SharedMem.pData->IsRunning = 0;
    }
    EvictionHelper_CloseSharedMemory(&g_SharedMem);

    DestroyWindow(hWnd);
    UnregisterClassW(wc.lpszClassName, hInstance);

    return 0;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        // Handle resize if needed
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool CreateDeviceD3D(HWND hWnd)
{
    // Create DXGI Factory
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
    }

    // Get adapter
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
            device.As(&g_Device);
            adapter.As(&g_Adapter);
            break;
        }
    }

    if (!g_Device) {
        return false;
    }

    // Create command queue
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_CommandQueue)))) {
        return false;
    }

    // Create swap chain
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = WINDOW_WIDTH;
    swapChainDesc.Height = WINDOW_HEIGHT;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = NUM_FRAMES;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swapChain1;
    if (FAILED(factory->CreateSwapChainForHwnd(g_CommandQueue.Get(), hWnd, &swapChainDesc, nullptr, nullptr, &swapChain1))) {
        return false;
    }
    swapChain1.As(&g_SwapChain);

    // Disable Alt+Enter fullscreen
    factory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);

    // Create RTV descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = NUM_FRAMES;
    if (FAILED(g_Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_RtvHeap)))) {
        return false;
    }
    g_RtvDescriptorSize = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Create SRV descriptor heap for ImGui
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_SrvHeap)))) {
        return false;
    }

    // Create frame resources
    for (UINT i = 0; i < NUM_FRAMES; i++) {
        if (FAILED(g_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_FrameContext[i].CommandAllocator)))) {
            return false;
        }
    }

    // Create command list
    if (FAILED(g_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_FrameContext[0].CommandAllocator.Get(), nullptr, IID_PPV_ARGS(&g_CommandList)))) {
        return false;
    }
    g_CommandList->Close();

    // Create fence
    if (FAILED(g_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_Fence)))) {
        return false;
    }
    g_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    CreateRenderTarget();
    CreateTrianglePipeline();

    return true;
}

void CreateRenderTarget()
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < NUM_FRAMES; i++) {
        g_SwapChain->GetBuffer(i, IID_PPV_ARGS(&g_RenderTargets[i]));
        g_Device->CreateRenderTargetView(g_RenderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += g_RtvDescriptorSize;
    }
}

void CleanupRenderTarget()
{
    for (UINT i = 0; i < NUM_FRAMES; i++) {
        g_RenderTargets[i].Reset();
    }
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();

    if (g_FenceEvent) {
        CloseHandle(g_FenceEvent);
        g_FenceEvent = nullptr;
    }

    g_Fence.Reset();
    g_CommandList.Reset();
    for (UINT i = 0; i < NUM_FRAMES; i++) {
        g_FrameContext[i].CommandAllocator.Reset();
    }
    g_VertexBuffer.Reset();
    g_PipelineState.Reset();
    g_RootSignature.Reset();
    g_SrvHeap.Reset();
    g_RtvHeap.Reset();
    g_SwapChain.Reset();
    g_CommandQueue.Reset();
    g_Adapter.Reset();
    g_Device.Reset();
}

void WaitForLastSubmittedFrame()
{
    FrameContext* frameCtx = &g_FrameContext[g_FrameIndex % NUM_FRAMES];
    UINT64 fenceValue = frameCtx->FenceValue;
    if (fenceValue == 0) return;

    if (g_Fence->GetCompletedValue() < fenceValue) {
        g_Fence->SetEventOnCompletion(fenceValue, g_FenceEvent);
        WaitForSingleObject(g_FenceEvent, INFINITE);
    }
}

void WaitForGpu()
{
    UINT64 fenceValue = g_FenceValue++;
    g_CommandQueue->Signal(g_Fence.Get(), fenceValue);
    g_Fence->SetEventOnCompletion(fenceValue, g_FenceEvent);
    WaitForSingleObject(g_FenceEvent, INFINITE);
}

FrameContext* WaitForNextFrameResources()
{
    UINT nextFrameIndex = g_SwapChain->GetCurrentBackBufferIndex();
    g_FrameIndex = nextFrameIndex;

    FrameContext* frameCtx = &g_FrameContext[nextFrameIndex];
    UINT64 fenceValue = frameCtx->FenceValue;
    if (fenceValue != 0 && g_Fence->GetCompletedValue() < fenceValue) {
        g_Fence->SetEventOnCompletion(fenceValue, g_FenceEvent);
        WaitForSingleObject(g_FenceEvent, INFINITE);
    }

    return frameCtx;
}

void CreateTrianglePipeline()
{
    // Vertex shader
    const char* vsSource = R"(
        struct VSInput {
            float3 position : POSITION;
            float4 color : COLOR;
        };
        struct PSInput {
            float4 position : SV_POSITION;
            float4 color : COLOR;
        };
        PSInput main(VSInput input) {
            PSInput output;
            output.position = float4(input.position, 1.0);
            output.color = input.color;
            return output;
        }
    )";

    // Pixel shader
    const char* psSource = R"(
        struct PSInput {
            float4 position : SV_POSITION;
            float4 color : COLOR;
        };
        float4 main(PSInput input) : SV_TARGET {
            return input.color;
        }
    )";

    ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;

    D3DCompile(vsSource, strlen(vsSource), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &errorBlob);
    D3DCompile(psSource, strlen(psSource), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &errorBlob);

    // Create root signature
    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signatureBlob;
    D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &errorBlob);
    g_Device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(), IID_PPV_ARGS(&g_RootSignature));

    // Input layout
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    // Pipeline state
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.pRootSignature = g_RootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    g_Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_PipelineState));

    // Create vertex buffer
    struct Vertex {
        float position[3];
        float color[4];
    };

    Vertex vertices[] = {
        { {  0.0f,  0.5f, 0.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
        { {  0.5f, -0.5f, 0.0f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
        { { -0.5f, -0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } }
    };

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = sizeof(vertices);
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    g_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_VertexBuffer));

    // Upload vertex data
    void* mappedData;
    g_VertexBuffer->Map(0, nullptr, &mappedData);
    memcpy(mappedData, vertices, sizeof(vertices));
    g_VertexBuffer->Unmap(0, nullptr);

    g_VertexBufferView.BufferLocation = g_VertexBuffer->GetGPUVirtualAddress();
    g_VertexBufferView.SizeInBytes = sizeof(vertices);
    g_VertexBufferView.StrideInBytes = sizeof(Vertex);
}

void AllocateVRAMRenderTargets(UINT64 targetBytes)
{
    WaitForGpu();

    // Calculate how many render targets we need
    // Each RT is RT_WIDTH x RT_HEIGHT x 4 bytes (RGBA8)
    UINT64 rtSize = RT_WIDTH * RT_HEIGHT * 4;
    size_t targetCount = (targetBytes > 0) ? static_cast<size_t>((targetBytes + rtSize - 1) / rtSize) : 0;

    // Release excess render targets
    while (g_VRAMRenderTargets.size() > targetCount) {
        g_VRAMRenderTargets.pop_back();
    }

    // Recreate RTV heap if we need more descriptors
    if (targetCount > 0) {
        size_t currentHeapSize = 0;
        if (g_VRAMRtvHeap) {
            currentHeapSize = g_VRAMRtvHeap->GetDesc().NumDescriptors;
        }

        if (targetCount > currentHeapSize) {
            // Need a bigger heap - save existing resources
            std::vector<ComPtr<ID3D12Resource>> existingResources;
            for (auto& rt : g_VRAMRenderTargets) {
                existingResources.push_back(rt.Resource);
            }
            g_VRAMRenderTargets.clear();
            g_VRAMRtvHeap.Reset();

            // Create new heap
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heapDesc.NumDescriptors = static_cast<UINT>(targetCount + 64); // Add some padding
            g_Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_VRAMRtvHeap));

            // Restore existing resources with new RTV handles
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_VRAMRtvHeap->GetCPUDescriptorHandleForHeapStart();
            for (auto& resource : existingResources) {
                VRAMRenderTarget vramRT;
                vramRT.Resource = resource;
                vramRT.RtvHandle = rtvHandle;
                g_Device->CreateRenderTargetView(resource.Get(), nullptr, rtvHandle);
                g_VRAMRenderTargets.push_back(std::move(vramRT));
                rtvHandle.ptr += g_RtvDescriptorSize;
            }
        }
    }

    // Allocate new render targets
    while (g_VRAMRenderTargets.size() < targetCount) {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = RT_WIDTH;
        texDesc.Height = RT_HEIGHT;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        clearValue.Color[0] = 0.0f;
        clearValue.Color[1] = 0.0f;
        clearValue.Color[2] = 0.0f;
        clearValue.Color[3] = 1.0f;

        VRAMRenderTarget vramRT;
        HRESULT hr = g_Device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            &clearValue,
            IID_PPV_ARGS(&vramRT.Resource)
        );

        if (FAILED(hr)) {
            // Out of VRAM, stop allocating
            break;
        }

        // Set residency priority to HIGH
        ID3D12Pageable* pageable = vramRT.Resource.Get();
        D3D12_RESIDENCY_PRIORITY priority = D3D12_RESIDENCY_PRIORITY_HIGH;
        g_Device->SetResidencyPriority(1, &pageable, &priority);

        // Create RTV
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_VRAMRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtvHandle.ptr += g_VRAMRenderTargets.size() * g_RtvDescriptorSize;
        vramRT.RtvHandle = rtvHandle;
        g_Device->CreateRenderTargetView(vramRT.Resource.Get(), nullptr, rtvHandle);

        g_VRAMRenderTargets.push_back(std::move(vramRT));
    }

    // Update shared memory with current allocation
    if (g_SharedMem.pData) {
        g_SharedMem.pData->CurrentVRAMAllocationBytes = g_VRAMRenderTargets.size() * rtSize;
        g_SharedMem.pData->AllocatedRenderTargetCount = static_cast<uint32_t>(g_VRAMRenderTargets.size());
    }
}

void RenderToAllVRAMTargets()
{
    if (g_VRAMRenderTargets.empty()) return;

    // Simple clear operation to each render target to ensure they stay resident
    const float clearColors[][4] = {
        { 0.2f, 0.0f, 0.0f, 1.0f },
        { 0.0f, 0.2f, 0.0f, 1.0f },
        { 0.0f, 0.0f, 0.2f, 1.0f },
        { 0.2f, 0.2f, 0.0f, 1.0f }
    };

    for (size_t i = 0; i < g_VRAMRenderTargets.size(); i++) {
        const float* color = clearColors[i % 4];
        g_CommandList->ClearRenderTargetView(g_VRAMRenderTargets[i].RtvHandle, color, 0, nullptr);
    }
}

void QueryMemoryInfo()
{
    if (g_Adapter && g_SharedMem.pData) {
        DXGI_QUERY_VIDEO_MEMORY_INFO localInfo = {};
        DXGI_QUERY_VIDEO_MEMORY_INFO nonLocalInfo = {};

        g_Adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &localInfo);
        g_Adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocalInfo);

        // Update shared memory with local (VRAM) info
        g_SharedMem.pData->LocalBudget = localInfo.Budget;
        g_SharedMem.pData->LocalCurrentUsage = localInfo.CurrentUsage;
        g_SharedMem.pData->LocalAvailableForReservation = localInfo.AvailableForReservation;
        g_SharedMem.pData->LocalCurrentReservation = localInfo.CurrentReservation;

        // Update shared memory with non-local (system) info
        g_SharedMem.pData->NonLocalBudget = nonLocalInfo.Budget;
        g_SharedMem.pData->NonLocalCurrentUsage = nonLocalInfo.CurrentUsage;
        g_SharedMem.pData->NonLocalAvailableForReservation = nonLocalInfo.AvailableForReservation;
        g_SharedMem.pData->NonLocalCurrentReservation = nonLocalInfo.CurrentReservation;
    }
}

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

using Microsoft::WRL::ComPtr;

void Check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        fprintf(stderr, "%s failed: %08lx\n", operation, static_cast<unsigned long>(result));
        std::exit(1);
    }
}

void Submit(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list, ID3D12Fence* fence, UINT64 value) {
    Check(list->Close(), "Close command list");
    ID3D12CommandList* lists[]{list};
    queue->ExecuteCommandLists(1, lists);
    Check(queue->Signal(fence, value), "Signal queue");
    auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(fence->SetEventOnCompletion(value, event), "Set event on public fence");
    auto wait = WaitForSingleObject(event, 10000);
    CloseHandle(event);
    if (wait != WAIT_OBJECT_0 || fence->GetCompletedValue() == UINT64_MAX) {
        fprintf(stderr, "GPU completion failed at fence %llu: wait=%lu\n", value, wait);
        std::exit(2);
    }
}

int main() {
    wchar_t path[MAX_PATH];
    auto length = GetEnvironmentVariableW(L"JWE3_PROBE_REPORT", path, MAX_PATH);
    if (length && length < MAX_PATH) {
        if (!_wfreopen(path, L"w", stdout) || !_wfreopen(path, L"a", stderr)) {
            return 4;
        }
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    constexpr UINT64 SIZE = 65536;
    ComPtr<ID3D12Device> device;
    Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "Create device");
    ComPtr<ID3D12Device3> device3;
    Check(device.As(&device3), "Get device3");
    auto* title = static_cast<uint32_t*>(VirtualAlloc(nullptr, SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!title) {
        return 1;
    }
    ComPtr<ID3D12Heap> heap;
    Check(device3->OpenExistingHeapFromAddress(title, IID_PPV_ARGS(&heap)), "Open existing heap");
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = SIZE;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> placed, upload, readback;
    Check(device->CreatePlacedResource(heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                       IID_PPV_ARGS(&placed)), "Create placed buffer");
    D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                               D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    Check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                          nullptr, IID_PPV_ARGS(&upload)), "Create upload buffer");
    props.Type = D3D12_HEAP_TYPE_READBACK;
    Check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr, IID_PPV_ARGS(&readback)), "Create readback buffer");

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "Create queue");
    Check(device->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(&allocator)), "Create allocator");
    Check(device->CreateCommandList(0, queueDesc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "Create command list");
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create public fence");

    uint32_t* source = nullptr;
    D3D12_RANGE noRead{};
    Check(upload->Map(0, &noRead, reinterpret_cast<void**>(&source)), "Map upload buffer");
    source[0] = 0x1234abcd;
    D3D12_RANGE written{0, sizeof(*source)};
    upload->Unmap(0, &written);

    title[0] = 0x5a5a1234;
    list->CopyBufferRegion(readback.Get(), 0, placed.Get(), 0, sizeof(uint32_t));
    Submit(queue.Get(), list.Get(), fence.Get(), 1);
    uint32_t* result = nullptr;
    D3D12_RANGE read{0, sizeof(*result)};
    Check(readback->Map(0, &read, reinterpret_cast<void**>(&result)), "Map readback buffer");
    printf("CPU -> GPU -> readback: %08x, expected 5a5a1234\n", result[0]);
    const bool uploaded = result[0] == title[0];
    readback->Unmap(0, &noRead);

    Check(allocator->Reset(), "Reset allocator");
    Check(list->Reset(allocator.Get(), nullptr), "Reset command list");
    list->CopyBufferRegion(placed.Get(), sizeof(uint32_t), upload.Get(), 0, sizeof(uint32_t));
    Submit(queue.Get(), list.Get(), fence.Get(), 2);
    printf("GPU -> CPU at public fence: %08x, expected 1234abcd\n", title[1]);
    const bool downloaded = title[1] == 0x1234abcd;
    placed.Reset();
    heap.Reset();
    VirtualFree(title, 0, MEM_RELEASE);
    printf("%s\n", uploaded && downloaded ? "PASS" : "FAIL");
    return uploaded && downloaded ? 0 : 3;
}

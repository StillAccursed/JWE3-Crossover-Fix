// Heap mirroring adapted from FH2-Port-AppleSilicon (MIT). See LICENSE.fh2.
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

namespace {
constexpr size_t HEAP_ALIGNMENT = 65536;
constexpr size_t MAX_MIRROR_BYTES = 256 * 1024 * 1024;
constexpr size_t VTABLE_CAPACITY = 112;
constexpr size_t QUERY_SLOT = 0;
constexpr size_t RELEASE_SLOT = 2;
constexpr size_t CREATE_QUEUE_SLOT = 8;
constexpr size_t CREATE_PLACED_SLOT = 29;
constexpr size_t OPEN_HEAP_SLOT = 48;
constexpr size_t EXECUTE_SLOT = 10;
constexpr size_t SIGNAL_SLOT = 14;

template<class T> struct ReleaseCom {
    void operator()(T* object) const {
        IUnknown_Release(reinterpret_cast<IUnknown*>(object));
    }
};
template<class T> using ComPtr = std::unique_ptr<T, ReleaseCom<T>>;

struct Table {
    std::array<void*, VTABLE_CAPACITY> _slots{};
    void** _original;
    size_t _count = 0;
};

struct Heap {
    uint8_t* _address;
    size_t _size;
};

struct Mirror {
    uint8_t* _address;
    uint8_t* _mapped = nullptr;
    std::vector<uint8_t> _last;
    std::vector<uint8_t> _snapshot;
};

struct State {
    std::recursive_mutex _lock;
    std::vector<std::unique_ptr<Table>> _tables;
    std::unordered_map<ID3D12Heap*, Heap> _heaps;
    std::unordered_map<ID3D12Resource*, std::unique_ptr<Mirror>> _mirrors;
    size_t _bytes = 0;
};

State& GetState() {
    static auto* state = new State;
    return *state;
}

void Log(const char* message, unsigned long long first = 0, unsigned long long second = 0) {
    static HANDLE file = [] {
        wchar_t path[MAX_PATH];
        const auto length = GetEnvironmentVariableW(L"JWE3_D3D12_LOG", path, MAX_PATH);
        if (!length || length >= MAX_PATH) {
            return INVALID_HANDLE_VALUE;
        }
        return CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }();
    if (file != INVALID_HANDLE_VALUE) {
        char line[256];
        auto length = snprintf(line, sizeof(line), message, first, second);
        DWORD written;
        if (length > 0 && length < static_cast<int>(sizeof(line))) {
            WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
        }
    }
}

bool ShouldHook() {
    static bool allowed = [] {
        wchar_t path[MAX_PATH];
        if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) {
            return false;
        }
        const auto* slash = wcsrchr(path, L'\\');
        const auto* name = slash ? slash + 1 : path;
        wchar_t disabled[2]{};
        GetEnvironmentVariableW(L"JWE3_D3D12_PASSTHROUGH", disabled, 2);
        return disabled[0] != L'1' &&
               (!_wcsicmp(name, L"JWE3.exe") || !_wcsicmp(name, L"JWE3HeapProbe.exe"));
    }();
    return allowed;
}

HMODULE GetBackend() {
    static HMODULE backend = [] {
        wchar_t path[MAX_PATH];
        auto size = GetSystemDirectoryW(path, MAX_PATH);
        if (!size || size >= MAX_PATH - 11) {
            return static_cast<HMODULE>(nullptr);
        }
        wcscpy(path + size, L"\\d3d12.dll");
        auto module = LoadLibraryW(path);
        HMODULE proxy = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&GetBackend), &proxy);
        if (!module || module == proxy) {
            Log("Cannot load system D3D12 backend (%llu)\r\n", GetLastError());
            return static_cast<HMODULE>(nullptr);
        }
        Log("Loaded system D3D12 backend\r\n");
        return module;
    }();
    return backend;
}

template<class Fn> Fn GetExport(const char* name) {
    auto backend = GetBackend();
    return backend ? reinterpret_cast<Fn>(GetProcAddress(backend, name)) : nullptr;
}

template<class Fn, class... Args> HRESULT Forward(const char* name, Args... args) {
    auto fn = GetExport<Fn>(name);
    return fn ? fn(args...) : E_NOTIMPL;
}

template<class Object> void** Slots(Object* object) {
    return *reinterpret_cast<void***>(object);
}

template<class Fn, class Object> Fn Original(Object* object, size_t slot) {
    // The shadow slots are the first member of Table.
    return reinterpret_cast<Fn>(reinterpret_cast<Table*>(Slots(object))->_original[slot]);
}

template<class Object> Table& PrepareTable(State& state, Object* object, size_t count) {
    auto slots = Slots(object);
    for (auto& table : state._tables) {
        if (slots == table->_slots.data() || slots == table->_original) {
            if (count > table->_count) {
                std::copy(table->_original + table->_count, table->_original + count,
                          table->_slots.begin() + table->_count);
                table->_count = count;
            }
            return *table;
        }
    }
    auto table = std::make_unique<Table>();
    table->_original = slots;
    table->_count = count;
    std::copy_n(slots, count, table->_slots.begin());
    auto& result = *table;
    state._tables.push_back(std::move(table));
    return result;
}

template<class Object> void SetTable(Object* object, Table& table) {
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(object), table._slots.data());
}

bool ReadTitle(const void* address, void* destination, size_t size) {
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, destination, size, &read) && read == size;
}

bool WriteTitle(void* address, const void* source, size_t size) {
    SIZE_T written = 0;
    return WriteProcessMemory(GetCurrentProcess(), address, source, size, &written) && written == size;
}

bool Sync(State& state, bool toGpu) {
    for (auto& [resource, mirror] : state._mirrors) {
        const auto size = mirror->_last.size();
        auto* title = mirror->_snapshot.data();
        auto* last = mirror->_last.data();
        if (!ReadTitle(mirror->_address, title, size)) {
            return false;
        }

        if (!toGpu) {
            D3D12_RANGE none{0, 0};
            ID3D12Resource_Unmap(resource, 0, &none);
            mirror->_mapped = nullptr;
            if (FAILED(ID3D12Resource_Map(resource, 0, nullptr,
                                         reinterpret_cast<void**>(&mirror->_mapped)))) {
                return false;
            }
        }

        auto* gpu = mirror->_mapped;
        if (!gpu) {
            return false;
        }

        // Preserve CPU edits made after submission, even within a changed block.
        for (size_t offset = 0; offset < size; offset += 64) {
            const size_t count = std::min<size_t>(64, size - offset);
            auto* source = (toGpu ? title : gpu) + offset;
            if (!memcmp(source, last + offset, count)) {
                continue;
            }
            if (toGpu) {
                memcpy(gpu + offset, source, count);
                memcpy(last + offset, source, count);
                continue;
            }
            bool changed = false;
            for (size_t i = offset; i < offset + count; ++i) {
                if (title[i] == last[i] && gpu[i] != last[i]) {
                    title[i] = gpu[i];
                    last[i] = gpu[i];
                    changed = true;
                }
            }
            if (changed && !WriteTitle(mirror->_address + offset, title + offset, count)) {
                return false;
            }
        }
    }
    return true;
}

using HeapRelease = decltype(ID3D12HeapVtbl::Release);
ULONG STDMETHODCALLTYPE ReleaseHeap(ID3D12Heap* heap) {
    auto& state = GetState();
    std::lock_guard lock(state._lock);
    auto remaining = Original<HeapRelease>(heap, RELEASE_SLOT)(heap);
    if (!remaining) {
        state._heaps.erase(heap);
    }
    return remaining;
}

using ResourceRelease = decltype(ID3D12ResourceVtbl::Release);
ULONG STDMETHODCALLTYPE ReleaseResource(ID3D12Resource* resource) {
    auto& state = GetState();
    std::lock_guard lock(state._lock);
    const auto release = Original<ResourceRelease>(resource, RELEASE_SLOT);
    auto remaining = release(resource);
    auto found = state._mirrors.find(resource);
    if (remaining == 1 && found != state._mirrors.end()) {
        if (found->second->_mapped) {
            D3D12_RANGE none{0, 0};
            ID3D12Resource_Unmap(resource, 0, &none);
        }
        state._bytes -= found->second->_last.size();
        state._mirrors.erase(found);
        remaining = release(resource);
    }
    return remaining;
}

HRESULT STDMETHODCALLTYPE OpenHeap(ID3D12Device3* device, const void* address, REFIID iid, void** result) {
    using Open = decltype(ID3D12Device3Vtbl::OpenExistingHeapFromAddress);
    auto hr = Original<Open>(device, OPEN_HEAP_SLOT)(device, address, iid, result);
    if (hr != E_NOTIMPL || !result || !IsEqualIID(iid, IID_ID3D12Heap)) {
        return hr;
    }
    *result = nullptr;

    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(address, &memory, sizeof(memory)) ||
        memory.AllocationBase != address || memory.State != MEM_COMMIT) {
        return E_INVALIDARG;
    }
    auto* base = static_cast<const uint8_t*>(address);
    size_t size = 0;
    while (VirtualQuery(base + size, &memory, sizeof(memory)) &&
           memory.AllocationBase == address && memory.State == MEM_COMMIT &&
           !(memory.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        size += memory.RegionSize;
    }
    if (!size || size > MAX_MIRROR_BYTES) {
        Log("Unsupported heap size %#llx\r\n", size);
        return E_INVALIDARG;
    }

    D3D12_HEAP_DESC desc{};
    desc.SizeInBytes = (size + HEAP_ALIGNMENT - 1) & ~(HEAP_ALIGNMENT - 1);
    desc.Alignment = HEAP_ALIGNMENT;
    desc.Properties = {D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0, 1, 1};
    ID3D12Heap* raw = nullptr;
    hr = ID3D12Device3_CreateHeap(device, &desc, iid, reinterpret_cast<void**>(&raw));
    if (FAILED(hr)) {
        Log("CreateHeap failed %#llx (size %#llx)\r\n", static_cast<uint32_t>(hr), size);
        return hr;
    }

    ComPtr<ID3D12Heap> heap(raw);
    auto& state = GetState();
    std::lock_guard lock(state._lock);
    auto& table = PrepareTable(state, raw, sizeof(ID3D12Heap1Vtbl) / sizeof(void*));
    table._slots[RELEASE_SLOT] = reinterpret_cast<void*>(&ReleaseHeap);
    state._heaps.emplace(raw, Heap{const_cast<uint8_t*>(base), size});
    SetTable(raw, table);
    *result = heap.release();
    Log("Opened mirrored heap size %#llx\r\n", size);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CreatePlaced(ID3D12Device* device, ID3D12Heap* heap, UINT64 offset,
                                       const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initial,
                                       const D3D12_CLEAR_VALUE* clear, REFIID iid, void** result) {
    using Create = decltype(ID3D12DeviceVtbl::CreatePlacedResource);
    auto create = Original<Create>(device, CREATE_PLACED_SLOT);
    auto& state = GetState();
    std::lock_guard lock(state._lock);
    const auto region = state._heaps.find(heap);
    if (region == state._heaps.end() || !result) {
        return create(device, heap, offset, desc, initial, clear, iid, result);
    }
    *result = nullptr;
    const auto& [address, size] = region->second;
    if (!desc || desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        !IsEqualIID(iid, IID_ID3D12Resource) || offset > size || !desc->Width ||
        desc->Width > size - offset || desc->Width > MAX_MIRROR_BYTES - state._bytes) {
        Log("Unsupported placed resource at %#llx\r\n", offset);
        return E_NOTIMPL;
    }

    auto mirror = std::make_unique<Mirror>();
    mirror->_address = address + offset;
    mirror->_last.resize(static_cast<size_t>(desc->Width));
    mirror->_snapshot.resize(mirror->_last.size());
    if (!ReadTitle(mirror->_address, mirror->_last.data(), mirror->_last.size())) {
        return E_INVALIDARG;
    }
    ID3D12Resource* raw = nullptr;
    auto hr = create(device, heap, offset, desc, initial, clear, iid, reinterpret_cast<void**>(&raw));
    if (FAILED(hr)) {
        Log("CreatePlacedResource failed %#llx\r\n", static_cast<uint32_t>(hr));
        return hr;
    }
    ComPtr<ID3D12Resource> resource(raw);
    hr = ID3D12Resource_Map(raw, 0, nullptr, reinterpret_cast<void**>(&mirror->_mapped));
    if (FAILED(hr) || !mirror->_mapped) {
        Log("Map placed resource failed %#llx\r\n", static_cast<uint32_t>(hr));
        return FAILED(hr) ? hr : E_FAIL;
    }
    memcpy(mirror->_mapped, mirror->_last.data(), mirror->_last.size());

    auto& table = PrepareTable(state, raw, sizeof(ID3D12Resource2Vtbl) / sizeof(void*));
    table._slots[RELEASE_SLOT] = reinterpret_cast<void*>(&ReleaseResource);
    state._mirrors.emplace(raw, std::move(mirror));
    ID3D12Resource_AddRef(raw);
    state._bytes += static_cast<size_t>(desc->Width);
    SetTable(raw, table);
    *result = resource.release();
    Log("Mapped placed buffer offset %#llx size %#llx\r\n", offset, desc->Width);
    return S_OK;
}

struct SyncJob {
    ComPtr<ID3D12Fence> _gpuDone;
    ComPtr<ID3D12Fence> _cpuDone;
};

void CALLBACK CopyBack(PTP_CALLBACK_INSTANCE, void* data) {
    std::unique_ptr<SyncJob> job(static_cast<SyncJob*>(data));
    auto hr = ID3D12Fence_SetEventOnCompletion(job->_gpuDone.get(), 1, nullptr);
    if (SUCCEEDED(hr) && ID3D12Fence_GetCompletedValue(job->_gpuDone.get()) != UINT64_MAX) {
        auto& state = GetState();
        {
            std::lock_guard lock(state._lock);
            if (!Sync(state, false)) {
                Log("GPU to CPU synchronization failed\r\n");
                return;
            }
        }
        ID3D12Fence_Signal(job->_cpuDone.get(), 1);
    }
}

void STDMETHODCALLTYPE Execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
    auto& state = GetState();
    std::lock_guard lock(state._lock);
    if (!Sync(state, true)) {
        Log("CPU to GPU synchronization failed\r\n");
        return;
    }
    using ExecuteLists = decltype(ID3D12CommandQueueVtbl::ExecuteCommandLists);
    Original<ExecuteLists>(queue, EXECUTE_SLOT)(queue, count, lists);
}

HRESULT STDMETHODCALLTYPE Signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    auto& state = GetState();
    std::lock_guard lock(state._lock);
    using SignalFence = decltype(ID3D12CommandQueueVtbl::Signal);
    auto signal = Original<SignalFence>(queue, SIGNAL_SLOT);
    if (state._mirrors.empty()) {
        return signal(queue, fence, value);
    }

    ID3D12Device* raw = nullptr;
    auto hr = ID3D12CommandQueue_GetDevice(queue, IID_ID3D12Device, reinterpret_cast<void**>(&raw));
    if (FAILED(hr)) {
        return hr;
    }
    ComPtr<ID3D12Device> device(raw);
    auto job = std::make_unique<SyncJob>();
    for (auto* destination : {&job->_gpuDone, &job->_cpuDone}) {
        ID3D12Fence* created = nullptr;
        hr = ID3D12Device_CreateFence(device.get(), 0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence,
                                    reinterpret_cast<void**>(&created));
        if (FAILED(hr)) {
            return hr;
        }
        destination->reset(created);
    }
    auto* gpuDone = job->_gpuDone.get();
    auto* cpuDone = job->_cpuDone.get();
    ID3D12Fence_AddRef(gpuDone);
    ID3D12Fence_AddRef(cpuDone);
    ComPtr<ID3D12Fence> keepGpuDone(gpuDone), keepCpuDone(cpuDone);
    auto* pending = job.release();
    if (!TrySubmitThreadpoolCallback(CopyBack, pending, nullptr)) {
        const auto error = GetLastError();
        delete pending;
        return HRESULT_FROM_WIN32(error);
    }
    hr = signal(queue, gpuDone, 1);
    if (FAILED(hr)) {
        ID3D12Fence_Signal(gpuDone, 1);
        return hr;
    }
    hr = ID3D12CommandQueue_Wait(queue, cpuDone, 1);
    if (FAILED(hr)) {
        return hr;
    }
    return signal(queue, fence, value);
}

HRESULT STDMETHODCALLTYPE CreateQueue(ID3D12Device* device, const D3D12_COMMAND_QUEUE_DESC* desc,
                                       REFIID iid, void** result) {
    using Create = decltype(ID3D12DeviceVtbl::CreateCommandQueue);
    auto hr = Original<Create>(device, CREATE_QUEUE_SLOT)(device, desc, iid, result);
    if (SUCCEEDED(hr) && result && *result && IsEqualIID(iid, IID_ID3D12CommandQueue)) {
        auto& state = GetState();
        std::lock_guard lock(state._lock);
        auto* queue = static_cast<ID3D12CommandQueue*>(*result);
        auto& table = PrepareTable(state, queue, sizeof(ID3D12CommandQueueVtbl) / sizeof(void*));
        table._slots[EXECUTE_SLOT] = reinterpret_cast<void*>(&Execute);
        table._slots[SIGNAL_SLOT] = reinterpret_cast<void*>(&Signal);
        SetTable(queue, table);
    }
    return hr;
}

struct DeviceInterface { const IID* _iid; size_t _slots; };

#define DEVICE_INTERFACE(Suffix) {&IID_ID3D12Device##Suffix, sizeof(ID3D12Device##Suffix##Vtbl) / sizeof(void*)}
const DeviceInterface DEVICE_INTERFACES[] = {
    DEVICE_INTERFACE(10), DEVICE_INTERFACE(9), DEVICE_INTERFACE(8), DEVICE_INTERFACE(7),
    DEVICE_INTERFACE(6), DEVICE_INTERFACE(5), DEVICE_INTERFACE(4), DEVICE_INTERFACE(3),
    DEVICE_INTERFACE(2), DEVICE_INTERFACE(1), DEVICE_INTERFACE()
};
#undef DEVICE_INTERFACE

void HookDevice(IUnknown* device, REFIID iid);

HRESULT STDMETHODCALLTYPE QueryDevice(IUnknown* device, REFIID iid, void** result) {
    auto query = Original<decltype(IUnknownVtbl::QueryInterface)>(device, QUERY_SLOT);
    auto hr = query(device, iid, result);
    if (SUCCEEDED(hr) && result && *result) {
        HookDevice(static_cast<IUnknown*>(*result), iid);
    }
    return hr;
}

void HookDevice(IUnknown* device, REFIID iid) {
    auto version = std::find_if(std::begin(DEVICE_INTERFACES), std::end(DEVICE_INTERFACES),
                                [&](const auto& candidate) { return IsEqualIID(*candidate._iid, iid); });
    if (version == std::end(DEVICE_INTERFACES)) {
        return;
    }

    auto& state = GetState();
    std::lock_guard lock(state._lock);
    for (auto& table : state._tables) {
        if (Slots(device) == table->_slots.data()) {
            return;
        }
    }

    auto count = version->_slots;
    for (auto candidate = std::begin(DEVICE_INTERFACES); candidate != version; ++candidate) {
        IUnknown* higher = nullptr;
        if (SUCCEEDED(IUnknown_QueryInterface(device, *candidate->_iid,
                                              reinterpret_cast<void**>(&higher)))) {
            const bool same = higher == device;
            IUnknown_Release(higher);
            if (same) {
                count = candidate->_slots;
                break;
            }
        }
    }

    auto& table = PrepareTable(state, device, count);
    table._slots[QUERY_SLOT] = reinterpret_cast<void*>(&QueryDevice);
    table._slots[CREATE_QUEUE_SLOT] = reinterpret_cast<void*>(&CreateQueue);
    table._slots[CREATE_PLACED_SLOT] = reinterpret_cast<void*>(&CreatePlaced);
    if (count > OPEN_HEAP_SLOT) {
        table._slots[OPEN_HEAP_SLOT] = reinterpret_cast<void*>(&OpenHeap);
    }
    SetTable(device, table);
    Log("Hooked device (%llu vtable entries)\r\n", count);
}
}

extern "C" HRESULT WINAPI ProxyCreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL level,
                                             REFIID iid, void** result) {
    auto hr = Forward<decltype(&D3D12CreateDevice)>("D3D12CreateDevice", adapter, level, iid, result);
    if (SUCCEEDED(hr) && result && *result && ShouldHook()) {
        HookDevice(static_cast<IUnknown*>(*result), iid);
    }
    return hr;
}

extern "C" HRESULT WINAPI ProxyGetInterface(REFCLSID clsid, REFIID iid, void** result) {
    return Forward<decltype(&D3D12GetInterface)>("D3D12GetInterface", clsid, iid, result);
}

extern "C" HRESULT WINAPI ProxyDebug(REFIID iid, void** result) {
    return Forward<decltype(&D3D12GetDebugInterface)>("D3D12GetDebugInterface", iid, result);
}

extern "C" HRESULT WINAPI ProxySerialize(const D3D12_ROOT_SIGNATURE_DESC* desc, D3D_ROOT_SIGNATURE_VERSION version,
                                          ID3DBlob** blob, ID3DBlob** error) {
    return Forward<decltype(&D3D12SerializeRootSignature)>("D3D12SerializeRootSignature", desc, version, blob, error);
}

extern "C" HRESULT WINAPI ProxySerializeVersioned(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc,
                                                   ID3DBlob** blob, ID3DBlob** error) {
    return Forward<decltype(&D3D12SerializeVersionedRootSignature)>("D3D12SerializeVersionedRootSignature",
                                                                   desc, blob, error);
}

extern "C" HRESULT WINAPI ProxyDeserialize(LPCVOID data, SIZE_T size, REFIID iid, void** result) {
    return Forward<decltype(&D3D12CreateRootSignatureDeserializer)>("D3D12CreateRootSignatureDeserializer",
                                                                   data, size, iid, result);
}

extern "C" HRESULT WINAPI ProxyDeserializeVersioned(LPCVOID data, SIZE_T size, REFIID iid, void** result) {
    return Forward<decltype(&D3D12CreateVersionedRootSignatureDeserializer)>(
        "D3D12CreateVersionedRootSignatureDeserializer", data, size, iid, result);
}

extern "C" HRESULT WINAPI ProxyExperimental(UINT count, const IID* iids, void* config, UINT* sizes) {
    return Forward<decltype(&D3D12EnableExperimentalFeatures)>("D3D12EnableExperimentalFeatures",
                                                              count, iids, config, sizes);
}

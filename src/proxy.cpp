// Heap mirroring adapted from FH2-Port-AppleSilicon (MIT). See LICENSE.fh2.
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>
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

enum class FenceStage {
    Idle,
    GetDevice,
    CreateGpuFence,
    CreateCpuFence,
    SubmitCallback,
    QueueGpuSignal,
    QueueCpuWait,
    QueueGameSignal,
    QueueDone,
    WaitForGpu,
    ReadGpuFence,
    AcquireCopyLock,
    CopyToGame,
    SignalCpuFence,
    CopyDone
};

#ifdef JWE3_FENCE_TRACE
class FenceTrace {
    static constexpr size_t SLOT_COUNT = 128;
    static constexpr size_t REPORT_COUNT = 3;
    static constexpr size_t REPORT_LIMIT = 64 * 1024;
    static constexpr DWORD POLL_MS = 1000;
    static constexpr ULONGLONG STALL_MS = 2000;

    struct Slot {
        std::atomic<unsigned> _owners{0};
        std::atomic<uint64_t> _queueStage{0};
        std::atomic<uint64_t> _copyStage{0};
        uint64_t _id = 0;
        uintptr_t _queue = 0;
        uintptr_t _fence = 0;
        uint64_t _fenceValue = 0;
        uint64_t _observedQueue = 0;
        uint64_t _observedCopy = 0;
        ULONGLONG _lastProgress = 0;
        bool _reportedError = false;
    };

    struct Entry {
        uint64_t _id;
        uintptr_t _queue;
        uintptr_t _fence;
        uint64_t _fenceValue;
        uint64_t _queueStage;
        uint64_t _copyStage;
        ULONGLONG _idleMs;
        unsigned _owners;
    };

    struct Report {
        std::array<Entry, SLOT_COUNT> _entries{};
        size_t _count = 0;
        uint64_t _dropped = 0;
        bool _stall = false;
        bool _error = false;
    };

    SRWLOCK _registryLock = SRWLOCK_INIT;
    std::array<Slot, SLOT_COUNT> _slots{};
    size_t _nextSlot = 0;
    uint64_t _nextId = 0;
    std::atomic<uint64_t> _dropped{0};
    bool _reportedStall = false;

public:
    class Ticket {
        friend class FenceTrace;
        Slot* _slot = nullptr;
        bool _copy = false;

        Ticket(Slot* slot, bool copy) : _slot(slot), _copy(copy) {}

    public:
        Ticket() = default;
        Ticket(const Ticket&) = delete;
        Ticket& operator=(const Ticket&) = delete;
        Ticket(Ticket&& other) noexcept : _slot(std::exchange(other._slot, nullptr)),
                                          _copy(other._copy) {}

        ~Ticket() {
            if (_slot) {
                _slot->_owners.fetch_sub(1, std::memory_order_acq_rel);
            }
        }

        // The queue and callback update separate lanes even if their calls overlap.
        void Record(FenceStage stage, HRESULT result = S_OK) const {
            if (_slot) {
                auto& lane = _copy ? _slot->_copyStage : _slot->_queueStage;
                const auto packed = (static_cast<uint64_t>(stage) << 32) |
                                    static_cast<uint32_t>(result);
                lane.store(packed, std::memory_order_relaxed);
            }
        }

        Ticket Fork() const {
            if (_slot) {
                _slot->_owners.fetch_add(1, std::memory_order_relaxed);
            }
            return Ticket(_slot, true);
        }
    };

    static Ticket Begin(ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value) {
        static FenceTrace trace;
        static const bool started = trace.Start();
        return started ? trace.Register(queue, fence, value) : Ticket{};
    }

private:
    static bool IsError(uint64_t packed) {
        return (packed & 0x80000000u) != 0;
    }

    bool Start() {
        // Keep the module loaded while its process-lifetime monitor is running.
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(&Monitor), &module)) {
            return false;
        }
        const auto thread = CreateThread(nullptr, 0, Monitor, this, 0, nullptr);
        if (!thread) {
            return false;
        }
        SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(thread);
        return true;
    }

    Ticket Register(ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value) {
        if (!TryAcquireSRWLockExclusive(&_registryLock)) {
            _dropped.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        for (size_t i = 0; i < SLOT_COUNT; ++i) {
            const auto index = (_nextSlot + i) % SLOT_COUNT;
            auto& slot = _slots[index];
            if (slot._owners.load(std::memory_order_acquire) ||
                (!slot._reportedError && (IsError(slot._queueStage.load(std::memory_order_relaxed)) ||
                                          IsError(slot._copyStage.load(std::memory_order_relaxed))))) {
                continue;
            }
            slot._id = ++_nextId;
            slot._queue = reinterpret_cast<uintptr_t>(queue);
            slot._fence = reinterpret_cast<uintptr_t>(fence);
            slot._fenceValue = value;
            slot._observedQueue = 0;
            slot._observedCopy = 0;
            slot._lastProgress = 0;
            slot._reportedError = false;
            slot._queueStage.store(0, std::memory_order_relaxed);
            slot._copyStage.store(0, std::memory_order_relaxed);
            slot._owners.store(1, std::memory_order_relaxed);
            _nextSlot = (index + 1) % SLOT_COUNT;
            ReleaseSRWLockExclusive(&_registryLock);
            return Ticket(&slot, false);
        }
        _dropped.fetch_add(1, std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&_registryLock);
        return {};
    }

    // Only the monitor inspects progress. The lock protects slot reuse during a scan.
    bool Scan(ULONGLONG now, Report& report) {
        if (!TryAcquireSRWLockExclusive(&_registryLock)) {
            return false;
        }
        bool stalled = false;
        bool failed = false;
        report._dropped = _dropped.load(std::memory_order_relaxed);
        for (auto& slot : _slots) {
            const auto owners = slot._owners.load(std::memory_order_acquire);
            const auto queue = slot._queueStage.load(std::memory_order_relaxed);
            const auto copy = slot._copyStage.load(std::memory_order_relaxed);
            const bool newError = !slot._reportedError && (IsError(queue) || IsError(copy));
            if (!owners && !newError) {
                continue;
            }
            if (!slot._lastProgress || queue != slot._observedQueue || copy != slot._observedCopy) {
                slot._lastProgress = now;
                slot._observedQueue = queue;
                slot._observedCopy = copy;
            }
            const auto idle = now - slot._lastProgress;
            stalled |= owners && idle >= STALL_MS;
            failed |= newError;
            slot._reportedError |= newError;
            report._entries[report._count++] = {
                slot._id, slot._queue, slot._fence, slot._fenceValue, queue, copy, idle, owners
            };
        }
        ReleaseSRWLockExclusive(&_registryLock);
        report._stall = stalled && !_reportedStall;
        report._error = failed;
        _reportedStall = stalled;
        return report._stall || report._error;
    }

    static const char* StageName(uint64_t packed) {
        static constexpr const char* names[] = {
            "idle", "get-device", "create-gpu-fence", "create-cpu-fence", "submit-callback",
            "queue-gpu-signal", "queue-cpu-wait", "queue-game-signal", "queue-done",
            "wait-for-gpu", "read-gpu-fence", "acquire-copy-lock", "copy-to-game",
            "signal-cpu-fence", "copy-done"
        };
        return names[packed >> 32];
    }

    static bool WriteReport(const Report& report) {
        wchar_t folder[MAX_PATH]{};
        const auto length = GetTempPathW(MAX_PATH, folder);
        constexpr wchar_t folderName[] = L"JWE3-FenceTrace";
        if (!length || length >= MAX_PATH - std::size(folderName) - 20) {
            return false;
        }
        wcscpy(folder + length, folderName);
        if (!CreateDirectoryW(folder, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            return false;
        }

        // Serialize rotation across processes in the same bottle.
        const auto mutex = CreateMutexW(nullptr, FALSE, L"Local\\JWE3-FenceTrace-Report");
        if (!mutex) {
            return false;
        }
        const auto wait = WaitForSingleObject(mutex, 1000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            CloseHandle(mutex);
            return false;
        }
        const bool written = WriteRotatingReport(report, folder);
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return written;
    }

    static bool WriteRotatingReport(const Report& report, const wchar_t* folder) {
        std::array<char, REPORT_LIMIT> buffer{};
        SYSTEMTIME utc{};
        GetSystemTime(&utc);
        const auto header = snprintf(buffer.data(), buffer.size(),
            "JWE3 fence trace v1 UTC=%04u-%02u-%02uT%02u:%02u:%02u.%03uZ pid=%lu "
            "stall=%u error=%u dropped_jobs=%llu\r\n",
            utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds,
            GetCurrentProcessId(), static_cast<unsigned>(report._stall),
            static_cast<unsigned>(report._error), static_cast<unsigned long long>(report._dropped));
        if (header <= 0 || static_cast<size_t>(header) >= buffer.size()) {
            return false;
        }
        size_t size = static_cast<size_t>(header);
        for (size_t i = 0; i < report._count; ++i) {
            const auto& entry = report._entries[i];
            const auto remaining = buffer.size() - size;
            const auto count = snprintf(buffer.data() + size, remaining,
                "job=%llu queue=%#llx game_fence=%#llx game_fence_value=%llu owners=%u idle_ms=%llu "
                "queue_stage=%s queue_hr=0x%08x copy_stage=%s copy_hr=0x%08x\r\n",
                static_cast<unsigned long long>(entry._id),
                static_cast<unsigned long long>(entry._queue),
                static_cast<unsigned long long>(entry._fence),
                static_cast<unsigned long long>(entry._fenceValue), entry._owners,
                static_cast<unsigned long long>(entry._idleMs),
                StageName(entry._queueStage), static_cast<unsigned>(entry._queueStage),
                StageName(entry._copyStage), static_cast<unsigned>(entry._copyStage));
            if (count <= 0 || static_cast<size_t>(count) >= remaining) {
                break;
            }
            size += static_cast<size_t>(count);
        }

        wchar_t target[MAX_PATH]{};
        FILETIME oldest{};
        for (size_t i = 0; i < REPORT_COUNT; ++i) {
            wchar_t candidate[MAX_PATH]{};
            swprintf(candidate, MAX_PATH, L"%ls\\fences-%u.log", folder,
                     static_cast<unsigned>(i + 1));
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (!GetFileAttributesExW(candidate, GetFileExInfoStandard, &attributes)) {
                if (GetLastError() == ERROR_FILE_NOT_FOUND) {
                    wcscpy(target, candidate);
                    break;
                }
                continue;
            }
            if (!*target || CompareFileTime(&attributes.ftLastWriteTime, &oldest) < 0) {
                oldest = attributes.ftLastWriteTime;
                wcscpy(target, candidate);
            }
        }
        if (!*target) {
            return false;
        }
        const auto file = CreateFileW(target, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }
        DWORD written = 0;
        const auto bytes = static_cast<DWORD>(size);
        const bool success = WriteFile(file, buffer.data(), bytes, &written, nullptr) &&
                             written == bytes && FlushFileBuffers(file);
        CloseHandle(file);
        return success;
    }

    static DWORD WINAPI Monitor(void* context) {
        auto& trace = *static_cast<FenceTrace*>(context);
        while (true) {
            Sleep(POLL_MS);
            Report report;
            if (trace.Scan(GetTickCount64(), report)) {
                WriteReport(report);
            }
        }
    }
};
#else
class FenceTrace {
public:
    struct Ticket {
        void Record(FenceStage, HRESULT = S_OK) const {}
        Ticket Fork() const { return {}; }
    };

    static Ticket Begin(ID3D12CommandQueue*, ID3D12Fence*, uint64_t) { return {}; }
};
#endif

struct SyncJob {
    ComPtr<ID3D12Fence> _gpuDone;
    ComPtr<ID3D12Fence> _cpuDone;
    FenceTrace::Ticket _trace;

    explicit SyncJob(FenceTrace::Ticket trace) : _trace(std::move(trace)) {}
};

void CALLBACK CopyBack(PTP_CALLBACK_INSTANCE, void* data) {
    std::unique_ptr<SyncJob> job(static_cast<SyncJob*>(data));
    job->_trace.Record(FenceStage::WaitForGpu);
    const auto hr = ID3D12Fence_SetEventOnCompletion(job->_gpuDone.get(), 1, nullptr);
    if (FAILED(hr)) {
        job->_trace.Record(FenceStage::WaitForGpu, hr);
        return;
    }
    job->_trace.Record(FenceStage::ReadGpuFence);
    if (ID3D12Fence_GetCompletedValue(job->_gpuDone.get()) == UINT64_MAX) {
        job->_trace.Record(FenceStage::ReadGpuFence, DXGI_ERROR_DEVICE_REMOVED);
        return;
    }
    auto& state = GetState();
    job->_trace.Record(FenceStage::AcquireCopyLock);
    {
        std::lock_guard lock(state._lock);
        job->_trace.Record(FenceStage::CopyToGame);
        if (!Sync(state, false)) {
            job->_trace.Record(FenceStage::CopyToGame, E_FAIL);
            Log("GPU to CPU synchronization failed\r\n");
            return;
        }
    }
    job->_trace.Record(FenceStage::SignalCpuFence);
    const auto signaled = ID3D12Fence_Signal(job->_cpuDone.get(), 1);
    job->_trace.Record(FAILED(signaled) ? FenceStage::SignalCpuFence : FenceStage::CopyDone,
                       signaled);
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

    const auto trace = FenceTrace::Begin(queue, fence, value);
    trace.Record(FenceStage::GetDevice);
    ID3D12Device* raw = nullptr;
    auto hr = ID3D12CommandQueue_GetDevice(queue, IID_ID3D12Device, reinterpret_cast<void**>(&raw));
    if (FAILED(hr)) {
        trace.Record(FenceStage::GetDevice, hr);
        return hr;
    }
    ComPtr<ID3D12Device> device(raw);
    auto job = std::make_unique<SyncJob>(trace.Fork());
    for (auto* destination : {&job->_gpuDone, &job->_cpuDone}) {
        const auto stage = destination == &job->_gpuDone ? FenceStage::CreateGpuFence :
                                                          FenceStage::CreateCpuFence;
        trace.Record(stage);
        ID3D12Fence* created = nullptr;
        hr = ID3D12Device_CreateFence(device.get(), 0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence,
                                    reinterpret_cast<void**>(&created));
        if (FAILED(hr)) {
            trace.Record(stage, hr);
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
    trace.Record(FenceStage::SubmitCallback);
    if (!TrySubmitThreadpoolCallback(CopyBack, pending, nullptr)) {
        const auto error = GetLastError();
        delete pending;
        hr = HRESULT_FROM_WIN32(error);
        trace.Record(FenceStage::SubmitCallback, hr);
        return hr;
    }
    trace.Record(FenceStage::QueueGpuSignal);
    hr = signal(queue, gpuDone, 1);
    if (FAILED(hr)) {
        trace.Record(FenceStage::QueueGpuSignal, hr);
        ID3D12Fence_Signal(gpuDone, 1);
        return hr;
    }
    trace.Record(FenceStage::QueueCpuWait);
    hr = ID3D12CommandQueue_Wait(queue, cpuDone, 1);
    if (FAILED(hr)) {
        trace.Record(FenceStage::QueueCpuWait, hr);
        return hr;
    }
    trace.Record(FenceStage::QueueGameSignal);
    hr = signal(queue, fence, value);
    trace.Record(FAILED(hr) ? FenceStage::QueueGameSignal : FenceStage::QueueDone, hr);
    return hr;
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

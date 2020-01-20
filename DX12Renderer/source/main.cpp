#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h> // For CommandLineToArgvW

// The min/max macros conflict with like-named member functions.
// Only use std::min and std::max defined in <algorithm>.
#if defined(min)
#undef min
#endif

#if defined(max)
#undef max
#endif

// In order to define a function called CreateWindow, the Windows macro needs to
// be undefined.
#if defined(CreateWindow)
#undef CreateWindow
#endif

// Windows Runtime Library. Needed for Microsoft::WRL::ComPtr<> template class.
#include <wrl.h>
using namespace Microsoft::WRL;

// DirectX 12 specific headers.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

// D3D12 extension library.
#include <d3dx12.h>

// STL Headers
#include <algorithm>
#include <cassert>
#include <chrono>

// Helper functions
#include <Helpers.h>

// The number of swap chain back buffers.
constexpr uint8_t g_NumFrames = 2;
// Use WARP adapter
bool g_UseWarp = false;

uint32_t g_ClientWidth = 1280;
uint32_t g_ClientHeight = 720;

// Set to true once the DX12 objects have been initialized.
bool g_IsInitialized = false;

// Window handle.
HWND g_hWnd;
// Window rectangle (used to toggle fullscreen state).
RECT g_WindowRect;


//-----------------------------------------------------------------------------
// DirectX 12 Objects
/*
Device is created from (on top of) an adapter. It represents a GPU and tracks
allocations of GPU memory. It is used to create command lists, queues, heapes,
fences, textures, buffers... It is not directly used for issuing draw or
dispatch commands. Destroying a divice makes all the resources and memory
allocated from it invalid.
*/
ComPtr<ID3D12Device2> g_Device;

/*
Queue on the GPU where command lists will be submitted.
*/
ComPtr<ID3D12CommandQueue> g_CommandQueue;
ComPtr<IDXGISwapChain4> g_SwapChain;

/*
Backbuffers are Textures but in DX12 everything is a Resource, not like in
Vulkan where we have either vkImage or vkBuffer.
*/
ComPtr<ID3D12Resource> g_BackBuffers[g_NumFrames];

/*
List of commands to be submitted to a command queue to be executed on the GPU.
*/
ComPtr<ID3D12GraphicsCommandList> g_CommandList;

/*
Backing memory from which the actual commands are allocated.
Cannot be reused until the commands allocated are done executing.
Therefore, we need to have at least one allocator per frame in flight
(per backbuffer in our case).
*/
ComPtr<ID3D12CommandAllocator> g_CommandAllocators[g_NumFrames];

/*
In DX12 descriptor = view. From this heap descriptors (views) will be
allocated. Descriptor heap will for example store RTVs. DHeap = array
of descriptors which are now now created one at a time as before. This
heap is used to store RTVs for SwapChain's back buffers.
*/
ComPtr<ID3D12DescriptorHeap> g_RTVDescriptorHeap;

/*
The size of a descriptor on a DHeap is vendor specific. Thus, we save the RTV
descriptor size in this variable so we know how are the RTV descriptors offset
on the DHeap.
*/
UINT g_RTVDescriptorSize;

/*
Depending on the flip model the indices of the current bbufer may not be
sequential. This is the current bbufer index.
*/
UINT g_CurrentBackBufferIndex;

//-----------------------------------------------------------------------------
// Synchronization objects
/*
Fences are used to synchronize submitting of command lists and end of execution
of the command lists. We should have a Fence per command queue. Fence stores
a single value (64 bit int) indicating state. This value may only increase.
*/
ComPtr<ID3D12Fence> g_Fence;
/*
This is used to signal the fence next.
*/
uint64_t g_FenceValue = 0;

/*
Here we store values we used to signal the command queue when submitting the
respective frames.
*/
uint64_t g_FrameFenceValues[g_NumFrames] = {};

/*
Handle to the OS event which will notify us that a fence has been signaled.
*/
HANDLE g_FenceEvent;

// By default, enable V-Sync.
// Can be toggled with the V key.
bool g_VSync = true;
bool g_TearingSupported = false;
// By default, use windowed mode.
// Can be toggled with the Alt+Enter or F11
bool g_Fullscreen = false;

//-----------------------------------------------------------------------------
void ParseCommandLineArguments() {
    int argc;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);

    for (size_t i = 0; i < argc; ++i) {
        if (::wcscmp(argv[i], L"-w") == 0 || ::wcscmp(argv[i], L"--width") == 0) {
            g_ClientWidth = ::wcstol(argv[++i], nullptr, 10);
        }
        if (::wcscmp(argv[i], L"-h") == 0 || ::wcscmp(argv[i], L"--height") == 0) {
            g_ClientHeight = ::wcstol(argv[++i], nullptr, 10);
        }
        if (::wcscmp(argv[i], L"-warp") == 0 || ::wcscmp(argv[i], L"--warp") == 0) {
            g_UseWarp = true;
        }
    }

    // Free memory allocated by CommandLineToArgvW
    ::LocalFree(argv);
}

//-----------------------------------------------------------------------------
void EnableDebugLayer() {
    #if defined(_DEBUG)
        // Always enable the debug layer before doing anything DX12 related
        // so all possible errors generated while creating DX12 objects
        // are caught by the debug layer.
        ComPtr<ID3D12Debug> debugInterface;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface)));
        debugInterface->EnableDebugLayer();
    #endif
}

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

//-----------------------------------------------------------------------------
void RegisterWindowClass(HINSTANCE hInst, const wchar_t* windowClassName) {
    // Register a window class for creating our render window with.
    WNDCLASSEXW windowClass = {};

    windowClass.cbSize = sizeof(WNDCLASSEX);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &WndProc;
    windowClass.cbClsExtra = 0;
    windowClass.cbWndExtra = 0;
    windowClass.hInstance = hInst;
    windowClass.hIcon = ::LoadIcon(hInst, NULL);
    windowClass.hCursor = ::LoadCursor(NULL, IDC_ARROW);
    windowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    windowClass.lpszMenuName = NULL;
    windowClass.lpszClassName = windowClassName;
    windowClass.hIconSm = ::LoadIcon(hInst, NULL);

    static ATOM atom = ::RegisterClassExW(&windowClass);
    assert(atom > 0);
}

//-----------------------------------------------------------------------------
HWND CreateWindow(const wchar_t* windowClassName, HINSTANCE hInst,
    const wchar_t* windowTitle, uint32_t width, uint32_t height) {
    int screenWidth = ::GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = ::GetSystemMetrics(SM_CYSCREEN);

    RECT windowRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    ::AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

    int windowWidth = windowRect.right - windowRect.left;
    int windowHeight = windowRect.bottom - windowRect.top;

    // Center the window within the screen. Clamp to 0, 0 for the top-left corner.
    int windowX = std::max<int>(0, (screenWidth - windowWidth) / 2);
    int windowY = std::max<int>(0, (screenHeight - windowHeight) / 2);
    HWND hWnd = ::CreateWindowExW(
        NULL,
        windowClassName,
        windowTitle,
        WS_OVERLAPPEDWINDOW,
        windowX,
        windowY,
        windowWidth,
        windowHeight,
        NULL,
        NULL,
        hInst,
        nullptr
    );

    assert(hWnd && "Failed to create window");

    return hWnd;
}

//-----------------------------------------------------------------------------
ComPtr<IDXGIAdapter4> GetAdapter(bool useWarp) {
    ComPtr<IDXGIFactory4> dxgiFactory;
    UINT createFactoryFlags = 0;
    #if defined(_DEBUG)
        createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
    #endif

    ThrowIfFailed(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory)));

    ComPtr<IDXGIAdapter1> dxgiAdapter1;
    ComPtr<IDXGIAdapter4> dxgiAdapter4;

    if (useWarp) {
        ThrowIfFailed(dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&dxgiAdapter1)));
        ThrowIfFailed(dxgiAdapter1.As(&dxgiAdapter4));
    } else {
        SIZE_T maxDedicatedVideoMemory = 0;
        for (UINT i = 0; dxgiFactory->EnumAdapters1(i, &dxgiAdapter1) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 dxgiAdapterDesc1;
            dxgiAdapter1->GetDesc1(&dxgiAdapterDesc1);

            // Check to see if the adapter can create a D3D12 device without actually 
            // creating it. The adapter with the largest dedicated video memory
            // is favored.
            if ((dxgiAdapterDesc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0
                && SUCCEEDED(D3D12CreateDevice(dxgiAdapter1.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))
                && dxgiAdapterDesc1.DedicatedVideoMemory > maxDedicatedVideoMemory) {
                maxDedicatedVideoMemory = dxgiAdapterDesc1.DedicatedVideoMemory;
                ThrowIfFailed(dxgiAdapter1.As(&dxgiAdapter4));
            }
        }
    }

    return dxgiAdapter4;
}

//-----------------------------------------------------------------------------
ComPtr<ID3D12Device2> CreateDevice(ComPtr<IDXGIAdapter4> adapter) {
    ComPtr<ID3D12Device2> d3d12Device2;
    ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12Device2)));

    // Enable debug messages in debug mode.
    #if defined(_DEBUG)
        ComPtr<ID3D12InfoQueue> pInfoQueue;
        if (SUCCEEDED(d3d12Device2.As(&pInfoQueue))) {
            pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
            pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);
            // Suppress whole categories of messages
            //D3D12_MESSAGE_CATEGORY Categories[] = {};

            // Suppress messages based on their severity level
            D3D12_MESSAGE_SEVERITY Severities[] =
            {
                D3D12_MESSAGE_SEVERITY_INFO
            };

            // Suppress individual messages by their ID
            D3D12_MESSAGE_ID DenyIds[] = {
                D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,   // I'm really not sure how to avoid this message.
                D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,                         // This warning occurs when using capture frame while graphics debugging.
                D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE,                       // This warning occurs when using capture frame while graphics debugging.
            };

            D3D12_INFO_QUEUE_FILTER NewFilter = {};
            //NewFilter.DenyList.NumCategories = _countof(Categories);
            //NewFilter.DenyList.pCategoryList = Categories;
            NewFilter.DenyList.NumSeverities = _countof(Severities);
            NewFilter.DenyList.pSeverityList = Severities;
            NewFilter.DenyList.NumIDs = _countof(DenyIds);
            NewFilter.DenyList.pIDList = DenyIds;

            ThrowIfFailed(pInfoQueue->PushStorageFilter(&NewFilter));
        }
    #endif

    return d3d12Device2;
}

//-----------------------------------------------------------------------------
/*
D3D12_COMMAND_LIST_TYPE is a type of the queue to create. There are three main
types. Direct, Compute, and Copy. Each one is a superset of the following ones.
This means that DirectQueue can do everything, Compute cannot draw and Copy
cannot dispatch. The GPU may have multiple queues of some types and it is
preferred to create the most specialized queue for given tasks.
E.g., for drawing, Direct is needed but for copying we can create Copy
and maybe the GPU will use specialized queue which can only copy, etc.
*/
ComPtr<ID3D12CommandQueue> CreateCommandQueue(ComPtr<ID3D12Device2> device, D3D12_COMMAND_LIST_TYPE type) {
    ComPtr<ID3D12CommandQueue> d3d12CommandQueue;

    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = type;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = 0;

    ThrowIfFailed(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&d3d12CommandQueue)));

    return d3d12CommandQueue;
}

//-----------------------------------------------------------------------------
/*
This allows us to support variable refresh rate displays.
*/
bool CheckTearingSupport() {
    BOOL allowTearing = FALSE;

    // Rather than create the DXGI 1.5 factory interface directly, we create the
    // DXGI 1.4 interface and query for the 1.5 interface. This is to enable the 
    // graphics debugging tools which will not support the 1.5 factory interface 
    // until a future update.
    ComPtr<IDXGIFactory4> factory4;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory4)))) {
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(factory4.As(&factory5))) {
            if (FAILED(factory5->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                &allowTearing, sizeof(allowTearing)))) {
                allowTearing = FALSE;
            }
        }
    }

    return allowTearing == TRUE;
}

//-----------------------------------------------------------------------------
/*
The swapchain needs at least 2 buffers (back and front).

IDXGISwapChain::Present swaps back and front buffer. Previously bit-block
transfer was used for preset. This meant that the DX runtime copyied the front
buffer to Desktop Window Manager's surface. After it was fully copyied the
image was presented to screen. From Windows 8 DXGI 1.2 flip presentation model
is used. This means that the front buffer is directly passed to the DWM for
presentation. It is more space and time efficient - no copy is needed.
DX12 does not support the bitblt model, only flip.

The swapchain stores pointers to front and all the back buffers. After present
the pointers are updated (another buffer is front and front becomes back).

Flip has two possible effects - DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL and DISCARD
seqential means that the DXGI will persist the contents of the bbuffer for us
DISCARD means that the contents will be discarded after present. Cannot be
used with multisampling. DISCARD cannot be used with partial presentation
additionally.

For max FPS with vsync-off, DISCARD should be used. It means that if the
previously presented frame is still in queue to be presented, it is discarded
and the new frame is placed in front of the queue instead.

SEQUENTIAL places the frame at the end of the queue. This may cause lag when
there are no more buffers to be used as a back buffer (Present will block the
calling thread until a buffer is made available).
*/
ComPtr<IDXGISwapChain4> CreateSwapChain(HWND hWnd,
    ComPtr<ID3D12CommandQueue> commandQueue,
    uint32_t width, uint32_t height, uint32_t bufferCount) {

    ComPtr<IDXGISwapChain4> dxgiSwapChain4;
    ComPtr<IDXGIFactory4> dxgiFactory4;
    UINT createFactoryFlags = 0;
    #if defined(_DEBUG)
        createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
    #endif

    ThrowIfFailed(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory4)));

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = width; // If zero, width from window is used, can be then obtained by GetDesc
    swapChainDesc.Height = height; // If zero, height from window is used, can be then obtained by GetDesc
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.Stereo = FALSE;
    swapChainDesc.SampleDesc = { 1, 0 }; // For flip model SC { 1, 0 } must be used.
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; // May by also SHADER_INPUT
    swapChainDesc.BufferCount = bufferCount;
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    // It is recommended to always allow tearing if tearing support is available.
    swapChainDesc.Flags = CheckTearingSupport() ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(dxgiFactory4->CreateSwapChainForHwnd(
        commandQueue.Get(),
        hWnd,
        &swapChainDesc,
        nullptr,
        nullptr,
        &swapChain1
    ));

    // Disable the Alt+Enter fullscreen toggle feature. Switching to fullscreen
    // will be handled manually.
    ThrowIfFailed(dxgiFactory4->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER));

    ThrowIfFailed(swapChain1.As(&dxgiSwapChain4));

    return dxgiSwapChain4;
}

//-----------------------------------------------------------------------------
/*
DHeap can be seen as an array of resource views. Before we can create any views
we need a memory for them - DHeap. Some views can be allocated from the same
heap, for example CBV, SRV, and UAV. But RTV and Sampler views require separate
DHeaps. 
*/
ComPtr<ID3D12DescriptorHeap> CreateDescriptorHeap(ComPtr<ID3D12Device2> device,
    D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t numDescriptors) {

    ComPtr<ID3D12DescriptorHeap> descriptorHeap;

    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.NumDescriptors = numDescriptors;
    desc.Type = type;
    /*
    Flags may contain D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE which makes
    the descriptors boundable on a command list to be referenced by shaders.
    Without it, CPU can stage the descriptors which can be then copyied to
    a shader visible descriptor heap.
    */

    ThrowIfFailed(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&descriptorHeap)));

    return descriptorHeap;
}

//-----------------------------------------------------------------------------
/*
RTV is a resource which can be bound to a slot in the output merger stage of
the pipeline.
*/
void UpdateRenderTargetViews(ComPtr<ID3D12Device2> device,
    ComPtr<IDXGISwapChain4> swapChain, ComPtr<ID3D12DescriptorHeap> descriptorHeap) {

    UINT rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(descriptorHeap->GetCPUDescriptorHandleForHeapStart());

    for (int i = 0; i < g_NumFrames; ++i) {
        ComPtr<ID3D12Resource> backBuffer;
        ThrowIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer)));

        // nullptr as description means that a description from the resource
        // will be used
        device->CreateRenderTargetView(backBuffer.Get(), nullptr, rtvHandle);

        g_BackBuffers[i] = backBuffer;

        // Manually offset the pointer by a RTV size
        rtvHandle.Offset(rtvDescriptorSize);
    }
}

//-----------------------------------------------------------------------------
/*
Just a memory from which the command lists will be allocated. Memory allocated
by an allocator is reclaimed by Reset. This must be done only after the
commands finished executing on the GPU. This is in turn checked by a fence.

To achieve the best FPS at least one allocator per frame in flight should be
used.

D3D12_COMMAND_LIST_TYPE_BUNDLE as a type means that the command buffer may be
executed only directily via a command list. Bundle is a small list of commands
recorder once and reused multiple times - even across frames. Also across
threads. Bundles are not tied to a pipeline state object. Meaning that the
PSO can update a descriptor table and the bundle will work with different
data. To be efficient bundles have some restrictions. For example may not be 
any commands which change the render target. The command to execute bundles
in command list is ExecuteBundle.
*/
ComPtr<ID3D12CommandAllocator> CreateCommandAllocator(ComPtr<ID3D12Device2> device,
    D3D12_COMMAND_LIST_TYPE type) {

    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ThrowIfFailed(device->CreateCommandAllocator(type, IID_PPV_ARGS(&commandAllocator)));

    return commandAllocator;
}

//-----------------------------------------------------------------------------
/*
Command list can be reset after execute (not after execute finishes as command
allocator). It also needs to be closed before reset.
*/
ComPtr<ID3D12GraphicsCommandList> CreateCommandList(ComPtr<ID3D12Device2> device,
    ComPtr<ID3D12CommandAllocator> commandAllocator, D3D12_COMMAND_LIST_TYPE type) {
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(device->CreateCommandList(0, type, commandAllocator.Get(), nullptr, IID_PPV_ARGS(&commandList)));

    ThrowIfFailed(commandList->Close());

    return commandList;
}

//-----------------------------------------------------------------------------
/*
Fence is GPU/CPU synchronization object. It can be used for synchronization
on either CPU or GPU. Internally it stores 64-bit uint, initialized upon
creation. On CPU the value is updated using ID3D12Fence::Signal, on the GPU it
is updated using ID3D12CommandQueue::Signal.
To wait for a fence to reach a specific value on CPU we use
ID3D12Fence::SetEventOnCompletion followed by WaitForSingleObject. To do the
same on the GPU we use ID3D12CommandQueue::Wait.

Each thread or GPU queue should have at least one fence and a corresponding
fence value. The same fence object should not be signaled from more than one
thread or GPU queue but more than one thread or queue can wait on the same
fence to be signaled.

An OS event handle is used to allow the CPU thread to wait until the fence
has been signaled with a particular value.
*/
ComPtr<ID3D12Fence> CreateFence(ComPtr<ID3D12Device2> device) {
    ComPtr<ID3D12Fence> fence;

    ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));

    return fence;
}

//-----------------------------------------------------------------------------
/*
*/
HANDLE CreateEventHandle() {
    HANDLE fenceEvent;

    fenceEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
    assert(fenceEvent && "Failed to create fence event.");

    return fenceEvent;
}


//-----------------------------------------------------------------------------
/*
Fence is signalled when the GPU reaches this command in its execution.
*/
uint64_t Signal(ComPtr<ID3D12CommandQueue> commandQueue, ComPtr<ID3D12Fence> fence,
    uint64_t& fenceValue) {

    uint64_t fenceValueForSignal = ++fenceValue;
    ThrowIfFailed(commandQueue->Signal(fence.Get(), fenceValueForSignal));

    return fenceValueForSignal;
}

//-----------------------------------------------------------------------------
/*
Read only resources such as material textures do NOT need to be waited for.
But resources such as render targets need to be synchronized to be protected
from being modified from multiple queues at the same time.

This blocks the calling thread until the fence is at `fenceValue`.
*/
void WaitForFenceValue(ComPtr<ID3D12Fence> fence, uint64_t fenceValue, HANDLE fenceEvent,
    std::chrono::milliseconds duration = std::chrono::milliseconds::max()) {

    if (fence->GetCompletedValue() < fenceValue) {
        ThrowIfFailed(fence->SetEventOnCompletion(fenceValue, fenceEvent));
        ::WaitForSingleObject(fenceEvent, static_cast<DWORD>(duration.count()));
    }
}

//-----------------------------------------------------------------------------
/*
Flushing the GPU may be useful for example before resizing bbufers in the swap
chain. This waits for all the command lists to finish execution on the GPU.
*/
void Flush(ComPtr<ID3D12CommandQueue> commandQueue, ComPtr<ID3D12Fence> fence,
    uint64_t& fenceValue, HANDLE fenceEvent) {

    uint64_t fenceValueForSignal = Signal(commandQueue, fence, fenceValue);
    WaitForFenceValue(fence, fenceValueForSignal, fenceEvent);
}

//-----------------------------------------------------------------------------
/*
*/
void Update() {
    static uint64_t frameCounter = 0;
    static double elapsedSeconds = 0.0;
    static std::chrono::high_resolution_clock clock;
    static auto t0 = clock.now();

    frameCounter++;
    auto t1 = clock.now();
    auto deltaTime = t1 - t0;
    t0 = t1;

    elapsedSeconds += deltaTime.count() * 1e-9;
    if (elapsedSeconds > 1.0) {
        char buffer[500];
        auto fps = frameCounter / elapsedSeconds;
        sprintf_s(buffer, 500, "FPS: %f\n", fps);
        OutputDebugStringA(buffer);

        frameCounter = 0;
        elapsedSeconds = 0.0;
    }
}

//-----------------------------------------------------------------------------
/*
The resources need to be in correct state, transition is done by a resouce
barrier. There are several types of barriers. 

Transition transitions a (sub)resource to a particular state before using it.
For example, before a texture can be used in a pixel shader it must be
transitioned to PIXEL_SHADER_RESOURCE state.

Aliasing specifies that a resource is used in a placed or reserved heap when
that resource is aliased with another resource in the same heap.

UAV: Indicates that all UAV accesses to a particular resource have completed
before any future UAV access can begin. This is necessary when the UAV is
transitioned for:
- Read > Write: Guarantees that all previous read operations on the UAV have
completed before being written to in another shader.
- Write > Read: Guarantees that all previous write operations on the UAV have
completed before being read from in another shader.
- Write > Write: Avoids race conditions that could be caused by different
shaders in a different draw or dispatch trying to write to the same resource
(does not avoid race conditions that could be caused in the same draw or
dispatch call).
- A UAV barrier is not needed if the resource is being used as a read-only
(Read > Read) resource between draw or dispatches.

The before state of the resource must be known, it is not tracked internally
and must be tracked by the application.

It is recommended to store all barriers in a list and execute them all at the
same time before an operation that requires the resource to be in a particular
state is executed.
*/
void Render() {
    auto commandAllocator = g_CommandAllocators[g_CurrentBackBufferIndex];
    auto backBuffer = g_BackBuffers[g_CurrentBackBufferIndex];

    commandAllocator->Reset();
    g_CommandList->Reset(commandAllocator.Get(), nullptr);
    
    // Clear the render target.
    {
        CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            backBuffer.Get(),
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET
        );

        g_CommandList->ResourceBarrier(1, &barrier);
        FLOAT clearColor[] = { 0.4f, 0.6f, 0.9f, 1.0f };
        
        // (const D3D12_CPU_DESCRIPTOR_HANDLE &other, 
        // INT offsetInDescriptors, 
        // UINT descriptorIncrementSize)
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(
            g_RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
            g_CurrentBackBufferIndex, 
            g_RTVDescriptorSize
        );

        g_CommandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    }

    // Present
    {
        CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            backBuffer.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        g_CommandList->ResourceBarrier(1, &barrier);
        
        // Close must be called before execute and reset
        ThrowIfFailed(g_CommandList->Close());

        ID3D12CommandList* const commandLists[] = {
            g_CommandList.Get()
        };

        // Errors in recording are reported here
        g_CommandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);
        
        UINT syncInterval = g_VSync ? 1 : 0;
        // DXGI_PRESENT_ALLOW_TEARING does not work with exclusive fullscreen
        UINT presentFlags = g_TearingSupported && !g_VSync ? DXGI_PRESENT_ALLOW_TEARING : 0;
        ThrowIfFailed(g_SwapChain->Present(syncInterval, presentFlags));

        g_FrameFenceValues[g_CurrentBackBufferIndex] = Signal(g_CommandQueue, g_Fence, g_FenceValue);
        
        // Move the bbufer index forward, with flip, the indices do not have to be sequential
        // GetCurrentBackBufferIndex takes care of that.
        g_CurrentBackBufferIndex = g_SwapChain->GetCurrentBackBufferIndex();

        // Wait until GPU finishes the draw of the new current bbufer
        WaitForFenceValue(g_Fence, g_FrameFenceValues[g_CurrentBackBufferIndex], g_FenceEvent);
    }
}

//-----------------------------------------------------------------------------
/*
*/
void Resize(uint32_t width, uint32_t height) {
    if (g_ClientWidth != width || g_ClientHeight != height) {
        // Don't allow 0 size swap chain back buffers.
        g_ClientWidth = std::max(1u, width);
        g_ClientHeight = std::max(1u, height);

        // Flush the GPU queue to make sure the swap chain's back buffers
        // are not being referenced by an in-flight command list.
        Flush(g_CommandQueue, g_Fence, g_FenceValue, g_FenceEvent);

        for (int i = 0; i < g_NumFrames; ++i) {
            // Any references to the back buffers must be released
            // before the swap chain can be resized.
            g_BackBuffers[i].Reset();
            g_FrameFenceValues[i] = g_FrameFenceValues[g_CurrentBackBufferIndex];
        }
        
        DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
        ThrowIfFailed(g_SwapChain->GetDesc(&swapChainDesc));
        ThrowIfFailed(g_SwapChain->ResizeBuffers(g_NumFrames, g_ClientWidth, g_ClientHeight,
            swapChainDesc.BufferDesc.Format, swapChainDesc.Flags));

        g_CurrentBackBufferIndex = g_SwapChain->GetCurrentBackBufferIndex();

        UpdateRenderTargetViews(g_Device, g_SwapChain, g_RTVDescriptorHeap);
    }
}

//-----------------------------------------------------------------------------
/*
*/
void SetFullscreen(bool fullscreen) {
    if (g_Fullscreen != fullscreen) {
        g_Fullscreen = fullscreen;

        if (g_Fullscreen) // Switching to fullscreen.
        {
            // Store the current window dimensions so they can be restored 
            // when switching out of fullscreen state.
            ::GetWindowRect(g_hWnd, &g_WindowRect);
            // Set the window style to a borderless window so the client area fills
            // the entire screen.
            UINT windowStyle = WS_OVERLAPPEDWINDOW & ~(WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);

            ::SetWindowLongW(g_hWnd, GWL_STYLE, windowStyle);
            // Query the name of the nearest display device for the window.
            // This is required to set the fullscreen dimensions of the window
            // when using a multi-monitor setup.
            HMONITOR hMonitor = ::MonitorFromWindow(g_hWnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFOEX monitorInfo = {};
            monitorInfo.cbSize = sizeof(MONITORINFOEX);
            ::GetMonitorInfo(hMonitor, &monitorInfo);
            
            ::SetWindowPos(g_hWnd, HWND_TOP,
                monitorInfo.rcMonitor.left,
                monitorInfo.rcMonitor.top,
                monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
                monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
                SWP_FRAMECHANGED | SWP_NOACTIVATE);

            ::ShowWindow(g_hWnd, SW_MAXIMIZE);
        } else {
            // Restore all the window decorators.
            ::SetWindowLong(g_hWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);

            ::SetWindowPos(g_hWnd, HWND_NOTOPMOST,
                g_WindowRect.left,
                g_WindowRect.top,
                g_WindowRect.right - g_WindowRect.left,
                g_WindowRect.bottom - g_WindowRect.top,
                SWP_FRAMECHANGED | SWP_NOACTIVATE);

            ::ShowWindow(g_hWnd, SW_NORMAL);
        }
    }
}

//-----------------------------------------------------------------------------
/*
*/
LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (g_IsInitialized) {
        switch (message) {
            case WM_PAINT:
                Update();
                Render();
                break;
            case WM_SYSKEYDOWN:
            case WM_KEYDOWN:
            {
                bool alt = (::GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

                switch (wParam) {
                    case 'V':
                        g_VSync = !g_VSync;
                        break;
                    case VK_ESCAPE:
                        ::PostQuitMessage(0);
                        break;
                    case VK_RETURN:
                        if (alt) {
                    case VK_F11:
                        SetFullscreen(!g_Fullscreen);
                        }
                        break;
                }
            }
            break;
            // The default window procedure will play a system notification sound 
            // when pressing the Alt+Enter keyboard combination if this message is 
            // not handled.
            case WM_SYSCHAR:
                break;
            case WM_SIZE:
            {
                RECT clientRect = {};
                ::GetClientRect(g_hWnd, &clientRect);

                int width = clientRect.right - clientRect.left;
                int height = clientRect.bottom - clientRect.top;

                Resize(width, height);
            }
            break;
            case WM_DESTROY:
                ::PostQuitMessage(0);
                break;
            default:
                return ::DefWindowProcW(hwnd, message, wParam, lParam);
        }
    } else {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }

    return 0;
}

//-----------------------------------------------------------------------------
/*
*/
int CALLBACK wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow) {
    // Windows 10 Creators update adds Per Monitor V2 DPI awareness context.
    // Using this awareness context allows the client area of the window 
    // to achieve 100% scaling while still allowing non-client window content to 
    // be rendered in a DPI sensitive fashion.
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Window class name. Used for registering / creating the window.
    const wchar_t* windowClassName = L"DX12WindowClass";
    ParseCommandLineArguments();

    EnableDebugLayer();

    g_TearingSupported = CheckTearingSupport();

    RegisterWindowClass(hInstance, windowClassName);
    g_hWnd = CreateWindow(windowClassName, hInstance, L"Learning DirectX 12",
        g_ClientWidth, g_ClientHeight);

    // Initialize the global window rect variable.
    ::GetWindowRect(g_hWnd, &g_WindowRect);

    ComPtr<IDXGIAdapter4> dxgiAdapter4 = GetAdapter(g_UseWarp);

    g_Device = CreateDevice(dxgiAdapter4);

    g_CommandQueue = CreateCommandQueue(g_Device, D3D12_COMMAND_LIST_TYPE_DIRECT);

    g_SwapChain = CreateSwapChain(g_hWnd, g_CommandQueue,
        g_ClientWidth, g_ClientHeight, g_NumFrames);

    g_CurrentBackBufferIndex = g_SwapChain->GetCurrentBackBufferIndex();

    g_RTVDescriptorHeap = CreateDescriptorHeap(g_Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, g_NumFrames);
    g_RTVDescriptorSize = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    UpdateRenderTargetViews(g_Device, g_SwapChain, g_RTVDescriptorHeap);

    for (int i = 0; i < g_NumFrames; ++i) {
        g_CommandAllocators[i] = CreateCommandAllocator(g_Device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    }
    g_CommandList = CreateCommandList(g_Device,
        g_CommandAllocators[g_CurrentBackBufferIndex], D3D12_COMMAND_LIST_TYPE_DIRECT
    );

    g_Fence = CreateFence(g_Device);
    g_FenceEvent = CreateEventHandle();

    g_IsInitialized = true;

    ::ShowWindow(g_hWnd, SW_SHOW);

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (::PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
        }
    }
    
    // Make sure the command queue has finished all commands before closing.
    Flush(g_CommandQueue, g_Fence, g_FenceValue, g_FenceEvent);

    ::CloseHandle(g_FenceEvent);

    return 0;
}


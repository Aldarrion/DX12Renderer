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


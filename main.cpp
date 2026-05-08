// trigger-shot: Webcam interval photo capture for Win32
//
// Enumerates all connected webcams and captures a JPEG from each one
// on a regular interval. Files are named:
//   <camera>_<start-timestamp>_<count5digits>.jpg
//
// Usage:
//   trigger-shot --delay <seconds> --output <path> [--count <n>]

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "propsys.lib")

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static std::atomic<bool> g_running{ true };

BOOL WINAPI CtrlHandler(DWORD fdwCtrlType)
{
    if (fdwCtrlType == CTRL_C_EVENT || fdwCtrlType == CTRL_BREAK_EVENT)
    {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::wstring SanitizeForFilename(std::wstring s)
{
    const std::wstring bad = L"\\/:*?\"<>| ";
    for (auto& c : s)
        if (bad.find(c) != std::wstring::npos)
            c = L'_';
    return s;
}

static std::wstring GetLocalTimestamp()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[32];
    swprintf_s(buf, L"%04d%02d%02d_%02d%02d%02d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------

struct Camera
{
    std::wstring     friendlyName;
    std::wstring     safeName;      // sanitized for filenames
    IMFSourceReader* reader  = nullptr;
    UINT32           width   = 0;
    UINT32           height  = 0;
    LONG             stride  = 0;   // negative = bottom-up

    Camera() = default;
    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;

    ~Camera()
    {
        if (reader) { reader->Release(); reader = nullptr; }
    }
};

// ---------------------------------------------------------------------------
// Media Foundation helpers
// ---------------------------------------------------------------------------

static HRESULT ConfigureReader(IMFSourceReader* reader, Camera& cam)
{
    // Ask for RGB32 output (video processor MFT handles conversion).
    IMFMediaType* pType = nullptr;
    HRESULT hr = MFCreateMediaType(&pType);

    if (SUCCEEDED(hr))
        hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr))
        hr = pType->SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_RGB32);

    if (SUCCEEDED(hr))
        hr = reader->SetCurrentMediaType(
            (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pType);

    if (pType) { pType->Release(); pType = nullptr; }
    if (FAILED(hr)) return hr;

    // Read back the negotiated media type to cache frame dimensions / stride.
    hr = reader->GetCurrentMediaType(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pType);

    if (SUCCEEDED(hr))
    {
        hr = MFGetAttributeSize(pType, MF_MT_FRAME_SIZE, &cam.width, &cam.height);

        if (SUCCEEDED(hr))
        {
            // MF_MT_DEFAULT_STRIDE stores a LONG as a UINT32 raw bits.
            UINT32 rawStride = 0;
            if (SUCCEEDED(pType->GetUINT32(MF_MT_DEFAULT_STRIDE, &rawStride)))
                cam.stride = static_cast<LONG>(rawStride);
            else
                cam.stride = static_cast<LONG>(cam.width * 4u); // assume top-down
        }
        pType->Release();
    }
    return hr;
}

// ---------------------------------------------------------------------------
// JPEG saving via WIC
// ---------------------------------------------------------------------------

static HRESULT SaveJpeg(
    const BYTE*        pixels,  // top-down, row-major, BGRA
    UINT32             width,
    UINT32             height,
    UINT32             rowBytes,
    const std::wstring& path)
{
    IWICImagingFactory*    pFactory    = nullptr;
    IWICBitmap*            pBitmap     = nullptr;
    IWICStream*            pStream     = nullptr;
    IWICBitmapEncoder*     pEncoder    = nullptr;
    IWICBitmapFrameEncode* pFrame      = nullptr;
    IPropertyBag2*         pProps      = nullptr;

    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&pFactory));

    if (SUCCEEDED(hr))
        hr = pFactory->CreateBitmapFromMemory(
            width, height,
            GUID_WICPixelFormat32bppBGRA,
            rowBytes, rowBytes * height,
            const_cast<BYTE*>(pixels),
            &pBitmap);

    if (SUCCEEDED(hr)) hr = pFactory->CreateStream(&pStream);
    if (SUCCEEDED(hr)) hr = pStream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = pFactory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &pEncoder);
    if (SUCCEEDED(hr)) hr = pEncoder->Initialize(pStream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = pEncoder->CreateNewFrame(&pFrame, &pProps);

    if (SUCCEEDED(hr))
    {
        // Set JPEG quality to 92%.
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v{};
        v.vt    = VT_R4;
        v.fltVal = 0.92f;
        pProps->Write(1, &opt, &v);

        hr = pFrame->Initialize(pProps);
    }

    if (SUCCEEDED(hr)) hr = pFrame->SetSize(width, height);

    if (SUCCEEDED(hr))
    {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        hr = pFrame->SetPixelFormat(&fmt);
    }

    if (SUCCEEDED(hr)) hr = pFrame->WriteSource(pBitmap, nullptr);
    if (SUCCEEDED(hr)) hr = pFrame->Commit();
    if (SUCCEEDED(hr)) hr = pEncoder->Commit();

    if (pProps)    pProps->Release();
    if (pFrame)    pFrame->Release();
    if (pEncoder)  pEncoder->Release();
    if (pStream)   pStream->Release();
    if (pBitmap)   pBitmap->Release();
    if (pFactory)  pFactory->Release();

    return hr;
}

// ---------------------------------------------------------------------------
// Frame capture
// ---------------------------------------------------------------------------

static bool CaptureFrame(Camera& cam, const fs::path& outPath)
{
    DWORD    streamIndex = 0, flags = 0;
    LONGLONG timestamp   = 0;
    IMFSample* pSample   = nullptr;

    HRESULT hr = cam.reader->ReadSample(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
        0, &streamIndex, &flags, &timestamp, &pSample);

    if (FAILED(hr) || !pSample)
    {
        if (pSample) pSample->Release();
        return false;
    }

    IMFMediaBuffer* pBuf = nullptr;
    hr = pSample->ConvertToContiguousBuffer(&pBuf);

    bool ok = false;
    if (SUCCEEDED(hr))
    {
        BYTE*  data   = nullptr;
        DWORD  curLen = 0;
        hr = pBuf->Lock(&data, nullptr, &curLen);

        if (SUCCEEDED(hr))
        {
            UINT32 absStride = static_cast<UINT32>(std::abs(cam.stride));
            bool   bottomUp  = (cam.stride < 0);

            // Normalise to top-down BGRA.
            std::vector<BYTE> topDown(absStride * cam.height);
            for (UINT32 row = 0; row < cam.height; ++row)
            {
                UINT32 srcRow = bottomUp ? (cam.height - 1u - row) : row;
                memcpy(topDown.data() + row * absStride,
                       data           + srcRow * absStride,
                       absStride);
            }

            hr = SaveJpeg(topDown.data(), cam.width, cam.height,
                          absStride, outPath.wstring());
            ok = SUCCEEDED(hr);
            pBuf->Unlock();
        }
        pBuf->Release();
    }

    pSample->Release();
    return ok;
}

// ---------------------------------------------------------------------------
// Camera enumeration
// ---------------------------------------------------------------------------

static std::vector<std::unique_ptr<Camera>> EnumerateCameras()
{
    std::vector<std::unique_ptr<Camera>> cameras;

    IMFAttributes* pAttrs = nullptr;
    HRESULT hr = MFCreateAttributes(&pAttrs, 1);
    if (FAILED(hr)) return cameras;

    hr = pAttrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                         MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** ppDevices = nullptr;
    UINT32 count = 0;

    if (SUCCEEDED(hr))
        hr = MFEnumDeviceSources(pAttrs, &ppDevices, &count);

    pAttrs->Release();

    if (FAILED(hr) || count == 0)
    {
        if (ppDevices) CoTaskMemFree(ppDevices);
        return cameras;
    }

    for (UINT32 i = 0; i < count; ++i)
    {
        auto cam = std::make_unique<Camera>();

        // Friendly name.
        WCHAR* nameStr = nullptr;
        UINT32 nameLen = 0;
        if (SUCCEEDED(ppDevices[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &nameStr, &nameLen))
            && nameStr)
        {
            cam->friendlyName = nameStr;
            CoTaskMemFree(nameStr);
        }
        else
        {
            cam->friendlyName = L"Camera_" + std::to_wstring(i);
        }
        cam->safeName = SanitizeForFilename(cam->friendlyName);

        // Activate source.
        IMFMediaSource* pSource = nullptr;
        if (FAILED(ppDevices[i]->ActivateObject(IID_PPV_ARGS(&pSource))))
        {
            ppDevices[i]->Release();
            continue;
        }

        // Create source reader with video processing enabled so the MFT
        // pipeline can convert any native format to our requested RGB32.
        IMFAttributes* pReaderAttrs = nullptr;
        MFCreateAttributes(&pReaderAttrs, 2);
        pReaderAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

        IMFSourceReader* pReader = nullptr;
        hr = MFCreateSourceReaderFromMediaSource(pSource, pReaderAttrs, &pReader);

        pReaderAttrs->Release();
        pSource->Release();

        if (FAILED(hr))
        {
            ppDevices[i]->Release();
            continue;
        }

        cam->reader = pReader;

        if (FAILED(ConfigureReader(pReader, *cam)))
        {
            // Camera opened but couldn't negotiate RGB32; skip it.
            ppDevices[i]->Release();
            continue;
        }

        // Warm-up: discard one frame so the first real capture is fresh.
        {
            DWORD si = 0, fl = 0;
            LONGLONG ts = 0;
            IMFSample* s = nullptr;
            pReader->ReadSample(
                (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                0, &si, &fl, &ts, &s);
            if (s) s->Release();
        }

        cameras.push_back(std::move(cam));
        ppDevices[i]->Release();
    }

    CoTaskMemFree(ppDevices);
    return cameras;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

struct Options
{
    double      delaySecs = 0.0;
    fs::path    outputDir;
    long long   maxCount  = -1;   // -1 = unlimited
};

static void PrintUsage(const wchar_t* prog)
{
    std::wcout
        << L"Usage:\n"
        << L"  " << prog << L" --delay <seconds> --output <path> [--count <n>]\n\n"
        << L"Options:\n"
        << L"  --delay  -d  <seconds>  Interval between captures (required, > 0)\n"
        << L"  --output -o  <path>     Output folder (required, created if absent)\n"
        << L"  --count  -n  <n>        Photos per camera (optional; Ctrl+C to stop)\n"
        << L"  --help   -h             Show this help\n";
}

static bool ParseArgs(int argc, wchar_t* argv[], Options& opts)
{
    bool hasDelay  = false;
    bool hasOutput = false;

    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];

        auto nextArg = [&]() -> std::wstring {
            if (i + 1 < argc) return argv[++i];
            return {};
        };

        if (a == L"--delay"  || a == L"-d") { opts.delaySecs = std::stod(nextArg()); hasDelay  = true; }
        else if (a == L"--output" || a == L"-o") { opts.outputDir  = nextArg(); hasOutput = true; }
        else if (a == L"--count"  || a == L"-n") { opts.maxCount   = std::stoll(nextArg()); }
        else if (a == L"--help"   || a == L"-h") { PrintUsage(argv[0]); return false; }
    }

    if (!hasDelay || !hasOutput)
    {
        std::wcerr << L"Error: --delay and --output are required.\n\n";
        PrintUsage(argv[0]);
        return false;
    }
    if (opts.delaySecs <= 0.0)
    {
        std::wcerr << L"Error: --delay must be a positive number.\n";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int wmain(int argc, wchar_t* argv[])
{
    Options opts;
    if (!ParseArgs(argc, argv, opts))
        return 1;

    // Create output directory.
    std::error_code ec;
    fs::create_directories(opts.outputDir, ec);
    if (ec)
    {
        std::wcerr << L"Error: cannot create output directory: "
                   << opts.outputDir.wstring() << L"\n  " << ec.message().c_str() << L"\n";
        return 1;
    }

    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    // COM / Media Foundation.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { std::wcerr << L"COM init failed.\n"; return 1; }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) { std::wcerr << L"MF init failed.\n"; CoUninitialize(); return 1; }

    // Enumerate cameras.
    std::wcout << L"Scanning for cameras...\n";
    auto cameras = EnumerateCameras();

    if (cameras.empty())
    {
        std::wcout << L"No webcams found.\n";
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    std::wcout << L"Found " << cameras.size() << L" camera(s):\n";
    for (auto& c : cameras)
        std::wcout << L"  [" << c->width << L"x" << c->height << L"]  " << c->friendlyName << L"\n";

    // Build the start-time stamp used in every filename.
    const std::wstring startStamp = GetLocalTimestamp();

    if (opts.maxCount >= 0)
        std::wcout << L"Capturing " << opts.maxCount << L" photo(s) per camera, "
                   << opts.delaySecs << L"s interval.\n";
    else
        std::wcout << L"Capturing every " << opts.delaySecs << L"s (press Ctrl+C to stop).\n";

    std::wcout << L"Output: " << opts.outputDir.wstring() << L"\n\n";

    // Capture loop.
    long long captureRound = 0;

    while (g_running)
    {
        if (opts.maxCount >= 0 && captureRound >= opts.maxCount)
            break;

        for (auto& cam : cameras)
        {
            if (!g_running) break;

            wchar_t countBuf[16];
            swprintf_s(countBuf, L"%05lld", captureRound);

            std::wstring filename =
                cam->safeName + L"_" + startStamp + L"_" + countBuf + L".jpg";
            fs::path outPath = opts.outputDir / filename;

            bool ok = CaptureFrame(*cam, outPath);

            std::wcout << (ok ? L"  saved  " : L"  FAILED ") << filename << L"\n";
        }

        ++captureRound;

        if (opts.maxCount >= 0 && captureRound >= opts.maxCount)
            break;

        // Sleep in small slices so Ctrl+C is responsive.
        auto wake = std::chrono::steady_clock::now()
                  + std::chrono::duration<double>(opts.delaySecs);
        while (g_running && std::chrono::steady_clock::now() < wake)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::wcout << L"\nDone. " << captureRound << L" round(s) completed.\n";

    MFShutdown();
    CoUninitialize();
    return 0;
}

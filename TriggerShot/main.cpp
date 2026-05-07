// TriggerShot - Time-lapse photography utility
// A product of J2i.net, LLC 2026. All Rights Reserved.
//
// Usage: TriggerShot <interval_seconds> <output_folder> [count]
//
// Enumerates WIA cameras, creates per-camera subfolders, and captures
// images at the specified interval until CTRL-C or count is reached.

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <wia.h>
#include <objbase.h>
#include <comdef.h>
#include <shlwapi.h>

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <atomic>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "wiaguid.lib")

// ---------------------------------------------------------------------------
// Global flag set by the CTRL-C handler to stop the capture loop
// ---------------------------------------------------------------------------
static std::atomic<bool> g_running(true);

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
{
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT)
    {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Returns a sortable timestamp string: YYYYMMDD_HHMMSS
static std::wstring GetTimestamp()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::wostringstream oss;
    oss << st.wYear
        << std::setfill(L'0') << std::setw(2) << st.wMonth
        << std::setw(2) << st.wDay
        << L"_"
        << std::setw(2) << st.wHour
        << std::setw(2) << st.wMinute
        << std::setw(2) << st.wSecond;
    return oss.str();
}

// Replaces characters that are illegal in Windows file/folder names
static std::wstring SanitizeName(const std::wstring& name)
{
    std::wstring result = name;
    for (wchar_t& c : result)
    {
        if (c == L'/' || c == L'\\' || c == L':' || c == L'*' ||
            c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|')
        {
            c = L'_';
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// WIA transfer callback — saves the incoming image stream to a local file
// ---------------------------------------------------------------------------
class WiaFileTransferCallback : public IWiaTransferCallback
{
public:
    explicit WiaFileTransferCallback(const std::wstring& filePath)
        : m_filePath(filePath), m_refCount(1)
    {
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IWiaTransferCallback)
        {
            *ppv = static_cast<IWiaTransferCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return InterlockedIncrement(&m_refCount);
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG count = InterlockedDecrement(&m_refCount);
        if (count == 0)
            delete this;
        return count;
    }

    // IWiaTransferCallback
    HRESULT STDMETHODCALLTYPE TransferCallback(LONG /*lFlags*/,
                                               WiaTransferParams* /*pParams*/) override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetNextStream(LONG /*lFlags*/,
                                            BSTR /*bstrItemName*/,
                                            BSTR /*bstrFullItemName*/,
                                            IStream** ppDestination) override
    {
        if (!ppDestination)
            return E_POINTER;

        return SHCreateStreamOnFileW(
            m_filePath.c_str(),
            STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE,
            ppDestination);
    }

private:
    std::wstring m_filePath;
    volatile LONG m_refCount;
};

// ---------------------------------------------------------------------------
// Per-camera metadata gathered during enumeration
// ---------------------------------------------------------------------------
struct CameraInfo
{
    std::wstring deviceId;
    std::wstring deviceName;
    std::wstring folderPath; // output sub-folder for this camera
};

// ---------------------------------------------------------------------------
// Issue a WIA_CMD_TAKE_PICTURE, enumerate the resulting child item, and
// transfer it to outputFilePath.
// ---------------------------------------------------------------------------
static HRESULT CaptureImage(IWiaDevMgr2* pDevMgr,
                             const CameraInfo& cam,
                             const std::wstring& outputFilePath)
{
    // Connect to the device
    BSTR bstrDevId = SysAllocString(cam.deviceId.c_str());
    IWiaItem2* pRootItem = nullptr;
    HRESULT hr = pDevMgr->CreateDevice(0, bstrDevId, &pRootItem);
    SysFreeString(bstrDevId);

    if (FAILED(hr))
    {
        std::wcerr << L"  Failed to connect to " << cam.deviceName
                   << L" (0x" << std::hex << hr << std::dec << L")" << std::endl;
        return hr;
    }

    // Take picture
    hr = pRootItem->ExecuteCommand(WIA_CMD_TAKE_PICTURE);
    if (FAILED(hr))
    {
        std::wcerr << L"  WIA_CMD_TAKE_PICTURE failed for " << cam.deviceName
                   << L" (0x" << std::hex << hr << std::dec << L")" << std::endl;
        pRootItem->Release();
        return hr;
    }

    // The captured image appears as a child item
    IEnumWiaItem2* pEnum = nullptr;
    hr = pRootItem->EnumChildItems(nullptr, &pEnum);
    if (FAILED(hr))
    {
        pRootItem->Release();
        return hr;
    }

    IWiaItem2* pImageItem = nullptr;
    ULONG fetched = 0;
    hr = pEnum->Next(1, &pImageItem, &fetched);
    pEnum->Release();

    if (FAILED(hr) || fetched == 0 || !pImageItem)
    {
        pRootItem->Release();
        return FAILED(hr) ? hr : E_FAIL;
    }

    // Transfer to file via the callback
    IWiaTransfer* pTransfer = nullptr;
    hr = pImageItem->QueryInterface(IID_IWiaTransfer, reinterpret_cast<void**>(&pTransfer));
    if (SUCCEEDED(hr))
    {
        WiaFileTransferCallback* pCallback = new WiaFileTransferCallback(outputFilePath);
        hr = pTransfer->Download(0, pCallback);
        pCallback->Release();
        pTransfer->Release();
    }

    pImageItem->Release();
    pRootItem->Release();
    return hr;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int wmain(int argc, wchar_t* argv[])
{
    std::wcout << L"A product of J2i.net, LLC 2026. All Rights Reserved." << std::endl;

    if (argc < 3)
    {
        std::wcout
            << L"\nUsage: TriggerShot <interval_seconds> <output_folder> [count]\n"
            << L"\n"
            << L"  interval_seconds  Time between photographs (seconds, must be > 0)\n"
            << L"  output_folder     Root folder for captured images\n"
            << L"  count             (Optional) Total number of capture rounds; "
               L"omit for continuous\n"
            << L"\nPress CTRL-C to stop early.\n";
        return 1;
    }

    // --- Parse arguments ---
    const int intervalSeconds = _wtoi(argv[1]);
    if (intervalSeconds <= 0)
    {
        std::wcerr << L"Error: interval_seconds must be a positive integer." << std::endl;
        return 1;
    }

    const std::wstring outputFolder = argv[2];

    int maxCount = 0; // 0 means run until CTRL-C
    if (argc >= 4)
    {
        maxCount = _wtoi(argv[3]);
        if (maxCount <= 0)
        {
            std::wcerr << L"Error: count must be a positive integer." << std::endl;
            return 1;
        }
    }

    // --- Create root output folder ---
    std::error_code ec;
    std::filesystem::create_directories(outputFolder, ec);
    if (ec)
    {
        std::wcerr << L"Error: could not create folder \"" << outputFolder
                   << L"\": " << ec.message().c_str() << std::endl;
        return 1;
    }

    // --- COM initialisation ---
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
    {
        std::wcerr << L"Error: CoInitializeEx failed (0x"
                   << std::hex << hr << std::dec << L")." << std::endl;
        return 1;
    }

    // --- WIA Device Manager ---
    IWiaDevMgr2* pDevMgr = nullptr;
    hr = CoCreateInstance(CLSID_WiaDevMgr2, nullptr, CLSCTX_LOCAL_SERVER,
                          IID_IWiaDevMgr2, reinterpret_cast<void**>(&pDevMgr));
    if (FAILED(hr))
    {
        std::wcerr << L"Error: failed to create WIA Device Manager (0x"
                   << std::hex << hr << std::dec << L")." << std::endl;
        CoUninitialize();
        return 1;
    }

    // --- Enumerate cameras ---
    std::vector<CameraInfo> cameras;

    IEnumWIA_DEV_INFO* pEnumInfo = nullptr;
    hr = pDevMgr->EnumDeviceInfo(WIA_DEVINFO_ENUM_LOCAL, &pEnumInfo);
    if (SUCCEEDED(hr))
    {
        IWiaPropertyStorage* pPropStorage = nullptr;
        ULONG fetched = 0;

        while (pEnumInfo->Next(1, &pPropStorage, &fetched) == S_OK && fetched > 0)
        {
            PROPSPEC specs[3] = {};
            PROPVARIANT vars[3] = {};

            specs[0].ulKind = PRSPEC_PROPID;
            specs[0].propid = WIA_DIP_DEV_TYPE;
            specs[1].ulKind = PRSPEC_PROPID;
            specs[1].propid = WIA_DIP_DEV_ID;
            specs[2].ulKind = PRSPEC_PROPID;
            specs[2].propid = WIA_DIP_DEV_NAME;

            if (SUCCEEDED(pPropStorage->ReadMultiple(3, specs, vars)))
            {
                const LONG devType = vars[0].lVal;
                // Only pick up camera-class devices
                if (GET_STIDEVICE_TYPE(devType) == StiDeviceTypeCameraDevice)
                {
                    CameraInfo cam;
                    if (vars[1].vt == VT_BSTR)
                        cam.deviceId = vars[1].bstrVal;
                    if (vars[2].vt == VT_BSTR)
                        cam.deviceName = vars[2].bstrVal;

                    cam.folderPath =
                        outputFolder + L"\\" + SanitizeName(cam.deviceName);

                    std::filesystem::create_directories(cam.folderPath, ec);

                    cameras.push_back(cam);
                    std::wcout << L"Found camera: " << cam.deviceName << std::endl;
                }

                PropVariantClear(&vars[0]);
                PropVariantClear(&vars[1]);
                PropVariantClear(&vars[2]);
            }

            pPropStorage->Release();
            pPropStorage = nullptr;
            fetched = 0;
        }

        pEnumInfo->Release();
    }

    if (cameras.empty())
    {
        std::wcout << L"No cameras found on this system." << std::endl;
        pDevMgr->Release();
        CoUninitialize();
        return 0;
    }

    // --- CTRL-C handler ---
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    // --- Capture loop ---
    int photoCount = 0;

    while (g_running && (maxCount == 0 || photoCount < maxCount))
    {
        ++photoCount;

        for (const auto& cam : cameras)
        {
            if (!g_running)
                break;

            const std::wstring timestamp = GetTimestamp();

            std::wostringstream filePath;
            filePath << cam.folderPath << L"\\"
                     << SanitizeName(cam.deviceName)
                     << L"_" << timestamp
                     << L"_" << std::setfill(L'0') << std::setw(6) << photoCount
                     << L".jpg";

            CaptureImage(pDevMgr, cam, filePath.str());
        }

        std::wcout << photoCount << std::endl;

        if (maxCount > 0 && photoCount >= maxCount)
            break;

        // Wait for the interval, checking for CTRL-C every 100 ms
        for (int i = 0; i < intervalSeconds * 10 && g_running; ++i)
            Sleep(100);
    }

    pDevMgr->Release();
    CoUninitialize();
    return 0;
}

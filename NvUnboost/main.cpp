#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <Windows.h>
#include <avrt.h>
#include <commctrl.h>
#include <process.h>
#include <winternl.h>

#include "resource.h"

namespace
{
    const UINT WM_NOTIFYICON = WM_USER;

    enum ColumnId
    {
        COLUMN_ID_TIME,
        COLUMN_ID_PROCESS_PATH,
        COLUMN_ID_PID,
        COLUMN_ID_TID,
        COLUMN_ID_BASE_PRIO,
        COLUMN_ID_DYN_PRIO,
        COLUMN_ID_COMMENT
    };

    struct THREAD_BASIC_INFORMATION
    {
        NTSTATUS  ExitStatus;
        PVOID     TebBaseAddress;
        CLIENT_ID ClientId;
        KAFFINITY AffinityMask;
        KPRIORITY Priority;
        KPRIORITY BasePriority;
    };

    struct ThreadId
    {
        DWORD pid;
        DWORD tid;

        bool operator<(const ThreadId& rhs) const
        {
            return std::make_pair(pid, tid) < std::make_pair(rhs.pid, rhs.tid);
        }
    };

    struct ThreadInfo
    {
        std::wstring procName;
        KPRIORITY basePriority;
        KPRIORITY priority;
        HANDLE thread;
    };

    const wchar_t g_windowName[] = L"NvUnboost " VERSION_STRING;

    decltype(&NtQueryInformationThread) g_ntQueryInformationThread = nullptr;
    decltype(&NtQuerySystemInformation) g_ntQuerySystemInformation = nullptr;
    decltype(&NtSetInformationThread) g_ntSetInformationThread = nullptr;

    HWND g_mainWindow = nullptr;
    HWND g_listView = nullptr;
    std::set<ThreadId> g_failedThreads;

    UINT g_taskBarCreated = 0;
    HICON g_trayIcon = nullptr;
    bool g_isTrayIconCreated = false;

    void addRow(const ThreadId& threadId, const ThreadInfo& threadInfo, wchar_t* msg)
    {
        LVITEM item = {};
        if (-1 == ListView_InsertItem(g_listView, &item))
        {
            return;
        }

        const DWORD MAX_TEXT_SIZE = 1024;
        wchar_t text[MAX_TEXT_SIZE] = {};
        SYSTEMTIME st = {};
        GetLocalTime(&st);
        swprintf_s(text, L"%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        ListView_SetItemText(g_listView, 0, COLUMN_ID_TIME, text);

        if (0 != threadId.pid)
        {
            wcscpy_s(text, threadInfo.procName.c_str());
            auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, threadId.pid);
            if (process)
            {
                DWORD size = MAX_TEXT_SIZE;
                QueryFullProcessImageName(process, 0, text, &size);
                CloseHandle(process);
            }
            ListView_SetItemText(g_listView, 0, COLUMN_ID_PROCESS_PATH, text);

            swprintf_s(text, L"%u", threadId.pid);
            ListView_SetItemText(g_listView, 0, COLUMN_ID_PID, text);

            swprintf_s(text, L"%u", threadId.tid);
            ListView_SetItemText(g_listView, 0, COLUMN_ID_TID, text);

            swprintf_s(text, L"%u", threadInfo.basePriority);
            ListView_SetItemText(g_listView, 0, COLUMN_ID_BASE_PRIO, text);

            swprintf_s(text, L"%u", threadInfo.priority);
            ListView_SetItemText(g_listView, 0, COLUMN_ID_DYN_PRIO, text);
        }

        ListView_SetItemText(g_listView, 0, COLUMN_ID_COMMENT, msg);

        const UINT MAX_ROWS = 1000;
        if (MAX_ROWS == ListView_GetItemCount(g_listView))
        {
            ListView_DeleteItem(g_listView, MAX_ROWS);
        }
    }

    void addTrayIcon()
    {
        NOTIFYICONDATA nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_mainWindow;
        if (g_isTrayIconCreated)
        {
            Shell_NotifyIcon(NIM_DELETE, &nid);
        }

        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = WM_NOTIFYICON;
        nid.hIcon = g_trayIcon;
        wcscpy_s(nid.szTip, g_windowName);
        nid.uVersion = NOTIFYICON_VERSION_4;
        g_isTrayIconCreated = Shell_NotifyIcon(NIM_ADD, &nid);
    }

    template <typename... Params>
    void reportThreadEvent(const wchar_t* fmt, const ThreadId& threadId, const ThreadInfo& threadInfo, Params... params)
    {
        wchar_t msg[256] = {};
        swprintf_s(msg, fmt, params...);
        addRow(threadId, threadInfo, msg);
    }

    template <typename... Params>
    void reportThreadError(const wchar_t* fmt, const ThreadId& threadId, const ThreadInfo& threadInfo, Params... params)
    {
        reportThreadEvent(fmt, threadId, threadInfo, params...);
        if (0 != threadId.pid)
        {
            g_failedThreads.insert(threadId);
            if (threadInfo.thread)
            {
                CloseHandle(threadInfo.thread);
            }
        }
    }

    template <typename... Params>
    void reportEvent(const wchar_t* fmt, Params... params)
    {
        reportThreadEvent(fmt, {}, {}, params...);
    }

    template <typename... Params>
    void fatalError(const wchar_t* fmt, Params... params)
    {
        wchar_t msg[256] = {};
        swprintf_s(msg, fmt, params...);
        MessageBox(nullptr, msg, L"NvUnboost", MB_ICONERROR);
        ExitProcess(0);
    }

    void boostThreadPrio()
    {
        DWORD taskIndex = 0;
        HANDLE task = AvSetMmThreadCharacteristics(L"Audio", &taskIndex);
        if (!task)
        {
            reportEvent(L"AvSetMmThreadCharacteristics failed (0x%X)", GetLastError());
        }
        else if (!AvSetMmThreadPriority(task, AVRT_PRIORITY_CRITICAL))
        {
            reportEvent(L"AvSetMmThreadPriority failed (0x%X)", GetLastError());
        }
    }

    std::map<ThreadId, ThreadInfo> getHighPrioThreads()
    {
        static std::vector<BYTE> buffer;
        ULONG size = static_cast<ULONG>(buffer.size());
        NTSTATUS status = 0;
        while (!NT_SUCCESS(status = g_ntQuerySystemInformation(SystemProcessInformation, buffer.data(), size, &size)))
        {
            if (size <= buffer.size())
            {
                reportEvent(L"NtQuerySystemInformation failed (0x%X)", status);
                return {};
            }
            buffer.resize(size);
        }

        if (0 == size)
        {
            reportEvent(L"Unknown error - no threads found");
            return {};
        }

        std::set<ThreadId> foundFailedThreads;
        std::map<ThreadId, ThreadInfo> highPrioThreads;

        auto ptr = buffer.data();
        while (ptr)
        {
            const auto spi = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(ptr);
            auto sti = reinterpret_cast<const SYSTEM_THREAD_INFORMATION*>(spi + 1);
            for (ULONG i = 0; i < spi->NumberOfThreads; ++i)
            {
                const auto pid = *reinterpret_cast<const DWORD*>(&sti->ClientId.UniqueProcess);
                if (0 == pid || 4 == pid)
                {
                    break;
                }

                const auto tid = *reinterpret_cast<const DWORD*>(&sti->ClientId.UniqueThread);
                const ThreadId threadId = { pid, tid };
                if (g_failedThreads.find(threadId) != g_failedThreads.end())
                {
                    foundFailedThreads.insert(threadId);
                    continue;
                }

                if (sti->BasePriority <= THREAD_PRIORITY_TIME_CRITICAL &&
                    sti->Priority > THREAD_PRIORITY_TIME_CRITICAL)
                {
                    ThreadInfo ti = {};
                    ti.procName.assign(spi->ImageName.Buffer, spi->ImageName.Length);
                    ti.basePriority = sti->BasePriority;
                    ti.priority = sti->Priority;
                    highPrioThreads[threadId] = ti;
                }

                sti++;
            }

            ptr = (0 != spi->NextEntryOffset) ? (ptr + spi->NextEntryOffset) : nullptr;
        }

        g_failedThreads.swap(foundFailedThreads);
        return highPrioThreads;
    }

    FARPROC getProcAddress(HMODULE mod, const char* procName)
    {
        FARPROC proc = GetProcAddress(mod, procName);
        if (!proc)
        {
            fatalError(L"Procedure not found: %s", procName);
        }
        return proc;
    }

    void unboostThreads()
    {
        auto suspiciousThreads = getHighPrioThreads();
        for (unsigned i = 0; i < 3 && !suspiciousThreads.empty(); ++i)
        {
            Sleep(100);
            const auto highPrioThreads = getHighPrioThreads();
            auto it = suspiciousThreads.begin();
            while (it != suspiciousThreads.end())
            {
                if (highPrioThreads.find(it->first) != highPrioThreads.end())
                {
                    ++it;
                }
                else
                {
                    it = suspiciousThreads.erase(it);
                }
            }
        }

        auto it = suspiciousThreads.begin();
        while (it != suspiciousThreads.end())
        {
            const auto thread = OpenThread(THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, it->first.tid);
            if (!thread)
            {
                reportThreadError(L"OpenThread failed (0x%X)", it->first, it->second, GetLastError());
                it = suspiciousThreads.erase(it);
                continue;
            }
            it->second.thread = thread;
            ++it;
        }

        for (unsigned i = 0; i < 10 && !suspiciousThreads.empty(); ++i)
        {
            Sleep(10);
            it = suspiciousThreads.begin();
            while (it != suspiciousThreads.end())
            {
                const auto ThreadPriority = static_cast<THREADINFOCLASS>(2);
                ULONG prio = 1;
                NTSTATUS status = g_ntSetInformationThread(it->second.thread, ThreadPriority, &prio, sizeof(prio));
                if (!NT_SUCCESS(status))
                {
                    reportThreadError(L"NtSetInformationThread failed (0x%X)", it->first, it->second, status);
                    it = suspiciousThreads.erase(it);
                    continue;
                }

                const auto ThreadBasicInformation = static_cast<THREADINFOCLASS>(0);
                THREAD_BASIC_INFORMATION tbi = {};
                status = g_ntQueryInformationThread(it->second.thread, ThreadBasicInformation, &tbi, sizeof(tbi), nullptr);
                if (!NT_SUCCESS(status))
                {
                    reportThreadError(L"NtQueryInformationThread failed (0x%X)", it->first, it->second, status);
                    it = suspiciousThreads.erase(it);
                    continue;
                }

                if (tbi.Priority <= THREAD_PRIORITY_TIME_CRITICAL)
                {
                    reportThreadEvent(L"Priority lowered to %u", it->first, it->second, tbi.Priority);
                    CloseHandle(it->second.thread);
                    it = suspiciousThreads.erase(it);
                    continue;
                }

                ++it;
            }
        }

        for (const auto& t : suspiciousThreads)
        {
            reportThreadError(L"Failed to lower priority", t.first, t.second);
        }
    }

    unsigned CALLBACK threadProc(void*)
    {
        boostThreadPrio();
        reportEvent(L"Monitoring started");

        while (true)
        {
            unboostThreads();
            Sleep(1000);
        }
    }

    LRESULT CALLBACK wndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (g_taskBarCreated == message)
        {
            addTrayIcon();
            if (g_isTrayIconCreated && IsIconic(hWnd))
            {
                ShowWindow(hWnd, SW_HIDE);
            }
            return 0;
        }

        switch (message)
        {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_NOTIFYICON:
            if (WM_LBUTTONUP == LOWORD(lParam))
            {
                if (IsWindowVisible(hWnd) && !IsIconic(hWnd))
                {
                    ShowWindow(hWnd, SW_MINIMIZE);
                }
                else
                {
                    ShowWindow(hWnd, SW_RESTORE);
                    SetForegroundWindow(hWnd);
                }
            }
            return 0;

        case WM_SIZE:
        {
            DWORD widths = 0;
            for (unsigned i = 0; i <= 6; ++i)
            {
                if (1 != i)
                {
                    widths += ListView_GetColumnWidth(g_listView, i);
                }
            }
            const int newWidth = LOWORD(lParam) - widths - 30;
            ListView_SetColumnWidth(g_listView, 1, max(newWidth, 300));
            MoveWindow(g_listView, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
            return 0;
        }

        case WM_WINDOWPOSCHANGED:
            if (g_isTrayIconCreated && IsIconic(hWnd))
            {
                ShowWindow(hWnd, SW_HIDE);
            }
            break;
        }

        return DefWindowProc(hWnd, message, wParam, lParam);
    }
}

#define GET_PROC_ADDRESS(mod, proc) reinterpret_cast<decltype(&proc)>(getProcAddress(mod, #proc))

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    const HANDLE mutex = CreateMutex(nullptr, FALSE, L"NvUnboost");
    if (mutex && ERROR_ALREADY_EXISTS == GetLastError())
    {
        fatalError(L"Another instance is already running!");
    }

    const auto ntdll = GetModuleHandle(L"ntdll.dll");
    if (!ntdll)
    {
        fatalError(L"Failed to get module handle for ntdll.dll (0x%X)", GetLastError());
    }

    g_ntQueryInformationThread = GET_PROC_ADDRESS(ntdll, NtQueryInformationThread);
    g_ntQuerySystemInformation = GET_PROC_ADDRESS(ntdll, NtQuerySystemInformation);
    g_ntSetInformationThread = GET_PROC_ADDRESS(ntdll, NtSetInformationThread);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"NvUnboost";
    LoadIconMetric(hInstance, MAKEINTRESOURCE(IDI_NVUNBOOST), LIM_LARGE, &wc.hIcon);
    LoadIconMetric(hInstance, MAKEINTRESOURCE(IDI_NVUNBOOST), LIM_SMALL, &wc.hIconSm);
    if (0 == RegisterClassEx(&wc))
    {
        fatalError(L"Failed to register window class (0x%X)", GetLastError());
    }

    g_mainWindow = CreateWindow(wc.lpszClassName, g_windowName, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);
    if (!g_mainWindow)
    {
        fatalError(L"Failed to create application window (0x%X)", GetLastError());
    }

    g_listView = CreateWindow(WC_LISTVIEW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOSORTHEADER,
        0, 0, 0, 0, g_mainWindow, nullptr, hInstance, nullptr);
    if (!g_listView)
    {
        fatalError(L"Failed to create list view (0x%X)", GetLastError());
    }

    ListView_SetExtendedListViewStyle(g_listView, LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP);

    LVCOLUMN col = {};
    col.mask = LVCF_WIDTH | LVCF_TEXT;
    col.cx = 80;
    col.pszText = const_cast<wchar_t*>(L"Time");
    ListView_InsertColumn(g_listView, COLUMN_ID_TIME, &col);

    col.cx = 300;
    col.pszText = const_cast<wchar_t*>(L"Process path");
    ListView_InsertColumn(g_listView, COLUMN_ID_PROCESS_PATH, &col);

    col.cx = 60;
    col.pszText = const_cast<wchar_t*>(L"PID");
    ListView_InsertColumn(g_listView, COLUMN_ID_PID, &col);

    col.pszText = const_cast<wchar_t*>(L"TID");
    ListView_InsertColumn(g_listView, COLUMN_ID_TID, &col);

    col.pszText = const_cast<wchar_t*>(L"Base prio");
    ListView_InsertColumn(g_listView, COLUMN_ID_BASE_PRIO, &col);

    col.pszText = const_cast<wchar_t*>(L"Dyn prio");
    ListView_InsertColumn(g_listView, COLUMN_ID_DYN_PRIO, &col);

    col.cx = 270;
    col.pszText = const_cast<wchar_t*>(L"Comment");
    ListView_InsertColumn(g_listView, COLUMN_ID_COMMENT, &col);

    g_taskBarCreated = RegisterWindowMessage(L"TaskbarCreated");
    g_trayIcon = wc.hIconSm;
    addTrayIcon();

    ShowWindow(g_mainWindow, nCmdShow);
    UpdateWindow(g_mainWindow);

    if (0 == lstrcmpiW(lpCmdLine, L"/startminimized"))
    {
        ShowWindow(g_mainWindow, SW_MINIMIZE);
    }

    const HANDLE thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, threadProc, nullptr, CREATE_SUSPENDED, nullptr));
    if (!thread)
    {
        fatalError(L"Failed to start monitoring thread (0x%X)", GetLastError());
    }

    boostThreadPrio();
    ResumeThread(thread);
    CloseHandle(thread);

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

#include "launcher_bootstrap.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <intrin.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstddef>
#include <cwchar>
#include <string>
#include <vector>

namespace
{
    constexpr wchar_t ConfigFileName[] = L"Config.ini";
    constexpr wchar_t SectionName[] = L"Launcher";
    constexpr int DefaultPort = 2311;
    constexpr int DefaultWaitSeconds = 20;
    constexpr int PortProbeTimeoutMilliseconds = 200;
    constexpr std::size_t CommandLineCapacity = 1024;
    constexpr std::size_t ValueCapacity = 512;

    // The PEB keeps pointing at this buffer for the whole process lifetime, so
    // it must outlive the call that installs it.
    wchar_t CommandLineBuffer[CommandLineCapacity] = {};
    char CommandLineAnsiBuffer[CommandLineCapacity] = {};

    // Snapshot of the launcher settings, filled in on the loader-lock thread
    // and consumed by the worker that starts the server. DllMain must not do
    // the blocking part itself: Wine keeps the loader lock held for the whole
    // call, and a multi-second CreateProcess + poll starves every other thread
    // (observed as "loader_section wait timed out ... retrying (60 sec)").
    wchar_t WorkerConfigPath[MAX_PATH] = {};
    wchar_t WorkerHost[64] = {};
    int WorkerPort = DefaultPort;
    volatile LONG WorkerStarted = 0;
    volatile LONG WatchdogStarted = 0;
    volatile LONG StartupGateDone = 0;
    int StartupGateSeconds = 12;
    int EntrancePort = DefaultPort;
    int ConnectWaitSeconds = 40;
    volatile LONG ConnectWaits = 0;

    bool BuildClientDirectory(wchar_t* path, DWORD capacity) noexcept;

    void TraceWide(const char* prefix, const wchar_t* text) noexcept;
    void StartHangWatchdog(int seconds) noexcept;

    // OutputDebugStringA raises DBG_PRINTEXCEPTION_C, which our vectored
    // handler sees; without re-entrancy protection logging would recurse
    // until the stack died.
    struct TraceGuard
    {
        static thread_local bool active;
        bool engaged = false;

        TraceGuard() noexcept
        {
            if (!active)
            {
                active = true;
                engaged = true;
            }
        }

        ~TraceGuard() noexcept
        {
            if (engaged)
            {
                active = false;
            }
        }

        explicit operator bool() const noexcept
        {
            return engaged;
        }
    };

    thread_local bool TraceGuard::active = false;

    void Trace(const char* message) noexcept
    {
        TraceGuard guard;
        if (!guard)
        {
            return;
        }

        OutputDebugStringA(message);

        // A file trace is the only way to diagnose this from a plain
        // double-click: the client has no console and DllMain runs before
        // any of its own logging exists.
        wchar_t directory[MAX_PATH] = {};
        if (!BuildClientDirectory(directory, MAX_PATH))
        {
            return;
        }

        std::wstring logPath(directory);
        logPath += L"launcher.log";

        const HANDLE file = CreateFileW(
            logPath.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return;
        }

        SYSTEMTIME now = {};
        GetLocalTime(&now);

        char line[768] = {};
        const int written = wsprintfA(
            line,
            "%04u-%02u-%02u %02u:%02u:%02u %s",
            now.wYear,
            now.wMonth,
            now.wDay,
            now.wHour,
            now.wMinute,
            now.wSecond,
            message);
        if (written > 0)
        {
            int length = written;
            while (length > 0
                   && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            {
                --length;
            }

            line[length] = '\r';
            line[length + 1] = '\n';
            length += 2;

            DWORD bytesWritten = 0;
            WriteFile(
                file,
                line,
                static_cast<DWORD>(length),
                &bytesWritten,
                nullptr);
        }

        CloseHandle(file);
    }

    void TraceWide(const char* prefix, const wchar_t* text) noexcept
    {
        char narrow[512] = {};
        if (text != nullptr
            && WideCharToMultiByte(
                   CP_ACP,
                   0,
                   text,
                   -1,
                   narrow,
                   sizeof(narrow),
                   nullptr,
                   nullptr) <= 0)
        {
            narrow[0] = '\0';
        }

        char line[640] = {};
        if (wsprintfA(line, "%s%s", prefix, narrow) > 0)
        {
            Trace(line);
        }
    }

    // Path of Config.ini beside the client executable, which is also the
    // working directory of the launcher-free layout.
    bool BuildClientDirectory(wchar_t* path, DWORD capacity) noexcept
    {
        const DWORD length = GetModuleFileNameW(nullptr, path, capacity);
        if (length == 0 || length >= capacity)
        {
            return false;
        }

        wchar_t* separator = std::wcsrchr(path, L'\\');
        if (separator == nullptr)
        {
            return false;
        }

        *(separator + 1) = L'\0';
        return true;
    }

    bool BuildConfigPath(wchar_t* path, DWORD capacity) noexcept
    {
        if (!BuildClientDirectory(path, capacity))
        {
            return false;
        }

        const std::size_t directoryLength = std::wcslen(path);
        const std::size_t nameLength = std::wcslen(ConfigFileName);
        if (directoryLength + nameLength + 1 > capacity)
        {
            return false;
        }

        std::wmemcpy(path + directoryLength, ConfigFileName, nameLength + 1);
        return true;
    }

    int ReadInt(const wchar_t* configPath, const wchar_t* key, int fallback) noexcept
    {
        return static_cast<int>(
            GetPrivateProfileIntW(SectionName, key, fallback, configPath));
    }

    void ReadString(
        const wchar_t* configPath,
        const wchar_t* key,
        const wchar_t* fallback,
        wchar_t* buffer,
        DWORD capacity) noexcept
    {
        if (GetPrivateProfileStringW(
                SectionName,
                key,
                fallback,
                buffer,
                capacity,
                configPath) == 0)
        {
            buffer[0] = L'\0';
        }
    }

    // Winsock is initialized once and never cleaned up: the client owns the
    // same refcount and tearing it down here could break its own sockets.
    void EnsureWinsock() noexcept
    {
        static const bool initialized = []
        {
            WSADATA data = {};
            return WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }();
        (void)initialized;
    }

    bool IsPortListening(const wchar_t* host, int port) noexcept
    {
        EnsureWinsock();

        const SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socketHandle == INVALID_SOCKET)
        {
            return false;
        }

        char narrowHost[64] = {};
        if (WideCharToMultiByte(
                CP_ACP,
                0,
                host,
                -1,
                narrowHost,
                sizeof(narrowHost),
                nullptr,
                nullptr) <= 0)
        {
            closesocket(socketHandle);
            return false;
        }

        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<unsigned short>(port));
        if (inet_pton(AF_INET, narrowHost, &address.sin_addr) != 1)
        {
            closesocket(socketHandle);
            return false;
        }

        u_long nonBlocking = 1;
        if (ioctlsocket(socketHandle, FIONBIO, &nonBlocking) != 0)
        {
            closesocket(socketHandle);
            return false;
        }

        bool listening = false;
        if (connect(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
        {
            listening = true;
        }
        else if (WSAGetLastError() == WSAEWOULDBLOCK)
        {
            fd_set writable = {};
            fd_set failed = {};
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(socketHandle, &writable);
            FD_SET(socketHandle, &failed);

            timeval timeout = {};
            timeout.tv_sec = 0;
            timeout.tv_usec = PortProbeTimeoutMilliseconds * 1000;

            const int ready = select(0, nullptr, &writable, &failed, &timeout);
            listening = ready > 0 && FD_ISSET(socketHandle, &writable) != 0;
        }

        closesocket(socketHandle);
        return listening;
    }

    // Kept open for the lifetime of the client process on purpose. Closing it
    // would kill the server immediately; the kernel closes it when the client
    // exits, and that is exactly when the server should stop too.
    HANDLE ServerJob = nullptr;

    // Builds the environment block handed to the server. NativeAOT reads its
    // GC settings from the environment before Main runs, so this is the only
    // place they can be set without touching the server.
    //
    // The x86 build dies under Wine's experimental wow64 while the GC is
    // clearing new heap segments: tens of thousands of write faults on
    // read-only pages, with no forward progress. Workstation GC with a single
    // heap allocates far fewer, smaller segments, which avoids that path.
    std::vector<wchar_t> BuildServerEnvironment(
        const wchar_t* configPath) noexcept
    {
        // Empty by default. Forcing workstation GC with a tiny gen0 made the
        // server die earlier under Wine, not later, so the overrides are opt-in
        // through Config.ini's ServerEnv key.
        constexpr const wchar_t* DefaultOverrides = L"";

        wchar_t raw[512] = {};
        ReadString(configPath, L"ServerEnv", DefaultOverrides, raw, 512);

        std::vector<std::wstring> overrides;
        {
            std::wstring current;
            for (const wchar_t* cursor = raw; ; ++cursor)
            {
                if (*cursor == L'\0' || *cursor == L';')
                {
                    if (!current.empty())
                    {
                        overrides.push_back(current);
                        current.clear();
                    }
                    if (*cursor == L'\0')
                    {
                        break;
                    }
                    continue;
                }
                current.push_back(*cursor);
            }
        }

        std::vector<std::wstring> entries;
        if (LPWCH block = GetEnvironmentStringsW())
        {
            for (const wchar_t* cursor = block; *cursor != L'\0'; )
            {
                std::wstring entry(cursor);
                cursor += entry.size() + 1;

                bool replaced = false;
                for (const std::wstring& override : overrides)
                {
                    const std::size_t equals = override.find(L'=');
                    if (equals == std::wstring::npos)
                    {
                        continue;
                    }
                    if (entry.size() > equals
                        && _wcsnicmp(
                               entry.c_str(),
                               override.c_str(),
                               equals) == 0
                        && entry[equals] == L'=')
                    {
                        replaced = true;
                        break;
                    }
                }

                if (!replaced)
                {
                    entries.push_back(std::move(entry));
                }
            }

            FreeEnvironmentStringsW(block);
        }

        for (const std::wstring& override : overrides)
        {
            if (override.find(L'=') != std::wstring::npos)
            {
                entries.push_back(override);
            }
        }

        std::vector<wchar_t> result;
        for (const std::wstring& entry : entries)
        {
            result.insert(result.end(), entry.begin(), entry.end());
            result.push_back(L'\0');
        }
        result.push_back(L'\0');

        char line[320] = {};
        if (wsprintfA(
                line,
                "server env: %d override(s), block %u wchar\n",
                static_cast<int>(overrides.size()),
                static_cast<unsigned>(result.size())) > 0)
        {
            Trace(line);
        }

        return result;
    }

    bool StartServer(
        const wchar_t* serverPath,
        bool killOnClientExit,
        const wchar_t* configPath) noexcept
    {
        std::wstring commandLine = L"\"";
        commandLine += serverPath;
        commandLine += L"\"";

        std::wstring workingDirectory(serverPath);
        const std::size_t separator = workingDirectory.find_last_of(L'\\');
        if (separator != std::wstring::npos)
        {
            workingDirectory.erase(separator);
        }

        // A job with KILL_ON_JOB_CLOSE is the only reliable way to stop the
        // server when the client dies: the DLL cannot run code after its own
        // process is gone, but the kernel closes this handle for us.
        HANDLE job = nullptr;
        if (killOnClientExit)
        {
            job = CreateJobObjectW(nullptr, nullptr);
            if (job != nullptr)
            {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
                limits.BasicLimitInformation.LimitFlags =
                    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (SetInformationJobObject(
                        job,
                        JobObjectExtendedLimitInformation,
                        &limits,
                        sizeof(limits)) == FALSE)
                {
                    CloseHandle(job);
                    job = nullptr;
                }
            }

            if (job == nullptr)
            {
                Trace("DFLegacy.Ijl15: could not create the server exit job.\n");
            }
        }

        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;

        // The server runs without a console, so its log would be lost. Send it
        // to server.log beside the executable instead of discarding it.
        HANDLE logHandle = INVALID_HANDLE_VALUE;
        BOOL inheritHandles = FALSE;
        if (!workingDirectory.empty())
        {
            std::wstring logPath = workingDirectory + L"\\server.log";
            logHandle = CreateFileW(
                logPath.c_str(),
                FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (logHandle != INVALID_HANDLE_VALUE)
            {
                SetHandleInformation(
                    logHandle,
                    HANDLE_FLAG_INHERIT,
                    HANDLE_FLAG_INHERIT);
                startup.dwFlags |= STARTF_USESTDHANDLES;
                startup.hStdOutput = logHandle;
                startup.hStdError = logHandle;
                startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
                inheritHandles = TRUE;
            }
        }

        std::vector<wchar_t> environment =
            BuildServerEnvironment(configPath);

        PROCESS_INFORMATION process = {};
        const BOOL created = CreateProcessW(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            inheritHandles,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
            environment.empty() ? nullptr : environment.data(),
            workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
            &startup,
            &process);

        if (logHandle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(logHandle);
        }

        if (created == FALSE)
        {
            if (job != nullptr)
            {
                CloseHandle(job);
            }

            return false;
        }

        CloseHandle(process.hThread);

        if (job != nullptr)
        {
            if (AssignProcessToJobObject(job, process.hProcess))
            {
                ServerJob = job;
            }
            else
            {
                Trace("DFLegacy.Ijl15: server started but is not tied to the client lifetime.\n");
                CloseHandle(job);
            }
        }

        CloseHandle(process.hProcess);
        return true;
    }

    // Replaces the PEB command line so the client parses our arguments. The
    // buffer lives in this module, which stays mapped for the whole process.
    bool PatchCommandLine(const wchar_t* text) noexcept
    {
        const std::size_t length = std::wcslen(text);
        if (length + 1 >= CommandLineCapacity)
        {
            return false;
        }

        std::wmemcpy(CommandLineBuffer, text, length + 1);

        auto* peb = reinterpret_cast<PEB*>(__readfsdword(0x30));
        if (peb == nullptr || peb->ProcessParameters == nullptr)
        {
            return false;
        }

        UNICODE_STRING& commandLine = peb->ProcessParameters->CommandLine;
        commandLine.Buffer = CommandLineBuffer;
        commandLine.Length = static_cast<USHORT>(length * sizeof(wchar_t));
        commandLine.MaximumLength =
            static_cast<USHORT>((length + 1) * sizeof(wchar_t));
        return true;
    }

    // Re-reads the command line a few seconds after startup. If the client or
    // another module restores the original text, the trace shows it.
    DWORD WINAPI DeferredCommandLineCheck(LPVOID) noexcept
    {
        Sleep(3000);
        TraceWide("delayed GetCommandLineW: ", GetCommandLineW());
        return 0;
    }

    // The CRT caches the command line the first time it is read. ijl15.dll is
    // twentieth in the import list, so an earlier module may already have
    // filled that cache with the original text; repoint it at our copy.
    void PatchCrtCommandLine(const wchar_t* wide, const char* ansi) noexcept
    {
        HMODULE crt = GetModuleHandleA("msvcr71.dll");
        if (crt == nullptr)
        {
            crt = GetModuleHandleA("msvcr80.dll");
        }
        if (crt == nullptr)
        {
            crt = GetModuleHandleA("msvcrt.dll");
        }
        if (crt == nullptr)
        {
            crt = GetModuleHandleA("ucrtbase.dll");
        }
        if (crt == nullptr)
        {
            Trace("DFLegacy.Ijl15: no CRT module found for the command line cache.\n");
            return;
        }

        using WideSlotAccessor = wchar_t** (__cdecl*)();
        using AnsiSlotAccessor = char** (__cdecl*)();

        bool patchedWide = false;
        bool patchedAnsi = false;

        if (auto accessor =
                reinterpret_cast<WideSlotAccessor>(GetProcAddress(crt, "__p__wcmdln")))
        {
            if (wchar_t** slot = accessor())
            {
                *slot = const_cast<wchar_t*>(wide);
                patchedWide = true;
            }
        }

        if (auto accessor =
                reinterpret_cast<AnsiSlotAccessor>(GetProcAddress(crt, "__p__acmdln")))
        {
            if (char** slot = accessor())
            {
                *slot = const_cast<char*>(ansi);
                patchedAnsi = true;
            }
        }

        char line[256] = {};
        if (wsprintfA(
                line,
                "CRT cache patched: wide=%d ansi=%d\n",
                patchedWide ? 1 : 0,
                patchedAnsi ? 1 : 0) > 0)
        {
            Trace(line);
        }
    }

    // kernel32 caches the command line pointer on first use, so rewriting the
    // PEB alone is not enough: the client keeps seeing the original text.
    // Redirecting the client's own import slots is what actually takes effect.
    LPWSTR WINAPI HookGetCommandLineW() noexcept
    {
        return CommandLineBuffer;
    }

    LPSTR WINAPI HookGetCommandLineA() noexcept
    {
        if (InterlockedCompareExchange(&StartupGateDone, 1, 0) == 0)
        {
            // The client's CRT init reaches here with the loader lock already
            // released, which makes this the safe place to stall.
            //
            // Stalling is not cosmetic: with a blocking wait during startup the
            // client gets all the way to a successful connect, while letting it
            // run straight through makes it fault on a null pointer about three
            // seconds in, before it ever opens a socket. Something in its own
            // start-up races, and the delay changes the outcome.
            if (StartupGateSeconds > 0)
            {
                wchar_t host[64] = {};
                ReadString(
                    WorkerConfigPath,
                    L"Host",
                    L"127.0.0.1",
                    host,
                    64);

                const int attempts = StartupGateSeconds * 5;
                int waited = 0;
                for (int attempt = 0; attempt < attempts; ++attempt)
                {
                    if (IsPortListening(host, EntrancePort))
                    {
                        break;
                    }
                    Sleep(200);
                    waited += 200;
                }

                char line[192] = {};
                wsprintfA(
                    line,
                    "startup gate: waited %d ms for port %d\n",
                    waited,
                    EntrancePort);
                Trace(line);
            }

            StartHangWatchdog(15);
        }

        return CommandLineAnsiBuffer;
    }

    bool PatchImportSlot(void** slot, void* replacement) noexcept
    {
        DWORD previous = 0;
        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous) == FALSE)
        {
            return false;
        }

        *slot = replacement;

        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(void*), previous, &ignored);
        return true;
    }

    // ---------------------------------------------------------------
    // Page protection shim.
    //
    // The client carries one section (nsn 0, 638 KB) that is decrypted on
    // demand and re-encrypted as soon as the code is done with it. That
    // dance is implemented as:
    //
    //     VirtualProtect(page, PAGE_READWRITE)   -> write plaintext
    //     ... run ...
    //     VirtualProtect(page, PAGE_READONLY)    -> write ciphertext back
    //
    // Under Wine's experimental wow64 the protection change does not always
    // take effect the way it does on Windows, so the write-back lands on a
    // page the host still considers read-only. That is exactly the crash we
    // see on Winlator: ERR=7 (write, protection violation), prot=1, with
    // rep stosb advancing one 0x1000 page at a time.
    //
    // Refusing to lower the protection removes the failure mode entirely:
    // every page stays writable, so both the decrypt and the re-encrypt
    // write succeed. Windows is unaffected because nothing there depends on
    // a page actually becoming read-only.
    // ---------------------------------------------------------------
    using VirtualProtectFn = BOOL (WINAPI*)(LPVOID, SIZE_T, DWORD, PDWORD);

    bool AllowPageDowngrade = false;
    VirtualProtectFn RealVirtualProtect = nullptr;
    volatile LONG DowngradeBlocked = 0;
    volatile LONG ProtectCallsLogged = 0;

    bool RequestsWriteAccess(DWORD protection) noexcept
    {
        const DWORD base = protection & 0xFF;
        return base == PAGE_READWRITE
            || base == PAGE_WRITECOPY
            || base == PAGE_EXECUTE_READWRITE
            || base == PAGE_EXECUTE_WRITECOPY;
    }

    BOOL WINAPI HookVirtualProtect(
        LPVOID address,
        SIZE_T size,
        DWORD newProtect,
        PDWORD oldProtect) noexcept
    {
        if (RealVirtualProtect == nullptr)
        {
            return FALSE;
        }

        const LONG call = InterlockedIncrement(&ProtectCallsLogged);
        if (call <= 32)
        {
            char line[192] = {};
            if (wsprintfA(
                    line,
                    "VirtualProtect #%d %p size=0x%X prot=0x%X%s\n",
                    static_cast<int>(call),
                    address,
                    static_cast<unsigned>(size),
                    static_cast<unsigned>(newProtect),
                    RequestsWriteAccess(newProtect) ? "" : " [non-writable]") > 0)
            {
                Trace(line);
            }
        }

        // A guard page is a deliberate trap; leave those alone.
        if (!AllowPageDowngrade
            && (newProtect & PAGE_GUARD) == 0
            && !RequestsWriteAccess(newProtect))
        {
            const DWORD modifiers = newProtect & (PAGE_NOCACHE | PAGE_WRITECOMBINE);
            const LONG seen = InterlockedIncrement(&DowngradeBlocked);
            if (seen <= 8)
            {
                char line[192] = {};
                if (wsprintfA(
                        line,
                        "blocked page downgrade #%d: %p size=0x%X want=0x%X -> RWX\n",
                        static_cast<int>(seen),
                        address,
                        static_cast<unsigned>(size),
                        static_cast<unsigned>(newProtect)) > 0)
                {
                    Trace(line);
                }
            }

            return RealVirtualProtect(
                address,
                size,
                PAGE_EXECUTE_READWRITE | modifiers,
                oldProtect);
        }

        return RealVirtualProtect(address, size, newProtect, oldProtect);
    }

    // Redirects every import slot of the main module that currently points at
    // kernel32!VirtualProtect. The client and its protector both reach the API
    // through this table, so one pass covers them.
    int InstallProtectionShim() noexcept
    {
        auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        if (base == nullptr)
        {
            return 0;
        }

        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        {
            return 0;
        }

        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
        {
            return 0;
        }

        const auto& directory =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (directory.VirtualAddress == 0)
        {
            return 0;
        }

        HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
        if (kernel32 == nullptr)
        {
            return 0;
        }

        auto* target = reinterpret_cast<void*>(
            GetProcAddress(kernel32, "VirtualProtect"));
        if (target == nullptr)
        {
            return 0;
        }

        RealVirtualProtect = reinterpret_cast<VirtualProtectFn>(target);

        int patched = 0;
        auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
            base + directory.VirtualAddress);
        for (; descriptor->Name != 0; ++descriptor)
        {
            auto* thunk =
                reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
            for (; thunk->u1.Function != 0; ++thunk)
            {
                auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
                if (*slot != target)
                {
                    continue;
                }

                if (PatchImportSlot(
                        slot,
                        reinterpret_cast<void*>(&HookVirtualProtect)))
                {
                    ++patched;
                }
            }
        }

        return patched;
    }

    // ---------------------------------------------------------------
    // Access-violation shim.
    //
    // The client maps some of its data with a writable view on Windows, then
    // writes into it from several threads. On Winlator the same view ends up
    // read-only, so those writes raise EXCEPTION_ACCESS_VIOLATION and the
    // process dies before Direct3D is ever initialised (black window).
    //
    // The kernel32!VirtualProtect hook above is not enough: the client
    // resolves MapViewOfFile dynamically, so there is no import slot to
    // redirect. Instead we take the fault itself, make the page writable and
    // resume. This works no matter which API produced the read-only page.
    // ---------------------------------------------------------------
    PVOID AccessViolationHandle = nullptr;
    volatile LONG AccessViolationsFixed = 0;
    volatile LONG ExceptionsObserved = 0;

    // Turns an address into "module+0xoffset" so a stack trace can be read
    // without symbols.
    bool IsExecutableProtection(DWORD protect) noexcept
    {
        const DWORD base = protect & 0xFF;
        return base == PAGE_EXECUTE
            || base == PAGE_EXECUTE_READ
            || base == PAGE_EXECUTE_READWRITE
            || base == PAGE_EXECUTE_WRITECOPY;
    }

    // Returns true only for addresses that live in an executable page of a
    // loaded module: a plain "inside a module" test also matches pointers to
    // string and table data, which is what polluted the first attempt.
    bool DescribeAddress(DWORD address, char* buffer, int capacity) noexcept
    {
        if (buffer == nullptr || capacity <= 0)
        {
            return false;
        }
        buffer[0] = '\0';

        if (address == 0)
        {
            wsprintfA(buffer, "0");
            return false;
        }

        MEMORY_BASIC_INFORMATION region = {};
        if (VirtualQuery(
                reinterpret_cast<void*>(address),
                &region,
                sizeof(region)) == 0
            || region.AllocationBase == nullptr
            || !IsExecutableProtection(region.Protect))
        {
            wsprintfA(buffer, "%08X", static_cast<unsigned>(address));
            return false;
        }

        char path[MAX_PATH] = {};
        if (GetModuleFileNameA(
                reinterpret_cast<HMODULE>(region.AllocationBase),
                path,
                MAX_PATH) > 0)
        {
            const char* name = path;
            for (const char* cursor = path; *cursor != '\0'; ++cursor)
            {
                if (*cursor == '\\' || *cursor == '/')
                {
                    name = cursor + 1;
                }
            }

            wsprintfA(
                buffer,
                "%s+0x%X",
                name,
                static_cast<unsigned>(
                    address
                    - static_cast<DWORD>(
                        reinterpret_cast<DWORD_PTR>(region.AllocationBase))));
            return true;
        }

        wsprintfA(buffer, "%08X", static_cast<unsigned>(address));
        return false;
    }

    // Dumps the first few words of the faulting thread's stack. On a jump to a
    // bogus address the top of the stack still holds the caller's return
    // address, which is what actually identifies the bug.
    // Scans a suspended thread's stack for executable addresses and writes
    // them as module+offset. Works for any thread, not just the faulting one.
    void TraceThreadFrames(
        DWORD stackPointer,
        DWORD instructionPointer,
        char* out,
        int capacity) noexcept
    {
        if (out == nullptr || capacity <= 0)
        {
            return;
        }

        int written = 0;
        char self[96] = {};
        DescribeAddress(instructionPointer, self, 96);
        written = wsprintfA(out, "eip=%s", self);

        auto* stack = reinterpret_cast<const DWORD*>(stackPointer);
        MEMORY_BASIC_INFORMATION region = {};
        if (stack == nullptr
            || VirtualQuery(
                   const_cast<DWORD*>(stack),
                   &region,
                   sizeof(region)) == 0
            || region.State != MEM_COMMIT
            || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
        {
            return;
        }

        constexpr int ScanWords = 192;
        int found = 0;
        for (int index = 0; index < ScanWords && found < 6; ++index)
        {
            char text[96] = {};
            if (!DescribeAddress(stack[index], text, 96))
            {
                continue;
            }
            if (written < 0 || written > capacity - 100)
            {
                break;
            }
            written += wsprintfA(out + written, " %s", text);
            ++found;
        }
    }

    void TraceStack(EXCEPTION_POINTERS* info) noexcept
    {
        if (info == nullptr || info->ContextRecord == nullptr)
        {
            return;
        }

        auto* context = info->ContextRecord;

        char registers[320] = {};
        wsprintfA(
            registers,
            "  regs: eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X "
            "ebp=%08X esp=%08X eip=%08X\n",
            static_cast<unsigned>(context->Eax),
            static_cast<unsigned>(context->Ebx),
            static_cast<unsigned>(context->Ecx),
            static_cast<unsigned>(context->Edx),
            static_cast<unsigned>(context->Esi),
            static_cast<unsigned>(context->Edi),
            static_cast<unsigned>(context->Ebp),
            static_cast<unsigned>(context->Esp),
            static_cast<unsigned>(context->Eip));
        Trace(registers);

        auto* stack = reinterpret_cast<const DWORD*>(context->Esp);
        if (stack == nullptr)
        {
            return;
        }

        // The fault may be a jump rather than a call, so the immediate return
        // address is not necessarily at [esp]. Scan the stack and keep only the
        // words that land inside a module: those are the real code addresses.
        constexpr int ScanWords = 192;
        MEMORY_BASIC_INFORMATION region = {};
        if (VirtualQuery(
                const_cast<DWORD*>(stack),
                &region,
                sizeof(region)) == 0
            || region.State != MEM_COMMIT
            || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
        {
            return;
        }

        char raw[640] = {};
        int rawWritten = wsprintfA(raw, "  raw stack:");
        for (int index = 0; index < 12; ++index)
        {
            if (rawWritten < 0 || rawWritten > 560)
            {
                break;
            }
            rawWritten += wsprintfA(
                raw + rawWritten,
                " %08X",
                static_cast<unsigned>(stack[index]));
        }
        if (rawWritten > 0)
        {
            wsprintfA(raw + rawWritten, "\n");
            Trace(raw);
        }

        char line[900] = {};
        int written = wsprintfA(line, "  code frames:");
        int found = 0;
        for (int index = 0; index < ScanWords && found < 8; ++index)
        {
            char text[96] = {};
            if (!DescribeAddress(stack[index], text, 96))
            {
                continue;
            }
            if (written < 0 || written > 760)
            {
                break;
            }
            written += wsprintfA(line + written, " %s", text);
            ++found;
        }

        if (found == 0)
        {
            wsprintfA(line + written, " (none)");
            written += 7;
        }

        if (written > 0)
        {
            wsprintfA(line + written, "\n");
            Trace(line);
        }
    }

    LONG CALLBACK FixAccessViolation(EXCEPTION_POINTERS* info) noexcept
    {
        if (info == nullptr || info->ExceptionRecord == nullptr)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        auto* record = info->ExceptionRecord;

        // Debug prints are not failures; ignoring them also keeps the handler
        // from reacting to our own OutputDebugStringA calls.
        if (record->ExceptionCode == 0x40010006
            || record->ExceptionCode == 0x4001000A)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // Log every exception we are shown, whatever the kind: BugTrap eats the
        // fatal one, so this trace is the only way to see what the client
        // actually raised before it wrote the minidump.
        const LONG observed = InterlockedIncrement(&ExceptionsObserved);
        if (observed <= 24)
        {
            char line[224] = {};
            const char* kind = "other";
            if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
            {
                kind = "access-violation";
            }
            else if (record->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION)
            {
                kind = "illegal-instruction";
            }
            else if (record->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO)
            {
                kind = "divide-by-zero";
            }
            else if (record->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
            {
                kind = "stack-overflow";
            }
            else if (record->ExceptionCode == 0xE06D7363)
            {
                kind = "cpp-exception";
            }
            else if (record->ExceptionCode == 0xC0000409)
            {
                kind = "stack-buffer-overrun";
            }
            else if (record->ExceptionCode == 0x80000003)
            {
                kind = "breakpoint";
            }

            if (wsprintfA(
                    line,
                    "exception #%d %s code=0x%08X at %p mode=0x%X addr=0x%X\n",
                    static_cast<int>(observed),
                    kind,
                    static_cast<unsigned>(record->ExceptionCode),
                    record->ExceptionAddress,
                    record->NumberParameters > 0
                        ? static_cast<unsigned>(record->ExceptionInformation[0])
                        : 0u,
                    record->NumberParameters > 1
                        ? static_cast<unsigned>(record->ExceptionInformation[1])
                        : 0u) > 0)
            {
                Trace(line);
            }

            TraceStack(info);
        }

        if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION
            || record->NumberParameters < 2)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // [0] == 0 is a read, which we must not paper over.
        const ULONG_PTR mode = record->ExceptionInformation[0];
        if (mode != 1 && mode != 8)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        void* address = reinterpret_cast<void*>(record->ExceptionInformation[1]);
        MEMORY_BASIC_INFORMATION region = {};
        if (VirtualQuery(address, &region, sizeof(region)) != sizeof(region))
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // Only committed, non-guard pages: a guard page is a deliberate trap.
        if (region.State != MEM_COMMIT || (region.Protect & PAGE_GUARD) != 0)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        DWORD previous = 0;
        if (RealVirtualProtect == nullptr
            || RealVirtualProtect(
                   region.BaseAddress,
                   region.RegionSize,
                   PAGE_EXECUTE_READWRITE,
                   &previous) == FALSE)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        const LONG seen = InterlockedIncrement(&AccessViolationsFixed);
        if (seen <= 16)
        {
            char line[192] = {};
            if (wsprintfA(
                    line,
                    "write fault fixed #%d at %p (region %p size=0x%X was 0x%X)\n",
                    static_cast<int>(seen),
                    address,
                    region.BaseAddress,
                    static_cast<unsigned>(region.RegionSize),
                    static_cast<unsigned>(previous)) > 0)
            {
                Trace(line);
            }
        }

        return EXCEPTION_CONTINUE_EXECUTION;
    }

    void InstallAccessViolationShim() noexcept
    {
        if (AccessViolationHandle != nullptr)
        {
            return;
        }

        AccessViolationHandle = AddVectoredExceptionHandler(1, FixAccessViolation);
        Trace(AccessViolationHandle != nullptr
                  ? "access violation shim installed.\n"
                  : "access violation shim could not be installed.\n");
    }

    // ---------------------------------------------------------------
    // RS.dll reporting stub.
    //
    // RS.dll is the 2008 ranking/report module. Its only ole32 import is
    // CoCreateInstance, and the class it asks for is registered by the retail
    // installer -- under Wine that lookup fails, RS.dll gets a null interface
    // and calls a virtual method through it. The client dies on the resulting
    // jump to address 0x8 before Direct3D is ever initialised.
    //
    // Reporting is non-essential: on Windows it already fails silently
    // (rs_log.txt shows "failed at 1") and the game does not care. Turning the
    // two exports into no-ops removes the null dereference entirely.
    // ---------------------------------------------------------------
    int StubModuleExports(const char* moduleName) noexcept
    {
        HMODULE module = GetModuleHandleA(moduleName);
        if (module == nullptr)
        {
            module = LoadLibraryA(moduleName);
        }
        if (module == nullptr)
        {
            return -1;
        }

        auto* base = reinterpret_cast<unsigned char*>(module);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        {
            return 0;
        }

        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
        {
            return 0;
        }

        const auto& directory =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (directory.VirtualAddress == 0 || directory.Size == 0)
        {
            return 0;
        }

        auto* exports = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(
            base + directory.VirtualAddress);
        auto* functions =
            reinterpret_cast<DWORD*>(base + exports->AddressOfFunctions);

        int patched = 0;
        for (DWORD index = 0; index < exports->NumberOfFunctions; ++index)
        {
            const DWORD rva = functions[index];
            if (rva == 0)
            {
                continue;
            }

            // Forwarded exports point back into the export directory.
            if (rva >= directory.VirtualAddress
                && rva < directory.VirtualAddress + directory.Size)
            {
                continue;
            }

            unsigned char* target = base + rva;
            DWORD previous = 0;
            if (VirtualProtect(target, 8, PAGE_EXECUTE_READWRITE, &previous) == FALSE)
            {
                continue;
            }

            // mov eax, 1 ; ret -> returns TRUE and does nothing.
            // Returning TRUE matters for BT_InstallSehFilter, which reports
            // success/failure to the caller.
            target[0] = 0xB8;
            target[1] = 0x01;
            target[2] = 0x00;
            target[3] = 0x00;
            target[4] = 0x00;
            target[5] = 0xC3;
            FlushInstructionCache(GetCurrentProcess(), target, 6);

            DWORD ignored = 0;
            VirtualProtect(target, 8, previous, &ignored);
            ++patched;
        }

        return patched;
    }

    // ---------------------------------------------------------------
    // Hang watchdog.
    //
    // The client does not always die under Wine: it can end up alive with a
    // black window while one thread holds a critical section and everyone else
    // waits on it. A crash dump is useless there, so after a delay we sample
    // every thread's instruction pointer and write it to the trace. That shows
    // exactly which call the stuck thread is sitting in.
    // ---------------------------------------------------------------
    DWORD WINAPI HangWatchdog(LPVOID parameter) noexcept
    {
        const int seconds = static_cast<int>(
            reinterpret_cast<INT_PTR>(parameter));
        if (seconds <= 0)
        {
            return 0;
        }

        Sleep(static_cast<DWORD>(seconds) * 1000u);

        Trace("watchdog: sampling thread instruction pointers.\n");

        const DWORD processId = GetCurrentProcessId();
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            Trace("watchdog: thread snapshot failed.\n");
            return 0;
        }

        THREADENTRY32 entry = {};
        entry.dwSize = sizeof(entry);

        int sampled = 0;
        if (Thread32First(snapshot, &entry))
        {
            do
            {
                if (entry.th32OwnerProcessID != processId)
                {
                    continue;
                }
                if (sampled >= 20)
                {
                    break;
                }

                HANDLE thread = OpenThread(
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                    FALSE,
                    entry.th32ThreadID);
                if (thread == nullptr)
                {
                    continue;
                }

                if (SuspendThread(thread) == static_cast<DWORD>(-1))
                {
                    CloseHandle(thread);
                    continue;
                }

                CONTEXT context = {};
                context.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(thread, &context))
                {
                    char where[96] = {};
                    DescribeAddress(
                        static_cast<DWORD>(context.Eip),
                        where,
                        96);

                    char line[224] = {};
                    wsprintfA(
                        line,
                        "watchdog: tid=%u eip=%08X (%s) esp=%08X\n",
                        static_cast<unsigned>(entry.th32ThreadID),
                        static_cast<unsigned>(context.Eip),
                        where,
                        static_cast<unsigned>(context.Esp));
                    Trace(line);

                    char frames[560] = {};
                    TraceThreadFrames(
                        static_cast<DWORD>(context.Esp),
                        static_cast<DWORD>(context.Eip),
                        frames,
                        560);
                    if (frames[0] != '\0')
                    {
                        char second[620] = {};
                        wsprintfA(second, "watchdog:   frames %s\n", frames);
                        Trace(second);
                    }

                    ++sampled;
                }

                ResumeThread(thread);
                CloseHandle(thread);
            }
            while (Thread32Next(snapshot, &entry));
        }

        CloseHandle(snapshot);

        char summary[128] = {};
        wsprintfA(summary, "watchdog: %d thread(s) sampled.\n", sampled);
        Trace(summary);
        return 0;
    }

    void StartHangWatchdog(int seconds) noexcept
    {
        if (seconds <= 0)
        {
            return;
        }

        if (HANDLE thread = CreateThread(
                nullptr,
                0,
                HangWatchdog,
                reinterpret_cast<LPVOID>(static_cast<INT_PTR>(seconds)),
                0,
                nullptr))
        {
            CloseHandle(thread);
        }
    }

    // ---------------------------------------------------------------
    // Entrance connect gate.
    //
    // The client connects to 127.0.0.1:2311 within a couple of seconds of
    // starting. On a phone the server needs about nine seconds to index
    // Script.pvf and open its listeners, so the connect is refused, the client
    // takes its error path and ends up hung behind a black window. On Windows
    // the server is ready in under two seconds, which is why the same build
    // works there.
    //
    // Instead of letting that first connect fail we hold it until the port is
    // listening. Delaying is safe; retrying connect() on the same socket after
    // a refusal is not.
    // ---------------------------------------------------------------
    using ConnectFn = int (WSAAPI*)(SOCKET, const struct sockaddr*, int);
    ConnectFn RealConnect = nullptr;

    int WSAAPI HookConnect(
        SOCKET socket,
        const struct sockaddr* name,
        int namelen) noexcept
    {
        if (RealConnect == nullptr)
        {
            return SOCKET_ERROR;
        }

        if (name != nullptr
            && name->sa_family == AF_INET
            && ConnectWaitSeconds > 0)
        {
            const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(name);
            const bool loopback =
                ipv4->sin_addr.s_addr == htonl(INADDR_LOOPBACK);
            const int port = ntohs(ipv4->sin_port);

            if (loopback && port == EntrancePort)
            {
                wchar_t host[64] = {};
                ReadString(
                    WorkerConfigPath,
                    L"Host",
                    L"127.0.0.1",
                    host,
                    64);

                const int attempts = ConnectWaitSeconds * 5;
                for (int attempt = 0; attempt < attempts; ++attempt)
                {
                    if (IsPortListening(host, EntrancePort))
                    {
                        if (attempt > 0)
                        {
                            char line[160] = {};
                            wsprintfA(
                                line,
                                "connect gate: waited %d ms for port %d\n",
                                attempt * 200,
                                EntrancePort);
                            Trace(line);
                        }
                        break;
                    }

                    Sleep(200);
                }

                const LONG waits = InterlockedIncrement(&ConnectWaits);
                if (waits <= 4)
                {
                    char line[160] = {};
                    wsprintfA(
                        line,
                        "connect gate: entry %d for port %d\n",
                        static_cast<int>(waits),
                        EntrancePort);
                    Trace(line);
                }
            }
        }

        return RealConnect(socket, name, namelen);
    }

    int InstallConnectHook() noexcept
    {
        auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        if (base == nullptr)
        {
            return 0;
        }

        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        {
            return 0;
        }

        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
        {
            return 0;
        }

        const auto& directory =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (directory.VirtualAddress == 0)
        {
            return 0;
        }

        HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
        if (ws2 == nullptr)
        {
            return 0;
        }

        auto* target = reinterpret_cast<void*>(
            GetProcAddress(ws2, "connect"));
        if (target == nullptr)
        {
            return 0;
        }

        RealConnect = reinterpret_cast<ConnectFn>(target);

        int patched = 0;
        auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
            base + directory.VirtualAddress);
        for (; descriptor->Name != 0; ++descriptor)
        {
            auto* thunk =
                reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
            for (; thunk->u1.Function != 0; ++thunk)
            {
                auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
                if (*slot != target)
                {
                    continue;
                }

                if (PatchImportSlot(slot, reinterpret_cast<void*>(&HookConnect)))
                {
                    ++patched;
                }
            }
        }

        return patched;
    }

    // ---------------------------------------------------------------
    // Loader failure probe.
    //
    // The crash signature is a call through a null object at offset 8, with an
    // ImagePacks2 path on the stack, which is what a missing module or export
    // looks like from the client's side: it gets null back and keeps going.
    // Logging every failed LoadLibraryA / GetProcAddress names the culprit.
    // ---------------------------------------------------------------
    using LoadLibraryAFn = HMODULE (WINAPI*)(LPCSTR);
    using GetProcAddressFn = FARPROC (WINAPI*)(HMODULE, LPCSTR);

    LoadLibraryAFn RealLoadLibraryA = nullptr;
    GetProcAddressFn RealGetProcAddress = nullptr;
    volatile LONG LoaderFailuresLogged = 0;

    HMODULE WINAPI HookLoadLibraryA(LPCSTR name) noexcept
    {
        HMODULE module = RealLoadLibraryA != nullptr
            ? RealLoadLibraryA(name)
            : nullptr;

        if (module == nullptr)
        {
            const LONG seen = InterlockedIncrement(&LoaderFailuresLogged);
            if (seen <= 24)
            {
                char line[288] = {};
                wsprintfA(
                    line,
                    "LoadLibraryA failed: %s\n",
                    name != nullptr ? name : "(null)");
                Trace(line);
            }
        }

        return module;
    }

    FARPROC WINAPI HookGetProcAddress(HMODULE module, LPCSTR name) noexcept
    {
        FARPROC address = RealGetProcAddress != nullptr
            ? RealGetProcAddress(module, name)
            : nullptr;

        if (name != nullptr)
        {
            // Watchlist: these are the APIs that reach nsi.dll, which is where
            // the client's main thread gets stuck. Knowing which one it asks
            // for is the whole point.
            static const char* const watched[] = {
                "GetAdaptersAddresses",
                "GetAdaptersInfo",
                "GetNetworkParams",
                "GetIpAddrTable",
                "GetTcpTable",
                "NotifyAddrChange",
                "NotifyUnicastIpAddressChange",
                "WSAIoctl",
                "getaddrinfo",
                "GetHostNameW",
                "GetComputerNameA",
                "GetComputerNameExA"
            };

            for (const char* candidate : watched)
            {
                if (_stricmp(name, candidate) != 0)
                {
                    continue;
                }

                char line[288] = {};
                wsprintfA(
                    line,
                    "resolved %s -> %p\n",
                    name,
                    address);
                Trace(line);
                break;
            }
        }

        if (address == nullptr)
        {
            const LONG seen = InterlockedIncrement(&LoaderFailuresLogged);
            if (seen <= 24)
            {
                char line[288] = {};
                wsprintfA(
                    line,
                    "GetProcAddress failed: %s (module %p)\n",
                    name != nullptr ? name : "(null)",
                    module);
                Trace(line);
            }
        }

        return address;
    }

    // Redirects every import slot of the main module that points at
    // moduleName!functionName. Returns the original address, or null.
    void* HookMainModuleImport(
        const char* moduleName,
        const char* functionName,
        void* replacement) noexcept
    {
        auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        if (base == nullptr)
        {
            return nullptr;
        }

        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        {
            return nullptr;
        }

        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
        {
            return nullptr;
        }

        const auto& directory =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (directory.VirtualAddress == 0)
        {
            return nullptr;
        }

        HMODULE provider = GetModuleHandleA(moduleName);
        if (provider == nullptr)
        {
            return nullptr;
        }

        auto* target = reinterpret_cast<void*>(
            GetProcAddress(provider, functionName));
        if (target == nullptr)
        {
            return nullptr;
        }

        auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
            base + directory.VirtualAddress);
        for (; descriptor->Name != 0; ++descriptor)
        {
            auto* thunk =
                reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
            for (; thunk->u1.Function != 0; ++thunk)
            {
                auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
                if (*slot == target)
                {
                    PatchImportSlot(slot, replacement);
                }
            }
        }

        return target;
    }

    void InstallLoaderProbe() noexcept
    {
        void* loadLibrary = HookMainModuleImport(
            "kernel32.dll",
            "LoadLibraryA",
            reinterpret_cast<void*>(&HookLoadLibraryA));
        if (loadLibrary != nullptr)
        {
            RealLoadLibraryA = reinterpret_cast<LoadLibraryAFn>(loadLibrary);
        }

        void* getProcAddress = HookMainModuleImport(
            "kernel32.dll",
            "GetProcAddress",
            reinterpret_cast<void*>(&HookGetProcAddress));
        if (getProcAddress != nullptr)
        {
            RealGetProcAddress =
                reinterpret_cast<GetProcAddressFn>(getProcAddress);
        }

        char line[160] = {};
        wsprintfA(
            line,
            "loader probe: LoadLibraryA=%d GetProcAddress=%d\n",
            loadLibrary != nullptr ? 1 : 0,
            getProcAddress != nullptr ? 1 : 0);
        Trace(line);
    }

    // ---------------------------------------------------------------
    // Local name resolution shim.
    //
    // The client asks for its own address the 2008 way: gethostname() and then
    // gethostbyname() on that name. Under Wine that second call ends up in
    // nsi.dll enumerating the host's network interfaces, and inside Winlator's
    // PRoot container that enumeration never returns. The main thread parks in
    // NtWaitForSingleObject with only two threads alive, which is exactly the
    // black window we see.
    //
    // Answering the local name ourselves keeps the client off that path.
    // ---------------------------------------------------------------
    using GetHostByNameFn = struct hostent* (WSAAPI*)(const char*);
    using GetHostNameFn = int (WSAAPI*)(char*, int);

    GetHostByNameFn RealGetHostByName = nullptr;
    GetHostNameFn RealGetHostName = nullptr;
    volatile LONG LocalNameQueries = 0;

    char LocalHostName[256] = "localhost";
    char LocalHostAlias0[] = "localhost";
    char* LocalHostAliases[] = { LocalHostAlias0, nullptr };
    char LocalHostAddress[4] = { 127, 0, 0, 1 };
    char* LocalHostAddresses[] = { LocalHostAddress, nullptr };
    hostent LocalHostEntry = {
        LocalHostName,
        LocalHostAliases,
        AF_INET,
        4,
        LocalHostAddresses
    };

    struct hostent* WSAAPI HookGetHostByName(const char* name) noexcept
    {
        const LONG seen = InterlockedIncrement(&LocalNameQueries);

        // A bare name with no dot cannot be a real remote host on this LAN:
        // the client always reaches the server by IP. Treating every such name
        // as the local machine keeps us off the nsi.dll enumeration path
        // without needing gethostname (winsock is not up yet when the hook is
        // installed, so asking for the real name there fails).
        bool local = name == nullptr || name[0] == '\0';
        if (!local)
        {
            local = _stricmp(name, "localhost") == 0
                || strchr(name, '.') == nullptr;
        }

        if (local)
        {
            if (seen <= 8)
            {
                char line[224] = {};
                wsprintfA(
                    line,
                    "gethostbyname(\"%s\") -> 127.0.0.1 (shimmed)\n",
                    name != nullptr ? name : "");
                Trace(line);
            }
            return &LocalHostEntry;
        }

        if (seen <= 8)
        {
            char line[224] = {};
            wsprintfA(
                line,
                "gethostbyname(\"%s\") -> real resolver\n",
                name != nullptr ? name : "");
            Trace(line);
        }

        return RealGetHostByName != nullptr
            ? RealGetHostByName(name)
            : nullptr;
    }

    GetHostNameFn RealGetHostNameLog = nullptr;

    int WSAAPI HookLogGetHostName(char* name, int length) noexcept
    {
        if (RealGetHostNameLog == nullptr)
        {
            RealGetHostNameLog = reinterpret_cast<GetHostNameFn>(
                GetProcAddress(GetModuleHandleA("ws2_32.dll"), "gethostname"));
        }

        const int result = RealGetHostNameLog != nullptr
            ? RealGetHostNameLog(name, length)
            : SOCKET_ERROR;

        char line[288] = {};
        wsprintfA(
            line,
            "gethostname -> %d \"%s\"\n",
            result,
            (result == 0 && name != nullptr) ? name : "");
        Trace(line);
        return result;
    }

    void InstallNameResolutionShim() noexcept
    {
        void* getHostByName = HookMainModuleImport(
            "ws2_32.dll",
            "gethostbyname",
            reinterpret_cast<void*>(&HookGetHostByName));
        if (getHostByName != nullptr)
        {
            RealGetHostByName = reinterpret_cast<GetHostByNameFn>(getHostByName);
        }

        void* hostName = HookMainModuleImport(
            "ws2_32.dll",
            "gethostname",
            reinterpret_cast<void*>(&HookLogGetHostName));
        (void)hostName;

        // gethostname is deliberately NOT hooked: faking it changed the
        // client's own start-up order and made it dereference an
        // uninitialised pointer (0xCDCDCDCD + 8) on Windows.
        char line[192] = {};
        wsprintfA(
            line,
            "name resolution shim: gethostbyname=%d (bare names -> 127.0.0.1)\n",
            getHostByName != nullptr ? 1 : 0);
        Trace(line);
    }

    int InstallCommandLineHooks() noexcept
    {
        auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        if (base == nullptr)
        {
            return 0;
        }

        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        {
            return 0;
        }

        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
        {
            return 0;
        }

        const auto& directory =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (directory.VirtualAddress == 0)
        {
            return 0;
        }

        HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
        if (kernel32 == nullptr)
        {
            return 0;
        }

        auto* targetWide =
            reinterpret_cast<void*>(GetProcAddress(kernel32, "GetCommandLineW"));
        auto* targetAnsi =
            reinterpret_cast<void*>(GetProcAddress(kernel32, "GetCommandLineA"));
        if (targetWide == nullptr || targetAnsi == nullptr)
        {
            return 0;
        }

        int patched = 0;
        auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
            base + directory.VirtualAddress);
        for (; descriptor->Name != 0; ++descriptor)
        {
            auto* thunk =
                reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
            for (; thunk->u1.Function != 0; ++thunk)
            {
                auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
                if (*slot == targetWide)
                {
                    if (PatchImportSlot(
                            slot,
                            reinterpret_cast<void*>(&HookGetCommandLineW)))
                    {
                        ++patched;
                    }
                }
                else if (*slot == targetAnsi)
                {
                    if (PatchImportSlot(
                            slot,
                            reinterpret_cast<void*>(&HookGetCommandLineA)))
                    {
                        ++patched;
                    }
                }
            }
        }

        return patched;
    }

    // Runs off the loader-lock thread. Probing the port, spawning the server
    // and waiting for it to listen are all blocking operations, so they must
    // not happen inside DllMain.
    DWORD WINAPI ServerStartWorker(LPVOID) noexcept
    {
        try
        {
            const wchar_t* configPath = WorkerConfigPath;
            const wchar_t* host = WorkerHost;
            const int port = WorkerPort;

            Trace("DFLegacy.Ijl15: server worker started.\n");

            if (ReadInt(configPath, L"AutoStartServer", 1) == 0)
            {
                return 0;
            }

            if (IsPortListening(host, port))
            {
                Trace("DFLegacy.Ijl15: entrance port already listening.\n");
                return 0;
            }

            wchar_t serverRelative[MAX_PATH] = {};
            ReadString(
                configPath,
                L"ServerPath",
                L"DFLegacy\\src\\DFLegacy.Server\\bin\\Release\\net10.0\\DFLegacy.Server.exe",
                serverRelative,
                MAX_PATH);
            if (serverRelative[0] == L'\0')
            {
                return 0;
            }

            wchar_t serverPath[MAX_PATH] = {};
            if (PathIsRelativeW(serverRelative))
            {
                wchar_t clientDirectory[MAX_PATH] = {};
                if (!BuildClientDirectory(clientDirectory, MAX_PATH))
                {
                    return 0;
                }

                const std::size_t directoryLength = std::wcslen(clientDirectory);
                const std::size_t relativeLength = std::wcslen(serverRelative);
                if (directoryLength + relativeLength + 1 > MAX_PATH)
                {
                    return 0;
                }

                std::wmemcpy(serverPath, clientDirectory, directoryLength);
                std::wmemcpy(
                    serverPath + directoryLength,
                    serverRelative,
                    relativeLength + 1);
            }
            else
            {
                std::wmemcpy(
                    serverPath,
                    serverRelative,
                    std::wcslen(serverRelative) + 1);
            }

            if (GetFileAttributesW(serverPath) == INVALID_FILE_ATTRIBUTES)
            {
                Trace("DFLegacy.Ijl15: launcher bootstrap could not find DFLegacy.Server.exe.\n");
                return 0;
            }

            if (!StartServer(
                    serverPath,
                    ReadInt(configPath, L"KillServerOnExit", 1) != 0,
                    configPath))
            {
                Trace("DFLegacy.Ijl15: launcher bootstrap could not start DFLegacy.Server.exe.\n");
                return 0;
            }

            const int waitSeconds = ReadInt(configPath, L"WaitSeconds", DefaultWaitSeconds);
            const int attempts = (waitSeconds > 0 ? waitSeconds : 0) * 10;
            for (int attempt = 0; attempt < attempts; ++attempt)
            {
                if (IsPortListening(host, port))
                {
                    Trace("DFLegacy.Ijl15: DFLegacy.Server is listening.\n");
                    return 0;
                }

                Sleep(100);
            }

            Trace("DFLegacy.Ijl15: DFLegacy.Server did not start listening in time.\n");
        }
        catch (...)
        {
            Trace("DFLegacy.Ijl15: server worker failed.\n");
        }

        return 0;
    }

    void ApplyCore() noexcept
    {
        wchar_t configPath[MAX_PATH] = {};
        if (!BuildConfigPath(configPath, MAX_PATH))
        {
            Trace("DFLegacy.Ijl15: launcher bootstrap skipped, no Config.ini path.\n");
            return;
        }

        {
            char line[160] = {};
            if (wsprintfA(
                    line,
                    "image base: %p\n",
                    reinterpret_cast<void*>(GetModuleHandleW(nullptr))) > 0)
            {
                Trace(line);
            }
        }

        // Runs before any client code: the section that is decrypted on demand
        // is touched long after this point, so the shim is in place by then.
        if (ReadInt(configPath, L"BlockPageDowngrade", 1) != 0)
        {
            const int slots = InstallProtectionShim();
            char line[192] = {};
            if (wsprintfA(
                    line,
                    "page downgrade shim installed: %d import slot(s)\n",
                    slots) > 0)
            {
                Trace(line);
            }
        }

        if (ReadInt(configPath, L"FixAccessViolation", 1) != 0)
        {
            InstallAccessViolationShim();
        }

        if (ReadInt(configPath, L"StubRsReporting", 1) != 0)
        {
            const int stubbed = StubModuleExports("RS.dll");
            char line[160] = {};
            if (wsprintfA(
                    line,
                    "RS.dll reporting stubbed: %d export(s)\n",
                    stubbed) > 0)
            {
                Trace(line);
            }
        }

        // BugTrap installs the unhandled-exception filter. Under Wine its
        // minidump writer is unimplemented, so it takes a lock, fails to write
        // the dump and never releases it: the crash becomes a permanent hang
        // behind a black window. Neutralising the filter lets the exception
        // reach our handler (and the log) instead.
        if (ReadInt(configPath, L"ProbeLoader", 1) != 0)
        {
            InstallLoaderProbe();
        }

        if (ReadInt(configPath, L"ShimLocalNameResolution", 1) != 0)
        {
            InstallNameResolutionShim();
        }

        if (ReadInt(configPath, L"StubBugTrap", 1) != 0)
        {
            const int stubbed = StubModuleExports("BugTrap.dll");
            char line[160] = {};
            if (wsprintfA(
                    line,
                    "BugTrap seh filter stubbed: %d export(s)\n",
                    stubbed) > 0)
            {
                Trace(line);
            }
        }

        if (ReadInt(configPath, L"Enabled", 1) == 0)
        {
            return;
        }

        // A launcher already provided the arguments; leave them alone.
        const wchar_t* existing = GetCommandLineW();
        if (existing != nullptr && std::wcsstr(existing, L"99?") != nullptr)
        {
            TraceWide("existing command line: ", existing);
            Trace("DFLegacy.Ijl15: launcher bootstrap skipped, command line already set.\n");
            return;
        }

        TraceWide("original command line: ", existing);

        wchar_t host[64] = {};
        wchar_t account[128] = {};
        wchar_t password[128] = {};
        ReadString(configPath, L"Host", L"127.0.0.1", host, 64);
        ReadString(configPath, L"Account", L"test", account, 128);
        ReadString(configPath, L"Password", L"test", password, 128);
        if (host[0] == L'\0' || account[0] == L'\0' || password[0] == L'\0')
        {
            Trace("DFLegacy.Ijl15: launcher bootstrap skipped, missing credentials.\n");
            return;
        }

        const int port = ReadInt(configPath, L"Port", DefaultPort);
        if (port <= 0 || port > 65535)
        {
            Trace("DFLegacy.Ijl15: launcher bootstrap skipped, invalid port.\n");
            return;
        }

        EntrancePort = port;
        ConnectWaitSeconds = ReadInt(configPath, L"ConnectWaitSeconds", 40);
        StartupGateSeconds = ReadInt(configPath, L"StartupGateSeconds", 12);

        if (ReadInt(configPath, L"WaitForEntrance", 1) != 0)
        {
            const int slots = InstallConnectHook();
            char line[192] = {};
            if (wsprintfA(
                    line,
                    "entrance connect gate: %d slot(s), wait %ds\n",
                    slots,
                    ConnectWaitSeconds) > 0)
            {
                Trace(line);
            }
        }

        // argv[0] must stay the executable path: the client reads its
        // entrance arguments from argv[1], and a command line that starts
        // with the argument string leaves it with no argument at all.
        wchar_t executablePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, executablePath, MAX_PATH) == 0)
        {
            Trace("DFLegacy.Ijl15: launcher bootstrap skipped, no executable path.\n");
            return;
        }

        wchar_t arguments[CommandLineCapacity] = {};
        const int written = swprintf_s(
            arguments,
            CommandLineCapacity,
            L"\"%s\" 99?%s?%d?%s?%s?0?0?0?0?0?0?0?0",
            executablePath,
            host,
            port,
            account,
            password);
        if (written <= 0)
        {
            Trace("DFLegacy.Ijl15: launcher bootstrap skipped, argument overflow.\n");
            return;
        }

        if (!PatchCommandLine(arguments))
        {
            Trace("DFLegacy.Ijl15: launcher bootstrap could not patch the command line.\n");
            return;
        }

        // Read the value back through both public accessors: if the client
        // still rejects the launch, this tells us whether the patch itself
        // failed or the client simply reads the arguments some other way.
        TraceWide("patched GetCommandLineW: ", GetCommandLineW());

        {
            char line[256] = {};
            if (wsprintfA(
                    line,
                    "patched command line length: %u\n",
                    static_cast<unsigned>(std::wcslen(CommandLineBuffer))) > 0)
            {
                Trace(line);
            }
        }

        if (WideCharToMultiByte(
                CP_ACP,
                0,
                CommandLineBuffer,
                -1,
                CommandLineAnsiBuffer,
                sizeof(CommandLineAnsiBuffer),
                nullptr,
                nullptr) > 0)
        {
            PatchCrtCommandLine(CommandLineBuffer, CommandLineAnsiBuffer);

            char line[256] = {};
            const int hooks = InstallCommandLineHooks();
            if (wsprintfA(line, "import slots hooked: %d\n", hooks) > 0)
            {
                Trace(line);
            }
        }
        {
            char line[640] = {};
            const char* ansi = GetCommandLineA();
            if (wsprintfA(
                    line,
                    "patched GetCommandLineA: %s",
                    ansi == nullptr ? "(null)" : ansi) > 0)
            {
                Trace(line);
            }
        }
        Trace("DFLegacy.Ijl15: entrance arguments installed.\n");

        if (HANDLE check = CreateThread(
                nullptr, 0, DeferredCommandLineCheck, nullptr, 0, nullptr))
        {
            CloseHandle(check);
        }

        // Snapshot the settings the worker needs, then hand the blocking work
        // (port probe, CreateProcess, wait-for-listen) to a background thread.
        // Doing it inline would keep the loader lock held for seconds and
        // deadlock every other thread under Wine.
        if (InterlockedCompareExchange(&WorkerStarted, 1, 0) == 0)
        {
            std::wmemcpy(WorkerConfigPath, configPath, std::wcslen(configPath) + 1);
            std::wmemcpy(WorkerHost, host, std::wcslen(host) + 1);
            WorkerPort = port;

            if (HANDLE worker = CreateThread(
                    nullptr, 0, ServerStartWorker, nullptr, 0, nullptr))
            {
                CloseHandle(worker);
            }
        }
    }
}

namespace dfl::launcher_bootstrap
{
    void Apply() noexcept
    {
        try
        {
            ApplyCore();
        }
        catch (...)
        {
            Trace("DFLegacy.Ijl15: launcher bootstrap failed.\n");
        }
    }
}

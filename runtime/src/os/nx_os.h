// SPDX-License-Identifier: GPL-3.0-or-later
// nx_os.h: the Win32 API the runtime is written against, for the POSIX (macOS) build.
//
// The runtime, the game modules and the vendored Link Play module are written against Win32: HANDLEs, SRWLOCK +
// CONDITION_VARIABLE, CreateThread, waitable timers, QueryPerformanceCounter, VirtualAlloc, CreateFileW/ReadFile.
// 90 of the 101 Win32 calls in the tree are one of those, and every one of them has a direct POSIX equivalent
// (docs/macos-port.md, D6). Rather than rewrite 80 files, this header declares the same names with the same
// signatures on top of pthreads, mmap/mach, clock_gettime and the POSIX file API, and the CMake build puts
// src/os/compat/ first on the include path so `<windows.h>` resolves here. The Windows build never sees this
// header at all: it keeps the real SDK headers, byte for byte (D12).
//
// Two things are NOT emulated, because emulating them would be a lie:
//   - section objects. CreateFileMappingW/MapViewOfFile* exist and behave (SEC_RESERVE, views at a fixed
//     address, commit through VirtualAlloc), implemented with mach_vm on top of an anonymous backing store.
//   - vectored exception handlers. AddVectoredExceptionHandler installs a mach EXC_BAD_ACCESS/EXC_BAD_INSTRUCTION
//     port handler. The handler runs on a helper thread, not on the faulting thread (that is the mach model), so
//     a handler may inspect and report but only a thread_state rewrite can resume the fault (write_watch.cpp).
//
// C11 and C++: the vendored pokopia-coop module is C11 and uses SRWLOCK/HANDLE/CreateThread, so the core types and
// those calls are plain C. The path and string calls are C++ inline overloads, because std::filesystem::path::c_str()
// is const char* here and const wchar_t* on Windows, and both must compile at the same call site.
#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#else  // ---------------------------------------------------------------- POSIX

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#include <sys/types.h>
#endif

// ---- calling conventions, attributes, decorations (all empty) ------------------------------------------------------
#define WINAPI
#define APIENTRY
#define CALLBACK
#define NTAPI
#define WINBASEAPI
#define STDAPICALLTYPE
#define STDMETHODCALLTYPE
#define STDMETHODIMP
#define STDAPI
#define DECLSPEC_NORETURN
#define WINAPIV
#ifndef __cdecl
#define __cdecl
#endif
#ifndef __stdcall
#define __stdcall
#endif
#ifndef __fastcall
#define __fastcall
#endif
#ifndef _cdecl
#define _cdecl
#endif
#ifndef _stdcall
#define _stdcall
#endif
#define WINAPI_FAMILY_PARTITION(x) 1

#if defined(__cplusplus)
#define NX_OS_INLINE inline
#else
#define NX_OS_INLINE static inline
#endif

// ---- integer and character types (Win32 widths, not the LP64 widths) ----------------------------------------------
typedef uint8_t BYTE;
typedef BYTE* PBYTE;
typedef BYTE* LPBYTE;
typedef uint16_t WORD;
typedef WORD* PWORD;
typedef WORD* LPWORD;
typedef uint32_t DWORD;
typedef DWORD* PDWORD;
typedef DWORD* LPDWORD;
typedef int32_t LONG;
typedef LONG* PLONG;
typedef LONG* LPLONG;
typedef uint32_t ULONG;
typedef ULONG* PULONG;
typedef uint64_t ULONGLONG;
typedef ULONGLONG* PULONGLONG;
typedef int64_t LONGLONG;
typedef uint64_t DWORD64;
typedef uint64_t ULONG64;
typedef int64_t LONG64;
typedef uint32_t DWORD32;
typedef int64_t* PLONGLONG;
typedef int32_t INT;
typedef uint32_t UINT;
typedef UINT* PUINT;
typedef int16_t SHORT;
typedef uint16_t USHORT;
typedef char CHAR;
typedef wchar_t WCHAR;
typedef uint16_t ATOM;
typedef uint32_t COLORREF;
typedef uint32_t LCID;
typedef uint16_t LANGID;
#define SUBLANGID(id) ((WORD)(id))
#define MAKELANGID(p, s) ((WORD)(((WORD)(s) << 10) | (WORD)(p)))
typedef int BOOL;
typedef BOOL* PBOOL;
typedef BOOL* LPBOOL;
typedef unsigned char BOOLEAN;
typedef BOOLEAN* PBOOLEAN;
typedef size_t SIZE_T;
typedef size_t* PSIZE_T;
typedef intptr_t SSIZE_T;
typedef intptr_t INT_PTR;
typedef intptr_t LONG_PTR;
typedef uintptr_t UINT_PTR;
typedef uintptr_t ULONG_PTR;
typedef uintptr_t DWORD_PTR;
typedef DWORD_PTR* PDWORD_PTR;
typedef UINT_PTR* PUINT_PTR;
typedef ULONG_PTR* PULONG_PTR;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef intptr_t LRESULT;
typedef int32_t HRESULT;
typedef int32_t NTSTATUS;
typedef HRESULT* PHRESULT;
typedef NTSTATUS* PNTSTATUS;

typedef void* LPVOID;
typedef void* PVOID;
typedef const void* LPCVOID;
typedef const void* PCVOID;
typedef char* LPSTR;
typedef char* PSTR;
typedef char* PCH;
typedef const char* LPCSTR;
typedef const char* PCSTR;
typedef const char* PCCH;
typedef wchar_t* LPWSTR;
typedef wchar_t* PWSTR;
typedef wchar_t* PWCHAR;
typedef const wchar_t* LPCWSTR;
typedef const wchar_t* PCWSTR;
typedef const wchar_t* PCWCHAR;

typedef void* HANDLE;
typedef HANDLE* PHANDLE;
typedef HANDLE* LPHANDLE;
typedef void* HMODULE;
typedef void* HINSTANCE;
typedef void* HLOCAL;
typedef void* HGLOBAL;
typedef void* HWND;
typedef void* HDC;
typedef void* HBRUSH;
typedef void* HFONT;
typedef void* HICON;
typedef void* HCURSOR;
typedef void* HMENU;
typedef void* HKEY;
typedef void* HMONITOR;
typedef void* HCRYPTMSG;
typedef void* HCERTSTORE;
typedef void* HCRYPTPROV;
typedef void* LSA_HANDLE;
typedef void* PSID;
typedef void* PACL;
typedef int (*FARPROC)(void);
typedef int (*PROC)(void);
typedef DWORD (*PTHREAD_START_ROUTINE)(LPVOID);

typedef union _LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    };
    struct {
        DWORD LowPart;
        LONG HighPart;
    } u;
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER, *LPLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct {
        DWORD LowPart;
        DWORD HighPart;
    };
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME, *PFILETIME, *LPFILETIME;

typedef struct _SYSTEMTIME {
    WORD wYear;
    WORD wMonth;
    WORD wDayOfWeek;
    WORD wDay;
    WORD wHour;
    WORD wMinute;
    WORD wSecond;
    WORD wMilliseconds;
} SYSTEMTIME, *PSYSTEMTIME, *LPSYSTEMTIME;

typedef struct _OVERLAPPED {
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    union {
        struct {
            DWORD Offset;
            DWORD OffsetHigh;
        };
        PVOID Pointer;
    };
    HANDLE hEvent;
    ULONG_PTR InternalLow;
} OVERLAPPED, *LPOVERLAPPED;

typedef struct _STARTUPINFOA {
    DWORD cb;
    LPSTR lpReserved;
    LPSTR lpDesktop;
    LPSTR lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    WORD wShowWindow, cbReserved2;
    LPBYTE lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOA, *LPSTARTUPINFOA;
typedef struct _STARTUPINFOW {
    DWORD cb;
    LPWSTR lpReserved;
    LPWSTR lpDesktop;
    LPWSTR lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    WORD wShowWindow, cbReserved2;
    LPBYTE lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOW, *LPSTARTUPINFOW;
typedef struct _PROCESS_INFORMATION {
    HANDLE hProcess;
    HANDLE hThread;
    DWORD dwProcessId;
    DWORD dwThreadId;
} PROCESS_INFORMATION, *LPPROCESS_INFORMATION;

typedef struct _GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
} GUID, IID, CLSID;
typedef GUID* LPGUID;
typedef const GUID* REFGUID;
typedef const IID& REFIID;
typedef const CLSID& REFCLSID;

#define CONST const
#define VOID void
#define TRUE 1
#define FALSE 0

// ---- status codes ------------------------------------------------------------------------------------------------
#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005)
#define E_NOINTERFACE ((HRESULT)0x80004002)
#define E_POINTER ((HRESULT)0x80004003)
#define E_INVALIDARG ((HRESULT)0x80070057)
#define E_NOTIMPL ((HRESULT)0x80004001)
#define E_OUTOFMEMORY ((HRESULT)0x8007000E)
#define E_UNEXPECTED ((HRESULT)0x8000FFFF)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define HRESULT_FROM_WIN32(e) ((HRESULT)(e) <= 0 ? ((HRESULT)(e)) : ((HRESULT)(((e) & 0x0000FFFF) | 0x80070000)))
#define MAKE_HRESULT(sev, fac, code) ((HRESULT)(((uint32_t)(sev) << 31) | ((uint32_t)(fac) << 16) | (uint32_t)(code)))
#define MAKE_SCODE(sev, fac, code) MAKE_HRESULT(sev, fac, code)

// ---- handles -----------------------------------------------------------------------------------------------------
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)0xFFFFFFFF)
#define MAX_PATH 260
#define MAX_PATHW 260

// ---- errors ------------------------------------------------------------------------------------------------------
#define ERROR_SUCCESS 0L
#define ERROR_INVALID_FUNCTION 1L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_PATH_NOT_FOUND 3L
#define ERROR_TOO_MANY_OPEN_FILES 4L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_INVALID_HANDLE 6L
#define ERROR_NOT_ENOUGH_MEMORY 8L
#define ERROR_INVALID_BLOCK 9L
#define ERROR_BAD_ENVIRONMENT 10L
#define ERROR_INVALID_DRIVE 15L
#define ERROR_NO_MORE_FILES 18L
#define ERROR_WRITE_PROTECT 19L
#define ERROR_NOT_READY 21L
#define ERROR_SHARING_VIOLATION 32L
#define ERROR_LOCK_VIOLATION 33L
#define ERROR_HANDLE_EOF 38L
#define ERROR_HANDLE_DISK_FULL 39L
#define ERROR_NOT_SUPPORTED 50L
#define ERROR_GEN_FAILURE 31L
#define ERROR_FILE_EXISTS 80L
#define ERROR_INVALID_PARAMETER 87L
#define ERROR_BROKEN_PIPE 109L
#define ERROR_BUFFER_OVERFLOW 111L
#define ERROR_DISK_FULL 112L
#define ERROR_CALL_NOT_IMPLEMENTED 120L
#define ERROR_INSUFFICIENT_BUFFER 122L
#define ERROR_INVALID_NAME 123L
#define ERROR_MOD_NOT_FOUND 126L
#define ERROR_PROC_NOT_FOUND 127L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_ENVVAR_NOT_FOUND 203L
#define ERROR_MORE_DATA 234L
#define ERROR_NO_DATA 232L
#define ERROR_OPERATION_ABORTED 995L
#define ERROR_IO_PENDING 997L
#define ERROR_TIMEOUT 1460L
#define ERROR_COMMITMENT_LIMIT 1455L
#define ERROR_NOT_FOUND 1168L
#define ERROR_INVALID_ADDRESS 487L
#define ERROR_CANCELLED 1223L

// The MSVC CRT's _wfopen: a wide path and a mode. On POSIX the path is converted to UTF-8 first.
// FILE comes from <stdio.h>: glibc's `typedef struct _IO_FILE FILE` happens to match a repeated typedef, Darwin's
// `struct __sFILE` does not, so the typedef is not repeated here.
FILE* nxos_wfopen(const wchar_t* path, const char* mode);
FILE* nxos_wfreopen(const wchar_t* path, const char* mode, FILE* stream);
#if defined(__cplusplus)
// std::filesystem::path::c_str() is const char* on POSIX and const wchar_t* on Windows, and the mode is written as
// L"wb" at the call sites, so every combination has to resolve.
FILE* nxos_wfopen(const char* path, const char* mode);
FILE* nxos_wfreopen(const char* path, const char* mode, FILE* stream);
FILE* nxos_wfopen(const char* path, const wchar_t* mode);
FILE* nxos_wfreopen(const char* path, const wchar_t* mode, FILE* stream);
FILE* nxos_wfopen(const wchar_t* path, const wchar_t* mode);
FILE* nxos_wfreopen(const wchar_t* path, const wchar_t* mode, FILE* stream);
#endif
#if defined(__cplusplus)
#define _wfopen(path, mode) nxos_wfopen((path), (mode))
#define _wfreopen(path, mode, stream) nxos_wfreopen((path), (mode), (stream))
#else
#define _wfopen(path, mode) nxos_wfopen((path), (mode))
#define _wfreopen(path, mode, stream) nxos_wfreopen((path), (mode), (stream))
#endif
DWORD GetLastError(void);

// The MSVC CRT's 64-bit integer helpers, which the runtime calls directly (kernel/sched.cpp parses the affinity config).
#if defined(__cplusplus)
#define _strtoui64(str, end, base) strtoull((str), (end), (base))
#define _strtoi64(str, end, base) strtoll((str), (end), (base))
#define _wcstoui64(str, end, base) wcstoull((str), (end), (base))
#else
#define _strtoui64(str, end, base) strtoull((str), (end), (base))
#define _strtoi64(str, end, base) strtoll((str), (end), (base))
#endif
void SetLastError(DWORD err);
void nxos_set_error_from_errno(int err);

// The MSVC CRT's localtime_s takes (tm*, const time_t*); POSIX's localtime_r takes (const time_t*, tm*). Same result,
// zero on success. Only the C++ call sites use it (games/pkp/pkp_save_uuid.cpp).
#if defined(__cplusplus)
inline int localtime_s(struct tm* out, const time_t* t) { return localtime_r(t, out) ? 0 : -1; }
#endif

// ---- memory ------------------------------------------------------------------------------------------------------
#define MEM_COMMIT 0x00001000
#define MEM_RESERVE 0x00002000
#define MEM_DECOMMIT 0x00004000
#define MEM_RELEASE 0x00008000
#define MEM_FREE 0x00010000
#define MEM_PRIVATE 0x00020000
#define MEM_MAPPED 0x00040000
#define MEM_RESET 0x00080000
#define MEM_TOP_DOWN 0x00100000
#define MEM_LARGE_PAGES 0x20000000

#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define PAGE_GUARD 0x100
#define PAGE_NOCACHE 0x200
#define PAGE_WRITECOMBINE 0x400

#define SEC_FILE 0x00800000
#define SEC_IMAGE 0x01000000
#define SEC_RESERVE 0x04000000
#define SEC_COMMIT 0x08000000
#define SEC_NOCACHE 0x10000000
#define SEC_LARGE_PAGES 0x80000000

#define FILE_MAP_WRITE 0x0002
#define FILE_MAP_READ 0x0004
#define FILE_MAP_ALL_ACCESS 0x000F001F
#define FILE_MAP_COPY 0x00000001
#define FILE_MAP_EXECUTE 0x0020

typedef struct _MEMORY_BASIC_INFORMATION {
    PVOID BaseAddress;
    PVOID AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION, *LPMEMORY_BASIC_INFORMATION;

typedef struct _SYSTEM_INFO {
    union {
        DWORD dwOemId;
        struct {
            WORD wProcessorArchitecture;
            WORD wReserved;
        };
    };
    DWORD dwPageSize;
    LPVOID lpMinimumApplicationAddress;
    LPVOID lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    WORD wProcessorLevel;
    WORD wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;

#define PROCESSOR_ARCHITECTURE_ARM64 12
#define PROCESSOR_ARCHITECTURE_AMD64 9

typedef struct _MEMORYSTATUSEX {
    DWORD dwLength;
    DWORD dwMemoryLoad;
    ULONGLONG ullTotalPhys;
    ULONGLONG ullAvailPhys;
    ULONGLONG ullTotalPageFile;
    ULONGLONG ullAvailPageFile;
    ULONGLONG ullTotalVirtual;
    ULONGLONG ullAvailVirtual;
    ULONGLONG ullAvailExtendedVirtual;
} MEMORYSTATUSEX, *LPMEMORYSTATUSEX;

LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect);
LPVOID VirtualAllocEx(HANDLE proc, LPVOID addr, SIZE_T size, DWORD type, DWORD protect);
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type);
BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD protect, PDWORD old_protect);
BOOL VirtualProtectEx(HANDLE proc, LPVOID addr, SIZE_T size, DWORD protect, PDWORD old_protect);
SIZE_T VirtualQuery(LPCVOID addr, MEMORY_BASIC_INFORMATION* mbi, SIZE_T len);
SIZE_T VirtualQueryEx(HANDLE proc, LPCVOID addr, MEMORY_BASIC_INFORMATION* mbi, SIZE_T len);
// macOS: [start, end) of the executable's __GUEST segment (cmake/guest_segment.py, 0x7000000000 + 72 GiB, slid by dyld).
// False when the binary has none (a dylib, a unit test). The guest layout must fit inside it: see src/mem/memory.cpp.
bool nxos_guest_reservation(uint64_t* start, uint64_t* end);

// Gives this thread a signal stack (64 KiB), so a handler can run when the thread's own stack is exhausted. Without it a
// stack overflow is reported as a second fault inside the handler (the report then runs on the exhausted stack and dies),
// which is exactly the case the report is needed for. Idempotent; the main thread gets one in install_signals().
void nxos_install_altstack(void);

// The name the runtime gave this thread (log_set_thread_name), for the signal handler: it reads it as a thread-local
// pointer instead of calling pthread_getname_np, which is not safe on an exhausted stack. The pointer must stay valid.
void nxos_set_thread_name(const char* name);
void GetSystemInfo(LPSYSTEM_INFO si);
void GetNativeSystemInfo(LPSYSTEM_INFO si);
BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX ms);
BOOL FlushInstructionCache(HANDLE proc, LPCVOID addr, SIZE_T size);

// Section objects (memory.cpp: the guest heap is a SEC_RESERVE pagefile section with views at fixed addresses).
HANDLE CreateFileMappingW(HANDLE file, void* sa, DWORD protect, DWORD hi, DWORD lo, LPCWSTR name);
HANDLE CreateFileMappingA(HANDLE file, void* sa, DWORD protect, DWORD hi, DWORD lo, LPCSTR name);
LPVOID MapViewOfFileEx(HANDLE section, DWORD access, DWORD hi, DWORD lo, SIZE_T size, LPVOID at);
LPVOID MapViewOfFile(HANDLE section, DWORD access, DWORD hi, DWORD lo, SIZE_T size);
BOOL UnmapViewOfFile(LPCVOID view);
BOOL FlushViewOfFile(LPCVOID view, SIZE_T size);

// ---- process and thread handles ----------------------------------------------------------------------------------
HANDLE GetCurrentProcess(void);
DWORD GetCurrentProcessId(void);
HANDLE GetCurrentThread(void);
DWORD GetCurrentThreadId(void);
HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid);
HANDLE OpenThread(DWORD access, BOOL inherit, DWORD tid);
BOOL TerminateProcess(HANDLE proc, UINT code);
void ExitProcess(UINT code);
void ExitThread(DWORD code);
BOOL GetExitCodeProcess(HANDLE proc, PDWORD code);
BOOL GetExitCodeThread(HANDLE thread, PDWORD code);
BOOL CloseHandle(HANDLE h);
BOOL DuplicateHandle(HANDLE src_proc, HANDLE src, HANDLE dst_proc, HANDLE* out, DWORD access, BOOL inherit, DWORD options);
DWORD GetProcessHandleCount(HANDLE proc, PDWORD count);

#define DUPLICATE_CLOSE_SOURCE 0x00000001
#define DUPLICATE_SAME_ACCESS 0x00000002

#define SYNCHRONIZE 0x00100000L
#define PROCESS_TERMINATE 0x0001
#define PROCESS_QUERY_INFORMATION 0x0400
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#define PROCESS_VM_READ 0x0010
#define THREAD_TERMINATE 0x0001
#define THREAD_SUSPEND_RESUME 0x0002
#define THREAD_GET_CONTEXT 0x0008
#define THREAD_SET_CONTEXT 0x0010
#define THREAD_SET_INFORMATION 0x0020
#define THREAD_QUERY_INFORMATION 0x0040
#define THREAD_QUERY_LIMITED_INFORMATION 0x0800
#define THREAD_ALL_ACCESS 0x1FFFFF
#define STILL_ACTIVE 259

// ---- threads -----------------------------------------------------------------------------------------------------
#define CREATE_SUSPENDED 0x00000004
#define CREATE_NEW_CONSOLE 0x00000010
#define CREATE_NO_WINDOW 0x08000000
#define DETACHED_PROCESS 0x00000008
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000
#define THREAD_PRIORITY_IDLE (-15)
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_MODE_BACKGROUND_BEGIN 0x00010000
#define THREAD_MODE_BACKGROUND_END 0x00020000
#define IDLE_PRIORITY_CLASS 0x00000040
#define BELOW_NORMAL_PRIORITY_CLASS 0x00004000
#define NORMAL_PRIORITY_CLASS 0x00000020
#define ABOVE_NORMAL_PRIORITY_CLASS 0x00008000
#define HIGH_PRIORITY_CLASS 0x00000080
#define REALTIME_PRIORITY_CLASS 0x00000100

HANDLE CreateThread(void* sa, SIZE_T stack, PTHREAD_START_ROUTINE start, LPVOID arg, DWORD flags, DWORD* tid);
BOOL SetThreadPriority(HANDLE thread, int priority);
int GetThreadPriority(HANDLE thread);
BOOL SetThreadPriorityBoost(HANDLE thread, BOOL disable);
DWORD_PTR SetThreadAffinityMask(HANDLE thread, DWORD_PTR mask);
typedef struct _PROCESSOR_NUMBER {
    WORD Group;
    BYTE Number;
    BYTE Reserved;
} PROCESSOR_NUMBER, *PPROCESSOR_NUMBER;
BOOL GetThreadIdealProcessorEx(HANDLE thread, PPROCESSOR_NUMBER out);
DWORD SetThreadIdealProcessorEx(HANDLE thread, const PROCESSOR_NUMBER* ideal, PDWORD previous);
DWORD GetCurrentProcessorNumber(void);
DWORD GetCurrentProcessorNumberEx(PPROCESSOR_NUMBER out);
BOOL SetThreadStackGuarantee(PULONG guarantee);
BOOL IsBadReadPtr(LPCVOID addr, UINT_PTR size);
BOOL IsBadWritePtr(LPVOID addr, UINT_PTR size);

// Processor topology (kernel/sched.cpp maps the guest's three cores onto host processors). Windows answers through
// GetLogicalProcessorInformationEx; the shim answers from sysctl, in the same structure.
typedef struct _GROUP_AFFINITY {
    DWORD_PTR Mask;
    WORD Group;
    WORD Reserved[3];
} GROUP_AFFINITY, *PGROUP_AFFINITY;

typedef struct _PROCESSOR_RELATIONSHIP {
    BYTE Flags;
    BYTE EfficiencyClass;
    BYTE Reserved[20];
    WORD GroupCount;
    GROUP_AFFINITY GroupMask[1];
} PROCESSOR_RELATIONSHIP, *PPROCESSOR_RELATIONSHIP;

typedef struct _SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX {
    int Relationship;
    DWORD Size;
    union {
        PROCESSOR_RELATIONSHIP Processor;
        BYTE Reserved[64];
    };
} SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, *PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX;

#define RelationProcessorCore 0
#define RelationNumaNode 1
#define RelationCache 2
#define RelationProcessorPackage 3
#define RelationGroup 4

BOOL GetLogicalProcessorInformationEx(int relationship, SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* buffer, PDWORD len);
BOOL GetProcessAffinityMask(HANDLE proc, PDWORD_PTR proc_mask, PDWORD_PTR sys_mask);
BOOL SetPriorityClass(HANDLE proc, DWORD cls);

// Power throttling (kernel/sched.cpp turns Windows 11's EcoQoS off for the whole process). macOS has no such state: the
// scheduler decides, and there is no timer-resolution floor to opt out of, so SetProcessInformation reports failure.
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
typedef struct _PROCESS_POWER_THROTTLING_STATE {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} PROCESS_POWER_THROTTLING_STATE, *PPROCESS_POWER_THROTTLING_STATE;
#define ProcessPowerThrottling 4
BOOL SetProcessInformation(HANDLE proc, int cls, LPVOID info, DWORD size);
DWORD GetPriorityClass(HANDLE proc);
DWORD SuspendThread(HANDLE thread);
DWORD ResumeThread(HANDLE thread);
BOOL SwitchToThread(void);
void Sleep(DWORD ms);
DWORD SleepEx(DWORD ms, BOOL alertable);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable);
DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL all, DWORD ms);
DWORD WaitForMultipleObjectsEx(DWORD count, const HANDLE* handles, BOOL all, DWORD ms, BOOL alertable);

#define MAXIMUM_WAIT_OBJECTS 64
#define THREAD_PRIORITY_ERROR_RETURN 0x7FFFFFFF
#define THREAD_PRIORITY_ERROR_RETURN_VALUE THREAD_PRIORITY_ERROR_RETURN
#define INFINITE 0xFFFFFFFF
#define WAIT_OBJECT_0 0x00000000
#define WAIT_ABANDONED 0x00000080
#define WAIT_ABANDONED_0 0x00000080
#define WAIT_TIMEOUT 0x00000102
#define WAIT_FAILED 0xFFFFFFFF

// ---- synchronisation primitives ----------------------------------------------------------------------------------
// SRWLOCK and CONDITION_VARIABLE are one pointer each: static storage zeroed by the loader must be a valid unlocked
// lock (os/sync.cpp relies on that, and KThread::cv_storage is one void*), and a Darwin pthread_mutex_t is 64 bytes
// whose zeroed state is not initialised. The pthread objects are allocated on first use instead.
typedef struct {
    void* impl;
} SRWLOCK, *PSRWLOCK, *LPSRWLOCK;
typedef struct {
    void* impl;
} CONDITION_VARIABLE, *PCONDITION_VARIABLE, *LPCONDITION_VARIABLE;
typedef struct {
    void* impl;
} CRITICAL_SECTION, *PCRITICAL_SECTION, *LPCRITICAL_SECTION;

#define SRWLOCK_INIT {0}
#define CONDITION_VARIABLE_INIT {0}

void InitializeSRWLock(PSRWLOCK lock);
void AcquireSRWLockExclusive(PSRWLOCK lock);
void ReleaseSRWLockExclusive(PSRWLOCK lock);
void AcquireSRWLockShared(PSRWLOCK lock);
void ReleaseSRWLockShared(PSRWLOCK lock);
BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK lock);
BOOLEAN TryAcquireSRWLockShared(PSRWLOCK lock);
void InitializeConditionVariable(PCONDITION_VARIABLE cv);
BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK lock, DWORD ms, ULONG flags);
BOOL SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD ms);
void WakeConditionVariable(PCONDITION_VARIABLE cv);
void WakeAllConditionVariable(PCONDITION_VARIABLE cv);

#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1

void InitializeCriticalSection(PCRITICAL_SECTION cs);
void EnterCriticalSection(PCRITICAL_SECTION cs);
void LeaveCriticalSection(PCRITICAL_SECTION cs);
BOOL TryEnterCriticalSection(PCRITICAL_SECTION cs);
void DeleteCriticalSection(PCRITICAL_SECTION cs);

HANDLE CreateEventA(void* sa, BOOL manual_reset, BOOL initial, LPCSTR name);
BOOL SetEvent(HANDLE ev);
BOOL ResetEvent(HANDLE ev);
HANDLE CreateMutexA(void* sa, BOOL initial_owner, LPCSTR name);
BOOL ReleaseMutex(HANDLE mutex);
HANDLE CreateSemaphoreW(void* sa, LONG initial, LONG max, LPCWSTR name);
BOOL ReleaseSemaphore(HANDLE sem, LONG release, PLONG previous);
BOOL SetWaitableTimer(HANDLE timer, const LARGE_INTEGER* due, LONG period, void* routine, void* arg, BOOL resume);
// The _Ex form adds a wake context and flags (tolerable delay, resume). The shim has neither: it forwards and ignores them.
BOOL SetWaitableTimerEx(HANDLE timer, const LARGE_INTEGER* due, LONG period, void* routine, void* arg,
                        void* wake_context, ULONG flags);
#define TOLERABLE_DELAY_NONE 0
#define TOLERABLE_DELAY_SHORT 1
#define TOLERABLE_DELAY_LONG 2
#define TIMER_RESUME 0x00000001
#define TIMER_NO_WAKE 0x00000002
BOOL CancelWaitableTimer(HANDLE timer);

#define CREATE_WAITABLE_TIMER_MANUAL_RESET 0x00000001
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#define TIMER_ALL_ACCESS 0x1F0003
#define TIMER_MODIFY_STATE 0x0002
#define TIMER_QUERY_STATE 0x0001

// ---- time --------------------------------------------------------------------------------------------------------
BOOL QueryPerformanceCounter(LARGE_INTEGER* out);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* out);
DWORD GetTickCount(void);
ULONGLONG GetTickCount64(void);
void GetSystemTime(LPSYSTEMTIME st);
void GetLocalTime(LPSYSTEMTIME st);
void GetSystemTimeAsFileTime(LPFILETIME ft);
BOOL SystemTimeToFileTime(const SYSTEMTIME* st, LPFILETIME ft);
BOOL FileTimeToSystemTime(const FILETIME* ft, LPSYSTEMTIME st);
BOOL FileTimeToLocalFileTime(const FILETIME* ft, LPFILETIME local);
BOOL LocalFileTimeToFileTime(const FILETIME* ft, LPFILETIME utc);
ULONGLONG GetSystemTimeAsUnixTime(void);
DWORD timeBeginPeriod(UINT ms);
DWORD timeEndPeriod(UINT ms);

// ---- files -------------------------------------------------------------------------------------------------------
#define GENERIC_READ 0x80000000L
#define GENERIC_WRITE 0x40000000L
#define GENERIC_EXECUTE 0x20000000L
#define GENERIC_ALL 0x10000000L

#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define FILE_SHARE_DELETE 0x00000004

#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5

#define FILE_ATTRIBUTE_READONLY 0x00000001
#define FILE_ATTRIBUTE_HIDDEN 0x00000002
#define FILE_ATTRIBUTE_SYSTEM 0x00000004
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_ARCHIVE 0x00000020
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_ATTRIBUTE_TEMPORARY 0x00000100
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400

#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2

// DwFlagsAndAttributes bits. POSIX has no advisory hint to carry them, so the performance hints are accepted and
// ignored; a flag with real semantics (FILE_FLAG_DELETE_ON_CLOSE, FILE_FLAG_OVERLAPPED) is deliberately not defined,
// so a use fails to compile instead of silently behaving differently. Positioned I/O comes through lpOverlapped.
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define FILE_FLAG_NO_BUFFERING 0x20000000
#define FILE_FLAG_WRITE_THROUGH 0x80000000

#define MOVEFILE_REPLACE_EXISTING 0x00000001
#define MOVEFILE_COPY_ALLOWED 0x00000002
#define MOVEFILE_WRITE_THROUGH 0x00000008

#define FindExInfoStandard 0
#define FindExInfoBasic 1
#define FindExInfoMaxInfoLevel 2
#define FindExSearchNameMatch 0
#define FindExSearchLimitToDirectories 1
#define FindExSearchLimitToDevices 2
#define FIND_FIRST_EX_CASE_SENSITIVE 0x00000001
#define FIND_FIRST_EX_LARGE_FETCH 0x00000002

typedef struct _WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    WCHAR cFileName[MAX_PATH];
    WCHAR cAlternateFileName[14];
} WIN32_FIND_DATAW, *PWIN32_FIND_DATAW, *LPWIN32_FIND_DATAW;

typedef struct _WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    CHAR cFileName[MAX_PATH];
    CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *PWIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;

// The path/name parameters accept const wchar_t* (a Win32 LPCWSTR, UTF-32 on Darwin) and, in C++, const char*
// (std::filesystem::path::c_str() on POSIX, UTF-8). nxos_* take UTF-8.
HANDLE nxos_create_file(const char* path, DWORD access, DWORD share, void* sa, DWORD disposition, DWORD flags);
BOOL nxos_delete_file(const char* path);
BOOL nxos_move_file(const char* from, const char* to, DWORD flags);
BOOL nxos_copy_file(const char* from, const char* to, BOOL fail_if_exists);
DWORD nxos_get_file_attributes(const char* path);
BOOL nxos_set_file_attributes(const char* path, DWORD attrs);
HANDLE nxos_find_first(const char* pattern, WIN32_FIND_DATAW* data);
BOOL nxos_find_next(HANDLE find, WIN32_FIND_DATAW* data);
HANDLE nxos_create_directory(const char* path);
BOOL nxos_remove_directory(const char* path);
DWORD nxos_full_path(const char* path, char* out, DWORD cap);
DWORD nxos_private_profile_string(const char* section, const char* key, const char* def, char* out, DWORD cap,
                                  const char* file);
HMODULE nxos_load_library(const char* path, DWORD flags);
DWORD nxos_get_module_file_name(HMODULE mod, char* out, DWORD cap);
HANDLE nxos_create_mutex(const char* name, BOOL initial_owner);
HANDLE nxos_create_event(const char* name, BOOL manual_reset, BOOL initial);
HANDLE nxos_create_waitable_timer(const char* name, DWORD flags, DWORD access);
DWORD nxos_get_environment(const char* name, char* out, DWORD cap);

BOOL ReadFile(HANDLE f, LPVOID buf, DWORD want, LPDWORD got, LPOVERLAPPED ov);
BOOL WriteFile(HANDLE f, LPCVOID buf, DWORD want, LPDWORD wrote, LPOVERLAPPED ov);
BOOL FlushFileBuffers(HANDLE f);
BOOL GetFileSizeEx(HANDLE f, PLARGE_INTEGER size);
DWORD GetFileSize(HANDLE f, LPDWORD high);

typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA, *LPWIN32_FILE_ATTRIBUTE_DATA;
#define GetFileExInfoStandard 0
#define GetFileExMaxInfoLevel 1

BOOL GetFileAttributesExW(LPCWSTR path, int level, WIN32_FILE_ATTRIBUTE_DATA* data);
BOOL nxos_get_file_attributes_ex(const char* path, WIN32_FILE_ATTRIBUTE_DATA* data);
void GetSystemTimePreciseAsFileTime(LPFILETIME ft);
BOOL SetFilePointerEx(HANDLE f, LARGE_INTEGER dist, PLARGE_INTEGER new_pos, DWORD method);
BOOL SetEndOfFile(HANDLE f);
BOOL ReadProcessMemory(HANDLE proc, LPCVOID addr, LPVOID out, SIZE_T size, PSIZE_T got);
BOOL WriteProcessMemory(HANDLE proc, LPVOID addr, LPCVOID src, SIZE_T size, PSIZE_T wrote);

// ---- modules and symbols -----------------------------------------------------------------------------------------
HMODULE GetModuleHandleA(LPCSTR name);
FARPROC GetProcAddress(HMODULE mod, LPCSTR name);
BOOL FreeLibrary(HMODULE mod);
DWORD GetModuleFileNameA(HMODULE mod, LPSTR out, DWORD cap);
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* out);

#define GET_MODULE_HANDLE_EX_FLAG_PIN 0x00000001
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x00000004
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000
#define LOAD_WITH_ALTERED_SEARCH_PATH 0x00000008

// ---- strings -----------------------------------------------------------------------------------------------------
#define CP_ACP 0
#define CP_OEMCP 1
#define CP_UTF8 65001
#define CP_UTF16 1200
#define MB_ERR_INVALID_CHARS 0x00000008
#define WC_ERR_INVALID_CHARS 0x00000080
#define WC_NO_BEST_FIT_CHARS 0x00000400

int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR src, int src_len, LPWSTR dst, int dst_len);
int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR src, int src_len, LPSTR dst, int dst_len, LPCSTR def, LPBOOL used);
int lstrlenW(LPCWSTR s);
int lstrlenA(LPCSTR s);
LPWSTR lstrcpyW(LPWSTR dst, LPCWSTR src);
int lstrcmpiW(LPCWSTR a, LPCWSTR b);
int lstrcmpiA(LPCSTR a, LPCSTR b);
LPWSTR CharUpperW(LPWSTR s);
DWORD FormatMessageW(DWORD flags, LPCVOID src, DWORD msg_id, DWORD lang, LPWSTR out, DWORD cap, void* args);
void OutputDebugStringA(LPCSTR s);
void OutputDebugStringW(LPCWSTR s);
BOOL CompareStringOrdinal(LPCWSTR a, int a_len, LPCWSTR b, int b_len, BOOL ignore_case);

// ---- registry (none on macOS: the callers fall back) -------------------------------------------------------------
#define HKEY_CLASSES_ROOT ((HKEY)(uintptr_t)0x80000000)
#define HKEY_CURRENT_USER ((HKEY)(uintptr_t)0x80000001)
#define HKEY_LOCAL_MACHINE ((HKEY)(uintptr_t)0x80000002)
#define HKEY_USERS ((HKEY)(uintptr_t)0x80000003)
#define HKEY_CURRENT_CONFIG ((HKEY)(uintptr_t)0x80000005)

#define KEY_READ 0x20019
#define KEY_WRITE 0x20006
#define KEY_ALL_ACCESS 0xF003F
#define KEY_WOW64_64KEY 0x0100
#define KEY_WOW64_32KEY 0x0200
#define REG_SZ 1
#define REG_EXPAND_SZ 2
#define REG_BINARY 3
#define REG_DWORD 4
#define REG_MULTI_SZ 7
#define REG_QWORD 11
#define RRF_RT_REG_SZ 0x00000002
#define RRF_RT_REG_DWORD 0x00000010
#define RRF_RT_REG_QWORD 0x00000040
#define RRF_RT_ANY 0x0000FFFF
#define RRF_SUBKEY_WOW6464KEY 0x00010000
#define RRF_SUBKEY_WOW6432KEY 0x00020000

LANGID GetUserDefaultUILanguage(void);
LANGID GetUserDefaultLangID(void);
LANGID GetSystemDefaultUILanguage(void);

#define PRIMARYLANGID(id) ((WORD)((id) & 0x3FF))
#define SUBLANGID_OF(id) ((WORD)(((id) >> 10) & 0x3F))
#define LANG_NEUTRAL 0x00
#define LANG_ENGLISH 0x09
#define LANG_FRENCH 0x0C
#define LANG_GERMAN 0x07
#define LANG_ITALIAN 0x10
#define LANG_JAPANESE 0x11
#define LANG_KOREAN 0x12
#define LANG_SPANISH 0x0A
#define LANG_CHINESE 0x04
#define LANG_PORTUGUESE 0x16
#define LANG_RUSSIAN 0x19
#define LANG_DUTCH 0x13
#define LANG_POLISH 0x15
#define SUBLANG_NEUTRAL 0x00
#define SUBLANG_ENGLISH_US 0x01
#define SUBLANG_ENGLISH_UK 0x02
#define SUBLANG_ENGLISH_CAN 0x04
#define SUBLANG_FRENCH 0x01
#define SUBLANG_FRENCH_CANADIAN 0x03
#define SUBLANG_GERMAN 0x01
#define SUBLANG_ITALIAN 0x01
#define SUBLANG_SPANISH 0x01
#define SUBLANG_SPANISH_MEXICAN 0x02
#define SUBLANG_SPANISH_MODERN 0x03
#define SUBLANG_CHINESE_HONGKONG 0x03
#define SUBLANG_CHINESE_MACAU 0x05
#define SUBLANG_JAPANESE_JAPAN 0x01
#define SUBLANG_KOREAN 0x01
#define SUBLANG_CHINESE_TRADITIONAL 0x01
#define SUBLANG_CHINESE_SIMPLIFIED 0x02
#define SUBLANG_PORTUGUESE_BRAZILIAN 0x01
#define SUBLANG_RUSSIAN_RUSSIA 0x01
#define SUBLANG_DUTCH 0x01
#define SUBLANG_POLISH_POLAND 0x01

LONG RegGetValueA(HKEY key, LPCSTR subkey, LPCSTR value, DWORD flags, LPDWORD type, LPVOID data, LPDWORD cap);
LONG RegOpenKeyExA(HKEY key, LPCSTR subkey, DWORD options, DWORD access, PHANDLE out);
LONG RegQueryValueExA(HKEY key, LPCSTR value, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD cap);
LONG RegCloseKey(HKEY key);
LONG RegSetValueExA(HKEY key, LPCSTR value, DWORD reserved, DWORD type, const BYTE* data, DWORD cap);
LONG RegCreateKeyExA(HKEY key, LPCSTR subkey, DWORD reserved, LPSTR cls, DWORD options, DWORD access, void* sa, PHANDLE out, LPDWORD disp);

// ---- environment and process creation ----------------------------------------------------------------------------
BOOL SetEnvironmentVariableA(LPCSTR name, LPCSTR value);
DWORD ExpandEnvironmentStringsA(LPCSTR src, LPSTR dst, DWORD cap);
BOOL CreateProcessW(LPCWSTR app, LPWSTR cmdline, void* pa, void* ta, BOOL inherit, DWORD flags, void* env, LPCWSTR dir,
                    STARTUPINFOW* si, PROCESS_INFORMATION* pi);
BOOL GetComputerNameA(LPSTR out, LPDWORD cap);
BOOL GetUserNameA(LPSTR out, LPDWORD cap);


// ---- console and UI (minimal: the game opens a console for logs, shows message boxes) -----------------------------
#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
#define MB_OK 0x00000000
#define MB_ICONERROR 0x00000010
#define MB_ICONWARNING 0x00000030
#define MB_ICONINFORMATION 0x00000040
#define MB_YESNO 0x00000004
#define IDYES 6
#define IDNO 7

HANDLE GetStdHandle(DWORD which);
BOOL GetConsoleMode(HANDLE h, PDWORD mode);
BOOL SetConsoleMode(HANDLE h, DWORD mode);
int MessageBoxA(HWND owner, LPCSTR text, LPCSTR caption, UINT type);
int MessageBoxW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type);

// ---- diagnostics -------------------------------------------------------------------------------------------------
BOOL IsDebuggerPresent(void);
void DebugBreak(void);
BOOL QueryThreadCycleTime(HANDLE thread, PULONGLONG cycles);
BOOL GetProcessTimes(HANDLE proc, LPFILETIME created, LPFILETIME exited, LPFILETIME kernel_t, LPFILETIME user_t);
BOOL GetThreadTimes(HANDLE thread, LPFILETIME created, LPFILETIME exited, LPFILETIME kernel_t, LPFILETIME user_t);

typedef struct _PROCESS_MEMORY_COUNTERS {
    DWORD cb;
    DWORD PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
} PROCESS_MEMORY_COUNTERS, *PPROCESS_MEMORY_COUNTERS;
typedef struct _PROCESS_MEMORY_COUNTERS_EX {
    DWORD cb;
    DWORD PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivateUsage;
} PROCESS_MEMORY_COUNTERS_EX, *PPROCESS_MEMORY_COUNTERS_EX;

BOOL GetProcessMemoryInfo(HANDLE proc, PROCESS_MEMORY_COUNTERS* pmc, DWORD cb);
DWORD GetGuiResources(HANDLE proc, DWORD flags);
#define GR_GDIOBJECTS 0
#define GR_USEROBJECTS 1

typedef struct _THREADENTRY32 {
    DWORD dwSize;
    DWORD cntUsage;
    DWORD th32ThreadID;
    DWORD th32OwnerProcessID;
    LONG tpBasePri;
    LONG tpDeltaPri;
    DWORD dwFlags;
} THREADENTRY32, *PTHREADENTRY32, *LPTHREADENTRY32;
#define TH32CS_SNAPTHREAD 0x00000004
#define TH32CS_SNAPPROCESS 0x00000002

HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD pid);
BOOL Thread32First(HANDLE snap, PTHREADENTRY32 te);
BOOL Thread32Next(HANDLE snap, PTHREADENTRY32 te);

// ---- structured exceptions (mach exception port, see os_posix.cpp) -----------------------------------------------
#define CONTEXT_AMD64 0x00100000
#define CONTEXT_ARM64 0x00400000
#define CONTEXT_CONTROL 0x00100001
#define CONTEXT_INTEGER 0x00100002
#define CONTEXT_SEGMENTS 0x00100004
#define CONTEXT_FLOATING_POINT 0x00100008
#define CONTEXT_DEBUG_REGISTERS 0x00100010
#define CONTEXT_FULL (CONTEXT_CONTROL | CONTEXT_INTEGER)
#define CONTEXT_ALL 0x0010001F
#define EXCEPTION_NONCONTINUABLE 0x01
#define EXCEPTION_MAXIMUM_PARAMETERS 15
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_ACCESS_VIOLATION 0xC0000005L
#define EXCEPTION_ILLEGAL_INSTRUCTION 0xC000001DL
#define EXCEPTION_PRIV_INSTRUCTION 0xC0000096L
#define EXCEPTION_BREAKPOINT 0x80000003L
#define EXCEPTION_STACK_OVERFLOW 0xC00000FDL
#define EXCEPTION_IN_PAGE_ERROR 0xC0000006L

typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    struct _EXCEPTION_RECORD* ExceptionRecord;
    PVOID ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;

typedef struct _CONTEXT {
    // AArch64 register file, in the order a crash report prints it. Only pc/sp/lr/fp and x0..x28 are filled in.
    ULONGLONG x[29];
    ULONGLONG fp;
    ULONGLONG lr;
    ULONGLONG sp;
    ULONGLONG pc;
    DWORD Cpsr;
    DWORD ContextFlags;   // CONTEXT_*: the arm64 file has no separately selectable parts
} CONTEXT, *PCONTEXT, *LPCONTEXT;

typedef struct _EXCEPTION_POINTERS {
    PEXCEPTION_RECORD ExceptionRecord;
    PCONTEXT ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;

BOOL GetThreadContext(HANDLE thread, LPCONTEXT ctx);
BOOL SetThreadContext(HANDLE thread, const CONTEXT* ctx);

typedef LONG (*PVECTORED_EXCEPTION_HANDLER)(EXCEPTION_POINTERS*);
typedef LONG (*PTOP_LEVEL_EXCEPTION_FILTER)(EXCEPTION_POINTERS*);
typedef LONG (*LPTOP_LEVEL_EXCEPTION_FILTER)(EXCEPTION_POINTERS*);

PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler);
ULONG RemoveVectoredExceptionHandler(PVOID handle);
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter);
void RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR* args);

// The POSIX stand-in for MSVC __try/__except. The renderer guards a memory copy, a texture prepare or a user hook
// because the guest pages under it can be decommitted; clang has no SEH, so the callable runs under a sigsetjmp in
// the SIGSEGV/SIGBUS handler and a fault inside it returns 0 instead of killing the process. Vectored handlers run
// first, exactly as on Windows, so write_watch's un-protect-and-retry still wins. The callable must not own
// resources: a fault leaves it without unwinding. Callers use nxrt/fault_guard.h, which is SEH on Windows and this here.
int nxos_guarded_call(void (*fn)(void*), void* arg);   // 1 = returned normally, 0 = faulted

// ---- COM (no-op: the launcher and the media/audio backends are Windows-only) -------------------------------------
#define COINIT_APARTMENTTHREADED 0x2
#define COINIT_MULTITHREADED 0x0
#define COINIT_DISABLE_OLE1DDE 0x4
#define CLSCTX_INPROC_SERVER 0x1
#define CLSCTX_ALL 0x17
#define RPC_E_CHANGED_MODE ((HRESULT)0x80010106)
#define CO_E_NOTINITIALIZED ((HRESULT)0x800401F0)

HRESULT CoInitializeEx(void* reserved, DWORD flags);
void CoUninitialize(void);
HRESULT CoCreateInstance(REFCLSID clsid, void* outer, DWORD clsctx, REFIID iid, void** out);

#if defined(__cplusplus)

// ---- C++ overloads: the path/name parameters ----------------------------------------------------------------
#include <cstring>   // std::strchr: libstdc++'s <string.h> leaves the C functions in the global namespace only
#include <string>

std::string nxos_utf8(const char* s);
std::string nxos_utf8(const wchar_t* s);

NX_OS_INLINE HANDLE CreateFileW(LPCWSTR p, DWORD a, DWORD sh, void* sa, DWORD d, DWORD f, HANDLE t) {
    (void)t;
    return nxos_create_file(nxos_utf8(p).c_str(), a, sh, sa, d, f);
}
NX_OS_INLINE HANDLE CreateFileW(LPCSTR p, DWORD a, DWORD sh, void* sa, DWORD d, DWORD f, HANDLE t) {
    (void)t;
    return nxos_create_file(p, a, sh, sa, d, f);
}
NX_OS_INLINE HANDLE CreateFileA(LPCSTR p, DWORD a, DWORD sh, void* sa, DWORD d, DWORD f, HANDLE t) {
    (void)t;
    return nxos_create_file(p, a, sh, sa, d, f);
}
NX_OS_INLINE HANDLE CreateFileA(LPCWSTR p, DWORD a, DWORD sh, void* sa, DWORD d, DWORD f, HANDLE t) {
    (void)t;
    return nxos_create_file(nxos_utf8(p).c_str(), a, sh, sa, d, f);
}
NX_OS_INLINE BOOL DeleteFileW(LPCWSTR p) { return nxos_delete_file(nxos_utf8(p).c_str()); }
NX_OS_INLINE BOOL DeleteFileW(LPCSTR p) { return nxos_delete_file(p); }
NX_OS_INLINE BOOL DeleteFileA(LPCSTR p) { return nxos_delete_file(p); }
NX_OS_INLINE BOOL MoveFileExW(LPCWSTR a, LPCWSTR b, DWORD f) {
    const std::string x = nxos_utf8(a), y = nxos_utf8(b);
    return nxos_move_file(x.c_str(), y.c_str(), f);
}
NX_OS_INLINE BOOL MoveFileExW(LPCSTR a, LPCSTR b, DWORD f) { return nxos_move_file(a, b, f); }
NX_OS_INLINE BOOL MoveFileW(LPCWSTR a, LPCWSTR b) { return MoveFileExW(a, b, 0); }
NX_OS_INLINE BOOL MoveFileExA(LPCSTR a, LPCSTR b, DWORD f) { return nxos_move_file(a, b, f); }
NX_OS_INLINE BOOL MoveFileA(LPCSTR a, LPCSTR b) { return nxos_move_file(a, b, 0); }
NX_OS_INLINE BOOL CopyFileA(LPCSTR a, LPCSTR b, BOOL fail_if_exists) { return nxos_copy_file(a, b, fail_if_exists); }
NX_OS_INLINE BOOL CreateDirectoryA(LPCSTR p, void* sa) { (void)sa; return nxos_create_directory(p) != INVALID_HANDLE_VALUE; }
NX_OS_INLINE BOOL RemoveDirectoryA(LPCSTR p) { return nxos_remove_directory(p); }
NX_OS_INLINE DWORD GetPrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR def, LPSTR out, DWORD cap, LPCSTR file) {
    return nxos_private_profile_string(section, key, def, out, cap, file);
}
NX_OS_INLINE DWORD GetFullPathNameA(LPCSTR p, DWORD cap, LPSTR out, LPSTR* file_part) {
    if (file_part) *file_part = nullptr;
    return nxos_full_path(p, out, cap);
}
NX_OS_INLINE DWORD GetFullPathNameW(LPCWSTR p, DWORD cap, LPWSTR out, LPWSTR* file_part) {
    const std::string narrow = nxos_utf8(p);
    std::string buf(cap ? cap : 1, '\0');
    const DWORD n = nxos_full_path(narrow.c_str(), buf.data(), cap);
    if (file_part) *file_part = nullptr;
    if (n == 0 || n >= cap) return n;
    return static_cast<DWORD>(MultiByteToWideChar(CP_UTF8, 0, buf.c_str(), static_cast<int>(n), out, static_cast<int>(cap)));
}
NX_OS_INLINE BOOL CopyFileW(LPCWSTR a, LPCWSTR b, BOOL fail_if_exists) {
    const std::string x = nxos_utf8(a), y = nxos_utf8(b);
    return nxos_copy_file(x.c_str(), y.c_str(), fail_if_exists);
}
NX_OS_INLINE BOOL CopyFileW(LPCSTR a, LPCSTR b, BOOL fail_if_exists) { return nxos_copy_file(a, b, fail_if_exists); }
NX_OS_INLINE DWORD GetFileAttributesW(LPCWSTR p) { return nxos_get_file_attributes(nxos_utf8(p).c_str()); }
NX_OS_INLINE DWORD GetFileAttributesW(LPCSTR p) { return nxos_get_file_attributes(p); }
NX_OS_INLINE DWORD GetFileAttributesA(LPCSTR p) { return nxos_get_file_attributes(p); }
NX_OS_INLINE BOOL GetFileAttributesExW(LPCSTR path, int level, WIN32_FILE_ATTRIBUTE_DATA* data) {
    return nxos_get_file_attributes_ex(path, data);
}
NX_OS_INLINE BOOL SetFileAttributesW(LPCWSTR p, DWORD a) { return nxos_set_file_attributes(nxos_utf8(p).c_str(), a); }
NX_OS_INLINE BOOL SetFileAttributesW(LPCSTR p, DWORD a) { return nxos_set_file_attributes(p, a); }
NX_OS_INLINE HANDLE FindFirstFileExW(LPCWSTR pattern, int level, WIN32_FIND_DATAW* data, int search, void* filter, DWORD flags) {
    (void)level; (void)search; (void)filter; (void)flags;
    return nxos_find_first(nxos_utf8(pattern).c_str(), data);
}
NX_OS_INLINE HANDLE FindFirstFileExW(LPCSTR pattern, int level, WIN32_FIND_DATAW* data, int search, void* filter, DWORD flags) {
    (void)level; (void)search; (void)filter; (void)flags;
    return nxos_find_first(pattern, data);
}
NX_OS_INLINE HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW* data) {
    return nxos_find_first(nxos_utf8(pattern).c_str(), data);
}
NX_OS_INLINE HANDLE CreateDirectoryW(LPCWSTR p, void* sa) { (void)sa; return nxos_create_directory(nxos_utf8(p).c_str()); }
NX_OS_INLINE HANDLE CreateDirectoryW(LPCSTR p, void* sa) { (void)sa; return nxos_create_directory(p); }
NX_OS_INLINE BOOL RemoveDirectoryW(LPCWSTR p) { return nxos_remove_directory(nxos_utf8(p).c_str()); }
NX_OS_INLINE BOOL RemoveDirectoryW(LPCSTR p) { return nxos_remove_directory(p); }
// The process working directory: chdir on the UTF-8 path, with the Win32 separators the callers build converted.
NX_OS_INLINE BOOL SetCurrentDirectoryW(LPCWSTR p) {
    std::string path = nxos_utf8(p);
    for (char& c : path)
        if (c == '\\') c = '/';
    return ::chdir(path.c_str()) == 0;
}
NX_OS_INLINE BOOL SetCurrentDirectoryW(LPCSTR p) {
    std::string path = p ? p : "";
    for (char& c : path)
        if (c == '\\') c = '/';
    return ::chdir(path.c_str()) == 0;
}
// The MSVC CRT's _putenv("NAME=value"): setenv, and unsetenv when the value is empty.
NX_OS_INLINE int _putenv(const char* s) {
    if (!s) return -1;
    const char* eq = std::strchr(s, '=');
    if (!eq) return -1;
    const std::string name(s, static_cast<size_t>(eq - s));
    return eq[1] ? ::setenv(name.c_str(), eq + 1, 1) : ::unsetenv(name.c_str());
}
NX_OS_INLINE HMODULE LoadLibraryW(LPCWSTR p) { return nxos_load_library(nxos_utf8(p).c_str(), 0); }
NX_OS_INLINE HMODULE LoadLibraryW(LPCSTR p) { return nxos_load_library(p, 0); }
NX_OS_INLINE HMODULE LoadLibraryA(LPCSTR p) { return nxos_load_library(p, 0); }
NX_OS_INLINE HMODULE LoadLibraryExW(LPCWSTR p, HANDLE f, DWORD flags) { (void)f; return nxos_load_library(nxos_utf8(p).c_str(), flags); }
NX_OS_INLINE HMODULE LoadLibraryExW(LPCSTR p, HANDLE f, DWORD flags) { (void)f; return nxos_load_library(p, flags); }
NX_OS_INLINE DWORD GetModuleFileNameW(HMODULE mod, LPWSTR out, DWORD cap) {
    std::string buf(cap, '\0');
    const DWORD n = nxos_get_module_file_name(mod, buf.data(), cap);
    if (n == 0) return 0;
    const int w = MultiByteToWideChar(CP_UTF8, 0, buf.c_str(), static_cast<int>(n), out, static_cast<int>(cap));
    return w > 0 ? static_cast<DWORD>(w) : 0;
}
NX_OS_INLINE HMODULE GetModuleHandleW(LPCWSTR name) {
    if (!name || !*name) return GetModuleHandleA(nullptr);
    (void)name;   // there is no ntdll here: the callers that ask for it want its functions (NtDelayExecution, NtQuerySystemInformation, RtlWaitOnAddress), so report absence
    SetLastError(ERROR_MOD_NOT_FOUND);
    return nullptr;
}
NX_OS_INLINE BOOL FindClose(HANDLE h) { return CloseHandle(h); }
NX_OS_INLINE BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW* data) { return nxos_find_next(h, data); }
NX_OS_INLINE HANDLE CreateMutexW(void* sa, BOOL owner, LPCWSTR name) {
    return nxos_create_mutex(name ? nxos_utf8(name).c_str() : nullptr, owner);
}
NX_OS_INLINE HANDLE CreateMutexW(void* sa, BOOL owner, LPCSTR name) { (void)sa; return nxos_create_mutex(name, owner); }
NX_OS_INLINE HANDLE CreateMutexA(void* sa, BOOL owner, LPCSTR name) { (void)sa; return nxos_create_mutex(name, owner); }
NX_OS_INLINE HANDLE CreateEventW(void* sa, BOOL manual, BOOL initial, LPCWSTR name) {
    (void)sa;
    return nxos_create_event(name ? nxos_utf8(name).c_str() : nullptr, manual, initial);
}
NX_OS_INLINE HANDLE CreateWaitableTimerExW(void* sa, LPCWSTR name, DWORD flags, DWORD access) {
    (void)sa;
    return nxos_create_waitable_timer(name ? nxos_utf8(name).c_str() : nullptr, flags, access);
}
NX_OS_INLINE DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR out, DWORD cap) {
    return nxos_get_environment(name, out, cap);
}
NX_OS_INLINE LONG RegGetValueA(HKEY key, LPCSTR subkey, LPCSTR value, DWORD flags, LPDWORD type, LPVOID data, LPDWORD cap) {
    (void)key; (void)subkey; (void)value; (void)flags; (void)type; (void)data;
    if (cap) *cap = 0;
    return ERROR_FILE_NOT_FOUND;
}

#endif  // __cplusplus

#endif  // _WIN32

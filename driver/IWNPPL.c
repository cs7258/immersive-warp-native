// ============================================================
// IWNPPL.c - P2b kernel helper: strip PPL (protected process
// light) protection from dwm.exe so the user-mode warp console
// can manual-map its hook DLL into the desktop composer.
//
// Principle (user decision 2026-10-26):
//   Desktop-level warping ONLY through the DWM path. This driver
//   is NOT a display/GPU driver - it only clears the EPROCESS
//   Protection byte of dwm.exe, nothing else.
//
// How it works:
//   PsSetCreateProcessNotifyRoutineEx fires when dwm.exe is
//   created. Before dwm initializes, we locate the PS_PROTECTION
//   byte inside EPROCESS and zero it, so the process is born
//   unprotected (no WinTcb PPL, no code-integrity signer lock).
//   The IFEO mitigation overrides (CFG off etc.) do the rest.
//
// EPROCESS offset independence:
//   We never hardcode a layout. The driver queries the current
//   protection level via ZwQueryInformationProcess(
//   ProcessProtectionInformation = 61) - legal from kernel even
//   for PPL - then scans the first 0x900 bytes of EPROCESS for
//   that exact byte. A single match is zeroed. If the scan is
//   ambiguous, a known-good offset for 1909 (0x6CA) is tried as
//   a last resort. Both steps are logged via DbgPrint.
//
// Load (test-signing mode, admin):
//   sc create IWNPPL type= kernel binPath= C:\path\IWNPPL.sys
//   sc start IWNPPL
// Unload:
//   sc stop IWNPPL && sc delete IWNPPL
// ============================================================
#include <ntifs.h>   // PsLookupProcessByProcessId / ObOpenObjectByPointer
#include <ntstrsafe.h>

// ---- registry breadcrumbs (visibility without DbgView) -----------------
// Every interesting step writes a one-line status to
// HKLM\SOFTWARE\IWNPPL\Status (REG_SZ). User mode can read it with
// PowerShell: Get-ItemProperty HKLM:\SOFTWARE\IWNPPL
static HANDLE g_StatusKey = NULL;

static VOID Note(PCSTR msg)
{
    if (!g_StatusKey) return;
    WCHAR w[220];
    ULONG n = 0;
    while (msg[n] && n < 200) { w[n] = (WCHAR)(UCHAR)msg[n]; n++; }
    w[n] = 0;
    UNICODE_STRING vn = RTL_CONSTANT_STRING(L"Status");
    ZwSetValueKey(g_StatusKey, &vn, 0, REG_SZ, w, (ULONG)((n + 1) * sizeof(WCHAR)));
}

#define PROTECTION_QUERY_CLASS  ((PROCESSINFOCLASS)61)   // ProcessProtectionInformation
#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

// The WDK NuGet public headers do not declare these Zw* info-class APIs -
// declare them manually (C4013 confirmed there is no prior declaration).
NTKERNELAPI NTSTATUS ZwQueryInformationProcess(
    HANDLE ProcessHandle,
    PROCESSINFOCLASS ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength,
    PULONG ReturnLength);

typedef ULONG SYSTEM_INFORMATION_CLASS;
NTKERNELAPI NTSTATUS ZwQuerySystemInformation(
    SYSTEM_INFORMATION_CLASS SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength);
#define IWN_SYSTEM_PROCESS_INFORMATION 5
#define SCAN_LO                 0x200
#define SCAN_HI                 0x900
#define FALLBACK_OFFSET         0x6CA                    // 18363 (1909) known offset
#define TAG                     'PNWI'

static PVOID g_NotifyHandle = NULL;

// Locate and zero the PS_PROTECTION byte inside EPROCESS.
static VOID StripProtection(PEPROCESS proc, HANDLE pid)
{
    HANDLE h = NULL;
    ULONG prot4 = 0;            // class 61 may require a 4-byte buffer
    ULONG retLen = 0;
    NTSTATUS st;
    CHAR msg[160];

    st = ObOpenObjectByPointer(proc, OBJ_KERNEL_HANDLE, NULL,
                               PROCESS_QUERY_LIMITED_INFORMATION,
                               *PsProcessType, KernelMode, &h);
    if (!NT_SUCCESS(st)) {
        RtlStringCbPrintfA(msg, sizeof(msg),
            "pid %llu: ObOpenObjectByPointer failed %08x",
            (ULONG64)(ULONG_PTR)pid, st);
        DbgPrint("IWNPPL: %s\n", msg); Note(msg);
        return;
    }
    st = ZwQueryInformationProcess(h, PROTECTION_QUERY_CLASS,
                                   &prot4, sizeof(prot4), &retLen);
    ZwClose(h);
    if (!NT_SUCCESS(st)) {
        RtlStringCbPrintfA(msg, sizeof(msg),
            "pid %llu: query protection failed %08x",
            (ULONG64)(ULONG_PTR)pid, st);
        DbgPrint("IWNPPL: %s\n", msg); Note(msg);
        return;
    }
    {
        UCHAR level = (UCHAR)(prot4 & 0xFF);
        if (level == 0) return;          // already unprotected

        __try {
            PUCHAR base = (PUCHAR)proc;
            LONG matches = 0;
            SIZE_T hit = 0;
            BOOLEAN patched = FALSE;
            SIZE_T patchedAt = 0;

            for (SIZE_T i = SCAN_LO; i < SCAN_HI; i++) {
                if (base[i] == level) {
                    matches++;
                    hit = i;
                    if (matches > 1) break;
                }
            }
            if (matches == 1) {
                base[hit] = 0;
                patched = TRUE;
                patchedAt = hit;
            } else if (base[FALLBACK_OFFSET] == level) {
                base[FALLBACK_OFFSET] = 0;
                patched = TRUE;
                patchedAt = FALLBACK_OFFSET;
            }
            RtlStringCbPrintfA(msg, sizeof(msg),
                "pid %llu: lvl=%02x matches=%ld patched=%u at=%llx",
                (ULONG64)(ULONG_PTR)pid, level, matches,
                patched ? 1u : 0u, (ULONG64)patchedAt);
            DbgPrint("IWNPPL: %s\n", msg); Note(msg);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Note("pid ?: exception while scanning EPROCESS");
        }
    }
}

// EX callback: 3 params, info==NULL means the process is being destroyed.
static VOID DwmNotifyCb(HANDLE parentId, HANDLE processId,
                        PPS_CREATE_NOTIFY_INFO info)
{
    PEPROCESS proc = NULL;

    UNREFERENCED_PARAMETER(parentId);
    if (!info || !info->ImageFileName) return;   // destruction or no image

    // match image file name tail "dwm.exe", case-insensitive
    {
        PCUNICODE_STRING img = info->ImageFileName;
        UNICODE_STRING tail, dwm = RTL_CONSTANT_STRING(L"dwm.exe");
        if (img->Length < dwm.Length) return;
        tail.Buffer = img->Buffer + (img->Length - dwm.Length) / sizeof(WCHAR);
        tail.Length = tail.MaximumLength = dwm.Length;
        if (!RtlEqualUnicodeString(&tail, &dwm, TRUE)) return;
    }

    if (NT_SUCCESS(PsLookupProcessByProcessId(processId, &proc))) {
        StripProtection(proc, processId);
        ObDereferenceObject(proc);
    }
}

VOID DriverUnload(PDRIVER_OBJECT drv)
{
    UNREFERENCED_PARAMETER(drv);
    if (g_NotifyHandle) {
        PsSetCreateProcessNotifyRoutineEx(DwmNotifyCb, TRUE);
        g_NotifyHandle = NULL;
    }
    if (g_StatusKey) { ZwClose(g_StatusKey); g_StatusKey = NULL; }
    DbgPrint("IWNPPL: unloaded\n");
}

// ---- running-dwm sweep -------------------------------------------------
// x64 SYSTEM_PROCESS_INFORMATION header layout (stable across Win10).
#pragma pack(push, 8)
typedef struct _IWN_SPI {
    ULONG       NextEntryOffset;        // 0x00
    ULONG       NumberOfThreads;        // 0x04
    LARGE_INTEGER WorkingSetPrivateSize; // 0x08
    ULONG       HardFaultCount;         // 0x10
    ULONG       NumberOfThreadsHighWatermark; // 0x14
    ULONGLONG   CycleTime;              // 0x18
    LARGE_INTEGER CreateTime;           // 0x20
    LARGE_INTEGER UserTime;             // 0x28
    LARGE_INTEGER KernelTime;           // 0x30
    UNICODE_STRING ImageName;           // 0x38
    KPRIORITY   BasePriority;           // 0x48
    HANDLE      UniqueProcessId;        // 0x50
} IWN_SPI, *PIWN_SPI;
#pragma pack(pop)

static VOID SweepRunningDwm(VOID)
{
    ULONG len = 1 << 20;
    PVOID buf = NULL;
    NTSTATUS st = STATUS_INFO_LENGTH_MISMATCH;

    for (int tries = 0; tries < 6 && st == STATUS_INFO_LENGTH_MISMATCH; tries++) {
        if (buf) ExFreePoolWithTag(buf, TAG);
        buf = ExAllocatePoolWithTag(NonPagedPool, len, TAG);
        if (!buf) return;
        st = ZwQuerySystemInformation(IWN_SYSTEM_PROCESS_INFORMATION, buf, len, &len);
        if (!NT_SUCCESS(st) && st != STATUS_INFO_LENGTH_MISMATCH) {
            ExFreePoolWithTag(buf, TAG);
            return;
        }
    }
    if (st != STATUS_SUCCESS || !buf) {
        if (buf) ExFreePoolWithTag(buf, TAG);
        return;
    }

    ULONG cleared = 0, seen = 0;
    PUCHAR cur = (PUCHAR)buf;
    UNICODE_STRING dwm = RTL_CONSTANT_STRING(L"dwm.exe");
    for (;;) {
        PIWN_SPI p = (PIWN_SPI)cur;
        if (p->ImageName.Length >= dwm.Length) {
            UNICODE_STRING tail;
            tail.Buffer = p->ImageName.Buffer +
                          (p->ImageName.Length - dwm.Length) / sizeof(WCHAR);
            tail.Length = tail.MaximumLength = dwm.Length;
            if (RtlEqualUnicodeString(&tail, &dwm, TRUE) && p->UniqueProcessId) {
                seen++;
                PEPROCESS proc;
                if (NT_SUCCESS(PsLookupProcessByProcessId(p->UniqueProcessId, &proc))) {
                    StripProtection(proc, p->UniqueProcessId);
                    ObDereferenceObject(proc);
                    cleared++;
                }
            }
        }
        if (!p->NextEntryOffset) break;
        cur += p->NextEntryOffset;
    }
    ExFreePoolWithTag(buf, TAG);
    {
        CHAR msg[96];
        RtlStringCbPrintfA(msg, sizeof(msg),
            "sweep done: dwm seen=%lu cleared=%lu", seen, cleared);
        DbgPrint("IWNPPL: %s\n", msg); Note(msg);
    }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING regPath)
{
    NTSTATUS st;

    UNREFERENCED_PARAMETER(regPath);
    drv->DriverUnload = DriverUnload;

    st = PsSetCreateProcessNotifyRoutineEx(DwmNotifyCb, FALSE);
    if (!NT_SUCCESS(st)) {
        DbgPrint("IWNPPL: PsSetCreateProcessNotifyRoutineEx failed %08x\n", st);
        return st;
    }
    g_NotifyHandle = (PVOID)(ULONG_PTR)1;

    // registry breadcrumbs: HKLM\SOFTWARE\IWNPPL\Status
    {
        UNICODE_STRING path =
            RTL_CONSTANT_STRING(L"\\Registry\\Machine\\SOFTWARE\\IWNPPL");
        OBJECT_ATTRIBUTES oa;
        InitializeObjectAttributes(&oa, &path,
            OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        st = ZwCreateKey(&g_StatusKey, KEY_SET_VALUE, &oa, 0, NULL,
                         REG_OPTION_NON_VOLATILE, NULL);
    }
    if (NT_SUCCESS(st)) {
        Note("loaded ver=sweep3 (reg breadcrumbs on)");
    } else {
        g_StatusKey = NULL;
        DbgPrint("IWNPPL: status key create failed %08x\n", st);
    }

    // Also strip any dwm.exe that is already running (driver started
    // after logon) so no logoff/reboot is needed just to refresh dwm.
    SweepRunningDwm();

    DbgPrint("IWNPPL: loaded, watching dwm.exe\n");
    return STATUS_SUCCESS;
}

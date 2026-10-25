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
#include <ntddk.h>

#define PROTECTION_QUERY_CLASS  ((PROCESSINFOCLASS)61)   // ProcessProtectionInformation
#define SCAN_LO                 0x200
#define SCAN_HI                 0x900
#define FALLBACK_OFFSET         0x6CA                    // 18363 (1909) known offset
#define TAG                     'PNWI'

static PVOID g_NotifyHandle = NULL;

// Locate and zero the PS_PROTECTION byte inside EPROCESS.
static VOID StripProtection(PEPROCESS proc, HANDLE pid)
{
    HANDLE h = NULL;
    UCHAR level = 0;
    ULONG retLen = 0;
    NTSTATUS st;

    st = ObOpenObjectByPointer(proc, OBJ_KERNEL_HANDLE, NULL,
                               PROCESS_QUERY_LIMITED_INFORMATION,
                               *PsProcessType, KernelMode, &h);
    if (!NT_SUCCESS(st)) {
        DbgPrint("IWNPPL: pid %p ObOpenObjectByPointer failed %08x\n",
                 (PVOID)(ULONG_PTR)pid, st);
        return;
    }
    st = ZwQueryInformationProcess(h, PROTECTION_QUERY_CLASS,
                                   &level, sizeof(level), &retLen);
    ZwClose(h);
    if (!NT_SUCCESS(st)) {
        DbgPrint("IWNPPL: pid %p query protection failed %08x\n",
                 (PVOID)(ULONG_PTR)pid, st);
        return;
    }
    if (level == 0) return;              // already unprotected

    __try {
        PUCHAR base = (PUCHAR)proc;
        LONG matches = 0;
        SIZE_T hit = 0;

        for (SIZE_T i = SCAN_LO; i < SCAN_HI; i++) {
            if (base[i] == level) {
                matches++;
                hit = i;
                if (matches > 1) break;
            }
        }
        if (matches == 1) {
            base[hit] = 0;
            DbgPrint("IWNPPL: pid %p protection 0x%02x cleared at EPROCESS+0x%llx\n",
                     (PVOID)(ULONG_PTR)pid, level, (ULONG64)hit);
            return;
        }
        // ambiguous scan - try the 1909 known offset
        if (base[FALLBACK_OFFSET] == level) {
            base[FALLBACK_OFFSET] = 0;
            DbgPrint("IWNPPL: pid %p protection 0x%02x cleared via fallback +0x%x (scan hits=%ld)\n",
                     (PVOID)(ULONG_PTR)pid, level, FALLBACK_OFFSET, matches);
            return;
        }
        DbgPrint("IWNPPL: pid %p protection 0x%02x NOT cleared (scan hits=%ld)\n",
                 (PVOID)(ULONG_PTR)pid, level, matches);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        DbgPrint("IWNPPL: pid %p exception while scanning EPROCESS\n",
                 (PVOID)(ULONG_PTR)pid);
    }
}

static VOID DwmNotifyCb(HANDLE parentId, HANDLE processId, BOOLEAN create,
                        PPS_CREATE_NOTIFY_INFO info)
{
    PEPROCESS proc = NULL;

    UNREFERENCED_PARAMETER(parentId);
    if (!create || !info || !info->ImageFileName) return;

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
    DbgPrint("IWNPPL: unloaded\n");
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

    // No enumeration here: if a dwm.exe is already running when the
    // driver loads, just kill and restart the console later - dwm
    // restarts on session changes and the callback catches every new
    // instance at birth.
    DbgPrint("IWNPPL: loaded, watching dwm.exe\n");
    return STATUS_SUCCESS;
}

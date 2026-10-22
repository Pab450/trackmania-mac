/*
 * fastread: a dinput8.dll shim that speeds up Openplanet under Wine.
 *
 * Openplanet reads game memory through ReadProcessMemory(GetCurrentProcess(), ...), thousands
 * of times per second. On Windows that is a cheap syscall; in Wine every call is a round trip
 * to wineserver, which costs Trackmania a third of its frame rate with heavier plugins.
 *
 * This shim takes the place of Openplanet's dinput8.dll loader (renamed dinput8_openplanet.dll),
 * loads it, then patches Openplanet.dll's import of ReadProcessMemory: reads of our own process
 * are copied directly after a VirtualQuery check (local in Wine, no wineserver). Everything else
 * goes to the real ReadProcessMemory. Build: x86_64-w64-mingw32-gcc, see ../tm (fastread on).
 */
#include <windows.h>
#include <string.h>

#define MAX_FAST_SIZE 4096

typedef BOOL (WINAPI *rpm_fn)(HANDLE, LPCVOID, LPVOID, SIZE_T, SIZE_T *);
typedef HRESULT (WINAPI *di8create_fn)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);

/* ../tm recognizes the installed shim by this string (FASTREAD_MARKER). */
__attribute__((used)) static const char marker[] = "fastread: Openplanet ReadProcessMemory shim";

static rpm_fn real_rpm;
static HMODULE loader;

/* Copies n bytes and returns 1, or 0 if a read faults: the exception handler below resumes at
 * safe_copy_fail. Kept in assembly so the faulting range is known exactly. */
int safe_copy(void *dst, const void *src, size_t n);
extern char safe_copy_end[], safe_copy_fail[];
__asm__(
    ".text\n"
    ".globl safe_copy\n"
    "safe_copy:\n"
    "    test %r8, %r8\n"
    "    jz 2f\n"
    "1:  movb (%rdx), %al\n"
    "    movb %al, (%rcx)\n"
    "    inc %rdx\n"
    "    inc %rcx\n"
    "    dec %r8\n"
    "    jnz 1b\n"
    "2:  mov $1, %eax\n"
    "    ret\n"
    ".globl safe_copy_end\n"
    "safe_copy_end:\n"
    ".globl safe_copy_fail\n"
    "safe_copy_fail:\n"
    "    xor %eax, %eax\n"
    "    ret\n");

static LONG CALLBACK on_fault(EXCEPTION_POINTERS *e)
{
    char *pc = (char *)e->ContextRecord->Rip;
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        pc >= (char *)safe_copy && pc < safe_copy_end) {
        e->ContextRecord->Rip = (DWORD64)safe_copy_fail;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Whether [addr, addr + size) is committed and readable without touching a guard page. */
static BOOL readable(const char *addr, SIZE_T size)
{
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const char *end = addr + size;
    MEMORY_BASIC_INFORMATION mbi;
    if (end < addr) return FALSE;
    while (addr < end) {
        if (!VirtualQuery(addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            !(mbi.Protect & ok) || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return FALSE;
        addr = (const char *)mbi.BaseAddress + mbi.RegionSize;
    }
    return TRUE;
}

static BOOL WINAPI fast_rpm(HANDLE process, LPCVOID addr, LPVOID buf, SIZE_T size, SIZE_T *read)
{
    if (process == GetCurrentProcess() && size <= MAX_FAST_SIZE && readable(addr, size) &&
        safe_copy(buf, addr, size)) {
        if (read) *read = size;
        return TRUE;
    }
    return real_rpm(process, addr, buf, size, read);
}

/* Points module's import of kernel32!ReadProcessMemory at fast_rpm. */
static void patch_import(HMODULE module)
{
    char *base = (char *)module;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp;
    if (!dir.VirtualAddress) return;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++) {
        IMAGE_THUNK_DATA *names, *slots;
        if (_stricmp(base + imp->Name, "kernel32.dll") || !imp->OriginalFirstThunk) continue;
        names = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
        slots = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            DWORD old;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (strcmp(((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name,
                       "ReadProcessMemory"))
                continue;
            real_rpm = (rpm_fn)slots->u1.Function;
            VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            slots->u1.Function = (ULONG_PTR)fast_rpm;
            VirtualProtect(&slots->u1.Function, sizeof(void *), old, &old);
            return;
        }
    }
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
    WCHAR path[MAX_PATH];
    WCHAR *slash;
    HMODULE openplanet;
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);

    /* Openplanet's loader sits next to this file; loading it loads Openplanet.dll. */
    GetModuleFileNameW(self, path, MAX_PATH);
    slash = wcsrchr(path, L'\\');
    if (slash) wcscpy(slash + 1, L"dinput8_openplanet.dll");
    loader = LoadLibraryW(path);

    if ((openplanet = GetModuleHandleW(L"Openplanet.dll"))) {
        AddVectoredExceptionHandler(1, on_fault);
        patch_import(openplanet);
    }
    return TRUE;
}

/* The game's only dinput8 import. Openplanet's loader forwards it to the system dinput8.dll. */
HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD version, REFIID iid, LPVOID *out,
                                  LPUNKNOWN outer)
{
    WCHAR path[MAX_PATH];
    di8create_fn create = NULL;
    if (loader) create = (di8create_fn)(void (*)(void))GetProcAddress(loader, "DirectInput8Create");
    if (!create) {
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat(path, L"\\dinput8.dll");
        create = (di8create_fn)(void (*)(void))GetProcAddress(LoadLibraryW(path), "DirectInput8Create");
    }
    return create ? create(inst, version, iid, out, outer) : E_FAIL;
}

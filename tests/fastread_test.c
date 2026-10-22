/*
 * Runs fastread's ReadProcessMemory replacement under Wine. Built and run by test_fastread_wine.py.
 * Prints one "ok"/"FAIL" line per check and exits non-zero if any check fails.
 */
#include "../fastread/fastread.c"
#include <stdio.h>

static int failures;

static void check(int condition, const char *name)
{
    printf("%s %s\n", condition ? "ok" : "FAIL", name);
    if (!condition) failures++;
}

static char *commit_page(DWORD protect)
{
    return VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, protect);
}

static DWORD WINAPI free_and_reallocate(LPVOID page_slot)
{
    char *volatile *slot = page_slot;
    int i;
    for (i = 0; i < 20000; i++) {
        VirtualFree(*slot, 0, MEM_RELEASE);
        *slot = VirtualAlloc(*slot, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    return 0;
}

int main(void)
{
    char buf[8192], *page, *guarded, *reserved, *pair;
    static char big[16384];
    char *volatile racing_page;
    SIZE_T read;
    DWORD old;
    int i, completed = 0;
    HANDLE thread, other;

    real_rpm = ReadProcessMemory;
    AddVectoredExceptionHandler(1, on_fault);

    /* safe_copy recovers from faults on either side of the copy. */
    reserved = VirtualAlloc(NULL, 4096, MEM_RESERVE, PAGE_NOACCESS);
    page = commit_page(PAGE_READWRITE);
    check(safe_copy(buf, page, 16) == 1, "safe_copy copies readable memory");
    check(safe_copy(buf, reserved, 16) == 0, "safe_copy returns 0 on an unreadable source");
    check(safe_copy(reserved, page, 16) == 0, "safe_copy returns 0 on an unwritable destination");
    check(safe_copy(buf, NULL, 0) == 1, "safe_copy of 0 bytes touches nothing");

    /* Fast path: own process, small, readable. */
    memcpy(page, "fastread", 9);
    read = 0;
    check(fast_rpm(GetCurrentProcess(), page, buf, 9, &read) && read == 9 &&
              !memcmp(buf, "fastread", 9), "reads own memory");
    check(fast_rpm(GetCurrentProcess(), page, buf, 4, NULL), "accepts a NULL byte count");
    check(fast_rpm(GetCurrentProcess(), page, buf, 0, &read) && read == 0, "reads 0 bytes");

    /* Everything the fast path declines still behaves like ReadProcessMemory. */
    read = 1;
    check(!fast_rpm(GetCurrentProcess(), reserved, buf, 16, &read), "fails on reserved memory");
    check(!fast_rpm(GetCurrentProcess(), NULL, buf, 1, &read), "fails on NULL");
    check(!fast_rpm(GetCurrentProcess(), (void *)~(ULONG_PTR)0, buf, 16, &read),
          "fails on a wrapping range");

    guarded = commit_page(PAGE_READWRITE | PAGE_GUARD);
    fast_rpm(GetCurrentProcess(), guarded, buf, 16, &read);
    {
        MEMORY_BASIC_INFORMATION mbi;
        VirtualQuery(guarded, &mbi, sizeof(mbi));
        check(mbi.Protect & PAGE_GUARD, "leaves a guard page armed");
    }

    other = OpenProcess(PROCESS_VM_READ, FALSE, GetCurrentProcessId());
    check(fast_rpm(other, page, buf, 9, &read) && read == 9 && !memcmp(buf, "fastread", 9),
          "reads through a real process handle");
    CloseHandle(other);

    pair = VirtualAlloc(NULL, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    VirtualProtect(pair + 4096, 4096, PAGE_NOACCESS, &old);
    check(!fast_rpm(GetCurrentProcess(), pair + 4090, buf, 16, &read),
          "fails on a range that runs into a no-access page");
    check(fast_rpm(GetCurrentProcess(), pair + 4080, buf, 16, &read) && read == 16,
          "reads up to the edge of a no-access page");

    for (i = 0; i < (int)sizeof(big); i++) big[i] = (char)i;
    check(fast_rpm(GetCurrentProcess(), big, buf, sizeof(buf), &read) && read == sizeof(buf) &&
              !memcmp(buf, big, sizeof(buf)), "reads above MAX_FAST_SIZE through the real call");

    /* Another thread frees the page while we read it: no crash, every call returns. */
    racing_page = commit_page(PAGE_READWRITE);
    thread = CreateThread(NULL, 0, free_and_reallocate, (LPVOID)&racing_page, 0, NULL);
    while (WaitForSingleObject(thread, 0) == WAIT_TIMEOUT) {
        fast_rpm(GetCurrentProcess(), racing_page, buf, 4096, &read);
        completed++;
    }
    CloseHandle(thread);
    check(completed > 0, "survives a page freed during reads");

    printf("%d failure(s)\n", failures);
    return failures != 0;
}

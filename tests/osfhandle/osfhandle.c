// SPDX-License-Identifier: GPL-2.0
/* Does the UCRT's _open_osfhandle accept a handle whose GetFileType is
 * FILE_TYPE_UNKNOWN? \\.\MaliG52 (a KMDF device with no device type) is
 * one; so is a mailslot. */
#include <windows.h>
#include <io.h>
#include <stdio.h>

int main(void)
{
    HANDLE h = CreateMailslotA("\\\\.\\mailslot\\malikm-osfhandle-test", 0, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("CreateMailslot failed: %lu\n", GetLastError());
        return 1;
    }
    SetLastError(0);
    DWORD type = GetFileType(h);
    printf("GetFileType = %lu (UNKNOWN=%d), last error %lu\n", type, FILE_TYPE_UNKNOWN, GetLastError());
    int fd = _open_osfhandle((intptr_t)h, 0);
    printf("_open_osfhandle -> %d\n", fd);
    return 0;
}

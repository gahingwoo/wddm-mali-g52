// SPDX-License-Identifier: GPL-2.0
/*
 * etwrec: record Microsoft-Windows-DxgKrnl to an .etl, for WinPE, where
 * wpr -start failed with 0x80070002 and there is no logman or tracelog.
 *
 *   etwrec start X:\dxgkrnl.etl     start a session, all keywords, verbose
 *   etwrec stop                     stop it (the file is complete after this)
 *
 * Decode on Linux with tools/etw/dxgk-decode.py.
 *
 * Build: cl /W4 tools\windows\etwrec.c advapi32.lib
 */
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define SESSION L"wddm-mali-g52-dxgkrnl"

/* Microsoft-Windows-DxgKrnl */
static const GUID DxgKrnl = {0x802ec45a, 0x1e99, 0x4b83, {0x99, 0x20, 0x87, 0xc9, 0x82, 0x77, 0xba, 0x9d}};

static EVENT_TRACE_PROPERTIES *props(const wchar_t *file)
{
    size_t size = sizeof(EVENT_TRACE_PROPERTIES) + 2 * 1024 * sizeof(wchar_t);
    EVENT_TRACE_PROPERTIES *p = (EVENT_TRACE_PROPERTIES *)calloc(1, size);
    if (p == NULL)
        return NULL;
    p->Wnode.BufferSize = (ULONG)size;
    p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p->Wnode.ClientContext = 1;                 /* QPC timestamps */
    p->LogFileMode = EVENT_TRACE_FILE_MODE_SEQUENTIAL;
    p->BufferSize = 1024;                       /* KB */
    p->MinimumBuffers = 64;
    p->MaximumBuffers = 128;
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    p->LogFileNameOffset = sizeof(EVENT_TRACE_PROPERTIES) + 1024 * sizeof(wchar_t);
    if (file != NULL)
        wcsncpy_s((wchar_t *)((char *)p + p->LogFileNameOffset), 1024, file, _TRUNCATE);
    return p;
}

static int start(const wchar_t *file)
{
    TRACEHANDLE h = 0;
    EVENT_TRACE_PROPERTIES *p = props(file);
    ULONG st;

    if (p == NULL)
        return 1;
    /* A session left from an earlier run would make StartTrace fail. */
    (void)ControlTraceW(0, SESSION, props(NULL), EVENT_TRACE_CONTROL_STOP);
    st = StartTraceW(&h, SESSION, p);
    printf("StartTrace: %lu\n", st);
    if (st != ERROR_SUCCESS)
        return 2;
    st = EnableTraceEx2(h, &DxgKrnl, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                        TRACE_LEVEL_VERBOSE, 0xFFFFFFFFFFFFFFFFull, 0, 0, NULL);
    printf("EnableTraceEx2(DxgKrnl): %lu\n", st);
    return st == ERROR_SUCCESS ? 0 : 3;
}

static int stop(void)
{
    EVENT_TRACE_PROPERTIES *p = props(NULL);
    ULONG st;

    if (p == NULL)
        return 1;
    st = ControlTraceW(0, SESSION, p, EVENT_TRACE_CONTROL_STOP);
    printf("ControlTrace(STOP): %lu, events lost %lu, buffers written %lu\n",
           st, p->EventsLost, p->BuffersWritten);
    return st == ERROR_SUCCESS ? 0 : 2;
}

int wmain(int argc, wchar_t **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 3 && _wcsicmp(argv[1], L"start") == 0)
        return start(argv[2]);
    if (argc == 2 && _wcsicmp(argv[1], L"stop") == 0)
        return stop();
    printf("usage: etwrec start <file.etl> | etwrec stop\n");
    return 1;
}

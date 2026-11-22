// SPDX-License-Identifier: GPL-2.0
/*
 * ccdtest: what Windows' display configuration (CCD) knows about the
 * adapter's targets, and a topology change with its return code. For the
 * HDMI hot-plug work (docs/HDMI-HPD.md), where DisplaySwitch /internal
 * changed nothing and reported nothing.
 *
 *   ccdtest                list every path Windows has, active or not
 *   ccdtest internal       SetDisplayConfig(SDC_TOPOLOGY_INTERNAL | SDC_APPLY)
 *   ccdtest external       ... SDC_TOPOLOGY_EXTERNAL
 *   ccdtest clone|extend   ... SDC_TOPOLOGY_CLONE / _EXTEND
 *   ccdtest validate X     the same with SDC_VALIDATE instead of SDC_APPLY
 *
 * Run it in the user's session (not as SYSTEM). Exit code: 0 on success,
 * else the Win32 error SetDisplayConfig or QueryDisplayConfig returned.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *Tech(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY T)
{
    switch (T) {
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI: return "HDMI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL: return "INTERNAL";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED: return "DP_EMBEDDED";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER: return "OTHER";
    default: return "?";
    }
}

static int List(void)
{
    UINT32 np = 0, nm = 0;
    DISPLAYCONFIG_PATH_INFO *p;
    DISPLAYCONFIG_MODE_INFO *m;
    LONG r = GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &np, &nm);

    if (r != ERROR_SUCCESS) {
        printf("GetDisplayConfigBufferSizes: %ld\n", r);
        return (int)r;
    }
    p = (DISPLAYCONFIG_PATH_INFO *)calloc(np, sizeof(*p));
    m = (DISPLAYCONFIG_MODE_INFO *)calloc(nm, sizeof(*m));
    if (p == NULL || m == NULL)
        return ERROR_OUTOFMEMORY;
    r = QueryDisplayConfig(QDC_ALL_PATHS, &np, p, &nm, m, NULL);
    if (r != ERROR_SUCCESS) {
        printf("QueryDisplayConfig: %ld\n", r);
        return (int)r;
    }
    printf("%u paths, %u modes\n", np, nm);
    for (UINT32 i = 0; i < np; i++) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME tn;
        memset(&tn, 0, sizeof(tn));
        tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tn.header.size = sizeof(tn);
        tn.header.adapterId = p[i].targetInfo.adapterId;
        tn.header.id = p[i].targetInfo.id;
        (void)DisplayConfigGetDeviceInfo(&tn.header);
        printf("path %u: source %u -> target %u (%s) available %d active %d flags 0x%x "
               "rotation %d scaling %d monitor \"%ls\" edid %d\n",
               i, p[i].sourceInfo.id, p[i].targetInfo.id, Tech(p[i].targetInfo.outputTechnology),
               p[i].targetInfo.targetAvailable, (p[i].flags & DISPLAYCONFIG_PATH_ACTIVE) != 0,
               p[i].flags, p[i].targetInfo.rotation, p[i].targetInfo.scaling,
               tn.monitorFriendlyDeviceName, tn.flags.edidIdsValid);
    }
    free(p);
    free(m);
    return 0;
}

int main(int argc, char **argv)
{
    UINT32 topo = 0, how = SDC_APPLY;
    int a = 1;
    LONG r;

    if (List() != 0)
        return 1;
    if (argc > a && strcmp(argv[a], "validate") == 0) {
        how = SDC_VALIDATE;
        a++;
    }
    if (argc <= a)
        return 0;
    if (strcmp(argv[a], "internal") == 0)
        topo = SDC_TOPOLOGY_INTERNAL;
    else if (strcmp(argv[a], "external") == 0)
        topo = SDC_TOPOLOGY_EXTERNAL;
    else if (strcmp(argv[a], "clone") == 0)
        topo = SDC_TOPOLOGY_CLONE;
    else if (strcmp(argv[a], "extend") == 0)
        topo = SDC_TOPOLOGY_EXTEND;
    else {
        printf("unknown topology %s\n", argv[a]);
        return 1;
    }
    r = SetDisplayConfig(0, NULL, 0, NULL, topo | how);
    printf("SetDisplayConfig(%s%s): %ld\n", how == SDC_VALIDATE ? "validate " : "", argv[a], r);
    if (r == ERROR_SUCCESS && how == SDC_APPLY) {
        Sleep(3000);
        (void)List();
    }
    return (int)r;
}

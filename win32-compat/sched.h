/* SPDX-License-Identifier: GPL-2.0 */
/* sched_yield for the Vulkan runtime's syncobj code (vk_drm_syncobj.c). */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllimport) int __stdcall SwitchToThread(void);
static inline int sched_yield(void) { SwitchToThread(); return 0; }
#ifdef __cplusplus
}
#endif

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The GPU's temperature and clock as dxgkrnl's performance queries want
 * them (DxgkDdiQueryAdapterInfo types 24 to 27), the decisions only: no
 * register is touched here (adapter.c reads the TSADC and the CRU). Plain
 * C with no WDK header, so that tests/perfdata-host can build it.
 *
 * What asks, and when (docs/PERFDATA.md, read from dxgkrnl 10.0.22621.2428
 * and Taskmgr 10.0.22621.2361 with their public symbols):
 *
 *   24 NODEPERFDATA         DXGADAPTER::GetNodePerfData, for
 *                           D3DKMTQueryAdapterInfo(KMTQAITYPE_NODEPERFDATA)
 *                           at any WDDM version, and for D3DKMTQueryStatistics
 *                           at WDDM 2.4 and up. Input: one ULONG, the
 *                           physical adapter in the top 16 bits and the node
 *                           in the low 16; output 44 bytes.
 *   25 ADAPTERPERFDATA      DXGADAPTER::GetAdapterPerfData, the same two
 *                           ways. Input: one ULONG, the physical adapter;
 *                           output 53 bytes. Task Manager's GPU page asks it
 *                           (KMTQAITYPE_ADAPTERPERFDATA) and shows "GPU
 *                           Temperature" when Temperature / 10 is not 0.
 *   26 ADAPTERPERFDATA_CAPS ADAPTER_RENDER::Initialize, once, at WDDM 2.4
 *                           and up only; output 28 bytes. A failure is only
 *                           logged.
 *   27 GPUVERSION           the same; output 128 bytes.
 *
 * dxgkrnl calls the driver for 24 and 25 at most once every 450 ms per
 * node or physical adapter and answers from its copy in between; it does
 * not call while the adapter is in a D3 transition. The structures are
 * byte-packed in d3dkmdt.h (pragma pack 1), hence 44 and 53.
 */
#pragma once

#ifdef _MSC_VER
#define MW_PD_INLINE static __inline
#else
#define MW_PD_INLINE static inline
#endif

#define MW_QAI_NODEPERFDATA         24u
#define MW_QAI_ADAPTERPERFDATA      25u
#define MW_QAI_ADAPTERPERFDATA_CAPS 26u
#define MW_QAI_GPUVERSION           27u

/* The output sizes dxgkrnl 22621.2428 passes (pack 1): what the driver
 * must accept, and the most it writes. */
#define MW_PD_NODE_SIZE             44u
#define MW_PD_ADAPTER_SIZE          53u
#define MW_PD_CAPS_SIZE             28u
#define MW_PD_GPUVERSION_SIZE       128u

/* The trips the driver reports, deci-Celsius: mainline rk3576.dtsi's gpu
 * thermal zone, gpu-alert 85 C (where thermal_calc.h holds 594 MHz) and
 * gpu-crit 115 C. */
#define MW_PD_TEMP_WARNING_DC       850u
#define MW_PD_TEMP_MAX_DC           1150u

/* Which of these the driver answers. At WDDM 1.3 none (STATUS_NOT_SUPPORTED
 * as before: the 1.3 driver stays the 1.3 driver of main); at 2.x all four,
 * unless the setting PerfData is 0. */
MW_PD_INLINE int MwPerfAnswers(unsigned Type, unsigned WddmProbe, unsigned PerfData)
{
    if (WddmProbe == 0 || PerfData == 0)
        return 0;
    return Type >= MW_QAI_NODEPERFDATA && Type <= MW_QAI_GPUVERSION;
}

/* 24 and 25 come about twice a second for as long as something watches
 * (Task Manager open, a monitoring tool): the boot journal and the
 * registry trace get the first answer of each only. */
MW_PD_INLINE int MwPerfPolled(unsigned Type)
{
    return Type == MW_QAI_NODEPERFDATA || Type == MW_QAI_ADAPTERPERFDATA;
}

/* The input check of 24: physical adapter 0, a node that exists. */
MW_PD_INLINE int MwPerfNodeOk(unsigned long In, unsigned Nodes)
{
    return (In >> 16) == 0 && (In & 0xFFFFu) < Nodes;
}

/* DXGK_ADAPTER_PERFDATA.Temperature: deci-Celsius, rounded, from the
 * hotter TSADC channel in milli-Celsius. 0 when there is no reading, which
 * Task Manager takes as "no sensor" and hides. A valid reading below 1 C
 * is reported as 1 C (10): the TSADC on this board does not get there, and
 * 0 would read as no sensor. */
MW_PD_INLINE unsigned long MwPerfDeciC(int Valid, long MilliC)
{
    if (!Valid)
        return 0;
    if (MilliC < 1000)
        return 10;
    return (unsigned long)((MilliC + 50) / 100);
}

/* What Task Manager 22621.2361 (AtmGpuView::UpdateUtilization) shows for
 * a Temperature: whole degrees (value / 10), and nothing when that is 0.
 * The AMD (vendor 0x1002) rescaling of values of 100.0 C and more does not
 * apply to this adapter. Returns -1 for "hidden". */
MW_PD_INLINE long MwTaskmgrShows(unsigned long Temperature)
{
    unsigned long c = Temperature / 10;
    return c == 0 ? -1 : (long)c;
}

/* The GPU clock from CRU_CLKSEL_CON165 (adapter.c): mux [7:5], divider
 * minus one [4:0]; bit 8 is the PVTPLL path, whose rate this driver does
 * not know. Pll is MwGpuPll (Hz per mux value, 0 for one never taken).
 * 0 when the rate is not known. */
MW_PD_INLINE unsigned long MwPerfClkHz(unsigned long ClkSel165, const unsigned long Pll[8])
{
    unsigned long parent;

    if (ClkSel165 == 0xFFFFFFFFul || (ClkSel165 & 0x100u) != 0)
        return 0;
    parent = Pll[(ClkSel165 >> 5) & 7];
    return parent / ((ClkSel165 & 0x1Fu) + 1);
}

/* The highest rate the CRU can make from the PLLs this driver takes, with
 * a divider of 1 to 32, at or below Limit (Hz). For MaxFrequency, Limit is
 * 600 MHz (the default ceiling: GPLL / 2 = 594 MHz); for MaxFrequencyOC it
 * is the rate GpuClkHigh and the supply allow now (702 MHz, SPLL / 1, when
 * they allow it). */
MW_PD_INLINE unsigned long MwPerfBestHz(unsigned long Limit, const unsigned long Pll[8])
{
    unsigned long best = 0;

    for (unsigned p = 0; p < 8; p++) {
        if (Pll[p] == 0)
            continue;
        for (unsigned long d = 1; d <= 32; d++) {
            unsigned long hz = Pll[p] / d;
            if (hz <= Limit && hz > best)
                best = hz;
        }
    }
    return best;
}

/* ---- the "RK3576 GPU" performance counters (pcw.c, maliwddm.man) ----
 * Read only when a PDH consumer collects them; independent of the WDDM
 * version and of the queries above. */

/* "Temperature (C)": whole degrees, the same reading as Task Manager's
 * (MwPerfDeciC / 10, so the two never disagree); 0 without a reading. */
MW_PD_INLINE unsigned long MwPcwTempC(int Valid, long MilliC)
{
    return MwPerfDeciC(Valid, MilliC) / 10;
}

/* "Clock (MHz)": rounded to the nearest MHz (594, 702, 198). */
MW_PD_INLINE unsigned long MwPcwMhz(unsigned long Hz)
{
    return (Hz + 500000ul) / 1000000ul;
}

/* "% busy" is a perf_100nsec_timer: the busy time in 100 ns units, which
 * PDH divides by the elapsed time between two samples. The driver counts
 * QueryPerformanceCounter ticks (SlotTicks). No overflow for any uptime:
 * whole seconds and the remainder are scaled apart. */
MW_PD_INLINE unsigned long long MwPcwTicksTo100ns(unsigned long long Ticks, unsigned long long Hz)
{
    if (Hz == 0)
        return 0;
    return (Ticks / Hz) * 10000000ull + (Ticks % Hz) * 10000000ull / Hz;
}

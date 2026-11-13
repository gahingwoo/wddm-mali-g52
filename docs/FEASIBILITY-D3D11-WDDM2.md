# Direct3D 11 and WDDM 2.x for wddm-mali-g52: feasibility report

Written 2026-11-13 from the source of both trees and public documentation. Nothing in it was measured on a board. Since then the cause of the AppContainer fallback was found and fixed (see [results/tspi3m-2026-11-13](results/tspi3m-2026-11-13/README.md)).

## 1. Short answer

**Question 1, Direct3D 11.** Feature level 10_1 is a modest step. Feature level 11_0 is a new project. Feature level 11_1 is not fully reachable.

- **10_1:** the frontend already contains 10.1 code. It is switched off (`SUPPORT_D3D10_1 0`). Four SM4.1 opcodes and MSAA are missing.
- **11_0:** the frontend has no D3D11 DDI code at all. TGSI cannot carry SM5. The G52 has no geometry or tessellation hardware, so hull and domain shaders need compute emulation.
- **11_1:** it requires UAVs in every stage. Panfrost forbids memory writes in vertex shaders by design.

There is a correctness problem at today's 10_0. Feature level 10_0 promises geometry shaders and stream output. Panfrost has no `create_gs_state` hook, and the frontend calls it for every geometry shader and for every stream-output-only shader. Any application that uses either will crash on a NULL function pointer. DWM and LogonUI never call these (checked in the logs), which is why nothing has crashed yet.

- Recommended order for Q1: make GS and stream output honest at 10_0 first, then 10_1. Only start on 11_0 if a named application needs it.
- Biggest unknown for Q1: whether Mesa's own GS and tessellation emulation library (`src/poly`, used today only by two Vulkan drivers) can be wired into Panfrost on Bifrost.

**Question 2, WDDM 2.x.** Feasible, and the Mali fits the GpuMmu model well. The reasons it was set aside were "a 1.3 precedent exists (viogpu3d), and 1.3 is smaller". Those were sound before anything worked. One of them now argues for 2.0. WDDM 1.x only makes resident what is in each submission's allocation list, which forced the shim to list every live BO on every submit. WDDM 2.0's per-device residency matches Panfrost's model directly.

None of the slowness or the shell crashes is explained by WDDM 1.3 in anything I found.

- Recommended order for Q2: after the performance work (rendering into the primary, VOP2 scan-out). Insert one cheap experiment first: declare WDDM 2.0 while keeping physical mode, and see whether dxgkrnl accepts it and whether the Task Manager GPU page appears.
- Biggest unknown for Q2: whether the D3D runtime accepts a UMD that offers only the D3D10 DDI when the kernel driver is WDDM 2.x. If it does not, WDDM 2.0 drags in the D3D11 DDI work, and the two projects merge. Microsoft's documentation does not say.

## 2. Direct3D 11

### 2.1 What the frontend implements today (read from source)

These are in `~/Desktop/mesa-26.2-mali/src/gallium/frontends/d3d10umd/`:

- **Switches:** `State.h:39-40` has `SUPPORT_D3D10_1 0` and `SUPPORT_D3D11 0`. `SUPPORT_MSAA 0` is set too.
- **Versions offered:** `Adapter.cpp` `GetSupportedVersions` lists only D3D10_0, D3D10_0_x and D3D10_0_7. `CreateDevice` accepts the same three.
- **What the logs show:** `Interface 0xa0009` is `D3D10_0_7_DDI_INTERFACE_VERSION`, "D3D10.0 on Windows 7" (`d3d10umddi.h`). Every process in the 2026-11-13 logs shows it, and every one also logs `Version 0x177a`.
- **`GetCaps`:** it zeroes every query.
- **10.1 code that exists:** `Device.cpp` already fills `D3D10_1DDI_DEVICEFUNCS` (`CreateShaderResourceView1`, `CreateBlendState1`, `ResourceConvert`). It only runs if the switch is turned on.
- **D3D11 DDI code:** none. There are no `D3D11DDI_*` tables, no hull, domain or compute stages, no UAVs and no deferred contexts.
- **Size of each DDI table:** `D3D10DDI_DEVICEFUNCS` has 103 entry points. `D3D11DDI_DEVICEFUNCS` has 152, `D3D11_1DDI_DEVICEFUNCS` has 157 and `D3DWDDM1_3DDI_DEVICEFUNCS` has 166 (counted in the WDK header).
- **Stubs:**
  - 66 `LOG_UNSUPPORTED_ENTRYPOINT` sites are in the frontend's own `D3DKMT*` exports. Those are irrelevant on the hardware path.
  - The remaining unsupported entry points are `_Blt` (DXGI) and `ResourceResolveSubResource`.
  - 31 `LOG_UNSUPPORTED(...)` sites cover: render target views into buffers, two query types, `DrawAuto` without a source, 11 places in `ShaderTGSI.c`, partial stream-output masks, and the 1-bit text filter.
- **Shader parser (`ShaderParse.c`):**
  - The opcode table is sized `D3D10_SB_NUM_OPCODES`.
  - Its last four entries are the SM4.1 opcodes LOD, GATHER4, SAMPLE_POS and SAMPLE_INFO. All four are `OP_NOT_DONE`.
  - No SM5 opcode exists. `info = &opcode_info[opcode->type]` has no bounds check, so an SM5 token would index past the table. That cannot happen at 10_0, but it is a trap.
- **Geometry shaders:**
  - `CreateGeometryShader` and `CreateGeometryShaderWithStreamOutput` call `pipe->create_gs_state` (`Shader.cpp:688`, `:906`). They do so even when the shader code is NULL, which is the stream-output-only case.
  - `GsSetShader` calls `pipe->bind_gs_state`.
  - Panfrost sets neither hook. `pan_shader.c:750-756` sets only the VS and FS hooks, and `pan_screen.c` initialises shader caps only for VS, FS and CS.
  - Result: any GS or stream-output use is a NULL call. The softpipe origin of the frontend (GS through the draw module) explains why it never mattered before.

**Upstream (read via the GitHub mirror `chaotic-cx/mesa-mirror`; gitlab.freedesktop.org blocked me with Anubis):**

- Commit `1f9af627b12f` (Erik Faye-Lund, 2026-09-16, MR 44717) "remove d3d10umd frontend". The reason given: "one of the last holdouts that generate TGSI".
- `src/gallium/frontends` on main now holds only dri, hgl, lavapipe, mediafoundation, rusticl, teflon, torx, va and wgl.
- TGSI opcodes are being removed (commits 2026-08-01: "tgsi: remove more unused opcodes", LIT, DST, LOG/EXP).
- I found no current D3D11 DDI frontend work in Mesa. The only prior art is the 2010 `d3d1x` COM state tracker (lists.freedesktop.org mesa-commit, 2010-09) and an old `d3d11ddi.txt` note. Neither is a DDI UMD.

### 2.2 Requirements by feature level

Feature-level contents come from Microsoft's "Direct3D feature levels" table. DDI interface versions come from `d3d10umddi.h`. Hardware status comes from Panfrost source in the 26.2 tree.

| FL | Requirement | Panfrost / G52 status | Frontend status | Work needed |
|---|---|---|---|---|
| 10_0 | SM 4.0 VS and PS | supported | done (TGSI) | none |
| 10_0 | Geometry shader | **none**: no GS stage, no GS hooks, no adjacency primitive modes | claimed, crashes on use | emulate in compute (`src/poly`), or fail creation cleanly |
| 10_0 | Stream out | supported (lowered into VS; `max_stream_output_buffers`) | routed through GS, so it crashes | route SO-only "GS" onto the VS's `stream_output` |
| 10_0 | BC1 to BC5 | depends on `GPU_TEXTURE_FEATURES_0` bits; Panfrost has the BC formats in its table and gates them on those bits | maps DXGI BC to DXT | read the register (already returned by the GPU_INFO escape, index 18); CPU decode on upload if absent |
| 10_0 | 8192 textures, 8 render targets, occlusion queries, instancing, alpha-to-coverage | 16384 textures (`1 << 14`); queries and instancing yes; max render targets from tile-buffer size, not checked for G52 | done | check the RT count on the board |
| 10_0 | 16 input slots | Panfrost does not set it explicitly (gallium default) | done | verify |
| 10_1 | SM 4.1 (LOD, GATHER4, SAMPLE_POS, SAMPLE_INFO) | gather and LOD query exist (`texture_query_lod` is arch ≥ 9 only, so LOD may need lowering on v7) | `OP_NOT_DONE`; tgsi_to_nir has TG4 and LODQ | implement the 4 opcodes |
| 10_1 | Cube map arrays | `cube_map_array = true` | 10.1 SRV code exists | test |
| 10_1 | MSAA (4x on common formats: *recalled, unverified*) | `texture_multisample`; 4x asserted for small formats | `SUPPORT_MSAA 0`; resolve unsupported | enable MSAA, implement resolve |
| 10_1 | 32 input slots | not verified | n/a | check |
| 11_0 | D3D11 DDI (`D3D11_0_DDI_INTERFACE_VERSION` 11.10, or `D3DWDDM1_3` 11.16 on WDDM 1.3) | n/a | **absent** | a D3D11 DDI layer (152 to 166 entry points) |
| 11_0 | SM 5.0 | n/a | parser stops at SM4.1 | a new translator (2.3) |
| 11_0 | Hull and domain shaders | **no hardware**; no Panfrost support | absent | compute emulation (`src/poly` has a tessellator) |
| 11_0 | Compute shader | `compute = true`, 3D grids, 256 threads | absent | wire it up |
| 11_0 | UAVs in PS and CS, 8 slots | images and SSBOs allowed in FS and CS only | absent | wire it up |
| 11_0 | BC6H and BC7 | the hardware formats exist (PanVK lists `MALI_BC6H_*`, `MALI_BC7_UNORM`); enabled only if the texture-feature bits say so | absent | as BC1 to 5 |
| 11_0 | 16384 textures, DrawIndirect | yes (`draw_indirect`) | absent | wire it up |
| 11_0 | Deferred contexts | n/a | absent | none: the runtime emulates them when the driver does not report command lists ([threading caps](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/supporting-threading--command-lists--and-3-d-pipeline)) |
| 11_1 | UAVs at every stage, 64 slots | **VS memory writes forbidden** (`allow_side_effects = (i != MESA_SHADER_VERTEX)`, "trouble with IDVS") | absent | not achievable without running VS as compute |
| 11_1 | Logic ops, target-independent rasterization, UAV-only rendering at 16 samples | not established | absent | unknown |
| 11.1 DDI | Required for 10, 10.1 and 11-class hardware by Windows 8 certification: no-overwrite/discard, ClearView, 5551/565/4444, partial constant buffer updates ([software requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/software-requirements)) | mostly API-side | absent | certification rule, not a load rule (M5-PLAN's reading still holds) |

Under the D3D10 DDI the feature level is fixed by the interface version. Microsoft's render-only sample notes that hardware feature levels 9_x "go through the D3D9 DDI". So there is no honest "10_0 without GS" level. The fix is emulation, or a clean failure that removes the device.

### 2.3 Shader translation: TGSI is a dead end for SM5

**Read from source:** `tgsi_to_nir.c` (2946 lines) has no atomic opcodes, no shared memory and only tessellation semantics. It has no hull or domain stage handling. Its interpolation opcodes are marked `XXX`. Mesa main is deleting TGSI opcodes, and the frontend that produced TGSI was removed. Extending both ends (the frontend's TGSI emitter and tgsi_to_nir) means investing in code Mesa is removing.

There are three alternatives.

1. **DXBC to NIR directly in the frontend.** This replaces `ShaderTGSI.c` (2331 lines). SM4 is a known quantity: the existing parser plus the semantics already debugged in patches 0026 to 0037 carry over. It also removes tgsi_to_nir from the path, and with it a whole class of this week's bugs. This is the realistic route.
2. **DXBC to SPIR-V to NIR.** Use vkd3d-shader (LGPL-2.1, *licence from memory, unverified*) or DXVK's dxbc compiler, then Mesa's `spirv_to_nir`. Both handle SM5 hull, domain and compute today. The risk is that they emit Vulkan-style resource binding, which would need remapping to gallium slots.
3. **PanVK plus DXVK.** Not realistic on this GPU, for three reasons:
   - PanVK on arch 6 and 7 refuses to load without `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER` (`panvk_physical_device.c:401-410`).
   - On those arches it reports Vulkan 1.0 (`get_api_version`), with `geometryShader = false` and `tessellationShader = false`.
   - DXVK's device check marks `geometryShader`, `textureCompressionBC`, `dynamicRendering`, `synchronization2` and `timelineSemaphore` as required ([dxvk_device_info.cpp](https://raw.githubusercontent.com/doitsujin/dxvk/master/src/dxvk/dxvk_device_info.cpp)).

   On Windows a Vulkan ICD would also need its own WDDM user-mode path. DWM loads the D3D UMD named in the INF regardless.

The geometry and tessellation emulation exists in Mesa 26.2 as `src/poly` (MIT, from Asahi), with `poly_nir_lower_gs.c`, `poly_nir_lower_tess.c` and the D3D11 tessellator. Only Vulkan drivers use it (Asahi's `hk`, KosmicKrisp). No Gallium driver does.

### 2.4 Which programs need which level

| Program | Need | Evidence |
|---|---|---|
| DWM | runs at 10_0 | observed on the board |
| Direct2D and DirectWrite | runs at 10_0 | observed: d2dtext matches WARP |
| XAML shell (Start, Search) | not a feature-level problem: they fell back to softpipe (`tgsi_exec` asserts) | results README. *Inferred:* the target falls back to softpipe whenever Panfrost fails to open (`d3d10_gdi.c:347-380`). The shim opens a `NUL` placeholder fd (`malikm_drm.c:85`), which may fail inside an AppContainer. Not verified |
| Edge, Chromium (ANGLE) | 10_0 gives GLES 2.0 only (WebGL 1); 10_1 and up give GLES 3.0; ANGLE never offers 3.1 | [renderer11_utils.cpp](https://chromium.googlesource.com/angle/angle/+/refs/heads/main/src/libANGLE/renderer/d3d/d3d11/renderer11_utils.cpp) `GetMaximumClientVersion`. Whether Chromium keeps GPU compositing at 10_0: not verified |
| Windows Terminal | its D3D backend needs 11_0, or 10_x with compute shaders on 4.x; otherwise it uses its Direct2D backend | `AtlasEngine.r.cpp` (microsoft/terminal, read via `gh`) |
| Media playback | needs the D3D11 video DDI; d3d10umd has none | not verified what Media Foundation falls back to |
| Games | most current titles want 11_0 | *recalled, unverified* |

Conclusion: 10_0 is livable for the desktop shell. 10_1 buys WebGL 2 in Edge. 11_0 buys Terminal's GPU path and games.

### 2.5 Staged plan

Every estimate assumes one person, with CI and the board rig as they are now.

- **S0, honest 10_0:** route SO-only shaders to Panfrost VS stream output, and make GS creation fail cleanly with device removal instead of a NULL call. **2 to 5 days.** Low risk.
- **S0b, real GS:** port `src/poly` GS lowering into Panfrost. **3 to 8 weeks.** This depends on:
  - building poly's OpenCL kernels through the existing Linux-side `mesa_clc` route;
  - Bifrost having what poly relies on (indirect draws, compute writes feeding draws);
  - the fact that no Gallium driver has done it before.
- **S1, 10_1:** turn on `SUPPORT_D3D10_1`, implement the 4 opcodes, enable MSAA and resolve, check 32 input slots. **1 to 3 weeks.** Depends on how much of MSAA works in Panfrost via this frontend. It has never been exercised.
- **S2, 11_0:** a DXBC to NIR translator (SM4 then SM5), the D3D11 DDI tables (start from `D3DWDDM1_3`, as Microsoft's render-only sample does), compute and UAVs, and tessellation through `src/poly`. **4 to 9 months.** The range is wide because tessellation emulation and SM5 coverage have no precedent here.
- **Not achievable:** hardware geometry shaders and tessellation (the hardware has neither), and full 11_1 (UAVs in VS) without redesigning Panfrost's vertex path.

## 3. WDDM 2.x

### 3.1 Why M5-PLAN set it aside, and whether that still holds

M5.2-PLAN "replaces the WDDM 2.0 / GpuMmu route ... with something smaller that has been done before". Its reasons, and how each stands now:

1. **viogpu3d is WDDM 1.3 with a d3d10umd-based UMD.** Still true, but it no longer decides anything: maliwddm 1.3 works and is proven on 23H2.
2. **"Panfrost needs a buffer's GPU address when it creates the buffer."** GpuMmu gives this too. The UMD maps right after `pfnAllocateCb`, and VidMm can pick the address ([GpuMmu model](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpummu-model)). That is exactly Panfrost's `AUTO_VA` contract (`panfrost_kmod.c:535-576`). The reason is neutral now.
3. **"VidMm guarantees residency of listed allocations."** In practice this forced the 2026-10-17 workaround: each SUBMIT lists every live BO (`malikm_drm.c:1568`). WDDM 2.0 moves residency to a per-device list ([driver residency](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/driver-residency-in-wddm-2-0)). This reason now favours 2.0.
4. **Open risks (35-bit VA, a software paging node).** Both are weakened by Microsoft's compute-only sample:
   - It is WDDM 2.6 GpuMmu, with `VirtualAddressBitCount = 0x20` (32 bits).
   - It uses 2 levels and `PageTableUpdateMode = CPU_VIRTUAL`.
   - It does `UPDATE_PAGE_TABLE` as a CPU copy inside `BuildPagingBuffer`.

   This comes from `compute-only-sample/coskmd/CosKmdAdapter.cpp`, read via `gh`. It is a sample, not proof that VidMm's paging process is happy with a 35-bit space.

### 3.2 What changes between 1.3 and 2.0 (documentation)

**Memory model.** Per-process GPU VA. Engines run in physical mode (allocation and patch lists as in 1.x) or virtual mode (no lists, `pfnSubmitCommandCb`) ([GPU virtual memory](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-virtual-memory-in-wddm-2-0)). The driver opts in with `VirtualAddressingSupported` plus `GpuMmuSupported` or `IoMmuSupported`, never both ([DXGK_VIDMMCAPS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_vidmmcaps)).

**Kernel DDIs for GpuMmu:**

- `DXGKQAITYPE_GPUMMUCAPS` (13), `PAGETABLELEVELDESC` (14), `PHYSICALADAPTERCAPS` (15) and `QUERYSEGMENT4` (11) ([QAI types](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ne-d3dkmddi-_dxgk_queryadapterinfotype)).
- `DxgkDdiCreateProcess` and `DestroyProcess`.
- `DxgkDdiSetRootPageTable`, which is called before a process's context runs, with the context idle ([SETROOTPAGETABLE](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_setrootpagetable)).
- `DxgkDdiGetRootPageTableSize`, needed only when `PageTableLevelCount` is 2 ([GPUMMUCAPS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_gpummucaps)).
- New paging operations: `UPDATE_PAGE_TABLE`, `FLUSH_TLB`, `COPY_PAGE_TABLE_ENTRIES`, `NOTIFY_RESIDENCY` and `UPDATE_CONTEXT_ALLOCATION`. TRANSFER and FILL are listed as "WDDMv1 only", and map/unmap aperture is common to both ([paging ops](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ne-d3dkmddi-_dxgk_buildpagingbuffer_operation)).
- `DxgkDdiSubmitCommandVirtual`.
- Monitored fences (context monitoring) ([WDDM 2.0 features](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/wddm-2-0-and-windows-10)).

**UMD callbacks** (in `d3dumddi.h`, under `D3D_UMD_INTERFACE_VERSION_WDDM2_0`): `pfnMakeResidentCb`, `pfnEvictCb`, `pfnCreatePagingQueueCb`, `pfnReserve/Map/Free/UpdateGpuVirtualAddressCb`, `pfnCreateContextVirtualCb`, `pfnSubmitCommandCb`, `pfnLock2Cb`, and the CPU/GPU sync-object waits and signals. The UMD takes over allocation usage tracking and renaming synchronisation.

**IoMmu does not apply.** It needs a PASID-capable system IOMMU sharing the CPU page tables ([IoMmu model](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/iommu-model)). The Mali has its own MMU.

**What is mandatory for a 2.x driver on 23H2:** I found no page that states it. Hardware scheduling and later QAI types are opt-in by their own pages. Windows 11's published minimum is "DirectX 12 or later with WDDM 2.0 driver" ([specs](https://www.microsoft.com/en-us/windows/windows-11-specifications)). 23H2 loads 1.3 regardless (observed).

### 3.3 How it maps to Bifrost and the current code

**Today (read from source):**

- One adapter-wide address space. `maligpu.c` `AsEnable` programs only AS0, and `MkJobStart` puts AS0 in every `JS_CONFIG`.
- A single VA bitmap from 32 MB to 4 GB (`MK_VA_START`, `MK_VA_END`). Fixed VA per allocation (`memory.c` `NewAllocation`, `MkVaAlloc`).
- Map and unmap on the CPU inside `MwBuildPagingBuffer`. One aperture segment of 1 GB (`MW_SEGMENT_SIZE`).
- Every process can reach every mapped allocation by VA.

**Hardware:**

- AS_PRESENT is `0xff`, so 8 address spaces. JS_PRESENT is `0x7`, so 3 job slots. MMU_FEATURES `0x2823` gives 35 VA bits and 40 PA bits. These are this board's values per the project memory note, from Linux dmesg.
- Linux Panfrost already does per-process address spaces on this GPU. `panfrost_mmu_as_get` assigns an AS per file and LRU-evicts an idle one when all 8 are taken (`panfrost_mmu.c:262-327`, linux-next-v8 tree).
- G52 has `HW_FEATURE_AARCH64_MMU` (`panfrost_features.h`), so the AArch64 4K format with `INA_BITS` is available. With 35 bits that is a 3-level walk. *Inferred:* this suits `PageTableLevelCount = 3` better than the private level-0 trick in M5-PLAN.

**Model:** GpuMmu. `SetRootPageTable` would record the root for the process. Job submit would acquire an AS slot for that root (Linux's LRU) and write the slot into `JS_CONFIG`. AS switching needs the slot idle, which matches the documented guarantee that the context is idle during root updates. Monitored fences: append a WRITE_VALUE job to each chain, which is already proven in M1/M3. Paging: CPU-virtual updates at build time, as the compute-only sample does.

**What the hardware cannot give:**

- More than 8 simultaneously resident processes. Beyond 8, AS slots must be stolen, as Linux does.
- Unknown: whether VidMm places system or paging-process mappings above 2^35.
- Unknown: whether a fixed root smaller than 4 KB (32 entries at 35 bits) is acceptable in `DXGK_PAGE_TABLE_LEVEL_DESC`.

**UMD side:** simpler.

- Panfrost's Gallium path uses `AUTO_VA` only (`pan_device.c:58`, `pan_bo.c:76`). `pfnMapGpuVirtualAddressCb` with VidMm choosing the address is the same contract.
- The `MW_ESCAPE_ALLOC_INFO` escape, the patch list and the every-live-BO list would go. In their place come `pfnMakeResidentCb` at BO creation and `pfnSubmitCommandCb`.
- Shared surfaces need a map in the opening process. Today one adapter-wide VA makes that implicit.

### 3.4 Gains

| Gain | Evidence |
|---|---|
| Task Manager GPU page | the Microsoft DirectX blog: "requires a GPU driver that supports WDDM version 2.0 or above"; data "gathered directly from VidSch and VidMm" ([blog](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/)). Engine names come from node metadata (`MwGetNodeMetadata` already reports "Mali-G52 job manager"). `NODEPERFDATA` and `ADAPTERPERFDATA` are 1803 additions for clocks and temperatures, not for the page itself |
| Per-process GPU isolation | GpuMmu docs; today all processes share AS0 |
| No per-submit residency list | residency docs; removes the shim workaround |
| D3D12 | WDDM 2.0 is a prerequisite (search results citing Microsoft). Irrelevant here |
| MPO, HDR, Miracast, Edge GPU process isolation, WSL | **not established**. The documentation I read does not tie them to 2.0, so I make no claim |
| Today's slowness | not fixed by 2.x. *Inferred:* it is two full-frame CPU copies per present (results README) |
| Today's AppContainer softpipe fallback | not fixed by 2.x. *Inferred:* it is the device-open path |

### 3.5 Costs, prior art and plan

**Prior art:**

- **viogpu3d:** WDDM 1.3 (repo docs).
- **Microsoft render-only sample:** also **WDDM 1.3**, not 2.x. `m_WDDMVersion = DXGKDDI_WDDMv1_3`, and `CreateProcess` and `SubmitCommandVirtual` return NOT_IMPLEMENTED. Its UMD offers `D3DWDDM1_3_DDI_SUPPORTED` and pipeline levels 10_0 to 11_1 (VC4 builds cap at 9_3). That is useful for the D3D11 work, not for WDDM 2.
- **Microsoft compute-only sample:** the GpuMmu reference. WDDM 2.6, CPU-virtual page tables, `SubmitCommandVirtual`. It is for an emulated compute engine with a D3D12-style UMD, so it says nothing about a D3D10 UMD.
- **Mesa d3d10umd:** a software DDI with no WDDM model of its own.

**Plan:**

- **W0, smallest step toward the GPU page:** set `WDDMVersion` to v2.0 and the interface to WDDM2_0, and keep physical mode. The documentation allows physical-mode engines under 2.0, but does not say a driver may leave VA off. Answer any new QAI types, and read the dxgkrnl ETW event 494 text. **2 to 5 days.** This one experiment answers four things: does dxgkrnl accept it, does the D3D10-DDI UMD still load, does the runtime fill the 2.0 callbacks, and does the GPU page appear.
- **W1, GpuMmu in the kernel driver:** caps, level descriptors, process and root table, paging operations, AS slot LRU, `SubmitCommandVirtual`, monitored fences. **4 to 10 weeks.**
- **W2, UMD and shim on 2.0 callbacks:** **2 to 5 weeks.**

Together this is a rewrite of the memory and submission half (`memory.c`, `command.c`, the shim's backend). The display half and `maligpu.c` power and jobs carry over. A driver can offer physical and virtual modes per engine, so W0 then W1 is incremental. The estimates assume W0 succeeds. If it shows that the D3D11 DDI is required, add S2.

**Recommendation:** run W0 when convenient. Do W1 and W2 after the performance work, and only if isolation or the Task Manager page is wanted.

## 4. Interactions

- **D3D11 does not need WDDM 2.0.** Microsoft's feature-level table lists WDDM 1.x for every D3D11 level up to 12_1. The render-only sample pairs a `D3DWDDM1_3` UMD (D3D11.1 DDI) with a 1.3 kernel driver.
- **WDDM 2.0 may need the D3D11 DDI.** This is unknown and is the main coupling risk. W0 measures it.
- **Performance work first:** rendering into primaries and VOP2 scan-out are independent of the WDDM version. VOP2 needs contiguous memory below 4 GB (or its IOMMU) under either model. A scan-out segment designed now carries over. *Inferred:* the per-submit residency list may cost time too, but that is unmeasured.
- **GS emulation through `src/poly` and the 11_0 hull and domain work share infrastructure.** Do S0b with S2 in mind.

## 5. Sources, and what I could not verify

**Source code:**

- `~/Desktop/mesa-26.2-mali`: `d3d10umd/*`, `targets/d3d10umd/d3d10_gdi.c`, `panfrost/pan_screen.c`, `pan_shader.c`, `lib/kmod/panfrost_kmod.c`, `vulkan/panvk_*`, `auxiliary/nir/tgsi_to_nir.c`, `src/poly`.
- `~/Desktop/wddm-mali-g52`: `drivers/common/maligpu.c`, `drivers/maliwddm/{memory,adapter,command}.c`, `win32-compat/malikm_drm.c`, docs, and the 2026-11-13 logs.
- Linux `panfrost_mmu.c` and `panfrost_features.h` (linux-next-v8).
- WDK `d3d10umddi.h` and `d3dumddi.h` (copies in scratchpad `hdr/`).
- Upstream Mesa via `chaotic-cx/mesa-mirror`.
- `microsoft/graphics-driver-samples` and `microsoft/terminal` via `gh`.

**Pages read:**

- [Software requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/software-requirements)
- [GPU virtual memory](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-virtual-memory-in-wddm-2-0)
- [GpuMmu model](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpummu-model)
- [IoMmu model](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/iommu-model)
- [DXGK_GPUMMUCAPS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_gpummucaps)
- [DXGK_VIDMMCAPS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_vidmmcaps)
- [DXGK_PHYSICALADAPTERCAPS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_physicaladaptercaps)
- [QAI types](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ne-d3dkmddi-_dxgk_queryadapterinfotype)
- [Paging operations](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ne-d3dkmddi-_dxgk_buildpagingbuffer_operation)
- [WDDM 2.0 features](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/wddm-2-0-and-windows-10)
- [Driver residency](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/driver-residency-in-wddm-2-0)
- [Feature levels](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-devices-downlevel-intro)
- [Task Manager blog](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/)
- [Windows 11 specs](https://www.microsoft.com/en-us/windows/windows-11-specifications)
- [ANGLE renderer11_utils](https://chromium.googlesource.com/angle/angle/+/refs/heads/main/src/libANGLE/renderer/d3d/d3d11/renderer11_utils.cpp)
- [DXVK device info](https://raw.githubusercontent.com/doitsujin/dxvk/master/src/dxvk/dxvk_device_info.cpp)
- [Phoronix on the removal](https://www.phoronix.com/news/Mesa-Drops-Direct3D-10-UMD) (search summary only)

**Could not reach or could not verify:**

- **Unreachable:** gitlab.freedesktop.org (Anubis), so the MR 44717 page and any MR-level D3D11 work went unread; the Phoronix TGSI article (403); a "what's new in WDDM 2.0" page (404).
- **Undocumented as far as I found:**
  - which DDIs and QAIs a 2.0 driver must implement to load;
  - whether physical-only 2.0 is accepted;
  - whether a D3D10-DDI UMD works under a 2.0 kernel driver;
  - VidMm's VA placement versus 35 bits;
  - what MPO, HDR, Edge isolation and WSL need.
- **Not measured on the board:** the G52's `TEXTURE_FEATURES` (BC support); its max render targets; 32 input slots; 10.1 MSAA rules (recalled); the vkd3d licence (recalled); the AppContainer fallback cause; Chromium's behaviour at 10_0; games' minimum level.

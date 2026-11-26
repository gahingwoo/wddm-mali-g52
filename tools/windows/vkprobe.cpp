// SPDX-License-Identifier: GPL-2.0
//
// vkprobe: what vulkaninfo says about the Mali under panvk, then a little
// GPU work through it, with no Vulkan loader in between.
//
//   vkprobe.exe [<path to the ICD DLL> | loader] [--emulated] [--window <seconds>]
//
// The first argument is the ICD to load (malivk_arm64.dll, or Mesa's
// vulkan_panfrost.dll in CI). The tool asks it for vk_icdGetInstanceProcAddr
// itself, so it runs where no vulkan-1.dll is installed (GitHub's runners)
// and tests the driver alone. "loader" instead loads vulkan-1.dll, which
// finds the driver the way a real program does (VulkanDriverName in the
// adapter's key, or VK_DRIVER_FILES). Without an argument: malivk_arm64.dll
// next to the tool, else "loader".
//
// panvk turns the G52 (Bifrost v7) down unless
// PAN_I_WANT_A_BROKEN_VULKAN_DRIVER is set; the tool sets it in its own
// environment before the driver is loaded.
//
// It prints, in order:
//   - the instance version, the physical devices, and for the first one its
//     properties, the LUID, some limits, the features DXVK-class programs
//     ask for, memory heaps and types, queue families, a few formats and
//     every device extension;
//   - fences and events before and after GPU work: an unsubmitted fence
//     must read "not ready" and time out at once, a submitted one must
//     signal, a host-set event must read set (these hold on the emulated
//     driver too);
//   - a fill and a copy of a buffer, checked on the CPU;
//   - a render pass that clears a 256x256 image, copied to a buffer and
//     checked;
//   - with --window <s>: a window, a Win32 surface, a swap chain, and
//     frames cleared to a cycling colour and presented for that long, with
//     the rate.
//
// --emulated: the GPU executes nothing (MALIKM_FAKE): the buffer and image
// contents are printed but not judged. Everything else still is.
//
// Exit code: 0 all good; 1 a GPU result was wrong; 2 no Vulkan device;
// 3 a fence or event answered wrongly; 4 a Vulkan call failed; 5 the
// window or swap chain could not be made; 100 the driver did not load.
//
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

typedef VkResult(VKAPI_PTR *PFN_negotiate)(uint32_t *);

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;
static PFN_vkGetDeviceProcAddr gdpa;
static int emulated;

#define IFN(n) static PFN_##n p##n
#define DFN(n) static PFN_##n p##n
IFN(vkCreateInstance);
IFN(vkEnumerateInstanceVersion);
IFN(vkEnumerateInstanceExtensionProperties);
IFN(vkEnumeratePhysicalDevices);
IFN(vkGetPhysicalDeviceProperties);
IFN(vkGetPhysicalDeviceProperties2KHR);
IFN(vkGetPhysicalDeviceFeatures);
IFN(vkGetPhysicalDeviceMemoryProperties);
IFN(vkGetPhysicalDeviceQueueFamilyProperties);
IFN(vkGetPhysicalDeviceFormatProperties);
IFN(vkEnumerateDeviceExtensionProperties);
IFN(vkCreateDevice);
IFN(vkGetDeviceProcAddr);
IFN(vkCreateWin32SurfaceKHR);
IFN(vkGetPhysicalDeviceSurfaceSupportKHR);
IFN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
IFN(vkGetPhysicalDeviceSurfaceFormatsKHR);
IFN(vkDestroySurfaceKHR);
IFN(vkDestroyInstance);
DFN(vkGetDeviceQueue);
DFN(vkCreateBuffer);
DFN(vkGetBufferMemoryRequirements);
DFN(vkAllocateMemory);
DFN(vkBindBufferMemory);
DFN(vkMapMemory);
DFN(vkCreateCommandPool);
DFN(vkAllocateCommandBuffers);
DFN(vkBeginCommandBuffer);
DFN(vkEndCommandBuffer);
DFN(vkCmdFillBuffer);
DFN(vkCmdCopyBuffer);
DFN(vkCmdPipelineBarrier);
DFN(vkCmdBeginRenderPass);
DFN(vkCmdEndRenderPass);
DFN(vkCmdCopyImageToBuffer);
DFN(vkQueueSubmit);
DFN(vkQueueWaitIdle);
DFN(vkCreateFence);
DFN(vkGetFenceStatus);
DFN(vkWaitForFences);
DFN(vkResetFences);
DFN(vkCreateEvent);
DFN(vkGetEventStatus);
DFN(vkSetEvent);
DFN(vkResetEvent);
DFN(vkCreateImage);
DFN(vkGetImageMemoryRequirements);
DFN(vkBindImageMemory);
DFN(vkCreateImageView);
DFN(vkCreateRenderPass);
DFN(vkCreateFramebuffer);
DFN(vkResetCommandBuffer);
DFN(vkCreateSemaphore);
DFN(vkCreateSwapchainKHR);
DFN(vkGetSwapchainImagesKHR);
DFN(vkAcquireNextImageKHR);
DFN(vkQueuePresentKHR);
DFN(vkDeviceWaitIdle);
DFN(vkDestroySwapchainKHR);
DFN(vkDestroyDevice);

static void
load_instance_fns(VkInstance i)
{
#define L(n) p##n = (PFN_##n)gipa(i, #n)
   L(vkEnumeratePhysicalDevices);
   L(vkGetPhysicalDeviceProperties);
   L(vkGetPhysicalDeviceProperties2KHR);
   L(vkGetPhysicalDeviceFeatures);
   L(vkGetPhysicalDeviceMemoryProperties);
   L(vkGetPhysicalDeviceQueueFamilyProperties);
   L(vkGetPhysicalDeviceFormatProperties);
   L(vkEnumerateDeviceExtensionProperties);
   L(vkCreateDevice);
   L(vkGetDeviceProcAddr);
   L(vkCreateWin32SurfaceKHR);
   L(vkGetPhysicalDeviceSurfaceSupportKHR);
   L(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
   L(vkGetPhysicalDeviceSurfaceFormatsKHR);
   L(vkDestroySurfaceKHR);
   L(vkDestroyInstance);
#undef L
}

static int
load_device_fns(VkDevice d)
{
   int missing = 0;
#define L(n)                                               \
   do {                                                    \
      p##n = (PFN_##n)pvkGetDeviceProcAddr(d, #n);         \
      if (!p##n) {                                         \
         printf("device function %s: not found\n", #n);    \
         missing++;                                        \
      }                                                    \
   } while (0)
   L(vkGetDeviceQueue);
   L(vkCreateBuffer);
   L(vkGetBufferMemoryRequirements);
   L(vkAllocateMemory);
   L(vkBindBufferMemory);
   L(vkMapMemory);
   L(vkCreateCommandPool);
   L(vkAllocateCommandBuffers);
   L(vkBeginCommandBuffer);
   L(vkEndCommandBuffer);
   L(vkCmdFillBuffer);
   L(vkCmdCopyBuffer);
   L(vkCmdPipelineBarrier);
   L(vkCmdBeginRenderPass);
   L(vkCmdEndRenderPass);
   L(vkCmdCopyImageToBuffer);
   L(vkQueueSubmit);
   L(vkQueueWaitIdle);
   L(vkCreateFence);
   L(vkGetFenceStatus);
   L(vkWaitForFences);
   L(vkResetFences);
   L(vkCreateEvent);
   L(vkGetEventStatus);
   L(vkSetEvent);
   L(vkResetEvent);
   L(vkCreateImage);
   L(vkGetImageMemoryRequirements);
   L(vkBindImageMemory);
   L(vkCreateImageView);
   L(vkCreateRenderPass);
   L(vkCreateFramebuffer);
   L(vkResetCommandBuffer);
   L(vkCreateSemaphore);
   L(vkDeviceWaitIdle);
   L(vkDestroyDevice);
#undef L
   return missing;
}

static const char *
vkr(VkResult r)
{
   switch (r) {
   case VK_SUCCESS: return "VK_SUCCESS";
   case VK_NOT_READY: return "VK_NOT_READY";
   case VK_TIMEOUT: return "VK_TIMEOUT";
   case VK_EVENT_SET: return "VK_EVENT_SET";
   case VK_EVENT_RESET: return "VK_EVENT_RESET";
   case VK_INCOMPLETE: return "VK_INCOMPLETE";
   case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
   case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
   case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
   case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
   case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
   case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
   case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
   case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
   case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
   case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
   case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
   default: return "?";
   }
}

#define CHECK(call)                                                        \
   do {                                                                    \
      VkResult r_ = (call);                                                \
      if (r_ != VK_SUCCESS) {                                              \
         printf("FAILED: %s = %d (%s), line %d\n", #call, r_, vkr(r_), __LINE__); \
         fflush(stdout);                                                   \
         exit(4);                                                          \
      }                                                                    \
   } while (0)

static VkPhysicalDeviceMemoryProperties memprops;

static uint32_t
mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
         return i;
   return UINT32_MAX;
}

struct hbuf {
   VkBuffer buf;
   VkDeviceMemory mem;
   void *map;
};

static hbuf
host_buffer(VkDeviceSize size, VkBufferUsageFlags usage)
{
   hbuf b = {};
   VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
   bi.size = size;
   bi.usage = usage;
   CHECK(pvkCreateBuffer(dev, &bi, NULL, &b.buf));
   VkMemoryRequirements req;
   pvkGetBufferMemoryRequirements(dev, b.buf, &req);
   VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
   ai.allocationSize = req.size;
   ai.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (ai.memoryTypeIndex == UINT32_MAX) {
      printf("FAILED: no host-visible coherent memory type for a buffer (bits 0x%x)\n",
             req.memoryTypeBits);
      exit(4);
   }
   CHECK(pvkAllocateMemory(dev, &ai, NULL, &b.mem));
   CHECK(pvkBindBufferMemory(dev, b.buf, b.mem, 0));
   CHECK(pvkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map));
   return b;
}

static VkQueue queue;
static VkCommandPool pool;

static VkCommandBuffer
begin_cmd(void)
{
   VkCommandBuffer cb;
   VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
   ai.commandPool = pool;
   ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   ai.commandBufferCount = 1;
   CHECK(pvkAllocateCommandBuffers(dev, &ai, &cb));
   VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   CHECK(pvkBeginCommandBuffer(cb, &bi));
   return cb;
}

static void
submit_wait(VkCommandBuffer cb, VkFence fence)
{
   CHECK(pvkEndCommandBuffer(cb));
   VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
   si.commandBufferCount = 1;
   si.pCommandBuffers = &cb;
   CHECK(pvkQueueSubmit(queue, 1, &si, fence));
   CHECK(pvkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000));
}

static int sync_bad;

static void
expect(const char *what, VkResult got, VkResult want)
{
   printf("  %-44s %s%s\n", what, vkr(got), got == want ? "" : "   <-- wrong");
   if (got != want)
      sync_bad = 1;
}

static void
print_device(VkPhysicalDevice pd)
{
   VkPhysicalDeviceProperties p;
   pvkGetPhysicalDeviceProperties(pd, &p);
   printf("device: %s\n", p.deviceName);
   printf("  apiVersion %u.%u.%u, driverVersion 0x%x, vendor 0x%x, device 0x%x, type %d\n",
          VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
          VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion, p.vendorID, p.deviceID,
          p.deviceType);
   if (pvkGetPhysicalDeviceProperties2KHR) {
      VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
      VkPhysicalDeviceDriverProperties drv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
      id.pNext = &drv;
      VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      p2.pNext = &id;
      pvkGetPhysicalDeviceProperties2KHR(pd, &p2);
      printf("  driver: %s, %s; conformance %u.%u.%u.%u\n", drv.driverName, drv.driverInfo,
             drv.conformanceVersion.major, drv.conformanceVersion.minor,
             drv.conformanceVersion.subminor, drv.conformanceVersion.patch);
      printf("  LUID valid %u: %02x%02x%02x%02x-%02x%02x%02x%02x, node mask %u\n",
             id.deviceLUIDValid, id.deviceLUID[7], id.deviceLUID[6], id.deviceLUID[5],
             id.deviceLUID[4], id.deviceLUID[3], id.deviceLUID[2], id.deviceLUID[1],
             id.deviceLUID[0], id.deviceNodeMask);
   }
   const VkPhysicalDeviceLimits &l = p.limits;
   printf("  limits: image2D %u, image3D %u, cube %u, layers %u, texel buffer %u\n",
          l.maxImageDimension2D, l.maxImageDimension3D, l.maxImageDimensionCube,
          l.maxImageArrayLayers, l.maxTexelBufferElements);
   printf("          UBO range %u, SSBO range %u, push constants %u, allocations %u\n",
          l.maxUniformBufferRange, l.maxStorageBufferRange, l.maxPushConstantsSize,
          l.maxMemoryAllocationCount);
   printf("          bound sets %u, samplers/stage %u, vertex attributes %u, color attachments %u\n",
          l.maxBoundDescriptorSets, l.maxPerStageDescriptorSamplers, l.maxVertexInputAttributes,
          l.maxColorAttachments);
   printf("          compute WG %u x %u x %u, invocations %u, shared %u; viewports %u\n",
          l.maxComputeWorkGroupSize[0], l.maxComputeWorkGroupSize[1],
          l.maxComputeWorkGroupSize[2], l.maxComputeWorkGroupInvocations,
          l.maxComputeSharedMemorySize, l.maxViewports);
   printf("          timestampPeriod %.3f, framebuffer samples 0x%x\n", l.timestampPeriod,
          l.framebufferColorSampleCounts);

   VkPhysicalDeviceFeatures f;
   pvkGetPhysicalDeviceFeatures(pd, &f);
   printf("  features: geometryShader %u, tessellationShader %u, multiViewport %u, "
          "shaderClipDistance %u, fillModeNonSolid %u,\n"
          "            depthClamp %u, dualSrcBlend %u, logicOp %u, independentBlend %u, "
          "imageCubeArray %u, BC %u, ETC2 %u, ASTC %u,\n"
          "            pipelineStatisticsQuery %u, occlusionQueryPrecise %u, "
          "fragmentStoresAndAtomics %u, robustBufferAccess %u\n",
          f.geometryShader, f.tessellationShader, f.multiViewport, f.shaderClipDistance,
          f.fillModeNonSolid, f.depthClamp, f.dualSrcBlend, f.logicOp, f.independentBlend,
          f.imageCubeArray, f.textureCompressionBC, f.textureCompressionETC2,
          f.textureCompressionASTC_LDR, f.pipelineStatisticsQuery, f.occlusionQueryPrecise,
          f.fragmentStoresAndAtomics, f.robustBufferAccess);

   pvkGetPhysicalDeviceMemoryProperties(pd, &memprops);
   for (uint32_t i = 0; i < memprops.memoryHeapCount; i++)
      printf("  heap %u: %llu MiB, flags 0x%x\n", i,
             (unsigned long long)(memprops.memoryHeaps[i].size >> 20),
             memprops.memoryHeaps[i].flags);
   for (uint32_t i = 0; i < memprops.memoryTypeCount; i++) {
      VkMemoryPropertyFlags fl = memprops.memoryTypes[i].propertyFlags;
      printf("  memory type %u: heap %u,%s%s%s%s\n", i, memprops.memoryTypes[i].heapIndex,
             fl & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? " DEVICE_LOCAL" : "",
             fl & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? " HOST_VISIBLE" : "",
             fl & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? " HOST_COHERENT" : "",
             fl & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? " HOST_CACHED" : "");
   }

   uint32_t nq = 0;
   pvkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
   VkQueueFamilyProperties qf[8];
   if (nq > 8)
      nq = 8;
   pvkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
   for (uint32_t i = 0; i < nq; i++)
      printf("  queue family %u: %u queue(s), flags 0x%x, timestamp bits %u\n", i,
             qf[i].queueCount, qf[i].queueFlags, qf[i].timestampValidBits);

   static const struct { VkFormat f; const char *n; } fmts[] = {
      {VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM"},
      {VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM"},
      {VK_FORMAT_B8G8R8A8_SRGB, "B8G8R8A8_SRGB"},
      {VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT"},
      {VK_FORMAT_R32_SFLOAT, "R32_SFLOAT"},
      {VK_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT"},
      {VK_FORMAT_D32_SFLOAT, "D32_SFLOAT"},
      {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1_RGBA_UNORM"},
      {VK_FORMAT_BC3_UNORM_BLOCK, "BC3_UNORM"},
      {VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, "ETC2_R8G8B8A8_UNORM"},
      {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, "ASTC_4x4_UNORM"},
   };
   for (const auto &x : fmts) {
      VkFormatProperties fp;
      pvkGetPhysicalDeviceFormatProperties(pd, x.f, &fp);
      printf("  format %-22s optimal 0x%05x linear 0x%05x buffer 0x%05x\n", x.n,
             fp.optimalTilingFeatures, fp.linearTilingFeatures, fp.bufferFeatures);
   }

   uint32_t ne = 0;
   pvkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
   VkExtensionProperties *ext = (VkExtensionProperties *)calloc(ne ? ne : 1, sizeof(*ext));
   pvkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ext);
   printf("  %u device extensions:", ne);
   for (uint32_t i = 0; i < ne; i++)
      printf("%s%s", i % 3 ? "  " : "\n    ", ext[i].extensionName);
   printf("\n");
   free(ext);
}

static int
has_device_ext(VkPhysicalDevice pd, const char *name)
{
   uint32_t ne = 0;
   pvkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
   VkExtensionProperties *ext = (VkExtensionProperties *)calloc(ne ? ne : 1, sizeof(*ext));
   pvkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ext);
   int found = 0;
   for (uint32_t i = 0; i < ne && !found; i++)
      found = !strcmp(ext[i].extensionName, name);
   free(ext);
   return found;
}

static VkRenderPass
clear_pass(VkFormat fmt, VkImageLayout final_layout)
{
   VkAttachmentDescription a = {};
   a.format = fmt;
   a.samples = VK_SAMPLE_COUNT_1_BIT;
   a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
   a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
   a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
   a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
   a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   a.finalLayout = final_layout;
   VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sp = {};
   sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
   sp.colorAttachmentCount = 1;
   sp.pColorAttachments = &ref;
   VkRenderPassCreateInfo ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
   ci.attachmentCount = 1;
   ci.pAttachments = &a;
   ci.subpassCount = 1;
   ci.pSubpasses = &sp;
   VkRenderPass rp;
   CHECK(pvkCreateRenderPass(dev, &ci, NULL, &rp));
   return rp;
}

static VkImageView
view_of(VkImage img, VkFormat fmt)
{
   VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
   vi.image = img;
   vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
   vi.format = fmt;
   vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   vi.subresourceRange.levelCount = 1;
   vi.subresourceRange.layerCount = 1;
   VkImageView v;
   CHECK(pvkCreateImageView(dev, &vi, NULL, &v));
   return v;
}

static LRESULT CALLBACK
wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
   if (m == WM_CLOSE) {
      PostQuitMessage(0);
      return 0;
   }
   return DefWindowProcA(h, m, w, l);
}

static int
window_test(VkPhysicalDevice pd, uint32_t qfi, double seconds)
{
   WNDCLASSA wc = {};
   wc.lpfnWndProc = wndproc;
   wc.hInstance = GetModuleHandleA(NULL);
   wc.lpszClassName = "vkprobe";
   wc.hCursor = LoadCursor(NULL, IDC_ARROW);
   RegisterClassA(&wc);
   HWND hwnd = CreateWindowA("vkprobe", "vkprobe (panvk on the Mali)",
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE, 64, 64, 656, 519, NULL, NULL,
                             wc.hInstance, NULL);
   if (!hwnd) {
      printf("window: CreateWindow failed (%lu)\n", GetLastError());
      return 5;
   }
   if (!pvkCreateWin32SurfaceKHR) {
      printf("window: vkCreateWin32SurfaceKHR not exposed\n");
      return 5;
   }
   VkWin32SurfaceCreateInfoKHR si = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
   si.hinstance = wc.hInstance;
   si.hwnd = hwnd;
   VkSurfaceKHR surf;
   CHECK(pvkCreateWin32SurfaceKHR(inst, &si, NULL, &surf));
   VkBool32 ok = VK_FALSE;
   CHECK(pvkGetPhysicalDeviceSurfaceSupportKHR(pd, qfi, surf, &ok));
   VkSurfaceCapabilitiesKHR caps;
   CHECK(pvkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
   uint32_t nf = 0;
   CHECK(pvkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, NULL));
   VkSurfaceFormatKHR sf[32];
   if (nf > 32)
      nf = 32;
   CHECK(pvkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, sf));
   printf("window: surface supported %u, %u formats (first %d), extent %ux%u, images %u..%u\n",
          ok, nf, nf ? sf[0].format : -1, caps.currentExtent.width, caps.currentExtent.height,
          caps.minImageCount, caps.maxImageCount);
   if (!ok || !nf)
      return 5;
   VkSurfaceFormatKHR fmt = sf[0];
   for (uint32_t i = 0; i < nf; i++)
      if (sf[i].format == VK_FORMAT_B8G8R8A8_UNORM)
         fmt = sf[i];
   VkExtent2D ext = caps.currentExtent;
   if (ext.width == UINT32_MAX)
      ext = {640, 480};

   VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
   sci.surface = surf;
   sci.minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount;
   if (caps.maxImageCount && sci.minImageCount > caps.maxImageCount)
      sci.minImageCount = caps.maxImageCount;
   sci.imageFormat = fmt.format;
   sci.imageColorSpace = fmt.colorSpace;
   sci.imageExtent = ext;
   sci.imageArrayLayers = 1;
   sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
   sci.preTransform = caps.currentTransform;
   sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
   sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
   sci.clipped = VK_TRUE;
   VkSwapchainKHR sc;
   CHECK(pvkCreateSwapchainKHR(dev, &sci, NULL, &sc));
   uint32_t ni = 0;
   CHECK(pvkGetSwapchainImagesKHR(dev, sc, &ni, NULL));
   VkImage imgs[8];
   if (ni > 8)
      ni = 8;
   CHECK(pvkGetSwapchainImagesKHR(dev, sc, &ni, imgs));
   printf("window: swap chain %ux%u, %u images, format %d\n", ext.width, ext.height, ni,
          fmt.format);

   VkRenderPass rp = clear_pass(fmt.format, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
   VkFramebuffer fbs[8];
   for (uint32_t i = 0; i < ni; i++) {
      VkImageView v = view_of(imgs[i], fmt.format);
      VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fi.renderPass = rp;
      fi.attachmentCount = 1;
      fi.pAttachments = &v;
      fi.width = ext.width;
      fi.height = ext.height;
      fi.layers = 1;
      CHECK(pvkCreateFramebuffer(dev, &fi, NULL, &fbs[i]));
   }
   VkCommandBuffer cbs[8];
   VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
   cai.commandPool = pool;
   cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cai.commandBufferCount = ni;
   CHECK(pvkAllocateCommandBuffers(dev, &cai, cbs));
   VkSemaphoreCreateInfo semi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
   VkSemaphore acq[2], done[2];
   VkFence inflight[2];
   for (int i = 0; i < 2; i++) {
      CHECK(pvkCreateSemaphore(dev, &semi, NULL, &acq[i]));
      CHECK(pvkCreateSemaphore(dev, &semi, NULL, &done[i]));
      CHECK(pvkCreateFence(dev, &fci, NULL, &inflight[i]));
   }

   LARGE_INTEGER freq, t0, t;
   QueryPerformanceFrequency(&freq);
   QueryPerformanceCounter(&t0);
   unsigned frames = 0;
   int quit = 0;
   for (;;) {
      MSG msg;
      while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
         if (msg.message == WM_QUIT)
            quit = 1;
         TranslateMessage(&msg);
         DispatchMessageA(&msg);
      }
      QueryPerformanceCounter(&t);
      double el = (double)(t.QuadPart - t0.QuadPart) / freq.QuadPart;
      if (quit || el >= seconds) {
         printf("window: %u frames in %.2f s, %.1f a second\n", frames, el, frames / el);
         break;
      }
      unsigned s = frames & 1;
      CHECK(pvkWaitForFences(dev, 1, &inflight[s], VK_TRUE, 10ull * 1000 * 1000 * 1000));
      uint32_t idx;
      VkResult r = pvkAcquireNextImageKHR(dev, sc, 10ull * 1000 * 1000 * 1000, acq[s],
                                          VK_NULL_HANDLE, &idx);
      if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
         printf("window: vkAcquireNextImageKHR %s after %u frames\n", vkr(r), frames);
         return r == VK_ERROR_OUT_OF_DATE_KHR ? 0 : 4;
      }
      CHECK(pvkResetFences(dev, 1, &inflight[s]));
      VkCommandBuffer cb = cbs[idx];
      CHECK(pvkResetCommandBuffer(cb, 0));
      VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      CHECK(pvkBeginCommandBuffer(cb, &bi));
      float ph = (float)(frames % 120) / 120.0f;
      VkClearValue cv;
      cv.color.float32[0] = ph;
      cv.color.float32[1] = 1.0f - ph;
      cv.color.float32[2] = 0.5f;
      cv.color.float32[3] = 1.0f;
      VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      rbi.renderPass = rp;
      rbi.framebuffer = fbs[idx];
      rbi.renderArea.extent = ext;
      rbi.clearValueCount = 1;
      rbi.pClearValues = &cv;
      pvkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
      pvkCmdEndRenderPass(cb);
      CHECK(pvkEndCommandBuffer(cb));
      VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
      sub.waitSemaphoreCount = 1;
      sub.pWaitSemaphores = &acq[s];
      sub.pWaitDstStageMask = &ws;
      sub.commandBufferCount = 1;
      sub.pCommandBuffers = &cb;
      sub.signalSemaphoreCount = 1;
      sub.pSignalSemaphores = &done[s];
      CHECK(pvkQueueSubmit(queue, 1, &sub, inflight[s]));
      VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
      pi.waitSemaphoreCount = 1;
      pi.pWaitSemaphores = &done[s];
      pi.swapchainCount = 1;
      pi.pSwapchains = &sc;
      pi.pImageIndices = &idx;
      r = pvkQueuePresentKHR(queue, &pi);
      if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
         printf("window: vkQueuePresentKHR %s after %u frames\n", vkr(r), frames);
         return r == VK_ERROR_OUT_OF_DATE_KHR ? 0 : 4;
      }
      frames++;
   }
   CHECK(pvkDeviceWaitIdle(dev));
   pvkDestroySwapchainKHR(dev, sc, NULL);
   pvkDestroySurfaceKHR(inst, surf, NULL);
   DestroyWindow(hwnd);
   return frames ? 0 : 5;
}

// A crash says where: the module and offset of the faulting address.
static LONG WINAPI
crash_filter(EXCEPTION_POINTERS *e)
{
   void *pc = e->ExceptionRecord->ExceptionAddress;
   HMODULE mod = NULL;
   char name[MAX_PATH] = "?";
   GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &mod);
   if (mod)
      GetModuleFileNameA(mod, name, sizeof name);
   printf("CRASH: exception 0x%08lx at %p = %s+0x%llx", e->ExceptionRecord->ExceptionCode, pc,
          name, (unsigned long long)((char *)pc - (char *)mod));
   if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
      printf(", %s 0x%llx", e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
             (unsigned long long)e->ExceptionRecord->ExceptionInformation[1]);
   printf("\n");
   fflush(stdout);
   return EXCEPTION_CONTINUE_SEARCH;
}

int
main(int argc, char **argv)
{
   SetUnhandledExceptionFilter(crash_filter);
   const char *icd = NULL;
   double window_s = 0;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--emulated"))
         emulated = 1;
      else if (!strcmp(argv[i], "--window") && i + 1 < argc)
         window_s = atof(argv[++i]);
      else
         icd = argv[i];
   }
   setvbuf(stdout, NULL, _IONBF, 0);
   if (!getenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER"))
      _putenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1");

   char self[MAX_PATH], path[MAX_PATH];
   GetModuleFileNameA(NULL, self, sizeof self);
   if (!icd) {
      strcpy(path, self);
      char *slash = strrchr(path, '\\');
      strcpy(slash ? slash + 1 : path, "malivk_arm64.dll");
      icd = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES ? path : "loader";
   }

   HMODULE m;
   if (!strcmp(icd, "loader")) {
      m = LoadLibraryA("vulkan-1.dll");
      if (!m) {
         printf("vulkan-1.dll did not load (%lu)\n", GetLastError());
         return 100;
      }
      gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(m, "vkGetInstanceProcAddr");
      printf("through the Vulkan loader\n");
   } else {
      m = LoadLibraryA(icd);
      if (!m) {
         printf("%s did not load (%lu)\n", icd, GetLastError());
         return 100;
      }
      PFN_negotiate neg = (PFN_negotiate)(void *)GetProcAddress(m, "vk_icdNegotiateLoaderICDInterfaceVersion");
      gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(m, "vk_icdGetInstanceProcAddr");
      uint32_t ver = 7;
      if (neg)
         neg(&ver);
      printf("ICD %s: loader interface version %u, enumerate-adapters %s\n", icd, ver,
             gipa && gipa(NULL, "vk_icdEnumerateAdapterPhysicalDevices") ? "yes" : "no");
   }
   if (!gipa) {
      printf("no vkGetInstanceProcAddr\n");
      return 100;
   }

   pvkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   pvkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)gipa(NULL, "vkEnumerateInstanceVersion");
   pvkEnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
   uint32_t iv = VK_API_VERSION_1_0;
   if (pvkEnumerateInstanceVersion)
      pvkEnumerateInstanceVersion(&iv);
   printf("instance version %u.%u.%u\n", VK_API_VERSION_MAJOR(iv), VK_API_VERSION_MINOR(iv),
          VK_API_VERSION_PATCH(iv));
   uint32_t nie = 0;
   pvkEnumerateInstanceExtensionProperties(NULL, &nie, NULL);
   VkExtensionProperties *ie = (VkExtensionProperties *)calloc(nie ? nie : 1, sizeof(*ie));
   pvkEnumerateInstanceExtensionProperties(NULL, &nie, ie);
   printf("%u instance extensions:", nie);
   int have_win32 = 0;
   for (uint32_t i = 0; i < nie; i++) {
      printf(" %s", ie[i].extensionName);
      have_win32 |= !strcmp(ie[i].extensionName, VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
   }
   printf("\n");

   const char *iext[3] = {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
   uint32_t niext = 1;
   if (have_win32) {
      iext[niext++] = VK_KHR_SURFACE_EXTENSION_NAME;
      iext[niext++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
   }
   VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
   app.pApplicationName = "vkprobe";
   app.apiVersion = VK_API_VERSION_1_0;
   VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
   ici.pApplicationInfo = &app;
   ici.enabledExtensionCount = niext;
   ici.ppEnabledExtensionNames = iext;
   CHECK(pvkCreateInstance(&ici, NULL, &inst));
   load_instance_fns(inst);

   uint32_t npd = 0;
   CHECK(pvkEnumeratePhysicalDevices(inst, &npd, NULL));
   printf("%u physical device(s)\n", npd);
   if (!npd)
      return 2;
   VkPhysicalDevice pds[4];
   if (npd > 4)
      npd = 4;
   CHECK(pvkEnumeratePhysicalDevices(inst, &npd, pds));
   VkPhysicalDevice pd = pds[0];
   print_device(pd);

   uint32_t nq = 0, qfi = UINT32_MAX;
   pvkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
   VkQueueFamilyProperties qf[8];
   if (nq > 8)
      nq = 8;
   pvkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
   for (uint32_t i = 0; i < nq && qfi == UINT32_MAX; i++)
      if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
         qfi = i;
   if (qfi == UINT32_MAX) {
      printf("no graphics queue\n");
      return 4;
   }
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
   qci.queueFamilyIndex = qfi;
   qci.queueCount = 1;
   qci.pQueuePriorities = &prio;
   const char *dext[1] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
   int swapchain = have_win32 && has_device_ext(pd, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
   VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
   dci.queueCreateInfoCount = 1;
   dci.pQueueCreateInfos = &qci;
   dci.enabledExtensionCount = swapchain ? 1 : 0;
   dci.ppEnabledExtensionNames = dext;
   CHECK(pvkCreateDevice(pd, &dci, NULL, &dev));
   if (load_device_fns(dev))
      return 4;
   if (swapchain) {
#define L(n) p##n = (PFN_##n)pvkGetDeviceProcAddr(dev, #n)
      L(vkCreateSwapchainKHR);
      L(vkGetSwapchainImagesKHR);
      L(vkAcquireNextImageKHR);
      L(vkQueuePresentKHR);
      L(vkDestroySwapchainKHR);
#undef L
   }
   pvkGetDeviceQueue(dev, qfi, 0, &queue);
   VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
   pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   pci.queueFamilyIndex = qfi;
   CHECK(pvkCreateCommandPool(dev, &pci, NULL, &pool));
   printf("device created, queue family %u\n", qfi);

   // Fences and events: what the shim's syncobj state has to get right.
   printf("synchronisation:\n");
   VkFence fence;
   VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(pvkCreateFence(dev, &fci, NULL, &fence));
   expect("new fence, vkGetFenceStatus", pvkGetFenceStatus(dev, fence), VK_NOT_READY);
   expect("new fence, vkWaitForFences(timeout 0)", pvkWaitForFences(dev, 1, &fence, VK_TRUE, 0),
          VK_TIMEOUT);
   expect("new fence, vkWaitForFences(timeout 20 ms)",
          pvkWaitForFences(dev, 1, &fence, VK_TRUE, 20ull * 1000 * 1000), VK_TIMEOUT);
   VkFence sfence;
   fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
   CHECK(pvkCreateFence(dev, &fci, NULL, &sfence));
   expect("fence created signalled, vkGetFenceStatus", pvkGetFenceStatus(dev, sfence), VK_SUCCESS);
   CHECK(pvkResetFences(dev, 1, &sfence));
   expect("after vkResetFences", pvkGetFenceStatus(dev, sfence), VK_NOT_READY);
   VkEvent ev;
   VkEventCreateInfo eci = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
   CHECK(pvkCreateEvent(dev, &eci, NULL, &ev));
   expect("new event", pvkGetEventStatus(dev, ev), VK_EVENT_RESET);
   CHECK(pvkSetEvent(dev, ev));
   expect("after vkSetEvent", pvkGetEventStatus(dev, ev), VK_EVENT_SET);
   CHECK(pvkResetEvent(dev, ev));
   expect("after vkResetEvent", pvkGetEventStatus(dev, ev), VK_EVENT_RESET);

   // A fill and a copy.
   int gpu_bad = 0;
   const VkDeviceSize n = 64 * 1024;
   hbuf src = host_buffer(n, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
   hbuf dst = host_buffer(n, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
   for (uint32_t i = 0; i < n / 4; i++) {
      ((uint32_t *)src.map)[i] = 0x01000000u + i;
      ((uint32_t *)dst.map)[i] = 0;
   }
   VkCommandBuffer cb = begin_cmd();
   pvkCmdFillBuffer(cb, dst.buf, 0, n / 2, 0xdeadbeefu);
   VkBufferCopy bc = {n / 2, n / 2, n / 2};
   pvkCmdCopyBuffer(cb, src.buf, dst.buf, 1, &bc);
   VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
   mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
   pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                         &mb, 0, NULL, 0, NULL);
   submit_wait(cb, fence);
   expect("submitted fence, after vkWaitForFences", pvkGetFenceStatus(dev, fence), VK_SUCCESS);
   CHECK(pvkResetFences(dev, 1, &fence));
   expect("submitted fence, after vkResetFences", pvkGetFenceStatus(dev, fence), VK_NOT_READY);
   {
      const uint32_t *d = (const uint32_t *)dst.map;
      uint32_t bad = 0, first = UINT32_MAX;
      for (uint32_t i = 0; i < n / 4; i++) {
         uint32_t want = i < n / 8 ? 0xdeadbeefu : 0x01000000u + i;
         if (d[i] != want) {
            bad++;
            if (first == UINT32_MAX)
               first = i;
         }
      }
      printf("fill+copy: %s (%u of %u words wrong; word 0 = 0x%08x, word %u = 0x%08x)%s\n",
             bad ? "WRONG" : "ok", bad, (unsigned)(n / 4), d[0], (unsigned)(n / 8),
             d[n / 8], emulated && bad ? ", not judged: emulated" : "");
      if (bad && !emulated)
         gpu_bad = 1;
   }

   // A render pass clearing an image, read back through a buffer.
   {
      const uint32_t w = 256, h = 256;
      const VkFormat f = VK_FORMAT_R8G8B8A8_UNORM;
      VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      ii.imageType = VK_IMAGE_TYPE_2D;
      ii.format = f;
      ii.extent = {w, h, 1};
      ii.mipLevels = 1;
      ii.arrayLayers = 1;
      ii.samples = VK_SAMPLE_COUNT_1_BIT;
      ii.tiling = VK_IMAGE_TILING_OPTIMAL;
      ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      VkImage img;
      CHECK(pvkCreateImage(dev, &ii, NULL, &img));
      VkMemoryRequirements req;
      pvkGetImageMemoryRequirements(dev, img, &req);
      VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      ai.allocationSize = req.size;
      ai.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      VkDeviceMemory im;
      CHECK(pvkAllocateMemory(dev, &ai, NULL, &im));
      CHECK(pvkBindImageMemory(dev, img, im, 0));
      VkRenderPass rp = clear_pass(f, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      VkImageView v = view_of(img, f);
      VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fi.renderPass = rp;
      fi.attachmentCount = 1;
      fi.pAttachments = &v;
      fi.width = w;
      fi.height = h;
      fi.layers = 1;
      VkFramebuffer fb;
      CHECK(pvkCreateFramebuffer(dev, &fi, NULL, &fb));
      hbuf rb = host_buffer((VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
      memset(rb.map, 0, (size_t)w * h * 4);
      cb = begin_cmd();
      VkClearValue cv;
      cv.color.float32[0] = 1.0f;
      cv.color.float32[1] = 0.5f;
      cv.color.float32[2] = 0.0f;
      cv.color.float32[3] = 1.0f;
      VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      rbi.renderPass = rp;
      rbi.framebuffer = fb;
      rbi.renderArea.extent = {w, h};
      rbi.clearValueCount = 1;
      rbi.pClearValues = &cv;
      pvkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
      pvkCmdEndRenderPass(cb);
      VkBufferImageCopy bic = {};
      bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      bic.imageSubresource.layerCount = 1;
      bic.imageExtent = {w, h, 1};
      pvkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &bic);
      pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                            &mb, 0, NULL, 0, NULL);
      submit_wait(cb, fence);
      CHECK(pvkResetFences(dev, 1, &fence));
      const uint32_t *px = (const uint32_t *)rb.map;
      const uint32_t want = 0xff0080ffu;   // R 255, G 128, B 0, A 255, little-endian RGBA
      uint32_t bad = 0;
      for (uint32_t i = 0; i < w * h; i++) {
         uint32_t p = px[i], g = (p >> 8) & 0xff;
         if ((p & 0xff) != 0xff || (g != 0x7f && g != 0x80) || ((p >> 16) & 0xff) || (p >> 24) != 0xff)
            bad++;
      }
      printf("clear 256x256: %s (%u of %u pixels wrong; pixel 0 = 0x%08x, want about 0x%08x)%s\n",
             bad ? "WRONG" : "ok", bad, w * h, px[0], want,
             emulated && bad ? ", not judged: emulated" : "");
      if (bad && !emulated)
         gpu_bad = 1;
   }

   int win = 0;
   if (window_s > 0) {
      if (!swapchain) {
         printf("window: no VK_KHR_win32_surface or VK_KHR_swapchain\n");
         win = 5;
      } else {
         win = window_test(pd, qfi, window_s);
      }
   }

   CHECK(pvkDeviceWaitIdle(dev));
   pvkDestroyDevice(dev, NULL);
   pvkDestroyInstance(inst, NULL);
   printf("result: sync %s, gpu %s, window %s\n", sync_bad ? "WRONG" : "ok",
          gpu_bad ? "WRONG" : emulated ? "not judged" : "ok",
          window_s > 0 ? (win ? "FAILED" : "ok") : "not run");
   if (sync_bad)
      return 3;
   if (win)
      return win;
   return gpu_bad ? 1 : 0;
}

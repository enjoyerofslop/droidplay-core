// The Android NVN replay's Vulkan device (gfx_vk.c), shared with the front end that presents the game's window texture (front_android.c).
#pragma once
#include <pthread.h>
#include <stdint.h>
#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR 1
#define VKB_FNS_SURFACE(X) X(vkCreateAndroidSurfaceKHR)
#else
#define VKB_FNS_SURFACE(X)   // Linux (dm/linux): the front end's window library makes the surface (front_linux.c)
#endif
#define VK_NO_PROTOTYPES 1   // every vk* below is a pointer, filled from the driver vk_driver.c opens: the system's, or DM_VK_DRIVER's
#include <vulkan/vulkan.h>

// The Vulkan functions the replay and the front end call. GLOBAL: loaded before there is an instance; the others through it (vk_driver_instance)
#define VKB_FNS_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
#define VKB_FNS(X) \
    X(vkAcquireNextImageKHR) X(vkAllocateCommandBuffers) X(vkAllocateDescriptorSets) X(vkAllocateMemory) X(vkBeginCommandBuffer) X(vkBindBufferMemory) \
    X(vkBindImageMemory) X(vkCmdBeginQuery) X(vkCmdBeginRenderPass) X(vkCmdBindDescriptorSets) X(vkCmdBindIndexBuffer) X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) X(vkCmdBlitImage) X(vkCmdClearAttachments) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdFillBuffer) \
    X(vkCmdCopyImage) X(vkCmdCopyImageToBuffer) X(vkCmdDispatch) X(vkCmdDispatchIndirect) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdDrawIndexedIndirect) X(vkCmdEndQuery) X(vkCmdEndRenderPass) \
    X(vkCmdPipelineBarrier) X(vkCmdPushConstants) X(vkCmdResetQueryPool) X(vkCmdResolveImage) X(vkCmdSetBlendConstants) X(vkCmdSetDepthBias) \
    X(vkCmdSetScissor) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilReference) X(vkCmdSetStencilWriteMask) X(vkCmdSetViewport) X(vkCmdWriteTimestamp) \
    VKB_FNS_SURFACE(X) X(vkCreateBuffer) X(vkCreateBufferView) X(vkCreateCommandPool) X(vkCreateComputePipelines) X(vkCreateDescriptorPool) \
    X(vkCreateDescriptorSetLayout) X(vkCreateDevice) X(vkCreateFence) X(vkCreateFramebuffer) X(vkCreateGraphicsPipelines) X(vkCreateImage) \
    X(vkCreateImageView) X(vkCreatePipelineCache) X(vkCreatePipelineLayout) X(vkCreateQueryPool) X(vkCreateRenderPass) X(vkCreateSampler) \
    X(vkCreateSemaphore) X(vkCreateShaderModule) X(vkCreateSwapchainKHR) X(vkDestroyBuffer) X(vkDestroyBufferView) X(vkDestroyCommandPool) X(vkDestroyDescriptorPool) \
    X(vkDestroyDevice) \
    X(vkDestroyDescriptorSetLayout) X(vkDestroyFence) X(vkDestroyFramebuffer) X(vkDestroyImage) X(vkDestroyImageView) X(vkDestroyInstance) X(vkDestroyPipeline) X(vkDestroyPipelineLayout) \
    X(vkDestroySemaphore) X(vkDestroyShaderModule) X(vkDestroySurfaceKHR) X(vkDestroySwapchainKHR) \
    X(vkDeviceWaitIdle) X(vkEndCommandBuffer) X(vkEnumerateDeviceExtensionProperties) X(vkEnumeratePhysicalDevices) X(vkFlushMappedMemoryRanges) X(vkFreeMemory) \
    X(vkGetBufferMemoryRequirements) X(vkGetDeviceProcAddr) X(vkGetDeviceQueue) X(vkGetImageMemoryRequirements) X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceImageFormatProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPipelineCacheData) X(vkGetQueryPoolResults) X(vkGetSwapchainImagesKHR) X(vkInvalidateMappedMemoryRanges) X(vkMapMemory) X(vkQueuePresentKHR) \
    X(vkQueueSubmit) X(vkResetCommandBuffer) X(vkResetCommandPool) X(vkResetDescriptorPool) X(vkResetFences) X(vkUnmapMemory) X(vkUpdateDescriptorSets) \
    X(vkWaitForFences)
#define VKB_DECL(f) extern __attribute__((visibility("hidden"))) PFN_##f f;
VKB_FNS_GLOBAL(VKB_DECL) VKB_FNS(VKB_DECL)
#undef VKB_DECL
// vk_driver.c. vk_driver_dirs (the front end, before the device is made): DM_VK_DRIVER is looked for relative to ext, the folder env.txt is in, and copied into internal.
// vk_driver_load: the driver opened and the GLOBAL functions loaded (0: no Vulkan); vk_driver_instance: the others, once the instance is made.
// vk_driver_name: the DM_VK_DRIVER file running (its name), NULL on the system's driver.
// vk_driver_fail: DM_VK_DRIVER_FAIL names this step (a test: it fails as if the DM_VK_DRIVER driver had failed it). vk_driver_fallback: that driver failed (why), so the
// system's is opened and the GLOBAL functions loaded from it, with a [RED] line; 0 when none was running or the system's has no Vulkan
void vk_driver_dirs(const char *ext, const char *internal);
int vk_driver_load(void);
void vk_driver_instance(VkInstance inst);
const char *vk_driver_name(void);
int vk_driver_fail(const char *step);
int vk_driver_fallback(const char *why);
const char *vk_result_str(VkResult r);   // VK_ERROR_INITIALIZATION_FAILED, ... for a log line
// The DM_VK_DRIVER start records (vk_driver.c, "Start records and the crash guard"). vk_driver_game: the game's folder and title ID (the front end, before vkb).
// vk_driver_ok: the game ran long enough to count as a good start; vk_driver_lost: the device was lost. vk_driver_note: the status screen's line about the driver (NULL:
// none chosen), warn when it isn't the one chosen
void vk_driver_game(const char *dir, const char *id);
void vk_driver_ok(void);
void vk_driver_lost(VkResult r);
const char *vk_driver_note(int *warn);
// The start record every start writes, whatever driver runs (vk_start.c): games/<title ID>/vk_start.txt. vk_start_game: the game's folder (the front end, before vkb);
// vk_start_device: the device is made; vk_start_pipe_failed: a pipeline the game needed was refused (its shaders' keys; b NULL for compute); vk_start_lost: the GPU
// was lost; vk_start_tick: the front end's loop, which writes what changed
void vk_start_game(const char *dir);
void vk_start_device(const char *gpu, int driver_id);
void vk_start_pipe_failed(const char *a, const char *b);
void vk_start_lost(VkResult r);
void vk_start_tick(void);
#ifndef __ANDROID__
// Linux: the instance extensions the window's surface needs, as the front end's window library names them (front_linux.c); the command-line host has none (front_cli.c)
uint32_t front_vk_instance_extensions(const char *const **names);
#endif
int vk_fp_host(void);   // vk_driver.c: the driver's compiles run with FPCR 0 (DM_VK_FP_HOST); the pipeline cache's file is named for it

typedef struct {
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qf;
    VkPhysicalDeviceProperties props; VkPhysicalDeviceMemoryProperties mp;
    int has_bc, has_astc, has_surface, has_swapchain;
    int has_display_timing;   // VK_GOOGLE_display_timing enabled: frames start on the display's vsyncs and are shown at a given time (gfx_vk_vsync.inc)
    pthread_mutex_t qmu;   // every vkQueueSubmit / vkQueuePresentKHR / vkQueueWaitIdle on q
} vkb_ctx;
vkb_ctx *vkb(void);        // made at first use; NULL when the device has no usable Vulkan
int vkb_fallback(const char *why);   // a DM_VK_DRIVER driver's device and instance made again on the system's driver, before the host starts (0: none was running, or no device)

// The window texture the game presented last (NVN QueuePresentTexture), for the front end to draw: always in VK_IMAGE_LAYOUT_GENERAL.
// vkb_frame_lock holds it (the texture is not destroyed meanwhile) until vkb_frame_unlock; 0 = no frame yet
typedef struct { VkImage img; VkFormat fmt; uint32_t w, h; int crop[4]; uint64_t n; int flip; uint64_t show_at; } vkb_frame;   // flip: stored bottom-up (NVN's lower-left window origin), shown flipped;
                                                                                                                      // show_at: when to show it (CLOCK_MONOTONIC ns, 0 = as soon as drawn; gfx_vk_vsync.inc)
int vkb_frame_lock(vkb_frame *f);
void vkb_pipeline_cache_save(void);   // the driver's compiled pipelines to DM_PIPELINE_CACHE, when they grew (the app paused or stopped)
void vkb_frame_unlock(void);
uint32_t vkb_mem_type(uint32_t bits, VkMemoryPropertyFlags want);   // a memory type index of bits with the wanted properties (UINT32_MAX = none)

// t_tbuf_rows <rows.spv>: a buffer texture read through a 2D slot as rows of 16384 texels, as the Vulkan replay makes one (tbuf_rows.h: gfx_vk.c vt_create's shape, vt_upload_'s
// fill and copy): for element counts from 1 to 1,048,576 (one short row, exactly one row, one past it, several rows with a short last one, 64 full rows), an R32_UINT
// optimal 2D image is filled by tbr_fill and vkCmdCopyBufferToImage from a "guest" buffer of distinct words, and a compute shader reads every texel of it by the translator's
// wrapped address (x & 0x3FFF, x >> 14), as the 1D fetches of a fetch1d slot do. Each element must read back as written, and the texels past the buffer in the last row as 0.
// Prints one line per case; exit 0 when every case reads right. dm/tools/tests/test_tbuf_rows.py compiles the shader (glslc), builds this with the NDK and runs it on a device
// with DM_TEST_SERIAL=<adb serial>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "../tbuf_rows.h"
#define CK(x) do { VkResult r_ = (x); if (r_) { printf("%s = %d\n", #x, r_); return 1; } } while (0)
static uint32_t mem_type(VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++) if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
}
static VkDevice d; static VkPhysicalDeviceMemoryProperties mp;
static int buf_new(VkDeviceSize sz, VkBufferUsageFlags u, VkBuffer *b, VkDeviceMemory *m, void **p) {
    VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sz, .usage = u}; CK(vkCreateBuffer(d, &bc, NULL, b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(d, *b, &mr);
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                               .memoryTypeIndex = mem_type(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CK(vkAllocateMemory(d, &ma, NULL, m)); CK(vkBindBufferMemory(d, *b, *m, 0)); CK(vkMapMemory(d, *m, 0, sz, 0, p));
    return 0;
}
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    static uint32_t code[1 << 16];
    FILE *f = fopen(argv[1], "rb"); if (!f) return 2; size_t nb = fread(code, 1, sizeof code, f); fclose(f);
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    VkInstance inst; CK(vkCreateInstance(&ic, NULL, &inst));
    uint32_t n1 = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &n1, &pd);
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp); vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R32_UINT, &fp);
    printf("%s (driver %#x), largest 2D image %u, R32_UINT sampled %s\n", pp.deviceName, pp.driverVersion, pp.limits.maxImageDimension2D,
           fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ? "yes" : "no");
    float pri = 1; VkDeviceQueueCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &pri};
    VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qc};
    CK(vkCreateDevice(pd, &dc, NULL, &d)); VkQueue q; vkGetDeviceQueue(d, 0, 0, &q);
    VkDescriptorSetLayoutBinding lb[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dlc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = lb}; VkDescriptorSetLayout dl; CK(vkCreateDescriptorSetLayout(d, &dlc, NULL, &dl));
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    VkPipelineLayoutCreateInfo lc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkPipelineLayout pl; CK(vkCreatePipelineLayout(d, &lc, NULL, &pl));
    VkShaderModuleCreateInfo mc = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nb, .pCode = code}; VkShaderModule m; CK(vkCreateShaderModule(d, &mc, NULL, &m));
    VkComputePipelineCreateInfo cc = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                      .module = m, .pName = "main"}, .layout = pl};
    VkPipeline pipe; CK(vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &cc, NULL, &pipe));
    VkSamplerCreateInfo sc = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkSampler smp; CK(vkCreateSampler(d, &sc, NULL, &smp));
    VkCommandPoolCreateInfo cpc = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT}; VkCommandPool cp; CK(vkCreateCommandPool(d, &cpc, NULL, &cp));
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .commandBufferCount = 1}; VkCommandBuffer c; CK(vkAllocateCommandBuffers(d, &ca, &c));
    static const uint32_t cases[] = {1, 48, 16383, 16384, 16385, 40000, 1048576};
    int bad_cases = 0;
    for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
        uint32_t n = cases[k], w, h;
        if (!tbr_shape(n, pp.limits.maxImageDimension2D, &w, &h)) { printf("%u elements: no rows on this GPU\n", n); bad_cases++; continue; }
        size_t gbytes = (size_t)n * 4, ibytes = (size_t)w * h * 4;
        uint32_t *guest = malloc(gbytes); if (!guest) return 2;
        for (uint32_t i = 0; i < n; i++) guest[i] = i * 2654435761u ^ 0x5a5a0000u ^ k;   // distinct words, none 0 but by chance
        VkBuffer sb, ob; VkDeviceMemory sm, om; void *sp, *op;
        if (buf_new(ibytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &sb, &sm, &sp) || buf_new(ibytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &ob, &om, &op)) return 1;
        tbr_fill(sp, ibytes, (const uint8_t *)guest, gbytes); memset(op, 0xab, ibytes);
        VkImageCreateInfo imc = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R32_UINT, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VkImage img; CK(vkCreateImage(d, &imc, NULL, &img));
        VkMemoryRequirements imr; vkGetImageMemoryRequirements(d, img, &imr);
        VkMemoryAllocateInfo ima = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = imr.size, .memoryTypeIndex = mem_type(&mp, imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        VkDeviceMemory im; CK(vkAllocateMemory(d, &ima, NULL, &im)); CK(vkBindImageMemory(d, img, im, 0));
        VkImageViewCreateInfo vc = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R32_UINT, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkImageView iv; CK(vkCreateImageView(d, &vc, NULL, &iv));
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
        VkDescriptorPoolCreateInfo dpc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = ps}; VkDescriptorPool dp; CK(vkCreateDescriptorPool(d, &dpc, NULL, &dp));
        VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dl}; VkDescriptorSet ds; CK(vkAllocateDescriptorSets(d, &dai, &ds));
        VkDescriptorImageInfo ii = {smp, iv, VK_IMAGE_LAYOUT_GENERAL}; VkDescriptorBufferInfo bi = {ob, 0, ibytes};
        VkWriteDescriptorSet wr[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
                                      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi}};
        vkUpdateDescriptorSets(d, 2, wr, 0, NULL);
        VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; CK(vkBeginCommandBuffer(c, &cbi));
        VkImageMemoryBarrier b0 = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b0);
        VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};   // packed rows, as vt_upload_ copies them
        vkCmdCopyBufferToImage(c, sb, img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        VkImageMemoryBarrier b1 = b0; b1.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b1.dstAccessMask = VK_ACCESS_SHADER_READ_BIT; b1.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
        uint32_t total = w * h;
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, pipe); vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
        vkCmdPushConstants(c, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &total); vkCmdDispatch(c, (total + 63) / 64, 1, 1);
        VkMemoryBarrier hb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
        CK(vkEndCommandBuffer(c));
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &c}; CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); CK(vkQueueWaitIdle(q));
        const uint32_t *o = op; uint32_t wrong = 0, tail_wrong = 0, first = UINT32_MAX;
        for (uint32_t x = 0; x < total; x++) { uint32_t want = x < n ? guest[x] : 0; if (o[x] != want) { if (x < n) wrong++; else tail_wrong++; if (first == UINT32_MAX) first = x; } }
        printf("%u elements as %ux%u rows: %u of %u elements read wrong, %u of %u texels past the buffer not 0%s", n, w, h, wrong, n, tail_wrong, total - n, wrong || tail_wrong ? "" : "\n");
        if (wrong || tail_wrong) { printf(" (first at %u: %#x, wanted %#x)\n", first, o[first], first < n ? guest[first] : 0); bad_cases++; }
        CK(vkResetCommandBuffer(c, 0));
        vkDestroyDescriptorPool(d, dp, NULL); vkDestroyImageView(d, iv, NULL); vkDestroyImage(d, img, NULL); vkFreeMemory(d, im, NULL);
        vkDestroyBuffer(d, sb, NULL); vkFreeMemory(d, sm, NULL); vkDestroyBuffer(d, ob, NULL); vkFreeMemory(d, om, NULL); free(guest);
    }
    printf("%d of %zu cases wrong\n", bad_cases, sizeof cases / sizeof *cases);
    return bad_cases ? 1 : 0;
}

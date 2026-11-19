// t_shx <comp.spv>: whether a compute shader's shuffles read the right lanes on this GPU. Two workgroups of 128 invocations; each invocation shuffles its value
// (a float, as the translator's SHFL helpers shuffle) to eight computed lanes (lane ^ 1, lane ^ 13, lane + 1 and lane - 1 clamped, a permutation, lane 0, a mask of
// the lane's own bits, a hash) and writes the results with its subgroup ID, lane, value and the subgroup size, 12 words each. The check finds each result's source
// lane in the same subgroup. dm/tools/tests/test_spv_shx.py writes the shader (as compiled, and as spv_shx.h makes its shuffles ShuffleXor) and, with
// DM_TEST_SERIAL=<adb serial>, builds this for Android and runs both there. Exit 0 when every result is right; a driver whose compiler dies on the shuffle ends
// the process with signal 11 instead
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d\n", #x, r_); return 2; } } while (0)
enum { G = 2, L = 128, NI = G * L, W = 12 };
static uint32_t idx_of(int k, uint32_t gid, uint32_t lane, uint32_t sz) {   // the shader's lanes
    switch (k) {
    case 0: return lane ^ 1u;
    case 1: return lane ^ 13u;
    case 2: return lane + 1u < sz - 1u ? lane + 1u : sz - 1u;
    case 3: return lane ? lane - 1u : 0u;
    case 4: return (lane * 5u + 3u) & (sz - 1u);
    case 5: return 0u;
    case 6: return lane ^ ((lane >> 2) & 7u);
    default: return (gid * 2654435761u >> 7) & (sz - 1u);
    }
}
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 2;
    static uint32_t code[1 << 16]; size_t nb = fread(code, 1, sizeof code, f); fclose(f);
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai}; VkInstance in; CK(vkCreateInstance(&ic, NULL, &in));
    uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(in, &n, &pd);
    VkPhysicalDeviceSubgroupProperties sg = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sg}; vkGetPhysicalDeviceProperties2(pd, &p2);
    printf("%s, driver %#x, subgroup size %u\n", p2.properties.deviceName, p2.properties.driverVersion, sg.subgroupSize); fflush(stdout);
    float pri = 1; VkDeviceQueueCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &pri};
    VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qc};
    VkDevice d; CK(vkCreateDevice(pd, &dc, NULL, &d)); VkQueue q; vkGetDeviceQueue(d, 0, 0, &q);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = NI * W * 4, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT}; VkBuffer buf; CK(vkCreateBuffer(d, &bc, NULL, &buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(d, buf, &mr);
    uint32_t ti = 0; for (; ti < mp.memoryTypeCount; ti++) if ((mr.memoryTypeBits >> ti & 1) && (mp.memoryTypes[ti].propertyFlags & 6) == 6) break;
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = ti}; VkDeviceMemory bm; CK(vkAllocateMemory(d, &ma, NULL, &bm));
    CK(vkBindBufferMemory(d, buf, bm, 0));
    uint32_t *p; CK(vkMapMemory(d, bm, 0, NI * W * 4, 0, (void **)&p)); memset(p, 0xff, NI * W * 4);
    VkDescriptorSetLayoutBinding lb = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dlc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &lb}; VkDescriptorSetLayout dl; CK(vkCreateDescriptorSetLayout(d, &dlc, NULL, &dl));
    VkPipelineLayoutCreateInfo lc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dl}; VkPipelineLayout pl; CK(vkCreatePipelineLayout(d, &lc, NULL, &pl));
    VkShaderModuleCreateInfo mc = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nb, .pCode = code}; VkShaderModule m; CK(vkCreateShaderModule(d, &mc, NULL, &m));
    VkComputePipelineCreateInfo cc = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                      .module = m, .pName = "main"}, .layout = pl};
    VkPipeline pipe; CK(vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &cc, NULL, &pipe));
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps}; VkDescriptorPool dp; CK(vkCreateDescriptorPool(d, &dpc, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dl}; VkDescriptorSet ds; CK(vkAllocateDescriptorSets(d, &dai, &ds));
    VkDescriptorBufferInfo bi = {buf, 0, NI * W * 4};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi};
    vkUpdateDescriptorSets(d, 1, &wr, 0, NULL);
    VkCommandPoolCreateInfo cpc = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool cp; CK(vkCreateCommandPool(d, &cpc, NULL, &cp));
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .commandBufferCount = 1}; VkCommandBuffer c; CK(vkAllocateCommandBuffers(d, &ca, &c));
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CK(vkBeginCommandBuffer(c, &cbi));
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, pipe); vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL); vkCmdDispatch(c, G, 1, 1);
    VkMemoryBarrier hb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    CK(vkEndCommandBuffer(c));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &c}; CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); CK(vkQueueWaitIdle(q));
    int bad = 0, checked = 0;
    for (uint32_t g = 0; g < NI; g++) {
        const uint32_t *e = p + g * W; uint32_t wg = g / L, sz = e[11];
        for (int k = 0; k < 8; k++) {
            uint32_t want = idx_of(k, g, e[9], sz), src = ~0u;
            for (uint32_t h = wg * L; h < (wg + 1) * L; h++) if (p[h * W + 8] == e[8] && p[h * W + 9] == want) { src = h; break; }
            if (src == ~0u) continue;   // a lane past the subgroup: undefined either way
            checked++;
            if (e[k] != p[src * W + 10]) { if (bad++ < 8) printf("invocation %u test %d: lane %u read %#x, want lane %u's %#x\n", g, k, e[9], e[k], want, p[src * W + 10]); }
        }
    }
    printf("%d of %d shuffles right\n", checked - bad, checked);
    return bad != 0 || !checked;
}

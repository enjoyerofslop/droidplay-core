// t_wg32 <comp.spv>: whether a task / mesh kernel's warp operations, written by the translator for a 32-lane warp and made workgroup-wide by spv_wg32.h, give a
// 32-lane warp's results on this GPU. One workgroup of 32 invocations (dm/tools/tests/test_spv_wg32.py writes the shader, as translated and as rewritten): each
// lane writes its ballot of lane % 3 == 0, the count of those set below it (the ballot & the Lt mask), an any of lane 31 and a shuffle of lane * 7 from lane
// (lane + 17) & 31; then the lanes from 20 up return, and the lowest lane left adds the count of a ballot of true into word 128 (a warp-aggregated atomic: 20).
// Exit 0 when all are right. test_spv_wg32.py builds and runs it on a device with DM_TEST_SERIAL=<adb serial>
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#define CK(x) do { VkResult r_ = (x); if (r_) { printf("%s = %d\n", #x, r_); return 1; } } while (0)
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 2;
    static uint32_t code[1 << 16]; size_t nb = fread(code, 1, sizeof code, f); fclose(f);
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    VkInstance in; CK(vkCreateInstance(&ic, NULL, &in));
    uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(in, &n, &pd);
    VkPhysicalDeviceSubgroupProperties sg = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sg}; vkGetPhysicalDeviceProperties2(pd, &p2);
    printf("%s, subgroup size %u, stages %#x\n", p2.properties.deviceName, sg.subgroupSize, sg.supportedStages);
    float pri = 1; VkDeviceQueueCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &pri};
    VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qc};
    VkDevice d; CK(vkCreateDevice(pd, &dc, NULL, &d)); VkQueue q; vkGetDeviceQueue(d, 0, 0, &q);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 1024, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT}; VkBuffer buf; CK(vkCreateBuffer(d, &bc, NULL, &buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(d, buf, &mr);
    uint32_t ti = 0; while (!(mr.memoryTypeBits & (1u << ti)) || (mp.memoryTypes[ti].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) !=
                                                                      (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ti++;
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = ti}; VkDeviceMemory bm; CK(vkAllocateMemory(d, &ma, NULL, &bm));
    CK(vkBindBufferMemory(d, buf, bm, 0));
    uint32_t *p; CK(vkMapMemory(d, bm, 0, 1024, 0, (void **)&p)); for (int i = 0; i < 256; i++) p[i] = 0xdeadbeefu; p[128] = 0;
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
    VkDescriptorBufferInfo bi = {buf, 0, 1024};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi};
    vkUpdateDescriptorSets(d, 1, &wr, 0, NULL);
    VkCommandPoolCreateInfo cpc = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool cp; CK(vkCreateCommandPool(d, &cpc, NULL, &cp));
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .commandBufferCount = 1}; VkCommandBuffer c; CK(vkAllocateCommandBuffers(d, &ca, &c));
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CK(vkBeginCommandBuffer(c, &cbi));
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, pipe); vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL); vkCmdDispatch(c, 1, 1, 1);
    VkMemoryBarrier hb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    CK(vkEndCommandBuffer(c));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &c}; CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); CK(vkQueueWaitIdle(q));
    uint32_t ballot = 0; for (uint32_t l = 0; l < 32; l++) if (l % 3 == 0) ballot |= 1u << l;
    int bad = 0;
    for (uint32_t l = 0; l < 32; l++) {
        uint32_t below = (uint32_t)__builtin_popcount(ballot & ((1u << l) - 1u)), want[4] = {ballot, below, 1u, ((l + 17u) & 31u) * 7u}, *got = p;
        int ok = got[l] == want[0] && got[32 + l] == want[1] && got[64 + l] == want[2] && got[96 + l] == want[3];
        if (!ok || l == 0 || l == 31) printf("lane %2u: ballot %#010x below %2u any %u shuffle %3u%s\n", l, got[l], got[32 + l], got[64 + l], got[96 + l], ok ? "" : "  WRONG");
        bad += !ok;
    }
    printf("aggregated add %u, want 20%s\n", p[128], p[128] == 20 ? "" : "  WRONG"); bad += p[128] != 20;
    return bad != 0;
}

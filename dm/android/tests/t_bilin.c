// t_bilin: spv_bilin.h's gathers against the CPU's bilinear filter, on the device. An 8x8 two-layer texture (D32_SFLOAT, then R32_SFLOAT) holds a smooth function
// of its texel's (i, j, layer); a fragment shader samples layer 1 of it through a LINEAR / LINEAR clamp-to-edge sampler at 64 positions along a row, from 0.4 texels
// before the first texel's centre to past the last's, at v = 2.3 texels, into a 64x1 R32_SFLOAT target: once as compiled, once rewritten by spv_bilin. The comparison
// form (sampler2DArrayShadow, LESS_OR_EQUAL) is drawn the same way on the D32_SFLOAT texture. Each is compared with the bilinear filter of the texels (of the
// comparisons) worked out on the CPU in double; a GPU that can't filter the format linearly reads the nearest texel as compiled (large differences there).
// Prints the largest difference of each; exit 0 when every rewritten one is within 1e-5. test_spv_bilin_device.py builds and runs it with DM_TEST_SERIAL=<adb serial>.
// The shaders (glslc --target-env=vulkan1.1 -O -mfmt=c): t_bilin_vs.inc a full-screen triangle; t_bilin_fs.inc `o = texture(t, vec3((gl_FragCoord.x * pc.scale +
// pc.offset) / 8.0, pc.v, pc.layer)).x` on a sampler2DArray at set 0 binding 0; t_bilin_fsc.inc `o = texture(t, vec4(..., pc.layer, pc.ref))` on a sampler2DArrayShadow
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "../spv_bilin.h"
static const uint32_t vs_spv[] =
#include "t_bilin_vs.inc"
;
static const uint32_t fs_spv[] =
#include "t_bilin_fs.inc"
;
static const uint32_t fsc_spv[] =
#include "t_bilin_fsc.inc"
;
#define N 8
#define W 64
#define CK(x) do { VkResult r_ = (x); if (r_) { printf("%s = %d\n", #x, r_); exit(1); } } while (0)
static VkDevice d; static VkPhysicalDevice pd; static VkPhysicalDeviceMemoryProperties mp; static VkQueue q; static VkCommandPool cp;
typedef struct { float scale, offset, v, layer, ref; } pc_t;
static const pc_t PC = {0.123f, -0.4f, (2.3f + 0.5f) / N, 1.0f, 0.5f};
static double texel(int i, int j, int l) { return 0.05 + 0.08 * i + 0.02 * j + 0.004 * i * j * (1 + l) + 0.003 * i * i; }   // under 1 (a depth)
static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) { for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return i; return ~0u; }
static void *buffer(VkDeviceSize n, VkBufferUsageFlags u, VkBuffer *b) {
    VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = u}; CK(vkCreateBuffer(d, &bc, NULL, b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(d, *b, &mr);
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory m; CK(vkAllocateMemory(d, &ma, NULL, &m)); CK(vkBindBufferMemory(d, *b, m, 0)); void *p; CK(vkMapMemory(d, m, 0, VK_WHOLE_SIZE, 0, &p)); return p;
}
static VkImage image(VkFormat f, uint32_t w, uint32_t h, uint32_t layers, VkImageUsageFlags u) {
    VkImageCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = layers,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = u};
    VkImage img; CK(vkCreateImage(d, &ic, NULL, &img)); VkMemoryRequirements mr; vkGetImageMemoryRequirements(d, img, &mr);
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory m; CK(vkAllocateMemory(d, &ma, NULL, &m)); CK(vkBindImageMemory(d, img, m, 0)); return img;
}
static VkCommandBuffer begin(void) {
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer c; CK(vkAllocateCommandBuffers(d, &ca, &c)); VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CK(vkBeginCommandBuffer(c, &bi)); return c;
}
static void end(VkCommandBuffer c) {
    CK(vkEndCommandBuffer(c)); VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &c}; CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); CK(vkQueueWaitIdle(q));
    vkFreeCommandBuffers(d, cp, 1, &c);
}
static void barrier(VkCommandBuffer c, VkImage img, VkImageAspectFlags a, uint32_t layers, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = {a, 0, 1, 0, layers}};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
// the module (rewritten by spv_bilin when bl): its samples of binding 0 at set 0, one channel
static VkShaderModule module(const uint32_t *w, size_t nw, int bl) {
    uint32_t *code = (uint32_t *)w; size_t n = nw;
    if (bl) { uint16_t b = 0 | 1 << 12; size_t need = 0; int left = 0; spv_bilin(w, nw, 0, &b, 1, NULL, 0, &need, &left);
        if (!need) { printf("spv_bilin rewrote nothing (left %d)\n", left); exit(1); }
        code = malloc(need * 4); n = spv_bilin(w, nw, 0, &b, 1, code, need, NULL, NULL); if (!n) { printf("spv_bilin failed\n"); exit(1); } }
    VkShaderModuleCreateInfo mc = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n * 4, .pCode = code}; VkShaderModule m; CK(vkCreateShaderModule(d, &mc, NULL, &m));
    if (bl) free(code); return m;
}
// one draw of the 64x1 row sampling tex (a view of format vf, layers 2) with the module fs: the 64 results
static void draw(VkImageView tv, VkSampler smp, VkShaderModule fs, float *outv) {
    VkImage rt = image(VK_FORMAT_R32_SFLOAT, W, 1, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkImageViewCreateInfo vc = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = rt, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R32_SFLOAT, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView rv; CK(vkCreateImageView(d, &vc, NULL, &rv));
    VkAttachmentDescription ad = {0, VK_FORMAT_R32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sd = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar};
    VkRenderPassCreateInfo rc = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sd};
    VkRenderPass rp; CK(vkCreateRenderPass(d, &rc, NULL, &rp));
    VkFramebufferCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &rv, .width = W, .height = 1, .layers = 1};
    VkFramebuffer fb; CK(vkCreateFramebuffer(d, &fc, NULL, &fb));
    VkDescriptorSetLayoutBinding lb = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo lc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &lb}; VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(d, &lc, NULL, &dsl));
    VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc_t)};
    VkPipelineLayoutCreateInfo plc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkPipelineLayout pl; CK(vkCreatePipelineLayout(d, &plc, NULL, &pl));
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}; VkDescriptorPoolCreateInfo dpc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
    VkDescriptorPool dp; CK(vkCreateDescriptorPool(d, &dpc, NULL, &dp));
    VkDescriptorSetAllocateInfo da = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl}; VkDescriptorSet ds; CK(vkAllocateDescriptorSets(d, &da, &ds));
    VkDescriptorImageInfo ii = {smp, tv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(d, 1, &wr, 0, NULL);
    VkShaderModule vs = module(vs_spv, sizeof vs_spv / 4, 0);
    VkPipelineShaderStageCreateInfo st[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
                                             {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport v = {0, 0, W, 1, 0, 1}; VkRect2D sc = {{0, 0}, {W, 1}};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &v, .scissorCount = 1, .pScissors = &sc};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
    VkGraphicsPipelineCreateInfo gc = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl, .renderPass = rp};
    VkPipeline p; CK(vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gc, NULL, &p));
    VkBuffer rb; float *rp_ = buffer(W * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rb);
    VkCommandBuffer c = begin();
    VkClearValue cv = {.color = {{-1, 0, 0, 0}}};
    VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = {{0, 0}, {W, 1}}, .clearValueCount = 1, .pClearValues = &cv};
    vkCmdBeginRenderPass(c, &rbi, VK_SUBPASS_CONTENTS_INLINE); vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, NULL); vkCmdPushConstants(c, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof PC, &PC);
    vkCmdDraw(c, 3, 1, 0, 0); vkCmdEndRenderPass(c);
    VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, 1, 1}};
    vkCmdCopyImageToBuffer(c, rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &bc);
    VkMemoryBarrier hb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    end(c); memcpy(outv, rp_, W * 4);
}
// the CPU's bilinear filter of layer 1 at pixel x (cmp: of the comparisons ref <= texel, as LESS_OR_EQUAL)
static double expect(int x, int cmp) {
    double pu = ((x + 0.5) * PC.scale + PC.offset) - 0.5, pv = PC.v * N - 0.5;
    int i0 = (int)floor(pu), j0 = (int)floor(pv); double fu = pu - i0, fv = pv - j0, s = 0;
    for (int dj = 0; dj < 2; dj++) for (int di = 0; di < 2; di++) {
        int i = i0 + di < 0 ? 0 : i0 + di > N - 1 ? N - 1 : i0 + di, j = j0 + dj < 0 ? 0 : j0 + dj > N - 1 ? N - 1 : j0 + dj;
        double t = texel(i, j, 1); if (cmp) t = (double)(float)PC.ref <= (double)(float)t ? 1.0 : 0.0;
        s += t * (di ? fu : 1 - fu) * (dj ? fv : 1 - fv); }
    return s;
}
int main(void) {
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai}; VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 1; vkEnumeratePhysicalDevices(inst, &n, &pd); VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp); vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    printf("%s, driver %#x\n", pp.deviceName, pp.driverVersion);
    uint32_t qn = 16; VkQueueFamilyProperties qf[16]; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf); uint32_t qi = 0; while (qi < qn && !(qf[qi].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qi++;
    float prio = 1; VkDeviceQueueCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qi, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qc}; CK(vkCreateDevice(pd, &dc, NULL, &d)); vkGetDeviceQueue(d, qi, 0, &q);
    VkCommandPoolCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qi}; CK(vkCreateCommandPool(d, &pc, NULL, &cp));
    VkShaderModule fs[2] = {module(fs_spv, sizeof fs_spv / 4, 0), module(fs_spv, sizeof fs_spv / 4, 1)}, fsc[2] = {module(fsc_spv, sizeof fsc_spv / 4, 0), module(fsc_spv, sizeof fsc_spv / 4, 1)};
    int bad = 0; const VkFormat fmts[2] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_R32_SFLOAT};
    for (int fi = 0; fi < 2; fi++) {
        VkFormat f = fmts[fi]; VkImageAspectFlags a = fi ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, f, &fp);
        int lin = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
        VkImage tex = image(f, N, N, 2, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        VkBuffer ub; float *up = buffer(N * N * 2 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &ub);
        for (int l = 0; l < 2; l++) for (int j = 0; j < N; j++) for (int i = 0; i < N; i++) up[(l * N + j) * N + i] = (float)texel(i, j, l);
        VkCommandBuffer c = begin(); barrier(c, tex, a, 2, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy bc = {.imageSubresource = {a, 0, 0, 2}, .imageExtent = {N, N, 1}}; vkCmdCopyBufferToImage(c, ub, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
        barrier(c, tex, a, 2, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); end(c);
        VkImageViewCreateInfo vc = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = tex, .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = f, .subresourceRange = {a, 0, 1, 0, 2}};
        VkImageView tv; CK(vkCreateImageView(d, &vc, NULL, &tv));
        for (int cmp = 0; cmp < (fi ? 1 : 2); cmp++) {
            VkSamplerCreateInfo sc = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .maxLod = 0.25f, .compareEnable = (VkBool32)cmp, .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
            VkSampler smp; CK(vkCreateSampler(d, &sc, NULL, &smp));
            for (int bl = 0; bl < 2; bl++) {
                float got[W]; draw(tv, smp, cmp ? fsc[bl] : fs[bl], got);
                double mx = 0; int at = 0; for (int x = 0; x < W; x++) { double e = fabs(got[x] - expect(x, cmp)); if (e > mx) { mx = e; at = x; } }
                printf("%s%s (%s linearly here), %s: largest difference %.7f (pixel %d: %.6f, want %.6f)\n", fi ? "R32_SFLOAT" : "D32_SFLOAT", cmp ? " compared" : "", lin ? "filtered" : "not filtered",
                       bl ? "spv_bilin's gathers" : "as compiled", mx, at, got[at], expect(at, cmp));
                if (bl && mx > 1e-5) bad++;
            }
        }
    }
    printf("%s\n", bad ? "FAIL" : "ok");
    return bad ? 1 : 0;
}

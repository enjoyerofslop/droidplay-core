"""dm/android/rec_desc.h (a vkUpdateDescriptorSets call's writes packed into a block of bytes for the command recorder's thread, and read back there: gfx_vk_rec.inc
R_UPDDS), built for the PC with the system's clang against the NDK's Vulkan headers and called through ctypes. Skipped without the NDK or a C compiler.
RecorderCalls reads the sources instead: every command the replay can make has a wrapper in gfx_vk_rec.inc."""
import ctypes
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_spv_readonly import NDK, HOST, REPO   # noqa: E402

# Each scenario packs writes into a block whose arrays are then overwritten at the source, unpacks, and checks what the recorder would pass on: → 0, or the line that failed
HARNESS = r'''
#include "rec_desc.h"
#define EXPORT __attribute__((visibility("default")))
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
static uint64_t block[RD_BYTES_MAX / 8 + 64];
#define H(T, v) ((T)(uintptr_t)(v))
EXPORT int a_set_of_every_kind(void) {   // a draw's set: constant buffers, a storage buffer, textures, a storage image, a buffer texture
    VkDescriptorBufferInfo ub[2] = {{H(VkBuffer, 0x11), 256, 400}, {H(VkBuffer, 0x12), 512, 64}}, sb = {H(VkBuffer, 0x13), 4096, 10027008};
    VkDescriptorImageInfo tex[3] = {{H(VkSampler, 0x21), H(VkImageView, 0x31), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {H(VkSampler, 0x22), H(VkImageView, 0x32), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                    {H(VkSampler, 0x23), H(VkImageView, 0x33), VK_IMAGE_LAYOUT_GENERAL}}, img = {VK_NULL_HANDLE, H(VkImageView, 0x34), VK_IMAGE_LAYOUT_GENERAL};
    VkBufferView bv[2] = {H(VkBufferView, 0x41), H(VkBufferView, 0x42)};
    VkDescriptorSet set = H(VkDescriptorSet, 0x99);
    VkWriteDescriptorSet w[6] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &ub[0], NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 2, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, NULL, &ub[1], NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 40, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &sb, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 60, 2, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, tex, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 80, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &img, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 90, 0, 2, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NULL, NULL, bv}};
    size_t n = rd_size(6, w);
    CHECK(n == 6 * sizeof(rd_write) + 3 * sizeof(VkDescriptorBufferInfo) + 4 * sizeof(VkDescriptorImageInfo) + 2 * sizeof(VkBufferView));
    memset(block, 0xee, sizeof block); rd_pack((uint8_t *)block, 6, w);
    CHECK(((uint8_t *)block)[n] == 0xee);   // nothing written past the size
    VkDescriptorBufferInfo ub0 = ub[0], ub1 = ub[1], sb0 = sb; VkDescriptorImageInfo t1 = tex[1], t2 = tex[2], i0 = img; VkBufferView b1 = bv[1];
    memset(ub, 0, sizeof ub); memset(&sb, 0, sizeof sb); memset(tex, 0, sizeof tex); memset(&img, 0, sizeof img); memset(bv, 0, sizeof bv);   // the replay's scratch, reused by the next draw
    VkWriteDescriptorSet o[6]; rd_unpack((const uint8_t *)block, 6, o);
    for (int i = 0; i < 6; i++) { CHECK(o[i].sType == VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET && !o[i].pNext && o[i].dstSet == set); CHECK(o[i].dstBinding == w[i].dstBinding);
        CHECK(o[i].dstArrayElement == w[i].dstArrayElement && o[i].descriptorCount == w[i].descriptorCount && o[i].descriptorType == w[i].descriptorType); }
    CHECK(o[0].pBufferInfo && !o[0].pImageInfo && !o[0].pTexelBufferView && !memcmp(o[0].pBufferInfo, &ub0, sizeof ub0));
    CHECK(!memcmp(o[1].pBufferInfo, &ub1, sizeof ub1) && !memcmp(o[2].pBufferInfo, &sb0, sizeof sb0));
    CHECK(o[3].pImageInfo && !o[3].pBufferInfo && o[3].pImageInfo[1].imageView == t1.imageView && o[3].pImageInfo[2].sampler == t2.sampler && o[3].pImageInfo[2].imageLayout == VK_IMAGE_LAYOUT_GENERAL);
    CHECK(o[4].pImageInfo->imageView == i0.imageView && o[5].pTexelBufferView && !o[5].pImageInfo && o[5].pTexelBufferView[1] == b1);
    for (int i = 0; i < 6; i++) { uintptr_t p = (uintptr_t)(o[i].pBufferInfo ? (const void *)o[i].pBufferInfo : o[i].pImageInfo ? (const void *)o[i].pImageInfo : (const void *)o[i].pTexelBufferView);
        CHECK(!(p & 7) && p >= (uintptr_t)block && p < (uintptr_t)block + n); }   // aligned, and inside the block
    return 0;
}
EXPORT int a_write_of_no_descriptors(void) {   // a count of 0 with no array
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, H(VkDescriptorSet, 1), 5, 0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, NULL, NULL}, o;
    CHECK(rd_size(1, &w) == sizeof(rd_write)); rd_pack((uint8_t *)block, 1, &w); rd_unpack((const uint8_t *)block, 1, &o);
    CHECK(o.descriptorCount == 0 && o.dstBinding == 5);
    return 0;
}
EXPORT int what_is_not_packed(void) {
    VkDescriptorBufferInfo b = {H(VkBuffer, 1), 0, 16}; int chain = 0;
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, H(VkDescriptorSet, 1), 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &b, NULL};
    CHECK(rd_size(1, &w) == sizeof(rd_write) + sizeof b);
    CHECK(rd_size(0, &w) == 0);                                                   // no writes
    w.pNext = &chain; CHECK(rd_size(1, &w) == 0); w.pNext = NULL;                 // a chain this does not copy
    w.descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK; CHECK(rd_size(1, &w) == 0); w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;   // its bytes are in a chain
    w.pBufferInfo = NULL; CHECK(rd_size(1, &w) == 0); w.pBufferInfo = &b;         // no array to copy
    static VkWriteDescriptorSet many[RD_MAX + 1]; for (int i = 0; i <= RD_MAX; i++) many[i] = w;
    CHECK(rd_size(RD_MAX, many) == RD_MAX * (sizeof(rd_write) + sizeof b)); CHECK(rd_size(RD_MAX + 1, many) == 0);   // more than a block's writes
    static VkDescriptorImageInfo big[RD_BYTES_MAX / sizeof(VkDescriptorImageInfo) + 1];   // a texture pool's whole array at once
    VkWriteDescriptorSet p = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, H(VkDescriptorSet, 1), 0, 0, (uint32_t)(sizeof big / sizeof *big), VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, big, NULL, NULL};
    CHECK(rd_size(1, &p) == 0); p.descriptorCount = 4096; CHECK(rd_size(1, &p) == sizeof(rd_write) + 4096 * sizeof *big);
    return 0;
}
'''


def compiler():
    if os.name == 'nt': return None
    if sys.platform == 'darwin': return ['xcrun', 'clang']
    cc = shutil.which('clang') or shutil.which('cc')
    return [cc] if cc else None


@unittest.skipIf(NDK is None or not compiler(), 'no Android NDK (its Vulkan headers) or no C compiler')
class RecDesc(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert NDK is not None
        cls.tmp = Path(tempfile.mkdtemp())
        inc = cls.tmp / 'inc'; inc.mkdir()   # the NDK's vulkan folder alone: its C library's headers are not this machine's
        shutil.copytree(NDK / 'toolchains/llvm/prebuilt' / HOST / 'sysroot/usr/include/vulkan', inc / 'vulkan')
        vk_video = NDK / 'toolchains/llvm/prebuilt' / HOST / 'sysroot/usr/include/vk_video'
        if vk_video.is_dir(): shutil.copytree(vk_video, inc / 'vk_video')
        src = cls.tmp / 'h.c'; src.write_text(HARNESS)
        subprocess.run([*compiler(), '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-I', inc, '-I', REPO / 'dm/android', src, '-o', cls.tmp / 'h.so'], check=True)
        cls.lib = ctypes.CDLL(str(cls.tmp / 'h.so'))

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_a_set_of_every_kind_comes_back_whole_after_its_arrays_are_reused(self): self.assertEqual(self.lib.a_set_of_every_kind(), 0)

    def test_a_write_of_no_descriptors(self): self.assertEqual(self.lib.a_write_of_no_descriptors(), 0)

    def test_chains_unknown_types_and_oversized_calls_are_left_to_a_direct_call(self): self.assertEqual(self.lib.what_is_not_packed(), 0)


# The recorder takes the replay's calls by macro (#define vkCmdX rec_vkCmdX, at the end of gfx_vk_rec.inc). A command with no macro is made on the replay thread, on a
# command buffer the recorder thread is filling: it lands ahead of the binds queued for it, and two threads write one command buffer.
NOT_RECORDED = {}   # a command made only where the recorder is off, by name: the file using it (it must turn itself off with the recorder); vkCmdFillBuffer has a record since the GPU fill (gfx_vk_gmem_inplace.inc gm_fill_gpu)


class RecorderCalls(unittest.TestCase):
    def test_every_command_the_loader_has_goes_through_the_recorder(self):
        android = REPO / 'dm/android'
        rec = (android / 'gfx_vk_rec.inc').read_text(encoding='utf-8')
        macros = set(re.findall(r'^#define (vkCmd\w+) rec_\1$', rec, re.M))
        loaded = set(re.findall(r'X\((vkCmd\w+)\)', (android / 'gfx_vk.h').read_text(encoding='utf-8')))
        self.assertGreater(len(loaded), 30)
        self.assertEqual(sorted(loaded - macros - set(NOT_RECORDED)), [], 'commands of gfx_vk.h with no wrapper in gfx_vk_rec.inc')
        for name, only in NOT_RECORDED.items():   # the exceptions are used where they say, and that file still turns itself off with the recorder
            users = sorted(p.name for p in [*android.glob('gfx_vk*.inc'), android / 'gfx_vk.c'] if re.search(rf'\b{name}\(', p.read_text(encoding='utf-8')))
            self.assertEqual(users, [only])
            self.assertRegex((android / only).read_text(encoding='utf-8'), r'rec_on\(\)')


if __name__ == '__main__':
    unittest.main()

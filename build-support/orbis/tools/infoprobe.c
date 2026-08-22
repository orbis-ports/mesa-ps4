/*
 * Copyright 2026 Mikołaj Mikołajczyk
 * SPDX-License-Identifier: MIT
 *
 * Enumerate physical devices, print what they say, exit. Nothing else.
 *
 * WHY NOT vulkaninfo. vulkaninfo constructs an AppGpu per device, which calls vkCreateDevice - and until
 * this port has a winsys that fails, taking RADV's error path down through radv_destroy_shader_arenas on
 * a device whose arenas were never created. The process dies mid-write and stdout is truncated, which
 * silently cost 57 of the 165 fields of the RADV_DEBUG=info dump: they looked absent from the driver when
 * they were simply never flushed.
 *
 * So this exists to make the phase-2 check possible at all: RADV_DEBUG=info prints radeon_info during
 * physical-device creation, and comparing that dump against notes/radeon_info-bonaire.txt is the most
 * valuable check in the plan - a known-good reference for every derived value, on a laptop.
 *
 * --create-device DOES call vkCreateDevice, and it exists because that premise changed: winsys/amdgpu/ is
 * now built for this platform, so there IS a winsys and the crash above had a cause rather than being a
 * property of the port. Failing inside one of ac_orbis_drm.c's own TODO bodies is the WANTED outcome - the
 * log names the next function to write, which is the same loop that carried phase 2 through thirteen of
 * them with no console. Kept behind a flag so the radeon_info dump above stays a clean measurement.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "shaders/pattern_spv.h"
#include "shaders/tri_vert_spv.inc"
#include "shaders/tri_frag_spv.inc"

/* ---------------- ONE COMPUTE DISPATCH, WHICH IS A DIFFERENT CLAIM FROM "THE SUBMIT WORKED".
 *
 * Everything up to here proves the GPU consumed a command stream: the arm's own end-of-pipe packet wrote a
 * fence label. It does NOT prove the GPU ran a SHADER, and on this port that is a much longer chain - ACO
 * compiling for gfx7, RADV's shader arenas, descriptor writes, the compute PM4 - every link of which is
 * untested. A buffer that comes back holding 0xA5000000 | invocation-id cannot be explained by anything but
 * that chain working.
 *
 * Returns true only on a verified read-back. Every failure prints where it stopped, because on this port the
 * interesting information is which link broke rather than that one did. */

/* ⚠ THE ONE OBJECT THIS PROBE MAY NOT LEAK, and it took the teardown section below with it.
 *
 * This tool leaks deliberately - buffers, memory, layouts, descriptor pools - because it exits immediately and
 * every early return is a failure already being printed. A PIPELINE is different: it holds entries in the
 * device's pipeline cache, and vk_pipeline_cache_destroy asserts that cache is empty. So the probe aborted
 * inside vkDestroyDevice with SIGABRT, in the teardown section written specifically to exercise RADV's
 * destruction path against this arm - the leak defeated the test built to find leaks.
 *
 * A registry rather than cleanup at each return, because the returns are failure paths and a failed probe must
 * not abort on the way out either: the exit status is what build.sh --host-orbis reads. */
#define PROBE_MAX_PIPELINES 4
static VkPipeline probe_pipelines[PROBE_MAX_PIPELINES];
static unsigned   probe_pipeline_count;

static void
probe_keep_pipeline(VkPipeline p)
{
   if (p != VK_NULL_HANDLE && probe_pipeline_count < PROBE_MAX_PIPELINES)
      probe_pipelines[probe_pipeline_count++] = p;
   else if (p != VK_NULL_HANDLE)
      printf("infoprobe: more than %d pipelines - raise PROBE_MAX_PIPELINES or teardown will abort\n",
             PROBE_MAX_PIPELINES);
}

static void
probe_destroy_pipelines(VkDevice dev)
{
   for (unsigned i = 0; i < probe_pipeline_count; i++)
      vkDestroyPipeline(dev, probe_pipelines[i], NULL);
   probe_pipeline_count = 0;
}

static bool
try_dispatch(VkDevice dev, VkPhysicalDevice pdev, VkQueue queue, VkCommandPool pool, uint32_t mem_type)
{
   enum { DWORDS = 1024, GROUP = 64 };
   const VkDeviceSize bytes = DWORDS * 4;
   VkResult r;

   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = bytes,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer buf = VK_NULL_HANDLE;
   r = vkCreateBuffer(dev, &bci, NULL, &buf);
   printf("dispatch: vkCreateBuffer -> %d\n", r);
   if (r != VK_SUCCESS)
      return false;

   /* The buffer's own requirements rather than the type picked earlier: a storage buffer may not accept every
    * memory type, and assuming it does is how this fails on hardware and not on a laptop. */
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(dev, buf, &req);
   if (!(req.memoryTypeBits & (1u << mem_type))) {
      printf("dispatch: memory type %u is not allowed for this buffer (mask 0x%x)\n", mem_type,
             req.memoryTypeBits);
      return false;
   }

   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = mem_type,
   };
   VkDeviceMemory mem = VK_NULL_HANDLE;
   r = vkAllocateMemory(dev, &mai, NULL, &mem);
   if (r != VK_SUCCESS) {
      printf("dispatch: vkAllocateMemory -> %d\n", r);
      return false;
   }
   r = vkBindBufferMemory(dev, buf, mem, 0);
   printf("dispatch: vkBindBufferMemory -> %d\n", r);
   if (r != VK_SUCCESS)
      return false;

   void *ptr = NULL;
   r = vkMapMemory(dev, mem, 0, req.size, 0, &ptr);
   if (r != VK_SUCCESS || !ptr) {
      printf("dispatch: vkMapMemory -> %d\n", r);
      return false;
   }
   /* Poisoned before the dispatch, so "the shader ran" and "the buffer happened to hold the right thing"
    * cannot be confused. */
   for (unsigned i = 0; i < DWORDS; i++)
      ((uint32_t *)ptr)[i] = 0xDEADBEEFu;

   VkShaderModuleCreateInfo smci = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(pattern_comp_spv),
      .pCode = pattern_comp_spv,
   };
   VkShaderModule module = VK_NULL_HANDLE;
   r = vkCreateShaderModule(dev, &smci, NULL, &module);
   printf("dispatch: vkCreateShaderModule -> %d\n", r);
   if (r != VK_SUCCESS)
      return false;

   VkDescriptorSetLayoutBinding bind = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
   };
   VkDescriptorSetLayoutCreateInfo dslci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &bind,
   };
   VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
   r = vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl);
   if (r != VK_SUCCESS) {
      printf("dispatch: vkCreateDescriptorSetLayout -> %d\n", r);
      return false;
   }

   VkPipelineLayoutCreateInfo plci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &dsl,
   };
   VkPipelineLayout layout = VK_NULL_HANDLE;
   r = vkCreatePipelineLayout(dev, &plci, NULL, &layout);
   if (r != VK_SUCCESS) {
      printf("dispatch: vkCreatePipelineLayout -> %d\n", r);
      return false;
   }

   /* THE LINK THAT MATTERS MOST: this is where ACO compiles for gfx7. */
   VkComputePipelineCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_COMPUTE_BIT,
         .module = module,
         .pName = "main",
      },
      .layout = layout,
   };
   VkPipeline pipe = VK_NULL_HANDLE;
   r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe);
   printf("dispatch: vkCreateComputePipelines -> %d%s\n", r, r == VK_SUCCESS ? " (ACO compiled it)" : "");
   if (r != VK_SUCCESS)
      return false;
   probe_keep_pipeline(pipe);

   VkDescriptorPoolSize psize = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1};
   VkDescriptorPoolCreateInfo dpci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &psize,
   };
   VkDescriptorPool dpool = VK_NULL_HANDLE;
   r = vkCreateDescriptorPool(dev, &dpci, NULL, &dpool);
   if (r != VK_SUCCESS) {
      printf("dispatch: vkCreateDescriptorPool -> %d\n", r);
      return false;
   }
   VkDescriptorSetAllocateInfo dsai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = dpool,
      .descriptorSetCount = 1,
      .pSetLayouts = &dsl,
   };
   VkDescriptorSet dset = VK_NULL_HANDLE;
   r = vkAllocateDescriptorSets(dev, &dsai, &dset);
   if (r != VK_SUCCESS) {
      printf("dispatch: vkAllocateDescriptorSets -> %d\n", r);
      return false;
   }

   VkDescriptorBufferInfo dbi = {.buffer = buf, .offset = 0, .range = bytes};
   VkWriteDescriptorSet write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = dset,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &dbi,
   };
   vkUpdateDescriptorSets(dev, 1, &write, 0, NULL);

   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   r = vkAllocateCommandBuffers(dev, &cbai, &cmd);
   if (r != VK_SUCCESS) {
      printf("dispatch: vkAllocateCommandBuffers -> %d\n", r);
      return false;
   }

   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   vkBeginCommandBuffer(cmd, &bi);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &dset, 0, NULL);
   vkCmdDispatch(cmd, DWORDS / GROUP, 1, 1);
   r = vkEndCommandBuffer(cmd);
   if (r != VK_SUCCESS) {
      printf("dispatch: vkEndCommandBuffer -> %d\n", r);
      return false;
   }

   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence = VK_NULL_HANDLE;
   r = vkCreateFence(dev, &fci, NULL, &fence);
   if (r != VK_SUCCESS)
      return false;

   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
   };
   r = vkQueueSubmit(queue, 1, &si, fence);
   printf("dispatch: vkQueueSubmit -> %d\n", r);
   if (r != VK_SUCCESS)
      return false;

   r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 1000000000ull);
   printf("dispatch: vkWaitForFences -> %d%s\n", r, r == VK_TIMEOUT ? " (TIMEOUT)" : "");
   if (r != VK_SUCCESS)
      return false;

   unsigned bad = 0;
   uint32_t first_bad = 0;
   for (unsigned i = 0; i < DWORDS; i++) {
      const uint32_t got = ((const uint32_t *)ptr)[i];
      if (got != (0xA5000000u | i)) {
         if (!bad)
            first_bad = got;
         ++bad;
      }
   }
   printf("dispatch: %u/%u dwords wrong%s", bad, (unsigned)DWORDS, bad ? "" : " - THE GPU RAN THE SHADER\n");
   if (bad)
      printf(", first wrong value 0x%08x (0xDEADBEEF means the shader never wrote)\n", first_bad);
   fflush(stdout);
   return bad == 0;
}


/* ---------------- A GRAPHICS PIPELINE, WHICH IS A MUCH LARGER CLAIM THAN A DISPATCH.
 *
 * The compute test proved one shader stage, one storage descriptor and a dispatch. A draw adds: ACO compiling
 * TWO stages for gfx7 (the vertex stage's export layout is where gfx6/7 differ most from later chips), the
 * fixed-function setup RADV emits for a render target, IMAGE TILING - the first surface this port has ever
 * created whose layout addrlib decided - and a colour-buffer format RADV programs into CB registers.
 *
 * The result is checked per pixel against the fragment shader's own formula, so it verifies WHERE each
 * fragment landed rather than just that something was written. A flat colour would pass with every fragment
 * in the wrong place.
 *
 * Dynamic rendering rather than a VkRenderPass: it is core in Vulkan 1.3, which this driver advertises, and it
 * removes two object types from a test that is not about them.
 */
static bool
try_draw(VkDevice dev, VkPhysicalDevice pdev, VkQueue queue, VkCommandPool pool)
{
   /* ⚠ SIZE FROM THE ENVIRONMENT, because the tiling question only exists above the macro-tiling threshold.
    * 64x64 degrades to a simpler tile mode and says nothing about a 1920x1080 colour target, which is the
    * surface the console refuses to render. The default is unchanged, so every existing invocation behaves as
    * before:
    *
    *     ORBIS_PROBE_W=1920 ORBIS_PROBE_H=1080 infoprobe --create-device
    *     ORBIS_PROBE_W=1920 ORBIS_PROBE_H=1080 ORBIS_TILE_MODE=1d infoprobe --create-device
    *
    * The two "image wants N bytes" lines are what say whether patch 0011's clamp took effect - and that is a
    * question the console cannot answer for itself, because a run that hangs never gets its log off the wire. */
   const char *const wenv = getenv("ORBIS_PROBE_W");
   const char *const henv = getenv("ORBIS_PROBE_H");
   const unsigned W = wenv != NULL ? (unsigned)strtoul(wenv, NULL, 10) : 64;
   const unsigned H = henv != NULL ? (unsigned)strtoul(henv, NULL, 10) : 64;
   const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
   VkResult r;

   /* ---- the render target. OPTIMAL tiling deliberately: LINEAR would sidestep addrlib, which is the part
    * this test most wants to exercise on a chip whose tile tables this port generated itself. */
   VkImageCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = fmt,
      .extent = {W, H, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage img = VK_NULL_HANDLE;
   r = vkCreateImage(dev, &ici, NULL, &img);
   printf("draw: vkCreateImage %ux%u OPTIMAL -> %d\n", W, H, r);
   if (r != VK_SUCCESS)
      return false;

   VkMemoryRequirements ireq;
   vkGetImageMemoryRequirements(dev, img, &ireq);
   printf("draw: image wants %llu bytes, alignment %llu, types 0x%x\n", (unsigned long long)ireq.size,
          (unsigned long long)ireq.alignment, ireq.memoryTypeBits);

   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pdev, &mp);
   uint32_t img_type = UINT32_MAX, host_type = UINT32_MAX;
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
      const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
      if (img_type == UINT32_MAX && (ireq.memoryTypeBits & (1u << i)) &&
          (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
         img_type = i;
      const VkMemoryPropertyFlags want =
         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      if (host_type == UINT32_MAX && (f & want) == want)
         host_type = i;
   }
   if (img_type == UINT32_MAX || host_type == UINT32_MAX) {
      printf("draw: no suitable memory types (image %u, host %u)\n", img_type, host_type);
      return false;
   }

   VkMemoryAllocateInfo imai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = ireq.size,
      .memoryTypeIndex = img_type,
   };
   VkDeviceMemory imem = VK_NULL_HANDLE;
   r = vkAllocateMemory(dev, &imai, NULL, &imem);
   if (r != VK_SUCCESS) {
      printf("draw: image vkAllocateMemory -> %d\n", r);
      return false;
   }
   r = vkBindImageMemory(dev, img, imem, 0);
   printf("draw: vkBindImageMemory -> %d\n", r);
   if (r != VK_SUCCESS)
      return false;

   VkImageViewCreateInfo ivci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = fmt,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   VkImageView view = VK_NULL_HANDLE;
   r = vkCreateImageView(dev, &ivci, NULL, &view);
   printf("draw: vkCreateImageView -> %d\n", r);
   if (r != VK_SUCCESS)
      return false;

   /* ---- the pipeline. TWO stages through ACO, which is the point. */
   VkShaderModuleCreateInfo vsci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = sizeof(tri_vert_spv),
                                    .pCode = tri_vert_spv};
   VkShaderModuleCreateInfo fsci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = sizeof(tri_frag_spv),
                                    .pCode = tri_frag_spv};
   VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
   if (vkCreateShaderModule(dev, &vsci, NULL, &vs) != VK_SUCCESS ||
       vkCreateShaderModule(dev, &fsci, NULL, &fs) != VK_SUCCESS) {
      printf("draw: vkCreateShaderModule failed\n");
      return false;
   }

   VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   VkPipelineLayout layout = VK_NULL_HANDLE;
   r = vkCreatePipelineLayout(dev, &plci, NULL, &layout);
   if (r != VK_SUCCESS) {
      printf("draw: vkCreatePipelineLayout -> %d\n", r);
      return false;
   }

   VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"},
   };
   VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport vp = {0, 0, (float)W, (float)H, 0, 1};
   VkRect2D sc = {{0, 0}, {W, H}};
   VkPipelineViewportStateCreateInfo vps = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc};
   VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
   VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cb = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &cba};
   VkPipelineRenderingCreateInfo pri = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt};

   VkGraphicsPipelineCreateInfo gpci = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &pri,
      .stageCount = 2, .pStages = stages,
      .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps,
      .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb,
      .layout = layout,
   };
   VkPipeline pipe = VK_NULL_HANDLE;
   r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe);
   printf("draw: vkCreateGraphicsPipelines -> %d%s\n", r,
          r == VK_SUCCESS ? " (ACO compiled a VS and a PS for gfx7)" : "");
   if (r != VK_SUCCESS)
      return false;
   probe_keep_pipeline(pipe);

   /* ---- the read-back buffer */
   const VkDeviceSize rb_bytes = (VkDeviceSize)W * H * 4;
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = rb_bytes,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
   VkBuffer rb = VK_NULL_HANDLE;
   if (vkCreateBuffer(dev, &bci, NULL, &rb) != VK_SUCCESS) {
      printf("draw: read-back vkCreateBuffer failed\n");
      return false;
   }
   VkMemoryRequirements breq;
   vkGetBufferMemoryRequirements(dev, rb, &breq);
   /* ⚠ ASKED AND THEN IGNORED, until now. The requirements were fetched for their SIZE and the type index
    * came from a scan of memory properties that never consulted memoryTypeBits - so a buffer this device
    * does not allow in that heap would fail at vkBindBufferMemory with "read-back memory failed" and no
    * hint. try_dispatch already checks this for its storage buffer; this path did not. */
   if (!(breq.memoryTypeBits & (1u << host_type))) {
      printf("draw: memory type %u is not allowed for the read-back buffer (mask 0x%x)\n", host_type,
             breq.memoryTypeBits);
      return false;
   }
   VkMemoryAllocateInfo bmai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = breq.size,
                                .memoryTypeIndex = host_type};
   VkDeviceMemory bmem = VK_NULL_HANDLE;
   if (vkAllocateMemory(dev, &bmai, NULL, &bmem) != VK_SUCCESS ||
       vkBindBufferMemory(dev, rb, bmem, 0) != VK_SUCCESS) {
      printf("draw: read-back memory failed\n");
      return false;
   }
   void *rbp = NULL;
   if (vkMapMemory(dev, bmem, 0, rb_bytes, 0, &rbp) != VK_SUCCESS || !rbp) {
      printf("draw: read-back vkMapMemory failed\n");
      return false;
   }
   memset(rbp, 0xCD, rb_bytes); /* poison, so "nothing was copied" names itself */

   /* ---- record: transition, render, transition, copy */
   VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1};
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   if (vkAllocateCommandBuffers(dev, &cbai, &cmd) != VK_SUCCESS) {
      printf("draw: vkAllocateCommandBuffers failed\n");
      return false;
   }

   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   vkBeginCommandBuffer(cmd, &bi);

   VkImageMemoryBarrier to_colour = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .image = img,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &to_colour);

   VkRenderingAttachmentInfo att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = view,
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue = {.color = {{0, 0, 0, 0}}},
   };
   VkRenderingInfo ri = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = {{0, 0}, {W, H}},
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &att,
   };
   vkCmdBeginRendering(cmd, &ri);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   /* ⚠ REPEATED ON DEMAND, TO FORCE RADV TO CHAIN ITS COMMAND STREAM. RADV grows a command buffer as a linked
    * list of IBs joined by IT_INDIRECT_BUFFER with CHAIN, and the orbis arm has to flatten that list into one
    * buffer - a fence appended after a chain is unreachable, which cost an evening of console runs.
    *
    * A small command buffer never chains, so on the laptop the flattening path would go untested and its first
    * real exercise would be a game. ORBIS_PROBE_DRAWS=2000 makes this probe's stream long enough to chain
    * several times, which is what turns "the flattener works" into something measurable here rather than there.
    * Default 1: every existing invocation behaves exactly as before. */
   const char *const draws_env = getenv("ORBIS_PROBE_DRAWS");
   const unsigned    draws = draws_env != NULL ? (unsigned)strtoul(draws_env, NULL, 10) : 1;
   for (unsigned d = 0; d < (draws ? draws : 1); ++d)
      vkCmdDraw(cmd, 3, 1, 0, 0);
   vkCmdEndRendering(cmd);

   VkImageMemoryBarrier to_src = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .image = img,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_src);

   VkBufferImageCopy copy = {
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {W, H, 1},
   };
   vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &copy);

   r = vkEndCommandBuffer(cmd);
   printf("draw: vkEndCommandBuffer -> %d\n", r);
   if (r != VK_SUCCESS)
      return false;

   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence = VK_NULL_HANDLE;
   if (vkCreateFence(dev, &fci, NULL, &fence) != VK_SUCCESS)
      return false;
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                      .pCommandBuffers = &cmd};
   r = vkQueueSubmit(queue, 1, &si, fence);
   printf("draw: vkQueueSubmit -> %d\n", r);
   if (r != VK_SUCCESS)
      return false;
   r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 1000000000ull);
   printf("draw: vkWaitForFences -> %d%s\n", r, r == VK_TIMEOUT ? " (TIMEOUT)" : "");
   if (r != VK_SUCCESS)
      return false;

   /* ---- verify against the fragment shader's own formula */
   const uint8_t *px = rbp;
   unsigned bad = 0;
   unsigned first_x = 0, first_y = 0;
   uint32_t first_got = 0;
   for (unsigned y = 0; y < H; y++) {
      for (unsigned x = 0; x < W; x++) {
         const uint8_t *p = px + (y * W + x) * 4;
         const bool ok = p[0] == (x & 0xff) && p[1] == (y & 0xff) && p[2] == 0xa5 && p[3] == 0xff;
         if (!ok) {
            if (!bad) {
               first_x = x;
               first_y = y;
               first_got = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                           ((uint32_t)p[3] << 24);
            }
            bad++;
         }
      }
   }
   if (bad)
      printf("draw: %u/%u pixels wrong, first at (%u,%u) got 0x%08x (0xCDCDCDCD means nothing was copied)\n",
             bad, W * H, first_x, first_y, first_got);
   else
      printf("draw: %u/%u pixels wrong - THE GPU RASTERISED THE TRIANGLE\n", bad, W * H);
   fflush(stdout);
   return bad == 0;
}

static void
try_create_device(VkPhysicalDevice pdev)
{
   /* One queue on the one family RADV reports for GFX. Nothing else is requested: the point is to reach
    * radv_create_winsys and then the first ac_drm_* body that is not written yet, not to exercise features. */
   uint32_t nfam = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nfam, NULL);
   printf("infoprobe: %u queue family/families\n", nfam);
   if (!nfam)
      return;

   const float prio = 1.0f;
   VkDeviceQueueCreateInfo q = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &prio,
   };
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &q,
   };

   VkDevice dev = VK_NULL_HANDLE;
   VkResult r = vkCreateDevice(pdev, &dci, NULL, &dev);
   printf("infoprobe: vkCreateDevice -> %d%s\n", r, r == VK_SUCCESS ? " (SUCCESS)" : "");
   fflush(stdout);
   if (r != VK_SUCCESS)
      return;

   /* AND NOW THE ACTUAL MILESTONE, because "vkCreateDevice returned 0" is not one: allocate memory, map it,
    * write a pattern and read it back. That is what distinguishes a mapping the driver ACCEPTED from one that
    * addresses real pages - and the two look identical right up to the first GPU read.
    *
    * HOST_VISIBLE | HOST_COHERENT deliberately: on the console that means ONION, and a CPU read-back of a
    * write-combined GARLIC buffer would measure the write-combine buffer rather than memory. The GNM backend
    * learned that once already. */
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pdev, &mp);

   uint32_t type = UINT32_MAX;
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
      const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      if ((mp.memoryTypes[i].propertyFlags & want) == want) {
         type = i;
         break;
      }
   }
   if (type == UINT32_MAX) {
      printf("infoprobe: no HOST_VISIBLE|HOST_COHERENT memory type\n");
      return;
   }

   const VkDeviceSize bytes = 4096;
   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = bytes,
      .memoryTypeIndex = type,
   };
   VkDeviceMemory mem = VK_NULL_HANDLE;
   r = vkAllocateMemory(dev, &mai, NULL, &mem);
   printf("infoprobe: vkAllocateMemory(type %u) -> %d\n", type, r);
   if (r != VK_SUCCESS)
      return;

   void *ptr = NULL;
   r = vkMapMemory(dev, mem, 0, bytes, 0, &ptr);
   printf("infoprobe: vkMapMemory -> %d, ptr %p\n", r, ptr);
   if (r != VK_SUCCESS || !ptr)
      return;

   /* A pattern that is not zero and not a repeat, so a mapping that silently addresses the wrong page fails
    * this rather than passing by accident. */
   uint32_t *w = ptr;
   for (unsigned i = 0; i < bytes / 4; i++)
      w[i] = 0xA5000000u | i;

   unsigned bad = 0;
   for (unsigned i = 0; i < bytes / 4; i++)
      if (w[i] != (0xA5000000u | i))
         bad++;
   printf("infoprobe: readback %s (%u/%u dwords wrong)\n", bad ? "MISMATCH" : "OK", bad,
          (unsigned)(bytes / 4));

   vkUnmapMemory(dev, mem);
   vkFreeMemory(dev, mem, NULL);
   printf("infoprobe: unmap + free returned\n");
   fflush(stdout);

   /* AN EMPTY SUBMIT, WHICH IS THE POINT. Phase 4 step 1 wants the SHAPE of a real submission - how many IB
    * chunks arrive, in what order, which chunk kinds RADV uses - and an empty command buffer still carries
    * RADV's preambles, its flush postamble and its fence and syncobj chunks. Recording nothing keeps the log
    * readable, and there is nothing to read back anyway while ac_drm_cs_submit_raw2 submits nothing. */
   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(dev, 0, 0, &queue);

   VkCommandPoolCreateInfo pci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   VkCommandPool pool = VK_NULL_HANDLE;
   r = vkCreateCommandPool(dev, &pci, NULL, &pool);
   printf("infoprobe: vkCreateCommandPool -> %d\n", r);
   if (r != VK_SUCCESS)
      return;

   /* ⚠ TWO, BECAUSE THE SEMAPHORE PAIR BELOW NEEDS TWO. The pair used to submit ONE command buffer twice
    * with the second submission issued while the first was still pending, which Vulkan forbids without
    * VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT - and this buffer is recorded without it. A probe written
    * to check that the arm orders two submissions correctly must not itself be the invalid one, or a driver
    * that noticed would be blamed for the diagnosis. Two buffers is also what an application does. */
   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 2,
   };
   VkCommandBuffer cmds[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
   r = vkAllocateCommandBuffers(dev, &cbai, cmds);
   printf("infoprobe: vkAllocateCommandBuffers -> %d\n", r);
   if (r != VK_SUCCESS)
      return;
   VkCommandBuffer cmd = cmds[0];

   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   for (unsigned i = 0; i < 2; i++) {
      vkBeginCommandBuffer(cmds[i], &bi);
      r = vkEndCommandBuffer(cmds[i]);
      if (r != VK_SUCCESS)
         break;
   }
   printf("infoprobe: vkEndCommandBuffer -> %d\n", r);

   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence = VK_NULL_HANDLE;
   r = vkCreateFence(dev, &fci, NULL, &fence);
   printf("infoprobe: vkCreateFence -> %d\n", r);
   if (r != VK_SUCCESS)
      return;

   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
   };
   r = vkQueueSubmit(queue, 1, &si, fence);
   printf("infoprobe: vkQueueSubmit -> %d\n", r);
   fflush(stdout);
   if (r != VK_SUCCESS)
      return;

   /* One second, not UINT64_MAX: a wait that never returns is indistinguishable from a driver that hung, and
    * this is exactly the phase where a fence might never be signalled. */
   r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 1000000000ull);
   printf("infoprobe: vkWaitForFences -> %d%s\n", r, r == VK_TIMEOUT ? " (TIMEOUT)" : "");
   fflush(stdout);

   /* ---------------- TWO SUBMITS AND A SEMAPHORE BETWEEN THEM, which is the whole of phase 5.
    *
    * The first submit signals the semaphore, the second waits on it. That is the ordering every renderer
    * relies on and the one the arm got wrong until the out-syncobjs carried a GPU sequence number: signalling
    * them when the packets were queued told the second submit the first had finished before it started.
    *
    * On this arm the expected outcome is that NOTHING has to be emitted for the wait - one in-order ring means
    * a syncobj owned by an earlier submission is already ordered - so a clean pass here plus the absence of
    * "waiting on the CPU for a syncobj no submission owns" in the log is the result. That absence is as much
    * the measurement as the return code. */
   VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkSemaphore sem = VK_NULL_HANDLE;
   r = vkCreateSemaphore(dev, &sci, NULL, &sem);
   printf("infoprobe: vkCreateSemaphore -> %d\n", r);
   if (r != VK_SUCCESS)
      return;

   VkFence fence2 = VK_NULL_HANDLE;
   r = vkCreateFence(dev, &fci, NULL, &fence2);
   if (r != VK_SUCCESS)
      return;

   const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   VkSubmitInfo si_signal = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &sem,
   };
   VkSubmitInfo si_wait = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &sem,
      .pWaitDstStageMask = &wait_stage,
      .commandBufferCount = 1,
      /* Not cmds[0]: submit A above is still pending when this is submitted, and one command buffer cannot
       * be in two pending submissions at once. */
      .pCommandBuffers = &cmds[1],
   };

   r = vkQueueSubmit(queue, 1, &si_signal, VK_NULL_HANDLE);
   printf("infoprobe: submit A (signals a semaphore) -> %d\n", r);
   if (r != VK_SUCCESS)
      return;
   r = vkQueueSubmit(queue, 1, &si_wait, fence2);
   printf("infoprobe: submit B (waits on it) -> %d\n", r);
   if (r != VK_SUCCESS)
      return;

   r = vkWaitForFences(dev, 1, &fence2, VK_TRUE, 1000000000ull);
   printf("infoprobe: ordered pair -> %d%s\n", r, r == VK_TIMEOUT ? " (TIMEOUT)" : "");
   fflush(stdout);

   const bool dispatched = try_dispatch(dev, pdev, queue, pool, type);
   (void)dispatched;

   try_draw(dev, pdev, queue, pool);

   /* ---------------- TEARDOWN, WHICH HAS NEVER RUN.
    *
    * Every probe so far has deliberately skipped vkDestroyDevice: on the console a crash there would land
    * after every measurement rather than invalidating one, so it was the right call while bringing things up.
    * The cost is that RADV's destruction path has never touched this arm, and that path is where the arm's
    * unusual choices live - an arena that is never released, BOs marked foreign that must not be freed, a ring
    * of fence command buffers, a sync provider handed out as heap copies. Every one of those is a leak or a
    * double-free waiting for the first application that creates and destroys a device twice, which is every
    * application.
    *
    * Cheap to find out here: the host has no GPU to hang and a crash is a backtrace rather than a flash. */
   printf("infoprobe: --- teardown ---\n");
   fflush(stdout);
   vkDestroyCommandPool(dev, pool, NULL);
   printf("infoprobe: vkDestroyCommandPool returned\n");
   fflush(stdout);
   vkDestroyFence(dev, fence, NULL);
   vkDestroyFence(dev, fence2, NULL);
   vkDestroySemaphore(dev, sem, NULL);
   printf("infoprobe: fences and the semaphore destroyed\n");
   fflush(stdout);
   probe_destroy_pipelines(dev);
   printf("infoprobe: %s\n", "pipelines destroyed - the device's pipeline cache must be empty to destroy it");
   fflush(stdout);
   vkDestroyDevice(dev, NULL);
   printf("infoprobe: vkDestroyDevice returned\n");
   fflush(stdout);

   /* ---------------- AND AGAIN, WHICH IS THE ACTUAL TEST.
    *
    * A clean vkDestroyDevice proves nothing on its own: every unusual thing this arm does at teardown is
    * SILENT. The arena is deliberately never released, BOs marked foreign must not be freed, the fence ring
    * hands out slots by sequence number, the sync provider is handed out as heap copies. None of that crashes
    * on the way down - it shows up as a SECOND device that cannot allocate, whose fences are already ahead of
    * its submissions, or whose VA allocator has lost its window.
    *
    * Every application creates a device more than once over its life. So: do it again, allocate, map, write,
    * read back. If the second device behaves like the first, teardown released what it should and kept what it
    * must. */
   /* THREE MORE, BECAUSE THE QUESTION IS WHETHER THE RESIDUE GROWS. The arm reports what it still holds at
    * every teardown, and one report cannot distinguish a LEAK from state that legitimately outlives the
    * device: RADV's amdgpu winsys is refcounted per device cookie and keeps its own BOs, VA ranges and
    * timeline syncobj alive across device create/destroy. Constant residue means that; growing residue means a
    * leak, and it is the growth that a real application would eventually die of. */
   for (unsigned pass = 2; pass <= 4; pass++) {
      printf("infoprobe: --- device %u ---\n", pass);
      fflush(stdout);

      VkDevice devN = VK_NULL_HANDLE;
      r = vkCreateDevice(pdev, &dci, NULL, &devN);
      if (r != VK_SUCCESS) {
         printf("infoprobe: device %u vkCreateDevice -> %d\n", pass, r);
         return;
      }

      VkDeviceMemory memN = VK_NULL_HANDLE;
      r = vkAllocateMemory(devN, &mai, NULL, &memN);
      if (r != VK_SUCCESS) {
         printf("infoprobe: device %u vkAllocateMemory -> %d\n", pass, r);
         return;
      }

      void *ptrN = NULL;
      r = vkMapMemory(devN, memN, 0, bytes, 0, &ptrN);
      if (r != VK_SUCCESS || !ptrN) {
         printf("infoprobe: device %u vkMapMemory -> %d\n", pass, r);
         return;
      }

      /* A pattern that differs per pass, so a stale mapping still holding an earlier one fails rather than
       * passing. */
      const uint32_t tag = 0x5A000000u | (pass << 20);
      uint32_t *wN = ptrN;
      for (unsigned i = 0; i < bytes / 4; i++)
         wN[i] = tag | i;
      unsigned badN = 0;
      for (unsigned i = 0; i < bytes / 4; i++)
         if (wN[i] != (tag | i))
            badN++;
      printf("infoprobe: device %u at %p, readback %s (%u wrong)\n", pass, ptrN, badN ? "MISMATCH" : "OK",
             badN);

      vkUnmapMemory(devN, memN);
      vkFreeMemory(devN, memN, NULL);
      vkDestroyDevice(devN, NULL);
      fflush(stdout);
   }
   printf("infoprobe: four devices created and destroyed\n");
   fflush(stdout);

   /* No vkDestroyDevice: tearing down is a separate question from bringing up, and RADV's teardown reaches
    * code paths (shader arenas, BO destruction) that this port has not exercised yet. When it is time, that
    * is its own step with its own log. */
}

int
main(int argc, char **argv)
{
   const bool want_device = argc > 1 && !strcmp(argv[1], "--create-device");
   VkInstance instance;
   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "orbis-infoprobe",
      /* ⚠ 1.3, AND IT IS LOAD-BEARING. The graphics test uses vkCmdBeginRendering, which is Vulkan 1.3 CORE -
       * with a 1.1 instance the loader's dispatch entry for it is NULL and calling it jumps to address zero.
       * That is what the first run of the draw test did: a SIGSEGV at 0x0 called from main, which reads like a
       * crash inside the driver and is nothing of the kind. The driver advertises 1.3.358, so ask for it. */
      .apiVersion = VK_API_VERSION_1_3,
   };
   VkInstanceCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };

   VkResult r = vkCreateInstance(&ci, NULL, &instance);
   if (r != VK_SUCCESS) {
      fprintf(stderr, "infoprobe: vkCreateInstance failed: %d\n", r);
      return 1;
   }

   uint32_t n = 0;
   r = vkEnumeratePhysicalDevices(instance, &n, NULL);
   if (r != VK_SUCCESS) {
      fprintf(stderr, "infoprobe: vkEnumeratePhysicalDevices failed: %d\n", r);
      return 1;
   }
   printf("infoprobe: %u physical device(s)\n", n);
   if (!n)
      return 1;

   VkPhysicalDevice *devs = calloc(n, sizeof(*devs));
   vkEnumeratePhysicalDevices(instance, &n, devs);

   for (uint32_t i = 0; i < n; i++) {
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(devs[i], &p);
      printf("infoprobe: [%u] %s  api %u.%u.%u  driver 0x%x  vendor 0x%04x device 0x%04x  type %d\n", i,
             p.deviceName, VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
             VK_VERSION_PATCH(p.apiVersion), p.driverVersion, p.vendorID, p.deviceID, p.deviceType);
      printf("infoprobe: [%u] timestampPeriod %.3f ns  maxComputeWorkGroupInvocations %u\n", i,
             p.limits.timestampPeriod, p.limits.maxComputeWorkGroupInvocations);
      if (want_device)
         try_create_device(devs[i]);
   }

   /* No vkDestroyInstance: the point is to exit before touching anything a missing winsys would break,
    * and the process is about to end anyway. */
   fflush(stdout);
   return 0;
}

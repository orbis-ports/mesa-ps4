// vsfetchprobe - build one gfx7 GRAPHICS pipeline with a chosen vertex attribute layout, and exit.
//
// acoprobe.c beside this file answers "what did ACO make of this compute module". This answers the
// question acoprobe cannot reach: what does the driver emit to FETCH A VERTEX ATTRIBUTE, for a
// format, an offset and a stride we choose. Under
//
//   LD_PRELOAD=libamdgpu_noop_drm_shim.so AMDGPU_GPU_ID=bonaire RADV_DEBUG=asm
//
// the driver believes it is a Sea Islands part, so the ISA printed is the ISA this port would run.
// Nothing is executed - the shim stubs the submit ioctl. The only thing under test is the COMPILER.
//
// ⚠ WHAT IT EXISTS TO MEASURE. ac_shader_util.c's is_fetch_size_safe() exempts GFX7-GFX9 from every
// alignment requirement: on those parts it declares any fetch size safe at any address. That is a
// claim about silicon, and this port runs on silicon nobody validated it against. A dword-aligned
// address that is not 8-aligned, with an 8-byte element, is the case where the claim decides the
// instruction - one tbuffer_load_format_xyzw if it holds, two tbuffer_load_format_xy if it does not.
// ORBIS_VS_STRICT_ALIGN=1 withdraws the exemption, and running this twice shows the difference as
// instructions rather than as an argument.
//
//   cc -O2 -o vsfetchprobe build-support/orbis/tools/vsfetchprobe.c -lvulkan
//   LD_PRELOAD=build-host/src/amd/drm-shim/libamdgpu_noop_drm_shim.so
//     AMDGPU_GPU_ID=bonaire
//     VK_DRIVER_FILES=build-host/src/amd/vulkan/radeon_devenv_icd.x86_64.json
//     RADV_DEBUG=asm ./vsfetchprobe [format] [offset] [stride] [same|split]
//
// Defaults are the layout that raised the question: R16G16B16A16_SINT at offset 36 in a 76-byte
// stride, which is Beetle PSX HW's texture-coordinate attribute.
//
// ⚠ THE SPIR-V IS EMBEDDED, not read from disk, so this tool cannot be run against the wrong module.
// It was generated from the GLSL below with glslc -O; regenerate the same way if it ever changes.
//
//   // vertex
//   #version 450
//   layout(location = 0) in vec4 pos;
//   layout(location = 1) in ivec4 ints;
//   layout(location = 0) out flat ivec4 out_ints;
//   void main() { gl_Position = pos; out_ints = ints; }
//
//   // fragment
//   #version 450
//   layout(location = 0) in flat ivec4 in_ints;
//   layout(location = 0) out vec4 colour;
//   void main() { colour = vec4(in_ints); }
//
// SPDX-License-Identifier: MIT
#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t vs_spirv[] = {
   0x07230203, 0x00010000, 0x000d000b, 0x0000001b, 0x00000000, 0x00020011,
   0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
   0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0009000f, 0x00000000,
   0x00000004, 0x6e69616d, 0x00000000, 0x0000000d, 0x00000011, 0x00000017,
   0x00000019, 0x00030047, 0x0000000b, 0x00000002, 0x00050048, 0x0000000b,
   0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x0000000b, 0x00000001,
   0x0000000b, 0x00000001, 0x00050048, 0x0000000b, 0x00000002, 0x0000000b,
   0x00000003, 0x00050048, 0x0000000b, 0x00000003, 0x0000000b, 0x00000004,
   0x00040047, 0x00000011, 0x0000001e, 0x00000000, 0x00030047, 0x00000017,
   0x0000000e, 0x00040047, 0x00000017, 0x0000001e, 0x00000000, 0x00040047,
   0x00000019, 0x0000001e, 0x00000001, 0x00020013, 0x00000002, 0x00030021,
   0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017,
   0x00000007, 0x00000006, 0x00000004, 0x00040015, 0x00000008, 0x00000020,
   0x00000000, 0x0004002b, 0x00000008, 0x00000009, 0x00000001, 0x0004001c,
   0x0000000a, 0x00000006, 0x00000009, 0x0006001e, 0x0000000b, 0x00000007,
   0x00000006, 0x0000000a, 0x0000000a, 0x00040020, 0x0000000c, 0x00000003,
   0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000003, 0x00040015,
   0x0000000e, 0x00000020, 0x00000001, 0x0004002b, 0x0000000e, 0x0000000f,
   0x00000000, 0x00040020, 0x00000010, 0x00000001, 0x00000007, 0x0004003b,
   0x00000010, 0x00000011, 0x00000001, 0x00040020, 0x00000013, 0x00000003,
   0x00000007, 0x00040017, 0x00000015, 0x0000000e, 0x00000004, 0x00040020,
   0x00000016, 0x00000003, 0x00000015, 0x0004003b, 0x00000016, 0x00000017,
   0x00000003, 0x00040020, 0x00000018, 0x00000001, 0x00000015, 0x0004003b,
   0x00000018, 0x00000019, 0x00000001, 0x00050036, 0x00000002, 0x00000004,
   0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x00000007,
   0x00000012, 0x00000011, 0x00050041, 0x00000013, 0x00000014, 0x0000000d,
   0x0000000f, 0x0003003e, 0x00000014, 0x00000012, 0x0004003d, 0x00000015,
   0x0000001a, 0x00000019, 0x0003003e, 0x00000017, 0x0000001a, 0x000100fd,
   0x00010038,
};

static const uint32_t fs_spirv[] = {
   0x07230203, 0x00010000, 0x000d000b, 0x00000010, 0x00000000, 0x00020011,
   0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
   0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000004,
   0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x0000000d, 0x00030010,
   0x00000004, 0x00000007, 0x00040047, 0x00000009, 0x0000001e, 0x00000000,
   0x00030047, 0x0000000d, 0x0000000e, 0x00040047, 0x0000000d, 0x0000001e,
   0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
   0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006,
   0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b,
   0x00000008, 0x00000009, 0x00000003, 0x00040015, 0x0000000a, 0x00000020,
   0x00000001, 0x00040017, 0x0000000b, 0x0000000a, 0x00000004, 0x00040020,
   0x0000000c, 0x00000001, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d,
   0x00000001, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003,
   0x000200f8, 0x00000005, 0x0004003d, 0x0000000b, 0x0000000e, 0x0000000d,
   0x0004006f, 0x00000007, 0x0000000f, 0x0000000e, 0x0003003e, 0x00000009,
   0x0000000f, 0x000100fd, 0x00010038,
};

// The three-component variant. Its GLSL is the same with ivec3 in place of ivec4:
//
//   layout(location = 1) in ivec3 ints;    // vertex
//   layout(location = 0) in flat ivec3 in_ints;   // fragment
//
// ⚠ IT EXISTS BECAUSE THREE-COMPONENT 8- AND 16-BIT FORMATS HAVE NO HARDWARE FORMAT AT ALL.
// ac_shader_util.c's table gives them has_hw_format 0xb - 1, 2 and 4 channels, never 3 - so every one
// of them has to be fetched as two instructions, and that split is a different code path from the
// whole-vector fetch the ivec4 modules exercise.
static const uint32_t vs3_spirv[] = {
   0x07230203, 0x00010000, 0x000d000b, 0x0000001b, 0x00000000, 0x00020011,
   0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
   0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0009000f, 0x00000000,
   0x00000004, 0x6e69616d, 0x00000000, 0x0000000d, 0x00000011, 0x00000017,
   0x00000019, 0x00030047, 0x0000000b, 0x00000002, 0x00050048, 0x0000000b,
   0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x0000000b, 0x00000001,
   0x0000000b, 0x00000001, 0x00050048, 0x0000000b, 0x00000002, 0x0000000b,
   0x00000003, 0x00050048, 0x0000000b, 0x00000003, 0x0000000b, 0x00000004,
   0x00040047, 0x00000011, 0x0000001e, 0x00000000, 0x00030047, 0x00000017,
   0x0000000e, 0x00040047, 0x00000017, 0x0000001e, 0x00000000, 0x00040047,
   0x00000019, 0x0000001e, 0x00000001, 0x00020013, 0x00000002, 0x00030021,
   0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017,
   0x00000007, 0x00000006, 0x00000004, 0x00040015, 0x00000008, 0x00000020,
   0x00000000, 0x0004002b, 0x00000008, 0x00000009, 0x00000001, 0x0004001c,
   0x0000000a, 0x00000006, 0x00000009, 0x0006001e, 0x0000000b, 0x00000007,
   0x00000006, 0x0000000a, 0x0000000a, 0x00040020, 0x0000000c, 0x00000003,
   0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000003, 0x00040015,
   0x0000000e, 0x00000020, 0x00000001, 0x0004002b, 0x0000000e, 0x0000000f,
   0x00000000, 0x00040020, 0x00000010, 0x00000001, 0x00000007, 0x0004003b,
   0x00000010, 0x00000011, 0x00000001, 0x00040020, 0x00000013, 0x00000003,
   0x00000007, 0x00040017, 0x00000015, 0x0000000e, 0x00000003, 0x00040020,
   0x00000016, 0x00000003, 0x00000015, 0x0004003b, 0x00000016, 0x00000017,
   0x00000003, 0x00040020, 0x00000018, 0x00000001, 0x00000015, 0x0004003b,
   0x00000018, 0x00000019, 0x00000001, 0x00050036, 0x00000002, 0x00000004,
   0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x00000007,
   0x00000012, 0x00000011, 0x00050041, 0x00000013, 0x00000014, 0x0000000d,
   0x0000000f, 0x0003003e, 0x00000014, 0x00000012, 0x0004003d, 0x00000015,
   0x0000001a, 0x00000019, 0x0003003e, 0x00000017, 0x0000001a, 0x000100fd,
   0x00010038,
};

static const uint32_t fs3_spirv[] = {
   0x07230203, 0x00010000, 0x000d000b, 0x00000016, 0x00000000, 0x00020011,
   0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
   0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000004,
   0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x0000000d, 0x00030010,
   0x00000004, 0x00000007, 0x00040047, 0x00000009, 0x0000001e, 0x00000000,
   0x00030047, 0x0000000d, 0x0000000e, 0x00040047, 0x0000000d, 0x0000001e,
   0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
   0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006,
   0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b,
   0x00000008, 0x00000009, 0x00000003, 0x00040015, 0x0000000a, 0x00000020,
   0x00000001, 0x00040017, 0x0000000b, 0x0000000a, 0x00000003, 0x00040020,
   0x0000000c, 0x00000001, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d,
   0x00000001, 0x00040017, 0x0000000f, 0x00000006, 0x00000003, 0x0004002b,
   0x00000006, 0x00000011, 0x3f800000, 0x00050036, 0x00000002, 0x00000004,
   0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000b,
   0x0000000e, 0x0000000d, 0x0004006f, 0x0000000f, 0x00000010, 0x0000000e,
   0x00050051, 0x00000006, 0x00000012, 0x00000010, 0x00000000, 0x00050051,
   0x00000006, 0x00000013, 0x00000010, 0x00000001, 0x00050051, 0x00000006,
   0x00000014, 0x00000010, 0x00000002, 0x00070050, 0x00000007, 0x00000015,
   0x00000012, 0x00000013, 0x00000014, 0x00000011, 0x0003003e, 0x00000009,
   0x00000015, 0x000100fd, 0x00010038,
};

// ⚠ ONLY SIGNED-INTEGER FORMATS. Vulkan requires the shader's base type to match the format's, and
// the vertex shader above declares ivec4. A UINT format here would be a validation error rather than
// a measurement, and the alignment behaviour under test is identical for both, so the list refuses
// instead of quietly widening.
static const struct {
  const char* name;
  VkFormat format;
  unsigned bytes;
  unsigned channels;
} kFormats[] = {
  {"r16g16b16a16_sint",  VK_FORMAT_R16G16B16A16_SINT,   8, 4},
  {"r16g16_sint",        VK_FORMAT_R16G16_SINT,         4, 2},
  {"r8g8b8a8_sint",      VK_FORMAT_R8G8B8A8_SINT,       4, 4},
  {"r32g32b32a32_sint",  VK_FORMAT_R32G32B32A32_SINT,  16, 4},
  {"r16g16b16_sint",     VK_FORMAT_R16G16B16_SINT,      6, 3},
  {"r8g8b8_sint",        VK_FORMAT_R8G8B8_SINT,         3, 3},
  {"r32g32b32_sint",     VK_FORMAT_R32G32B32_SINT,     12, 3},
};

static void die(const char* what, VkResult r) {
  fprintf(stderr, "vsfetchprobe: %s failed: VkResult %d\n", what, (int)r);
  exit(2);
  }

int main(int argc, char** argv) {
  const char* want = argc > 1 ? argv[1] : "r16g16b16a16_sint";
  const unsigned offset = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 36u;
  const unsigned stride = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 0) : 76u;
  // ⚠ WHICH BINDING THE FLOAT ATTRIBUTE SITS ON DECIDES THE ANSWER, and it took a wrong reading to
  // notice. RADV's only compile-time knowledge of a binding's alignment is the widest attribute
  // alignment among the attributes IN THAT BINDING whose offset is a multiple of it. Put the
  // R32G32B32A32_SFLOAT on the same binding and the binding is known to be 4-aligned; put it on its
  // own and the integer binding is known only to be 2-aligned. Those are different cases and they
  // compile differently, so the probe has to be told which one is meant.
  //
  //   same   one binding, interleaved float + integer   - Beetle PSX HW's layout
  //   split  a binding each                             - the CTS single_attribute layout
  const int split = argc > 4 && strcmp(argv[4], "split") == 0;

  VkFormat format = VK_FORMAT_UNDEFINED;
  unsigned bytes = 0;
  unsigned channels = 0;
  for (unsigned i = 0; i < sizeof(kFormats) / sizeof(kFormats[0]); ++i) {
    if (strcmp(kFormats[i].name, want) == 0) {
      format = kFormats[i].format;
      bytes = kFormats[i].bytes;
      channels = kFormats[i].channels;
      }
    }
  if (format == VK_FORMAT_UNDEFINED) {
    fprintf(stderr, "vsfetchprobe: unknown format '%s'. Known:", want);
    for (unsigned i = 0; i < sizeof(kFormats) / sizeof(kFormats[0]); ++i)
      fprintf(stderr, " %s", kFormats[i].name);
    fprintf(stderr, "\n");
    exit(2);
    }

  // ⚠ SAY WHAT IS BEING MEASURED, INCLUDING THE ANSWER THE ARITHMETIC ALREADY GIVES. The whole point
  // is a comparison between two runs, and a dump of ISA with no header is a dump nobody can pair up.
  printf("== vsfetchprobe: %s, %u channel(s), element %u B, at offset %u in stride %u\n",
         want, channels, bytes, offset, stride);
  printf("   offset %% element = %u, stride %% element = %u  ->  %s\n",
         offset % bytes, stride % bytes,
         (offset % bytes == 0 && stride % bytes == 0) ? "naturally aligned"
                                                      : "NOT naturally aligned - this is the case under test");
  printf("   ORBIS_VS_STRICT_ALIGN = %s\n", getenv("ORBIS_VS_STRICT_ALIGN") ? getenv("ORBIS_VS_STRICT_ALIGN") : "unset");
  printf("   layout: %s\n", split
         ? "SPLIT - the float attribute is on its own binding, so binding 0 is known only to the\n"
           "           integer format's channel alignment. This is the CTS single_attribute shape."
         : "SAME  - R32G32B32A32_SFLOAT shares binding 0 at offset 0, so the binding is known\n"
           "           4-aligned. This is Beetle PSX HW's interleaved shape.");
  fflush(stdout);

  const VkApplicationInfo app = {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "vsfetchprobe",
    .apiVersion = VK_API_VERSION_1_3,
  };
  const VkInstanceCreateInfo ici = {
    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pApplicationInfo = &app,
  };
  VkInstance instance;
  VkResult r = vkCreateInstance(&ici, NULL, &instance);
  if (r != VK_SUCCESS) die("vkCreateInstance", r);

  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, NULL);
  if (count == 0) {
    fprintf(stderr, "vsfetchprobe: no physical device. Is the drm-shim preloaded and VK_DRIVER_FILES set?\n");
    exit(2);
    }
  VkPhysicalDevice* devices = calloc(count, sizeof(*devices));
  vkEnumeratePhysicalDevices(instance, &count, devices);
  VkPhysicalDevice pdev = devices[0];

  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(pdev, &props);
  // ⚠ AND NAME THE DEVICE. A probe meant for gfx7 that silently picked the machine's own GPU would
  // print a perfectly good answer about the wrong part - which is this project's most expensive
  // recurring mistake, in its cheapest possible form.
  printf("   device: %s\n\n", props.deviceName);
  fflush(stdout);

  const float priority = 1.0f;
  const VkDeviceQueueCreateInfo qci = {
    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueFamilyIndex = 0,
    .queueCount = 1,
    .pQueuePriorities = &priority,
  };
  VkPhysicalDeviceVulkan13Features vk13 = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    .dynamicRendering = VK_TRUE,
  };
  const VkDeviceCreateInfo dci = {
    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .pNext = &vk13,
    .queueCreateInfoCount = 1,
    .pQueueCreateInfos = &qci,
  };
  VkDevice device;
  r = vkCreateDevice(pdev, &dci, NULL, &device);
  if (r != VK_SUCCESS) die("vkCreateDevice", r);

  // ⚠ THE SHADER'S VECTOR WIDTH HAS TO MATCH THE FORMAT'S CHANNEL COUNT, or the case under test is
  // not the one named: a three-channel format read by an ivec4 is the "missing components" path, which
  // the console's own run showed passing while the full read fails.
  const int three = channels == 3;
  VkShaderModule vs, fs;
  const VkShaderModuleCreateInfo vsci = {
    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize = three ? sizeof(vs3_spirv) : sizeof(vs_spirv),
    .pCode    = three ? vs3_spirv : vs_spirv,
  };
  const VkShaderModuleCreateInfo fsci = {
    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize = three ? sizeof(fs3_spirv) : sizeof(fs_spirv),
    .pCode    = three ? fs3_spirv : fs_spirv,
  };
  if ((r = vkCreateShaderModule(device, &vsci, NULL, &vs)) != VK_SUCCESS) die("vkCreateShaderModule(vertex)", r);
  if ((r = vkCreateShaderModule(device, &fsci, NULL, &fs)) != VK_SUCCESS) die("vkCreateShaderModule(fragment)", r);

  VkPipelineLayout layout;
  const VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  if ((r = vkCreatePipelineLayout(device, &plci, NULL, &layout)) != VK_SUCCESS) die("vkCreatePipelineLayout", r);

  const VkVertexInputBindingDescription bindings[] = {
    {.binding = 0, .stride = stride, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX},
    {.binding = 1, .stride = 16,     .inputRate = VK_VERTEX_INPUT_RATE_VERTEX},
  };
  const VkVertexInputAttributeDescription attributes[] = {
    {.location = 0, .binding = split ? 1u : 0u, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0},
    {.location = 1, .binding = 0, .format = format, .offset = offset},
  };
  const VkPipelineVertexInputStateCreateInfo vi = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    .vertexBindingDescriptionCount = split ? 2u : 1u, .pVertexBindingDescriptions = bindings,
    .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = attributes,
  };
  const VkPipelineInputAssemblyStateCreateInfo ia = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  const VkPipelineViewportStateCreateInfo vp = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    .viewportCount = 1, .scissorCount = 1,
  };
  const VkPipelineRasterizationStateCreateInfo rs = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
    .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
    .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
  };
  const VkPipelineMultisampleStateCreateInfo ms = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  const VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
  const VkPipelineColorBlendStateCreateInfo cb = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
    .attachmentCount = 1, .pAttachments = &cba,
  };
  const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  const VkPipelineDynamicStateCreateInfo ds = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
    .dynamicStateCount = 2, .pDynamicStates = dynamic,
  };
  const VkFormat colour_format = VK_FORMAT_R8G8B8A8_UNORM;
  const VkPipelineRenderingCreateInfo rendering = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
    .colorAttachmentCount = 1, .pColorAttachmentFormats = &colour_format,
  };
  const VkPipelineShaderStageCreateInfo stages[] = {
    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
     .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
     .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"},
  };
  const VkGraphicsPipelineCreateInfo gpci = {
    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
    .pNext = &rendering,
    .stageCount = 2, .pStages = stages,
    .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
    .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb,
    .pDynamicState = &ds, .layout = layout,
  };

  VkPipeline pipeline;
  r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, NULL, &pipeline);
  if (r != VK_SUCCESS) die("vkCreateGraphicsPipelines", r);

  printf("\n== pipeline created. The vertex shader's ISA is above, from RADV_DEBUG=asm.\n");
  printf("   Count the tbuffer_load_format_* instructions: one xyzw means the fetch was taken whole,\n"
         "   two xy (or four x) mean it was split to reach natural alignment.\n");

  vkDestroyPipeline(device, pipeline, NULL);
  vkDestroyPipelineLayout(device, layout, NULL);
  vkDestroyShaderModule(device, fs, NULL);
  vkDestroyShaderModule(device, vs, NULL);
  vkDestroyDevice(device, NULL);
  vkDestroyInstance(instance, NULL);
  free(devices);
  return 0;
  }

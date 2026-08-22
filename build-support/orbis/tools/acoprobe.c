// acoprobe - create one Vulkan compute pipeline from a .sprv file and exit.
//
// Its whole purpose is to make RADV compile a shader we care about for a GPU that is not in this
// machine. Under `LD_PRELOAD=libamdgpu_noop_drm_shim.so AMDGPU_GPU_ID=bonaire` the driver believes it
// is talking to a gfx7 (CIK) part, and `RADV_DEBUG=asm` makes it print the ISA it produced.
//
// Nothing is executed: the shim stubs the submit ioctl. The only thing under test is the COMPILER.
#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ⚠ EVERY CHECK HERE EXISTS BECAUSE THIS FUNCTION USED TO SEGFAULT INSTEAD OF SAYING WHAT WAS WRONG.
//
// It read a length with no bound, malloc'd it without checking, and handed whatever came back to
// vkCreateShaderModule. A file that is not SPIR-V - the wrong path, a .spv that failed to generate and is
// zero bytes, a text file - therefore crashed inside the driver's SPIR-V parser rather than being refused
// here. That is the worst possible failure for a tool whose entire job is to say what the COMPILER did with
// a module: the crash looks like a compiler bug.
//
// Vulkan requires pCode to be a multiple of 4 bytes beginning with the SPIR-V magic word, and a driver is
// entitled to assume both. Checking them costs eight lines and turns a backtrace into a sentence.
static void* readFile(const char* path, size_t* size) {
  FILE* f = fopen(path,"rb");
  if(f==NULL) {
    fprintf(stderr,"acoprobe: cannot open %s\n",path);
    exit(2);
    }
  if(fseek(f,0,SEEK_END)!=0) {
    fprintf(stderr,"acoprobe: cannot seek %s - not a regular file?\n",path);
    exit(2);
    }
  long n = ftell(f);
  if(n<=0) {
    fprintf(stderr,"acoprobe: %s is %ld bytes - nothing to compile\n",path,n);
    exit(2);
    }
  if((n%4)!=0) {
    fprintf(stderr,"acoprobe: %s is %ld bytes, not a multiple of 4 - SPIR-V is a stream of 32-bit words, "
                   "so this is not one\n",path,n);
    exit(2);
    }
  rewind(f);
  void* p = malloc((size_t)n);
  if(p==NULL) {
    fprintf(stderr,"acoprobe: out of memory reading %ld bytes from %s\n",n,path);
    exit(2);
    }
  if(fread(p,1,(size_t)n,f)!=(size_t)n) {
    fprintf(stderr,"acoprobe: short read on %s\n",path);
    exit(2);
    }
  fclose(f);

  const uint32_t magic = *(const uint32_t*)p;
  if(magic!=0x07230203u) {
    // 0x03022307 is the same word byte-swapped: a valid module written by a tool of the other endianness,
    // which this program does not translate. Named separately because it is a different mistake.
    fprintf(stderr,"acoprobe: %s starts with 0x%08x, not the SPIR-V magic 0x07230203%s\n",
            path,magic,magic==0x03022307u ? " (byte-swapped - this module is big-endian)" : "");
    exit(2);
    }

  *size = (size_t)n;
  return p;
  }

int main(int argc, char** argv) {
  if(argc<2) {
    fprintf(stderr,"usage: acoprobe <module.sprv> [entrypoint]\n");
    return 2;
    }
  const char* path  = argv[1];
  const char* entry = (argc>2) ? argv[2] : "main";

  VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
  app.pApplicationName = "acoprobe";
  // 1.0 on purpose: the point is to compile, and asking for more would let a version check refuse
  // before a single shader is seen.
  app.apiVersion = VK_API_VERSION_1_0;

  VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
  ici.pApplicationInfo = &app;

  VkInstance inst = VK_NULL_HANDLE;
  VkResult   rc   = vkCreateInstance(&ici,NULL,&inst);
  if(rc!=VK_SUCCESS) {
    fprintf(stderr,"acoprobe: vkCreateInstance = %d\n",rc);
    return 3;
    }

  uint32_t ndev = 0;
  vkEnumeratePhysicalDevices(inst,&ndev,NULL);
  if(ndev==0) {
    fprintf(stderr,"acoprobe: no physical device - is the shim preloaded and the ICD selected?\n");
    return 3;
    }
  VkPhysicalDevice* devs = calloc(ndev,sizeof(*devs));
  vkEnumeratePhysicalDevices(inst,&ndev,devs);

  VkPhysicalDeviceProperties props = {};
  vkGetPhysicalDeviceProperties(devs[0],&props);
  // The line that proves WHICH device the compiler targeted. Without it a run that silently fell back
  // to another GPU would look like a successful gfx7 compile.
  printf("acoprobe: device \"%s\" apiVersion %u.%u.%u driverVersion 0x%x\n",
         props.deviceName,
         VK_VERSION_MAJOR(props.apiVersion),VK_VERSION_MINOR(props.apiVersion),
         VK_VERSION_PATCH(props.apiVersion),props.driverVersion);

  uint32_t nq = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(devs[0],&nq,NULL);
  VkQueueFamilyProperties* qf = calloc(nq,sizeof(*qf));
  vkGetPhysicalDeviceQueueFamilyProperties(devs[0],&nq,qf);
  uint32_t qidx = UINT32_MAX;
  for(uint32_t i=0; i<nq; ++i)
    if((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)!=0) {
      qidx = i;
      break;
      }
  if(qidx==UINT32_MAX) {
    fprintf(stderr,"acoprobe: no compute queue\n");
    return 3;
    }

  const float prio = 1.f;
  VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
  qci.queueFamilyIndex = qidx;
  qci.queueCount       = 1;
  qci.pQueuePriorities = &prio;

  VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos    = &qci;

  VkDevice dev = VK_NULL_HANDLE;
  rc = vkCreateDevice(devs[0],&dci,NULL,&dev);
  if(rc!=VK_SUCCESS) {
    fprintf(stderr,"acoprobe: vkCreateDevice = %d\n",rc);
    return 3;
    }

  size_t   codeSize = 0;
  void*    code     = readFile(path,&codeSize);

  VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
  smci.codeSize = codeSize;
  smci.pCode    = (const uint32_t*)code;

  VkShaderModule mod = VK_NULL_HANDLE;
  rc = vkCreateShaderModule(dev,&smci,NULL,&mod);
  if(rc!=VK_SUCCESS) {
    fprintf(stderr,"acoprobe: vkCreateShaderModule = %d\n",rc);
    return 4;
    }

  // A layout wide enough for anything: 32 bindings of each of the kinds these shaders use, and one
  // push range of the maximum this hardware guarantees. Building the real layout per module would mean
  // reflecting the SPIR-V, and the compiler does not need us to be right about it - only to be
  // permissive enough that pipeline creation is not refused before ACO runs.
  VkDescriptorSetLayoutBinding binds[96];
  uint32_t nb = 0;
  static const VkDescriptorType kinds[] = {
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    };
  for(uint32_t i=0; i<32; ++i) {
    memset(&binds[nb],0,sizeof(binds[nb]));
    binds[nb].binding         = i;
    binds[nb].descriptorType  = kinds[i%4];
    binds[nb].descriptorCount = 1;
    binds[nb].stageFlags      = VK_SHADER_STAGE_ALL;
    ++nb;
    }

  VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
  dslci.bindingCount = nb;
  dslci.pBindings    = binds;

  VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
  rc = vkCreateDescriptorSetLayout(dev,&dslci,NULL,&dsl);
  if(rc!=VK_SUCCESS) {
    fprintf(stderr,"acoprobe: vkCreateDescriptorSetLayout = %d\n",rc);
    return 4;
    }

  VkPushConstantRange push = { VK_SHADER_STAGE_ALL, 0, 128 };
  VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
  plci.setLayoutCount         = 1;
  plci.pSetLayouts            = &dsl;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges    = &push;

  VkPipelineLayout lay = VK_NULL_HANDLE;
  rc = vkCreatePipelineLayout(dev,&plci,NULL,&lay);
  if(rc!=VK_SUCCESS) {
    fprintf(stderr,"acoprobe: vkCreatePipelineLayout = %d\n",rc);
    return 4;
    }

  VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
  cpci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  cpci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
  cpci.stage.module = mod;
  cpci.stage.pName  = entry;
  cpci.layout       = lay;

  VkPipeline pipe = VK_NULL_HANDLE;
  rc = vkCreateComputePipelines(dev,VK_NULL_HANDLE,1,&cpci,NULL,&pipe);
  // THE RESULT LINE. A refusal is a fact and not a failure of the experiment: it says the module needs
  // something this permissive layout did not offer, which is information about the module.
  printf("acoprobe: vkCreateComputePipelines(%s) = %d%s\n",
         path,rc,rc==VK_SUCCESS ? "  COMPILED" : "  REFUSED");
  return rc==VK_SUCCESS ? 0 : 5;
  }

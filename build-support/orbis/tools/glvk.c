/* glvk - OpenGL and Vulkan alive in one process, which is what RetroArch actually needs.
 *
 * SPDX-License-Identifier: MIT
 *
 * WHY THIS EXISTS. glrun proved the frame reaches the screen and gltri proved shaders compile and
 * rasterise. Both used GL alone. RetroArch does not: it is ONE eboot with a video driver chosen at
 * runtime, and its libretro cores are split between Vulkan ones and GL ones. So the question that
 * decides whether any of this is usable there is not "does GL work" but "does GL work while Vulkan
 * is up, and does Vulkan survive GL being torn down".
 *
 * That is not a formality on this port. The link-time half is already answered - libvulkan_radeon.a
 * exports no bare vk* symbols, only radv_* and vk_icdGetInstanceProcAddr, so vkloader's thunks and
 * zink's direct resolution cannot collide even under --whole-archive. The RUNTIME half is open: this
 * winsys has global state that a second device has never been asked to share. ac_orbis_drm.c keeps a
 * process-wide submit lock, one fence label, one VA allocator, one syncobj table and a retire ring;
 * every one of those was written and measured with a single device in the process.
 *
 * ⚠ EVERY VULKAN CALL HERE GOES THROUGH vk_icdGetInstanceProcAddr BY HAND, and no vkloader thunks are
 * linked. That is deliberate: it is the smallest possible statement of the contract a consumer needs,
 * and it demonstrates the pattern without dragging in a generated file. A title that wants the plain
 * vkCreateInstance spelling links vkloader; nothing here has to.
 *
 * ⚠ THE VULKAN SIDE PROVES EXECUTION, NOT JUST CREATION. vkCmdFillBuffer into a HOST_VISIBLE buffer,
 * fence, read back. A device that was created and never ran anything would pass a "did it come up"
 * test and tell us nothing - build-hostorbis passes exactly that test and executes nothing at all.
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <vulkan/vulkan.h>
#include <ps4_app.h>
#include <stdint.h>
#include <string.h>

#define W 1920
#define H 1080
#define FILL_WORDS 256

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);

/* The handful of entry points this needs, resolved once. */
static struct {
   VkInstance instance;
   VkPhysicalDevice pdev;
   VkDevice device;
   VkQueue queue;
   uint32_t qfam;
   VkCommandPool pool;
   VkCommandBuffer cmd;
   VkBuffer buf;
   VkDeviceMemory mem;
   void *map;
   VkFence fence;

   PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
   PFN_vkDestroyInstance DestroyInstance;
   PFN_vkDestroyDevice DestroyDevice;
   PFN_vkResetCommandBuffer ResetCommandBuffer;
   PFN_vkBeginCommandBuffer BeginCommandBuffer;
   PFN_vkEndCommandBuffer EndCommandBuffer;
   PFN_vkCmdFillBuffer CmdFillBuffer;
   PFN_vkQueueSubmit QueueSubmit;
   PFN_vkWaitForFences WaitForFences;
   PFN_vkResetFences ResetFences;
   PFN_vkDestroyFence DestroyFence;
   PFN_vkDestroyBuffer DestroyBuffer;
   PFN_vkFreeMemory FreeMemory;
   PFN_vkUnmapMemory UnmapMemory;
   PFN_vkDestroyCommandPool DestroyCommandPool;
} vk;

#define IPA(name) ((PFN_vk##name)vk_icdGetInstanceProcAddr(vk.instance, "vk" #name))
#define DPA(name) vk.name = (PFN_vk##name)vk.GetDeviceProcAddr(vk.device, "vk" #name)

static int
vk_bringup(void)
{
   PFN_vkCreateInstance CreateInstance =
      (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
   if (CreateInstance == NULL) {
      ps4_log("VK: the ICD did not answer for vkCreateInstance");
      return 0;
   }

   /* apiVersion 1.1 rather than 1.0: the physical-device dispatch table is built from it, and a 1.0
    * instance leaves vkGetPhysicalDeviceProperties2 out of that table entirely. That is not a
    * hypothetical - it is the shape of the NULL-dispatch crash this port hit on its first GL run,
    * from the other direction. */
   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "glvk",
      .apiVersion = VK_API_VERSION_1_1,
   };
   const VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkResult r = CreateInstance(&ici, NULL, &vk.instance);
   if (r != VK_SUCCESS) { ps4_log("VK: vkCreateInstance -> %d", (int)r); return 0; }

   PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = IPA(EnumeratePhysicalDevices);
   uint32_t n = 1;
   r = EnumeratePhysicalDevices(vk.instance, &n, &vk.pdev);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n < 1) {
      ps4_log("VK: no physical device (%d, n=%u)", (int)r, n);
      return 0;
   }

   PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = IPA(GetPhysicalDeviceProperties);
   VkPhysicalDeviceProperties props;
   GetPhysicalDeviceProperties(vk.pdev, &props);
   ps4_log("VK: device = %s, API %u.%u.%u", props.deviceName,
           VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
           VK_VERSION_PATCH(props.apiVersion));

   PFN_vkGetPhysicalDeviceQueueFamilyProperties GetQFP = IPA(GetPhysicalDeviceQueueFamilyProperties);
   uint32_t nq = 0;
   GetQFP(vk.pdev, &nq, NULL);
   VkQueueFamilyProperties qfp[8];
   if (nq > 8) nq = 8;
   GetQFP(vk.pdev, &nq, qfp);
   vk.qfam = UINT32_MAX;
   for (uint32_t i = 0; i < nq; i++)
      if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { vk.qfam = i; break; }
   if (vk.qfam == UINT32_MAX) { ps4_log("VK: no graphics queue family"); return 0; }

   const float prio = 1.0f;
   const VkDeviceQueueCreateInfo dqci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = vk.qfam, .queueCount = 1, .pQueuePriorities = &prio,
   };
   const VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &dqci,
   };
   PFN_vkCreateDevice CreateDevice = IPA(CreateDevice);
   r = CreateDevice(vk.pdev, &dci, NULL, &vk.device);
   if (r != VK_SUCCESS) { ps4_log("VK: vkCreateDevice -> %d", (int)r); return 0; }

   vk.GetDeviceProcAddr = IPA(GetDeviceProcAddr);
   vk.DestroyInstance = IPA(DestroyInstance);
   DPA(DestroyDevice); DPA(ResetCommandBuffer); DPA(BeginCommandBuffer); DPA(EndCommandBuffer);
   DPA(CmdFillBuffer); DPA(QueueSubmit); DPA(WaitForFences); DPA(ResetFences);
   DPA(DestroyFence); DPA(DestroyBuffer); DPA(FreeMemory); DPA(UnmapMemory);
   DPA(DestroyCommandPool);

   PFN_vkGetDeviceQueue GetDeviceQueue = (PFN_vkGetDeviceQueue)vk.GetDeviceProcAddr(vk.device, "vkGetDeviceQueue");
   GetDeviceQueue(vk.device, vk.qfam, 0, &vk.queue);

   const VkCommandPoolCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = vk.qfam,
   };
   PFN_vkCreateCommandPool CreateCommandPool = (PFN_vkCreateCommandPool)vk.GetDeviceProcAddr(vk.device, "vkCreateCommandPool");
   if (CreateCommandPool(vk.device, &cpci, NULL, &vk.pool) != VK_SUCCESS) return 0;

   const VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = vk.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
   };
   PFN_vkAllocateCommandBuffers AllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)vk.GetDeviceProcAddr(vk.device, "vkAllocateCommandBuffers");
   if (AllocateCommandBuffers(vk.device, &cbai, &vk.cmd) != VK_SUCCESS) return 0;

   const VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = FILL_WORDS * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   PFN_vkCreateBuffer CreateBuffer = (PFN_vkCreateBuffer)vk.GetDeviceProcAddr(vk.device, "vkCreateBuffer");
   if (CreateBuffer(vk.device, &bci, NULL, &vk.buf) != VK_SUCCESS) return 0;

   PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = (PFN_vkGetBufferMemoryRequirements)vk.GetDeviceProcAddr(vk.device, "vkGetBufferMemoryRequirements");
   VkMemoryRequirements mr;
   GetBufferMemoryRequirements(vk.device, vk.buf, &mr);

   PFN_vkGetPhysicalDeviceMemoryProperties GetPDMP = IPA(GetPhysicalDeviceMemoryProperties);
   VkPhysicalDeviceMemoryProperties mp;
   GetPDMP(vk.pdev, &mp);
   uint32_t type = UINT32_MAX;
   const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) { type = i; break; }
   if (type == UINT32_MAX) { ps4_log("VK: no host-visible coherent memory type"); return 0; }

   const VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mr.size, .memoryTypeIndex = type,
   };
   PFN_vkAllocateMemory AllocateMemory = (PFN_vkAllocateMemory)vk.GetDeviceProcAddr(vk.device, "vkAllocateMemory");
   if (AllocateMemory(vk.device, &mai, NULL, &vk.mem) != VK_SUCCESS) return 0;
   PFN_vkBindBufferMemory BindBufferMemory = (PFN_vkBindBufferMemory)vk.GetDeviceProcAddr(vk.device, "vkBindBufferMemory");
   BindBufferMemory(vk.device, vk.buf, vk.mem, 0);
   PFN_vkMapMemory MapMemory = (PFN_vkMapMemory)vk.GetDeviceProcAddr(vk.device, "vkMapMemory");
   if (MapMemory(vk.device, vk.mem, 0, VK_WHOLE_SIZE, 0, &vk.map) != VK_SUCCESS) return 0;

   const VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   PFN_vkCreateFence CreateFence = (PFN_vkCreateFence)vk.GetDeviceProcAddr(vk.device, "vkCreateFence");
   if (CreateFence(vk.device, &fci, NULL, &vk.fence) != VK_SUCCESS) return 0;

   ps4_log("VK: instance, device, queue, command pool and a mapped buffer are up");
   return 1;
}

/* One fill-and-verify round. `tag` says where in the sequence we are, so a failure names the moment
 * rather than the operation. */
static int
vk_execute(const char *tag, uint32_t pattern)
{
   volatile uint32_t *words = (volatile uint32_t *)vk.map;
   for (int i = 0; i < FILL_WORDS; i++)
      words[i] = 0xAAAAAAAAu;

   vk.ResetCommandBuffer(vk.cmd, 0);
   const VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   vk.BeginCommandBuffer(vk.cmd, &bi);
   vk.CmdFillBuffer(vk.cmd, vk.buf, 0, FILL_WORDS * 4, pattern);
   vk.EndCommandBuffer(vk.cmd);

   vk.ResetFences(vk.device, 1, &vk.fence);
   const VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &vk.cmd,
   };
   VkResult r = vk.QueueSubmit(vk.queue, 1, &si, vk.fence);
   if (r != VK_SUCCESS) { ps4_log("VK %s: vkQueueSubmit -> %d", tag, (int)r); return 0; }

   r = vk.WaitForFences(vk.device, 1, &vk.fence, VK_TRUE, 5ull * 1000 * 1000 * 1000);
   if (r != VK_SUCCESS) { ps4_log("VK %s: vkWaitForFences -> %d", tag, (int)r); return 0; }

   int bad = 0;
   for (int i = 0; i < FILL_WORDS; i++)
      if (words[i] != pattern) bad++;

   ps4_log("VK %s: fill 0x%08x -> word[0]=0x%08x, %d/%d words wrong  %s",
           tag, pattern, words[0], bad, FILL_WORDS, bad == 0 ? "OK" : "MISMATCH");
   return bad == 0;
}

static void
vk_teardown(void)
{
   if (vk.device == VK_NULL_HANDLE) return;
   vk.DestroyFence(vk.device, vk.fence, NULL);
   vk.UnmapMemory(vk.device, vk.mem);
   vk.DestroyBuffer(vk.device, vk.buf, NULL);
   vk.FreeMemory(vk.device, vk.mem, NULL);
   vk.DestroyCommandPool(vk.device, vk.pool, NULL);
   vk.DestroyDevice(vk.device, NULL);
   vk.DestroyInstance(vk.instance, NULL);
   ps4_log("VK: torn down");
}

/* ------------------------------------------------------------------ the GL half */

static const float verts[] = {
   -0.8f, -0.8f,  0.8f, -0.8f,  0.0f, 0.8f,
};
static const char *vs_src =
   "#version 100\n"
   "attribute vec2 a_pos;\n"
   "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
static const char *fs_src =
   "#version 100\n"
   "precision mediump float;\n"
   "uniform vec3 u_col;\n"
   "void main() { gl_FragColor = vec4(u_col, 1.0); }\n";

static EGLDisplay dpy = EGL_NO_DISPLAY;
static EGLSurface surf = EGL_NO_SURFACE;
static EGLContext ctx = EGL_NO_CONTEXT;
static GLuint prog, vbo;
static GLint u_col;

static GLuint
compile(GLenum type, const char *src, const char *what)
{
   GLuint sh = glCreateShader(type);
   glShaderSource(sh, 1, &src, NULL);
   glCompileShader(sh);
   GLint ok = 0;
   glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
   if (!ok) ps4_log("GL: %s failed to compile", what);
   return ok ? sh : 0;
}

static int
gl_bringup(void)
{
   dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   EGLint mj = 0, mn = 0;
   if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &mj, &mn)) { ps4_log("GL: no EGL"); return 0; }

   static const EGLint cfg_attrs[] = {
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE
   };
   EGLConfig cfg = NULL;
   EGLint n = 0;
   if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n) || n < 1) { ps4_log("GL: no config"); return 0; }

   surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)1, NULL);
   eglBindAPI(EGL_OPENGL_ES_API);
   static const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
   if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surf, surf, ctx)) {
      ps4_log("GL: no context"); return 0;
   }
   ps4_log("GL: %s", (const char *)glGetString(GL_RENDERER));
   ps4_log("GL: %s", (const char *)glGetString(GL_VERSION));

   GLuint vs = compile(GL_VERTEX_SHADER, vs_src, "vertex shader");
   GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src, "fragment shader");
   if (vs == 0 || fs == 0) return 0;
   prog = glCreateProgram();
   glAttachShader(prog, vs);
   glAttachShader(prog, fs);
   glBindAttribLocation(prog, 0, "a_pos");
   glLinkProgram(prog);
   GLint linked = 0;
   glGetProgramiv(prog, GL_LINK_STATUS, &linked);
   if (!linked) { ps4_log("GL: link failed"); return 0; }
   u_col = glGetUniformLocation(prog, "u_col");

   glGenBuffers(1, &vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *)0);
   glEnableVertexAttribArray(0);
   glViewport(0, 0, W, H);
   glUseProgram(prog);
   ps4_log("GL: context, shaders and vertex buffer are up");
   return 1;
}

static int
gl_frame(const uint8_t *col)
{
   glClearColor(0.06f, 0.06f, 0.09f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glUniform3f(u_col, col[0] / 255.0f, col[1] / 255.0f, col[2] / 255.0f);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   glFinish();

   uint8_t got[4] = { 0, 0, 0, 0 };
   glReadPixels(W / 2, (int)(0.3665f * H), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
   const int ok = got[0] == col[0] && got[1] == col[1] && got[2] == col[2];
   if (!ok)
      ps4_log("GL: centroid got %3u %3u %3u want %3u %3u %3u  MISMATCH",
              got[0], got[1], got[2], col[0], col[1], col[2]);
   eglSwapBuffers(dpy, surf);
   return ok;
}

static void
gl_teardown(void)
{
   if (dpy == EGL_NO_DISPLAY) return;
   glDeleteBuffers(1, &vbo);
   glDeleteProgram(prog);
   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(dpy, ctx);
   eglDestroySurface(dpy, surf);
   eglTerminate(dpy);
   dpy = EGL_NO_DISPLAY;
   ps4_log("GL: torn down");
}

int
main(void)
{
   ps4_app_init("glvk", "gl-and-vulkan");
   ps4_log("start - Vulkan and OpenGL in one process, the RetroArch shape");

   int step_vk_alone = 0, step_gl_up = 0, step_gl_frames = 0, step_vk_with_gl = 0, step_vk_after_gl = 0;

   /* 1. Vulkan on its own. If this fails nothing after it means anything. */
   if (!vk_bringup()) { ps4_log_fatal("VERDICT: FAIL - Vulkan did not come up"); ps4_idle_forever("no vk"); return 1; }
   step_vk_alone = vk_execute("alone", 0xDEADBEEFu);

   /* 2. GL up while the Vulkan device is still alive. This is the step that asks whether the winsys
    *    tolerates a second device in the process at all. */
   step_gl_up = gl_bringup();
   if (!step_gl_up) { ps4_log_fatal("VERDICT: FAIL - GL did not come up beside Vulkan"); ps4_idle_forever("no gl"); return 1; }

   /* 3. Frames, with a Vulkan submit interleaved between them - the two devices taking turns on the
    *    one submit lock, which is the arrangement RetroArch produces when a GL core runs while a
    *    Vulkan context is still allocated. */
   static const uint8_t cols[][3] = {
      { 240, 160,  32 }, {  32, 200, 240 }, { 200,  40, 160 },
      {  80, 240,  80 }, { 240, 240,  64 }, { 120, 120, 255 },
   };
   int frames_ok = 0, interleave_ok = 0;
   const int frames = 24;
   for (int i = 0; i < frames; i++) {
      frames_ok += gl_frame(cols[i % 6]);
      if (i % 6 == 5)
         interleave_ok += vk_execute("interleaved", 0xC0FFEE00u + (uint32_t)i);
   }
   step_gl_frames = (frames_ok == frames);
   step_vk_with_gl = (interleave_ok == frames / 6);
   ps4_log("GL frames: %d/%d correct; Vulkan submits between them: %d/%d correct",
           frames_ok, frames, interleave_ok, frames / 6);

   /* 4. GL down, Vulkan still expected to work. RetroArch unloads a core and loads another. */
   gl_teardown();
   step_vk_after_gl = vk_execute("after GL teardown", 0x5A5A5A5Au);

   vk_teardown();

   const int all = step_vk_alone && step_gl_up && step_gl_frames && step_vk_with_gl && step_vk_after_gl;
   ps4_log("steps: vk-alone=%d gl-up=%d gl-frames=%d vk-during-gl=%d vk-after-gl=%d",
           step_vk_alone, step_gl_up, step_gl_frames, step_vk_with_gl, step_vk_after_gl);
   ps4_log("VERDICT: %s", all ? "OK - Vulkan and OpenGL coexist in one process"
                              : "FAIL - see the step flags above");

   ps4_idle_forever("glvk done");
   return 0;
}

/* presentprobe - drive one swapchain present, on the host, through the same code the console dies in.
 *
 * WHY THIS EXISTS. OpenGothic's first vkQueuePresentKHR takes the process down with SIGSEGV on the console.
 * The backtrace names the path exactly:
 *
 *     wsi_common_queue_present -> wsi_queue_submit2_unordered -> vk_device_copy_semaphore_payloads
 *       -> __vk_sync_wait_many -> vk_sync_type::wait_many        <- fault, read at 0x9
 *
 * Every one of those frames is generic Mesa reached through OUR arm, and build.sh --host-orbis builds that
 * arm as an ordinary Linux ICD. So the whole path is reachable on a laptop, where a fault is a backtrace
 * rather than a ten-minute flash - which is the entire argument for keeping that loop working.
 *
 * WHAT IT TAKES TO REACH IT. wsi_queue_submit2_unordered is only used for a present's WAIT semaphores, and
 * only when there are no command buffers in the submission. So the probe needs a real swapchain, a real
 * acquire, and a present that waits on a semaphore some earlier submission signalled - which is what an
 * engine does every frame and what nothing else in this port's tools has ever done.
 *
 *     VK_DRIVER_FILES=<build-hostorbis icd.json> presentprobe
 */
#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(what, expr)                                                                                     \
   do {                                                                                                       \
      const VkResult _r = (expr);                                                                             \
      printf("presentprobe: %-34s -> %d%s\n", (what), _r, _r == VK_SUCCESS ? "" : "  FAILED");                \
      fflush(stdout);                                                                                         \
      if (_r != VK_SUCCESS)                                                                                   \
         return 1;                                                                                            \
   } while (0)

int
main(void)
{
   const char *inst_exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .pApplicationName = "presentprobe",
                            .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pApplicationInfo = &app,
                               .enabledExtensionCount = 2,
                               .ppEnabledExtensionNames = inst_exts};
   VkInstance inst = VK_NULL_HANDLE;
   CHECK("vkCreateInstance", vkCreateInstance(&ici, NULL, &inst));

   uint32_t ndev = 0;
   vkEnumeratePhysicalDevices(inst, &ndev, NULL);
   if (ndev == 0) {
      printf("presentprobe: no physical device - is VK_DRIVER_FILES pointing at the orbis ICD?\n");
      return 1;
   }
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   ndev = 1;
   vkEnumeratePhysicalDevices(inst, &ndev, &pdev);

   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pdev, &props);
   printf("presentprobe: device \"%s\"\n", props.deviceName);

   uint32_t nq = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nq, NULL);
   VkQueueFamilyProperties *qf = calloc(nq, sizeof(*qf));
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nq, qf);
   uint32_t qidx = UINT32_MAX;
   for (uint32_t i = 0; i < nq; i++)
      if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
         qidx = i;
         break;
      }
   if (qidx == UINT32_MAX) {
      printf("presentprobe: no graphics queue\n");
      return 1;
   }

   const float prio = 1.f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = qidx,
                                  .queueCount = 1,
                                  .pQueuePriorities = &prio};
   const char *dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qci,
                             .enabledExtensionCount = 1,
                             .ppEnabledExtensionNames = dev_exts};
   VkDevice dev = VK_NULL_HANDLE;
   CHECK("vkCreateDevice", vkCreateDevice(pdev, &dci, NULL, &dev));

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(dev, qidx, 0, &queue);

   PFN_vkCreateHeadlessSurfaceEXT create_surface =
      (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(inst, "vkCreateHeadlessSurfaceEXT");
   if (create_surface == NULL) {
      printf("presentprobe: vkCreateHeadlessSurfaceEXT is not exposed\n");
      return 1;
   }
   VkHeadlessSurfaceCreateInfoEXT sci = {.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
   VkSurfaceKHR surface = VK_NULL_HANDLE;
   CHECK("vkCreateHeadlessSurfaceEXT", create_surface(inst, &sci, NULL, &surface));

   VkBool32 supported = VK_FALSE;
   vkGetPhysicalDeviceSurfaceSupportKHR(pdev, qidx, surface, &supported);
   printf("presentprobe: queue family %u presents: %s\n", qidx, supported ? "yes" : "NO");

   uint32_t nfmt = 0;
   vkGetPhysicalDeviceSurfaceFormatsKHR(pdev, surface, &nfmt, NULL);
   VkSurfaceFormatKHR *fmts = calloc(nfmt ? nfmt : 1, sizeof(*fmts));
   vkGetPhysicalDeviceSurfaceFormatsKHR(pdev, surface, &nfmt, fmts);
   printf("presentprobe: %u surface format(s), taking [0] = %d / colourspace %d\n", nfmt, fmts[0].format,
          fmts[0].colorSpace);

   VkSurfaceCapabilitiesKHR caps;
   vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pdev, surface, &caps);
   const uint32_t want_images = caps.minImageCount < 2 ? 2 : caps.minImageCount;

   VkSwapchainCreateInfoKHR swci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                    .surface = surface,
                                    .minImageCount = want_images,
                                    .imageFormat = fmts[0].format,
                                    .imageColorSpace = fmts[0].colorSpace,
                                    .imageExtent = {1920, 1080},
                                    .imageArrayLayers = 1,
                                    .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                    .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                    .preTransform = caps.currentTransform,
                                    .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                    .presentMode = VK_PRESENT_MODE_FIFO_KHR,
                                    .clipped = VK_TRUE};
   VkSwapchainKHR swapchain = VK_NULL_HANDLE;
   CHECK("vkCreateSwapchainKHR", vkCreateSwapchainKHR(dev, &swci, NULL, &swapchain));

   uint32_t nimg = 0;
   vkGetSwapchainImagesKHR(dev, swapchain, &nimg, NULL);
   printf("presentprobe: swapchain has %u image(s)\n", nimg);

   /* ⚠ TWO SEMAPHORES, AND THE SECOND ONE IS THE WHOLE POINT. The present waits on `rendered`, which an
    * earlier submission signals - and a present whose wait list is non-empty is what sends WSI down
    * wsi_queue_submit2_unordered, which is where the console dies. A present with no wait semaphore does not
    * go there and would prove nothing. */
   VkSemaphoreCreateInfo sem_ci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
   CHECK("vkCreateSemaphore(acquired)", vkCreateSemaphore(dev, &sem_ci, NULL, &acquired));
   CHECK("vkCreateSemaphore(rendered)", vkCreateSemaphore(dev, &sem_ci, NULL, &rendered));

   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .queueFamilyIndex = qidx};
   VkCommandPool pool = VK_NULL_HANDLE;
   CHECK("vkCreateCommandPool", vkCreateCommandPool(dev, &pci, NULL, &pool));
   VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1};
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   CHECK("vkAllocateCommandBuffers", vkAllocateCommandBuffers(dev, &cbai, &cmd));
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   vkBeginCommandBuffer(cmd, &bi);
   CHECK("vkEndCommandBuffer", vkEndCommandBuffer(cmd));

   uint32_t image_index = 0;
   CHECK("vkAcquireNextImageKHR",
         vkAcquireNextImageKHR(dev, swapchain, 1000000000ull, acquired, VK_NULL_HANDLE, &image_index));
   printf("presentprobe: acquired image %u\n", image_index);

   const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                      .waitSemaphoreCount = 1,
                      .pWaitSemaphores = &acquired,
                      .pWaitDstStageMask = &wait_stage,
                      .commandBufferCount = 1,
                      .pCommandBuffers = &cmd,
                      .signalSemaphoreCount = 1,
                      .pSignalSemaphores = &rendered};
   CHECK("vkQueueSubmit(signals rendered)", vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));

   printf("presentprobe: --- vkQueuePresentKHR, waiting on one semaphore ---\n");
   fflush(stdout);
   VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                          .waitSemaphoreCount = 1,
                          .pWaitSemaphores = &rendered,
                          .swapchainCount = 1,
                          .pSwapchains = &swapchain,
                          .pImageIndices = &image_index};
   const VkResult pr = vkQueuePresentKHR(queue, &pi);
   printf("presentprobe: vkQueuePresentKHR -> %d%s\n", pr,
          pr == VK_SUCCESS ? "  THE PRESENT PATH SURVIVED" : "");
   fflush(stdout);

   /* ⚠ A SECOND PRESENT, BECAUSE ONE NO LONGER REACHES THE COPY.
    *
    * wsi_common_headless defers the copy and the flip by one present, so the frame above is still
    * pending when this function returns - and a probe that stops here exercises the request and never
    * the work. It used to print "present copied 8100 KiB, flipping index 0"; without a second present
    * that line, and everything it proves, silently disappears from the host build.
    *
    * Presenting again is what flushes the first frame, so this probe now covers the deferral itself:
    * the log must show exactly ONE copy and flip, for image 0, after the second request. */
   {
      uint32_t second = 0;
      const VkResult acq2 =
         vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &second);
      printf("presentprobe: --- second acquire -> %d, image %u ---\n", acq2, second);
      fflush(stdout);

      if (acq2 == VK_SUCCESS) {
         VkPresentInfoKHR pi2 = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                 .swapchainCount = 1,
                                 .pSwapchains = &swapchain,
                                 .pImageIndices = &second};
         const VkResult pr2 = vkQueuePresentKHR(queue, &pi2);
         printf("presentprobe: second vkQueuePresentKHR -> %d%s\n", pr2,
                pr2 == VK_SUCCESS ? "  THE DEFERRED FRAME WAS FLUSHED" : "");
         fflush(stdout);
      }
   }

   vkQueueWaitIdle(queue);
   printf("presentprobe: done\n");
   return pr == VK_SUCCESS ? 0 : 1;
}

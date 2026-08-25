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
 *
 * ⚠ AND TWO MODES FOR THE TWO PATHS NOTHING EVER RUNS, because a present that succeeds exercises neither.
 * Both of them are error handling in wsi_common_headless.c, both were wrong, both were fixed by reading
 * rather than by a test, and the Vulkan CTS marks WSI NotSupported on this target - so this file is the
 * only thing that can ask.
 *
 *     presentprobe --create-destroy [N] [--images K]
 *         N times: make a swapchain, present one frame into it, destroy it. The present is DEFERRED by a
 *         frame, so at every destroy there is a frame still owed to the display, and the destroy has to
 *         flush it BEFORE it frees the images it would read. It used to do it after.
 *
 *     presentprobe --starve [K] [--images N]
 *         One swapchain, K presents, with ORBIS_WSI_FAIL_FLIP set so flips fail. A failed flip must give
 *         the image back; it used to keep it, and after image_count failures vkAcquireNextImageKHR has
 *         nothing left to hand out and the title stops with an empty log. Reports where acquire starved,
 *         and how many images are still acquirable at the end.
 *
 *     presentprobe --acquire-timeout [--images N]
 *         Takes every image, then asks for one more twice: with a zero timeout, which must be
 *         VK_NOT_READY, and with a finite non-zero one, which must be VK_TIMEOUT. This file returned
 *         VK_NOT_READY for both, which says "there is no image" where the truth was "the wait ran out".
 *
 * ⚠ --images IS NOT COSMETIC, and it is the difference between covering the first bug and only looking
 * like it. The WSI registers the swapchain's OWN images with video-out when there are enough of them
 * (reserve + 2, so four while presents are deferred), and then a present copies NOTHING - so a present
 * that reads a freed image reads nothing and no sanitizer has anything to say. With three images the
 * registration is refused, the WSI falls back to its own buffers and a full-frame memcpy out of the image,
 * and a freed image is READ. Run the create/destroy loop both ways.
 */
#include <vulkan/vulkan.h>

#include <stdbool.h>
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

/* Everything the two new modes need that is not the swapchain itself. Built once by probe_open and then
 * left alone: a mode that recreated the device per iteration would be measuring device creation. */
struct probe {
   VkInstance               inst;
   VkPhysicalDevice         pdev;
   VkDevice                 dev;
   VkQueue                  queue;
   uint32_t                 qidx;
   VkSurfaceKHR             surface;
   VkSurfaceFormatKHR       fmt;
   VkSurfaceCapabilitiesKHR caps;
   VkCommandPool            pool;
   VkCommandBuffer          cmd;
};

static int
probe_open(struct probe *p)
{
   memset(p, 0, sizeof(*p));

   const char *inst_exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .pApplicationName = "presentprobe",
                            .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pApplicationInfo = &app,
                               .enabledExtensionCount = 2,
                               .ppEnabledExtensionNames = inst_exts};
   CHECK("vkCreateInstance", vkCreateInstance(&ici, NULL, &p->inst));

   uint32_t ndev = 0;
   vkEnumeratePhysicalDevices(p->inst, &ndev, NULL);
   if (ndev == 0) {
      printf("presentprobe: no physical device - is VK_DRIVER_FILES pointing at the orbis ICD?\n");
      return 1;
   }
   ndev = 1;
   vkEnumeratePhysicalDevices(p->inst, &ndev, &p->pdev);

   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(p->pdev, &props);
   printf("presentprobe: device \"%s\"\n", props.deviceName);

   uint32_t nq = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(p->pdev, &nq, NULL);
   VkQueueFamilyProperties *qf = calloc(nq, sizeof(*qf));
   vkGetPhysicalDeviceQueueFamilyProperties(p->pdev, &nq, qf);
   p->qidx = UINT32_MAX;
   for (uint32_t i = 0; i < nq; i++)
      if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
         p->qidx = i;
         break;
      }
   free(qf);
   if (p->qidx == UINT32_MAX) {
      printf("presentprobe: no graphics queue\n");
      return 1;
   }

   const float prio = 1.f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = p->qidx,
                                  .queueCount = 1,
                                  .pQueuePriorities = &prio};
   const char *dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qci,
                             .enabledExtensionCount = 1,
                             .ppEnabledExtensionNames = dev_exts};
   CHECK("vkCreateDevice", vkCreateDevice(p->pdev, &dci, NULL, &p->dev));

   vkGetDeviceQueue(p->dev, p->qidx, 0, &p->queue);

   PFN_vkCreateHeadlessSurfaceEXT create_surface =
      (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(p->inst, "vkCreateHeadlessSurfaceEXT");
   if (create_surface == NULL) {
      printf("presentprobe: vkCreateHeadlessSurfaceEXT is not exposed\n");
      return 1;
   }
   VkHeadlessSurfaceCreateInfoEXT sci = {.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
   CHECK("vkCreateHeadlessSurfaceEXT", create_surface(p->inst, &sci, NULL, &p->surface));

   VkBool32 supported = VK_FALSE;
   vkGetPhysicalDeviceSurfaceSupportKHR(p->pdev, p->qidx, p->surface, &supported);
   printf("presentprobe: queue family %u presents: %s\n", p->qidx, supported ? "yes" : "NO");

   uint32_t nfmt = 0;
   vkGetPhysicalDeviceSurfaceFormatsKHR(p->pdev, p->surface, &nfmt, NULL);
   VkSurfaceFormatKHR *fmts = calloc(nfmt ? nfmt : 1, sizeof(*fmts));
   vkGetPhysicalDeviceSurfaceFormatsKHR(p->pdev, p->surface, &nfmt, fmts);
   p->fmt = fmts[0];
   free(fmts);
   printf("presentprobe: %u surface format(s), taking [0] = %d / colourspace %d\n", nfmt, p->fmt.format,
          p->fmt.colorSpace);

   vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p->pdev, p->surface, &p->caps);

   VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .queueFamilyIndex = p->qidx};
   CHECK("vkCreateCommandPool", vkCreateCommandPool(p->dev, &pci, NULL, &p->pool));
   VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = p->pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1};
   CHECK("vkAllocateCommandBuffers", vkAllocateCommandBuffers(p->dev, &cbai, &p->cmd));
   VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   vkBeginCommandBuffer(p->cmd, &bi);
   CHECK("vkEndCommandBuffer", vkEndCommandBuffer(p->cmd));

   return 0;
}

/* ⚠ EVERYTHING GIVEN BACK, so that a leak report is about the driver rather than about this file. The
 * probe used to return from main with the instance, the device, the pool and the surface all still live,
 * and under LeakSanitizer that is 28 KiB of noise standing between the reader and anything real. With
 * them destroyed, `ASAN_OPTIONS=detect_leaks=1 presentprobe --create-destroy N` is a direct answer to
 * "does a swapchain teardown give everything back", which is the other half of the question the loop
 * below asks. */
static void
probe_close(struct probe *p)
{
   if (p->dev != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(p->dev);
      if (p->pool != VK_NULL_HANDLE)
         vkDestroyCommandPool(p->dev, p->pool, NULL);
      vkDestroyDevice(p->dev, NULL);
   }
   if (p->inst != VK_NULL_HANDLE) {
      if (p->surface != VK_NULL_HANDLE)
         vkDestroySurfaceKHR(p->inst, p->surface, NULL);
      vkDestroyInstance(p->inst, NULL);
   }
   memset(p, 0, sizeof(*p));
}

/* `images` of 0 means "whatever the surface says is the minimum", which is what an application does and
 * what the default mode has always asked for. Anything else is passed through untouched - including a
 * count BELOW that minimum, which is the only way to make the WSI fall back from registering the
 * swapchain's own images to allocating its own and copying into them. */
static VkResult
probe_make_swapchain(struct probe *p, uint32_t images, VkSwapchainKHR *out)
{
   const uint32_t want = images != 0 ? images : (p->caps.minImageCount < 2 ? 2 : p->caps.minImageCount);
   VkSwapchainCreateInfoKHR swci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                    .surface = p->surface,
                                    .minImageCount = want,
                                    .imageFormat = p->fmt.format,
                                    .imageColorSpace = p->fmt.colorSpace,
                                    .imageExtent = {1920, 1080},
                                    .imageArrayLayers = 1,
                                    .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                    .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                    .preTransform = p->caps.currentTransform,
                                    .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                    .presentMode = VK_PRESENT_MODE_FIFO_KHR,
                                    .clipped = VK_TRUE};
   *out = VK_NULL_HANDLE;
   return vkCreateSwapchainKHR(p->dev, &swci, NULL, out);
}

/* ---------------------------------------------------------------- the original probe, unchanged
 *
 * One swapchain, one present that waits on a semaphore, and a second present to flush the deferral. This
 * is what build.sh has always run and what reproduced two NULL sync entries; the modes below are extra
 * arguments and take nothing away from it.
 */
static int
probe_default(struct probe *p)
{
   VkSwapchainKHR swapchain = VK_NULL_HANDLE;
   CHECK("vkCreateSwapchainKHR", probe_make_swapchain(p, 0, &swapchain));

   uint32_t nimg = 0;
   vkGetSwapchainImagesKHR(p->dev, swapchain, &nimg, NULL);
   printf("presentprobe: swapchain has %u image(s)\n", nimg);

   /* ⚠ TWO SEMAPHORES, AND THE SECOND ONE IS THE WHOLE POINT. The present waits on `rendered`, which an
    * earlier submission signals - and a present whose wait list is non-empty is what sends WSI down
    * wsi_queue_submit2_unordered, which is where the console dies. A present with no wait semaphore does not
    * go there and would prove nothing. */
   VkSemaphoreCreateInfo sem_ci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
   CHECK("vkCreateSemaphore(acquired)", vkCreateSemaphore(p->dev, &sem_ci, NULL, &acquired));
   CHECK("vkCreateSemaphore(rendered)", vkCreateSemaphore(p->dev, &sem_ci, NULL, &rendered));

   uint32_t image_index = 0;
   CHECK("vkAcquireNextImageKHR",
         vkAcquireNextImageKHR(p->dev, swapchain, 1000000000ull, acquired, VK_NULL_HANDLE, &image_index));
   printf("presentprobe: acquired image %u\n", image_index);

   const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                      .waitSemaphoreCount = 1,
                      .pWaitSemaphores = &acquired,
                      .pWaitDstStageMask = &wait_stage,
                      .commandBufferCount = 1,
                      .pCommandBuffers = &p->cmd,
                      .signalSemaphoreCount = 1,
                      .pSignalSemaphores = &rendered};
   CHECK("vkQueueSubmit(signals rendered)", vkQueueSubmit(p->queue, 1, &si, VK_NULL_HANDLE));

   printf("presentprobe: --- vkQueuePresentKHR, waiting on one semaphore ---\n");
   fflush(stdout);
   VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                          .waitSemaphoreCount = 1,
                          .pWaitSemaphores = &rendered,
                          .swapchainCount = 1,
                          .pSwapchains = &swapchain,
                          .pImageIndices = &image_index};
   const VkResult pr = vkQueuePresentKHR(p->queue, &pi);
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
         vkAcquireNextImageKHR(p->dev, swapchain, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &second);
      printf("presentprobe: --- second acquire -> %d, image %u ---\n", acq2, second);
      fflush(stdout);

      if (acq2 == VK_SUCCESS) {
         VkPresentInfoKHR pi2 = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                 .swapchainCount = 1,
                                 .pSwapchains = &swapchain,
                                 .pImageIndices = &second};
         const VkResult pr2 = vkQueuePresentKHR(p->queue, &pi2);
         printf("presentprobe: second vkQueuePresentKHR -> %d%s\n", pr2,
                pr2 == VK_SUCCESS ? "  THE DEFERRED FRAME WAS FLUSHED" : "");
         fflush(stdout);
      }
   }

   vkQueueWaitIdle(p->queue);
   /* ⚠ AND THE SWAPCHAIN IS DESTROYED NOW, WHICH IT NEVER WAS. This probe used to return with the
    * swapchain still alive and let the process exit take it, so the teardown path - the one that has to
    * flush the frame still owed to the display before it frees the images that frame lives in - was
    * never once executed by anything in this tree. --create-destroy runs it in a loop; the default mode
    * runs it once, because a default that skips it is a default that hides it. */
   vkDestroySwapchainKHR(p->dev, swapchain, NULL);
   vkDestroySemaphore(p->dev, acquired, NULL);
   vkDestroySemaphore(p->dev, rendered, NULL);
   printf("presentprobe: done\n");
   return pr == VK_SUCCESS ? 0 : 1;
}

/* ---------------------------------------------------------------- create, present once, destroy
 *
 * ⚠ THE TEARDOWN IS A FRAME BOUNDARY TOO, and it was the only one nothing ever crossed here. A swapchain
 * is destroyed on every resolution change and at exit, and with presents deferred there is ALWAYS a frame
 * still owed at that moment - so the destroy has to present it. It did, and it did so after destroying
 * every image, reading a cpu_map that Unmap and Free had already invalidated. That is a use-after-free on
 * every teardown of every swapchain in the default configuration, and nothing in this tree had ever
 * destroyed a swapchain that had presented.
 *
 * So: present exactly one frame, then destroy, N times. Deferred mode deliberately - ORBIS_WSI_NO_DEFER
 * would leave nothing pending and the loop would prove nothing.
 *
 * The frame is a real one: acquire with a semaphore, a submission that waits on it and signals another,
 * and a present that waits on THAT - the same shape as the default mode, because a present with an empty
 * wait list takes a different route through WSI and this loop should not quietly stop covering it.
 */
static int
probe_create_destroy(struct probe *p, unsigned loops, uint32_t images)
{
   printf("presentprobe: create/destroy loop - %u iteration(s), %u image(s) requested\n", loops, images);
   fflush(stdout);

   for (unsigned i = 0; i < loops; i++) {
      VkSwapchainKHR swapchain = VK_NULL_HANDLE;
      const VkResult cr = probe_make_swapchain(p, images, &swapchain);
      if (cr != VK_SUCCESS) {
         printf("presentprobe: iteration %u: vkCreateSwapchainKHR -> %d  FAILED\n", i, cr);
         return 1;
      }

      uint32_t nimg = 0;
      vkGetSwapchainImagesKHR(p->dev, swapchain, &nimg, NULL);

      VkSemaphoreCreateInfo sem_ci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
      vkCreateSemaphore(p->dev, &sem_ci, NULL, &acquired);
      vkCreateSemaphore(p->dev, &sem_ci, NULL, &rendered);

      uint32_t idx = 0;
      const VkResult acq =
         vkAcquireNextImageKHR(p->dev, swapchain, 1000000000ull, acquired, VK_NULL_HANDLE, &idx);
      if (acq != VK_SUCCESS) {
         printf("presentprobe: iteration %u: vkAcquireNextImageKHR -> %d  FAILED\n", i, acq);
         return 1;
      }

      const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                         .waitSemaphoreCount = 1,
                         .pWaitSemaphores = &acquired,
                         .pWaitDstStageMask = &wait_stage,
                         .commandBufferCount = 1,
                         .pCommandBuffers = &p->cmd,
                         .signalSemaphoreCount = 1,
                         .pSignalSemaphores = &rendered};
      vkQueueSubmit(p->queue, 1, &si, VK_NULL_HANDLE);

      VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                             .waitSemaphoreCount = 1,
                             .pWaitSemaphores = &rendered,
                             .swapchainCount = 1,
                             .pSwapchains = &swapchain,
                             .pImageIndices = &idx};
      const VkResult pr = vkQueuePresentKHR(p->queue, &pi);

      /* Idle first, so the destroy below is the only thing left that can touch the images. Without it a
       * fault could be blamed on a submission still running, which is exactly the ambiguity this loop
       * exists to remove. */
      vkQueueWaitIdle(p->queue);

      printf("presentprobe: iteration %2u: %u image(s), acquired %u, present -> %d, destroying with a "
             "frame still pending\n",
             i, nimg, idx, pr);
      fflush(stdout);

      vkDestroySwapchainKHR(p->dev, swapchain, NULL);
      vkDestroySemaphore(p->dev, acquired, NULL);
      vkDestroySemaphore(p->dev, rendered, NULL);
   }

   printf("presentprobe: create/destroy loop survived %u teardown(s) with a deferred frame pending\n",
          loops);
   return 0;
}

/* ---------------------------------------------------------------- presents that fail
 *
 * ⚠ WHAT HAPPENS TO AN IMAGE WHOSE FLIP DID NOT HAPPEN. Nothing else asks. A failed flip is reported to
 * the application as VK_ERROR_OUT_OF_DATE_KHR, and an application answers that by rebuilding the
 * swapchain - so a leak of one image per failure is invisible to any real title and fatal to the one that
 * keeps going. This mode is the one that keeps going.
 *
 * It reports two things, and the second is the point:
 *
 *   where acquire first failed   - with the leak, that is after about image_count failures; without it,
 *                                  never
 *   images still acquirable      - drained at the end with no presents in between, so it is the free
 *                                  count and not a rate
 *
 * Needs ORBIS_WSI_FAIL_FLIP in the environment; without it every flip succeeds and the mode measures
 * nothing, so it says so rather than passing quietly.
 */
static int
probe_starve(struct probe *p, unsigned presents, uint32_t images)
{
   const char *const fail = getenv("ORBIS_WSI_FAIL_FLIP");
   printf("presentprobe: starvation test - %u present(s), %u image(s) requested, ORBIS_WSI_FAIL_FLIP=%s\n",
          presents, images, fail != NULL ? fail : "(unset)");
   if (fail == NULL)
      printf("presentprobe: ORBIS_WSI_FAIL_FLIP is not set - no flip will fail and this test cannot "
             "starve. Set it to 1 to make every flip fail.\n");
   fflush(stdout);

   VkSwapchainKHR swapchain = VK_NULL_HANDLE;
   CHECK("vkCreateSwapchainKHR", probe_make_swapchain(p, images, &swapchain));

   uint32_t nimg = 0;
   vkGetSwapchainImagesKHR(p->dev, swapchain, &nimg, NULL);
   printf("presentprobe: swapchain has %u image(s)\n", nimg);

   unsigned ok = 0, out_of_date = 0, other = 0;
   int      starved_at = -1;

   for (unsigned i = 0; i < presents; i++) {
      uint32_t idx = 0;
      /* ⚠ NOT ZERO AND NOT INFINITE. acquire_next_image skips an image it has just handed back once
       * before returning it, so a zero timeout can report NOT_READY about a swapchain that has free
       * images - and an infinite timeout turns the failure this test looks for into a hang. 100 ms is
       * far more than the two passes need. */
      const VkResult acq = vkAcquireNextImageKHR(p->dev, swapchain, 100ull * 1000 * 1000, VK_NULL_HANDLE,
                                                 VK_NULL_HANDLE, &idx);
      if (acq != VK_SUCCESS) {
         starved_at = (int)i;
         printf("presentprobe: STARVED - vkAcquireNextImageKHR -> %d at present %u of %u, after %u "
                "failed present(s) on a %u-image swapchain\n",
                acq, i, presents, out_of_date, nimg);
         fflush(stdout);
         break;
      }

      VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                             .swapchainCount = 1,
                             .pSwapchains = &swapchain,
                             .pImageIndices = &idx};
      const VkResult pr = vkQueuePresentKHR(p->queue, &pi);
      if (pr == VK_SUCCESS)
         ok++;
      else if (pr == VK_ERROR_OUT_OF_DATE_KHR)
         out_of_date++;
      else
         other++;
   }

   vkQueueWaitIdle(p->queue);

   /* How many images the swapchain will still hand out, asked without presenting anything in between so
    * the answer is a count and not a rate. */
   unsigned free_now = 0;
   for (;;) {
      uint32_t idx = 0;
      const VkResult acq = vkAcquireNextImageKHR(p->dev, swapchain, 100ull * 1000 * 1000, VK_NULL_HANDLE,
                                                 VK_NULL_HANDLE, &idx);
      if (acq != VK_SUCCESS)
         break;
      free_now++;
      if (free_now > nimg) /* cannot happen; a runaway loop would be worse than a wrong number */
         break;
   }

   printf("presentprobe: presents: %u ok, %u OUT_OF_DATE, %u other\n", ok, out_of_date, other);
   printf("presentprobe: %u of %u image(s) still acquirable after the run\n", free_now, nimg);

   vkDestroySwapchainKHR(p->dev, swapchain, NULL);

   if (starved_at >= 0) {
      printf("presentprobe: FAILED - acquire starved at present %d. A failed flip is keeping the image "
             "it failed to show.\n",
             starved_at);
      return 1;
   }
   if (fail != NULL && out_of_date == 0) {
      printf("presentprobe: FAILED - ORBIS_WSI_FAIL_FLIP was set and not one present reported "
             "OUT_OF_DATE. The injection did not reach the flip.\n");
      return 1;
   }
   printf("presentprobe: no starvation - %u present(s) survived %u failed flip(s)\n", presents,
          out_of_date);
   return 0;
}

/* ---------------------------------------------------------------- what an expired wait is called
 *
 * ⚠ TWO ANSWERS THAT A LOOP CANNOT TELL APART IF THE DRIVER GIVES ONLY ONE. vkAcquireNextImageKHR has
 * two ways to come back empty and they mean different things: with a zero timeout the caller asked
 * "is one free right now" and VK_NOT_READY is the answer, while with a non-zero timeout the caller
 * waited and VK_TIMEOUT says the wait ran out. wsi_common_headless.c returned VK_NOT_READY for both.
 *
 * Nothing in this port noticed, because nothing in this port ever ran out of images on purpose - and
 * the layer about to be built on this path does read the difference: a GL-over-Vulkan frontend that
 * takes VK_NOT_READY from a wait as "this swapchain has no images for you" answers by rebuilding the
 * swapchain or by spinning, and either one appears as a stall three layers above the mistake.
 *
 * So the swapchain is emptied first - acquire until it refuses, which leaves every image held by this
 * process - and then the same question is asked twice with the two timeouts. Both codes are asserted;
 * checking only the VK_TIMEOUT one would pass a driver that had simply started returning VK_TIMEOUT
 * for everything, which is the same bug with the answers exchanged.
 */
static const char *
probe_result_name(VkResult r)
{
   switch (r) {
   case VK_SUCCESS:   return "VK_SUCCESS";
   case VK_NOT_READY: return "VK_NOT_READY";
   case VK_TIMEOUT:   return "VK_TIMEOUT";
   default:           return "(other)";
   }
}

static int
probe_acquire_timeout(struct probe *p, uint32_t images)
{
   printf("presentprobe: acquire timeout codes - %u image(s) requested\n", images);
   fflush(stdout);

   VkSwapchainKHR swapchain = VK_NULL_HANDLE;
   CHECK("vkCreateSwapchainKHR", probe_make_swapchain(p, images, &swapchain));

   uint32_t nimg = 0;
   vkGetSwapchainImagesKHR(p->dev, swapchain, &nimg, NULL);

   /* 100 ms rather than 0, because acquire skips an image it has just handed back once before
    * returning it - a zero timeout can stop one image early and then the swapchain is not actually
    * empty, which is the one precondition this test has. */
   unsigned held = 0;
   for (;;) {
      uint32_t idx = 0;
      const VkResult acq = vkAcquireNextImageKHR(p->dev, swapchain, 100ull * 1000 * 1000,
                                                 VK_NULL_HANDLE, VK_NULL_HANDLE, &idx);
      if (acq != VK_SUCCESS)
         break;
      held++;
      if (held > nimg)
         break;
   }
   printf("presentprobe: swapchain has %u image(s), %u now held - it can hand out no more\n", nimg,
          held);

   uint32_t idx = 0;
   const VkResult zero = vkAcquireNextImageKHR(p->dev, swapchain, 0, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                               &idx);
   const VkResult finite = vkAcquireNextImageKHR(p->dev, swapchain, 50ull * 1000 * 1000,
                                                 VK_NULL_HANDLE, VK_NULL_HANDLE, &idx);

   printf("presentprobe: timeout 0     -> %d %s (want %d VK_NOT_READY)\n", zero,
          probe_result_name(zero), VK_NOT_READY);
   printf("presentprobe: timeout 50 ms -> %d %s (want %d VK_TIMEOUT)\n", finite,
          probe_result_name(finite), VK_TIMEOUT);

   vkDestroySwapchainKHR(p->dev, swapchain, NULL);

   int rc = 0;
   if (held != nimg) {
      printf("presentprobe: FAILED - held %u of %u images, so the swapchain was not empty and neither "
             "answer means anything\n",
             held, nimg);
      rc = 1;
   }
   if (zero != VK_NOT_READY) {
      printf("presentprobe: FAILED - a zero timeout must be VK_NOT_READY, not %s\n",
             probe_result_name(zero));
      rc = 1;
   }
   if (finite != VK_TIMEOUT) {
      printf("presentprobe: FAILED - an expired non-zero timeout must be VK_TIMEOUT, not %s. A caller "
             "that tells the two apart reads this as a swapchain with no images rather than as a wait "
             "that ran out.\n",
             probe_result_name(finite));
      rc = 1;
   }
   if (rc == 0)
      printf("presentprobe: acquire reports VK_NOT_READY for a poll and VK_TIMEOUT for an expired "
             "wait\n");
   return rc;
}

int
main(int argc, char **argv)
{
   unsigned create_destroy = 0, starve = 0;
   bool     acquire_timeout = false;
   uint32_t images = 0;

   for (int i = 1; i < argc; i++) {
      const bool has_n = i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9';

      if (strcmp(argv[i], "--create-destroy") == 0) {
         create_destroy = has_n ? (unsigned)strtoul(argv[++i], NULL, 10) : 16;
      } else if (strcmp(argv[i], "--starve") == 0) {
         starve = has_n ? (unsigned)strtoul(argv[++i], NULL, 10) : 64;
      } else if (strcmp(argv[i], "--acquire-timeout") == 0) {
         acquire_timeout = true;
      } else if (strcmp(argv[i], "--images") == 0 && i + 1 < argc) {
         images = (uint32_t)strtoul(argv[++i], NULL, 10);
      } else {
         printf("presentprobe: usage: presentprobe [--create-destroy [N]] [--starve [K]] "
                "[--acquire-timeout] [--images N]\n");
         return 2;
      }
   }

   struct probe p;
   const int    opened = probe_open(&p);
   if (opened != 0)
      return opened;

   int rc = 0;
   if (create_destroy == 0 && starve == 0 && !acquire_timeout)
      rc = probe_default(&p);
   if (create_destroy != 0)
      rc |= probe_create_destroy(&p, create_destroy, images);
   if (starve != 0)
      rc |= probe_starve(&p, starve, images);
   if (acquire_timeout)
      rc |= probe_acquire_timeout(&p, images);

   probe_close(&p);
   return rc;
}

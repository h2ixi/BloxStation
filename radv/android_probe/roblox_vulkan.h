/* Roblox's Vulkan renderer on the PS5's RADV.
 *
 * On Android the engine dlopen()s libvulkan.so, takes vkGetInstanceProcAddr
 * from it and draws to the activity's window through VK_KHR_android_surface.
 * Here that "libvulkan.so" is this title's own RADV (linked in, reached through
 * vk_icdGetInstanceProcAddr), and the window is the VideoOut display:
 *
 * - every function pointer handed to the engine is a readable trampoline (the
 *   title's code is execute-only, and the engine reads function bytes);
 * - VK_KHR_android_surface is advertised; at vkCreateInstance it is replaced by
 *   VK_KHR_surface + VK_KHR_display, and other extensions RADV does not report
 *   are left out (and logged) instead of failing the instance;
 * - vkCreateAndroidSurfaceKHR asks the demo loop to give the display up, then
 *   makes a display-plane surface on VideoOut;
 * - vkCreateDevice leaves out device extensions RADV does not report.
 *
 * Included by radv_smoke.c after roblox_trampoline(). */

static volatile int rv_display_wanted, rv_display_released, rv_demo_presenting;
static VkInstance rv_instance;
static VkPhysicalDevice rv_phys;
static unsigned rv_missing;

/* ---- trampolines, one per target ------------------------------------------ */
#define RV_TCACHE 4096
static struct {
   void *fn, *stub;
} rv_tcache[RV_TCACHE];
static pthread_mutex_t rv_lock = PTHREAD_MUTEX_INITIALIZER;

static void *
rv_tramp(void *fn)
{
   if (!fn)
      return NULL;
   pthread_mutex_lock(&rv_lock);
   const unsigned h = (unsigned)(((uintptr_t)fn >> 4) * 2654435761u) % RV_TCACHE;
   void *stub = fn;
   for (unsigned i = 0; i < RV_TCACHE; i++) {
      const unsigned k = (h + i) % RV_TCACHE;
      if (rv_tcache[k].fn == fn) {
         stub = rv_tcache[k].stub;
         break;
      }
      if (!rv_tcache[k].fn) {
         stub = roblox_trampoline(fn);
         rv_tcache[k].fn = fn;
         rv_tcache[k].stub = stub;
         break;
      }
   }
   pthread_mutex_unlock(&rv_lock);
   return stub;
}

static PFN_vkVoidFunction
rv_real(VkInstance inst, const char *name)
{
   return vk_icdGetInstanceProcAddr(inst, name);
}

/* ---- instance -------------------------------------------------------------- */
static VkResult VKAPI_CALL
rv_EnumerateInstanceExtensionProperties(const char *layer, uint32_t *count, VkExtensionProperties *props)
{
   if (layer) {
      *count = 0;
      return VK_ERROR_LAYER_NOT_PRESENT;
   }
   PFN_vkEnumerateInstanceExtensionProperties real =
      (PFN_vkEnumerateInstanceExtensionProperties)rv_real(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
   uint32_t n = 0;
   real(NULL, &n, NULL);
   VkExtensionProperties *all = calloc(n + 1, sizeof(*all));
   if (!all)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   real(NULL, &n, all);
   snprintf(all[n].extensionName, sizeof(all[n].extensionName), "VK_KHR_android_surface");
   all[n].specVersion = 6;
   n++;
   VkResult r = VK_SUCCESS;
   if (!props) {
      *count = n;
   } else {
      const uint32_t c = *count < n ? *count : n;
      memcpy(props, all, c * sizeof(*all));
      if (c < n)
         r = VK_INCOMPLETE;
      *count = c;
   }
   free(all);
   return r;
}

static VkResult VKAPI_CALL
rv_EnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *props)
{
   (void)props;
   *count = 0;
   return VK_SUCCESS;
}

static bool
rv_listed(const VkExtensionProperties *list, uint32_t n, const char *name)
{
   for (uint32_t i = 0; i < n; i++)
      if (strcmp(list[i].extensionName, name) == 0)
         return true;
   return false;
}

static VkResult VKAPI_CALL
rv_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkInstance *out)
{
   PFN_vkCreateInstance real = (PFN_vkCreateInstance)rv_real(VK_NULL_HANDLE, "vkCreateInstance");
   PFN_vkEnumerateInstanceExtensionProperties en =
      (PFN_vkEnumerateInstanceExtensionProperties)rv_real(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
   uint32_t have = 0;
   en(NULL, &have, NULL);
   VkExtensionProperties *list = calloc(have ? have : 1, sizeof(*list));
   en(NULL, &have, list);
   const char *names[96];
   uint32_t n = 0;
   char dropped[512] = "";
   for (uint32_t i = 0; i < ci->enabledExtensionCount && n < 90; i++) {
      const char *e = ci->ppEnabledExtensionNames[i];
      if (strcmp(e, "VK_KHR_android_surface") == 0)
         continue;
      if (rv_listed(list, have, e))
         names[n++] = e;
      else
         snprintf(dropped + strlen(dropped), sizeof(dropped) - strlen(dropped), " %s", e);
   }
   const char *ours[] = {"VK_KHR_surface", "VK_KHR_display"};
   for (unsigned k = 0; k < 2; k++) {
      bool already = false;
      for (uint32_t i = 0; i < n; i++)
         already |= strcmp(names[i], ours[k]) == 0;
      if (!already)
         names[n++] = ours[k];
   }
   free(list);
   VkInstanceCreateInfo c = *ci;
   c.enabledExtensionCount = n;
   c.ppEnabledExtensionNames = names;
   c.enabledLayerCount = 0;
   c.ppEnabledLayerNames = NULL;
   const uint32_t api = ci->pApplicationInfo ? ci->pApplicationInfo->apiVersion : 0;
   report("roblox-vk: vkCreateInstance(app \"%s\", API %u.%u, %u extensions asked, %u given; left out:%s)",
          ci->pApplicationInfo && ci->pApplicationInfo->pApplicationName ? ci->pApplicationInfo->pApplicationName : "",
          VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), ci->enabledExtensionCount, n,
          dropped[0] ? dropped : " none");
   const VkResult r = real(&c, alloc, out);
   report("roblox-vk: vkCreateInstance -> %d", r);
   if (r == VK_SUCCESS)
      rv_instance = *out;
   return r;
}

/* ---- the window: VideoOut's display plane ---------------------------------- */
struct rv_android_surface_info { /* VkAndroidSurfaceCreateInfoKHR */
   VkStructureType sType;
   const void *pNext;
   VkFlags flags;
   void *window;
};

/* The surface size chosen below (ROBLOX_RES). The display-plane surface
 * reported the display's own size (4K) as its current extent, so Roblox drew
 * a 3840x2160 swapchain: four times the pixels of the 1080p surface asked
 * for. Its capabilities now report this size. */
static VkExtent2D rv_surface_ext;
/* the display's refresh rate in mHz (the chosen mode), told to Roblox */
static uint32_t rv_refresh_mhz;

static VkResult VKAPI_CALL
rv_CreateAndroidSurfaceKHR(VkInstance inst, const struct rv_android_surface_info *ci,
                           const VkAllocationCallbacks *alloc, VkSurfaceKHR *out)
{
   report("roblox-vk: vkCreateAndroidSurfaceKHR(window %p): asking the demo for the display",
          ci ? ci->window : NULL);
   __atomic_store_n(&rv_display_wanted, 1, __ATOMIC_SEQ_CST);
   /* Only a demo that is presenting has to be waited for; one that has not
    * started its swapchain sees the flag and never starts it (and it may be
    * blocked on this very call, on the main thread). */
   if (__atomic_load_n(&rv_demo_presenting, __ATOMIC_SEQ_CST)) {
      for (int i = 0; i < 200 && !__atomic_load_n(&rv_display_released, __ATOMIC_SEQ_CST); i++)
         usleep(50 * 1000);
      if (!__atomic_load_n(&rv_display_released, __ATOMIC_SEQ_CST))
         report("roblox-vk: the demo did not give the display up in 10 s; trying anyway");
   }
   PFN_vkEnumeratePhysicalDevices enum_pd = (PFN_vkEnumeratePhysicalDevices)rv_real(inst, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceDisplayPropertiesKHR props_fn =
      (PFN_vkGetPhysicalDeviceDisplayPropertiesKHR)rv_real(inst, "vkGetPhysicalDeviceDisplayPropertiesKHR");
   PFN_vkGetDisplayModePropertiesKHR modes_fn =
      (PFN_vkGetDisplayModePropertiesKHR)rv_real(inst, "vkGetDisplayModePropertiesKHR");
   PFN_vkCreateDisplayPlaneSurfaceKHR create =
      (PFN_vkCreateDisplayPlaneSurfaceKHR)rv_real(inst, "vkCreateDisplayPlaneSurfaceKHR");
   if (!enum_pd || !props_fn || !modes_fn || !create) {
      report("roblox-vk: VK_KHR_display commands missing on this instance");
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   VkPhysicalDevice pd = VK_NULL_HANDLE;
   uint32_t count = 1;
   VkResult r = enum_pd(inst, &count, &pd);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !pd) {
      report("roblox-vk: no physical device (%d)", r);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   VkDisplayPropertiesKHR display;
   count = 1;
   r = props_fn(pd, &count, &display);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count < 1) {
      report("roblox-vk: no display (%d)", r);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   VkDisplayModePropertiesKHR modes[4];
   count = 4;
   r = modes_fn(pd, display.display, &count, modes);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count < 1) {
      report("roblox-vk: no display mode (%d)", r);
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   /* Roblox draws at the surface size. 4K costs the GPU 4x a 1080p frame;
    * ROBLOX_RES=1080 (default) asks for a 1920x1080 surface scaled to the
    * display, ROBLOX_RES=native keeps the display's own size. */
   VkExtent2D ext = display.physicalResolution;
   const char *res = getenv("ROBLOX_RES");
   if (!res || strcmp(res, "native") != 0) {
      const int h = res ? atoi(res) : 1080;
      if (h >= 360 && (uint32_t)h < ext.height)
         ext = (VkExtent2D){(uint32_t)h * 16 / 9, (uint32_t)h};
   }
   /* the fastest refresh the display offers (a 120 Hz TV mode lets Roblox
    * run above 60); ROBLOX_HZ=60 keeps the first mode */
   unsigned pick = 0;
   for (unsigned m = 0; m < count && m < 4; m++) {
      report("roblox-vk: display mode %u: %ux%u at %.2f Hz", m, modes[m].parameters.visibleRegion.width,
             modes[m].parameters.visibleRegion.height, modes[m].parameters.refreshRate / 1000.0);
      const char *hz = getenv("ROBLOX_HZ");
      const uint32_t cap = hz ? (uint32_t)atoi(hz) * 1000 + 999 : 0xffffffffu;
      if (modes[m].parameters.refreshRate > modes[pick].parameters.refreshRate && modes[m].parameters.refreshRate <= cap)
         pick = m;
   }
   VkDisplaySurfaceCreateInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
      .displayMode = modes[pick].displayMode,
      .planeIndex = 0,
      .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
      .imageExtent = ext,
   };
   r = create(inst, &info, alloc, out);
   if (r != VK_SUCCESS && (ext.width != display.physicalResolution.width)) {
      report("roblox-vk: %ux%u surface refused (%d); using the display's own size", ext.width, ext.height, r);
      info.imageExtent = ext = display.physicalResolution;
      r = create(inst, &info, alloc, out);
   }
   if (r == VK_SUCCESS) {
      rv_surface_ext = ext;
      rv_refresh_mhz = modes[pick].parameters.refreshRate;
   }
   report("roblox-vk: display-plane surface %ux%u (display %ux%u) on %s -> %d", ext.width, ext.height,
          display.physicalResolution.width, display.physicalResolution.height,
          display.displayName ? display.displayName : "?", r);
   return r;
}

/* ---- device ---------------------------------------------------------------- */
static VkDevice rv_device; /* the engine's device, for the clean exit */
static int rv_quitting;    /* set by the clean exit: presents park the render thread */
static VkResult VKAPI_CALL
rv_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDevice *out)
{
   PFN_vkCreateDevice real = (PFN_vkCreateDevice)rv_real(rv_instance, "vkCreateDevice");
   PFN_vkEnumerateDeviceExtensionProperties en =
      (PFN_vkEnumerateDeviceExtensionProperties)rv_real(rv_instance, "vkEnumerateDeviceExtensionProperties");
   uint32_t have = 0;
   en(pd, NULL, &have, NULL);
   VkExtensionProperties *list = calloc(have ? have : 1, sizeof(*list));
   en(pd, NULL, &have, list);
   const char *names[160];
   uint32_t n = 0;
   char dropped[768] = "";
   for (uint32_t i = 0; i < ci->enabledExtensionCount && n < 160; i++) {
      const char *e = ci->ppEnabledExtensionNames[i];
      if (rv_listed(list, have, e))
         names[n++] = e;
      else
         snprintf(dropped + strlen(dropped), sizeof(dropped) - strlen(dropped), " %s", e);
   }
   free(list);
   VkDeviceCreateInfo c = *ci;
   c.enabledExtensionCount = n;
   c.ppEnabledExtensionNames = names;
   c.enabledLayerCount = 0;
   c.ppEnabledLayerNames = NULL;
   report("roblox-vk: vkCreateDevice(%u queue families, %u extensions asked, %u given; left out:%s)",
          ci->queueCreateInfoCount, ci->enabledExtensionCount, n, dropped[0] ? dropped : " none");
   const VkResult r = real(pd, &c, alloc, out);
   report("roblox-vk: vkCreateDevice -> %d", r);
   rv_phys = pd;
   if (r == VK_SUCCESS)
      rv_device = *out;
   return r;
}

static VkResult VKAPI_CALL
rv_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice pd, VkSurfaceKHR surf, VkSurfaceCapabilitiesKHR *caps)
{
   PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR real =
      (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)rv_real(rv_instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
   const VkResult r = real(pd, surf, caps);
   if (r == VK_SUCCESS && rv_surface_ext.width && !getenv("ROBLOX_NATIVE_SWAPCHAIN")) {
      static int said;
      if (!said++)
         report("roblox-vk: surface capabilities: current extent %ux%u -> %ux%u", caps->currentExtent.width,
                caps->currentExtent.height, rv_surface_ext.width, rv_surface_ext.height);
      caps->currentExtent = rv_surface_ext;
      if (caps->minImageExtent.width > rv_surface_ext.width || caps->minImageExtent.height > rv_surface_ext.height)
         caps->minImageExtent = rv_surface_ext;
      caps->maxImageExtent = rv_surface_ext;
   }
   return r;
}

/* ---- the overlay -----------------------------------------------------------
 * The on-screen guide (roblox_guide.h) is copied into each frame just before
 * it is presented, while a guide page is open. */
static struct {
   VkDevice dev;
   VkSwapchainKHR sc;
   VkExtent2D ext;
   uint32_t n;
   VkImage img[8];
   VkCommandPool pool;
   VkCommandBuffer cb[8];
   VkFence fence[8];
   VkSemaphore done[8];
   VkBuffer gbuf;     /* the guide page (roblox_guide.h) */
   uint32_t *gmap;
   int gpage, bgra;
   int ready, failed;
} rvc;
static pthread_mutex_t rvc_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t rv_present_family; /* the queue family of the presenting queue (vkGetDeviceQueue) */
static VkQueue rv_present_queue_seen[8];
static uint32_t rv_present_queue_family[8];

#define RVD(name) ((PFN_##name)gdpa(dev, #name))
/* a mapped host-visible transfer-source buffer of `size` bytes, or NULL */
static void *
rv_host_buffer(VkDevice dev, PFN_vkGetDeviceProcAddr gdpa, VkDeviceSize size, VkBuffer *out)
{
   const VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size,
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
   if (RVD(vkCreateBuffer)(dev, &bi, NULL, out) != VK_SUCCESS)
      return NULL;
   VkMemoryRequirements mr;
   RVD(vkGetBufferMemoryRequirements)(dev, *out, &mr);
   VkPhysicalDeviceMemoryProperties mp;
   ((PFN_vkGetPhysicalDeviceMemoryProperties)rv_real(rv_instance, "vkGetPhysicalDeviceMemoryProperties"))(rv_phys, &mp);
   uint32_t type = UINT32_MAX;
   const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
      if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
         type = i;
   const VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, type};
   VkDeviceMemory mem;
   void *map = NULL;
   if (type == UINT32_MAX || RVD(vkAllocateMemory)(dev, &ai, NULL, &mem) != VK_SUCCESS ||
       RVD(vkBindBufferMemory)(dev, *out, mem, 0) != VK_SUCCESS ||
       RVD(vkMapMemory)(dev, mem, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS)
      return NULL;
   return map;
}

static void
rv_overlay_swapchain(VkDevice dev, VkSwapchainKHR sc, VkExtent2D ext, VkFormat fmt)
{
   pthread_mutex_lock(&rvc_lock);
   rvc.ready = 0; /* the old one's objects are left as they are (rare) */
   rvc.dev = dev, rvc.sc = sc, rvc.ext = ext;
   rvc.bgra = fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB;
   rvc.gpage = -1;
   if (fmt != VK_FORMAT_B8G8R8A8_UNORM && fmt != VK_FORMAT_R8G8B8A8_UNORM && fmt != VK_FORMAT_B8G8R8A8_SRGB &&
       fmt != VK_FORMAT_R8G8B8A8_SRGB) {
      pthread_mutex_unlock(&rvc_lock);
      return;
   }
   PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)rv_real(rv_instance, "vkGetDeviceProcAddr");
   uint32_t n = 8;
   VkResult r = RVD(vkGetSwapchainImagesKHR)(dev, sc, &n, rvc.img);
   if (r != VK_SUCCESS && r != VK_INCOMPLETE)
      goto fail;
   rvc.n = n;
   if (!rvc.gbuf) {
      rvc.gmap = rv_host_buffer(dev, gdpa, (VkDeviceSize)GUIDE_W * GUIDE_H * 4, &rvc.gbuf);
      if (!rvc.gmap)
         goto fail;
   }
   if (!rvc.pool) {
      if (rv_present_queue_seen[0])
         rv_present_family = rv_present_queue_family[0];
      const VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                          VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, rv_present_family};
      if (RVD(vkCreateCommandPool)(dev, &pi, NULL, &rvc.pool) != VK_SUCCESS)
         goto fail;
      const VkCommandBufferAllocateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, rvc.pool,
                                              VK_COMMAND_BUFFER_LEVEL_PRIMARY, 8};
      if (RVD(vkAllocateCommandBuffers)(dev, &ci, rvc.cb) != VK_SUCCESS)
         goto fail;
      for (int i = 0; i < 8; i++) {
         const VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
         const VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0};
         if (RVD(vkCreateFence)(dev, &fi, NULL, &rvc.fence[i]) != VK_SUCCESS ||
             RVD(vkCreateSemaphore)(dev, &si, NULL, &rvc.done[i]) != VK_SUCCESS)
            goto fail;
      }
   }
   rvc.ready = 1;
   report("roblox-vk: overlay (guide) ready (%u images, queue family %u)", n, rv_present_family);
   pthread_mutex_unlock(&rvc_lock);
   return;
fail:
   rvc.failed = 1;
   report("roblox-vk: overlay (guide) could not be set up");
   pthread_mutex_unlock(&rvc_lock);
}

/* before the real present: when a guide page is open, copy it into the image
 * and present after that copy; returns the info to present with */
static const VkPresentInfoKHR *
rv_overlay_draw(VkQueue q, const VkPresentInfoKHR *info, VkPresentInfoKHR *mine, VkSemaphore *wait)
{
   const int page = __atomic_load_n(&rg_page, __ATOMIC_ACQUIRE);
   const int guide = page >= 0 && rvc.gmap && rvc.ext.width >= GUIDE_W && rvc.ext.height >= GUIDE_H;
   if (!guide || !rvc.ready || info->swapchainCount != 1 || info->pSwapchains[0] != rvc.sc)
      return info;
   const uint32_t ii = info->pImageIndices[0];
   if (ii >= rvc.n)
      return info;
   for (unsigned k = 0; k < 8; k++)
      if (rv_present_queue_seen[k] == q && rv_present_queue_family[k] != rv_present_family)
         return info; /* the pool belongs to another family */
   VkDevice dev = rvc.dev;
   PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)rv_real(rv_instance, "vkGetDeviceProcAddr");
   static PFN_vkWaitForFences wf;
   static PFN_vkResetFences rf;
   static PFN_vkBeginCommandBuffer bcb;
   static PFN_vkEndCommandBuffer ecb;
   static PFN_vkCmdPipelineBarrier bar;
   static PFN_vkCmdCopyBufferToImage cpy;
   static PFN_vkQueueSubmit sub;
   if (!wf) {
      wf = RVD(vkWaitForFences), rf = RVD(vkResetFences), bcb = RVD(vkBeginCommandBuffer);
      ecb = RVD(vkEndCommandBuffer), bar = RVD(vkCmdPipelineBarrier), cpy = RVD(vkCmdCopyBufferToImage);
      sub = RVD(vkQueueSubmit);
   }
   if (wf(dev, 1, &rvc.fence[ii], VK_TRUE, 100000000ull) != VK_SUCCESS)
      return info;
   if (rvc.gpage != page) {
      /* a new page: nothing may still be copying from the buffer */
      if (wf(dev, rvc.n, rvc.fence, VK_TRUE, 1000000000ull) != VK_SUCCESS)
         return info;
      rg_decode(page, rvc.gmap, rvc.bgra);
      rvc.gpage = page;
   }
   rf(dev, 1, &rvc.fence[ii]);
   VkCommandBuffer cb = rvc.cb[ii];
   const VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
   bcb(cb, &bi);
   VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                             rvc.img[ii], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   bar(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
   /* the page, in the middle of the screen */
   const VkBufferImageCopy g = {0, GUIDE_W, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                {(int32_t)(rvc.ext.width - GUIDE_W) / 2, (int32_t)(rvc.ext.height - GUIDE_H) / 2, 0},
                                {GUIDE_W, GUIDE_H, 1}};
   cpy(cb, rvc.gbuf, rvc.img[ii], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &g);
   b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, b.dstAccessMask = 0;
   b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
   bar(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
   ecb(cb);
   VkPipelineStageFlags st[16];
   const uint32_t nw = info->waitSemaphoreCount < 16 ? info->waitSemaphoreCount : 16;
   for (uint32_t i = 0; i < nw; i++)
      st[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   const VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, nw, info->pWaitSemaphores, st, 1, &cb, 1,
                            &rvc.done[ii]};
   if (sub(q, 1, &si, rvc.fence[ii]) != VK_SUCCESS) {
      static int said;
      if (!said++)
         report("roblox-vk: overlay: submit failed");
      return info;
   }
   *wait = rvc.done[ii];
   *mine = *info;
   mine->waitSemaphoreCount = 1;
   mine->pWaitSemaphores = wait;
   return mine;
}

static void VKAPI_CALL
rv_GetDeviceQueue(VkDevice dev, uint32_t family, uint32_t index, VkQueue *q)
{
   PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)rv_real(rv_instance, "vkGetDeviceProcAddr");
   RVD(vkGetDeviceQueue)(dev, family, index, q);
   for (unsigned k = 0; k < 8; k++)
      if (!rv_present_queue_seen[k] || rv_present_queue_seen[k] == *q) {
         rv_present_queue_seen[k] = *q;
         rv_present_queue_family[k] = family;
         break;
      }
}

/* ---- swapchain and presents: counted, so the log shows frames ------------- */
static unsigned rv_presents;
static uint64_t rv_present_t0;
static VkResult VKAPI_CALL
rv_CreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *alloc,
                      VkSwapchainKHR *out)
{
   PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)rv_real(rv_instance, "vkGetDeviceProcAddr");
   PFN_vkCreateSwapchainKHR real = (PFN_vkCreateSwapchainKHR)gdpa(dev, "vkCreateSwapchainKHR");
   /* test: ROBLOX_PRESENT_MODE=n overrides the present mode (0 immediate,
    * 1 mailbox, 2 fifo) to see how fast frames really are */
   VkSwapchainCreateInfoKHR c2 = *ci;
   if (getenv("ROBLOX_PRESENT_MODE"))
      c2.presentMode = (VkPresentModeKHR)atoi(getenv("ROBLOX_PRESENT_MODE"));
   c2.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT; /* the guide is copied in (rv_overlay) */
   ci = &c2;
   const VkResult r = real(dev, ci, alloc, out);
   if (r == VK_SUCCESS)
      rv_overlay_swapchain(dev, *out, ci->imageExtent, ci->imageFormat);
   report("roblox-vk: vkCreateSwapchainKHR(%ux%u, %u images, format %d, present mode %d) -> %d", ci->imageExtent.width,
          ci->imageExtent.height, ci->minImageCount, ci->imageFormat, ci->presentMode, r);
   return r;
}
static uint64_t rv_present_ns; /* total time inside the real vkQueuePresentKHR */
/* Freezes: the gap between two presents. Counted per 10 s window by the fps
 * monitor (radv_smoke.c), which also samples the presenting thread's stack
 * while a frame is late. */
static uint64_t rv_last_present_ns, rv_gap_max_ns;
static unsigned rv_gaps_50ms, rv_gaps_250ms;
static pthread_t rv_present_thread;
static int rv_present_thread_set;

static VkResult VKAPI_CALL
rv_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR *info)
{
   static PFN_vkQueuePresentKHR real;
   if (!real) {
      PFN_vkGetInstanceProcAddr g = vk_icdGetInstanceProcAddr;
      real = (PFN_vkQueuePresentKHR)g(rv_instance, "vkQueuePresentKHR");
   }
   while (__atomic_load_n(&rv_quitting, __ATOMIC_ACQUIRE))
      usleep(100 * 1000); /* the title is exiting: no more frames */
   while (__atomic_load_n(&rw_render_pause, __ATOMIC_ACQUIRE))
      usleep(20 * 1000); /* the in-game browser is up (roblox_webview.h) */
   VkPresentInfoKHR mine;
   VkSemaphore overlay_wait;
   info = rv_overlay_draw(q, info, &mine, &overlay_wait);
   const uint64_t pt0 = now_ns();
   const VkResult r = real(q, info);
   const uint64_t pt1 = now_ns();
   __atomic_add_fetch(&rv_present_ns, pt1 - pt0, __ATOMIC_RELAXED);
   const uint64_t prev = __atomic_exchange_n(&rv_last_present_ns, pt1, __ATOMIC_RELAXED);
   if (prev) {
      const uint64_t gap = pt1 - prev;
      if (gap > 50000000ull)
         __atomic_add_fetch(&rv_gaps_50ms, 1, __ATOMIC_RELAXED);
      if (gap > 250000000ull)
         __atomic_add_fetch(&rv_gaps_250ms, 1, __ATOMIC_RELAXED);
      if (gap > __atomic_load_n(&rv_gap_max_ns, __ATOMIC_RELAXED))
         __atomic_store_n(&rv_gap_max_ns, gap, __ATOMIC_RELAXED);
   }
   if (!rv_present_thread_set) {
      rv_present_thread = pthread_self();
      __atomic_store_n(&rv_present_thread_set, 1, __ATOMIC_RELEASE);
   }
   const unsigned n = __atomic_add_fetch(&rv_presents, 1, __ATOMIC_RELAXED);
   if (n == 1) {
      rv_present_t0 = now_ns();
#if defined(__PROSPERO__)
      /* the system's splash screen stays over the title until it is hidden
       * (the demo, which normally does it, has stepped aside) */
      report("roblox-vk: first frame; hide splash screen returned 0x%08x", (unsigned)sceSystemServiceHideSplashScreen());
#endif
   }
   if (n <= 3 || n % 300 == 0)
      report("roblox-vk: frame %u presented -> %d (%.1f fps)", n, r,
             n > 1 ? (n - 1) / ((now_ns() - rv_present_t0) / 1e9) : 0.0);
   return r;
}

/* ---- clean exit ------------------------------------------------------------
 * A title killed while its GPU work is in flight seems to leave its process
 * half torn down: the next launch of this title then fails to load a system
 * library the old process had mapped ("ETXTBSY ... writecount=-1"), until
 * the console restarts. A launcher asks for a clean exit by creating
 * /app0/quit-request; the render thread is parked at its next present, the
 * device is drained, and the process exits by itself. */
static void *
rv_quit_watch(void *arg)
{
   (void)arg;
   for (;;) {
      usleep(500 * 1000);
      if (access("/app0/quit-request", F_OK) != 0)
         continue;
      unlink("/app0/quit-request");
      report("roblox-vk: quit requested: parking presents, draining the GPU");
      __atomic_store_n(&rv_quitting, 1, __ATOMIC_RELEASE);
      usleep(300 * 1000); /* let in-flight submits reach the queue */
      if (rv_device) {
         PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)rv_real(rv_instance, "vkGetDeviceProcAddr");
         PFN_vkDeviceWaitIdle idle = gdpa ? (PFN_vkDeviceWaitIdle)gdpa(rv_device, "vkDeviceWaitIdle") : NULL;
         const uint64_t t = now_ns();
         const VkResult r = idle ? idle(rv_device) : VK_ERROR_UNKNOWN;
         report("roblox-vk: vkDeviceWaitIdle -> %d in %.0f ms; exiting", r, (now_ns() - t) / 1e6);
      }
      ra_close_port(); /* the PS5 audio port (roblox_audio.h) */
      report_sync();
      _exit(0);
   }
   return NULL;
}

/* ---- the entry points ------------------------------------------------------ */
static PFN_vkVoidFunction VKAPI_CALL rv_GetInstanceProcAddr(VkInstance inst, const char *name);

static PFN_vkVoidFunction VKAPI_CALL
rv_GetDeviceProcAddr(VkDevice dev, const char *name)
{
   PFN_vkGetDeviceProcAddr real = (PFN_vkGetDeviceProcAddr)rv_real(rv_instance, "vkGetDeviceProcAddr");
   if (name && strcmp(name, "vkGetDeviceProcAddr") == 0)
      return (PFN_vkVoidFunction)rv_tramp((void *)rv_GetDeviceProcAddr);
   if (name && strcmp(name, "vkCreateSwapchainKHR") == 0)
      return (PFN_vkVoidFunction)rv_tramp((void *)rv_CreateSwapchainKHR);
   if (name && strcmp(name, "vkQueuePresentKHR") == 0)
      return (PFN_vkVoidFunction)rv_tramp((void *)rv_QueuePresentKHR);
   if (name && strcmp(name, "vkGetDeviceQueue") == 0)
      return (PFN_vkVoidFunction)rv_tramp((void *)rv_GetDeviceQueue);
   PFN_vkVoidFunction f = real ? real(dev, name) : NULL;
   if (!f && rv_missing++ < 60)
      report("roblox-vk: device command %s: not available", name ? name : "?");
   return (PFN_vkVoidFunction)rv_tramp((void *)f);
}

static PFN_vkVoidFunction VKAPI_CALL
rv_GetInstanceProcAddr(VkInstance inst, const char *name)
{
   if (!name)
      return NULL;
   static const struct {
      const char *name;
      void *fn;
   } ours[] = {
      {"vkGetInstanceProcAddr", (void *)rv_GetInstanceProcAddr},
      {"vkGetDeviceProcAddr", (void *)rv_GetDeviceProcAddr},
      {"vkEnumerateInstanceExtensionProperties", (void *)rv_EnumerateInstanceExtensionProperties},
      {"vkEnumerateInstanceLayerProperties", (void *)rv_EnumerateInstanceLayerProperties},
      {"vkCreateInstance", (void *)rv_CreateInstance},
      {"vkCreateAndroidSurfaceKHR", (void *)rv_CreateAndroidSurfaceKHR},
      {"vkCreateDevice", (void *)rv_CreateDevice},
      {"vkCreateSwapchainKHR", (void *)rv_CreateSwapchainKHR},
      {"vkQueuePresentKHR", (void *)rv_QueuePresentKHR},
      {"vkGetDeviceQueue", (void *)rv_GetDeviceQueue},
      {"vkGetPhysicalDeviceSurfaceCapabilitiesKHR", (void *)rv_GetPhysicalDeviceSurfaceCapabilitiesKHR},
   };
   for (unsigned i = 0; i < sizeof(ours) / sizeof(ours[0]); i++)
      if (strcmp(name, ours[i].name) == 0)
         return (PFN_vkVoidFunction)rv_tramp(ours[i].fn);
   PFN_vkVoidFunction f = rv_real(inst, name);
   if (!f && rv_missing++ < 60)
      report("roblox-vk: %s: not available", name);
   return (PFN_vkVoidFunction)rv_tramp((void *)f);
}

/* dlsym(dlopen("libvulkan.so"), name) */
static void *
rv_dlsym(const char *name)
{
   static unsigned said;
   if (said++ < 12)
      report("roblox-vk: dlsym(libvulkan.so, %s)", name ? name : "?");
   return (void *)rv_GetInstanceProcAddr(VK_NULL_HANDLE, name);
}

/*
 * PS5 Vulkan - RADV smoke test.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The first program RADV runs on the console (docs/VULKAN_1_4_PLAN.md, Phase 1):
 * an instance and a device through the RADV archive the title links, then four
 * results a working GPU path has to produce exactly -- a buffer fill, a buffer
 * copy, a compute dispatch and a triangle drawn over a clear -- each read back
 * and compared on the CPU. Every line goes to klog and to radv-smoke.txt beside
 * the title (/app0), which FTP reaches.
 *
 * It links no Vulkan loader: every command comes from RADV's
 * vk_icdGetInstanceProcAddr, as a title that links the archive gets them.
 */

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "radv_smoke_shaders.h"

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

#if defined(__PROSPERO__)
#include <sys/stat.h>
#include <unistd.h>

#define RESULTS_PATH "/app0/radv-smoke.txt"
/* libkernel's klog writer: a title's stderr does not reach klog, and klog is
 * what tools/run-title.py watches for the run's end. */
int sceKernelDebugOutText(int channel, const char *text);
/* libkernel: the PS5's own notification pop-up (top left), as homebrew uses
 * it (the PS5 heads these "Debug:", with or without an icon). */
typedef struct ps5_notify_request {
   int32_t type, req_id, priority, msg_id, target_id, user_id, unk1, unk2, app_id, error_num, unk3;
   uint8_t use_icon_uri;
   char message[1024];
   char icon_uri[1024];
   char unk[1027];
} __attribute__((packed)) ps5_notify_request;
_Static_assert(sizeof(ps5_notify_request) == 3120, "notification request is 3120 bytes");
int sceKernelSendNotificationRequest(int, ps5_notify_request *, size_t, int);
static void
ps5_notify(const char *text)
{
   ps5_notify_request req;
   memset(&req, 0, sizeof(req));
   req.target_id = -1;
   req.use_icon_uri = 1;
   snprintf(req.message, sizeof(req.message), "%s", text);
   snprintf(req.icon_uri, sizeof(req.icon_uri), "cxml://psnotification/tex_icon_system");
   sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}
/* libSceSystemService: the launch splash covers the display until hidden. */
int sceSystemServiceHideSplashScreen(void);
/* libScePad / libSceUserService, as ps5-payload-dev SDL's PS5 joystick uses
 * them (src/joystick/ps5/SDL_ps5joystick.h). */
typedef struct DemoPadData {
   uint32_t buttons;
   uint8_t lx, ly, rx, ry, l2, r2;
   uint16_t padding;
   float quat[4], vel[3], acc[3];
   uint8_t touch[24];
   uint8_t connected;
   uint64_t timestamp;
   uint8_t ext[16];
   uint8_t count;
   uint8_t unknown[15];
} DemoPadData;
typedef struct DemoPadColor { uint8_t r, g, b, a; } DemoPadColor;
int scePadInit(void);
int scePadOpen(int user_id, int type, int index, void *param);
int scePadGetHandle(int user_id, int type, int index);
int scePadReadState(int handle, DemoPadData *data);
int scePadSetLightBar(int handle, const DemoPadColor *color);
int sceUserServiceInitialize(void *);
int sceUserServiceGetLoginUserIdList(int user_ids[4]);
/* ps5platform/exec.h: read-write-execute direct memory, outside the GPU window. */
void *ps5_exec_allocate(size_t bytes, uintptr_t anchor);
/* ps5platform/klog.h: the driver's messages on standard error, in klog. */
int ps5_klog_capture_stderr(const char *prefix);
#else
#define RESULTS_PATH "radv-smoke.txt"
#endif

#ifndef MIN2
#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#endif

static FILE *results;
static unsigned passed, failed;

/* Every line goes to standard error (klog), to klog directly and to the
 * flushed results file; on the console that costs about 15 ms a line. The
 * Roblox engine logs from its game and render threads, so writing there made
 * every burst of engine log lines a frozen frame. Once report_async_start()
 * has run, report() only formats the line into a lock-free queue (safe in a
 * signal handler) and one writer thread does the slow writes; a fault path
 * calls report_sync() first so its lines are written before the process
 * ends. Lines that find the queue full are counted, not written. */
static void
report_write(const char *line, int flush)
{
   fprintf(stderr, "[radv-smoke] %s\n", line);
   fflush(stderr);
#if defined(__PROSPERO__)
   char klog_line[600];
   snprintf(klog_line, sizeof(klog_line), "[radv-smoke] %s\n", line);
   sceKernelDebugOutText(0, klog_line);
#endif
   if (results) {
      fprintf(results, "%s\n", line);
      if (flush)
         fflush(results);
   }
}

#define REPORT_QUEUE 4096 /* a power of two */
static struct report_slot {
   uint64_t seq;
   char line[512];
} report_queue[REPORT_QUEUE];
static uint64_t report_head, report_tail;
static int report_async;
static pthread_mutex_t report_writer_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t report_written, report_dropped;

static int
report_enqueue(const char *line)
{
   uint64_t pos = __atomic_load_n(&report_head, __ATOMIC_RELAXED);
   for (;;) {
      struct report_slot *slot = &report_queue[pos & (REPORT_QUEUE - 1)];
      const uint64_t seq = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
      const int64_t dif = (int64_t)(seq - pos);
      if (dif == 0) {
         if (__atomic_compare_exchange_n(&report_head, &pos, pos + 1, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            snprintf(slot->line, sizeof(slot->line), "%s", line);
            __atomic_store_n(&slot->seq, pos + 1, __ATOMIC_RELEASE);
            return 1;
         }
      } else if (dif < 0) {
         return 0; /* full */
      } else {
         pos = __atomic_load_n(&report_head, __ATOMIC_RELAXED);
      }
   }
}

/* Writes the queued lines in order; 0 when the queue is empty (or its next
 * line is still being filled). Under report_writer_lock. */
static int
report_dequeue_one(int flush)
{
   const uint64_t pos = __atomic_load_n(&report_tail, __ATOMIC_RELAXED);
   struct report_slot *slot = &report_queue[pos & (REPORT_QUEUE - 1)];
   if (__atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE) != pos + 1)
      return 0;
   char line[sizeof(slot->line)];
   memcpy(line, slot->line, sizeof(line));
   __atomic_store_n(&slot->seq, pos + REPORT_QUEUE, __ATOMIC_RELEASE);
   __atomic_store_n(&report_tail, pos + 1, __ATOMIC_RELAXED);
   report_write(line, flush);
   __atomic_add_fetch(&report_written, 1, __ATOMIC_RELAXED);
   return 1;
}

static void *
report_writer(void *arg)
{
   (void)arg;
   for (;;) {
      pthread_mutex_lock(&report_writer_lock);
      /* the results file is flushed once per batch, not per line: the
       * engine writes to the same /data, where flushes can stall */
      int n = 0;
      while (n < 64 && report_dequeue_one(0))
         n++;
      if (n && results)
         fflush(results);
      pthread_mutex_unlock(&report_writer_lock);
      if (n == 0)
         usleep(2000);
   }
   return NULL;
}

static void
report_async_start(void)
{
   for (uint64_t i = 0; i < REPORT_QUEUE; i++)
      report_queue[i].seq = i; /* slot i takes line i first */
   pthread_t t;
   if (pthread_create(&t, NULL, report_writer, NULL) == 0) {
      pthread_detach(t);
      __atomic_store_n(&report_async, 1, __ATOMIC_RELEASE);
   }
}

/* Back to direct writes, after writing what is queued: for a fault handler
 * or an exit, so no line is lost. The writer thread may be in the middle of
 * a line; wait for it a short while, then write anyway. */
static void
report_sync(void)
{
   if (!__atomic_exchange_n(&report_async, 0, __ATOMIC_ACQ_REL))
      return;
   int locked = 0;
   for (int i = 0; i < 200 && !(locked = pthread_mutex_trylock(&report_writer_lock) == 0); i++)
      usleep(1000);
   while (report_dequeue_one(1))
      ;
   if (locked)
      pthread_mutex_unlock(&report_writer_lock);
}

static void
report(const char *format, ...)
{
   char line[512];
   va_list args;
   va_start(args, format);
   vsnprintf(line, sizeof(line), format, args);
   va_end(args);
   if (__atomic_load_n(&report_async, __ATOMIC_ACQUIRE)) {
      if (!report_enqueue(line))
         __atomic_add_fetch(&report_dropped, 1, __ATOMIC_RELAXED);
      return;
   }
   report_write(line, 1);
}

static void
check(bool ok, const char *what)
{
   report("%s %s", ok ? "PASS" : "FAIL", what);
   if (ok)
      passed++;
   else
      failed++;
}

/* ------------------------------------------------------------ entry points */

#define INSTANCE_COMMANDS(X)                                                                       \
   X(DestroyInstance)                                                                              \
   X(EnumeratePhysicalDevices)                                                                     \
   X(GetPhysicalDeviceProperties2)                                                                 \
   X(GetPhysicalDeviceQueueFamilyProperties)                                                       \
   X(GetPhysicalDeviceMemoryProperties)                                                            \
   X(GetPhysicalDeviceFeatures)                                                                    \
   X(GetPhysicalDeviceFeatures2)                                                                   \
   X(CreateDevice)                                                                                 \
   X(EnumerateDeviceExtensionProperties)                                                           \
   X(GetDeviceProcAddr)

#define DEVICE_COMMANDS(X)                                                                         \
   X(DestroyDevice)                                                                                \
   X(GetDeviceQueue)                                                                               \
   X(QueueSubmit)                                                                                  \
   X(QueueWaitIdle)                                                                                \
   X(DeviceWaitIdle)                                                                               \
   X(CreateFence)                                                                                  \
   X(DestroyFence)                                                                                 \
   X(WaitForFences)                                                                                \
   X(ResetFences)                                                                                  \
   X(CreateCommandPool)                                                                            \
   X(DestroyCommandPool)                                                                           \
   X(AllocateCommandBuffers)                                                                       \
   X(BeginCommandBuffer)                                                                           \
   X(EndCommandBuffer)                                                                             \
   X(ResetCommandBuffer)                                                                           \
   X(CreateBuffer)                                                                                 \
   X(DestroyBuffer)                                                                                \
   X(GetBufferMemoryRequirements)                                                                  \
   X(CreateImage)                                                                                  \
   X(DestroyImage)                                                                                 \
   X(GetImageMemoryRequirements)                                                                   \
   X(CreateImageView)                                                                              \
   X(DestroyImageView)                                                                             \
   X(AllocateMemory)                                                                               \
   X(FreeMemory)                                                                                   \
   X(BindBufferMemory)                                                                             \
   X(BindImageMemory)                                                                              \
   X(MapMemory)                                                                                    \
   X(CmdFillBuffer)                                                                                \
   X(CmdCopyBuffer)                                                                                \
   X(CmdCopyImageToBuffer)                                                                         \
   X(CmdPipelineBarrier2)                                                                          \
   X(CmdBindPipeline)                                                                              \
   X(CmdBindDescriptorSets)                                                                        \
   X(CmdDispatch)                                                                                  \
   X(CmdPushConstants)                                                                             \
   X(CmdBeginRendering)                                                                            \
   X(CmdEndRendering)                                                                              \
   X(CmdDraw)                                                                                      \
   X(CmdDrawIndexed)                                                                               \
   X(CmdDrawIndirect)                                                                              \
   X(CmdDrawIndexedIndirect)                                                                       \
   X(CmdDrawIndirectCount)                                                                         \
   X(CmdBindIndexBuffer)                                                                           \
   X(CmdSetViewport)                                                                               \
   X(CmdSetScissor)                                                                                \
   X(CreateShaderModule)                                                                           \
   X(DestroyShaderModule)                                                                          \
   X(CreateDescriptorSetLayout)                                                                    \
   X(DestroyDescriptorSetLayout)                                                                   \
   X(CreatePipelineLayout)                                                                         \
   X(DestroyPipelineLayout)                                                                        \
   X(CreateComputePipelines)                                                                       \
   X(CreateGraphicsPipelines)                                                                      \
   X(DestroyPipeline)                                                                              \
   X(CreateDescriptorPool)                                                                         \
   X(DestroyDescriptorPool)                                                                        \
   X(AllocateDescriptorSets)                                                                       \
   X(UpdateDescriptorSets)

/* Loaded when c->mesh. */
#define MESH_COMMANDS(X)                                                                           \
   X(CmdDrawMeshTasksEXT)                                                                          \
   X(CmdDrawMeshTasksIndirectEXT)

#define DECLARE(name) static PFN_vk##name vk##name;
INSTANCE_COMMANDS(DECLARE)
DEVICE_COMMANDS(DECLARE)
MESH_COMMANDS(DECLARE)
static PFN_vkCreateInstance vkCreateInstance;

static bool
load_instance(VkInstance instance)
{
   bool ok = true;
#define LOAD(name)                                                                                 \
   vk##name = (PFN_vk##name)vk_icdGetInstanceProcAddr(instance, "vk" #name);                       \
   if (!vk##name) {                                                                                \
      report("missing vk" #name);                                                                  \
      ok = false;                                                                                  \
   }
   INSTANCE_COMMANDS(LOAD)
#undef LOAD
   return ok;
}

static bool
load_device(VkDevice device)
{
   bool ok = true;
#define LOAD(name)                                                                                 \
   vk##name = (PFN_vk##name)vkGetDeviceProcAddr(device, "vk" #name);                               \
   if (!vk##name) {                                                                                \
      report("missing vk" #name);                                                                  \
      ok = false;                                                                                  \
   }
   DEVICE_COMMANDS(LOAD)
#undef LOAD
   return ok;
}

/* ----------------------------------------------------------------- context */

struct context {
   VkInstance instance;
   VkPhysicalDevice physical;
   VkDevice device;
   VkQueue queue;
   uint32_t family;
   VkPhysicalDeviceMemoryProperties memory;
   VkCommandPool pool;
   VkCommandBuffer cmd;
   VkFence fence;
   /* VK_KHR_fragment_shader_barycentric is reported and enabled. */
   bool barycentric;
   /* VK_KHR_fragment_shading_rate is reported, and its pipeline rate enabled. */
   bool shading_rate;
   /* VK_KHR_acceleration_structure is reported and enabled, with buffer
    * device addresses. */
   bool acceleration_structure;
   /* VK_KHR_display is reported and enabled with VK_KHR_surface, and
    * VK_KHR_swapchain with them. */
   bool display;
   /* VK_EXT_descriptor_buffer is reported and enabled (test_sparse_timing). */
   bool descriptor_buffer;
   /* VK_EXT_mesh_shader is reported and its mesh shaders enabled (test_mesh),
    * and its task shaders when reported (test_task). */
   bool mesh;
   bool task;
   /* How render_readback_with records its draw (test_geometry's draw checks):
    * directly by default. Indices are 16-bit. */
   struct {
      enum {
         DRAW_DIRECT,
         DRAW_INDEXED,
         DRAW_INDIRECT,
         DRAW_INDEXED_INDIRECT,
         DRAW_INDIRECT_COUNT,
         DRAW_MESH,
         DRAW_MESH_INDIRECT,
      } mode;
      /* DRAW_MESH's workgroups. */
      uint32_t groups[3];
      bool restart;
      VkBuffer indices;
      VkBuffer args;
      VkBuffer count;
      uint32_t first_index;
      int32_t vertex_offset;
      uint32_t max_draws;
   } draw;
};

static uint32_t
memory_type(const struct context *c, uint32_t bits, VkMemoryPropertyFlags wanted)
{
   for (uint32_t i = 0; i < c->memory.memoryTypeCount; i++) {
      if ((bits & (1u << i)) && (c->memory.memoryTypes[i].propertyFlags & wanted) == wanted)
         return i;
   }
   return UINT32_MAX;
}

struct buffer {
   VkBuffer buffer;
   VkDeviceMemory memory;
   void *map;
   VkDeviceSize size;
};

static bool
buffer_create(struct context *c, VkDeviceSize size, VkBufferUsageFlags usage, struct buffer *out)
{
   *out = (struct buffer){.size = size};
   const VkBufferCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   if (vkCreateBuffer(c->device, &info, NULL, &out->buffer) != VK_SUCCESS)
      return false;
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(c->device, out->buffer, &req);
   const uint32_t type = memory_type(c, req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   const VkMemoryAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = type,
   };
   if (type == UINT32_MAX || vkAllocateMemory(c->device, &alloc, NULL, &out->memory) != VK_SUCCESS ||
       vkBindBufferMemory(c->device, out->buffer, out->memory, 0) != VK_SUCCESS ||
       vkMapMemory(c->device, out->memory, 0, VK_WHOLE_SIZE, 0, &out->map) != VK_SUCCESS)
      return false;
   return true;
}

static void
buffer_destroy(struct context *c, struct buffer *b)
{
   vkDestroyBuffer(c->device, b->buffer, NULL);
   vkFreeMemory(c->device, b->memory, NULL);
}

static bool
begin(struct context *c)
{
   const VkCommandBufferBeginInfo info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   return vkResetCommandBuffer(c->cmd, 0) == VK_SUCCESS && vkBeginCommandBuffer(c->cmd, &info) == VK_SUCCESS;
}

/* Ends, submits and waits for the command buffer, and makes what the GPU wrote
 * visible to the host's reads. */
static bool
submit_and_wait(struct context *c, const char *what)
{
   const VkMemoryBarrier2 host = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
      .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
   };
   const VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1,
      .pMemoryBarriers = &host,
   };
   vkCmdPipelineBarrier2(c->cmd, &dependency);
   VkResult result = vkEndCommandBuffer(c->cmd);
   if (result != VK_SUCCESS) {
      report("%s: vkEndCommandBuffer returned %d", what, result);
      return false;
   }
   const VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &c->cmd,
   };
   vkResetFences(c->device, 1, &c->fence);
   result = vkQueueSubmit(c->queue, 1, &submit, c->fence);
   if (result != VK_SUCCESS) {
      report("%s: vkQueueSubmit returned %d", what, result);
      return false;
   }
   result = vkWaitForFences(c->device, 1, &c->fence, VK_TRUE, UINT64_C(20000000000));
   if (result != VK_SUCCESS) {
      report("%s: vkWaitForFences returned %d", what, result);
      return false;
   }
   return true;
}

/* ------------------------------------------------------------------- tests */

static void
test_fill(struct context *c)
{
   struct buffer b;
   const VkDeviceSize size = 65536;
   if (!buffer_create(c, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &b)) {
      check(false, "fill: buffer");
      return;
   }
   memset(b.map, 0xcd, size);
   bool ok = begin(c);
   vkCmdFillBuffer(c->cmd, b.buffer, 0, size, 0x12345678u);
   ok = ok && submit_and_wait(c, "fill");
   uint32_t wrong = 0, first_wrong = UINT32_MAX;
   const uint32_t *words = b.map;
   for (uint32_t i = 0; ok && i < size / 4; i++) {
      if (words[i] != 0x12345678u) {
         if (first_wrong == UINT32_MAX)
            first_wrong = i;
         wrong++;
      }
   }
   if (wrong)
      report("fill: %u of %u words wrong, the first at %u reads 0x%08x", wrong, (unsigned)(size / 4), first_wrong,
             words[first_wrong]);
   check(ok && wrong == 0, "fill: vkCmdFillBuffer of 64 KiB reads back exactly");
   buffer_destroy(c, &b);
}

static void
test_copy(struct context *c)
{
   struct buffer src, dst;
   const VkDeviceSize size = 1u << 20;
   if (!buffer_create(c, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &src) ||
       !buffer_create(c, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &dst)) {
      check(false, "copy: buffers");
      return;
   }
   uint32_t *const in = src.map;
   for (uint32_t i = 0; i < size / 4; i++)
      in[i] = i * 2654435761u;
   memset(dst.map, 0, size);
   bool ok = begin(c);
   const VkBufferCopy region = {.srcOffset = 0, .dstOffset = 0, .size = size};
   vkCmdCopyBuffer(c->cmd, src.buffer, dst.buffer, 1, &region);
   ok = ok && submit_and_wait(c, "copy");
   const bool same = ok && memcmp(src.map, dst.map, size) == 0;
   if (ok && !same) {
      const uint32_t *const out = dst.map;
      for (uint32_t i = 0; i < size / 4; i++) {
         if (out[i] != in[i]) {
            report("copy: word %u reads 0x%08x, not 0x%08x", i, out[i], in[i]);
            break;
         }
      }
   }
   check(same, "copy: vkCmdCopyBuffer of 1 MiB reads back exactly");
   buffer_destroy(c, &src);
   buffer_destroy(c, &dst);
}

static VkShaderModule
shader(struct context *c, const uint32_t *code, size_t bytes)
{
   const VkShaderModuleCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = bytes,
      .pCode = code,
   };
   VkShaderModule module = VK_NULL_HANDLE;
   vkCreateShaderModule(c->device, &info, NULL, &module);
   return module;
}

static void
test_compute(struct context *c)
{
   enum { COUNT = 4096 };
   struct buffer b;
   if (!buffer_create(c, COUNT * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &b)) {
      check(false, "compute: buffer");
      return;
   }
   memset(b.map, 0, COUNT * 4);
   const VkDescriptorSetLayoutBinding binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
   };
   const VkDescriptorSetLayoutCreateInfo set_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &binding,
   };
   VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
   VkPipelineLayout layout = VK_NULL_HANDLE;
   VkPipeline pipeline = VK_NULL_HANDLE;
   VkDescriptorPool pool = VK_NULL_HANDLE;
   VkDescriptorSet set = VK_NULL_HANDLE;
   VkShaderModule module = shader(c, radv_smoke_comp, sizeof(radv_smoke_comp));
   bool ok = module && vkCreateDescriptorSetLayout(c->device, &set_info, NULL, &set_layout) == VK_SUCCESS;
   const VkPipelineLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout,
   };
   ok = ok && vkCreatePipelineLayout(c->device, &layout_info, NULL, &layout) == VK_SUCCESS;
   const VkComputePipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage =
         {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module,
            .pName = "main",
         },
      .layout = layout,
   };
   ok = ok && vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline) == VK_SUCCESS;
   check(ok, "compute: SPIR-V compiles to a pipeline on the console");
   const VkDescriptorPoolSize size = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1};
   const VkDescriptorPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &size,
   };
   ok = ok && vkCreateDescriptorPool(c->device, &pool_info, NULL, &pool) == VK_SUCCESS;
   const VkDescriptorSetAllocateInfo set_alloc = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &set_layout,
   };
   ok = ok && vkAllocateDescriptorSets(c->device, &set_alloc, &set) == VK_SUCCESS;
   if (ok) {
      const VkDescriptorBufferInfo buffer_info = {.buffer = b.buffer, .offset = 0, .range = VK_WHOLE_SIZE};
      const VkWriteDescriptorSet write = {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = set,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &buffer_info,
      };
      vkUpdateDescriptorSets(c->device, 1, &write, 0, NULL);
      ok = begin(c);
      vkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(c->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
      vkCmdDispatch(c->cmd, COUNT / 64, 1, 1);
      ok = ok && submit_and_wait(c, "compute");
   }
   uint32_t wrong = 0;
   const uint32_t *const values = b.map;
   for (uint32_t i = 0; ok && i < COUNT; i++) {
      if (values[i] != i * 3u + 7u) {
         if (wrong == 0)
            report("compute: value %u reads %u, not %u", i, values[i], i * 3u + 7u);
         wrong++;
      }
   }
   check(ok && wrong == 0, "compute: 64 workgroups of 64 write every value their index names");
   vkDestroyPipeline(c->device, pipeline, NULL);
   vkDestroyDescriptorPool(c->device, pool, NULL);
   vkDestroyPipelineLayout(c->device, layout, NULL);
   vkDestroyDescriptorSetLayout(c->device, set_layout, NULL);
   vkDestroyShaderModule(c->device, module, NULL);
   buffer_destroy(c, &b);
}

enum { TARGET_SIZE = 256 };

/* Draws vertex_count vertices with the stages into a cleared blue 256-square
 * RGBA8 target and reads the target back into readback. */
static bool
render_readback_with(struct context *c, const char *what, const VkPipelineShaderStageCreateInfo *stages,
                     uint32_t stage_count, VkPrimitiveTopology topology, uint32_t patch_points,
                     uint32_t vertex_count, struct buffer *readback, VkBuffer storage, const void *pipeline_next)
{
   const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
   VkImage image = VK_NULL_HANDLE;
   VkDeviceMemory image_memory = VK_NULL_HANDLE;
   VkImageView view = VK_NULL_HANDLE;
   VkPipelineLayout layout = VK_NULL_HANDLE;
   VkPipeline pipeline = VK_NULL_HANDLE;
   VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
   VkDescriptorPool pool = VK_NULL_HANDLE;
   VkDescriptorSet set = VK_NULL_HANDLE;
   memset(readback->map, 0, TARGET_SIZE * TARGET_SIZE * 4);
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {TARGET_SIZE, TARGET_SIZE, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   bool ok = vkCreateImage(c->device, &image_info, NULL, &image) == VK_SUCCESS;
   if (ok) {
      VkMemoryRequirements req;
      vkGetImageMemoryRequirements(c->device, image, &req);
      const VkMemoryAllocateInfo alloc = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = req.size,
         .memoryTypeIndex = memory_type(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
      };
      ok = vkAllocateMemory(c->device, &alloc, NULL, &image_memory) == VK_SUCCESS &&
           vkBindImageMemory(c->device, image, image_memory, 0) == VK_SUCCESS;
   }
   const VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = format,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   ok = ok && vkCreateImageView(c->device, &view_info, NULL, &view) == VK_SUCCESS;
   if (storage) {
      /* One storage buffer at set 0, binding 0, for every graphics stage. */
      const VkDescriptorSetLayoutBinding binding = {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | (c->mesh ? VK_SHADER_STAGE_MESH_BIT_EXT : 0),
      };
      const VkDescriptorSetLayoutCreateInfo set_info = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
         .bindingCount = 1,
         .pBindings = &binding,
      };
      const VkDescriptorPoolSize size = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1};
      const VkDescriptorPoolCreateInfo pool_info = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
         .maxSets = 1,
         .poolSizeCount = 1,
         .pPoolSizes = &size,
      };
      ok = ok && vkCreateDescriptorSetLayout(c->device, &set_info, NULL, &set_layout) == VK_SUCCESS &&
           vkCreateDescriptorPool(c->device, &pool_info, NULL, &pool) == VK_SUCCESS;
      const VkDescriptorSetAllocateInfo set_alloc = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
         .descriptorPool = pool,
         .descriptorSetCount = 1,
         .pSetLayouts = &set_layout,
      };
      ok = ok && vkAllocateDescriptorSets(c->device, &set_alloc, &set) == VK_SUCCESS;
      if (ok) {
         const VkDescriptorBufferInfo buffer_info = {.buffer = storage, .offset = 0, .range = VK_WHOLE_SIZE};
         const VkWriteDescriptorSet write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &buffer_info,
         };
         vkUpdateDescriptorSets(c->device, 1, &write, 0, NULL);
      }
   }
   const VkPipelineLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = storage ? 1 : 0,
      .pSetLayouts = &set_layout,
   };
   ok = ok && vkCreatePipelineLayout(c->device, &layout_info, NULL, &layout) == VK_SUCCESS;
   const VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   const VkPipelineInputAssemblyStateCreateInfo assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = topology,
      .primitiveRestartEnable = c->draw.restart,
   };
   const VkPipelineTessellationStateCreateInfo tessellation = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
      .patchControlPoints = patch_points,
   };
   const VkPipelineViewportStateCreateInfo viewport_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .scissorCount = 1,
   };
   const VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
   };
   const VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
   };
   const VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask = 0xf};
   const VkPipelineColorBlendStateCreateInfo blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend_attachment,
   };
   const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
   const VkPipelineDynamicStateCreateInfo dynamic = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 2,
      .pDynamicStates = dynamic_states,
   };
   const VkPipelineRenderingCreateInfo rendering_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .pNext = pipeline_next,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &format,
   };
   const VkGraphicsPipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rendering_info,
      .stageCount = stage_count,
      .pStages = stages,
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &assembly,
      .pTessellationState = patch_points ? &tessellation : NULL,
      .pViewportState = &viewport_state,
      .pRasterizationState = &raster,
      .pMultisampleState = &multisample,
      .pColorBlendState = &blend,
      .pDynamicState = &dynamic,
      .layout = layout,
   };
   ok = ok && vkCreateGraphicsPipelines(c->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline) == VK_SUCCESS;
   char label[96];
   snprintf(label, sizeof(label), "%s: a graphics pipeline compiles on the console", what);
   check(ok, label);

   if (ok) {
      ok = begin(c);
      const VkImageMemoryBarrier2 to_attachment = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
         .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .image = image,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      const VkDependencyInfo first = {
         .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
         .imageMemoryBarrierCount = 1,
         .pImageMemoryBarriers = &to_attachment,
      };
      vkCmdPipelineBarrier2(c->cmd, &first);
      const VkRenderingAttachmentInfo attachment = {
         .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
         .imageView = view,
         .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .clearValue = {.color = {.float32 = {0.0f, 0.0f, 1.0f, 1.0f}}},
      };
      const VkRenderingInfo rendering = {
         .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
         .renderArea = {{0, 0}, {TARGET_SIZE, TARGET_SIZE}},
         .layerCount = 1,
         .colorAttachmentCount = 1,
         .pColorAttachments = &attachment,
      };
      vkCmdBeginRendering(c->cmd, &rendering);
      vkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      if (set)
         vkCmdBindDescriptorSets(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
      const VkViewport viewport = {0.0f, 0.0f, (float)TARGET_SIZE, (float)TARGET_SIZE, 0.0f, 1.0f};
      const VkRect2D scissor = {{0, 0}, {TARGET_SIZE, TARGET_SIZE}};
      vkCmdSetViewport(c->cmd, 0, 1, &viewport);
      vkCmdSetScissor(c->cmd, 0, 1, &scissor);
      if (c->draw.indices)
         vkCmdBindIndexBuffer(c->cmd, c->draw.indices, 0, VK_INDEX_TYPE_UINT16);
      switch (c->draw.mode) {
      case DRAW_DIRECT:
         vkCmdDraw(c->cmd, vertex_count, 1, 0, 0);
         break;
      case DRAW_INDEXED:
         vkCmdDrawIndexed(c->cmd, vertex_count, 1, c->draw.first_index, c->draw.vertex_offset, 0);
         break;
      case DRAW_INDIRECT:
         vkCmdDrawIndirect(c->cmd, c->draw.args, 0, c->draw.max_draws, sizeof(VkDrawIndirectCommand));
         break;
      case DRAW_INDEXED_INDIRECT:
         vkCmdDrawIndexedIndirect(c->cmd, c->draw.args, 0, c->draw.max_draws, sizeof(VkDrawIndexedIndirectCommand));
         break;
      case DRAW_INDIRECT_COUNT:
         vkCmdDrawIndirectCount(c->cmd, c->draw.args, 0, c->draw.count, 0, c->draw.max_draws,
                                sizeof(VkDrawIndirectCommand));
         break;
      case DRAW_MESH:
         vkCmdDrawMeshTasksEXT(c->cmd, c->draw.groups[0], c->draw.groups[1], c->draw.groups[2]);
         break;
      case DRAW_MESH_INDIRECT:
         vkCmdDrawMeshTasksIndirectEXT(c->cmd, c->draw.args, 0, c->draw.max_draws,
                                       sizeof(VkDrawMeshTasksIndirectCommandEXT));
         break;
      }
      vkCmdEndRendering(c->cmd);
      const VkImageMemoryBarrier2 to_transfer = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
         .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
         .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
         .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .image = image,
         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      const VkDependencyInfo second = {
         .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
         .imageMemoryBarrierCount = 1,
         .pImageMemoryBarriers = &to_transfer,
      };
      vkCmdPipelineBarrier2(c->cmd, &second);
      const VkBufferImageCopy copy = {
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageExtent = {TARGET_SIZE, TARGET_SIZE, 1},
      };
      vkCmdCopyImageToBuffer(c->cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->buffer, 1, &copy);
      ok = ok && submit_and_wait(c, what);
   }

   vkDestroyPipeline(c->device, pipeline, NULL);
   vkDestroyPipelineLayout(c->device, layout, NULL);
   vkDestroyDescriptorPool(c->device, pool, NULL);
   vkDestroyDescriptorSetLayout(c->device, set_layout, NULL);
   vkDestroyImageView(c->device, view, NULL);
   vkDestroyImage(c->device, image, NULL);
   vkFreeMemory(c->device, image_memory, NULL);
   return ok;
}

static bool
render_readback(struct context *c, const char *what, const VkPipelineShaderStageCreateInfo *stages,
                uint32_t stage_count, VkPrimitiveTopology topology, uint32_t patch_points, uint32_t vertex_count,
                struct buffer *readback)
{
   return render_readback_with(c, what, stages, stage_count, topology, patch_points, vertex_count, readback,
                               VK_NULL_HANDLE, NULL);
}

static VkPipelineShaderStageCreateInfo
stage_info(VkShaderStageFlagBits stage, VkShaderModule module)
{
   return (VkPipelineShaderStageCreateInfo){
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = stage,
      .module = module,
      .pName = "main",
   };
}

static void
test_triangle(struct context *c)
{
   struct buffer readback;
   if (!buffer_create(c, TARGET_SIZE * TARGET_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback)) {
      check(false, "triangle: readback buffer");
      return;
   }
   VkShaderModule vert = shader(c, radv_smoke_vert, sizeof(radv_smoke_vert));
   VkShaderModule frag = shader(c, radv_smoke_frag, sizeof(radv_smoke_frag));
   const VkPipelineShaderStageCreateInfo stages[2] = {
      stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
      stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
   };
   bool ok = vert && frag &&
             render_readback(c, "triangle", stages, 2, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 3, &readback);

   /* The triangle covers x/128 + y/256 < 1 (Vulkan's y runs down the target);
    * every other texel keeps the clear. */
   uint32_t wrong = 0;
   const uint32_t red = 0xff0000ffu, blue = 0xffff0000u;
   const uint32_t *const texels = readback.map;
   for (uint32_t y = 0; ok && y < TARGET_SIZE; y++) {
      for (uint32_t x = 0; x < TARGET_SIZE; x++) {
         const double edge = (x + 0.5) / 128.0 + (y + 0.5) / 256.0;
         /* Texels whose centre is within half a texel of the edge may go
          * either way. */
         if (edge > 0.995 && edge < 1.005)
            continue;
         const uint32_t expected = edge < 1.0 ? red : blue;
         if (texels[y * TARGET_SIZE + x] != expected) {
            if (wrong < 4)
               report("triangle: texel (%u, %u) reads 0x%08x, not 0x%08x", x, y, texels[y * TARGET_SIZE + x],
                      expected);
            wrong++;
         }
      }
   }
   if (wrong)
      report("triangle: %u texels wrong", wrong);
   check(ok && wrong == 0, "triangle: a clear and a draw read back texel for texel");

   vkDestroyShaderModule(c->device, vert, NULL);
   vkDestroyShaderModule(c->device, frag, NULL);
   buffer_destroy(c, &readback);
}

/* The coherence probe (docs/VULKAN_1_4_PLAN.md, S7) on the memory the driver
 * maps for the host, with no flush and no invalidate between the CPU and the
 * GPU, which is what HOST_COHERENT promises. Each part keeps the other
 * direction out of its way:
 * - the CPU writes a source the GPU then copies at once, into a region the CPU
 *   has never touched, so its lines are still in the CPU's caches and the
 *   destination's are not;
 * - the GPU fills a region the CPU has just read, so the CPU's caches hold the
 *   old words;
 * and the CPU's reads and writes of mapped memory are timed against its own
 * heap's, which says whether the mapping is cached. */
static uint64_t
now_ns(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static uint64_t coherence_sink;

static uint64_t
read_ns(const void *memory, size_t bytes)
{
   const uint64_t *words = memory;
   uint64_t sum = 0;
   const uint64_t start = now_ns();
   for (size_t i = 0; i < bytes / 8; i++)
      sum += words[i];
   const uint64_t end = now_ns();
   coherence_sink += sum;
   return end - start;
}

static uint64_t
write_ns(void *memory, size_t bytes)
{
   uint64_t *words = memory;
   const uint64_t start = now_ns();
   for (size_t i = 0; i < bytes / 8; i++)
      words[i] = i;
   return now_ns() - start;
}

static void
test_coherence(struct context *c)
{
   enum { ROUNDS = 64, PIECE = 4096, STRIDE = 65536 };
   struct buffer src, dst;
   if (!buffer_create(c, PIECE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &src) ||
       !buffer_create(c, (VkDeviceSize)ROUNDS * STRIDE, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &dst)) {
      check(false, "coherence: buffers");
      return;
   }

   /* The CPU's writes, read by the GPU. */
   unsigned stale_rounds = 0, stale_words = 0;
   bool ok = true;
   for (uint32_t round = 0; ok && round < ROUNDS; round++) {
      uint32_t *const in = src.map;
      for (uint32_t i = 0; i < PIECE / 4; i++)
         in[i] = (round + 1) * 0x9e3779b9u ^ i;
      ok = begin(c);
      const VkBufferCopy region = {.srcOffset = 0, .dstOffset = (VkDeviceSize)round * STRIDE, .size = PIECE};
      vkCmdCopyBuffer(c->cmd, src.buffer, dst.buffer, 1, &region);
      ok = ok && submit_and_wait(c, "coherence");
      const uint32_t *const out = (const uint32_t *)((const uint8_t *)dst.map + (size_t)round * STRIDE);
      unsigned wrong = 0;
      for (uint32_t i = 0; ok && i < PIECE / 4; i++)
         wrong += out[i] != ((round + 1) * 0x9e3779b9u ^ i);
      stale_rounds += wrong != 0;
      stale_words += wrong;
   }
   if (stale_rounds)
      report("coherence: %u of %u rounds read stale source words (%u words)", stale_rounds, ROUNDS, stale_words);
   check(ok && stale_rounds == 0, "coherence: the GPU reads what the CPU just wrote, without a flush");

   /* The GPU's writes over lines the CPU holds. */
   stale_rounds = 0;
   stale_words = 0;
   for (uint32_t round = 0; ok && round < ROUNDS; round++) {
      const uint32_t *const words = (const uint32_t *)((const uint8_t *)dst.map + (size_t)round * STRIDE);
      uint32_t held = 0;
      for (uint32_t i = 0; i < PIECE / 4; i++)
         held += words[i];
      coherence_sink += held;
      ok = begin(c);
      vkCmdFillBuffer(c->cmd, dst.buffer, (VkDeviceSize)round * STRIDE, PIECE, 0xa5a50000u | round);
      ok = ok && submit_and_wait(c, "coherence");
      unsigned wrong = 0;
      for (uint32_t i = 0; ok && i < PIECE / 4; i++)
         wrong += words[i] != (0xa5a50000u | round);
      stale_rounds += wrong != 0;
      stale_words += wrong;
   }
   if (stale_rounds)
      report("coherence: %u of %u rounds read stale words the CPU held (%u words)", stale_rounds, ROUNDS,
             stale_words);
   check(ok && stale_rounds == 0, "coherence: the CPU reads what the GPU wrote over lines it held, without an "
                                  "invalidate");
   buffer_destroy(c, &src);
   buffer_destroy(c, &dst);

   /* Cached or not: 16 MiB read and written, mapped and on the heap, the best
    * of three each. */
   enum { TIMED = 16 << 20 };
   struct buffer timed;
   void *const heap = malloc(TIMED);
   if (heap && buffer_create(c, TIMED, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &timed)) {
      uint64_t mapped_read = UINT64_MAX, heap_read = UINT64_MAX, mapped_write = UINT64_MAX,
               heap_write = UINT64_MAX;
      for (int i = 0; i < 3; i++) {
         mapped_write = MIN2(mapped_write, write_ns(timed.map, TIMED));
         heap_write = MIN2(heap_write, write_ns(heap, TIMED));
         mapped_read = MIN2(mapped_read, read_ns(timed.map, TIMED));
         heap_read = MIN2(heap_read, read_ns(heap, TIMED));
      }
      report("coherence: 16 MiB read in %.2f ms mapped, %.2f ms on the heap; written in %.2f ms mapped, %.2f ms "
             "on the heap",
             mapped_read / 1e6, heap_read / 1e6, mapped_write / 1e6, heap_write / 1e6);
      buffer_destroy(c, &timed);
   }
   free(heap);
}

/* The triangle of test_triangle, shaded by a fragment shader whose private
 * array of n floats (a specialisation constant) is written and read at
 * indices that depend on the texel: past a few dozen floats the compiler keeps
 * it in scratch memory. Each texel's colour is the array's sum. */
static void
test_scratch(struct context *c)
{
   struct buffer readback;
   if (!buffer_create(c, TARGET_SIZE * TARGET_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback)) {
      check(false, "scratch: readback buffer");
      return;
   }
   VkShaderModule vert = shader(c, radv_smoke_vert, sizeof(radv_smoke_vert));
   VkShaderModule frag = shader(c, radv_smoke_scratch_frag, sizeof(radv_smoke_scratch_frag));
   static const int32_t sizes[] = {16, 64, 256, 1024};
   for (unsigned v = 0; v < sizeof(sizes) / sizeof(sizes[0]); v++) {
      const int32_t n = sizes[v];
      const VkSpecializationMapEntry entry = {.constantID = 0, .offset = 0, .size = sizeof(n)};
      const VkSpecializationInfo specialisation = {
         .mapEntryCount = 1,
         .pMapEntries = &entry,
         .dataSize = sizeof(n),
         .pData = &n,
      };
      VkPipelineShaderStageCreateInfo stages[2] = {
         stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
         stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
      };
      stages[1].pSpecializationInfo = &specialisation;
      char what[64];
      snprintf(what, sizeof(what), "scratch with %d floats", (int)n);
      bool ok = vert && frag &&
                render_readback(c, what, stages, 2, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 3, &readback);
      uint32_t wrong = 0;
      const uint32_t blue = 0xffff0000u;
      const uint32_t *const texels = readback.map;
      for (uint32_t y = 0; ok && y < TARGET_SIZE; y++) {
         for (uint32_t x = 0; x < TARGET_SIZE; x++) {
            const double edge = (x + 0.5) / 128.0 + (y + 0.5) / 256.0;
            if (edge > 0.995 && edge < 1.005)
               continue;
            uint32_t sum = 0;
            for (int32_t i = 0; i < n; i++)
               sum += (uint32_t)((i * 7 + (int32_t)x) & 15);
            const uint32_t expected =
               edge < 1.0 ? (sum & 0xffu) | (((sum >> 8) & 0xffu) << 8) | 0xff000000u : blue;
            if (texels[y * TARGET_SIZE + x] != expected) {
               if (wrong < 4)
                  report("%s: texel (%u, %u) reads 0x%08x, not 0x%08x", what, x, y, texels[y * TARGET_SIZE + x],
                         expected);
               wrong++;
            }
         }
      }
      if (wrong)
         report("%s: %u texels wrong", what, wrong);
      char description[96];
      snprintf(description, sizeof(description), "%s: every texel has its array's sum", what);
      check(ok && wrong == 0, description);
   }
   vkDestroyShaderModule(c->device, vert, NULL);
   vkDestroyShaderModule(c->device, frag, NULL);
   buffer_destroy(c, &readback);
}

/* test_triangle's triangle, whose vertices name themselves 1, 2 and 3; the
 * fragment shader writes the three values it reads per vertex as its red,
 * green and blue bytes. With the first vertex provoking, vertex_id[i] is
 * vertex i's. */
static void
test_barycentric(struct context *c)
{
   struct buffer readback;
   if (!buffer_create(c, TARGET_SIZE * TARGET_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback)) {
      check(false, "barycentric: readback buffer");
      return;
   }
   VkShaderModule vert = shader(c, radv_smoke_bary_vert, sizeof(radv_smoke_bary_vert));
   VkShaderModule frag = shader(c, radv_smoke_bary_frag, sizeof(radv_smoke_bary_frag));
   for (uint32_t winding = 0; winding < 2; winding++) {
   const VkBool32 other_winding = winding;
   const VkSpecializationMapEntry entry = {.constantID = 0, .offset = 0, .size = sizeof(other_winding)};
   const VkSpecializationInfo specialisation = {
      .mapEntryCount = 1, .pMapEntries = &entry, .dataSize = sizeof(other_winding), .pData = &other_winding};
   VkPipelineShaderStageCreateInfo stages[2] = {
      stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
      stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
   };
   stages[0].pSpecializationInfo = &specialisation;
   const char *const what = winding ? "barycentric, other winding" : "barycentric";
   bool ok = vert && frag &&
             render_readback(c, what, stages, 2, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 3, &readback);
   uint32_t wrong = 0;
   const uint32_t expected_inside = 0xff030201u, blue = 0xffff0000u;
   const uint32_t *const texels = readback.map;
   for (uint32_t y = 0; ok && y < TARGET_SIZE; y++) {
      for (uint32_t x = 0; x < TARGET_SIZE; x++) {
         const double edge = (x + 0.5) / 128.0 + (y + 0.5) / 256.0;
         if (edge > 0.995 && edge < 1.005)
            continue;
         const uint32_t expected = edge < 1.0 ? expected_inside : blue;
         if (texels[y * TARGET_SIZE + x] != expected) {
            if (wrong < 4)
               report("%s: texel (%u, %u) reads 0x%08x, not 0x%08x", what, x, y, texels[y * TARGET_SIZE + x],
                      expected);
            wrong++;
         }
      }
   }
   if (wrong)
      report("%s: %u texels wrong", what, wrong);
   char description[128];
   snprintf(description, sizeof(description), "%s: each vertex's value read per vertex, in the triangle's vertex order",
            what);
   check(ok && wrong == 0, description);
   }
   vkDestroyShaderModule(c->device, vert, NULL);
   vkDestroyShaderModule(c->device, frag, NULL);
   buffer_destroy(c, &readback);
}

/* dEQP-VK.fragment_shading_barycentric's triangle list: two triangles over
 * the whole target, split along the diagonal, the right-hand corners at
 * w = 16; vertex i names itself i + 1 and the fragment shader writes the
 * three values it reads per vertex as its red, green and blue bytes. */
static void
test_barycentric_pair(struct context *c)
{
   struct buffer readback;
   if (!buffer_create(c, TARGET_SIZE * TARGET_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback)) {
      check(false, "barycentric pair: readback buffer");
      return;
   }
   VkShaderModule vert = shader(c, radv_smoke_bary6_vert, sizeof(radv_smoke_bary6_vert));
   VkShaderModule frag = shader(c, radv_smoke_bary_frag, sizeof(radv_smoke_bary_frag));
   const VkPipelineShaderStageCreateInfo stages[2] = {
      stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
      stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
   };
   bool ok = vert && frag &&
             render_readback(c, "barycentric pair", stages, 2, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 6, &readback);
   uint32_t wrong = 0;
   const uint32_t *const texels = readback.map;
   for (uint32_t y = 0; ok && y < TARGET_SIZE; y++) {
      for (uint32_t x = 0; x < TARGET_SIZE; x++) {
         if (x == y || x + 1 == y || y + 1 == x)
            continue;
         /* Below the diagonal: the first triangle (vertices 1, 2, 3). */
         const uint32_t expected = x < y ? 0xff030201u : 0xff060504u;
         if (texels[y * TARGET_SIZE + x] != expected) {
            if (wrong < 4)
               report("barycentric pair: texel (%u, %u) reads 0x%08x, not 0x%08x", x, y,
                      texels[y * TARGET_SIZE + x], expected);
            wrong++;
         }
      }
   }
   if (wrong)
      report("barycentric pair: %u texels wrong", wrong);
   check(ok && wrong == 0, "barycentric pair: each triangle's values read per vertex, in its vertex order");
   vkDestroyShaderModule(c->device, vert, NULL);
   vkDestroyShaderModule(c->device, frag, NULL);
   buffer_destroy(c, &readback);
}

/* A pipeline fragment shading rate over a triangle covering the target: at
 * 2x2 the fragment shader runs once for each 2x2 block of texels and reports
 * the rate (gl_ShadingRateEXT 5, 2 pixels each way), at 1x1 once for each
 * texel. Every texel is written either way. */
static void
test_shading_rate(struct context *c)
{
   struct buffer readback, counts;
   if (!buffer_create(c, TARGET_SIZE * TARGET_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback)) {
      check(false, "shading rate: readback buffer");
      return;
   }
   if (!buffer_create(c, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &counts)) {
      check(false, "shading rate: count buffer");
      buffer_destroy(c, &readback);
      return;
   }
   VkShaderModule vert = shader(c, radv_smoke_full_vert, sizeof(radv_smoke_full_vert));
   VkShaderModule frag = shader(c, radv_smoke_rate_frag, sizeof(radv_smoke_rate_frag));
   const VkPipelineShaderStageCreateInfo stages[2] = {
      stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
      stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
   };
   static const struct {
      uint32_t size, rate;
   } rates[2] = {{2, 5}, {1, 0}};
   for (unsigned r = 0; r < 2; r++) {
      const uint32_t size = rates[r].size;
      const VkPipelineFragmentShadingRateStateCreateInfoKHR rate_state = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR,
         .fragmentSize = {size, size},
         .combinerOps = {VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR},
      };
      char what[64];
      snprintf(what, sizeof(what), "shading rate %ux%u", size, size);
      memset(counts.map, 0, 4096);
      const bool ok = vert && frag &&
                      render_readback_with(c, what, stages, 2, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 3, &readback,
                                           counts.buffer, &rate_state);
      const uint32_t *const texels = readback.map;
      const uint32_t *const words = counts.map;
      unsigned wrong = 0;
      for (unsigned i = 0; ok && i < TARGET_SIZE * TARGET_SIZE; i++)
         wrong += texels[i] != 0xff0000ffu;
      const uint32_t expected = TARGET_SIZE * TARGET_SIZE / (size * size);
      report("%s: %u invocations (%u expected), rates seen 0x%x (0x%x expected), %u texels wrong", what,
             ok ? words[0] : 0, expected, ok ? words[1] : 0, 1u << rates[r].rate, wrong);
      char description[128];
      snprintf(description, sizeof(description),
               "%s: the fragment shader runs once for each %ux%u block and reports the rate", what, size, size);
      check(ok && wrong == 0 && words[0] == expected && words[1] == 1u << rates[r].rate, description);
   }
   vkDestroyShaderModule(c->device, vert, NULL);
   vkDestroyShaderModule(c->device, frag, NULL);
   buffer_destroy(c, &counts);
   buffer_destroy(c, &readback);
}

/* The primitive ID a fragment shader reads when no earlier stage writes it:
 * the vertex shader (NGG) exports it. Two triangles split the target, the
 * left one primitive 0 (red), the right one primitive 1 (green). */
static void
test_primitive_id(struct context *c)
{
   struct buffer readback;
   if (!buffer_create(c, TARGET_SIZE * TARGET_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback)) {
      check(false, "primitive ID: readback buffer");
      return;
   }
   VkShaderModule vert = shader(c, radv_smoke_prim_id_vert, sizeof(radv_smoke_prim_id_vert));
   VkShaderModule frag = shader(c, radv_smoke_prim_id_frag, sizeof(radv_smoke_prim_id_frag));
   const VkPipelineShaderStageCreateInfo stages[2] = {
      stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
      stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
   };
   const bool ok = vert && frag &&
                   render_readback(c, "primitive ID", stages, 2, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 6, &readback);
   const uint32_t *const texels = readback.map;
   unsigned left_red = 0, left_green = 0, right_red = 0, right_green = 0, other = 0;
   for (unsigned y = 0; ok && y < TARGET_SIZE; y++) {
      for (unsigned x = 0; x < TARGET_SIZE; x++) {
         const uint32_t t = texels[y * TARGET_SIZE + x];
         const bool left = x < TARGET_SIZE / 2;
         if (t == 0xff0000ffu)
            left ? left_red++ : right_red++;
         else if (t == 0xff00ff00u)
            left ? left_green++ : right_green++;
         else
            other++;
      }
   }
   report("primitive ID: left %u red %u green, right %u red %u green, %u other", left_red, left_green, right_red,
          right_green, other);
   if (getenv("RADV_SMOKE_PRIM_ID_RAW")) {
      /* Diagnostic: the raw IDs each side read. */
      VkShaderModule raw = shader(c, radv_smoke_prim_id_raw_frag, sizeof(radv_smoke_prim_id_raw_frag));
      const VkPipelineShaderStageCreateInfo raw_stages[2] = {
         stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
         stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, raw),
      };
      if (raw && render_readback(c, "primitive ID raw", raw_stages, 2, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 6,
                                 &readback)) {
         const unsigned y = TARGET_SIZE / 2;
         report("primitive ID raw: left 0x%08x, right 0x%08x, top-left 0x%08x, bottom-right 0x%08x",
                texels[y * TARGET_SIZE + TARGET_SIZE / 4], texels[y * TARGET_SIZE + 3 * TARGET_SIZE / 4],
                texels[TARGET_SIZE / 8 * TARGET_SIZE + TARGET_SIZE / 8],
                texels[(TARGET_SIZE - 1) * TARGET_SIZE + TARGET_SIZE - 1]);
      }
      vkDestroyShaderModule(c->device, raw, NULL);
   }
   const unsigned half = TARGET_SIZE * TARGET_SIZE / 2;
   check(ok && left_red == half && right_green == half,
         "primitive ID: the fragment shader reads each triangle's own ID without a stage writing it");
   vkDestroyShaderModule(c->device, vert, NULL);
   vkDestroyShaderModule(c->device, frag, NULL);
   buffer_destroy(c, &readback);
}

static void *
thread_mxcsr(void *out)
{
   unsigned mxcsr;
   __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
   *(unsigned *)out = mxcsr;
   return NULL;
}

/* The title's startup sets the IEEE state (the console starts a title with
 * flush-to-zero and denormals-are-zero), and a new thread starts with its
 * creator's: exception flags aside, MXCSR reads 0x1f80 in both. */
static void
test_fp_state(void)
{
   unsigned mxcsr = 0, thread_value = 0;
   __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
   pthread_t thread;
   const bool made = pthread_create(&thread, NULL, thread_mxcsr, &thread_value) == 0 &&
                     pthread_join(thread, NULL) == 0;
   report("fp state: main 0x%x, a new thread 0x%x", mxcsr, thread_value);
   check((mxcsr & ~0x3fu) == 0x1f80 && made && (thread_value & ~0x3fu) == 0x1f80,
         "fp state: main and a new thread run with IEEE denormals (MXCSR 0x1f80)");
}

#if defined(__PROSPERO__)
/* A title's sandbox refuses access() for every path; the platform's
 * ps5_access, bound in its place, answers from stat() and open(). RADV's
 * shader cache makes its folders in the title's own, open to FTP (0777). */
static void
test_title_files(void)
{
   const bool found = access("/app0", F_OK) == 0 && access(RESULTS_PATH, R_OK | W_OK) == 0;
   errno = 0;
   const bool missing = access("/app0/no-such-file", F_OK) == -1 && errno == ENOENT;
   check(found && missing, "files: access answers in the title's folder");
   struct stat status;
   const bool cache = stat("/app0/radv-shader-cache/mesa_shader_cache_db", &status) == 0 &&
                      S_ISDIR(status.st_mode) && (status.st_mode & 0777) == 0777;
   check(cache, "files: RADV's shader cache is in /app0/radv-shader-cache, open to FTP");
}
#endif

/* One quad patch over the whole target, its evaluation positions taken from
 * the tessellation coordinates alone or from the control points the control
 * shader wrote (which travel through the off-chip ring). */
static void
test_tessellation(struct context *c)
{
   struct buffer readback;
   if (!buffer_create(c, TARGET_SIZE * TARGET_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback)) {
      check(false, "tessellation: readback buffer");
      return;
   }
   VkShaderModule vert = shader(c, radv_smoke_tess_vert, sizeof(radv_smoke_tess_vert));
   VkShaderModule tesc = shader(c, radv_smoke_tess_tesc, sizeof(radv_smoke_tess_tesc));
   VkShaderModule from_coord = shader(c, radv_smoke_tess_coord_tese, sizeof(radv_smoke_tess_coord_tese));
   VkShaderModule from_patch = shader(c, radv_smoke_tess_patch_tese, sizeof(radv_smoke_tess_patch_tese));
   VkShaderModule frag = shader(c, radv_smoke_frag, sizeof(radv_smoke_frag));
   const struct {
      const char *what;
      VkShaderModule tese;
   } variants[2] = {
      {"tessellation from coordinates", from_coord},
      {"tessellation from control points", from_patch},
   };
   for (unsigned v = 0; v < 2; v++) {
      const VkPipelineShaderStageCreateInfo stages[4] = {
         stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
         stage_info(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, tesc),
         stage_info(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, variants[v].tese),
         stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
      };
      const bool ok = vert && tesc && variants[v].tese && frag &&
                      render_readback(c, variants[v].what, stages, 4, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST, 4, 4,
                                      &readback);
      uint32_t red = 0;
      const uint32_t *const texels = readback.map;
      for (uint32_t i = 0; ok && i < TARGET_SIZE * TARGET_SIZE; i++)
         red += texels[i] == 0xff0000ffu;
      char label[96];
      snprintf(label, sizeof(label), "%s: the patch covers the target (%u of %u texels)", variants[v].what, red,
               TARGET_SIZE * TARGET_SIZE);
      check(ok && red == TARGET_SIZE * TARGET_SIZE, label);
   }

   /* The CTS's shape: generic control points, nine-by-nine, and a colour from
    * the evaluation shader. */
   VkShaderModule varying_vert = shader(c, radv_smoke_tess_varying_vert, sizeof(radv_smoke_tess_varying_vert));
   VkShaderModule varying_tesc = shader(c, radv_smoke_tess_varying_tesc, sizeof(radv_smoke_tess_varying_tesc));
   VkShaderModule varying_tese = shader(c, radv_smoke_tess_varying_tese, sizeof(radv_smoke_tess_varying_tese));
   VkShaderModule colour_frag = shader(c, radv_smoke_colour_frag, sizeof(radv_smoke_colour_frag));
   const struct {
      const char *what;
      VkShaderModule tesc, frag;
   } shapes[1] = {
      {"tessellation with varyings", varying_tesc, colour_frag},
   };
   for (unsigned v = 0; v < 1; v++) {
      const VkPipelineShaderStageCreateInfo stages[4] = {
         stage_info(VK_SHADER_STAGE_VERTEX_BIT, varying_vert),
         stage_info(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, shapes[v].tesc),
         stage_info(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, varying_tese),
         stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, shapes[v].frag),
      };
      const char *const what = shapes[v].what;
      const bool ok = varying_vert && shapes[v].tesc && varying_tese && shapes[v].frag &&
                      render_readback(c, what, stages, 4, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST, 4, 4, &readback);
      uint32_t red = 0, blue = 0;
      const uint32_t *const texels = readback.map;
      for (uint32_t i = 0; ok && i < TARGET_SIZE * TARGET_SIZE; i++) {
         red += texels[i] == 0xff0000ffu;
         blue += texels[i] == 0xffff0000u;
      }
      if (ok && red != TARGET_SIZE * TARGET_SIZE)
         report("%s: %u red, %u blue, texel (128, 128) reads 0x%08x", what, red, blue,
                texels[128 * TARGET_SIZE + 128]);
      char label[96];
      snprintf(label, sizeof(label), "%s: the patch covers the target in its colour", what);
      check(ok && red == TARGET_SIZE * TARGET_SIZE, label);
   }
   /* Levels 2 to 9: NGG culling once dropped every patch above level 2. */
   VkShaderModule level_tesc = shader(c, radv_smoke_tess_level_tesc, sizeof(radv_smoke_tess_level_tesc));
   uint32_t levels_covered = 0;
   for (unsigned level = 2; level <= 9; level++) {
      const float value = (float)level;
      const VkSpecializationMapEntry entry = {.constantID = 0, .offset = 0, .size = sizeof(float)};
      const VkSpecializationInfo special = {
         .mapEntryCount = 1, .pMapEntries = &entry, .dataSize = sizeof(float), .pData = &value};
      VkPipelineShaderStageCreateInfo stages[4] = {
         stage_info(VK_SHADER_STAGE_VERTEX_BIT, varying_vert),
         stage_info(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, level_tesc),
         stage_info(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, varying_tese),
         stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
      };
      stages[1].pSpecializationInfo = &special;
      const bool ok =
         render_readback(c, "tessellation levels", stages, 4, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST, 4, 4, &readback);
      uint32_t red = 0;
      const uint32_t *const texels = readback.map;
      for (uint32_t i = 0; ok && i < TARGET_SIZE * TARGET_SIZE; i++)
         red += texels[i] == 0xff0000ffu;
      if (red == TARGET_SIZE * TARGET_SIZE)
         levels_covered++;
      else
         report("tessellation level %u: %u of %u texels red", level, red, TARGET_SIZE * TARGET_SIZE);
   }
   check(levels_covered == 8, "tessellation levels 2 to 9: each patch covers the target");
   vkDestroyShaderModule(c->device, level_tesc, NULL);
   vkDestroyShaderModule(c->device, varying_vert, NULL);
   vkDestroyShaderModule(c->device, varying_tesc, NULL);
   vkDestroyShaderModule(c->device, varying_tese, NULL);
   vkDestroyShaderModule(c->device, colour_frag, NULL);
   vkDestroyShaderModule(c->device, vert, NULL);
   vkDestroyShaderModule(c->device, tesc, NULL);
   vkDestroyShaderModule(c->device, from_coord, NULL);
   vkDestroyShaderModule(c->device, from_patch, NULL);
   vkDestroyShaderModule(c->device, frag, NULL);
   buffer_destroy(c, &readback);
}

/* The rows of four a readback covers entirely in red, as a mask. */
static unsigned
rows_red(const struct buffer *readback)
{
   const uint32_t *const texels = readback->map;
   unsigned rows = 0;
   for (unsigned row = 0; row < 4; row++) {
      bool all = true;
      for (uint32_t i = row * TARGET_SIZE * TARGET_SIZE / 4; all && i < (row + 1) * TARGET_SIZE * TARGET_SIZE / 4; i++)
         all = texels[i] == 0xff0000ffu;
      rows |= (unsigned)all << row;
   }
   return rows;
}

/* Draws with a geometry shader and the recording c->draw names; the rows
 * covered, or ~0u when the draw failed. */
static unsigned
draw_rows(struct context *c, VkShaderModule vert, VkShaderModule geom, VkShaderModule frag,
          VkPrimitiveTopology topology, uint32_t vertex_count, struct buffer *readback)
{
   const VkPipelineShaderStageCreateInfo stages[3] = {
      stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
      stage_info(VK_SHADER_STAGE_GEOMETRY_BIT, geom),
      stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
   };
   return render_readback(c, "geometry draws", stages, 3, topology, 0, vertex_count, readback) ? rows_red(readback)
                                                                                               : ~0u;
}

/* Geometry shader draws whose indices or counts live in memory, and
 * triangle strips cut by primitive restart: each covers the rows it names. */
static void
test_geometry_draws(struct context *c, struct buffer *readback, VkShaderModule points, VkShaderModule frag)
{
   struct buffer indices, args, count;
   if (!buffer_create(c, 256, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &indices) ||
       !buffer_create(c, 256, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, &args) ||
       !buffer_create(c, 16, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, &count)) {
      check(false, "geometry draws: buffers");
      return;
   }
   VkShaderModule strip = shader(c, radv_smoke_gs_16_geom, sizeof(radv_smoke_gs_16_geom));
   VkShaderModule quads = shader(c, radv_smoke_gs_quads_vert, sizeof(radv_smoke_gs_quads_vert));
   VkShaderModule triangles = shader(c, radv_smoke_gs_triangles_geom, sizeof(radv_smoke_gs_triangles_geom));
   uint16_t *const index = indices.map;
   uint32_t *const word = args.map;
   uint32_t *const counted = count.map;
   unsigned rows;

   /* Points 1 and 3: indices 0 and 2 after the first, offset by one. */
   index[0] = 9;
   index[1] = 0;
   index[2] = 2;
   c->draw = (__typeof__(c->draw)){.mode = DRAW_INDEXED, .indices = indices.buffer, .first_index = 1, .vertex_offset = 1};
   rows = draw_rows(c, points, strip, frag, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, 2, readback);
   if (rows != 0xa)
      report("geometry draws: an indexed draw covered rows 0x%x", rows);
   check(rows == 0xa, "geometry: an indexed draw with a vertex offset draws the points its indices name");

   const VkDrawIndexedIndirectCommand indexed_args = {2, 1, 1, 1, 0};
   memcpy(word, &indexed_args, sizeof(indexed_args));
   c->draw = (__typeof__(c->draw)){.mode = DRAW_INDEXED_INDIRECT, .indices = indices.buffer, .args = args.buffer,
                                   .max_draws = 1};
   rows = draw_rows(c, points, strip, frag, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, 0, readback);
   if (rows != 0xa)
      report("geometry draws: an indexed indirect draw covered rows 0x%x", rows);
   check(rows == 0xa, "geometry: an indexed indirect draw draws the points its indices name");

   const VkDrawIndirectCommand indirect_args = {2, 1, 1, 0};
   memcpy(word, &indirect_args, sizeof(indirect_args));
   c->draw = (__typeof__(c->draw)){.mode = DRAW_INDIRECT, .args = args.buffer, .max_draws = 1};
   rows = draw_rows(c, points, strip, frag, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, 0, readback);
   if (rows != 0x6)
      report("geometry draws: an indirect draw covered rows 0x%x", rows);
   check(rows == 0x6, "geometry: an indirect draw draws the points its arguments name");

   /* Two draws in memory, point 0 and point 3, and a count of one. */
   const VkDrawIndirectCommand two[2] = {{1, 1, 0, 0}, {1, 1, 3, 0}};
   memcpy(word, two, sizeof(two));
   counted[0] = 1;
   c->draw = (__typeof__(c->draw)){.mode = DRAW_INDIRECT_COUNT, .args = args.buffer, .count = count.buffer,
                                   .max_draws = 2};
   rows = draw_rows(c, points, strip, frag, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, 0, readback);
   if (rows != 0x1)
      report("geometry draws: an indirect count draw covered rows 0x%x", rows);
   check(rows == 0x1, "geometry: an indirect count draw draws only the draws counted");

   /* Two strips of two triangles each, rows 0 and 2, cut by a restart. */
   const uint16_t strips[9] = {0, 1, 2, 3, 0xffff, 4, 5, 6, 7};
   memcpy(index, strips, sizeof(strips));
   c->draw = (__typeof__(c->draw)){.mode = DRAW_INDEXED, .restart = true, .indices = indices.buffer};
   rows = draw_rows(c, quads, triangles, frag, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 9, readback);
   if (rows != 0x5)
      report("geometry draws: strips with a restart covered rows 0x%x", rows);
   check(rows == 0x5, "geometry: primitive restart cuts a triangle strip");

   const VkDrawIndexedIndirectCommand strip_args = {9, 1, 0, 0, 0};
   memcpy(word, &strip_args, sizeof(strip_args));
   c->draw = (__typeof__(c->draw)){.mode = DRAW_INDEXED_INDIRECT, .restart = true, .indices = indices.buffer,
                                   .args = args.buffer, .max_draws = 1};
   rows = draw_rows(c, quads, triangles, frag, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 0, readback);
   if (rows != 0x5)
      report("geometry draws: indirect strips with a restart covered rows 0x%x", rows);
   check(rows == 0x5, "geometry: primitive restart cuts a triangle strip drawn indirectly");

   c->draw = (__typeof__(c->draw)){.mode = DRAW_DIRECT};
   vkDestroyShaderModule(c->device, strip, NULL);
   vkDestroyShaderModule(c->device, quads, NULL);
   vkDestroyShaderModule(c->device, triangles, NULL);
   buffer_destroy(c, &indices);
   buffer_destroy(c, &args);
   buffer_destroy(c, &count);
}

/* Points drawn into strips by geometry shaders: every output size from 16 to
 * 128 vertices at 1, 2 and 4 points, a vertex count chosen from
 * gl_PrimitiveIDIn, a colour varying, and the IDs the invocations get.
 * Counts that are not constant go through NGG's workgroup repack. */
static void
test_geometry(struct context *c)
{
   struct buffer readback;
   if (!buffer_create(c, TARGET_SIZE * TARGET_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback)) {
      check(false, "geometry: readback buffer");
      return;
   }
   enum { ROW = TARGET_SIZE * TARGET_SIZE / 4 };
   VkShaderModule vert = shader(c, radv_smoke_gs_points_vert, sizeof(radv_smoke_gs_points_vert));
   VkShaderModule frag = shader(c, radv_smoke_frag, sizeof(radv_smoke_frag));

   /* Draws count points with the geometry shader and counts red texels. */
   #define DRAW_POINTS(vs, gs, fs, count, red)                                                                     \
      do {                                                                                                     \
         const VkPipelineShaderStageCreateInfo stages_[3] = {                                                   \
            stage_info(VK_SHADER_STAGE_VERTEX_BIT, vs),                                                         \
            stage_info(VK_SHADER_STAGE_GEOMETRY_BIT, gs),                                                       \
            stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, fs),                                                       \
         };                                                                                                     \
         const bool ok_ = render_readback(c, "geometry", stages_, 3, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, 0, count, \
                                          &readback);                                                           \
         const uint32_t *const texels_ = readback.map;                                                          \
         red = 0;                                                                                               \
         for (uint32_t i_ = 0; ok_ && i_ < TARGET_SIZE * TARGET_SIZE; i_++)                                     \
            red += texels_[i_] == 0xff0000ffu;                                                                  \
      } while (0)

   const struct {
      unsigned vertices;
      const uint32_t *code;
      size_t bytes;
   } shapes[5] = {
      {16, radv_smoke_gs_16_geom, sizeof(radv_smoke_gs_16_geom)},
      {32, radv_smoke_gs_32_geom, sizeof(radv_smoke_gs_32_geom)},
      {64, radv_smoke_gs_64_geom, sizeof(radv_smoke_gs_64_geom)},
      {100, radv_smoke_gs_100_geom, sizeof(radv_smoke_gs_100_geom)},
      {128, radv_smoke_gs_128_geom, sizeof(radv_smoke_gs_128_geom)},
   };
   unsigned sizes_right = 0;
   for (unsigned s = 0; s < 5; s++) {
      VkShaderModule geom = shader(c, shapes[s].code, shapes[s].bytes);
      const unsigned counts[3] = {1, 2, 4};
      for (unsigned n = 0; n < 3; n++) {
         uint32_t red;
         DRAW_POINTS(vert, geom, frag, counts[n], red);
         if (red == counts[n] * ROW)
            sizes_right++;
         else
            report("geometry: %u vertices from %u points drew %u texels, not %u", shapes[s].vertices, counts[n], red,
                   counts[n] * ROW);
      }
      vkDestroyShaderModule(c->device, geom, NULL);
   }
   check(sizes_right == 15, "geometry: strips of 16 to 128 vertices from 1, 2 and 4 points cover their rows");

   VkShaderModule by_id = shader(c, radv_smoke_gs_primid_geom, sizeof(radv_smoke_gs_primid_geom));
   VkShaderModule reads_id = shader(c, radv_smoke_gs_primid_fixed_geom, sizeof(radv_smoke_gs_primid_fixed_geom));
   uint32_t one, two, three, four;
   DRAW_POINTS(vert, by_id, frag, 1, one);
   DRAW_POINTS(vert, by_id, frag, 2, two);
   DRAW_POINTS(vert, reads_id, frag, 1, three);
   DRAW_POINTS(vert, reads_id, frag, 2, four);
   if (one != ROW || two != 2 * ROW)
      report("geometry: a count from gl_PrimitiveIDIn drew %u and %u texels", one, two);
   check(one == ROW && two == 2 * ROW, "geometry: a vertex count chosen from gl_PrimitiveIDIn draws every strip");
   check(three == ROW && four == 2 * ROW, "geometry: a shader that may return early draws every strip");
   vkDestroyShaderModule(c->device, by_id, NULL);
   vkDestroyShaderModule(c->device, reads_id, NULL);

   VkShaderModule colour_vert = shader(c, radv_smoke_gs_colour_vert, sizeof(radv_smoke_gs_colour_vert));
   VkShaderModule colour_geom = shader(c, radv_smoke_gs_colour_geom, sizeof(radv_smoke_gs_colour_geom));
   VkShaderModule colour_frag = shader(c, radv_smoke_colour_frag, sizeof(radv_smoke_colour_frag));
   uint32_t coloured;
   DRAW_POINTS(colour_vert, colour_geom, colour_frag, 2, coloured);
   check(coloured == 2 * ROW, "geometry: a colour passes from the vertex shader to the fragment shader");
   vkDestroyShaderModule(c->device, colour_vert, NULL);
   vkDestroyShaderModule(c->device, colour_geom, NULL);
   vkDestroyShaderModule(c->device, colour_frag, NULL);
   #undef DRAW_POINTS

   test_geometry_draws(c, &readback, vert, frag);

   /* The IDs four points' invocations record. */
   struct buffer record;
   VkShaderModule record_geom = shader(c, radv_smoke_gs_record_geom, sizeof(radv_smoke_gs_record_geom));
   bool ids_right = false;
   if (record_geom && buffer_create(c, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &record)) {
      memset(record.map, 0, 4096);
      const VkPipelineShaderStageCreateInfo stages[3] = {
         stage_info(VK_SHADER_STAGE_VERTEX_BIT, vert),
         stage_info(VK_SHADER_STAGE_GEOMETRY_BIT, record_geom),
         stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
      };
      const bool ok = render_readback_with(c, "geometry record", stages, 3, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, 0, 4,
                                           &readback, record.buffer, NULL);
      const uint32_t *const words = record.map;
      unsigned seen = 0;
      for (unsigned i = 0; ok && words[0] == 4 && i < 4; i++) {
         if (words[1 + 2 * i] < 4 && words[2 + 2 * i] == 0)
            seen |= 1u << words[1 + 2 * i];
      }
      ids_right = seen == 0xf;
      if (!ids_right)
         report("geometry: %u invocations recorded (%u %u) (%u %u) (%u %u) (%u %u)", words[0], words[1], words[2],
                words[3], words[4], words[5], words[6], words[7], words[8]);
      buffer_destroy(c, &record);
   }
   check(ids_right, "geometry: four points' invocations get primitive IDs 0 to 3");
   vkDestroyShaderModule(c->device, record_geom, NULL);
   vkDestroyShaderModule(c->device, vert, NULL);
   vkDestroyShaderModule(c->device, frag, NULL);
   buffer_destroy(c, &readback);
}

/* ------------------------------------------------- acceleration structures */

static PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR;
static PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR;
static PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR;
static PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR;
static PFN_vkGetBufferDeviceAddress vkGetBufferDeviceAddress;

/* A host-visible buffer with a device address. */
static bool
address_buffer_create(struct context *c, VkDeviceSize size, VkBufferUsageFlags usage, struct buffer *out,
                      VkDeviceAddress *address)
{
   *out = (struct buffer){.size = size};
   const VkBufferCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   if (vkCreateBuffer(c->device, &info, NULL, &out->buffer) != VK_SUCCESS)
      return false;
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(c->device, out->buffer, &req);
   const VkMemoryAllocateFlagsInfo flags = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
      .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
   };
   const uint32_t type = memory_type(c, req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   const VkMemoryAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &flags,
      .allocationSize = req.size,
      .memoryTypeIndex = type,
   };
   if (type == UINT32_MAX || vkAllocateMemory(c->device, &alloc, NULL, &out->memory) != VK_SUCCESS ||
       vkBindBufferMemory(c->device, out->buffer, out->memory, 0) != VK_SUCCESS ||
       vkMapMemory(c->device, out->memory, 0, VK_WHOLE_SIZE, 0, &out->map) != VK_SUCCESS)
      return false;
   const VkBufferDeviceAddressInfo address_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
      .buffer = out->buffer,
   };
   *address = vkGetBufferDeviceAddress(c->device, &address_info);
   return true;
}

/* Builds a bottom level of `count` triangles on the GPU into host-visible
 * memory. Returns whether each triangle's first vertex is in the structure. */
static bool
build_bottom_level(struct context *c, const float (*vertices)[3], uint32_t count)
{
   struct buffer vertex_buffer, structure_buffer, scratch_buffer;
   VkDeviceAddress vertex_address, structure_address, scratch_address;
   if (!address_buffer_create(c, sizeof(float) * 9 * count,
                              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, &vertex_buffer,
                              &vertex_address)) {
      report("acceleration structure: vertex buffer");
      return false;
   }
   memcpy(vertex_buffer.map, vertices, sizeof(float) * 9 * count);

   VkAccelerationStructureGeometryKHR geometry = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
      .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
      .geometry.triangles =
         {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
            .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
            .vertexData.deviceAddress = vertex_address,
            .vertexStride = sizeof(float) * 3,
            .maxVertex = 3 * count - 1,
            .indexType = VK_INDEX_TYPE_NONE_KHR,
         },
      .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
   };
   VkAccelerationStructureBuildGeometryInfoKHR build = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
      .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
      .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
      .geometryCount = 1,
      .pGeometries = &geometry,
   };
   VkAccelerationStructureBuildSizesInfoKHR sizes = {
      .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
   };
   vkGetAccelerationStructureBuildSizesKHR(c->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
                                           &count, &sizes);
   report("acceleration structure: %u triangles take %llu bytes, %llu of scratch", count,
          (unsigned long long)sizes.accelerationStructureSize, (unsigned long long)sizes.buildScratchSize);

   bool ok = address_buffer_create(c, sizes.accelerationStructureSize,
                                   VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, &structure_buffer,
                                   &structure_address) &&
             address_buffer_create(c, sizes.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &scratch_buffer,
                                   &scratch_address);
   VkAccelerationStructureKHR structure = VK_NULL_HANDLE;
   if (ok) {
      memset(structure_buffer.map, 0xcd, sizes.accelerationStructureSize);
      const VkAccelerationStructureCreateInfoKHR create = {
         .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
         .buffer = structure_buffer.buffer,
         .size = sizes.accelerationStructureSize,
         .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
      };
      ok = vkCreateAccelerationStructureKHR(c->device, &create, NULL, &structure) == VK_SUCCESS;
   }
   if (ok) {
      build.dstAccelerationStructure = structure;
      build.scratchData.deviceAddress = scratch_address;
      const VkAccelerationStructureBuildRangeInfoKHR range = {.primitiveCount = count};
      const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
      ok = begin(c);
      vkCmdBuildAccelerationStructuresKHR(c->cmd, 1, &build, &ranges);
      ok = ok && submit_and_wait(c, "acceleration structure");
   }

   bool found = false;
   if (ok) {
      const uint32_t *const words = structure_buffer.map;
      const uint32_t total = (uint32_t)(sizes.accelerationStructureSize / 4);
      found = true;
      for (uint32_t t = 0; t < count; t++) {
         bool here = false;
         for (uint32_t i = 0; i + 3 <= total && !here; i++)
            here = memcmp(words + i, vertices[3 * t], sizeof(float) * 3) == 0;
         found = found && here;
      }
   } else {
      report("acceleration structure: the build of %u triangles did not complete", count);
   }
   if (structure)
      vkDestroyAccelerationStructureKHR(c->device, structure, NULL);
   buffer_destroy(c, &structure_buffer);
   buffer_destroy(c, &scratch_buffer);
   buffer_destroy(c, &vertex_buffer);
   return found;
}

/* GPU builds of bottom levels of one and two triangles: the structure holds
 * each triangle's vertices. Runs where acceleration structures are reported
 * (RADV_PS5_RAY_TRACING=1 until ray tracing is on). */
static void
test_acceleration_structure(struct context *c)
{
   vkCreateAccelerationStructureKHR =
      (PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr(c->device, "vkCreateAccelerationStructureKHR");
   vkDestroyAccelerationStructureKHR =
      (PFN_vkDestroyAccelerationStructureKHR)vkGetDeviceProcAddr(c->device, "vkDestroyAccelerationStructureKHR");
   vkGetAccelerationStructureBuildSizesKHR = (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr(
      c->device, "vkGetAccelerationStructureBuildSizesKHR");
   vkCmdBuildAccelerationStructuresKHR = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(
      c->device, "vkCmdBuildAccelerationStructuresKHR");
   vkGetBufferDeviceAddress = (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(c->device, "vkGetBufferDeviceAddress");
   if (!vkCreateAccelerationStructureKHR || !vkDestroyAccelerationStructureKHR ||
       !vkGetAccelerationStructureBuildSizesKHR || !vkCmdBuildAccelerationStructuresKHR || !vkGetBufferDeviceAddress) {
      check(false, "acceleration structure: the extension's commands load");
      return;
   }

   static const float one[3][3] = {{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f}};
   check(build_bottom_level(c, one, 1), "acceleration structure: a GPU-built bottom level holds its one triangle");

   static const float two[6][3] = {
      {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f},
      {2.0f, 2.0f, 3.0f}, {3.0f, 2.0f, 3.0f}, {2.0f, 3.0f, 3.0f},
   };
   check(build_bottom_level(c, two, 2), "acceleration structure: a GPU-built bottom level holds its two triangles");
}

/* --------------------------------------------------------- sparse timing */

/* How the cost of sparse buffers grows with their number, which made the CTS's
 * 32-buffer sparse descriptor buffer cases outlast its watchdog (sparse-db-
 * scale-1 in PS5_Vulkan: 1, 8 and 16 buffers took about 1, 3 and 10 s). Makes
 * 48 sparse-binding buffers of 64 KiB and binds each to memory of its own,
 * as those cases do, timing the creation, the bind and its fence wait; one
 * line per eighth buffer. */
static bool
supported_sparse_binding(struct context *c)
{
   VkPhysicalDeviceFeatures supported;
   vkGetPhysicalDeviceFeatures(c->physical, &supported);
   return supported.sparseBinding;
}

static void
test_sparse_timing(struct context *c)
{
   PFN_vkQueueBindSparse bind_sparse = (PFN_vkQueueBindSparse)vkGetDeviceProcAddr(c->device, "vkQueueBindSparse");
   if (!bind_sparse) {
      check(false, "sparse timing: vkQueueBindSparse");
      return;
   }
   enum { COUNT = 48 };
   for (unsigned kind = 0; kind < (c->descriptor_buffer ? 2u : 1u); kind++) {
   const char *const kind_name = kind ? "descriptor buffers" : "storage buffers";
   VkBuffer buffers[COUNT] = {0};
   VkDeviceMemory memories[COUNT] = {0};
   uint64_t create_ns = 0, bind_ns = 0;
   bool ok = true;
   for (unsigned i = 0; i < COUNT && ok; i++) {
      const VkBufferCreateInfo info = {
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT,
         .size = 64 * 1024,
         .usage = kind ? VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                       : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      uint64_t t0 = now_ns();
      ok &= vkCreateBuffer(c->device, &info, NULL, &buffers[i]) == VK_SUCCESS;
      const uint64_t t1 = now_ns();
      if (!ok)
         break;
      VkMemoryRequirements req;
      vkGetBufferMemoryRequirements(c->device, buffers[i], &req);
      const VkMemoryAllocateInfo alloc = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = req.size,
         .memoryTypeIndex = memory_type(c, req.memoryTypeBits, 0),
      };
      ok &= vkAllocateMemory(c->device, &alloc, NULL, &memories[i]) == VK_SUCCESS;
      if (!ok)
         break;
      const VkSparseMemoryBind bind = {.size = req.size, .memory = memories[i]};
      const VkSparseBufferMemoryBindInfo buffer_bind = {.buffer = buffers[i], .bindCount = 1, .pBinds = &bind};
      const VkBindSparseInfo bind_info = {
         .sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
         .bufferBindCount = 1,
         .pBufferBinds = &buffer_bind,
      };
      const uint64_t t2 = now_ns();
      vkResetFences(c->device, 1, &c->fence);
      ok &= bind_sparse(c->queue, 1, &bind_info, c->fence) == VK_SUCCESS;
      ok &= vkWaitForFences(c->device, 1, &c->fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
      const uint64_t t3 = now_ns();
      create_ns += t1 - t0;
      bind_ns += t3 - t2;
      if ((i + 1) % 8 == 0)
         report("sparse timing, %s: buffer %u: creation %.3f ms, bind %.3f ms; %u so far: %.3f ms, %.3f ms",
                kind_name, i + 1, (t1 - t0) / 1e6, (t3 - t2) / 1e6, i + 1, create_ns / 1e6, bind_ns / 1e6);
   }
   check(ok, kind ? "sparse timing: 48 sparse descriptor buffers created and bound"
                  : "sparse timing: 48 sparse buffers created and bound");
   const uint64_t t0 = now_ns();
   for (unsigned i = 0; i < COUNT; i++) {
      if (buffers[i])
         vkDestroyBuffer(c->device, buffers[i], NULL);
      if (memories[i])
         vkFreeMemory(c->device, memories[i], NULL);
   }
   report("sparse timing, %s: destroying them took %.3f ms", kind_name, (now_ns() - t0) / 1e6);
   }
}

/* ----------------------------------------------------------------- display */

/* VK_KHR_display on VideoOut (Mesa's wsi_common_videoout.c): the display and
 * its modes, a plane surface, and a swapchain presenting 60 frames, each
 * cleared to a gray of its own. FIFO paces presents to flips: the last 50 take
 * 50 refresh periods within 15%. The last frame is read back before its
 * present. A swapchain made with the first as oldSwapchain presents 5 more,
 * and the retired one's acquire reports it out of date. */
#define DISPLAY_INSTANCE_COMMANDS(X)                                                               \
   X(GetPhysicalDeviceDisplayPropertiesKHR)                                                        \
   X(GetDisplayModePropertiesKHR)                                                                  \
   X(CreateDisplayPlaneSurfaceKHR)                                                                 \
   X(DestroySurfaceKHR)                                                                            \
   X(GetPhysicalDeviceSurfaceSupportKHR)                                                           \
   X(GetPhysicalDeviceSurfaceCapabilitiesKHR)                                                      \
   X(GetPhysicalDeviceSurfaceFormatsKHR)

#define DISPLAY_DEVICE_COMMANDS(X)                                                                 \
   X(CreateSwapchainKHR)                                                                           \
   X(DestroySwapchainKHR)                                                                          \
   X(GetSwapchainImagesKHR)                                                                        \
   X(AcquireNextImageKHR)                                                                          \
   X(QueuePresentKHR)                                                                              \
   X(CreateSemaphore)                                                                              \
   X(DestroySemaphore)                                                                             \
   X(CmdClearColorImage)                                                                           \
   X(CmdCopyBufferToImage)

DISPLAY_INSTANCE_COMMANDS(DECLARE)
DISPLAY_DEVICE_COMMANDS(DECLARE)

static void
display_image_barrier(struct context *c, VkImage image, VkImageLayout from, VkImageLayout to,
                      VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                      VkAccessFlags2 dst_access)
{
   const VkImageMemoryBarrier2 barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = src_stage,
      .srcAccessMask = src_access,
      .dstStageMask = dst_stage,
      .dstAccessMask = dst_access,
      .oldLayout = from,
      .newLayout = to,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   const VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &barrier,
   };
   vkCmdPipelineBarrier2(c->cmd, &dependency);
}

/* One frame: acquire, clear to gray, read back when asked, present. */
static VkResult
display_frame(struct context *c, VkSwapchainKHR swapchain, const VkImage *images, VkSemaphore acquired,
              VkSemaphore rendered, float gray, struct buffer *readback)
{
   uint32_t index;
   VkResult result = vkAcquireNextImageKHR(c->device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
   if (result != VK_SUCCESS)
      return result;
   const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   vkResetCommandBuffer(c->cmd, 0);
   vkBeginCommandBuffer(c->cmd, &begin_info);
   display_image_barrier(c, images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT);
   const VkClearColorValue color = {.float32 = {gray, gray, gray, 1.0f}};
   const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
   vkCmdClearColorImage(c->cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
   VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   if (readback) {
      display_image_barrier(c, images[index], layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
      layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      /* Two pixels: the first tile's and the last one's. */
      const VkBufferImageCopy copies[2] = {
         {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {1, 1, 1}},
         {.bufferOffset = 4,
          .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
          .imageOffset = {3839, 2159, 0},
          .imageExtent = {1, 1, 1}},
      };
      vkCmdCopyImageToBuffer(c->cmd, images[index], layout, readback->buffer, 2, copies);
   }
   display_image_barrier(c, images[index], layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
   vkEndCommandBuffer(c->cmd);

   const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   const VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &acquired,
      .pWaitDstStageMask = &wait_stage,
      .commandBufferCount = 1,
      .pCommandBuffers = &c->cmd,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &rendered,
   };
   vkResetFences(c->device, 1, &c->fence);
   result = vkQueueSubmit(c->queue, 1, &submit, c->fence);
   if (result != VK_SUCCESS)
      return result;
   const VkPresentInfoKHR present = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &rendered,
      .swapchainCount = 1,
      .pSwapchains = &swapchain,
      .pImageIndices = &index,
   };
   result = vkQueuePresentKHR(c->queue, &present);
   /* The one command buffer is recorded again next frame. */
   vkWaitForFences(c->device, 1, &c->fence, VK_TRUE, UINT64_MAX);
   return result;
}

static VkSwapchainKHR
display_swapchain(struct context *c, VkSurfaceKHR surface, VkSwapchainKHR old, VkImage *images, uint32_t *count)
{
   const VkSwapchainCreateInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = surface,
      .minImageCount = 3,
      .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
      .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
      .imageExtent = {3840, 2160},
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = VK_PRESENT_MODE_FIFO_KHR,
      .clipped = VK_TRUE,
      .oldSwapchain = old,
   };
   VkSwapchainKHR swapchain = VK_NULL_HANDLE;
   VkResult result = vkCreateSwapchainKHR(c->device, &info, NULL, &swapchain);
   check(result == VK_SUCCESS, old ? "display: a swapchain replacing the first" : "display: vkCreateSwapchainKHR");
   if (result != VK_SUCCESS)
      return VK_NULL_HANDLE;
   *count = 8;
   result = vkGetSwapchainImagesKHR(c->device, swapchain, count, images);
   check(result == VK_SUCCESS && *count >= 3 && *count <= 5, "display: 3 to 5 swapchain images");
   return swapchain;
}

static void
test_display(struct context *c)
{
   bool ok = true;
#define LOAD_I(name)                                                                               \
   vk##name = (PFN_vk##name)vk_icdGetInstanceProcAddr(c->instance, "vk" #name);                    \
   ok &= vk##name != NULL;
#define LOAD_D(name)                                                                               \
   vk##name = (PFN_vk##name)vkGetDeviceProcAddr(c->device, "vk" #name);                            \
   ok &= vk##name != NULL;
   DISPLAY_INSTANCE_COMMANDS(LOAD_I)
   DISPLAY_DEVICE_COMMANDS(LOAD_D)
#undef LOAD_I
#undef LOAD_D
   check(ok, "display: the VK_KHR_display and VK_KHR_swapchain commands");
   if (!ok)
      return;

   VkDisplayPropertiesKHR display;
   uint32_t count = 1;
   VkResult result = vkGetPhysicalDeviceDisplayPropertiesKHR(c->physical, &count, &display);
   check(result == VK_SUCCESS && count == 1 && display.physicalResolution.width == 3840 &&
            display.physicalResolution.height == 2160,
         "display: one 3840x2160 display");
   if (count != 1)
      return;
   VkDisplayModePropertiesKHR modes[2];
   count = 2;
   result = vkGetDisplayModePropertiesKHR(c->physical, display.display, &count, modes);
   check(result == VK_SUCCESS && count >= 1, "display: its modes");
   if (count == 0)
      return;
   report("display: %s, %u mode(s), the first at %.3f Hz", display.displayName, count,
          modes[0].parameters.refreshRate / 1000.0);

   const VkDisplaySurfaceCreateInfoKHR surface_info = {
      .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
      .displayMode = modes[0].displayMode,
      .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
      .imageExtent = {3840, 2160},
   };
   VkSurfaceKHR surface;
   result = vkCreateDisplayPlaneSurfaceKHR(c->instance, &surface_info, NULL, &surface);
   check(result == VK_SUCCESS, "display: vkCreateDisplayPlaneSurfaceKHR");
   if (result != VK_SUCCESS)
      return;
   VkBool32 supported = VK_FALSE;
   vkGetPhysicalDeviceSurfaceSupportKHR(c->physical, c->family, surface, &supported);
   VkSurfaceCapabilitiesKHR caps;
   vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c->physical, surface, &caps);
   check(supported && caps.currentExtent.width == 3840 && caps.currentExtent.height == 2160 &&
            caps.minImageCount <= 3 && caps.maxImageCount >= 3,
         "display: the surface's support and capabilities");

   struct buffer readback;
   VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
   const VkSemaphoreCreateInfo semaphore_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   if (!buffer_create(c, 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback) ||
       vkCreateSemaphore(c->device, &semaphore_info, NULL, &acquired) != VK_SUCCESS ||
       vkCreateSemaphore(c->device, &semaphore_info, NULL, &rendered) != VK_SUCCESS) {
      check(false, "display: its semaphores and readback buffer");
      vkDestroySurfaceKHR(c->instance, surface, NULL);
      return;
   }

   VkImage images[8];
   VkSwapchainKHR swapchain = display_swapchain(c, surface, VK_NULL_HANDLE, images, &count);
   if (swapchain) {
      const unsigned frames = 60, paced = 50;
      uint64_t start = 0;
      unsigned presented = 0;
      for (unsigned frame = 0; frame < frames; frame++) {
         if (frame == frames - paced)
            start = now_ns();
         const float gray = 0.10f + 0.10f * (float)(frame % 16) / 16.0f;
         result = display_frame(c, swapchain, images, acquired, rendered, gray,
                                frame == frames - 1 ? &readback : NULL);
         if (result != VK_SUCCESS)
            break;
         presented++;
      }
      const double seconds = (now_ns() - start) / 1e9;
      const double expected = paced * 1000.0 / modes[0].parameters.refreshRate;
      check(presented == frames, "display: 60 frames acquired, cleared and presented");
      report("display: the last %u frames took %.3f s, %.3f s at the mode's refresh", paced, seconds, expected);
      check(presented == frames && seconds > expected * 0.85 && seconds < expected * 1.15,
            "display: FIFO presents paced by the display's flips");
      /* B8G8R8A8: 0.10 + 0.10 * 11/16 of 255, rounded. */
      const uint8_t *pixels = readback.map;
      const uint8_t want = (uint8_t)((0.10f + 0.10f * 11.0f / 16.0f) * 255.0f + 0.5f);
      check(presented == frames && pixels[0] == want && pixels[1] == want && pixels[2] == want && pixels[3] == 255 &&
               pixels[4] == want && pixels[7] == 255,
            "display: the last frame's first and last pixels read back");

      uint32_t new_count = 0;
      VkImage new_images[8];
      VkSwapchainKHR replacement = display_swapchain(c, surface, swapchain, new_images, &new_count);
      if (replacement) {
         uint32_t index;
         result = vkAcquireNextImageKHR(c->device, swapchain, 0, VK_NULL_HANDLE, c->fence, &index);
         check(result == VK_ERROR_OUT_OF_DATE_KHR, "display: the retired swapchain's acquire is out of date");
         vkDestroySwapchainKHR(c->device, swapchain, NULL);
         swapchain = VK_NULL_HANDLE;
         unsigned more = 0;
         for (unsigned frame = 0; frame < 5; frame++)
            more += display_frame(c, replacement, new_images, acquired, rendered, 0.0f, NULL) == VK_SUCCESS;
         check(more == 5, "display: 5 frames through the replacement");
         vkDeviceWaitIdle(c->device);
         vkDestroySwapchainKHR(c->device, replacement, NULL);
      }
      if (swapchain)
         vkDestroySwapchainKHR(c->device, swapchain, NULL);
   }
   vkDestroySemaphore(c->device, acquired, NULL);
   vkDestroySemaphore(c->device, rendered, NULL);
   buffer_destroy(c, &readback);
   vkDestroySurfaceKHR(c->instance, surface, NULL);
}

/* ------------------------------------------------------------------- demo */

#include "android_probe/android_elf_loader.h"
#include "android_probe/bionic_shims.h"
#include "android_probe/fake_jni.h"
#include "android_probe/roblox_audio.h"
#include "android_probe/roblox_keyboard.h"
#include "android_probe/roblox_cookies.h"
#include "android_probe/roblox_accounts.h"
#include "android_probe/web_relay.h"
#include "android_probe/roblox_guide.h"
#include "android_probe/roblox_webview.h"

/* Java methods the title answers itself instead of the generic fake JNI. */
static int
roblox_java_hook(const char *key, const char *sig, const uint64_t *args, uint64_t *result)
{
   return ra_java_hook(key, sig, args, result) || rk_java_hook(key, sig, args, result) ||
          rc_java_hook(key, sig, args, result) || rw_java_hook(key, sig, args, result);
}

static void
bionic_report_line(const char *line)
{
   report("bionic: %s", line);
}

/* Step 3 of the native-Android plan: load an Android x86_64 .so (DT_NEEDED
 * libc.so and liblog.so, as an app's library has) from /app0/libprobe.so into
 * executable direct memory, bind its Bionic imports to the title's own
 * functions, run its constructors and call it. 1 on success, 0 on failure,
 * -1 when the library is absent. */
static int
android_log_print_shim(int prio, const char *tag, const char *fmt, ...)
{
   char text[300];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(text, sizeof(text), fmt, ap);
   va_end(ap);
   report("android: [liblog %d %s] %s", prio, tag, text);
   return 0;
}

static void
android_loader_log(const char *line)
{
   report("android: %s", line);
}

static void *
android_exec_alloc(size_t bytes)
{
#if defined(__PROSPERO__)
   void *p = ps5_exec_allocate(bytes, (uintptr_t)(void *)&android_exec_alloc);
   {
      static unsigned said;
      if (said++ < 24)
         report("exec alloc %zu bytes at %p", bytes, p);
   }
   return p;
#else
   return NULL;
#endif
}

static int
test_android_library(void)
{
#if defined(__PROSPERO__)
   const char *path = "/app0/libprobe.so";
#else
   const char *path = "libprobe.so";
#endif
   FILE *f = fopen(path, "rb");
   if (!f) {
      report("android: %s is not there; the Android library check is skipped", path);
      return -1;
   }
   static uint8_t image[1 << 20];
   const size_t size = fread(image, 1, sizeof(image), f);
   fclose(f);
   const struct ael_import imports[] = {
      {"__android_log_print", (void *)android_log_print_shim},
      {"strlen", (void *)strlen},
      {"malloc", (void *)malloc},
      {"free", (void *)free},
      {NULL, NULL},
   };
   struct ael_library lib;
   const int rc = ael_load(image, size, imports, android_exec_alloc, android_loader_log, &lib);
   check(rc == 0, "android: an Android x86_64 .so loads, relocates and binds its Bionic imports");
   if (rc != 0)
      return 0;
   const char *(*name)(void) = (const char *(*)(void))ael_symbol(&lib, "probe_name");
   int (*entry)(int) = (int (*)(int))ael_symbol(&lib, "probe_entry");
   check(name && entry, "android: its exports are found");
   if (!name || !entry)
      return 0;
   const int got = entry(7);
   report("android: %s says probe_entry(7) = %d (want 1085)", name(), got);
   check(got == 1085, "android: code from the Android library runs on the PS5 CPU");
   return got == 1085;
}

/* Step 4: the real Roblox client library. /app0/libroblox.so (x86_64, from
 * the official Roblox APK) is mapped, its 576,942 relocations (Android packed
 * APS2 included) applied and its imports bound. Imports this title does not
 * provide yet get trap thunks: "mov edi, index; movabs rax, handler; jmp rax",
 * so a call to one is logged by name instead of jumping to nothing.
 * ROBLOX_STAGE=ctors also runs its constructors. 1 on success, 0 on failure,
 * -1 when the library is absent. */
#define ROBLOX_MAX_TRAPS 1024
static const char *roblox_trap_names[ROBLOX_MAX_TRAPS];
static unsigned roblox_trap_count, roblox_trap_calls;
static uint8_t *roblox_trap_code;

static uint64_t
roblox_trap_handler(uint32_t index)
{
   roblox_trap_calls++;
   static uint8_t seen[ROBLOX_MAX_TRAPS];
   const bool first = index < ROBLOX_MAX_TRAPS && !seen[index];
   if (first)
      seen[index] = 1;
   if (first || (roblox_trap_calls & (roblox_trap_calls - 1)) == 0)
      report("roblox: call %u to an import not provided yet: %s", roblox_trap_calls,
             index < roblox_trap_count ? roblox_trap_names[index] : "?");
   return 0;
}

static void *
roblox_trap_resolver(const char *name, void *ctx)
{
   (void)ctx;
   if (!roblox_trap_code || roblox_trap_count >= ROBLOX_MAX_TRAPS)
      return NULL;
   const uint32_t index = roblox_trap_count++;
   roblox_trap_names[index] = name;
   uint8_t *t = roblox_trap_code + index * 32;
   const uint64_t handler = (uint64_t)(uintptr_t)&roblox_trap_handler;
   t[0] = 0xbf; /* mov edi, imm32 */
   memcpy(t + 1, &index, 4);
   t[5] = 0x48; /* movabs rax, imm64 */
   t[6] = 0xb8;
   memcpy(t + 7, &handler, 8);
   t[15] = 0xff; /* jmp rax */
   t[16] = 0xe0;
   return t;
}

/* The title's own code is execute-only on the PS5 (reading it is a
 * SYSTEM_XO_VIOLATION), but the library reads the first bytes of functions it
 * is handed. Every function given to it is therefore a readable stub in
 * executable direct memory: mov r11, target; jmp r11. */
static uint8_t *roblox_tramp_code;
static unsigned roblox_tramp_count;

static void *
roblox_trampoline(void *target)
{
   if (!target)
      return NULL;
   if (!roblox_tramp_code)
      roblox_tramp_code = android_exec_alloc(4096 * 16);
   if (!roblox_tramp_code || roblox_tramp_count >= 4096)
      return target;
   uint8_t *t = roblox_tramp_code + 16 * roblox_tramp_count++;
   const uint64_t to = (uint64_t)(uintptr_t)target;
   t[0] = 0x49, t[1] = 0xbb;
   memcpy(t + 2, &to, 8);
   t[10] = 0x41, t[11] = 0xff, t[12] = 0xe3;
   t[13] = 0xcc, t[14] = 0xcc, t[15] = 0xcc;
   return t;
}

#include "android_probe/roblox_vulkan.h"
#include "android_probe/warp/warp_gateway.h"
#include "android_probe/warp/warp_register.h"

static void roblox_warp_log(const char *line) { report("roblox: %s", line); }

/* Roblox's web traffic can use an in-title WARP SOCKS5 gateway when a
 * /data/roblox/files/warp.json identity is present. Otherwise no external
 * gateway is selected implicitly: ROBLOX_GATEWAY=ip:port selects one explicitly.
 * Provisioning for fresh installs is not yet implemented securely. */
/* The web relay for the PS5 browser (web_relay.c): roblox.com pages through
 * the gateway, with the active account's sign-in. ROBLOX_NO_WEB_RELAY=1: off. */
void wireguard_random_bytes(void *bytes, size_t size);

static void
roblox_relay_log(const char *line)
{
   report("roblox: %s", line);
}

static void
roblox_relay_cookies(const char *host, char *out, size_t cap)
{
   (void)host;
   size_t n = 0;
   out[0] = 0;
   pthread_mutex_lock(&rc_lock);
   for (unsigned i = 0; i < rc_n; i++) {
      if (strcmp(rc_jar[i].name, ".ROBLOSECURITY") != 0 && strcmp(rc_jar[i].name, "RBXEventTrackerV2") != 0 &&
          strcmp(rc_jar[i].name, "GuestData") != 0)
         continue;
      const int w = snprintf(out + n, cap - n, "%s%s=%s", n ? "; " : "", rc_jar[i].name, rc_jar[i].value);
      if (w < 0 || (size_t)w >= cap - n) {
         out[n] = 0;
         break;
      }
      n += (size_t)w;
   }
   pthread_mutex_unlock(&rc_lock);
}

static void
roblox_start_web_relay(void)
{
   const char *gw = getenv("ROBLOX_GATEWAY");
   if (getenv("ROBLOX_NO_WEB_RELAY") || !gw)
      return;
   char host[64] = "127.0.0.1";
   unsigned port = 1080;
   const char *colon = strrchr(gw, ':');
   if (colon) {
      snprintf(host, sizeof(host), "%.*s", (int)(colon - gw), gw);
      port = (unsigned)atoi(colon + 1);
   }
   static const struct web_relay_hooks hooks = {roblox_relay_log, roblox_relay_cookies, wireguard_random_bytes,
                                                    "/data/roblox/cache/web", rw_app_link};
   if (web_relay_start(8900, host, (unsigned short)port, &hooks) != 0)
      report("roblox: web relay did not start; web pages open directly");
}

static void
roblox_start_network(void)
{
   static const char *conf = "/data/roblox/files/warp.json";
   if (getenv("ROBLOX_GATEWAY")) {
      report("roblox: net: gateway %s (ROBLOX_GATEWAY)", getenv("ROBLOX_GATEWAY"));
      return;
   }
   if (access(conf, R_OK) != 0) {
      /* First run on this console: register its own WARP identity (its own
       * key, never shared). Nothing happens when warp.json exists. */
      mkdir("/data/roblox/files", 0700);
      const int r = warp_first_run_register("/data/roblox/files");
      report("roblox: net: first-run WARP registration -> %d%s", r, r == 0 ? " (registered)" : "");
   }
   if (access(conf, R_OK) != 0) {
      report("roblox: net: no %s; no gateway configured (network setup required)", conf);
      return;
   }
   if (warp_gateway_start(conf, 1080, roblox_warp_log) == 0) {
      setenv("ROBLOX_GATEWAY", "127.0.0.1:1080", 1);
      report("roblox: net: built-in WARP gateway on 127.0.0.1:1080 (no PC needed)");
   } else {
      report("roblox: net: built-in WARP gateway failed to start; no gateway configured");
   }
   roblox_start_web_relay();
}

static void *
roblox_wrap_import(const char *name, void *target)
{
   (void)name;
   return roblox_trampoline(target);
}

/* The title's own functions where Bionic's are the same ABI. */
#include <signal.h>
#include <ucontext.h>

static uintptr_t roblox_base, roblox_span;
/* The library's bytes as read from disk: the fault handler re-copies whole
 * pages of them when the console drops the backing of a code page
 * (instruction fetch SIGBUS with a resident, RWX page). */
static const uint8_t *roblox_image_file;
static size_t roblox_image_file_size;

struct roblox_phdr { /* Elf64_Phdr, as the loader hands it to dl_iterate_phdr */
   uint32_t p_type, p_flags;
   uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};

/* Hex words at a pointer, skipping anything that is not a mapped page: a
 * fault raised inside the fault reporter would lose the report itself. */
static void
roblox_dump_words(const char *label, uintptr_t p, int words)
{
   unsigned char vec[1];
   char line[400];
   if (p < 0x10000 || p >= 0x800000000000ul || (p & 7))
      return;
   for (int i = 0; i < words; i += 4) {
      const uintptr_t at = p + (uintptr_t)i * 8;
      if (mincore((void *)(at & ~(uintptr_t)4095), 4096, (char *)vec) != 0)
         return;
      int n = snprintf(line, sizeof(line), "roblox:    %s+%02x:", label, i * 8);
      for (int j = 0; j < 4 && i + j < words; j++)
         n += snprintf(line + n, sizeof(line) - (size_t)n, " %016lx",
                       (unsigned long)((const uintptr_t *)at)[j]);
      report("%s", line);
   }
}

/* A window of raw stack words around a frame pointer, four per line: shows the
 * locals of the faulting frame and of its callers (the copy constructors that
 * build the objects this bug is about), so the object can be read by hand. */
static void
roblox_dump_stack(uintptr_t fp)
{
   unsigned char vec[1];
   char line[400];
   if (fp < 0x10000 || (fp & 7))
      return;
   for (uintptr_t at = fp - 0x100; at < fp + 0x400; at += 32) {
      if (mincore((void *)(at & ~(uintptr_t)4095), 4096, (char *)vec) != 0)
         continue;
      int n = snprintf(line, sizeof(line), "roblox:    stk %+ld:", (long)((intptr_t)at - (intptr_t)fp));
      for (int j = 0; j < 4; j++)
         n += snprintf(line + n, sizeof(line) - (size_t)n, " %016lx",
                       (unsigned long)((const uintptr_t *)at)[j]);
      report("%s", line);
   }
}

/* A fault inside the library: where, as an offset into libroblox.so that a
 * disassembler on the PC can look up. */

/* ---- raw Linux syscalls inside the library --------------------------------
 * libroblox.so issues 16 `syscall` instructions of its own (futex, gettid,
 * getpid, and nine whose number is computed at run time). They bypass every
 * import shim and reach the PS5's FreeBSD kernel with Linux numbers - 202
 * (futex) is __sysctl there - and FreeBSD reports errors as a positive errno,
 * which Linux code takes for success. So each one is turned into ud2 at load
 * time; the SIGILL comes here, the Linux call is carried out through the
 * shims, the Linux-style result (-errno on failure) goes in rax, and the
 * thread resumes after the instruction. */
static const uint32_t roblox_syscall_sites[] = {
   0x28bdf7b, 0x28bdf9e, 0x28be11d, 0x28be16c, 0x28be196, 0x28be21b, 0x28be6a9, 0x6225869,
   0x62258e2, 0x62259a0, 0x6226867, 0x622695d, 0x6226a3b, 0x6226adb, 0x622761a, 0x6227fd8};
static unsigned roblox_syscall_hits[sizeof(roblox_syscall_sites) / sizeof(roblox_syscall_sites[0])];

static void
roblox_syscall_patch(uint8_t *base)
{
   unsigned done = 0;
   for (unsigned i = 0; i < sizeof(roblox_syscall_sites) / sizeof(roblox_syscall_sites[0]); i++) {
      uint8_t *at = base + roblox_syscall_sites[i];
      if (at[0] == 0x0f && at[1] == 0x05) {
         at[1] = 0x0b; /* syscall -> ud2 */
         done++;
      } else {
         report("roblox: raw syscall site lib+0x%x has %02x %02x, left alone", roblox_syscall_sites[i], at[0], at[1]);
      }
   }
   report("roblox: %u raw syscall sites now trap into the emulator", done);
}

static long
roblox_linux_syscall(long n, long a0, long a1, long a2, long a3, long a4, long a5)
{
   (void)a4;
   long r;
   switch (n) {
   case 0: /* Linux read on descriptors opened by the compatibility layer. */
      if (!bionic_track_openat)
         return -38;
      r = bionic_read((int)a0, (void *)a1, (size_t)a2);
      return r < 0 ? -(long)bn_linux_errno(errno) : r;
   case 3: /* Linux close. */
      if (!bionic_track_openat)
         return -38;
      r = bionic_close((int)a0);
      return r < 0 ? -(long)bn_linux_errno(errno) : r;
   case 257: /* Linux openat; optional directory-handle compatibility experiment. */
      if (!bionic_track_openat)
         return -38;
      /* Absolute paths ignore dirfd under Linux; route them through the
       * normal Bionic path layer, including its /proc compatibility. */
      if (a1 && ((const char *)a1)[0] == '/')
         r = bionic_open((const char *)a1, (int)a2, (int)a3);
      else
         r = ps5_openat((int)a0 == -100 ? AT_FDCWD : (int)a0, (const char *)a1,
                        bionic_open_flags((int)a2), (int)a3);
      if (r < 0) {
         const int e = errno;
         report("roblox: experimental openat dirfd=%ld path=%.160s flags=0x%lx errno=%d",
                a0, (const char *)a1, a2, e);
         return -(long)bn_linux_errno(e);
      }
      return r;
   case 138: { /* Linux fstatfs on our emulated /proc/self/maps. */
      if (!bionic_track_openat || !a1)
         return !a1 ? -14 : -38;
      struct stat actual, proc;
      if (fstat((int)a0, &actual) != 0)
         return -(long)bn_linux_errno(errno);
      if (stat("/data/roblox/proc/maps", &proc) != 0 ||
          actual.st_dev != proc.st_dev || actual.st_ino != proc.st_ino)
         return -38;
      /* Linux x86_64 struct statfs: 15 64-bit words, with fsid in
       * word 7. /proc is a virtual filesystem and has no block capacity. */
      uint64_t *out = (uint64_t *)a1;
      memset(out, 0, 120);
      out[0] = 0x9fa0; /* PROC_SUPER_MAGIC */
      out[1] = 4096;
      out[8] = 255; /* f_namelen */
      out[9] = 4096; /* f_frsize */
      return 0;
   }
   case 202: /* futex */
      r = bionic_futex((volatile int *)a0, (int)a1, (int)a2, (const struct timespec *)a3, (int)a5);
      return r < 0 ? -(long)errno : r;
   case 186: return bionic_gettid();
   case 39: return (long)getpid();
   case 110: return 1; /* getppid */
   case 102: case 104: case 107: case 108: return 0; /* getuid, getgid, geteuid, getegid */
   case 24: sched_yield(); return 0;
   case 228: /* clock_gettime */
      return bionic_clock_gettime((int)a0, (struct timespec *)a1) == 0 ? 0 : -22;
   case 96: /* gettimeofday */
      return gettimeofday((struct timeval *)a0, NULL) == 0 ? 0 : -22;
   case 318: /* getrandom */
      bionic_getentropy((void *)a0, (size_t)a1);
      return a1;
   case 13: case 14: return 0;  /* rt_sigaction, rt_sigprocmask: virtual */
   case 131: return 0;          /* sigaltstack */
   case 157: return 0;          /* prctl */
   case 101: return 0;          /* ptrace: "no debugger" */
   case 62: case 200: case 234: /* kill, tkill, tgkill: not delivered */
      return 0;
   default: return -38; /* ENOSYS */
   }
}

/* 1 when the SIGILL was one of the patched sites (and has been handled). */
static int
roblox_syscall_trap(int sig, ucontext_t *uc)
{
   if (sig != SIGILL || !roblox_base)
      return 0;
   const uintptr_t rip = (uintptr_t)uc->uc_mcontext.mc_rip;
   if (rip < roblox_base || rip >= roblox_base + roblox_span)
      return 0;
   const uint32_t off = (uint32_t)(rip - roblox_base);
   int site = -1;
   for (unsigned i = 0; i < sizeof(roblox_syscall_sites) / sizeof(roblox_syscall_sites[0]); i++)
      if (roblox_syscall_sites[i] == off)
         site = (int)i;
   if (site < 0)
      return 0;
   mcontext_t *m = &uc->uc_mcontext;
   const long n = (long)m->mc_rax;
   const int saved = errno;
   const long r = roblox_linux_syscall(n, (long)m->mc_rdi, (long)m->mc_rsi, (long)m->mc_rdx, (long)m->mc_r10,
                                       (long)m->mc_r8, (long)m->mc_r9);
   errno = saved;
   const unsigned hits = __atomic_add_fetch(&roblox_syscall_hits[site], 1, __ATOMIC_RELAXED);
   if (hits <= 3 || (hits & (hits - 1)) == 0)
      report("roblox: raw syscall %ld at lib+0x%x (hit %u): args %lx %lx %lx -> %ld", n, off, hits,
             (unsigned long)m->mc_rdi, (unsigned long)m->mc_rsi, (unsigned long)m->mc_rdx, r);
   m->mc_rax = (uint64_t)r;
   m->mc_rcx = m->mc_rip + 2; /* what syscall leaves in rcx / r11 */
   m->mc_r11 = m->mc_rflags;
   m->mc_rip += 2;
   return 1;
}


/* The library takes %fs:0 as the calling thread's identity (Android's TLS
 * block starts with a pointer to itself; its allocator compares a span's
 * owner with it to choose the owner-only, non-atomic free path). On the PS5
 * %fs:0 is 0 on every thread, so every thread looked like the owner of every
 * span: concurrent non-atomic frees, chunks returned to the system while
 * still in use, and the heap corruption that followed. %fs:0x10 holds the
 * thread's own struct pthread pointer there - non-zero, one per thread - so
 * the seven instructions (all `fs: op reg, [disp32]`) are pointed at it. */
static const uint32_t roblox_fs0_sites[] = {0x1a90bd9, 0x1a90ed0, 0x1a9435e, 0x1ee4dd9,
                                            0x6907c99, 0x6908336, 0x6909214};

static void
roblox_fs0_patch(uint8_t *base)
{
   unsigned done = 0;
   for (unsigned i = 0; i < sizeof(roblox_fs0_sites) / sizeof(roblox_fs0_sites[0]); i++) {
      uint8_t *at = base + roblox_fs0_sites[i];
      /* 64 REX op modrm(SIB) 25 00000000 */
      if (at[0] == 0x64 && (at[1] & 0xf0) == 0x40 && (at[3] & 0xc7) == 0x04 && at[4] == 0x25 && at[5] == 0 &&
          at[6] == 0 && at[7] == 0 && at[8] == 0) {
         at[5] = 0x10;
         done++;
      } else {
         report("roblox: %%fs:0 site lib+0x%x does not match; left alone", roblox_fs0_sites[i]);
      }
   }
   report("roblox: %u of %u thread-identity reads moved from %%fs:0 (0 on the PS5) to %%fs:0x10", done,
          (unsigned)(sizeof(roblox_fs0_sites) / sizeof(roblox_fs0_sites[0])));
}


/* SIGUSR2 prints where the interrupted thread is: rip and the rbp chain, as
 * library offsets (one line per thread). Installed at start-up. */
static void
roblox_sample_handler(int sig, siginfo_t *info, void *context)
{
   (void)sig;
   (void)info;
   const int saved_errno = errno; /* the interrupted thread's */
   const ucontext_t *uc = context;
   const uintptr_t rip = (uintptr_t)uc->uc_mcontext.mc_rip;
   /* one line per thread: rip, then the library frames of the rbp chain */
   char line[700];
   size_t at = (size_t)snprintf(line, sizeof(line), "roblox: SAMPLE thread %x at %s+0x%lx:",
                                (unsigned)((uintptr_t)pthread_self() >> 4),
                                rip >= roblox_base && rip < roblox_base + roblox_span ? "lib" : "out",
                                (unsigned long)(rip >= roblox_base && rip < roblox_base + roblox_span ? rip - roblox_base
                                                                                                       : rip));
   uintptr_t fp = (uintptr_t)uc->uc_mcontext.mc_rbp;
   for (int d = 0; d < 28 && fp > 0x10000 && (fp & 7) == 0 && at < sizeof(line) - 24; d++) {
      const uintptr_t ret = ((const uintptr_t *)fp)[1];
      if (ret >= roblox_base && ret < roblox_base + roblox_span)
         at += (size_t)snprintf(line + at, sizeof(line) - at, " %lx", (unsigned long)(ret - roblox_base));
      else
         at += (size_t)snprintf(line + at, sizeof(line) - at, " .");
      const uintptr_t next = ((const uintptr_t *)fp)[0];
      if (next <= fp)
         break;
      fp = next;
   }
   report("%s", line);
   errno = saved_errno;
}
/* Frame-rate monitor: every 10 s, frames shown in that window and how much of
 * it was spent inside vkQueuePresentKHR (display wait). */
static void *
roblox_fps_monitor(void *arg)
{
   (void)arg;
   unsigned last = 0, windows = 0;
   uint64_t last_ns = 0, dm_last[11] = {0};
   for (;;) {
      sleep(10);
      const unsigned n = __atomic_load_n(&rv_presents, __ATOMIC_RELAXED);
      const uint64_t pns = __atomic_load_n(&rv_present_ns, __ATOMIC_RELAXED);
      if (n == 0)
         continue;
      const unsigned d = n - last;
      uint64_t dm[11];
      bionic_dm_stats(dm);
      const unsigned g50 = __atomic_exchange_n(&rv_gaps_50ms, 0, __ATOMIC_RELAXED);
      const unsigned g250 = __atomic_exchange_n(&rv_gaps_250ms, 0, __ATOMIC_RELAXED);
      const uint64_t gmax = __atomic_exchange_n(&rv_gap_max_ns, 0, __ATOMIC_RELAXED);
      report("roblox: fps %.1f (frames %u, %u%% of the time in present; freezes: %u over 50 ms, %u over 250 ms, "
             "longest %llu ms; direct memory: %llu maps (%llu reused, %llu cached), "
             "%llu unmaps, %llu frees, %llu ms; zeroed %llu MiB in %llu ms; hole search %llu ms, cache %llu ms, unmap %llu ms; "
             "log: %llu lines written, %llu dropped; failed maps %llu in %llu ms)",
             d / 10.0, n, (unsigned)((pns - last_ns) / 100000000ull), g50, g250, (unsigned long long)(gmax / 1000000), (unsigned long long)(dm[0] - dm_last[0]),
             (unsigned long long)(dm[4] - dm_last[4]), (unsigned long long)(dm[5] - dm_last[5]),
             (unsigned long long)(dm[1] - dm_last[1]), (unsigned long long)(dm[2] - dm_last[2]),
             (unsigned long long)(dm[3] - dm_last[3]), (unsigned long long)(dm[6] - dm_last[6]),
             (unsigned long long)(dm[7] - dm_last[7]), (unsigned long long)(dm[8] - dm_last[8]),
             (unsigned long long)(dm[9] - dm_last[9]), (unsigned long long)(dm[10] - dm_last[10]),
             (unsigned long long)__atomic_load_n(&report_written, __ATOMIC_RELAXED),
             (unsigned long long)__atomic_load_n(&report_dropped, __ATOMIC_RELAXED),
             (unsigned long long)__atomic_load_n(&bionic_mmap_fails, __ATOMIC_RELAXED),
             (unsigned long long)(__atomic_load_n(&bionic_mmap_fail_ns, __ATOMIC_RELAXED) / 1000000));
      memcpy(dm_last, dm, sizeof(dm));
      {
         uint64_t calls, total, slowest;
         const char *kind;
         bionic_fio_stats(&calls, &total, &slowest, &kind);
         uint64_t kept, spilled, written_out;
         size_t live;
         vf_stats(&kept, &spilled, &written_out, &live);
         if (calls)
            report("roblox: files: %llu calls in %llu ms, slowest %llu ms (%s); spill files in memory %llu "
                   "(%zu KiB live), to storage %llu, written out %llu",
                   (unsigned long long)calls, (unsigned long long)(total / 1000000),
                   (unsigned long long)(slowest / 1000000), kind, (unsigned long long)kept, live >> 10,
                   (unsigned long long)spilled, (unsigned long long)written_out);
      }
      if (++windows % 6 == 0) {
         uint64_t w[5];
         bionic_dm_waste(w);
         report("roblox: direct memory %llu MiB in %llu regions; released inside them %llu MiB "
                "(%llu MiB in free 16 KiB blocks); cache %llu MiB",
                (unsigned long long)(w[0] >> 20), (unsigned long long)w[3], (unsigned long long)(w[1] >> 20),
                (unsigned long long)(w[2] >> 20), (unsigned long long)(w[4] >> 20));
      }
      last = n;
      last_ns = pns;
   }
   return NULL;
}
/* While a frame is late (no present for 200 ms, then every 400 ms of the same
 * freeze), sample where the presenting thread is (SIGUSR2 -> roblox: SAMPLE),
 * at most 4 times in each 10 s (start-up used to spend a budget of 40 for the
 * whole run before any game started). */
static void *
roblox_freeze_watch(void *arg)
{
   (void)arg;
   unsigned samples = 0;
   uint64_t sampled_for = 0, next_at = 0, window = 0;
   for (;;) {
      usleep(50 * 1000);
      if (now_ns() - window >= 10000000000ull) {
         window = now_ns();
         samples = 0;
      }
      if (samples >= 4)
         continue;
      if (!__atomic_load_n(&rv_present_thread_set, __ATOMIC_ACQUIRE) ||
          __atomic_load_n(&rw_render_pause, __ATOMIC_ACQUIRE) || __atomic_load_n(&rv_quitting, __ATOMIC_ACQUIRE))
         continue;
      const uint64_t last = __atomic_load_n(&rv_last_present_ns, __ATOMIC_RELAXED);
      const uint64_t t = now_ns();
      if (!last || t - last < 200000000ull)
         continue;
      if (sampled_for != last) {
         sampled_for = last;
         next_at = t;
      }
      if (t >= next_at) {
         report("roblox: freeze: no frame for %llu ms", (unsigned long long)((t - last) / 1000000));
         pthread_kill(rv_present_thread, SIGUSR2);
         samples++;
         next_at = t + 400000000ull;
      }
   }
   return NULL;
}
static void
roblox_sample_start(void)
{
   struct sigaction sa;
   memset(&sa, 0, sizeof(sa));
   sa.sa_sigaction = roblox_sample_handler;
   sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
   sigaction(SIGUSR2, &sa, NULL);
   report_async_start();
   pthread_t m;
   pthread_create(&m, NULL, roblox_fps_monitor, NULL);
   pthread_detach(m);
   pthread_create(&m, NULL, roblox_freeze_watch, NULL);
   pthread_detach(m);
}


static void *
roblox_lua_login_hold(void *arg)
{
   volatile int32_t *mode = (volatile int32_t *)((uintptr_t)arg + 0x6ec5bb8);
   report("roblox: Lua login: flag lib+0x6ec5bb8 was %d, set to 1", *mode);
   for (int i = 0; i < 60; i++) {
      if (*mode != 1) {
         report("roblox: Lua login: flag rewritten to %d after %d s; set back to 1", *mode, i * 2);
         *mode = 1;
      }
      if (i == 0)
         *mode = 1;
      usleep(2000 * 1000);
   }
   return NULL;
}


/* ---- controller: the PS5 pad as an Android gamepad ------------------------
 * What the app's Java input listener (kk.f) does with Android key and motion
 * events: nativeGamepadButtonEvent(device, keycode, 1 down / 0 up) and
 * nativeGamepadAxisEvent(device, axis, x, y, z) with Android's own axis ids;
 * at connect, the supported keys/axes, then
 * nativeGamepadConnectEventWithGamepadType(device, type) - type 2 is what the
 * app gives a DualSense (PlayStation button icons). */
static void *roblox_pad_fn[9]; /* button, axis, connect, key, motion, mouse move, mouse button, disconnect, wheel */
static void (*roblox_back_pressed)(void *, void *); /* NativeGLInterface.nativeHandleBackPressed */
static void *roblox_refresh_fn[2];
static void *(*roblox_get_cookies)(void *, void *, void *); /* NativeSettingsInterface.nativeGetCookiesForDomain */ /* nativePassSupportedRefreshRates, nativePassCurrentDisplayRefreshRate */
uint64_t warp_rx_packets(void); /* warp_gateway.c */
void warp_rotate_endpoint(void);
static void (*roblox_app_set_active)(void *, void *); /* JNIAppLifecycleNativeAdapter.setActive */
/* NativeGLInterface.nativeAppBridgeV2UpdateSurfaceAppWithPlatformParams and its arguments */
static void (*roblox_surface_app)(void *, void *, void *, void *);
static void *roblox_gl_cls, *roblox_app_surface, *roblox_app_params;
/* Controller input goes to the view the engine keeps as a weak pointer in
 * the view registry lib+0x1fd493e(4) (read by lib+0x1fdb8f8). A game's view
 * takes it over; when the game ends that view is destroyed and the pointer
 * expires, so every button was dropped and the menus no longer answered.
 * The menus' view is put back (roblox_input_view_restore) and the window
 * focus sent again. Tried alone and not enough: window focus, handing the
 * surface back (UpdateSurfaceAppWithPlatformParams), starting the menus'
 * DataModel again. */

static void (*roblox_window_focus)(void *, void *, long long, int); /* onWindowFocusChangedNative */
static void (*roblox_surface_created)(void *, void *, long long, void *);
static void (*roblox_surface_changed)(void *, void *, long long, void *, int, int, int);
static void *roblox_activity_obj;
static long long roblox_activity_handle;

/* A libc++ weak pointer {object, control block}; the control block holds
 * owners - 1 at +8 (-1 once the object is gone) and weak owners - 1 at +16. */
struct roblox_weak_slot {
   uintptr_t obj, ctrl;
};

static int
roblox_ctrl_alive(uintptr_t ctrl)
{
   return ctrl >= 0x100000 && !(ctrl & 7) && __atomic_load_n((long *)(ctrl + 8), __ATOMIC_ACQUIRE) >= 0;
}

/* While the slot points at a live object outside a game, remember the first
 * one (with a weak reference of ours, so its control block stays
 * allocated); when it is expired outside a game, point it back. Returns 1
 * when it did. */
static int
roblox_weak_slot_restore(volatile uintptr_t *slot, struct roblox_weak_slot *saved)
{
   const uintptr_t ctrl = slot[1];
   if (roblox_ctrl_alive(ctrl)) {
      if (!roblox_ctrl_alive(saved->ctrl) && ctrl != saved->ctrl) {
         __atomic_add_fetch((long *)(ctrl + 16), 1, __ATOMIC_ACQ_REL);
         saved->obj = slot[0];
         saved->ctrl = ctrl;
      }
      return 0;
   }
   if (!roblox_ctrl_alive(saved->ctrl) || ctrl == saved->ctrl)
      return 0;
   __atomic_add_fetch((long *)(saved->ctrl + 16), 1, __ATOMIC_ACQ_REL); /* the slot's weak reference */
   slot[0] = saved->obj;
   slot[1] = saved->ctrl;
   return 1;
}

/* Two registries hold "the view on screen" for type 4 (the menus and games
 * alike): lib+0x1fd493e (input, slot at +0x1120) and lib+0x200915e (Back and
 * other app events, slot at +0x8). Both expire when a game ends. Called every
 * ~200 ms from the pad thread; returns 1 when it put the menus back. */
static struct roblox_weak_slot input_view; /* the menus' view, for input */
/* a copy of the menus' DataModel-change listener (see roblox_view_dm_hook):
 * the engine's own listener object does not live long */
static uintptr_t roblox_view_dm_listener[4];
static int
roblox_input_view_restore(int in_game)
{
   static struct roblox_weak_slot app_view;
   void *(*input_registry)(int) = (void *(*)(int))(roblox_base + 0x1fd493e);
   void *(*app_registry)(int) = (void *(*)(int))(roblox_base + 0x200915e);
   if (in_game)
      return 0;
   char *a = input_registry(4), *b = app_registry(4);
   int restored = 0;
   if (a)
      restored |= roblox_weak_slot_restore((volatile uintptr_t *)(a + 0x1120), &input_view);
   if (b)
      restored |= roblox_weak_slot_restore((volatile uintptr_t *)(b + 0x8), &app_view);
   if (restored)
      report("roblox: pad: menus' view for input %p (control %p), for app events %p (control %p)",
             (void *)input_view.obj, (void *)input_view.ctrl, (void *)app_view.obj, (void *)app_view.ctrl);
   return restored;
}
/* The engine's "this view takes the input" method (lib+0x2288554, obj,
 * focused) is reached only through vtables (directly and through a thunk,
 * lib+0x5ca8d4c). Their slots are pointed at these wrappers, which remember
 * the object the menus were focused with, so the same call can be made again
 * after a game (Android's app does it when it is shown again). */
static void (*roblox_focus_direct)(void *, int);
static void (*roblox_focus_thunk)(void *, int);
static void *roblox_focus_direct_obj, *roblox_focus_thunk_obj;
static unsigned roblox_focus_calls;

static void
roblox_focus_direct_hook(void *obj, int focused)
{
   if (focused && !__atomic_load_n(&bionic_in_game, __ATOMIC_RELAXED) && !roblox_focus_direct_obj)
      roblox_focus_direct_obj = obj;
   if (roblox_focus_calls++ < 20)
      report("roblox: focus: %p %d (direct)", obj, focused);
   roblox_focus_direct(obj, focused);
}

static void
roblox_focus_thunk_hook(void *obj, int focused)
{
   if (focused && !__atomic_load_n(&bionic_in_game, __ATOMIC_RELAXED) && !roblox_focus_thunk_obj)
      roblox_focus_thunk_obj = obj;
   if (roblox_focus_calls++ < 20)
      report("roblox: focus: %p %d (thunk)", obj, focused);
   roblox_focus_thunk(obj, focused);
}

/* The input manager's listener for "a view's DataModel changed"
 * (lib+0x5d06da4, vtable slot lib+0x6dd6ab8; this+8 is the manager): if the
 * view is the manager's input view (+0x1120), it binds the input to that
 * view's DataModel (connection at +0x1118). When a game ends, the menus'
 * DataModel comes back while the slot still names the dead game view, so the
 * bind is skipped and every button and click is lost (Back uses another
 * path). After the slot is put back, the call is made again. */
static void (*roblox_view_dm_changed)(void *, void *);
static unsigned roblox_view_dm_calls;

static void
roblox_view_dm_hook(void *self, void *view)
{
   const uintptr_t *l = self;
   void *mgr = ((void *(*)(int))(roblox_base + 0x1fd493e))(4);
   const int in_game = __atomic_load_n(&bionic_in_game, __ATOMIC_RELAXED);
   /* the function uses only this+8 (the input manager) and this+0x10 */
   if (!in_game && (void *)l[1] == mgr && !roblox_view_dm_listener[1]) /* the first: the menus' */
      memcpy(roblox_view_dm_listener, l, sizeof(roblox_view_dm_listener));
   if (roblox_view_dm_calls++ < 40)
      report("roblox: input: view %p has a new DataModel (listener %p: +8 %p (manager %p), +0x10 %p, in game %d)", view,
             self, (void *)l[1], mgr, (void *)l[2], in_game);
   roblox_view_dm_changed(self, view);
}

static void *
roblox_view_dm_rebind_thread(void *view)
{
   char *owner = (char *)roblox_view_dm_listener[1];
   report("roblox: input: binding the menus' DataModel again (view %p, manager %p, +0x10 %p, slot %p/%p)", view,
          owner, (void *)roblox_view_dm_listener[2], (void *)((uintptr_t *)(owner + 0x1120))[0],
          (void *)((uintptr_t *)(owner + 0x1120))[1]);
   roblox_view_dm_changed(roblox_view_dm_listener, view);
   report("roblox: input: bound to the menus' DataModel again");
   return NULL;
}

static void
roblox_focus_hooks_install(uint8_t *base)
{
   uintptr_t *dm_slot = (uintptr_t *)(base + 0x6dd6ab8);
   if (*dm_slot == (uintptr_t)(base + 0x5d06da4) && !getenv("ROBLOX_NO_DM_HOOK")) {
      roblox_view_dm_changed = (void (*)(void *, void *))(base + 0x5d06da4);
      *dm_slot = (uintptr_t)roblox_view_dm_hook;
   }
   report("roblox: input: DataModel-change hook %s", roblox_view_dm_changed ? "installed" : "not installed");

   unsigned direct = 0, thunk = 0;
   for (uintptr_t *p = (uintptr_t *)(base + 0x6960940); p < (uintptr_t *)(base + 0x6e60708); p++) {
      if (*p == (uintptr_t)(base + 0x2288554)) {
         roblox_focus_direct = (void (*)(void *, int))(base + 0x2288554);
         *p = (uintptr_t)roblox_focus_direct_hook;
         direct++;
      } else if (*p == (uintptr_t)(base + 0x5ca8d4c)) {
         roblox_focus_thunk = (void (*)(void *, int))(base + 0x5ca8d4c);
         *p = (uintptr_t)roblox_focus_thunk_hook;
         thunk++;
      }
   }
   report("roblox: focus hooks: %u direct, %u thunk vtable slots", direct, thunk);
}

#define RP_DEV 1
/* The games that use the touchpad mouse (not made for consoles): place ids in
 * /data/roblox/files/mouse-games.txt. set < 0: is place listed; else add (1) or
 * remove (0) it. */
static int
roblox_mouse_game(long long place, int set)
{
   static const char fn[] = "/data/roblox/files/mouse-games.txt";
   if (!place)
      return 0;
   long long ids[256];
   unsigned n = 0;
   int found = 0;
   FILE *f = fopen(fn, "r");
   if (f) {
      long long v;
      while (n < 256 && fscanf(f, "%lld", &v) == 1) {
         if (v == place)
            found = 1;
         else
            ids[n++] = v;
      }
      fclose(f);
   }
   if (set < 0 || (set ? found : !found))
      return found;
   if (set && n < 256)
      ids[n++] = place;
   char tmp[64];
   snprintf(tmp, sizeof(tmp), "%s.tmp", fn);
   f = fopen(tmp, "w");
   if (!f)
      return set;
   for (unsigned i = 0; i < n; i++)
      fprintf(f, "%lld\n", ids[i]);
   if (fclose(f) == 0)
      rename(tmp, fn);
   return set;
}

static void *
roblox_pad_thread(void *arg)
{
   (void)arg;
   void *env = NULL;
   fj_AttachCurrentThread(&fj_the_vm, (fj_env **)&env, NULL);
   void *cls = fj_class("com/roblox/engine/jni/NativeInputInterface");
   void (*btn)(void *, void *, int, int, int) = roblox_pad_fn[0];
   void (*axis)(void *, void *, int, int, float, float, float) = roblox_pad_fn[1];
   void (*conn)(void *, void *, int, int) = roblox_pad_fn[2];
   void (*key)(void *, void *, int, int, unsigned char, int) = roblox_pad_fn[3];
   void (*motion)(void *, void *, int, int, int, unsigned char, int) = roblox_pad_fn[4];
   if (!btn || !axis || !conn) {
      report("roblox: pad: input entry points missing");
      return NULL;
   }
   int pad = -1;
   for (int tries = 0; tries < 60 && pad < 0; tries++) {
      sceUserServiceInitialize(NULL);
      scePadInit();
      int users[4] = {-1, -1, -1, -1};
      sceUserServiceGetLoginUserIdList(users);
      for (int u = 0; u < 4 && pad < 0; u++) {
         if (users[u] == -1)
            continue;
         pad = scePadOpen(users[u], 0, 0, NULL);
         if (pad < 0)
            pad = scePadGetHandle(users[u], 0, 0);
      }
      if (pad < 0)
         sleep(1);
   }
   report("roblox: pad: handle %d", pad);
   if (pad < 0)
      return NULL;
   /* PS5 button bit -> Android keycode */
   static const struct { uint32_t bit; int key; } map[] = {
      {0x4000, 96},  /* cross    -> BUTTON_A */
      {0x2000, 97},  /* circle   -> BUTTON_B */
      {0x8000, 99},  /* square   -> BUTTON_X */
      {0x1000, 100}, /* triangle -> BUTTON_Y */
      {0x0400, 102}, /* L1 */
      {0x0800, 103}, /* R1 */
      {0x0100, 104}, /* L2 */
      {0x0200, 105}, /* R2 */
      {0x0002, 106}, /* L3 */
      {0x0004, 107}, /* R3 */
      {0x0008, 108}, /* options  -> START */
      {0x0010, 19},  /* d-pad up */
      {0x0040, 20},  /* down */
      {0x0080, 21},  /* left */
      {0x0020, 22},  /* right */
   };
   const int type = 2;
   if (key) {
      for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
         key(env, cls, RP_DEV, map[i].key, 1, type);
      key(env, cls, RP_DEV, 109, 1, type); /* BUTTON_SELECT: the touchpad button in games */
   }
   if (motion) {
      static const int axes[] = {0, 1, 11, 14, 15, 16, 17, 18};
      for (unsigned i = 0; i < sizeof(axes) / sizeof(axes[0]); i++)
         motion(env, cls, RP_DEV, axes[i], -1, 1, type);
   }
   conn(env, cls, RP_DEV, type);
   report("roblox: pad: DualSense connected to Roblox as gamepad %d (type %d)", RP_DEV, type);
   /* The app's input service did not record that connection (its gamepad
    * list stayed empty and the menus showed Xbox prompts), so the
    * controller is announced again 3 s after each "app ready" / "game
    * loaded" log line, when that DataModel's input service exists. */
   /* FFlagEnableSeamlessForceOTAUpdate2 (lib+0x6f01a70, see the
    * ClientAppSettings.json writer): if the settings file did not turn it
    * off, turn it off here (the flags are loaded by now) */
   {
      volatile unsigned char *ota = (volatile unsigned char *)(roblox_base + 0x6f01a70);
      const unsigned char was = *ota;
      if (was && !getenv("ROBLOX_KEEP_OTA_FLAG"))
         *ota = 0;
      report("roblox: pad: FFlagEnableSeamlessForceOTAUpdate2 was %u, now %u", was, *ota);
   }
   /* GameBasicSettingsFramerateCap (lib+0x6ec41f0): with it on, games take
    * their frame-rate cap from the user's setting / TaskSchedulerTargetFps
    * (lib+0x2074e54: min(target, 240), 60 when unset); off, games stayed at
    * 60 with the display at 120 Hz. The flag name did not take from the
    * settings JSON. ROBLOX_FPS60=1 leaves it off. */
   if (!getenv("ROBLOX_FPS60")) {
      *(volatile unsigned char *)(roblox_base + 0x6ec41f0) = 1;
      report("roblox: pad: GameBasicSettingsFramerateCap on (games may run above 60)");
      /* GraphicsOptimizationModeFRMFrameRateTarget (value lib+0x6ecf428):
       * the frame-rate manager's cap in games ("gfx/frm/frameRateCap") */
      volatile int *frm = (volatile int *)(roblox_base + 0x6ecf428);
      const int was = *frm;
      if (was > 0 && was < 120)
         *frm = 120;
      report("roblox: pad: FRM frame-rate target %d -> %d", was, *frm);
   }
   unsigned events_seen = __atomic_load_n(&bionic_app_events, __ATOMIC_RELAXED);
   /* The display's refresh rate, as Android's app passes it (uh.t0):
    * nativePassSupportedRefreshRates(float[]) and
    * nativePassCurrentDisplayRefreshRate(float). Without them the engine took
    * 60 Hz and capped games at 60 with the display at 119.88 Hz. */
   {
      void (*pass_rates)(void *, void *, void *) = roblox_refresh_fn[0];
      void (*pass_current)(void *, void *, float) = roblox_refresh_fn[1];
      void *gl_cls = fj_class("com/roblox/engine/jni/NativeGLInterface");
      for (int w = 0; w < 100 && !rv_refresh_mhz; w++)
         usleep(100 * 1000); /* the surface (and its mode) comes first */
      const float hz = rv_refresh_mhz ? rv_refresh_mhz / 1000.0f : 60.0f;
      if (pass_rates && !getenv("ROBLOX_NO_REFRESH_RATE")) {
         struct fj_obj *arr = fj_array(hz > 61 ? 2 : 1, sizeof(float), "[F");
         if (arr) {
            ((float *)arr->data)[0] = 59.94f;
            if (hz > 61)
               ((float *)arr->data)[1] = hz;
            pass_rates(env, gl_cls, arr);
         }
      }
      if (pass_current && !getenv("ROBLOX_NO_REFRESH_RATE"))
         pass_current(env, gl_cls, hz);
      report("roblox: display refresh rate %.2f Hz passed to the engine (%p %p)", hz, (void *)pass_rates,
             (void *)pass_current);
   }
   uint64_t announce_at = 0;
   unsigned announced = 0;
   uint32_t last = 0;
   float last_ax[6] = {0};
   unsigned presses = 0, backs = 0, loops = 0, restores = 0;
   void *gl_cls = fj_class("com/roblox/engine/jni/NativeGLInterface");
   /* The touchpad is a trackpad: a finger moves a mouse pointer over the
    * 1920x1080 window (the touchpad reports the same range), a click is a
    * left click there. Games made for touch screens have buttons the d-pad
    * cannot reach; this reaches them. ROBLOX_NO_TRACKPAD=1 turns it off. */
   void (*mouse_move)(void *, void *, float, float, float, float) = roblox_pad_fn[5];
   void (*mouse_button)(void *, void *, float, float, unsigned char, int) = roblox_pad_fn[6];
   const int trackpad = mouse_move && mouse_button && !getenv("ROBLOX_NO_TRACKPAD");
   float px = 960, py = 540;
   int touch_id = -1, touch_x = 0, touch_y = 0, clicked = 0;
   unsigned moves = 0;
   const int click_button = getenv("ROBLOX_CLICK_BUTTON") ? atoi(getenv("ROBLOX_CLICK_BUTTON")) : 0;
   for (;;) {
      const unsigned events = __atomic_load_n(&bionic_app_events, __ATOMIC_RELAXED);
      if (events != events_seen) {
         events_seen = events;
         announce_at = now_ns() + 3000000000ull;
      }
      if (announce_at && now_ns() >= announce_at) {
         announce_at = 0;
         conn(env, cls, RP_DEV, type);
         if (announced++ < 20)
            report("roblox: pad: DualSense announced again (type %d)", type);
      }
      /* the engine dropped its render view: hand it the surface again, as
       * Android's SurfaceView does (the new view had no size and the game's
       * load divided by it: SIGFPE at lib+0x50d778f) */
      {
         static unsigned views_seen;
         const unsigned v = __atomic_load_n(&bionic_view_destroyed, __ATOMIC_ACQUIRE);
         if (v != views_seen) {
            views_seen = v;
            if (roblox_surface_created && roblox_surface_changed && !getenv("ROBLOX_NO_VIEW_SURFACE")) {
               struct fj_obj *sf = fj_new(FJ_OBJECT, "Surface", NULL);
               sf->cls = fj_class("android/view/Surface");
               roblox_surface_created(env, roblox_activity_obj, roblox_activity_handle, sf);
               roblox_surface_changed(env, roblox_activity_obj, roblox_activity_handle, sf, 1, 1920, 1080);
               report("roblox: pad: render view dropped: surface handed over again");
            }
         }
      }
      /* A tunnel stall while the app starts makes it show "Connection Error
       * - Retry" over its sign-in page (Landing); the login is still saved and
       * Retry brings Home back. Once data flows again (packets from
       * Cloudflare in the last 10 s) Retry (Cross) is pressed for the user,
       * up to 10 times, 25 s apart; from the second try the tunnel moves to
       * another endpoint first. ROBLOX_NO_AUTO_RETRY=1 turns it off. */
      {
         static uint64_t next_try, rx_seen, rx_seen_at, pressed_at;
         static unsigned tries;
         static int gave_up; /* a press opened a sign-up / sign-in page */
         const uint64_t t = now_ns();
         const uint64_t rx = warp_rx_packets();
         if (rx != rx_seen) {
            rx_seen = rx;
            rx_seen_at = t;
         }
         /* No error dialog: the saved sign-in no longer works and Cross hit
          * a Landing button. Go back (Circle) and leave Landing alone. */
         if (pressed_at && t - pressed_at < 10000000000ull && !gave_up &&
             (strstr(bionic_app_page, "SignUp") || strstr(bionic_app_page, "Login") ||
              strstr(bionic_app_page, "SignIn"))) {
            gave_up = 1;
            pressed_at = 0;
            report("roblox: Retry opened %s: the sign-in has ended; going back, no more auto-Retry", bionic_app_page);
            usleep(500 * 1000);
            btn(env, cls, RP_DEV, 97, 1);
            usleep(120 * 1000);
            btn(env, cls, RP_DEV, 97, 0);
         }
         if (strcmp(bionic_app_page, "Landing") != 0) {
            if (strcmp(bionic_app_page, "Home") == 0)
               gave_up = 0; /* signed in again */
            tries = 0;
            next_try = 0;
         } else if (!getenv("ROBLOX_NO_AUTO_RETRY") && !gave_up && tries < 10) {
            if (!next_try)
               next_try = t + 5000000000ull;
            else if (t >= next_try && rx_seen_at && t - rx_seen_at < 10000000000ull) {
               char buf[256] = "";
               FILE *f = fopen("/data/roblox/files/session.txt", "rb");
               if (f) {
                  (void)!fread(buf, 1, sizeof(buf) - 1, f);
                  fclose(f);
               }
               if (!strstr(buf, ".ROBLOSECURITY=")) { /* really signed out: leave it */
                  tries = 10;
                  continue;
               }
               tries++;
               next_try = t + 25000000000ull;
               if (tries > 1) {
                  /* the last Retry failed although data flows: the tunnel is
                   * slow, not dead; move to another WARP endpoint first */
                  warp_rotate_endpoint();
                  usleep(6000 * 1000);
               }
               report("roblox: connection back on the connection-error page: pressing Retry (%u)", tries);
               btn(env, cls, RP_DEV, 96, 1);
               usleep(120 * 1000);
               btn(env, cls, RP_DEV, 96, 0);
               pressed_at = now_ns();
            }
         }
      }
      /* The user's Maximum Frame Rate (UserGameSettings.FramerateCap) goes to
       * the task scheduler only at start-up (lib+0x1f1be09 -> lib+0x5c6fc97),
       * and only with GameBasicSettingsFramerateCap on, which happens later
       * here; so games ran at the default 60. Apply it again 5 s after each
       * game has loaded (and once at start). ROBLOX_FPS60=1 skips it. */
      {
         static int applied_for = -1;
         static uint64_t apply_at;
         const int ig = __atomic_load_n(&bionic_in_game, __ATOMIC_RELAXED);
         if (ig != applied_for && !apply_at)
            apply_at = now_ns() + 5000000000ull;
         if (apply_at && now_ns() >= apply_at && !getenv("ROBLOX_FPS60")) {
            apply_at = 0;
            applied_for = ig;
            ((void (*)(void))(roblox_base + 0x5c6fc97))();
            report("roblox: frame-rate cap applied to the task scheduler (in game %d)", ig);
         }
      }
      /* save the login the engine has now, 3 s after a sign-in or account
       * switch (roblox_cookies.h) */
      {
         static unsigned logins_seen;
         static uint64_t sync_at;
         const unsigned le = __atomic_load_n(&bionic_login_events, __ATOMIC_RELAXED);
         if (le != logins_seen) {
            logins_seen = le;
            sync_at = now_ns() + 3000000000ull;
         }
         /* and once a minute: the account switcher's cookie (rbxas) changes
          * when its list is used, without a login event */
         if (!sync_at && loops % 7500 == 3000)
            sync_at = now_ns();
         if (sync_at && now_ns() >= sync_at) {
            sync_at = 0;
            rc_sync_from_engine(env, roblox_get_cookies);
         }
      }
      /* The guide as a welcome (roblox_guide.h), once per player: the
       * first time each signed-in account reaches Home on this console (/data/roblox/files/guide-shown
       * holds the user ids that saw it). ROBLOX_NO_GUIDE=1: never. */
      if (loops % 125 == 60 && !getenv("ROBLOX_NO_GUIDE")) {
         static uint64_t home_since;
         static long long checked_user = -1;
         static int guide_due;
         static const char shown_file[] = "/data/roblox/files/guide-shown";
         const long long user = __atomic_load_n(&bionic_user_id, __ATOMIC_RELAXED);
         if (user != checked_user) {
            checked_user = user;
            guide_due = 0;
            if (user > 0) {
               guide_due = 1;
               FILE *gf = fopen(shown_file, "r");
               char line[32];
               while (gf && fgets(line, sizeof(line), gf))
                  if (strtoll(line, NULL, 10) == user)
                     guide_due = 0;
               if (gf)
                  fclose(gf);
            }
         }
         /* not during start-up: wait for 1200 frames and 15 s on Home with
          * frames moving, so it shows over a page that is ready */
         static unsigned frames_at;
         const unsigned frames = __atomic_load_n(&rv_presents, __ATOMIC_RELAXED);
         const int home = guide_due && strcmp(bionic_app_page, "Home") == 0 &&
                          !__atomic_load_n(&bionic_in_game, __ATOMIC_RELAXED) &&
                          !__atomic_load_n(&rk_active, __ATOMIC_ACQUIRE) &&
                          !__atomic_load_n(&rw_dialog_open, __ATOMIC_ACQUIRE) && !rg_is_open() && frames >= 1200;
         if (!home)
            home_since = 0;
         else if (!home_since)
            home_since = now_ns(), frames_at = frames;
         else if (now_ns() - home_since > 15000000000ull && frames - frames_at > 600) { /* Home settled */
            guide_due = 0;
            FILE *gf = fopen(shown_file, "a");
            if (gf) {
               fprintf(gf, "%lld\n", user);
               fclose(gf);
            }
            report("roblox: welcome: first time on Home for this player: opening the guide");
            rg_open(0);
         }
      }
      /* the account switcher's list: keep the largest one (roblox_accounts.h) */
      {
         static unsigned logouts_seen;
         const unsigned lo = __atomic_load_n(&bionic_logout_events, __ATOMIC_RELAXED);
         if (lo != logouts_seen) {
            logouts_seen = lo;
            ra_watch(1);
         } else if (loops % 1250 == 600) { /* ~every 10 s */
            ra_watch(0);
         }
      }
      if (loops % 3750 == 1) { /* ~every 30 s: the engine's frame-rate settings */
         static int said;
         if (said++ < 40)
            report("roblox: fps flags: TaskSchedulerTargetFps %d, GameBasicSettingsFramerateCap %d, FRM target %d, "
                   "GraphicsOptimizationMode min frame %d ms, max frame %d ms",
                   *(volatile int *)(roblox_base + 0x6f1a558), *(volatile unsigned char *)(roblox_base + 0x6ec41f0),
                   *(volatile int *)(roblox_base + 0x6ecf428), *(volatile int *)(roblox_base + 0x6ecf3f8),
                   *(volatile int *)(roblox_base + 0x6ecf410));
      }
      if (++loops % 25 == 0 && !getenv("ROBLOX_NO_VIEW_FIX") &&
          roblox_input_view_restore(__atomic_load_n(&bionic_in_game, __ATOMIC_RELAXED))) {
         /* the menus' view has the input again. The engine's own "view takes
          * the input" step (lib+0x2288554) then posts one task to the view
          * (descriptor lib+0x6dd1fba) through lib+0x590857a; post it too. */
         {
            void *(*registry)(int) = (void *(*)(int))(roblox_base + 0x1fd493e);
            void *(*current)(void *) = (void *(*)(void *))(roblox_base + 0x1fdb8f8);
            void (*post)(void *, void *, int) = (void (*)(void *, void *, int))(roblox_base + 0x590857a);
            void (*cleanup)(void *) = (void (*)(void *))(roblox_base + 0x29a4bc2);
            void *view = current(registry(4));
            if (view) {
               uint64_t task[12];
               memset(task, 0, sizeof(task));
               task[3] = (uint64_t)(roblox_base + 0x6dd1fba);
               post(view, task, 1);
               cleanup(task);
            }
         }
         /* and tell the engine the window has focus, as Android does when the
          * menus come back */
         if (roblox_surface_created && roblox_surface_changed && !getenv("ROBLOX_NO_SURFACE_AGAIN")) {
            /* as Android does when the app's own view is shown again */
            struct fj_obj *sf = fj_new(FJ_OBJECT, "Surface", NULL);
            sf->cls = fj_class("android/view/Surface");
            roblox_surface_created(env, roblox_activity_obj, roblox_activity_handle, sf);
            roblox_surface_changed(env, roblox_activity_obj, roblox_activity_handle, sf, 1, 1920, 1080);
         }
         if (roblox_window_focus) {
            roblox_window_focus(env, roblox_activity_obj, roblox_activity_handle, 0);
            usleep(50 * 1000);
            roblox_window_focus(env, roblox_activity_obj, roblox_activity_handle, 1);
         }
         /* the menus' view takes the input again, as at startup */
         if (roblox_focus_direct_obj)
            roblox_focus_direct(roblox_focus_direct_obj, 1);
         else if (roblox_focus_thunk_obj)
            roblox_focus_thunk(roblox_focus_thunk_obj, 1);
         report("roblox: focus: menus focused again (%p / %p)", roblox_focus_direct_obj, roblox_focus_thunk_obj);
         /* bind the input to the menus' DataModel again */
         if (roblox_view_dm_changed && roblox_view_dm_listener[1] && input_view.obj && !getenv("ROBLOX_NO_REBIND")) {
            pthread_t t; /* its own thread: it takes engine locks */
            if (pthread_create(&t, NULL, roblox_view_dm_rebind_thread, (void *)input_view.obj) == 0)
               pthread_detach(t);
         }
         /* the app hands its surface to the menus again (startup did this once:
          * updateSurfaceLuaApp -> SurfaceController::update) */
         if (roblox_surface_app && !getenv("ROBLOX_NO_SURFACE_APP"))
            roblox_surface_app(env, roblox_gl_cls, roblox_app_surface, roblox_app_params);
         /* Android's app lifecycle: the menus are active again */
         {
            void (*set_active)(void *, void *) = roblox_app_set_active;
            if (set_active && !getenv("ROBLOX_NO_SET_ACTIVE")) {
               struct fj_obj *o = fj_new(FJ_OBJECT, "JNIAppLifecycleNativeAdapter", NULL);
               o->cls = fj_class("com/roblox/universalapp/applifecyclenativeadapter/JNIAppLifecycleNativeAdapter");
               set_active(env, o);
               report("roblox: pad: app lifecycle set active");
            }
         }
         /* the menus kept the controller as connected to the ended game:
          * disconnect it, then announce it again */
         void (*disconn)(void *, void *, int) = roblox_pad_fn[7];
         if (disconn)
            disconn(env, cls, RP_DEV);
         announce_at = now_ns() + 500000000ull;
         if (restores++ < 10)
            report("roblox: pad: input pointed back at the app's menus after the game");
      }
      DemoPadData st;
      memset(&st, 0, sizeof(st));
      if (scePadReadState(pad, &st) == 0 && st.connected) {
         rw_pad_tick(env);
         { /* the in-game browser has no Close of its own: Circle closes it */
            static unsigned prev_b;
            if (__atomic_load_n(&rw_dialog_open, __ATOMIC_ACQUIRE) && (st.buttons & 0x2000u) && !(prev_b & 0x2000u))
               __atomic_store_n(&rw_close_req, 1, __ATOMIC_RELEASE);
            if (__atomic_load_n(&rw_dialog_open, __ATOMIC_ACQUIRE) && st.buttons != prev_b) {
               static int said;
               if (said++ < 8)
                  report("roblox: web: pad while the browser is open: buttons 0x%x", st.buttons);
            }
            prev_b = st.buttons;
         }
         const int guide_owns = rg_pad(st.buttons); /* the on-screen guide (roblox_guide.h) */
         if (__atomic_load_n(&rk_active, __ATOMIC_ACQUIRE) || guide_owns) {
            /* the system keyboard or the guide has the controller: release everything */
            st.touch[0] = 0; /* no fingers: the touchpad mouse stays still */
            st.buttons = 0;
            st.lx = st.ly = st.rx = st.ry = 128;
            st.l2 = st.r2 = 0;
         }
         const uint32_t changed = st.buttons ^ last;
         for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
            if (changed & map[i].bit) {
               const int down = (st.buttons & map[i].bit) != 0;
               btn(env, cls, RP_DEV, map[i].key, down);
               /* Android turns a controller B the page does not use into the
                * system Back key, and a game's page only goes back that way.
                * There Circle is also Back; in a game it stays a game button.
                * ROBLOX_NO_CIRCLE_BACK=1 turns it off. */
               /* only on a game's page: Back on Home is "leave the app" on
                * Android and made Roblox reload its menus and crash */
               if (map[i].key == 97 && !down && roblox_back_pressed && !getenv("ROBLOX_NO_CIRCLE_BACK") &&
                   !__atomic_load_n(&bionic_in_game, __ATOMIC_RELAXED) &&
                   strcmp(bionic_app_page, "ExperienceDetail") == 0) {
                  roblox_back_pressed(env, gl_cls);
                  bionic_app_page[0] = 0; /* left that page: no second Back on Home */
                  if (backs++ < 10)
                     report("roblox: pad: Circle sent Back to the app");
               }
               if (down && presses++ < 40)
                  report("roblox: pad: button %d pressed", map[i].key);
            }
         last = st.buttons;
         /* In a game the touchpad is what it is on a real PS5's Roblox: its
          * button is ButtonSelect (games show it as "Menu"; Roblox's UI
          * navigation), and fingers do nothing. Any mouse input makes Roblox
          * drop the controller's GUI selection, so X then pressed nothing (Work
          * at a Pizza Place's job picker). Holding the touchpad button 1 s
          * switches the touchpad mouse on/off in games (PS5 notification); in
          * the app's menus it is always the mouse. ROBLOX_GAME_MOUSE=1 starts
          * with it on. */
         static int game_mouse = -1, click_blocked;
         static long long mouse_place = -1;
         const long long place = __atomic_load_n(&bionic_place_id, __ATOMIC_RELAXED);
         if (game_mouse < 0 || place != mouse_place) {
            /* each game starts the way it was left: the games where the mouse
             * was switched on are kept in mouse-games.txt (one place id a line) */
            mouse_place = place;
            game_mouse = getenv("ROBLOX_GAME_MOUSE") != NULL || roblox_mouse_game(place, -1);
            if (place && game_mouse)
               report("roblox: pad: touchpad mouse on for this game (place %lld, remembered)", place);
         }
         const int in_game = __atomic_load_n(&bionic_in_game, __ATOMIC_RELAXED) != 0;
         const int pad_mouse = trackpad && (!in_game || game_mouse);
         {
            static uint64_t tp_down_at;
            static int tp_select_down, tp_toggled, tp_x0, tp_y0;
            const int tp = (st.buttons & 0x100000) != 0;
            const uint64_t t = now_ns();
            if (tp && !tp_down_at) {
               tp_down_at = t, tp_toggled = 0;
               tp_x0 = st.touch[8] | st.touch[9] << 8;
               tp_y0 = st.touch[10] | st.touch[11] << 8;
            }
            if (in_game && tp && !tp_toggled && t - tp_down_at > 1000000000ull) {
               /* a held click that has not moved the pointer: the switch */
               const int fx = st.touch[8] | st.touch[9] << 8, fy = st.touch[10] | st.touch[11] << 8;
               if (!pad_mouse || (abs(fx - tp_x0) < 40 && abs(fy - tp_y0) < 40)) {
                  tp_toggled = 1;
                  click_blocked = 1;
                  if (clicked) { /* let go of the mouse button first */
                     mouse_button(env, cls, px, py, 0, click_button);
                     clicked = 0;
                  }
                  game_mouse = !game_mouse;
                  roblox_mouse_game(place, game_mouse);
                  report("roblox: pad: touchpad mouse in games %s (place %lld)", game_mouse ? "ON" : "off", place);
                  ps5_notify(game_mouse ? "Touchpad mouse: ON for this game (hold the touchpad 1 s to turn off)"
                                        : "Touchpad mouse: OFF for this game - touchpad = Menu (hold 1 s for the mouse)");
               }
            }
            /* ButtonSelect when the touchpad is not the mouse: a short press,
             * sent when it is let go (a hold is the mouse switch instead) */
            if (tp_select_down) {
               btn(env, cls, RP_DEV, 109, 0);
               tp_select_down = 0;
            }
            if (!tp && tp_down_at) {
               if (!tp_toggled && !pad_mouse && in_game) {
                  btn(env, cls, RP_DEV, 109, 1);
                  tp_select_down = 1; /* up on the next pass */
                  if (presses++ < 40)
                     report("roblox: pad: touchpad button -> ButtonSelect");
               }
               tp_down_at = 0;
            }
         }
         if (pad_mouse) {
            /* touch data: count, 7 reserved bytes, then {x u16, y u16, id u8, 3 reserved} per finger */
            const int fingers = st.touch[0];
            const int fx = st.touch[8] | st.touch[9] << 8, fy = st.touch[10] | st.touch[11] << 8, fid = st.touch[12];
            /* two fingers: scroll (mouse wheel at the pointer), like a
             * laptop touchpad; finger moves up = content moves up (natural) */
            void (*wheel)(void *, void *, float, float, float) = roblox_pad_fn[8];
            static float scroll_acc;
            static int scroll_last_y = -1;
            if (fingers >= 2 && wheel && !getenv("ROBLOX_NO_SCROLL")) {
               const int y2 = (fy + (st.touch[14] | st.touch[15] << 8)) / 2;
               if (scroll_last_y >= 0) {
                  scroll_acc += (float)(y2 - scroll_last_y);
                  /* one wheel notch per 40 touchpad units of travel */
                  while (scroll_acc >= 40.0f || scroll_acc <= -40.0f) {
                     const float notch = scroll_acc > 0 ? 1.0f : -1.0f;
                     wheel(env, cls, px, py, notch);
                     scroll_acc -= notch * 40.0f;
                     static int said;
                     if (said++ < 3)
                        report("roblox: pad: touchpad scroll %+.0f at %.0f,%.0f", notch, px, py);
                  }
               }
               scroll_last_y = y2;
               touch_id = -2; /* the pointer stays where it is until one finger is down again */
               goto touch_done;
            }
            scroll_last_y = -1;
            scroll_acc = 0;
            if (fingers > 0 && fid == touch_id) {
               const float dx = (float)(fx - touch_x), dy = (float)(fy - touch_y);
               if (dx != 0 || dy != 0) {
                  px += dx;
                  py += dy;
                  px = px < 0 ? 0 : px > 1919 ? 1919 : px;
                  py = py < 0 ? 0 : py > 1079 ? 1079 : py;
                  mouse_move(env, cls, px, py, dx, dy);
                  if (moves++ < 3)
                     report("roblox: pad: trackpad moves the pointer to %.0f,%.0f", px, py);
               }
            }
            touch_id = fingers > 0 ? fid : -1;
         touch_done:
            touch_x = fx;
            touch_y = fy;
            /* click_blocked: the hold that switched the mouse on/off is no click */
            const int tpb = (st.buttons & 0x100000) != 0;
            static int prev_tpb, click_up_next;
            if (in_game) {
               /* in a game a click is sent when a short press is let go, so the
                * 1 s hold that switches the mouse off clicks nothing */
               if (click_up_next) {
                  mouse_button(env, cls, px, py, 0, click_button);
                  click_up_next = 0;
               }
               if (!tpb && prev_tpb && !click_blocked) {
                  mouse_button(env, cls, px, py, 1, click_button);
                  click_up_next = 1;
                  if (presses++ < 40)
                     report("roblox: pad: touchpad click at %.0f,%.0f", px, py);
               }
               prev_tpb = tpb;
               if (!tpb)
                  click_blocked = 0;
               goto click_done;
            }
            prev_tpb = tpb;
            const int click = tpb && !click_blocked;
            if (!tpb)
               click_blocked = 0;
            if (click != clicked) {
               mouse_button(env, cls, px, py, (unsigned char)click, click_button);
               if (click && presses++ < 40)
                  report("roblox: pad: touchpad click at %.0f,%.0f", px, py);
               clicked = click;
            }
         click_done:;
         }
         /* sticks: -1..1 with a dead zone; Android y is down-positive and the
          * app sends -y; triggers 0..1 */
         float a[6];
         const uint8_t raw[4] = {st.lx, st.ly, st.rx, st.ry};
         for (int k = 0; k < 4; k++) {
            float v = (raw[k] - 128) / 127.0f;
            if (v > 1) v = 1;
            if (v < -1) v = -1;
            if (v > -0.12f && v < 0.12f) v = 0;
            a[k] = v;
         }
         a[4] = st.l2 / 255.0f;
         a[5] = st.r2 / 255.0f;
         /* anti-AFK on (L3+R3 hold, below): the camera turns right forever,
          * unless the right stick is being used */
         static int afk_on;
         if (afk_on && a[2] == 0 && a[3] == 0)
            a[2] = 0.35f;
         if (a[0] != last_ax[0] || a[1] != last_ax[1])
            axis(env, cls, RP_DEV, 0, a[0], -a[1], 0);
         if (a[2] != last_ax[2] || a[3] != last_ax[3])
            axis(env, cls, RP_DEV, 11, a[2], -a[3], 0);
         if (a[4] != last_ax[4])
            axis(env, cls, RP_DEV, 17, 0, 0, a[4]);
         if (a[5] != last_ax[5])
            axis(env, cls, RP_DEV, 18, 0, 0, a[5]);
         memcpy(last_ax, a, sizeof(a));
         /* Anti-AFK: hold L3 + R3 (both sticks pressed) for 1 s to switch it
          * on or off (light bar green and a PS5 pop-up). While on, the camera
          * spins right in a circle so Roblox's 20-min idle kick never comes. */
         {
            static uint64_t combo_since;
            const uint64_t t = now_ns();
            const int combo = (st.buttons & 0x0006) == 0x0006;
            if (combo) {
               if (!combo_since)
                  combo_since = t;
               else if (combo_since != 1 && t - combo_since > 1000000000ull) {
                  afk_on = !afk_on;
                  combo_since = 1; /* once per hold */
                  const DemoPadColor on = {0, 200, 0, 255}, off = {0, 0, 160, 255};
                  scePadSetLightBar(pad, afk_on ? &on : &off);
                  report("roblox: pad: anti-AFK %s", afk_on ? "ON" : "off");
                  ps5_notify(afk_on ? "Anti-AFK: ON - camera spinning (hold L3+R3 to stop)" : "Anti-AFK: OFF");
               }
            } else {
               combo_since = 0;
            }
         }
      }
      usleep(8000);
   }
   return NULL;
}

static void
roblox_fault(int sig, siginfo_t *info, void *context)
{
   if (roblox_syscall_trap(sig, (ucontext_t *)context))
      return;
   if (rk_guard) { /* a guarded system-keyboard call faulted: back to it */
      sigjmp_buf *g = rk_guard;
      rk_guard = NULL;
      report("roblox: keyboard: fault (signal %d) at rip %p caught", sig, (void *)((ucontext_t *)context)->uc_mcontext.mc_rip);
      siglongjmp(*g, 1);
   }
   report_sync(); /* the fault report below is written before the process ends */
   /* One thread reports; a fault in the reporting thread's own dump ends the
    * process, while a second thread that faults meanwhile waits (it used to
    * end the process before the first thread's report was written). */
   static volatile uintptr_t reporter;
   static __thread int reporting_here;
   const ucontext_t *uc = context;
   if (reporting_here) { /* a read in the dump below faulted */
      report("roblox: FAULT while reporting (signal %d)", sig);
      _exit(4);
   }
   const uintptr_t me = (uintptr_t)pthread_self();
   uintptr_t none = 0;
   if (!__atomic_compare_exchange_n(&reporter, &none, me, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
      report("roblox: FAULT signal %d in thread %x too, at rip %p (lib+0x%lx); waiting for the first report", sig,
             (unsigned)(me >> 4), (void *)uc->uc_mcontext.mc_rip,
             (unsigned long)((uintptr_t)uc->uc_mcontext.mc_rip - roblox_base));
      for (;;)
         sleep(60);
   }
   reporting_here = 1;
   const uintptr_t rip = (uintptr_t)uc->uc_mcontext.mc_rip;
   const uintptr_t rsp = (uintptr_t)uc->uc_mcontext.mc_rsp;
   const bool inside = rip >= roblox_base && rip < roblox_base + roblox_span;
   report("roblox: FAULT signal %d at rip %p (%s+0x%lx), address %p, rsp %p, thread %x", sig, (void *)rip,
          inside ? "libroblox.so" : "outside", inside ? (unsigned long)(rip - roblox_base) : 0ul,
          info ? info->si_addr : NULL, (void *)rsp, (unsigned)((uintptr_t)pthread_self() >> 4));
   /* The caller's return address on the stack, if it is in the library. */
   for (int i = 0; i < 64; i++) {
      const uintptr_t v = ((const uintptr_t *)rsp)[i];
      if (v >= roblox_base && v < roblox_base + roblox_span)
         report("roblox:    stack[%d] libroblox.so+0x%lx", i, (unsigned long)(v - roblox_base));
   }
   report("roblox:    rdi %p rsi %p rdx %p rcx %p rax %p rbx %p rbp %p", (void *)uc->uc_mcontext.mc_rdi,
          (void *)uc->uc_mcontext.mc_rsi, (void *)uc->uc_mcontext.mc_rdx, (void *)uc->uc_mcontext.mc_rcx,
          (void *)uc->uc_mcontext.mc_rax, (void *)uc->uc_mcontext.mc_rbx, (void *)uc->uc_mcontext.mc_rbp);
   report("roblox:    r8 %p r9 %p r10 %p r11 %p r12 %p r13 %p r14 %p r15 %p", (void *)uc->uc_mcontext.mc_r8,
          (void *)uc->uc_mcontext.mc_r9, (void *)uc->uc_mcontext.mc_r10, (void *)uc->uc_mcontext.mc_r11,
          (void *)uc->uc_mcontext.mc_r12, (void *)uc->uc_mcontext.mc_r13, (void *)uc->uc_mcontext.mc_r14,
          (void *)uc->uc_mcontext.mc_r15);
   report("roblox:    sync: lock-unknown %u unlock-unknown %u dropped-while-held %u",
          bionic_sync_locked_unknown, bionic_sync_unlocked_unknown, bionic_sync_dropped_locked);
   /* The frame-pointer chain: the library keeps rbp frames. */
   uintptr_t fp = (uintptr_t)uc->uc_mcontext.mc_rbp;
   for (int depth = 0; depth < 24 && fp > 0x10000 && (fp & 7) == 0; depth++) {
      const uintptr_t ret = ((const uintptr_t *)fp)[1];
      if (ret >= roblox_base && ret < roblox_base + roblox_span)
         report("roblox:    frame %d libroblox.so+0x%lx", depth, (unsigned long)(ret - roblox_base));
      else
         report("roblox:    frame %d %p", depth, (void *)ret);
      const uintptr_t next = ((const uintptr_t *)fp)[0];
      if (next <= fp)
         break;
      fp = next;
   }
   /* Last of all, because these read memory the library pointed at and a read
    * of a page that is mapped but not readable kills the rest of the report:
    * the stack window (this frame and its callers), then 16 words at every
    * register. On a corrupt-container fault this shows the object that holds
    * the bad field and what the words around it look like. */
   roblox_dump_stack((uintptr_t)uc->uc_mcontext.mc_rbp);
   {
      const __register_t vals[] = {uc->uc_mcontext.mc_r12, uc->uc_mcontext.mc_r13, uc->uc_mcontext.mc_rsi,
                                   uc->uc_mcontext.mc_rdi, uc->uc_mcontext.mc_r14, uc->uc_mcontext.mc_r15,
                                   uc->uc_mcontext.mc_rbx, uc->uc_mcontext.mc_rbp, uc->uc_mcontext.mc_rax,
                                   uc->uc_mcontext.mc_rcx, uc->uc_mcontext.mc_rdx, uc->uc_mcontext.mc_r8,
                                   uc->uc_mcontext.mc_r9,  uc->uc_mcontext.mc_r10, uc->uc_mcontext.mc_r11};
      static const char *const names[] = {"r12", "r13", "rsi", "rdi", "r14", "r15", "rbx", "rbp",
                                          "rax", "rcx", "rdx", "r8",  "r9",  "r10", "r11"};
      for (unsigned i = 0; i < sizeof(vals) / sizeof(vals[0]); i++)
         roblox_dump_words(names[i], (uintptr_t)vals[i], 16);
   }
   reporting_here = 0; /* the reads above are done; the repair path below may retry */
   /* Ground truth for an instruction-fetch fault (address == rip on the PS5):
    * is the page still mapped, and does giving it execute permission let the
    * thread carry on? */
   if (sig == SIGBUS && info && (uintptr_t)info->si_addr == rip) {
      const uintptr_t page = rip & ~(uintptr_t)16383;
      static volatile int retried;
      unsigned char vec[1];
      char check[64] = "";
      if (mincore((void *)page, 16384, (char *)vec) != 0) {
         snprintf(check, sizeof(check), "not mapped (errno %d)", errno);
      }
      report("roblox:    instruction page %p: %s", (void *)page,
             check[0] ? check : (vec[0] & 1 ? "resident" : "mapped, not resident"));
      /* Put the original bytes of that page back, from the copy of the file
       * in memory, and try the instruction again. */
      /* Diagnostic only: compare (never write) the page against the file. A
       * code page's relocated bytes legitimately differ, so only a large
       * difference means anything. */
      if (roblox_image_file && rip >= roblox_base) {
         const uintptr_t vaddr = (uintptr_t)page - roblox_base;
         const uint8_t *src = NULL;
         size_t len = 0;
         const struct roblox_phdr *ph = bionic_lib_phdr;
         for (unsigned i = 0; i < bionic_lib_phnum; i++)
            if (ph[i].p_type == 1 /* PT_LOAD */ && vaddr >= ph[i].p_vaddr &&
                vaddr < ph[i].p_vaddr + ph[i].p_filesz) {
               src = roblox_image_file + ph[i].p_offset + (vaddr - ph[i].p_vaddr);
               len = ph[i].p_filesz - (vaddr - ph[i].p_vaddr);
               if (len > 16384)
                  len = 16384;
            }
         if (src && vaddr + len <= roblox_image_file_size) {
            size_t diff = 0;
            for (size_t k = 0; k < len; k++)
               diff += ((const uint8_t *)page)[k] != src[k];
            report("roblox:    page vs file image: %zu of %zu bytes differ", diff, len);
         }
      }
      if (retried++ < 3) {
         const int prc = mprotect((void *)page, 16384, PROT_READ | PROT_WRITE | PROT_EXEC);
         report("roblox:    mprotect(%p, 16384, RWX) = %d (errno %d); retrying the instruction",
                (void *)page, prc, prc ? errno : 0);
         return;
      }
      report("roblox:    the same page faulted again after mprotect: giving up");
   }
   _exit(3);
}

static void
roblox_install_fault_report(void)
{
   /* This thread too gets an alternate stack, so a stack overflow on it
    * still reaches the report. */
   stack_t ss;
   ss.ss_size = 256 * 1024;
   ss.ss_sp = malloc(ss.ss_size);
   ss.ss_flags = 0;
   if (ss.ss_sp)
      sigaltstack(&ss, NULL);
   struct sigaction sa;
   memset(&sa, 0, sizeof(sa));
   sa.sa_sigaction = roblox_fault;
   sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
   const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};
   for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
      if (sigaction(sigs[i], &sa, NULL) != 0)
         report("roblox: sigaction(%d) failed, errno %d", sigs[i], errno);
}


static void
roblox_fatal_hook(int kind, const char *message)
{
   report("roblox: FATAL from the engine (kind %d): %s", kind, message ? message : "(null)");
   {
      char line[600];
      size_t at = (size_t)snprintf(line, sizeof(line), "roblox: FATAL call chain:");
      uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
      for (int d = 0; d < 24 && fp > 0x10000 && (fp & 7) == 0 && at < sizeof(line) - 24; d++) {
         const uintptr_t ret = ((const uintptr_t *)fp)[1];
         if (ret >= roblox_base && ret < roblox_base + roblox_span)
            at += (size_t)snprintf(line + at, sizeof(line) - at, " %lx", (unsigned long)(ret - roblox_base));
         else
            at += (size_t)snprintf(line + at, sizeof(line) - at, " .");
         const uintptr_t next = ((const uintptr_t *)fp)[0];
         if (next <= fp)
            break;
         fp = next;
      }
      report("%s", line);
   }
   bionic_park("engine fatal error", kind, __builtin_return_address(0));
}

static void
roblox_before_constructors(const struct ael_library *lib)
{
   /* The engine's fatal-error function (lib+0x68f17a7, (int kind, const char
    * *message)) ends in a trap; say its message and park the thread instead. */
   /* ROBLOX_PURE=1: the engine's code is left exactly as shipped (no fatal
    * hook, no syscall or %fs:0 rewrites); %fs:0 is made a per-thread self
    * value instead (bionic_fs_self), as Android's TLS has it. */
   const bool pure = getenv("ROBLOX_PURE") != NULL;
   if (pure && getenv("ROBLOX_PURE_SYSCALLS"))
      report("roblox: pure mode: only raw syscall sites are modified");
   else if (pure)
      report("roblox: pure mode: the engine's code is not modified");
   if (!pure) {
      uint8_t *at = (uint8_t *)lib->base + 0x68f17a7;
      const uint64_t to = (uint64_t)(uintptr_t)roblox_trampoline((void *)roblox_fatal_hook);
      at[0] = 0x48, at[1] = 0xb8; /* movabs rax, hook; jmp rax */
      memcpy(at + 2, &to, 8);
      at[10] = 0xff, at[11] = 0xe0;
   }
   /* ROBLOX_NOFREE=1 (diagnostic): the engine's own allocator never frees
    * (its core free routine, lib+0x1a9432b, returns at once). A use-after-
    * free or double free cannot happen then; a wild write still can. */
   if (!getenv("ROBLOX_RAW_SYSCALLS") && (!pure || getenv("ROBLOX_PURE_SYSCALLS")))
      roblox_syscall_patch((uint8_t *)lib->base);
   if (!getenv("ROBLOX_KEEP_FS0") && !pure)
      roblox_fs0_patch((uint8_t *)lib->base);
   if (pure) {
      bionic_fs_self_on = 1;
      bionic_fs_self();
      report("roblox: %%fs:0 on the main thread is now %p", bionic_fs0_read());
   }
   /* The app shell's frame-rate throttle: setThrottleFramerateEnabled
    * (lib+0x2395236) stores its flag in [object+0x1e9]; with it on, the
    * engine draws about 1 frame a second (meant for a phone app left idle in
    * the background). Store 0 instead: "mov byte [rdi+0x1e9], 0" is the same
    * 7 bytes. ROBLOX_ALLOW_THROTTLE=1 keeps the original. */
   if (!pure && !getenv("ROBLOX_ALLOW_THROTTLE")) {
      uint8_t *at = (uint8_t *)lib->base + 0x2395236;
      static const uint8_t want[7] = {0x40, 0x88, 0xb7, 0xe9, 0x01, 0x00, 0x00};
      if (memcmp(at, want, 7) == 0) {
         static const uint8_t off[7] = {0xc6, 0x87, 0xe9, 0x01, 0x00, 0x00, 0x00};
         memcpy(at, off, 7);
         report("roblox: frame-rate throttle (idle 1 fps) disabled");
      } else {
         report("roblox: throttle setter bytes differ (%02x %02x %02x); not changed", at[0], at[1], at[2]);
      }
   }
   /* The engine's own surface update, nativeActivity_onSurfaceChanged
    * (lib+0x29d2722), always returns at once, as it did on every good run:
    * "cmpb $0,0x299(%rbx); je <log Flags-Not-Received, return>" at
    * lib+0x29d27a9 becomes "jmp <that>". When the flags were in before the
    * menus' DataModel loaded (a start-up race; slow synchronous logging used
    * to hide it) it ran with the window's values instead: screen DPI scale 0,
    * the menus laid out magnified and blank, or the camera dividing by 0
    * (SIGFPE at lib+0x50d778f). The app shell's
    * UpdateSurfaceAppWithPlatformParams sets the surface and scale instead.
    * ROBLOX_KEEP_SURFACE_CHANGED=1 keeps the original. */
   if (!pure && !getenv("ROBLOX_KEEP_SURFACE_CHANGED")) {
      uint8_t *at = (uint8_t *)lib->base + 0x29d27a9;
      static const uint8_t want[13] = {0x80, 0xbb, 0x99, 0x02, 0x00, 0x00, 0x00, 0x0f, 0x84, 0x23, 0x01, 0x00, 0x00};
      if (memcmp(at, want, sizeof(want)) == 0) {
         static const uint8_t skip[13] = {0xe9, 0x2b, 0x01, 0x00, 0x00, /* jmp lib+0x29d28d9 */
                                          0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
         memcpy(at, skip, sizeof(skip));
         report("roblox: engine's own surface update skipped (lib+0x29d27a9)");
      } else {
         report("roblox: surface-update check bytes differ (%02x %02x %02x); not changed", at[0], at[1], at[2]);
      }
   }
   /* The screen's DPI scale is never 0 either. Its setter (lib+0x228b5d2,
    * logs "Setting screen DPI scale to %.2f (was %.2f)", stores at
    * [this+0xd60]) got 0 from the surface update above. Its first 32 bytes,
    * a check against a flag-set override value, become "xorps xmm1,xmm1;
    * ucomiss xmm0,xmm1; jbe <return without storing>; jmp <the rest>": a
    * scale <= 0 or NaN is ignored and the previous one (1.0) kept.
    * ROBLOX_ALLOW_DPI_ZERO=1 keeps the original. */
   if (!pure && !getenv("ROBLOX_ALLOW_DPI_ZERO")) {
      uint8_t *at = (uint8_t *)lib->base + 0x228b5f2;
      static const uint8_t want[32] = {0x80, 0x3d, 0xe7, 0x78, 0xc4, 0x04, 0x00, 0x74, 0x17, 0xf3, 0x0f,
                                       0x2a, 0x0d, 0xf5, 0x78, 0xc4, 0x04, 0x0f, 0x2e, 0xc8, 0x0f, 0x85,
                                       0x81, 0x01, 0x00, 0x00, 0x0f, 0x8a, 0x7b, 0x01, 0x00, 0x00};
      if (memcmp(at, want, 32) == 0) {
         uint8_t code[32];
         memset(code, 0x90, sizeof(code));
         static const uint8_t guard[14] = {0x0f, 0x57, 0xc9,                   /* xorps %xmm1,%xmm1 */
                                           0x0f, 0x2e, 0xc1,                   /* ucomiss %xmm1,%xmm0 */
                                           0x0f, 0x86, 0x8f, 0x01, 0x00, 0x00, /* jbe lib+0x228b78d */
                                           0xeb, 0x12};                        /* jmp lib+0x228b612 */
         memcpy(code, guard, sizeof(guard));
         memcpy(at, code, sizeof(code));
         report("roblox: screen DPI scale: 0 is ignored (lib+0x228b5f2)");
      } else {
         report("roblox: DPI setter bytes differ (%02x %02x %02x); not changed", at[0], at[1], at[2]);
      }
   }
   /* PlayStation button names and icons. The engine's GetStringForKeyCode
    * (lib+0x22db9d4) reads the input service's gamepad type (+0x43c): 1-2
    * give PlayStation names (ButtonCross, ButtonCircle...), 3 Xbox, anything
    * else the generic ButtonA/ButtonB, which the app's Lua (FoundationUI
    * detectInputPlatform: GetStringForKeyCode(ButtonA) == "ButtonCross")
    * then draws with Xbox letters. The type our DualSense connect event
    * reports does not reach that field, so "lea ecx,[rax-1]" becomes
    * "xor ecx,ecx; nop": always the PlayStation names (a DualSense is the
    * only controller here). ROBLOX_XBOX_GLYPHS=1 keeps the original. */
   if (!pure && !getenv("ROBLOX_XBOX_GLYPHS")) {
      uint8_t *at = (uint8_t *)lib->base + 0x22dba01;
      static const uint8_t want[6] = {0x8d, 0x48, 0xff, 0x83, 0xf9, 0x01};
      if (memcmp(at, want, 6) == 0) {
         at[0] = 0x31, at[1] = 0xc9, at[2] = 0x90;
         report("roblox: button names: PlayStation (Cross, Circle, Square, Triangle)");
      } else {
         report("roblox: button-name check bytes differ (%02x %02x %02x); not changed", at[0], at[1], at[2]);
      }
      /* GetImageForKeyCode's image path (lib+0x22db554) reads the same field:
       * 1-2 PlayStationController/ (2: its PS5/ Options and Create), 3
       * XboxController/. The app's Lua checks these paths first, before the
       * names. "mov r12d,[r12+0x43c]" becomes "mov r12d,2; nop": PS5 images
       * (shipped in assets/content/textures/ui/Controls/PlayStationController,
       * tools/make-ps-button-icons.py; the APK has none). */
      uint8_t *img = (uint8_t *)lib->base + 0x22db5ad;
      static const uint8_t iwant[8] = {0x45, 0x8b, 0xa4, 0x24, 0x3c, 0x04, 0x00, 0x00};
      if (memcmp(img, iwant, 8) == 0) {
         static const uint8_t ps5[8] = {0x41, 0xbc, 0x02, 0x00, 0x00, 0x00, 0x66, 0x90};
         memcpy(img, ps5, 8);
         report("roblox: button images: PlayStation 5");
      } else {
         report("roblox: button-image check bytes differ (%02x %02x %02x); not changed", img[0], img[1], img[2]);
      }
   }
   if (!pure && !getenv("ROBLOX_NO_FOCUS_HOOK"))
      roblox_focus_hooks_install((uint8_t *)lib->base);
   /* The app's UI model (UniversalApp.rbxm) carries Roblox's signatures; ours
    * is changed (Guide menu item and theme, tools/patch-ui.sh), so the
    * check's list of failures (lib+0x2153871) is treated as empty: the
    * "je no_errors" after it (lib+0x626ea3b, 0f 84 rel32) becomes a jmp.
    * Without this the app logs "Signature verification failed" and stops
    * (engine fatal error). */
   uint8_t *at = (uint8_t *)lib->base + 0x626ea3b;
   if (at[0] == 0x0f && at[1] == 0x84) {
      at[0] = 0x90, at[1] = 0xe9; /* nop; jmp rel32 (same target) */
      report("roblox: UI model signature check skipped (lib+0x626ea3b)");
   } else if (!(at[0] == 0x90 && at[1] == 0xe9)) {
      report("roblox: UI model signature check: bytes differ (%02x %02x); not patched", at[0], at[1]);
   }
   /* and its result, "some signature verified" (mov -0x109(%rbp),%bl at
    * lib+0x626ec00), is true: when false the caller (lib+0x625e312) raises
    * an engine fatal error ("No signatures in the model could be verified") */
   static const uint8_t want[6] = {0x8a, 0x9d, 0xf7, 0xfe, 0xff, 0xff};
   uint8_t *ok = (uint8_t *)lib->base + 0x626ec00;
   if (memcmp(ok, want, 6) == 0) {
      static const uint8_t yes[6] = {0xb3, 0x01, 0x90, 0x90, 0x90, 0x90}; /* mov $1,%bl */
      memcpy(ok, yes, 6);
   } else if (!(ok[0] == 0xb3 && ok[1] == 0x01)) {
      report("roblox: UI model signature result: bytes differ; not patched");
   }
   if (!getenv("ROBLOX_NO_VULKAN"))
      bionic_vulkan_dlsym = rv_dlsym;
   roblox_base = (uintptr_t)lib->base;
   roblox_span = lib->span;
   roblox_install_fault_report();
   bionic_lib_base = (uintptr_t)lib->base;
   bionic_lib_phdr = lib->phdr;
   bionic_lib_phnum = lib->phnum;
   report("roblox: dl_iterate_phdr will report base %p, %u program headers", (void *)lib->base, lib->phnum);
}

/* Step 6: what Android does when the Roblox activity is created. Its Java
 * GameActivity.onCreate calls initializeNativeCode(internalDataDir, obbDir,
 * externalDataDir, assetManager, savedState, configuration); the native side
 * starts the game thread (android_main) and registers the activity's
 * callbacks, which are then called in Android's order: start, resume. */
static void
roblox_start_activity(struct ael_library *lib, void *vm)
{
   (void)vm;
   void *init = ael_symbol(lib, "Java_com_google_androidgamesdk_GameActivity_initializeNativeCode");
   check(init != NULL, "roblox: GameActivity's initializeNativeCode is found");
   if (!init)
      return;
   void *env = fj_env_ptr();
   bionic_track_openat = getenv("ROBLOX_OPENAT_TEST") != NULL;
   if (bionic_track_openat)
      report("roblox: experimental directory tracking and raw openat enabled");
   struct fj_obj *activity = fj_new(FJ_OBJECT, "MainGameActivity", NULL);
   activity->cls = fj_class("com/roblox/client/startup/MainGameActivity");
   struct fj_obj *assets = fj_new(FJ_OBJECT, "AssetManager", NULL);
   assets->cls = fj_class("android/content/res/AssetManager");
   struct fj_obj *config = fj_new(FJ_OBJECT, "Configuration", NULL);
   config->cls = fj_class("android/content/res/Configuration");
   void *files = fj_NewStringUTF(env, "/data/roblox/files");
   void *obb = fj_NewStringUTF(env, "/data/roblox/obb");
   void *external = fj_NewStringUTF(env, "/data/roblox/external");
   mkdir("/data/roblox", 0777);
   mkdir("/data/roblox/files", 0777);
   mkdir("/data/roblox/obb", 0777);
   mkdir("/data/roblox/external", 0777);
   mkdir("/data/roblox", 0777);
   mkdir("/data/roblox/files", 0777);
   mkdir("/data/roblox/cache", 0777);
   /* the app extracts its CA bundle (assets/ssl/cacert.pem) to files/exe/ssl on
    * first start; curl reads it from there for every HTTPS request */
   {
      mkdir("/data/roblox/files/exe", 0777);
      mkdir("/data/roblox/files/exe/ssl", 0777);
      struct stat st;
      if (stat("/data/roblox/files/exe/ssl/cacert.pem", &st) != 0 || st.st_size == 0) {
         FILE *in = fopen("/app0/assets/ssl/cacert.pem", "rb");
         FILE *out = in ? fopen("/data/roblox/files/exe/ssl/cacert.pem", "wb") : NULL;
         size_t total = 0;
         if (in && out) {
            char buf[16384];
            size_t k;
            while ((k = fread(buf, 1, sizeof(buf), in)) > 0)
               total += fwrite(buf, 1, k, out);
         }
         if (out)
            fclose(out);
         if (in)
            fclose(in);
         report("roblox: CA bundle copied to files/exe/ssl/cacert.pem: %zu bytes", total);
      }
   }
   /* Console ("ten-foot") interface: the layout Roblox uses on Xbox and
    * PlayStation - big focusable tiles driven by a controller, not the phone
    * layout. Engine flags are read from files/exe/ClientSettings/
    * ClientAppSettings.json (unknown names are ignored, so both prefixes).
    * ROBLOX_MOBILE_UI=1 leaves the phone layout. */
   {
      mkdir("/data/roblox/files/exe/ClientSettings", 0777);
      FILE *cs = fopen("/data/roblox/files/exe/ClientSettings/ClientAppSettings.json", "wb");
      if (cs) {
         const bool tv = !getenv("ROBLOX_MOBILE_UI");
         fprintf(cs, "{\n  \"FFlagForceTenFootInterface\": %s,\n  \"DFFlagForceTenFootInterface\": %s",
                 tv ? "true" : "false", tv ? "true" : "false");
         /* Seamless forced OTA update: with it, starting a game from the menus
          * after an earlier game (stage LuaApp) tears the menus' render view
          * down; the new view had no DPI scale (0) and the game's camera
          * divided by it (SIGFPE at lib+0x50d778f). ROBLOX_KEEP_OTA_FLAG=1
          * keeps it. */
         /* the console Settings page: without the unified page the app's
          * useSettingsPageItems reads item.itemInfo.itemType of an item that
          * has no itemInfo, React drops the whole menu (grey screen) */
         if (tv && !getenv("ROBLOX_OLD_SETTINGS_PAGE"))
            fprintf(cs, ",\n  \"FFlagUnifyConsoleSettingsPage9\": true");
         if (!getenv("ROBLOX_KEEP_OTA_FLAG"))
            fprintf(cs, ",\n  \"FFlagEnableSeamlessForceOTAUpdate2\": false,\n"
                        "  \"DFFlagEnableSeamlessForceOTAUpdate2\": false");
         fprintf(cs, "\n}\n");
         fclose(cs);
         report("roblox: ClientAppSettings.json written: console (ten-foot) interface %s", tv ? "ON" : "off");
      }
   }
   /* When the PS5 refused an audio port, the app saved "NoSound Driver" as
    * the chosen output device and kept selecting it later; forget it so the
    * real output is used. */
   {
      const char *path = "/data/roblox/files/appData/LocalStorage/appStorage.json";
      FILE *f = fopen(path, "rb");
      char *buf = NULL;
      long n = 0;
      if (f) {
         fseek(f, 0, SEEK_END);
         n = ftell(f);
         fseek(f, 0, SEEK_SET);
         buf = n > 0 ? malloc((size_t)n + 1) : NULL;
         if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
            free(buf);
            buf = NULL;
         }
         fclose(f);
      }
      static const char bad[] = "\"SelectedOutputDeviceName\":\"NoSound Driver\"";
      char *hit = buf ? (buf[n] = 0, strstr(buf, bad)) : NULL;
      if (hit) {
         static const char good[] = "\"SelectedOutputDeviceName\":\"\"";
         FILE *o = fopen("/data/roblox/files/appData/LocalStorage/appStorage.json.new", "wb");
         if (o) {
            fwrite(buf, 1, (size_t)(hit - buf), o);
            fwrite(good, 1, sizeof(good) - 1, o);
            fwrite(hit + sizeof(bad) - 1, 1, (size_t)(n - (hit - buf) - (long)(sizeof(bad) - 1)), o);
            fclose(o);
            rename("/data/roblox/files/appData/LocalStorage/appStorage.json.new", path);
            report("roblox: audio: saved output device \"NoSound Driver\" cleared");
         }
      }
      free(buf);
   }
   ra_restore_at_start(); /* the account switcher's list (roblox_accounts.h) */
   report("roblox: working directory /data/roblox/files: chdir %d", chdir("/data/roblox/files"));
   const unsigned natives_before = fj_native_count, traps_before = roblox_trap_calls;
   /* Android's UI thread always has a looper; GameActivity asks for it. */
   bionic_ALooper_prepare(0);
   /* What RobloxApplication.onCreate and MainGameActivity.onCreate call into
    * the library before GameActivity starts, in the app's order. */
   {
      struct fj_obj *app = fj_new(FJ_OBJECT, "RobloxApplication", NULL);
      app->cls = fj_class("com/roblox/client/RobloxApplication");
      struct {
         const char *symbol;
         int kind; /* 0 (env, cls, obj); 1 (env, cls, assets); 2 storage; 3 (env, cls, string); 4 (env, cls, bool) */
         const char *text;
      } steps[] = {
         {"Java_com_roblox_client_flags_FlagJniInterface_nativeInitializeNativeFlags", 5, NULL},
         {"Java_com_roblox_client_JNIAAssetManagerSetup_initNative", 1, NULL},
         {"Java_com_roblox_client_LocalStorageManager_initStorageManagerNativeV3", 2, NULL},
         {"Java_com_roblox_universalapp_linking_JNIBaseUrlProtocol_init", 0, NULL},
         {"Java_com_roblox_universalapp_linking_JNIWebLoginProtocol_init", 0, NULL},
         {"Java_com_roblox_client_startup_MainGameActivity_nativeSetAssetPath", 3, "/app0/assets/content"},
         {"Java_com_roblox_client_startup_MainGameActivity_nativeEnableVersionBFlagCache", 4, NULL},
         {"Java_com_roblox_client_startup_MainGameActivity_nativePreloadFlagOverrides", 3, "{}"},
      };
      void *cls = fj_class("java/lang/Object");
      for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
         void *fn = ael_symbol(lib, steps[i].symbol);
         const unsigned t0 = roblox_trap_calls;
         if (!fn) {
            report("roblox: app init %s: not found", steps[i].symbol + 5);
            continue;
         }
         report("roblox: app init %s: calling", steps[i].symbol + 5);
         switch (steps[i].kind) {
         case 0: ((void (*)(void *, void *, void *))fn)(env, cls, app); break;
         case 1: ((void (*)(void *, void *, void *))fn)(env, cls, assets); break;
         case 2:
            ((void (*)(void *, void *, void *, void *, void *))fn)(env, app, assets,
                                                                  fj_NewStringUTF(env, "/data/roblox/files"),
                                                                  fj_NewStringUTF(env, "/data/roblox/cache"));
            break;
         case 3: ((void (*)(void *, void *, void *))fn)(env, cls, fj_NewStringUTF(env, steps[i].text)); break;
         case 4: ((void (*)(void *, void *, unsigned char))fn)(env, cls, 0); break;
         case 5: {
            /* FlagSettings: no cached flags on a first run (an empty String[]). */
            void *r = ((void *(*)(void *, void *, void *))fn)(env, cls, fj_NewObjectArray(env, 0, NULL, NULL));
            report("roblox: native flags initialised: result %s", r ? "object" : "null");
            break;
         }
         }
         report("roblox: app init %s: returned, %u calls to things not provided", steps[i].symbol + 5,
                roblox_trap_calls - t0);
      }
      mkdir("/data/roblox/cache", 0777);
   }
   rw_setup(lib, env); /* web pages in the PS5 browser (roblox_webview.h) */
   report("roblox: calling GameActivity initializeNativeCode");
   typedef long long (*init_fn)(void *, void *, void *, void *, void *, void *, void *, void *);
   const long long handle = ((init_fn)init)(env, activity, files, obb, external, assets, NULL, config);
   report("roblox: initializeNativeCode returned handle 0x%llx; %u activity callbacks registered, %u calls to things "
          "not provided",
          (unsigned long long)handle, fj_native_count - natives_before, roblox_trap_calls - traps_before);
   check(handle != 0, "roblox: the Roblox activity is created and its game thread started");
   if (!handle)
      return;
   {
      /* Probe the dispatcher the recursing routine (lib+0x68eeeec) uses when
       * the thread has no task context: services at keys lib+0x6e71a28/48. */
      typedef void *(*get_fn)(void);
      void *a = ((get_fn)(roblox_base + 0x291f610))();
      void *b = ((get_fn)(roblox_base + 0x291f630))();
      const int inctx = ((int (*)(void *, void *))(roblox_base + 0x29381b0))(NULL, NULL) & 1;
      report("roblox: probe main thread %p: task context %d; service A %p, service B %p", (void *)pthread_self(),
             inctx, a, b);
      if (a)
         report("roblox: probe A+0x18 = %p", *(void **)((char *)a + 0x18));
      if (b)
         for (int k = 0; k < 6; k++) {
            const uintptr_t v = ((uintptr_t *)b)[k];
            report("roblox: probe B[%d] = %p (lib+0x%lx)", k, (void *)v,
                   v >= roblox_base && v < roblox_base + roblox_span ? (unsigned long)(v - roblox_base) : 0ul);
         }
      if (b && *(void **)b) {
         void **table = *(void ***)b;
         for (int k = 226; k <= 230; k++) {
            const uintptr_t f = (uintptr_t)table[k];
            report("roblox: probe B table[%d] = %p (lib+0x%lx)", k, (void *)f,
                   f >= roblox_base ? (unsigned long)(f - roblox_base) : 0ul);
         }
      }
   }
   {
      /* MainGameActivity.bootstrapTheApp -> (executor) C2 -> F2: device info,
       * then the engine's InitParams. */
      void *dev = ael_symbol(lib, "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeSetDeviceInfo");
      void *init_params = ael_symbol(lib, "Java_com_roblox_client_startup_MainGameActivity_nativeAppBridgeSetInitParams");
      struct fj_obj *dp = fj_new(FJ_OBJECT, "DeviceParams", NULL);
      dp->cls = fj_class("com/roblox/engine/jni/model/DeviceParams");
      struct fj_obj *ip = fj_new(FJ_OBJECT, "InitParams", NULL);
      ip->cls = fj_class("com/roblox/engine/jni/autovalue/InitParams");
      const unsigned t0 = roblox_trap_calls;
      report("roblox: bootstrap: nativeSetDeviceInfo %s", dev ? "calling" : "missing");
      if (dev)
         ((void (*)(void *, void *, void *))dev)(env, fj_class("com/roblox/engine/jni/NativeSettingsInterface"), dp);
      report("roblox: bootstrap: nativeAppBridgeSetInitParams %s", init_params ? "calling" : "missing");
      if (init_params)
         ((void (*)(void *, void *, void *))init_params)(env, fj_class("com/roblox/client/startup/MainGameActivity"), ip);
      report("roblox: bootstrap: done, %u calls to things not provided", roblox_trap_calls - t0);

      /* The app shell (InitHelper.startAppBridge, then ActivityNativeMain):
       * start the AppBridge, init the data model, start the app on a
       * surface, and load the Lua app. */
      void *bridge = fj_class("com/roblox/engine/jni/NativeAppBridgeInterface");
      void *gl = fj_class("com/roblox/engine/jni/NativeGLInterface");
      struct fj_obj *surface = fj_new(FJ_OBJECT, "Surface", NULL);
      surface->cls = fj_class("android/view/Surface");
      struct fj_obj *sp = fj_new(FJ_OBJECT, "StartAppParams", NULL);
      sp->cls = fj_class("com/roblox/engine/jni/autovalue/StartAppParams");
      struct fj_obj *pp = fj_new(FJ_OBJECT, "PlatformParams", NULL);
      pp->cls = fj_class("com/roblox/engine/jni/model/PlatformParams");
      struct {
         const char *symbol;
         int kind;
      } shell[] = {
         {"Java_com_roblox_engine_jni_NativeAppBridgeInterface_setIsFirstInstall", 0},
         {"Java_com_roblox_engine_jni_NativeAppBridgeInterface_nativeAppBridgeAppStart__Ljava_lang_String_2Ljava_lang_String_2ZLjava_lang_String_2Ljava_lang_String_2Ljava_lang_String_2", 1},
         {"Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeV2InitWithParams", 2},
         {"Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeV2StartAppWithParams", 3},
         {"Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeStartLuaAppDM", 4},
         {"Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeV2UpdateSurfaceAppWithPlatformParams", 5},
      };
      {
         /* RobloxApplication / InitHelper settings (a1.X0, a1.U0, k0$a), then
          * GetClientSettingsTask: client settings (none: Roblox's servers are
          * not reachable from here), which initialises the TaskScheduler. */
         struct fj_obj *list = fj_new(FJ_OBJECT, "ArrayList", NULL);
         list->cls = fj_class("java/util/ArrayList");
         struct {
            const char *name;
            int n;
            const char *a[3];
            int returns_int;
         } st[] = {
            {"NativeSettingsInterface_nativeSetCacheDirectory", 1, {"/data/roblox/cache"}, 0},
            {"NativeSettingsInterface_nativeSetFilesDirectory", 1, {"/data/roblox/files"}, 0},
            {"NativeSettingsInterface_nativeSetBaseDataDirectories", 2, {"/data/roblox", ""}, 0},
            {"NativeSettingsInterface_nativeSetExternalDirectory", 1, {"/data/roblox/external"}, 0},
            {"NativeSettingsInterface_nativeSetBaseUrl", 2, {"https://www.roblox.com/", "roblox.com"}, 0},
            {"NativeSettingsInterface_nativeSetRobloxChannel", 1, {""}, 0},
            {"NativeSettingsInterface_nativeSetRobloxVersion", 1, {"2.746.1266"}, 0},
            {"NativeSettingsInterface_nativeInitFastLog", 0, {NULL}, 0},
            {"NativeGLInterface_nativeInitClientSettings", 3, {"{\"applicationSettings\":{}}", "", "AndroidApp"}, 1},
            {"NativeGLInterface_nativePostClientSettingsLoadedInitialization3", -1, {NULL}, 0},
         };
         mkdir("/data/roblox/external", 0777);
         /* Roblox's live flags for the Android client (clientsettingscdn
          * .../v2/settings/application/AndroidApp, fetched on the PC through
          * the gateway): the app downloads them before starting the engine.
          * With an empty set, game servers send packets for features the
          * client has off ("Received ISR Packet without ISR being enabled")
          * and the join fails. */
         {
            FILE *cf = fopen("/data/roblox/files/clientsettings.json", "rb");
            if (cf) {
               fseek(cf, 0, SEEK_END);
               const long n = ftell(cf);
               fseek(cf, 0, SEEK_SET);
               char *json = n > 0 ? malloc((size_t)n + 1) : NULL;
               if (json && fread(json, 1, (size_t)n, cf) == (size_t)n) {
                  json[n] = 0;
                  /* our own flag values, appended to applicationSettings (the
                   * last copy of a name wins): ClientAppSettings.json only
                   * takes Roblox's small allowlist. ROBLOX_NO_FLAG_PATCH=1
                   * leaves the set as downloaded. */
                  static const char extra[] =
                     /* the console Settings page: without the unified page the
                      * app's useSettingsPageItems reads item.itemInfo.itemType
                      * of an item with no itemInfo and React drops the whole
                      * menu (grey screen) */
                     ", \"FFlagUnifyConsoleSettingsPage9\": \"True\""
                     /* see the pad thread: tears the menus' render view down on
                      * a second game (SIGFPE at lib+0x50d778f) */
                     ", \"FFlagEnableSeamlessForceOTAUpdate2\": \"False\""
                     /* frame rate above 60 when the display runs at 120 Hz
                      * (Roblox also has its own Maximum Frame Rate setting) */
                     ", \"DFIntTaskSchedulerTargetFps\": \"120\""
                     /* games aimed at 16 ms frames (60 fps) even with the
                      * display at 120 Hz and FramerateCap 120 */
                     ", \"DFIntGraphicsOptimizationModeMinFrameTimeTargetMs\": \"8\""
                     ", \"DFIntGraphicsOptimizationModeFRMFrameRateTarget\": \"120\""
                     /* use the user's Maximum Frame Rate (GlobalBasicSettings
                      * FramerateCap) in games; off on Android, games stayed
                      * at 60 with the display at 120 Hz */
                     ", \"FFlagGameBasicSettingsFramerateCap\": \"True\""
                     ", \"DFFlagGameBasicSettingsFramerateCap\": \"True\"";
                  char *end = strrchr(json, '}');
                  char *inner = end ? end - 1 : NULL;
                  while (inner && inner > json && *inner != '}')
                     inner--;
                  char *patched = inner && inner > json && !getenv("ROBLOX_NO_FLAG_PATCH")
                                     ? malloc((size_t)n + sizeof(extra) + 1) : NULL;
                  if (patched) {
                     const size_t head = (size_t)(inner - json);
                     memcpy(patched, json, head);
                     memcpy(patched + head, extra, sizeof(extra) - 1);
                     strcpy(patched + head + sizeof(extra) - 1, inner);
                     free(json);
                     json = patched;
                     report("roblox: client settings: our flags appended (%s)", extra);
                  }
                  for (unsigned k = 0; k < sizeof(st) / sizeof(st[0]); k++)
                     if (strcmp(st[k].name, "NativeGLInterface_nativeInitClientSettings") == 0)
                        st[k].a[0] = json;
                  report("roblox: client settings: Roblox's live Android flags, %ld bytes", n);
               }
               fclose(cf);
            } else {
               report("roblox: client settings: none saved; empty set");
            }
         }
         /* the display's refresh rates, as Android's app passes them (uh.t0);
          * read when the task scheduler starts (nativeInitClientSettings
          * below), so before that. The 119.88 Hz mode is only known once the
          * surface exists, later: it is offered here unless ROBLOX_HZ=60. */
         {
            void (*pass_rates)(void *, void *, void *) =
               ael_symbol(lib, "Java_com_roblox_engine_jni_NativeGLInterface_nativePassSupportedRefreshRates");
            void (*pass_current)(void *, void *, float) =
               ael_symbol(lib, "Java_com_roblox_engine_jni_NativeGLInterface_nativePassCurrentDisplayRefreshRate");
            const char *hzs = getenv("ROBLOX_HZ");
            const float hz = hzs && atoi(hzs) <= 60 ? 59.94f : 119.88f;
            void *glc = fj_class("com/roblox/engine/jni/NativeGLInterface");
            if (pass_rates) {
               struct fj_obj *arr = fj_array(hz > 61 ? 2 : 1, sizeof(float), "[F");
               if (arr) {
                  ((float *)arr->data)[0] = 59.94f;
                  if (hz > 61)
                     ((float *)arr->data)[1] = hz;
                  pass_rates(env, glc, arr);
               }
            }
            if (pass_current)
               pass_current(env, glc, hz);
            report("roblox: display refresh rate %.2f Hz passed before the task scheduler starts", hz);
            /* engine feature ApplicationFrameRatePolicy (lib+0x6f16da0): the
             * task scheduler then paces by the display's rate instead of a
             * fixed 60. ROBLOX_NO_APP_FPS_POLICY=1 leaves it as it was. */
            if (hz > 61 && getenv("ROBLOX_APP_FPS_POLICY")) { /* test only: no effect on the game cap */
               volatile unsigned char *pol = (volatile unsigned char *)((uintptr_t)lib->base + 0x6f16da0);
               report("roblox: ApplicationFrameRatePolicy %u -> 1", *pol);
               *pol = 1;
            }
         }
         void *settings_cls = fj_class("com/roblox/engine/jni/NativeSettingsInterface");
         for (unsigned k = 0; k < sizeof(st) / sizeof(st[0]); k++) {
            char sym[200];
            snprintf(sym, sizeof(sym), "Java_com_roblox_engine_jni_%s", st[k].name);
            void *fn = ael_symbol(lib, sym);
            if (!fn) {
               report("roblox: settings %s: missing", st[k].name);
               continue;
            }
            void *s0 = st[k].n > 0 ? fj_NewStringUTF(env, st[k].a[0]) : NULL;
            void *s1 = st[k].n > 1 ? fj_NewStringUTF(env, st[k].a[1]) : NULL;
            void *s2 = st[k].n > 2 ? fj_NewStringUTF(env, st[k].a[2]) : NULL;
            if (st[k].n < 0)
               s0 = list;
            const unsigned u0 = roblox_trap_calls;
            report("roblox: settings %s: calling", st[k].name);
            const long rc = ((long (*)(void *, void *, void *, void *, void *))fn)(env, settings_cls, s0, s1, s2);
            report("roblox: settings %s: returned %ld, %u calls to things not provided", st[k].name,
                   st[k].returns_int ? rc : 0L, roblox_trap_calls - u0);
         }
         /* GameBasicSettingsFramerateCap is an engine feature (Lua asks
          * GetEngineFeature), not a flag the settings JSON reaches; the task
          * scheduler reads the frame cap (lib+0x2074e54: min(TargetFps, 240),
          * or 60 with the feature off) when it starts, so it is switched on
          * here, before that. Games stayed at 60 with the display at 120 Hz. */
         if (!getenv("ROBLOX_FPS60")) {
            *(volatile unsigned char *)(roblox_base + 0x6ec41f0) = 1;
            report("roblox: settings: GameBasicSettingsFramerateCap on before the task scheduler starts");
         }
         /* Saved login (roblox_cookies.h keeps it from sign-ins on the PS5;
          * Quick Login on the PC can write it too): the Android app
          * hands its saved cookies to the engine the same way at start-up
          * (rg.a1.T0 -> nativeSetMultipleCookies(baseUrl, "name=value; ...")).
          * The value is a credential: only its length is logged. */
         {
            FILE *sf = fopen("/data/roblox/files/session.txt", "rb");
            void *setc = ael_symbol(lib, "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeSetMultipleCookies");
            if (sf && setc) {
               static char cookie[8192];
               const size_t n = fread(cookie, 1, sizeof(cookie) - 1, sf);
               cookie[n] = 0;
               while (n && (cookie[strlen(cookie) - 1] == '\n' || cookie[strlen(cookie) - 1] == '\r'))
                  cookie[strlen(cookie) - 1] = 0;
               ((void (*)(void *, void *, void *, void *))setc)(env, settings_cls,
                                                                 fj_NewStringUTF(env, "https://www.roblox.com/"),
                                                                 fj_NewStringUTF(env, cookie));
               report("roblox: saved login handed to the engine (%zu bytes)", strlen(cookie));
               memset(cookie, 0, sizeof(cookie));
            } else {
               report("roblox: no saved login (/data/roblox/files/session.txt): the app will ask to log in");
            }
            if (sf)
               fclose(sf);
         }
      }
      for (unsigned i = 0; i < sizeof(shell) / sizeof(shell[0]); i++) {
         void *fn = ael_symbol(lib, shell[i].symbol);
         const char *name = strstr(shell[i].symbol, "_native") ? strstr(shell[i].symbol, "_native") + 1
                                                                 : strrchr(shell[i].symbol, '_') + 1;
         if (!fn) {
            report("roblox: app shell %s: missing", name);
            continue;
         }
         const unsigned u0 = roblox_trap_calls;
         report("roblox: app shell %.60s: calling", name);
         switch (shell[i].kind) {
         case 0: ((void (*)(void *, void *, unsigned char))fn)(env, bridge, 1); break;
         case 1:
            ((void (*)(void *, void *, void *, void *, unsigned char, void *, void *, void *))fn)(
               env, bridge, fj_NewStringUTF(env, "https://www.roblox.com/"),
               fj_NewStringUTF(env, "Mozilla/5.0 (Linux; Android 13) ROBLOX Android App 2.746.1266 Tablet Hybrid()"), 1,
               fj_NewStringUTF(env, ""), fj_NewStringUTF(env, ""), fj_NewStringUTF(env, ""));
            break;
         case 2: ((void (*)(void *, void *, void *))fn)(env, gl, ip); break;
         case 3: ((void (*)(void *, void *, void *))fn)(env, gl, sp); break;
         case 4:
            ((void (*)(void *, void *))fn)(env, gl);
            break;
         case 5:
            ((void (*)(void *, void *, void *, void *))fn)(env, gl, surface, pp);
            roblox_surface_app = (void (*)(void *, void *, void *, void *))fn; /* again after a game */
            roblox_gl_cls = gl;
            roblox_app_surface = surface;
            roblox_app_params = pp;
            break;
         }
         report("roblox: app shell %.60s: returned, %u calls to things not provided", name, roblox_trap_calls - u0);
      }
   }
   typedef void (*life_fn)(void *, void *, long long);
   const char *steps[] = {"onStartNative", "onResumeNative", "onWindowFocusChangedNative"};
   for (unsigned i = 0; i < 3; i++) {
      void *fn = fj_find_native(steps[i]);
      report("roblox: activity %s: %s", steps[i], fn ? "calling" : "not registered");
      if (!fn)
         continue;
      if (i == 2) {
         ((void (*)(void *, void *, long long, int))fn)(env, activity, handle, 1);
         roblox_window_focus = (void (*)(void *, void *, long long, int))fn; /* see roblox_input_view_restore */
         roblox_activity_obj = activity;
         roblox_activity_handle = handle;
      }
      else
         ((life_fn)fn)(env, activity, handle);
      usleep(500 * 1000);
   }
   {
      /* The window exists: Android's SurfaceView reports created + changed. */
      struct fj_obj *surface = fj_new(FJ_OBJECT, "Surface", NULL);
      surface->cls = fj_class("android/view/Surface");
      void *created = fj_find_native("onSurfaceCreatedNative");
      void *changed = fj_find_native("onSurfaceChangedNative");
      report("roblox: surface: created %s, changed %s", created ? "calling" : "missing", changed ? "calling" : "missing");
      roblox_surface_created = created; /* used again after a game: roblox_input_view_restore */
      roblox_surface_changed = changed;
      if (created)
         ((void (*)(void *, void *, long long, void *))created)(env, activity, handle, surface);
      if (changed)
         ((void (*)(void *, void *, long long, void *, int, int, int))changed)(env, activity, handle, surface, 1,
                                                                             1920, 1080);
      report("roblox: surface handed over (1920x1080)");
   }
   rk_preload();
   rw_preload();
   {
      unlink("/app0/quit-request"); /* a stale request must not end this run */
      pthread_t qt;
      if (pthread_create(&qt, NULL, rv_quit_watch, NULL) == 0)
         pthread_detach(qt);
   }
   roblox_back_pressed = ael_symbol(lib, "Java_com_roblox_engine_jni_NativeGLInterface_nativeHandleBackPressed");
   roblox_get_cookies = ael_symbol(lib, "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeGetCookiesForDomain");
   roblox_refresh_fn[0] = ael_symbol(lib, "Java_com_roblox_engine_jni_NativeGLInterface_nativePassSupportedRefreshRates");
   roblox_refresh_fn[1] = ael_symbol(lib, "Java_com_roblox_engine_jni_NativeGLInterface_nativePassCurrentDisplayRefreshRate");
   roblox_app_set_active =
      ael_symbol(lib, "Java_com_roblox_universalapp_applifecyclenativeadapter_JNIAppLifecycleNativeAdapter_setActive");
   rk_pass_text = ael_symbol(lib, "Java_com_roblox_engine_jni_NativeGLInterface_nativePassText");
   rk_return_pressed = ael_symbol(lib, "Java_com_roblox_engine_jni_NativeGLInterface_nativeReturnPressedFromOnScreenKeyboard");
   report("roblox: keyboard: nativePassText %p, nativeReturnPressedFromOnScreenKeyboard %p", rk_pass_text,
          rk_return_pressed);
   if (!getenv("ROBLOX_NO_PAD")) {
      static const char *const in[] = {"nativeGamepadButtonEvent", "nativeGamepadAxisEvent",
                                       "nativeGamepadConnectEventWithGamepadType",
                                       "nativeSetGamepadSupportedKeyWithGamepadType",
                                       "nativeSetGamepadSupportedMotionWithGamepadType",
                                       "nativePassMouseMove", "nativePassMouseButton",
                                       "nativeGamepadDisconnectEvent", "nativePassMouseWheel"};
      for (int k = 0; k < 9; k++) {
         char sym[160];
         snprintf(sym, sizeof(sym), "Java_com_roblox_engine_jni_NativeInputInterface_%s", in[k]);
         roblox_pad_fn[k] = ael_symbol(lib, sym);
      }
      pthread_t pt;
      pthread_create(&pt, NULL, roblox_pad_thread, NULL);
      pthread_detach(pt);
   }
   /* Give the game thread time to run and log what it reaches for. */
   for (int s = 0; s < 15; s++) {
      usleep(1000 * 1000);
      report("roblox: %d s after start: %u calls to things not provided so far", s + 1, roblox_trap_calls);
   }
}

static int *
roblox_errno_shim(void)
{
   return &errno;
}

static int
test_roblox_library(void)
{
   /* first: the engine's start-up requests (its sign-in check among them)
    * come within seconds and must already find the gateway */
   roblox_start_network();
#if defined(__PROSPERO__)
   const char *path = "/app0/libroblox.so";
#else
   const char *path = "libroblox.so";
#endif
   FILE *f = fopen(path, "rb");
   if (!f) {
      report("roblox: %s is not there; the Roblox library check is skipped", path);
      return -1;
   }
   fseek(f, 0, SEEK_END);
   const long size = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint8_t *image = size > 0 ? malloc((size_t)size) : NULL;
   const bool read_ok = image && fread(image, 1, (size_t)size, f) == (size_t)size;
   fclose(f);
   roblox_image_file = image;
   roblox_image_file_size = read_ok ? (size_t)size : 0;
   report("roblox: read %s, %ld bytes: %s", path, size, read_ok ? "ok" : "failed");
   check(read_ok, "roblox: libroblox.so read into memory");
   if (!read_ok) {
      free(image);
      return 0;
   }
   roblox_trap_code = android_exec_alloc(ROBLOX_MAX_TRAPS * 32);
   bionic_shims_init(bionic_report_line);
   const struct ael_import *imports = bionic_imports;
   (void)roblox_errno_shim;
   const char *stage = getenv("ROBLOX_STAGE");
   const bool activity = stage && strcmp(stage, "activity") == 0;
   const bool jni = activity || (stage && strcmp(stage, "jni") == 0);
   const bool ctors = jni || (stage && strcmp(stage, "ctors") == 0);
   const struct ael_options o = {
      .imports = imports,
      .resolve = roblox_trap_resolver,
      .alloc = android_exec_alloc,
      .log = android_loader_log,
      .skip_constructors = !ctors,
      .before_constructors = roblox_before_constructors,
      .wrap = roblox_wrap_import,
   };
   struct ael_library lib;
   const uint64_t t0 = now_ns();
   const int rc = ael_load_ex(image, (size_t)size, &o, &lib);
   free(image);
   report("roblox: load rc=%d in %.2f s, %u imports given trap thunks", rc, (now_ns() - t0) / 1e9, roblox_trap_count);
   check(rc == 0, "roblox: libroblox.so maps, relocates (576,942) and binds its imports on the PS5");
   if (rc != 0)
      return 0;
   void *jni_onload = ael_symbol(&lib, "JNI_OnLoad");
   void *jni_ptr = jni_onload;
   report("roblox: JNI_OnLoad at %p (library offset 0x%llx)", jni_ptr,
          jni_ptr ? (unsigned long long)((uint8_t *)jni_ptr - lib.base) : 0ull);
   check(jni_ptr != NULL, "roblox: its JNI_OnLoad entry point is found");
   if (ctors)
      report("roblox: %llu constructors returned; %u calls hit unprovided imports",
             (unsigned long long)(lib.init_arraysz / 8), roblox_trap_calls);
   if (ctors)
      roblox_sample_start();
   /* Console ("ten-foot") interface. GuiService's ten-foot byte: read by
    * GuiService:IsTenFootInterface(), and with it set the engine classes the
    * device as "Consoles" (not "SmallWindowsHandhelds") - the Xbox/PlayStation
    * layout, built for a controller. Its only writer is the Lua method
    * ForceTenFootInterface, so set once here. ROBLOX_MOBILE_UI=1: phone layout. */
   /* Lua login: the engine's own login screen (drawn by Roblox, no Android
    * Java views - which the PS5 doesn't have; without it the app routes to
    * "LoginNative" and waits on a grey screen). nativeIsLuaLoginEnabled()
    * is "int at lib+0x6ec5bb8 == 1" (a fast flag: server flags may rewrite it,
    * so it is held for the first two minutes). ROBLOX_NATIVE_LOGIN=1: leave it. */
   if (ctors && !getenv("ROBLOX_NATIVE_LOGIN")) {
      pthread_t hold;
      pthread_create(&hold, NULL, roblox_lua_login_hold, (void *)roblox_base);
      pthread_detach(hold);
   }
   if (ctors && !getenv("ROBLOX_MOBILE_UI")) {
      volatile uint8_t *tenfoot = (volatile uint8_t *)(roblox_base + 0x711c5a8);
      *tenfoot = 1;
      report("roblox: console (ten-foot) interface on (byte lib+0x711c5a8 = %u)", *tenfoot);
   }
   if (jni && jni_onload) {
      /* Android's runtime calls JNI_OnLoad(vm, NULL) right after
       * System.loadLibrary("roblox"). */
      void *vm = fj_create(bionic_report_line, roblox_trap_resolver);
      fj_java_hook = roblox_java_hook; /* FMOD sound -> PS5 audio out; text boxes -> system keyboard */
      /* The JNI tables hold the title's own (execute-only) functions too. */
      for (size_t i = 0; i < FJ_ENV_SLOTS; i++)
         if (fj_env_table[i] && ((uint8_t *)fj_env_table[i] < roblox_trap_code ||
                                 (uint8_t *)fj_env_table[i] >= roblox_trap_code + ROBLOX_MAX_TRAPS * 32))
            fj_env_table[i] = roblox_trampoline(fj_env_table[i]);
      for (size_t i = 0; i < FJ_VM_SLOTS; i++)
         if (fj_vm_table[i] && ((uint8_t *)fj_vm_table[i] < roblox_trap_code ||
                                (uint8_t *)fj_vm_table[i] >= roblox_trap_code + ROBLOX_MAX_TRAPS * 32))
            fj_vm_table[i] = roblox_trampoline(fj_vm_table[i]);
      const unsigned before = roblox_trap_calls;
      report("roblox: calling JNI_OnLoad");
      const int version = ((int (*)(void *, void *))jni_onload)(vm, NULL);
      report("roblox: JNI_OnLoad returned 0x%x; %u classes looked up, %u members, %u natives registered, %u calls to "
             "things not provided",
             (unsigned)version, fj_classes, fj_methods, fj_natives, roblox_trap_calls - before);
      check(version >= 0x10006, "roblox: JNI_OnLoad runs on the PS5 and returns a JNI version");
      if (activity)
         roblox_start_activity(&lib, vm);
   }
   return jni_ptr != NULL;
}

/* A solid colour square, for the Android check's result on screen. */
static void
fill_square(uint8_t *px, int size, uint8_t r, uint8_t g, uint8_t b)
{
   for (int i = 0; i < size * size; i++) {
      const int x = i % size, y = i / size;
      const bool edge = x < 12 || y < 12 || x >= size - 12 || y >= size - 12;
      px[i * 4 + 0] = edge ? 255 : b;
      px[i * 4 + 1] = edge ? 255 : g;
      px[i * 4 + 2] = edge ? 255 : r;
      px[i * 4 + 3] = 255;
   }
}

/* A visible demo for a person watching the TV: the background cycles through
 * the hues, and a gradient square bounces across a 4K swapchain, for
 * RADV_DEMO_SECONDS (default 60). Everything is drawn by the GPU's copy and
 * clear engines through RADV's VK_KHR_display swapchain. */
static void
hue_rgb(float h, float *r, float *g, float *b)
{
   h -= (float)(int)h;
   const float x = h * 6.0f;
   const int i = (int)x;
   const float f = x - (float)i;
   switch (i % 6) {
   case 0: *r = 1; *g = f; *b = 0; break;
   case 1: *r = 1 - f; *g = 1; *b = 0; break;
   case 2: *r = 0; *g = 1; *b = f; break;
   case 3: *r = 0; *g = 1 - f; *b = 1; break;
   case 4: *r = f; *g = 0; *b = 1; break;
   default: *r = 1; *g = 0; *b = 1 - f; break;
   }
}

static void
test_demo(struct context *c)
{
   bool ok = true;
#define LOAD_I(name)                                                                               \
   vk##name = (PFN_vk##name)vk_icdGetInstanceProcAddr(c->instance, "vk" #name);                    \
   ok &= vk##name != NULL;
#define LOAD_D(name)                                                                               \
   vk##name = (PFN_vk##name)vkGetDeviceProcAddr(c->device, "vk" #name);                            \
   ok &= vk##name != NULL;
   DISPLAY_INSTANCE_COMMANDS(LOAD_I)
   DISPLAY_DEVICE_COMMANDS(LOAD_D)
#undef LOAD_I
#undef LOAD_D
   check(ok, "demo: display commands");
   if (!ok)
      return;

   VkDisplayPropertiesKHR display;
   uint32_t count = 1;
   if (vkGetPhysicalDeviceDisplayPropertiesKHR(c->physical, &count, &display) != VK_SUCCESS || count != 1) {
      check(false, "demo: a display");
      return;
   }
   VkDisplayModePropertiesKHR modes[2];
   count = 2;
   if (vkGetDisplayModePropertiesKHR(c->physical, display.display, &count, modes) != VK_SUCCESS || !count) {
      check(false, "demo: a display mode");
      return;
   }
   const VkDisplaySurfaceCreateInfoKHR surface_info = {
      .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
      .displayMode = modes[0].displayMode,
      .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
      .imageExtent = {3840, 2160},
   };
   VkSurfaceKHR surface;
   if (vkCreateDisplayPlaneSurfaceKHR(c->instance, &surface_info, NULL, &surface) != VK_SUCCESS) {
      check(false, "demo: surface");
      return;
   }
   VkBool32 supported = VK_FALSE;
   vkGetPhysicalDeviceSurfaceSupportKHR(c->physical, c->family, surface, &supported);

   /* The bouncing square: SQ x SQ BGRA texels with a diagonal gradient and a
    * white border, written by the CPU once. */
   enum { SQ = 512, BORDER = 16 };
   struct buffer square;
   VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
   const VkSemaphoreCreateInfo semaphore_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   if (!buffer_create(c, (VkDeviceSize)SQ * SQ * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &square) ||
       vkCreateSemaphore(c->device, &semaphore_info, NULL, &acquired) != VK_SUCCESS ||
       vkCreateSemaphore(c->device, &semaphore_info, NULL, &rendered) != VK_SUCCESS) {
      check(false, "demo: buffers");
      vkDestroySurfaceKHR(c->instance, surface, NULL);
      return;
   }
   uint8_t *px = square.map;
   /* Step 3's result, shown as a fixed square at the top left: green when the
    * Android library ran, red when it failed, gray when it is absent. */
   enum { STATUS = 384 };
   const int android = test_android_library();
   const int roblox = test_roblox_library();
   struct buffer status, status2;
   const bool have_status = buffer_create(c, (VkDeviceSize)STATUS * STATUS * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &status);
   const bool have_status2 = buffer_create(c, (VkDeviceSize)STATUS * STATUS * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &status2);
   if (have_status2) {
      /* Step 4's result beside it: blue when libroblox.so loaded. */
      if (roblox > 0)
         fill_square(status2.map, STATUS, 30, 90, 230);
      else if (roblox == 0)
         fill_square(status2.map, STATUS, 220, 30, 30);
      else
         fill_square(status2.map, STATUS, 110, 110, 110);
   }
   if (have_status) {
      if (android > 0)
         fill_square(status.map, STATUS, 20, 200, 60);
      else if (android == 0)
         fill_square(status.map, STATUS, 220, 30, 30);
      else
         fill_square(status.map, STATUS, 110, 110, 110);
   }
   for (int y = 0; y < SQ; y++) {
      for (int x = 0; x < SQ; x++) {
         uint8_t *p = px + ((size_t)y * SQ + x) * 4;
         const bool edge = x < BORDER || y < BORDER || x >= SQ - BORDER || y >= SQ - BORDER;
         p[0] = edge ? 255 : (uint8_t)(255 - x / 2);       /* B */
         p[1] = edge ? 255 : (uint8_t)(y / 2);             /* G */
         p[2] = edge ? 255 : (uint8_t)((x + y) / 4);       /* R */
         p[3] = 255;
      }
   }

   VkImage images[8];
   /* Roblox's renderer may already want the display (vkCreateAndroidSurfaceKHR):
    * then the demo gives it up instead of starting its own swapchain. */
   if (__atomic_load_n(&rv_display_wanted, __ATOMIC_SEQ_CST)) {
      report("demo: Roblox's renderer wants the display; the demo steps aside");
      vkDeviceWaitIdle(c->device);
      vkDestroySurfaceKHR(c->instance, surface, NULL);
      __atomic_store_n(&rv_display_released, 1, __ATOMIC_SEQ_CST);
      for (;;) {
         sleep(10);
         report("demo: idle while Roblox has the display");
      }
   }
   VkSwapchainKHR swapchain = display_swapchain(c, surface, VK_NULL_HANDLE, images, &count);
   if (swapchain) {
#if defined(__PROSPERO__)
      report("demo: hide splash screen returned 0x%08x", (unsigned)sceSystemServiceHideSplashScreen());
#endif
      /* 0 (the default) runs until the title is closed from the home screen:
       * returning from main crashes a native title. */
      const char *s = getenv("RADV_DEMO_SECONDS");
      const double seconds = s ? atof(s) : 0.0;
      const uint64_t start = now_ns();
      int sx = 200, sy = 200, dx = 18, dy = 13;
      unsigned frames = 0, failures = 0;
      int pad = -1;
      uint32_t last_buttons = 0;
      unsigned presses = 0, stick_frames = 0;
      float hue = 0.0f;
      bool hue_paused = false;
#if defined(__PROSPERO__)
      {
         const int us = sceUserServiceInitialize(NULL);
         const int pi = scePadInit();
         int users[4] = {-1, -1, -1, -1};
         const int ul = sceUserServiceGetLoginUserIdList(users);
         for (int u = 0; u < 4 && pad < 0; u++) {
            if (users[u] == -1)
               continue;
            pad = scePadOpen(users[u], 0, 0, NULL);
            if (pad < 0)
               pad = scePadGetHandle(users[u], 0, 0);
         }
         report("demo: pad: userservice 0x%08x, padinit 0x%08x, users 0x%08x [%d %d %d %d], handle %d",
                (unsigned)us, (unsigned)pi, (unsigned)ul, users[0], users[1], users[2], users[3], pad);
      }
#endif
      report("demo: running for %.0f s (0 = forever; hue background + bouncing square, 3840x2160)", seconds);
      bool for_roblox = false;
      __atomic_store_n(&rv_demo_presenting, 1, __ATOMIC_SEQ_CST);
      while (seconds <= 0.0 || (now_ns() - start) / 1e9 < seconds) {
         if (__atomic_load_n(&rv_display_wanted, __ATOMIC_SEQ_CST)) {
            report("demo: Roblox's renderer wants the display; the demo steps aside after %u frames", frames);
            for_roblox = true;
            break;
         }
         uint32_t index;
         if (vkAcquireNextImageKHR(c->device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index) != VK_SUCCESS) {
            failures++;
            break;
         }
         const VkCommandBufferBeginInfo begin_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
         };
         vkResetCommandBuffer(c->cmd, 0);
         vkBeginCommandBuffer(c->cmd, &begin_info);
         display_image_barrier(c, images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT);
         float r, g, b;
         hue_rgb(hue, &r, &g, &b);
         const VkClearColorValue color = {.float32 = {0.15f + 0.6f * b, 0.15f + 0.6f * g, 0.15f + 0.6f * r, 1.0f}};
         const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
         vkCmdClearColorImage(c->cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
         /* Clear then copy: both transfer writes to the same image. */
         display_image_barrier(c, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT);
         const VkBufferImageCopy copy = {
            .bufferRowLength = SQ,
            .bufferImageHeight = SQ,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset = {sx, sy, 0},
            .imageExtent = {SQ, SQ, 1},
         };
         vkCmdCopyBufferToImage(c->cmd, square.buffer, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
         if (have_status) {
            const VkBufferImageCopy status_copy = {
               .bufferRowLength = STATUS,
               .bufferImageHeight = STATUS,
               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
               .imageOffset = {64, 64, 0},
               .imageExtent = {STATUS, STATUS, 1},
            };
            vkCmdCopyBufferToImage(c->cmd, status.buffer, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &status_copy);
         }
         if (have_status2) {
            const VkBufferImageCopy status2_copy = {
               .bufferRowLength = STATUS,
               .bufferImageHeight = STATUS,
               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
               .imageOffset = {64 + STATUS + 48, 64, 0},
               .imageExtent = {STATUS, STATUS, 1},
            };
            vkCmdCopyBufferToImage(c->cmd, status2.buffer, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &status2_copy);
         }
         display_image_barrier(c, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
         vkEndCommandBuffer(c->cmd);
         const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
         const VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &acquired,
            .pWaitDstStageMask = &wait_stage,
            .commandBufferCount = 1,
            .pCommandBuffers = &c->cmd,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &rendered,
         };
         vkResetFences(c->device, 1, &c->fence);
         if (vkQueueSubmit(c->queue, 1, &submit, c->fence) != VK_SUCCESS) {
            failures++;
            break;
         }
         const VkPresentInfoKHR present = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &rendered,
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &index,
         };
         const VkResult presented = vkQueuePresentKHR(c->queue, &present);
         vkWaitForFences(c->device, 1, &c->fence, VK_TRUE, UINT64_MAX);
         if (presented != VK_SUCCESS) {
            failures++;
            break;
         }
         frames++;
         if (!hue_paused)
            hue += 1.0f / 600.0f;
         bool manual = false;
#if defined(__PROSPERO__)
         if (pad >= 0) {
            DemoPadData state;
            memset(&state, 0, sizeof(state));
            if (scePadReadState(pad, &state) == 0 && state.connected) {
               const uint32_t pressed = state.buttons & ~last_buttons;
               last_buttons = state.buttons;
               if (pressed & 0x4000u) { /* Cross: invert the square's colours */
                  for (size_t i = 0; i < (size_t)SQ * SQ; i++) {
                     px[i * 4 + 0] = 255 - px[i * 4 + 0];
                     px[i * 4 + 1] = 255 - px[i * 4 + 1];
                     px[i * 4 + 2] = 255 - px[i * 4 + 2];
                  }
                  presses++;
                  report("demo: Cross pressed (%u presses)", presses);
               }
               if (pressed & 0x2000u) { /* Circle: pause or resume the background */
                  hue_paused = !hue_paused;
                  presses++;
                  report("demo: Circle pressed, background %s", hue_paused ? "paused" : "cycling");
               }
               if (pressed & 0x1000u) { /* Triangle: jump the hue */
                  hue += 0.25f;
                  presses++;
                  report("demo: Triangle pressed, hue %.2f", hue);
               }
               const int ax = (int)state.lx - 128, ay = (int)state.ly - 128;
               int mx = (ax > 24 || ax < -24) ? ax / 3 : 0;
               int my = (ay > 24 || ay < -24) ? ay / 3 : 0;
               if (state.buttons & 0x0080u) mx = -30; /* D-pad */
               if (state.buttons & 0x0020u) mx = 30;
               if (state.buttons & 0x0010u) my = -30;
               if (state.buttons & 0x0040u) my = 30;
               if (mx || my) {
                  manual = true;
                  stick_frames++;
                  sx += mx;
                  sy += my;
                  if (sx < 0) sx = 0;
                  if (sy < 0) sy = 0;
                  if (sx + SQ > 3840) sx = 3840 - SQ;
                  if (sy + SQ > 2160) sy = 2160 - SQ;
                  if (stick_frames % 120 == 1)
                     report("demo: stick moving the square to %d,%d", sx, sy);
               }
               if ((frames & 7) == 0) {
                  const DemoPadColor bar = {(uint8_t)(r * 255), (uint8_t)(g * 255), (uint8_t)(b * 255), 255};
                  scePadSetLightBar(pad, &bar);
               }
            }
         }
#endif
         if (manual)
            continue;
         sx += dx;
         sy += dy;
         if (sx < 0 || sx + SQ > 3840) {
            dx = -dx;
            sx += 2 * dx;
         }
         if (sy < 0 || sy + SQ > 2160) {
            dy = -dy;
            sy += 2 * dy;
         }
         if (frames % 600 == 0)
            report("demo: %u frames, %.1f fps", frames, frames / ((now_ns() - start) / 1e9));
      }
      const double took = (now_ns() - start) / 1e9;
      report("demo: %u frames in %.1f s (%.1f fps)", frames, took, frames / took);
      check(frames > 0 && failures == 0, "demo: frames presented until the end");
      vkDeviceWaitIdle(c->device);
      vkDestroySwapchainKHR(c->device, swapchain, NULL);
      if (for_roblox) {
         vkDestroySurfaceKHR(c->instance, surface, NULL);
         __atomic_store_n(&rv_display_released, 1, __ATOMIC_SEQ_CST);
         for (;;) {
            sleep(10);
            report("demo: idle while Roblox has the display");
         }
      }
   }
   vkDestroySemaphore(c->device, acquired, NULL);
   vkDestroySemaphore(c->device, rendered, NULL);
   buffer_destroy(c, &square);
   if (have_status)
      buffer_destroy(c, &status);
   if (have_status2)
      buffer_destroy(c, &status2);
   vkDestroySurfaceKHR(c->instance, surface, NULL);
}

static bool
context_create(struct context *c)
{
   vkCreateInstance = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
   if (!vkCreateInstance) {
      report("vk_icdGetInstanceProcAddr has no vkCreateInstance");
      return false;
   }
   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "radv-smoke",
      .apiVersion = VK_API_VERSION_1_4,
   };
   /* VK_KHR_display where it is reported, for test_display. */
   const PFN_vkEnumerateInstanceExtensionProperties enumerate_instance_extensions =
      (PFN_vkEnumerateInstanceExtensionProperties)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE,
                                                                            "vkEnumerateInstanceExtensionProperties");
   VkExtensionProperties instance_extensions[64];
   uint32_t instance_extension_count = sizeof(instance_extensions) / sizeof(instance_extensions[0]);
   c->display = false;
   if (enumerate_instance_extensions &&
       enumerate_instance_extensions(NULL, &instance_extension_count, instance_extensions) >= VK_SUCCESS) {
      for (uint32_t i = 0; i < instance_extension_count; i++)
         c->display |= strcmp(instance_extensions[i].extensionName, VK_KHR_DISPLAY_EXTENSION_NAME) == 0;
   }
   const char *const display_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
   const VkInstanceCreateInfo instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
      .enabledExtensionCount = c->display ? 2 : 0,
      .ppEnabledExtensionNames = display_extensions,
   };
   VkResult result = vkCreateInstance(&instance_info, NULL, &c->instance);
   check(result == VK_SUCCESS, "vkCreateInstance");
   if (result != VK_SUCCESS || !load_instance(c->instance))
      return false;
   uint32_t count = 1;
   result = vkEnumeratePhysicalDevices(c->instance, &count, &c->physical);
   check((result == VK_SUCCESS || result == VK_INCOMPLETE) && count == 1, "one physical device");
   if (count == 0)
      return false;

   VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
   VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
   vkGetPhysicalDeviceProperties2(c->physical, &props);
   report("device: %s, Vulkan %u.%u.%u, %s %s", props.properties.deviceName,
          VK_API_VERSION_MAJOR(props.properties.apiVersion), VK_API_VERSION_MINOR(props.properties.apiVersion),
          VK_API_VERSION_PATCH(props.properties.apiVersion), driver.driverName, driver.driverInfo);

   VkQueueFamilyProperties families[8];
   count = 8;
   vkGetPhysicalDeviceQueueFamilyProperties(c->physical, &count, families);
   c->family = UINT32_MAX;
   for (uint32_t i = 0; i < count; i++) {
      if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
          (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
         c->family = i;
         break;
      }
   }
   if (c->family == UINT32_MAX) {
      check(false, "a graphics and compute queue family");
      return false;
   }
   vkGetPhysicalDeviceMemoryProperties(c->physical, &c->memory);

   const float priority = 1.0f;
   const VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = c->family,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR barycentric = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR,
      .fragmentShaderBarycentric = VK_TRUE,
   };
   VkPhysicalDeviceFragmentShadingRateFeaturesKHR shading_rate = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
      .pipelineFragmentShadingRate = VK_TRUE,
   };
   VkPhysicalDeviceVulkan12Features features12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      .drawIndirectCount = VK_TRUE,
   };
   VkPhysicalDeviceVulkan13Features features13 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .pNext = &features12,
      .synchronization2 = VK_TRUE,
      .dynamicRendering = VK_TRUE,
   };
   const char *device_extensions[12];
   uint32_t enabled_extensions = 0;
   VkExtensionProperties extensions[512];
   uint32_t extension_count = sizeof(extensions) / sizeof(extensions[0]);
   c->barycentric = false;
   c->shading_rate = false;
   c->acceleration_structure = false;
   if (vkEnumerateDeviceExtensionProperties(c->physical, NULL, &extension_count, extensions) >= VK_SUCCESS) {
      for (uint32_t i = 0; i < extension_count; i++) {
         c->barycentric |= strcmp(extensions[i].extensionName, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME) == 0;
         c->shading_rate |= strcmp(extensions[i].extensionName, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) == 0;
         c->acceleration_structure |=
            strcmp(extensions[i].extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0;
         c->descriptor_buffer |= strcmp(extensions[i].extensionName, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME) == 0;
         c->mesh |= strcmp(extensions[i].extensionName, VK_EXT_MESH_SHADER_EXTENSION_NAME) == 0;
      }
   }
   VkPhysicalDeviceMeshShaderFeaturesEXT mesh = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
   };
   if (c->mesh) {
      VkPhysicalDeviceFeatures2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &mesh};
      vkGetPhysicalDeviceFeatures2(c->physical, &query);
      c->mesh = mesh.meshShader;
      c->task = mesh.taskShader;
      mesh.pNext = NULL;
      mesh.multiviewMeshShader = VK_FALSE;
      mesh.primitiveFragmentShadingRateMeshShader = VK_FALSE;
      mesh.meshShaderQueries = VK_FALSE;
   }
   if (c->mesh) {
      device_extensions[enabled_extensions++] = VK_EXT_MESH_SHADER_EXTENSION_NAME;
      mesh.pNext = features13.pNext;
      features13.pNext = &mesh;
   }
   VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration_structure = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
      .accelerationStructure = VK_TRUE,
   };
   if (c->acceleration_structure) {
      device_extensions[enabled_extensions++] = VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME;
      device_extensions[enabled_extensions++] = VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME;
      features12.bufferDeviceAddress = VK_TRUE;
      acceleration_structure.pNext = features13.pNext;
      features13.pNext = &acceleration_structure;
   }
   if (c->barycentric) {
      device_extensions[enabled_extensions++] = VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME;
      barycentric.pNext = features13.pNext;
      features13.pNext = &barycentric;
   }
   if (c->display)
      device_extensions[enabled_extensions++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
   VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor_buffer = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT,
      .descriptorBuffer = VK_TRUE,
   };
   c->descriptor_buffer &= getenv("RADV_SMOKE_SPARSE_TIMING") != NULL;
   if (c->descriptor_buffer) {
      device_extensions[enabled_extensions++] = VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME;
      features12.bufferDeviceAddress = VK_TRUE;
      descriptor_buffer.pNext = features13.pNext;
      features13.pNext = &descriptor_buffer;
   }
   if (c->shading_rate) {
      device_extensions[enabled_extensions++] = VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME;
      shading_rate.pNext = features13.pNext;
      features13.pNext = &shading_rate;
   }
   VkPhysicalDeviceFeatures supported;
   vkGetPhysicalDeviceFeatures(c->physical, &supported);
   const VkPhysicalDeviceFeatures features = {
      .sparseBinding = supported.sparseBinding,
      .geometryShader = VK_TRUE,
      .tessellationShader = VK_TRUE,
      .vertexPipelineStoresAndAtomics = VK_TRUE,
      .fragmentStoresAndAtomics = VK_TRUE,
   };
   const VkDeviceCreateInfo device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = &features13,
      .pEnabledFeatures = &features,
      .enabledExtensionCount = enabled_extensions,
      .ppEnabledExtensionNames = device_extensions,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
   };
   result = vkCreateDevice(c->physical, &device_info, NULL, &c->device);
   check(result == VK_SUCCESS, "vkCreateDevice");
   if (result != VK_SUCCESS || !load_device(c->device))
      return false;
   if (c->mesh) {
#define LOAD(name) c->mesh &= (vk##name = (PFN_vk##name)vkGetDeviceProcAddr(c->device, "vk" #name)) != NULL;
      MESH_COMMANDS(LOAD)
#undef LOAD
   }
   vkGetDeviceQueue(c->device, c->family, 0, &c->queue);

   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = c->family,
   };
   const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   if (vkCreateCommandPool(c->device, &pool_info, NULL, &c->pool) != VK_SUCCESS ||
       vkCreateFence(c->device, &fence_info, NULL, &c->fence) != VK_SUCCESS)
      return false;
   const VkCommandBufferAllocateInfo cmd_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = c->pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   return vkAllocateCommandBuffers(c->device, &cmd_info, &c->cmd) == VK_SUCCESS;
}

/* A mesh shader whose outputs follow an atomic (radv/shaders/mesh_ticket.mesh):
 * each of 32 x 32 workgroups takes a ticket and paints its 8-pixel cell in the
 * ticket's colour with 128 triangles, which this GPU gets in two parts. Every
 * cell must be one colour, every ticket taken once, and the counter must end
 * at the number of workgroups: the workgroup ran once, and its second part
 * showed what the first computed. 1,024 workgroups also go round the port's
 * publish ring more than once. */
static bool
mesh_tickets_check(struct context *c, const char *what, const struct buffer *readback, const struct buffer *tickets)
{
   enum { CELLS = TARGET_SIZE / 8, WORKGROUPS = CELLS * CELLS };
   static bool seen[WORKGROUPS];
   memset(seen, 0, sizeof(seen));
   const uint8_t *pixels = readback->map;
   uint32_t mixed = 0, uncovered = 0, repeated = 0;
   for (uint32_t cy = 0; cy < CELLS; cy++) {
      for (uint32_t cx = 0; cx < CELLS; cx++) {
         const uint8_t *first = pixels + ((cy * 8) * TARGET_SIZE + cx * 8) * 4;
         bool uniform = true;
         for (uint32_t y = 0; y < 8; y++)
            for (uint32_t x = 0; x < 8; x++)
               uniform &= memcmp(pixels + ((cy * 8 + y) * TARGET_SIZE + cx * 8 + x) * 4, first, 4) == 0;
         mixed += !uniform;
         if (first[2] != 0 || first[3] != 255) {
            uncovered++;
            continue;
         }
         const uint32_t ticket = first[0] | (uint32_t)first[1] << 8;
         if (ticket >= WORKGROUPS || seen[ticket])
            repeated++;
         else
            seen[ticket] = true;
      }
   }
   const uint32_t counter = *(const uint32_t *)tickets->map;
   char label[160];
   snprintf(label, sizeof(label), "%s: %u workgroups each ran once and every cell shows one ticket", what, WORKGROUPS);
   const bool ok = mixed == 0 && uncovered == 0 && repeated == 0 && counter == WORKGROUPS;
   if (!ok)
      report("%s: %u cells of two colours, %u uncovered, %u tickets out of range or twice, counter %u", what, mixed,
             uncovered, repeated, counter);
   check(ok, label);
   (void)c;
   return ok;
}

static void
test_mesh(struct context *c)
{
   struct buffer readback = {0}, tickets = {0}, args = {0};
   if (!buffer_create(c, TARGET_SIZE * TARGET_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback) ||
       !buffer_create(c, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &tickets) ||
       !buffer_create(c, 2 * sizeof(VkDrawMeshTasksIndirectCommandEXT), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, &args)) {
      check(false, "mesh: buffers");
      return;
   }
   VkShaderModule mesh = shader(c, radv_smoke_mesh_ticket_mesh, sizeof(radv_smoke_mesh_ticket_mesh));
   VkShaderModule frag = shader(c, radv_smoke_mesh_colour_frag, sizeof(radv_smoke_mesh_colour_frag));
   const VkPipelineShaderStageCreateInfo stages[2] = {
      stage_info(VK_SHADER_STAGE_MESH_BIT_EXT, mesh),
      stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
   };

   /* One draw of 32 x 32 workgroups. */
   memset(tickets.map, 0, 64);
   c->draw.mode = DRAW_MESH;
   c->draw.groups[0] = TARGET_SIZE / 8;
   c->draw.groups[1] = TARGET_SIZE / 8;
   c->draw.groups[2] = 1;
   if (render_readback_with(c, "mesh tickets, one draw", stages, 2, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 0,
                            &readback, tickets.buffer, NULL))
      mesh_tickets_check(c, "mesh tickets, one draw", &readback, &tickets);

   /* The same cells from two indirect draws of 32 x 16: the second's
    * workgroups come after the first's. */
   memset(tickets.map, 0, 64);
   VkDrawMeshTasksIndirectCommandEXT *const commands = args.map;
   commands[0] = (VkDrawMeshTasksIndirectCommandEXT){TARGET_SIZE / 8, TARGET_SIZE / 16, 1};
   commands[1] = commands[0];
   c->draw.mode = DRAW_MESH_INDIRECT;
   c->draw.args = args.buffer;
   c->draw.max_draws = 2;
   if (render_readback_with(c, "mesh tickets, two indirect draws", stages, 2, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                            0, 0, &readback, tickets.buffer, NULL))
      mesh_tickets_check(c, "mesh tickets, two indirect draws", &readback, &tickets);

   memset(&c->draw, 0, sizeof(c->draw));
   vkDestroyShaderModule(c->device, mesh, NULL);
   vkDestroyShaderModule(c->device, frag, NULL);
   buffer_destroy(c, &args);
   buffer_destroy(c, &tickets);
   buffer_destroy(c, &readback);
}

/* Task shaders (radv/shaders/task_payload.task): 8,192 task workgroups, each
 * filling its 16 KiB payload with its number and launching two mesh workgroups
 * that paint their 2-pixel cells from its first and last words. The port runs
 * task shaders in chunks of as many workgroups as its payload ring holds
 * (4,096 of these), so the draw crosses a chunk; every cell must name its task
 * and mesh workgroup, and no payload may be torn. */
static bool
task_payload_check(const char *what, const struct buffer *readback)
{
   const uint8_t *pixels = readback->map;
   uint32_t wrong = 0, torn = 0;
   for (uint32_t cell = 0; cell < 128 * 128; cell++) {
      const uint32_t task = cell / 2, workgroup = cell % 2;
      const uint32_t x = (cell % 128) * 2, y = (cell / 128) * 2;
      const uint8_t *p = pixels + (y * TARGET_SIZE + x) * 4;
      const uint8_t expected[4] = {task & 255, (task >> 8) | (workgroup << 7), 0, 255};
      torn += p[2] == 255;
      wrong += memcmp(p, expected, 4) != 0;
   }
   char label[160];
   snprintf(label, sizeof(label), "%s: 8192 task workgroups' payloads reach their 16384 mesh workgroups", what);
   const bool ok = wrong == 0;
   if (!ok)
      report("%s: %u cells wrong, %u from torn payloads", what, wrong, torn);
   check(ok, label);
   return ok;
}

static void
test_task(struct context *c)
{
   struct buffer readback = {0}, args = {0};
   if (!buffer_create(c, TARGET_SIZE * TARGET_SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback) ||
       !buffer_create(c, 2 * sizeof(VkDrawMeshTasksIndirectCommandEXT), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, &args)) {
      check(false, "task: buffers");
      return;
   }
   VkShaderModule task = shader(c, radv_smoke_task_payload_task, sizeof(radv_smoke_task_payload_task));
   VkShaderModule mesh = shader(c, radv_smoke_mesh_payload_mesh, sizeof(radv_smoke_mesh_payload_mesh));
   VkShaderModule frag = shader(c, radv_smoke_mesh_colour_frag, sizeof(radv_smoke_mesh_colour_frag));
   const VkPipelineShaderStageCreateInfo stages[3] = {
      stage_info(VK_SHADER_STAGE_TASK_BIT_EXT, task),
      stage_info(VK_SHADER_STAGE_MESH_BIT_EXT, mesh),
      stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
   };

   c->draw.mode = DRAW_MESH;
   c->draw.groups[0] = 128;
   c->draw.groups[1] = 64;
   c->draw.groups[2] = 1;
   if (render_readback_with(c, "task payloads, one draw", stages, 3, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 0,
                            &readback, VK_NULL_HANDLE, NULL))
      task_payload_check("task payloads, one draw", &readback);

   VkDrawMeshTasksIndirectCommandEXT *const commands = args.map;
   commands[0] = (VkDrawMeshTasksIndirectCommandEXT){128, 32, 1};
   commands[1] = commands[0];
   c->draw.mode = DRAW_MESH_INDIRECT;
   c->draw.args = args.buffer;
   c->draw.max_draws = 2;
   if (render_readback_with(c, "task payloads, two indirect draws", stages, 3, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                            0, 0, &readback, VK_NULL_HANDLE, NULL))
      task_payload_check("task payloads, two indirect draws", &readback);

   memset(&c->draw, 0, sizeof(c->draw));
   vkDestroyShaderModule(c->device, task, NULL);
   vkDestroyShaderModule(c->device, mesh, NULL);
   vkDestroyShaderModule(c->device, frag, NULL);
   buffer_destroy(c, &args);
   buffer_destroy(c, &readback);
}

/* The external host memory probe (docs/CTS_GAPS.md, VK_EXT_external_memory_host):
 * whether the GPU reaches a title's own anonymous (flexible) memory once
 * sceKernelMprotect grants it GPU access, at the address the CPU uses, as the
 * driver's direct memory is. A shader writes 16 KiB through a raw device
 * address. A GPU fault ends the run, so this check comes last. */
#if defined(__PROSPERO__)
int32_t sceKernelMprotect(const void *address, size_t length, int protection);
#include <sys/mman.h>

static void
test_host_pointer(struct context *c)
{
   enum { WORDS = 4096 };
   void *const memory = mmap(NULL, WORDS * 4, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
   if (memory == MAP_FAILED) {
      check(false, "host pointer: mmap");
      return;
   }
   memset(memory, 0, WORDS * 4);
   /* CPU read and write, GPU read and write. */
   const int32_t result = sceKernelMprotect(memory, WORDS * 4, 0x1 | 0x2 | 0x10 | 0x20);
   report("host pointer: sceKernelMprotect with GPU access returned 0x%08x", (unsigned)result);
   if (result != 0) {
      check(false, "host pointer: anonymous memory takes GPU access");
      munmap(memory, WORDS * 4);
      return;
   }

   const VkPushConstantRange range = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 12};
   const VkPipelineLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &range,
   };
   VkPipelineLayout layout = VK_NULL_HANDLE;
   VkPipeline pipeline = VK_NULL_HANDLE;
   VkShaderModule module = shader(c, radv_smoke_ptr_write_comp, sizeof(radv_smoke_ptr_write_comp));
   bool ok = module && vkCreatePipelineLayout(c->device, &layout_info, NULL, &layout) == VK_SUCCESS;
   const VkComputePipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage =
         {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module,
            .pName = "main",
         },
      .layout = layout,
   };
   ok = ok && vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline) == VK_SUCCESS;
   if (ok) {
      struct {
         uint64_t address;
         uint32_t value;
      } __attribute__((packed)) push = {(uint64_t)(uintptr_t)memory, 0x5a000000u};
      ok = begin(c);
      vkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdPushConstants(c->cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
      vkCmdDispatch(c->cmd, WORDS / 64, 1, 1);
      ok = ok && submit_and_wait(c, "host pointer");
   }
   unsigned wrong = 0;
   const volatile uint32_t *const words = memory;
   for (uint32_t i = 0; ok && i < WORDS; i++)
      wrong += words[i] != 0x5a000000u + i;
   if (ok && wrong)
      report("host pointer: %u of %u words wrong, the first reads 0x%08x", wrong, WORDS, words[0]);
   check(ok && wrong == 0, "host pointer: a shader writes a title's anonymous memory through its address");
   vkDestroyPipeline(c->device, pipeline, NULL);
   vkDestroyPipelineLayout(c->device, layout, NULL);
   vkDestroyShaderModule(c->device, module, NULL);
   munmap(memory, WORDS * 4);
}
#endif

int
main(void)
{
   const int captured = ps5_klog_capture_stderr("[radv-smoke:stderr] ");
   /* Optional NAME=VALUE lines for the driver's environment (RADV_DEBUG,
    * ACO_DEBUG), read before the driver is. */
   FILE *const env = fopen("/app0/radv-smoke-env.txt", "r");
   if (env) {
      char line[256];
      while (fgets(line, sizeof(line), env)) {
         line[strcspn(line, "\r\n")] = '\0';
         char *const equals = strchr(line, '=');
         if (equals && line[0] != '#') {
            *equals = '\0';
            setenv(line, equals + 1, 1);
         }
      }
      fclose(env);
   }
   bionic_environ_setup(); /* the engine's environment (mimalloc options) */
   /* RADV_SMOKE_STDERR names a file for standard error instead of klog, for
    * output too large for it (RADV_DEBUG=shaders). */
   if (getenv("RADV_SMOKE_STDERR") && freopen(getenv("RADV_SMOKE_STDERR"), "w", stderr))
      setvbuf(stderr, NULL, _IOLBF, 0);
   results = fopen(RESULTS_PATH, "w");
   report("RADV smoke test starts");
   /* The line below arrives in klog prefixed "[radv-smoke:stderr]" when the
    * capture works; the driver's own messages take the same way. */
   report("standard error to klog: %s", captured == 0 ? "captured" : "not captured");
   fprintf(stderr, "standard error reaches klog\n");
  test_fp_state();
  struct context c = {0};
   /* RADV_SMOKE_ONLY=task runs the task shader checks alone, for a driver
    * debugging them (RADV_DEBUG=shaders dumps every shader compiled). */
   const bool ready = context_create(&c);
#if defined(__PROSPERO__)
   if (ready)
      test_title_files();
#endif
   const char *const only = getenv("RADV_SMOKE_ONLY");
   if (ready && only && strcmp(only, "demo") == 0) {
      if (c.display)
         test_demo(&c);
      else
         check(false, "demo: VK_KHR_display is not reported");
   } else if (ready && only && strcmp(only, "task") == 0) {
      if (c.task)
         test_task(&c);
   } else if (ready) {
      test_fill(&c);
      test_copy(&c);
      test_compute(&c);
      test_triangle(&c);
      test_tessellation(&c);
      /* Diagnostic runs without NGG, where a legacy GS hangs, skip the GS checks. */
      if (!getenv("RADV_SMOKE_SKIP_GEOMETRY"))
         test_geometry(&c);
      test_scratch(&c);
      test_coherence(&c);
      test_primitive_id(&c);
      if (c.barycentric) {
         test_barycentric(&c);
         test_barycentric_pair(&c);
      } else {
         report("barycentric: VK_KHR_fragment_shader_barycentric is not reported; its checks are skipped");
      }
      if (c.shading_rate)
         test_shading_rate(&c);
      else
         report("shading rate: VK_KHR_fragment_shading_rate is not reported; its checks are skipped");
      if (c.acceleration_structure)
         test_acceleration_structure(&c);
      if (c.mesh)
         test_mesh(&c);
      else
         report("mesh: VK_EXT_mesh_shader is not reported; its checks are skipped");
      if (c.task)
         test_task(&c);
      else
         report("task: task shaders are not reported; their checks are skipped");
      if (getenv("RADV_SMOKE_SPARSE_TIMING") && supported_sparse_binding(&c))
         test_sparse_timing(&c);
      if (c.display)
         test_display(&c);
      else
         report("display: VK_KHR_display is not reported; its checks are skipped");
#if defined(__PROSPERO__)
      /* Last: a GPU fault ends the run. It needs buffer device addresses. */
      if (c.acceleration_structure)
         test_host_pointer(&c);
#endif
   }
   if (c.device) {
      vkDeviceWaitIdle(c.device);
      vkDestroyFence(c.device, c.fence, NULL);
      vkDestroyCommandPool(c.device, c.pool, NULL);
      vkDestroyDevice(c.device, NULL);
   }
   if (c.instance)
      vkDestroyInstance(c.instance, NULL);
   report("RADV smoke test ends: %u passed, %u failed", passed, failed);
   if (results)
      fclose(results);
   return failed == 0 && passed > 0 ? 0 : 1;
}

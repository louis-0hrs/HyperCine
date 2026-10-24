// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
//
// Headless Vulkan compute backend for the HyperCine P1 slice.
//
// Design choices (all documented in docs/LINUX_VULKAN_BACKEND.md):
//   * libvulkan is loaded through dlopen so that a host without the Vulkan
//     loader at runtime still links the binary; discovery then reports
//     "unavailable" instead of aborting at load time.
//   * Buffers are host-visible + host-coherent for P1: correctness first. The
//     measured GPU time is the compute dispatch (timestamped), not the copy.
//   * Nothing here includes Vulkan headers in a public header; they live only
//     in this translation unit.

#include "hypercine/vulkan_frontend.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
static void* vk_open(const char* p) { return reinterpret_cast<void*>(LoadLibraryA(p)); }
static void* vk_sym(void* h, const char* n) { return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(h), n)); }
#else
#include <dlfcn.h>
static void* vk_open(const char* p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static void* vk_sym(void* h, const char* n) { return dlsym(h, n); }
#endif

// The generated header ships static const arrays per shader.
#include "vulkan_shaders_generated.h"

namespace hypercine {
namespace vulkan {
namespace {

constexpr uint32_t kInvalid = 0xFFFFFFFFu;

PFN_vkGetInstanceProcAddr fp_vkGetInstanceProcAddr = nullptr;

#define VK_FN_LIST(X) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkCreateDevice) \
    X(vkEnumeratePhysicalDevices) \
    X(vkDestroyInstance) \
    X(vkGetDeviceQueue) \
    X(vkCreateBuffer) \
    X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) \
    X(vkAllocateMemory) \
    X(vkFreeMemory) \
    X(vkBindBufferMemory) \
    X(vkMapMemory) \
    X(vkUnmapMemory) \
    X(vkCreateShaderModule) \
    X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) \
    X(vkCreateComputePipelines) \
    X(vkDestroyPipeline) \
    X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) \
    X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) \
    X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdPushConstants) \
    X(vkCmdDispatch) \
    X(vkCmdPipelineBarrier) \
    X(vkEndCommandBuffer) \
    X(vkQueueSubmit) \
    X(vkQueueWaitIdle) \
    X(vkDestroyDevice) \
    X(vkCreateQueryPool) \
    X(vkDestroyQueryPool) \
    X(vkCmdWriteTimestamp) \
    X(vkGetQueryPoolResults) \
    X(vkCmdResetQueryPool)

struct Api {
    void* handle = nullptr;
#define DECL(f) PFN_##f f = nullptr;
    VK_FN_LIST(DECL)
#undef DECL
    PFN_vkCreateInstance vkCreateInstance = nullptr;

    bool load() {
#if defined(_WIN32)
        const char* libs[] = {"vulkan-1.dll"};
#else
        const char* libs[] = {"libvulkan.so.1", "libvulkan.so"};
#endif
        for (const char* l : libs) { handle = vk_open(l); if (handle) break; }
        if (!handle) return false;
        fp_vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(vk_sym(handle, "vkGetInstanceProcAddr"));
        if (!fp_vkGetInstanceProcAddr) return false;
        vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(fp_vkGetInstanceProcAddr(nullptr, "vkCreateInstance"));
        if (!vkCreateInstance) return false;
        return true;
    }
};

Api g_api;
bool g_api_loaded = false;

bool ensure_api() {
    if (g_api_loaded) return g_api.handle != nullptr;
    g_api_loaded = true;
    g_api.load();
    return g_api.handle != nullptr;
}

// Resolve a device-level function through the loader once a device is live.
void* dev_proc(VkInstance instance, const char* name) {
    return reinterpret_cast<void*>(fp_vkGetInstanceProcAddr(instance, name));
}

std::string version_string(uint32_t v) {
    std::ostringstream ss;
    ss << VK_VERSION_MAJOR(v) << "." << VK_VERSION_MINOR(v) << "." << VK_VERSION_PATCH(v);
    return ss.str();
}

struct InstanceBundle {
    VkInstance instance = VK_NULL_HANDLE;
    std::vector<VkPhysicalDevice> devices;
    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties mem{};
    uint32_t compute_family = kInvalid;
};

bool open_instance(InstanceBundle& out, std::string& why) {
    if (!ensure_api()) { why = "libvulkan could not be loaded"; return false; }

    // Instance-level entry points must be queried with a live VkInstance;
    // vkGetInstanceProcAddr(nullptr, ...) only returns global commands.
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "HyperCine";
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;

    // VK_KHR_portability_enumeration is needed on some platforms (e.g. macOS
    // MoltenVK) to enumerate non-conformant devices; harmless when absent.
    std::vector<const char*> exts;
#if defined(__APPLE__)
    exts.push_back("VK_KHR_portability_enumeration");
#endif
    ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.empty() ? nullptr : exts.data();

    if (g_api.vkCreateInstance(&ci, nullptr, &out.instance) != VK_SUCCESS) {
        why = "vkCreateInstance failed";
        return false;
    }

    // Load instance-level commands now that an instance exists.
    g_api.vkEnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(fp_vkGetInstanceProcAddr(out.instance, "vkEnumeratePhysicalDevices"));
    g_api.vkGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(fp_vkGetInstanceProcAddr(out.instance, "vkGetPhysicalDeviceProperties"));
    g_api.vkGetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(fp_vkGetInstanceProcAddr(out.instance, "vkGetPhysicalDeviceMemoryProperties"));
    g_api.vkGetPhysicalDeviceQueueFamilyProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(fp_vkGetInstanceProcAddr(out.instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    g_api.vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(fp_vkGetInstanceProcAddr(out.instance, "vkCreateDevice"));
    g_api.vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(fp_vkGetInstanceProcAddr(out.instance, "vkDestroyInstance"));
    if (!g_api.vkEnumeratePhysicalDevices || !g_api.vkGetPhysicalDeviceProperties ||
        !g_api.vkGetPhysicalDeviceMemoryProperties || !g_api.vkGetPhysicalDeviceQueueFamilyProperties ||
        !g_api.vkCreateDevice || !g_api.vkDestroyInstance) {
        why = "Vulkan instance commands unavailable from the loader";
        return false;
    }

    uint32_t count = 0;
    if (g_api.vkEnumeratePhysicalDevices(out.instance, &count, nullptr) != VK_SUCCESS || count == 0) {
        why = "no Vulkan physical devices";
        return false;
    }
    out.devices.resize(count);
    g_api.vkEnumeratePhysicalDevices(out.instance, &count, out.devices.data());

    // Prefer a discrete GPU with a compute queue; fall back to integrated, then
    // anything with a compute queue. Never silently pick a CPU device if a
    // real GPU is present.
    int best_score = -1;
    for (VkPhysicalDevice d : out.devices) {
        VkPhysicalDeviceProperties p{};
        g_api.vkGetPhysicalDeviceProperties(d, &p);
        uint32_t qcount = 0;
        g_api.vkGetPhysicalDeviceQueueFamilyProperties(d, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qcount);
        g_api.vkGetPhysicalDeviceQueueFamilyProperties(d, &qcount, qs.data());
        uint32_t fam = kInvalid;
        for (uint32_t i = 0; i < qcount; ++i) {
            if (qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { fam = i; break; }
        }
        if (fam == kInvalid) continue;
        int score = 0;
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score = 3;
        else if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score = 2;
        else if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) score = 0;
        else score = 1;
        if (score > best_score) {
            best_score = score;
            out.chosen = d;
            out.props = p;
            out.compute_family = fam;
        }
    }
    if (out.chosen == VK_NULL_HANDLE) { why = "no compute-capable queue family"; return false; }
    g_api.vkGetPhysicalDeviceMemoryProperties(out.chosen, &out.mem);
    return true;
}

uint32_t find_host_memory_type(const VkPhysicalDeviceMemoryProperties& mem) {
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        if ((mem.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    return kInvalid;
}

} // namespace

struct VulkanFrontend::Impl {
    double last_gpu_ns = -1.0;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = kInvalid;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties mem{};
    uint32_t host_memory = kInvalid;

    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;
    VkQueryPool query_pool = VK_NULL_HANDLE;
    bool timestamps = false;

    // device-level entry points
    PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
    PFN_vkCreateBuffer vkCreateBuffer = nullptr;
    PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory vkAllocateMemory = nullptr;
    PFN_vkFreeMemory vkFreeMemory = nullptr;
    PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
    PFN_vkMapMemory vkMapMemory = nullptr;
    PFN_vkUnmapMemory vkUnmapMemory = nullptr;
    PFN_vkCreateShaderModule vkCreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule vkDestroyShaderModule = nullptr;
    PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout vkCreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout = nullptr;
    PFN_vkCreateComputePipelines vkCreateComputePipelines = nullptr;
    PFN_vkDestroyPipeline vkDestroyPipeline = nullptr;
    PFN_vkCreateDescriptorPool vkCreateDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets = nullptr;
    PFN_vkFreeDescriptorSets vkFreeDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets = nullptr;
    PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers vkFreeCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
    PFN_vkCmdBindPipeline vkCmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets = nullptr;
    PFN_vkCmdPushConstants vkCmdPushConstants = nullptr;
    PFN_vkCmdDispatch vkCmdDispatch = nullptr;
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
    PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
    PFN_vkQueueSubmit vkQueueSubmit = nullptr;
    PFN_vkQueueWaitIdle vkQueueWaitIdle = nullptr;
    PFN_vkCreateQueryPool vkCreateQueryPool = nullptr;
    PFN_vkCmdWriteTimestamp vkCmdWriteTimestamp = nullptr;
    PFN_vkGetQueryPoolResults vkGetQueryPoolResults = nullptr;
    PFN_vkCmdResetQueryPool vkCmdResetQueryPool = nullptr;
    PFN_vkDestroyQueryPool vkDestroyQueryPool = nullptr;
    PFN_vkDestroyDevice vkDestroyDevice = nullptr;

    template <typename T>
    void bind(const char* name, T& slot) {
        slot = reinterpret_cast<T>(dev_proc(instance, name));
        if (!slot) throw std::runtime_error(std::string("Vulkan device function missing: ") + name);
    }

    // --- resource helpers ---
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };

    Buffer make_buffer(VkDeviceSize bytes, VkBufferUsageFlags usage) {
        Buffer b;
        b.size = bytes;
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = bytes;
        bi.usage = usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bi, nullptr, &b.buffer) != VK_SUCCESS)
            throw std::runtime_error("vkCreateBuffer failed");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, b.buffer, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = host_memory;
        if (vkAllocateMemory(device, &ai, nullptr, &b.memory) != VK_SUCCESS)
            throw std::runtime_error("vkAllocateMemory failed");
        if (vkBindBufferMemory(device, b.buffer, b.memory, 0) != VK_SUCCESS)
            throw std::runtime_error("vkBindBufferMemory failed");
        if (vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped) != VK_SUCCESS)
            throw std::runtime_error("vkMapMemory failed");
        return b;
    }

    void destroy(Buffer& b) {
        if (b.mapped) { vkUnmapMemory(device, b.memory); b.mapped = nullptr; }
        if (b.buffer) { vkDestroyBuffer(device, b.buffer, nullptr); b.buffer = VK_NULL_HANDLE; }
        if (b.memory) { vkFreeMemory(device, b.memory, nullptr); b.memory = VK_NULL_HANDLE; }
    }

    struct Pipeline {
        VkShaderModule module = VK_NULL_HANDLE;
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
    };

    Pipeline make_pipeline(const uint32_t* spv, unsigned words, uint32_t binding_count,
                          uint32_t push_bytes) {
        Pipeline p;
        VkShaderModuleCreateInfo mi{};
        mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mi.codeSize = static_cast<size_t>(words) * 4u;
        mi.pCode = spv;
        if (vkCreateShaderModule(device, &mi, nullptr, &p.module) != VK_SUCCESS)
            throw std::runtime_error("vkCreateShaderModule failed");

        std::vector<VkDescriptorSetLayoutBinding> b(binding_count);
        for (uint32_t i = 0; i < binding_count; ++i) {
            b[i].binding = i;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dl{};
        dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dl.bindingCount = binding_count;
        dl.pBindings = b.data();
        if (vkCreateDescriptorSetLayout(device, &dl, nullptr, &p.dsl) != VK_SUCCESS)
            throw std::runtime_error("vkCreateDescriptorSetLayout failed");

        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        range.offset = 0;
        range.size = push_bytes;
        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &p.dsl;
        pl.pushConstantRangeCount = push_bytes ? 1u : 0u;
        pl.pPushConstantRanges = push_bytes ? &range : nullptr;
        if (vkCreatePipelineLayout(device, &pl, nullptr, &p.layout) != VK_SUCCESS)
            throw std::runtime_error("vkCreatePipelineLayout failed");

        VkComputePipelineCreateInfo cp{};
        cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cp.stage.module = p.module;
        cp.stage.pName = "main";
        cp.layout = p.layout;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp, nullptr, &p.pipeline) != VK_SUCCESS)
            throw std::runtime_error("vkCreateComputePipelines failed");
        return p;
    }

    void destroy_pipeline(Pipeline& p) {
        if (p.pipeline) vkDestroyPipeline(device, p.pipeline, nullptr);
        if (p.layout) vkDestroyPipelineLayout(device, p.layout, nullptr);
        if (p.dsl) vkDestroyDescriptorSetLayout(device, p.dsl, nullptr);
        if (p.module) vkDestroyShaderModule(device, p.module, nullptr);
        p = Pipeline{};
    }

    // One dispatch: upload `input` into buffer 0, run, download all writable
    // buffers back into the provided host pointers via a caller-supplied copy.
    struct DispatchResult {
        double gpu_ns = -1.0; // -1 when timestamps are unavailable
    };

    DispatchResult run(Pipeline& pipe, uint32_t binding_count,
                       std::vector<VkDeviceSize> sizes,
                       const std::vector<Buffer>& buffers,
                       const void* push, uint32_t push_bytes,
                       uint32_t gx, uint32_t gy, uint32_t gz) {
        // descriptor set
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = desc_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &pipe.dsl;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(device, &ai, &set) != VK_SUCCESS)
            throw std::runtime_error("vkAllocateDescriptorSets failed");

        std::vector<VkDescriptorBufferInfo> infos(binding_count);
        std::vector<VkWriteDescriptorSet> writes(binding_count);
        for (uint32_t i = 0; i < binding_count; ++i) {
            infos[i].buffer = buffers[i].buffer;
            infos[i].offset = 0;
            infos[i].range = VK_WHOLE_SIZE;
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device, binding_count, writes.data(), 0, nullptr);

        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = cmd_pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device, &cbai, &cmd) != VK_SUCCESS)
            throw std::runtime_error("vkAllocateCommandBuffers failed");

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        // Query reset is a recording-time command; it must sit between
        // vkBeginCommandBuffer and vkEndCommandBuffer.
        if (timestamps) vkCmdResetQueryPool(cmd, query_pool, 0, 2);
        if (timestamps)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, query_pool, 0);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.layout, 0, 1, &set, 0, nullptr);
        if (push_bytes) vkCmdPushConstants(cmd, pipe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes, push);
        vkCmdDispatch(cmd, gx, gy, gz);
        if (timestamps)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool, 1);
        vkEndCommandBuffer(cmd);

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS)
            throw std::runtime_error("vkQueueSubmit failed");
        if (vkQueueWaitIdle(queue) != VK_SUCCESS)
            throw std::runtime_error("vkQueueWaitIdle failed");

        // Release the per-dispatch set and its recording so the pool is reused.
        vkFreeCommandBuffers(device, cmd_pool, 1, &cmd);
        if (vkFreeDescriptorSets) vkFreeDescriptorSets(device, desc_pool, 1, &set);

        DispatchResult res;
        if (timestamps) {
            uint64_t ts[2] = {0, 0};
            if (vkGetQueryPoolResults(device, query_pool, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
                const double period = static_cast<double>(props.limits.timestampPeriod);
                res.gpu_ns = static_cast<double>(ts[1] - ts[0]) * period;
            }
        }
        last_gpu_ns = res.gpu_ns;
        return res;
    }

    void create_device(const InstanceBundle& b) {
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = b.compute_family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        if (g_api.vkCreateDevice(b.chosen, &dci, nullptr, &device) != VK_SUCCESS)
            throw std::runtime_error("vkCreateDevice failed");

        auto load_dev = [&](const char* n) { return dev_proc(instance, n); };
#define LOADDEV(f) f = reinterpret_cast<PFN_##f>(load_dev(#f)); if (!f) throw std::runtime_error(std::string("missing device fn ") + #f);
        LOADDEV(vkGetDeviceQueue)
        LOADDEV(vkCreateBuffer)
        LOADDEV(vkDestroyBuffer)
        LOADDEV(vkGetBufferMemoryRequirements)
        LOADDEV(vkAllocateMemory)
        LOADDEV(vkFreeMemory)
        LOADDEV(vkBindBufferMemory)
        LOADDEV(vkMapMemory)
        LOADDEV(vkUnmapMemory)
        LOADDEV(vkCreateShaderModule)
        LOADDEV(vkDestroyShaderModule)
        LOADDEV(vkCreateDescriptorSetLayout)
        LOADDEV(vkDestroyDescriptorSetLayout)
        LOADDEV(vkCreatePipelineLayout)
        LOADDEV(vkDestroyPipelineLayout)
        LOADDEV(vkCreateComputePipelines)
        LOADDEV(vkDestroyPipeline)
        LOADDEV(vkCreateDescriptorPool)
        LOADDEV(vkDestroyDescriptorPool)
        LOADDEV(vkAllocateDescriptorSets)
        LOADDEV(vkFreeDescriptorSets)
        LOADDEV(vkUpdateDescriptorSets)
        LOADDEV(vkCreateCommandPool)
        LOADDEV(vkDestroyCommandPool)
        LOADDEV(vkAllocateCommandBuffers)
        LOADDEV(vkFreeCommandBuffers)
        LOADDEV(vkBeginCommandBuffer)
        LOADDEV(vkCmdBindPipeline)
        LOADDEV(vkCmdBindDescriptorSets)
        LOADDEV(vkCmdPushConstants)
        LOADDEV(vkCmdDispatch)
        LOADDEV(vkEndCommandBuffer)
        LOADDEV(vkQueueSubmit)
        LOADDEV(vkQueueWaitIdle)
#undef LOADDEV
        // optional
        vkCmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(load_dev("vkCmdPipelineBarrier"));
        vkCreateQueryPool = reinterpret_cast<PFN_vkCreateQueryPool>(load_dev("vkCreateQueryPool"));
        vkCmdWriteTimestamp = reinterpret_cast<PFN_vkCmdWriteTimestamp>(load_dev("vkCmdWriteTimestamp"));
        vkGetQueryPoolResults = reinterpret_cast<PFN_vkGetQueryPoolResults>(load_dev("vkGetQueryPoolResults"));
        vkCmdResetQueryPool = reinterpret_cast<PFN_vkCmdResetQueryPool>(load_dev("vkCmdResetQueryPool"));
        vkDestroyQueryPool = reinterpret_cast<PFN_vkDestroyQueryPool>(load_dev("vkDestroyQueryPool"));

        vkGetDeviceQueue(device, b.compute_family, 0, &queue);

        vkDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(load_dev("vkDestroyDevice"));
        if (!vkDestroyDevice) throw std::runtime_error("missing device fn vkDestroyDevice");

        VkCommandPoolCreateInfo cp{};
        cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cp.queueFamilyIndex = b.compute_family;
        if (vkCreateCommandPool(device, &cp, nullptr, &cmd_pool) != VK_SUCCESS)
            throw std::runtime_error("vkCreateCommandPool failed");

        VkDescriptorPoolSize size{};
        size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        size.descriptorCount = 1024;
        VkDescriptorPoolCreateInfo dp{};
        dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        // Sets are freed after each dispatch; the flag is mandatory for that.
        dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        dp.maxSets = 256;
        dp.poolSizeCount = 1;
        dp.pPoolSizes = &size;
        if (vkCreateDescriptorPool(device, &dp, nullptr, &desc_pool) != VK_SUCCESS)
            throw std::runtime_error("vkCreateDescriptorPool failed");

        uint32_t qcount = 0;
        g_api.vkGetPhysicalDeviceQueueFamilyProperties(physical, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qcount);
        g_api.vkGetPhysicalDeviceQueueFamilyProperties(physical, &qcount, qs.data());
        const bool ts_ok = timestamps = (b.compute_family < qcount && qs[b.compute_family].timestampValidBits > 0)
                          && vkCreateQueryPool && vkCmdWriteTimestamp && vkGetQueryPoolResults && vkCmdResetQueryPool;
        if (ts_ok) {
            VkQueryPoolCreateInfo qi{};
            qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2;
            if (vkCreateQueryPool(device, &qi, nullptr, &query_pool) != VK_SUCCESS) {
                query_pool = VK_NULL_HANDLE;
                timestamps = false;
            }
        }
    }
};

VulkanFrontend::VulkanFrontend() : impl_(nullptr) {
    info_ = DiscoverDevice();
    if (!info_.available) return;
    try {
        InstanceBundle b;
        std::string why;
        if (!open_instance(b, why)) { info_.available = false; info_.notes = why; return; }
        impl_ = new Impl();
        impl_->instance = b.instance;
        impl_->physical = b.chosen;
        impl_->props = b.props;
        impl_->mem = b.mem;
        impl_->queue_family = b.compute_family;
        impl_->host_memory = find_host_memory_type(b.mem);
        if (impl_->host_memory == kInvalid) throw std::runtime_error("no host-visible memory type");
        impl_->create_device(b);
    } catch (const std::exception& e) {
        info_.available = false;
        info_.notes = e.what();
        if (impl_) { delete impl_; impl_ = nullptr; }
    }
}

VulkanFrontend::~VulkanFrontend() {
    if (!impl_) return;
    if (impl_->device) {
        if (impl_->query_pool && impl_->vkDestroyQueryPool) impl_->vkDestroyQueryPool(impl_->device, impl_->query_pool, nullptr);
        if (impl_->desc_pool) impl_->vkDestroyDescriptorPool(impl_->device, impl_->desc_pool, nullptr);
        if (impl_->cmd_pool) impl_->vkDestroyCommandPool(impl_->device, impl_->cmd_pool, nullptr);
        impl_->vkDestroyDevice(impl_->device, nullptr);
    }
    if (impl_->instance) g_api.vkDestroyInstance(impl_->instance, nullptr);
    delete impl_;
    impl_ = nullptr;
}

DeviceInfo DiscoverDevice() {
    DeviceInfo out;
    InstanceBundle b;
    std::string why;
    if (!open_instance(b, why)) { out.notes = why; return out; }
    out.available = true;
    out.device_name = b.props.deviceName;
    out.vendor_id = b.props.vendorID;
    out.device_id = b.props.deviceID;
    out.api_version = b.props.apiVersion;
    out.driver_version = b.props.driverVersion;
    out.compute_queue_family = b.compute_family;
    out.max_storage_buffer_range = b.props.limits.maxStorageBufferRange;
    out.max_compute_workgroup_invocations = b.props.limits.maxComputeWorkGroupInvocations;
    g_api.vkDestroyInstance(b.instance, nullptr);
    return out;
}

bool IsAvailable() { return DiscoverDevice().available; }

double VulkanFrontend::last_dispatch_gpu_ns() const {
    return impl_ ? impl_->last_gpu_ns : -1.0;
}

std::string VulkanFrontend::ShaderFingerprint() const {
    std::ostringstream ss;
    ss << "color_cg709 words " << color_cg709_spv_words << " fnv1a64 "
       << std::hex << color_cg709_spv_fnv1a64 << std::dec << "\n";
    ss << "wavelet_h26 words " << wavelet_h26_spv_words << " fnv1a64 "
       << std::hex << wavelet_h26_spv_fnv1a64 << std::dec << "\n";
    return ss.str();
}

void VulkanFrontend::ConvertBgra(const uint8_t* data, std::size_t bytes,
                                 int width, int height, std::size_t stride,
                                 int16_t* y, int y_stride,
                                 int16_t* cr, int16_t* cb, int c_stride) {
    if (!impl_) throw std::runtime_error("Vulkan frontend not available: " + info_.notes);
    if (!data || width < 2 || height < 1) throw std::runtime_error("invalid color input");
    if (stride < static_cast<std::size_t>(width) * 4u) throw std::runtime_error("stride too small");
    if (bytes < stride * (static_cast<std::size_t>(height) - 1u) + static_cast<std::size_t>(width) * 4u)
        throw std::runtime_error("input bytes too small");

    // Input: one uint per pixel (b | g<<8 | r<<16), stride in uints.
    const uint32_t in_stride = static_cast<uint32_t>(width);
    std::vector<uint32_t> packed(static_cast<size_t>(in_stride) * height);
    for (int row = 0; row < height; ++row) {
        const uint8_t* src = data + static_cast<size_t>(row) * stride;
        for (int x = 0; x < width; ++x) {
            const uint8_t* p = src + static_cast<size_t>(x) * 4u;
            packed[static_cast<size_t>(row) * in_stride + x] =
                static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16);
        }
    }

    const uint32_t y_str = static_cast<uint32_t>(y_stride);
    const uint32_t c_str = static_cast<uint32_t>(c_stride >= width / 2 ? c_stride : width / 2);

    auto in_buf = impl_->make_buffer(static_cast<VkDeviceSize>(in_stride) * height * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto y_buf = impl_->make_buffer(static_cast<VkDeviceSize>(y_str) * height * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto cr_buf = impl_->make_buffer(static_cast<VkDeviceSize>(c_str) * height * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto cb_buf = impl_->make_buffer(static_cast<VkDeviceSize>(c_str) * height * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memcpy(in_buf.mapped, packed.data(), packed.size() * 4u);

    struct Push { uint32_t width, height, in_stride, y_stride, c_stride; } push{};
    push.width = static_cast<uint32_t>(width);
    push.height = static_cast<uint32_t>(height);
    push.in_stride = in_stride;
    push.y_stride = y_str;
    push.c_stride = c_str;

    auto pipe = impl_->make_pipeline(color_cg709_spv, color_cg709_spv_words, 4, sizeof(Push));
    std::vector<Impl::Buffer> bufs{in_buf, y_buf, cr_buf, cb_buf};
    impl_->run(pipe, 4, {}, bufs, &push, sizeof(Push),
               (static_cast<uint32_t>(width) + 7u) / 8u, (static_cast<uint32_t>(height) + 7u) / 8u, 1u);

    // Download int16 planes from the int32 device planes.
    auto copy_out = [&](const Impl::Buffer& src, int16_t* dst, int dw, int dh, int dstride) {
        const int32_t* mp = static_cast<const int32_t*>(src.mapped);
        const size_t row_elems = static_cast<size_t>(src.size) / 4u / static_cast<size_t>(dh);
        for (int row = 0; row < dh; ++row)
            for (int x = 0; x < dw; ++x)
                dst[static_cast<size_t>(row) * dstride + x] =
                    static_cast<int16_t>(mp[static_cast<size_t>(row) * row_elems + x]);
    };
    copy_out(y_buf, y, width, height, y_stride);
    copy_out(cr_buf, cr, width / 2, height, c_stride);
    copy_out(cb_buf, cb, width / 2, height, c_stride);

    impl_->destroy(in_buf);
    impl_->destroy(y_buf);
    impl_->destroy(cr_buf);
    impl_->destroy(cb_buf);
    impl_->destroy_pipeline(pipe);
}

void VulkanFrontend::ForwardHorizontal26(const int16_t* src, int width, int height, int src_stride,
                                         int16_t* low, int low_stride,
                                         int16_t* high, int high_stride) {
    if (!impl_) throw std::runtime_error("Vulkan frontend not available: " + info_.notes);
    if (width < 6 || (width % 2) || height < 1) throw std::runtime_error("invalid wavelet dimensions");

    const uint32_t half_w = static_cast<uint32_t>(width / 2);
    const uint32_t src_str = static_cast<uint32_t>(src_stride);
    const uint32_t low_str = static_cast<uint32_t>(low_stride);
    const uint32_t high_str = static_cast<uint32_t>(high_stride);

    std::vector<int32_t> in(static_cast<size_t>(src_str) * height, 0);
    for (int row = 0; row < height; ++row)
        for (int x = 0; x < width; ++x)
            in[static_cast<size_t>(row) * src_str + x] = src[static_cast<size_t>(row) * src_stride + x];

    auto in_buf = impl_->make_buffer(in.size() * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto low_buf = impl_->make_buffer(static_cast<VkDeviceSize>(low_str) * height * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto high_buf = impl_->make_buffer(static_cast<VkDeviceSize>(high_str) * height * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memcpy(in_buf.mapped, in.data(), in.size() * 4u);

    struct Push { uint32_t width, height, src_stride, low_stride, high_stride, pad0; } push{};
    push.width = static_cast<uint32_t>(width);
    push.height = static_cast<uint32_t>(height);
    push.src_stride = src_str;
    push.low_stride = low_str;
    push.high_stride = high_str;

    auto pipe = impl_->make_pipeline(wavelet_h26_spv, wavelet_h26_spv_words, 3, sizeof(Push));
    std::vector<Impl::Buffer> bufs{in_buf, low_buf, high_buf};
    impl_->run(pipe, 3, {}, bufs, &push, sizeof(Push),
               (half_w + 7u) / 8u, (static_cast<uint32_t>(height) + 7u) / 8u, 1u);

    auto download = [&](const Impl::Buffer& b, int16_t* dst, int dw, int dh, int dstride) {
        const int32_t* mp = static_cast<const int32_t*>(b.mapped);
        const size_t row_elems = static_cast<size_t>(b.size) / 4u / static_cast<size_t>(dh);
        for (int row = 0; row < dh; ++row)
            for (int x = 0; x < dw; ++x)
                dst[static_cast<size_t>(row) * dstride + x] =
                    static_cast<int16_t>(mp[static_cast<size_t>(row) * row_elems + x]);
    };
    download(low_buf, low, width / 2, height, low_stride);
    download(high_buf, high, width / 2, height, high_stride);

    impl_->destroy(in_buf);
    impl_->destroy(low_buf);
    impl_->destroy(high_buf);
    impl_->destroy_pipeline(pipe);
}

void VulkanFrontend::ForwardVertical26(const int16_t* src, int width, int height, int src_stride,
                                       int16_t* low, int low_stride,
                                       int16_t* high, int high_stride) {
    if (!impl_) throw std::runtime_error("Vulkan frontend not available: " + info_.notes);
    if (height < 6 || (height % 2) || width < 1) throw std::runtime_error("invalid wavelet dimensions");

    const uint32_t src_str = static_cast<uint32_t>(src_stride);
    const uint32_t low_str = static_cast<uint32_t>(low_stride);
    const uint32_t high_str = static_cast<uint32_t>(high_stride);
    const uint32_t half_h = static_cast<uint32_t>(height / 2);

    std::vector<int32_t> in(static_cast<size_t>(src_str) * height, 0);
    for (int row = 0; row < height; ++row)
        for (int x = 0; x < width; ++x)
            in[static_cast<size_t>(row) * src_str + x] = src[static_cast<size_t>(row) * src_stride + x];

    auto in_buf = impl_->make_buffer(in.size() * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto low_buf = impl_->make_buffer(static_cast<VkDeviceSize>(low_str) * half_h * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto high_buf = impl_->make_buffer(static_cast<VkDeviceSize>(high_str) * half_h * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memcpy(in_buf.mapped, in.data(), in.size() * 4u);

    struct Push { uint32_t width, height, src_stride, low_stride, high_stride, pad0; } push{};
    push.width = static_cast<uint32_t>(width);
    push.height = static_cast<uint32_t>(height);
    push.src_stride = src_str;
    push.low_stride = low_str;
    push.high_stride = high_str;

    auto pipe = impl_->make_pipeline(wavelet_v26_spv, wavelet_v26_spv_words, 3, sizeof(Push));
    std::vector<Impl::Buffer> bufs{in_buf, low_buf, high_buf};
    impl_->run(pipe, 3, {}, bufs, &push, sizeof(Push),
               (static_cast<uint32_t>(width) + 7u) / 8u, (half_h + 7u) / 8u, 1u);

    auto download = [&](const Impl::Buffer& b, int16_t* dst, int dw, int dh, int dstride) {
        const int32_t* mp = static_cast<const int32_t*>(b.mapped);
        const size_t row_elems = static_cast<size_t>(b.size) / 4u / static_cast<size_t>(dh);
        for (int row = 0; row < dh; ++row)
            for (int x = 0; x < dw; ++x)
                dst[static_cast<size_t>(row) * dstride + x] =
                    static_cast<int16_t>(mp[static_cast<size_t>(row) * row_elems + x]);
    };
    download(low_buf, low, width, height / 2, low_stride);
    download(high_buf, high, width, height / 2, high_stride);

    impl_->destroy(in_buf);
    impl_->destroy(low_buf);
    impl_->destroy(high_buf);
    impl_->destroy_pipeline(pipe);
}

}} // namespace hypercine::vulkan

#include "plvk.h"
#include "../ffmpeg.h"

#include "streaming/session.h"
#include "streaming/streamutils.h"

// Implementation in plvk_c.c
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

#include <SDL3/SDL_vulkan.h>

extern "C" {
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/imgutils.h>
}

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <new>
#include <numeric>
#include <vector>
#include <set>

#ifndef VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME
#define VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME "VK_KHR_video_decode_av1"
#endif

// Keep these in sync with hwcontext_vulkan.c
static const char *k_OptionalDeviceExtensions[] = {
    /* Misc or required by other extensions */
    //VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME,
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
    VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME,
    VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME,
    VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,

    /* Imports/exports */
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
#ifdef Q_OS_WIN32
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
#endif

    /* Video encoding/decoding */
    VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
    VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
    VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
    VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
#if LIBAVCODEC_VERSION_MAJOR >= 61
    VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME, // FFmpeg 7.0 uses the official Khronos AV1 extension
#else
    "VK_MESA_video_decode_av1", // FFmpeg 6.1 uses the Mesa AV1 extension
#endif
};

static void pl_log_cb(void*, enum pl_log_level level, const char *msg)
{
    switch (level) {
    case PL_LOG_FATAL:
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s", msg);
        break;
    case PL_LOG_ERR:
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s", msg);
        break;
    case PL_LOG_WARN:
        if (strncmp(msg, "Masking `", 9) == 0) {
            return;
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s", msg);
        break;
    case PL_LOG_INFO:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s", msg);
        break;
    case PL_LOG_DEBUG:
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s", msg);
        break;
    case PL_LOG_NONE:
    case PL_LOG_TRACE:
        SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s", msg);
        break;
    }
}

namespace {

constexpr uint64_t kImportedHostAllocationMagic = UINT64_C(0x535443484f535449);

struct ImportedHostAllocation {
    uint64_t magic = kImportedHostAllocationMagic;
    pl_gpu gpu = nullptr;
    pl_buf buffer = nullptr;
    void* base = nullptr;
};

void freeImportedHostAllocation(void* opaque, uint8_t*)
{
    auto allocation = static_cast<ImportedHostAllocation*>(opaque);
    SDL_assert(allocation != nullptr);
    SDL_assert(allocation->magic == kImportedHostAllocationMagic);

    pl_buf_destroy(allocation->gpu, &allocation->buffer);
    SDL_aligned_free(allocation->base);
    delete allocation;
}

ImportedHostAllocation* getImportedHostAllocation(const AVFrame* frame, int plane)
{
    AVBufferRef* buffer = av_frame_get_plane_buffer(const_cast<AVFrame*>(frame), plane);
    if (buffer == nullptr) {
        return nullptr;
    }

    auto allocation = static_cast<ImportedHostAllocation*>(
        av_buffer_pool_buffer_get_opaque(buffer));
    if (allocation == nullptr || allocation->magic != kImportedHostAllocationMagic) {
        return nullptr;
    }

    return allocation;
}

size_t alignUp(size_t value, size_t alignment)
{
    SDL_assert(alignment != 0);
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

void PlVkRenderer::lockQueue(struct AVHWDeviceContext *dev_ctx, uint32_t queue_family, uint32_t index)
{
    auto me = (PlVkRenderer*)dev_ctx->user_opaque;
    me->m_Vulkan->lock_queue(me->m_Vulkan, queue_family, index);
}

void PlVkRenderer::unlockQueue(struct AVHWDeviceContext *dev_ctx, uint32_t queue_family, uint32_t index)
{
    auto me = (PlVkRenderer*)dev_ctx->user_opaque;
    me->m_Vulkan->unlock_queue(me->m_Vulkan, queue_family, index);
}

void PlVkRenderer::overlayUploadComplete(void* opaque)
{
    SDL_DestroySurface((SDL_Surface*)opaque);
}

int PlVkRenderer::getMappedBuffer(AVCodecContext *context, AVFrame *frame, int flags)
{
    auto decoder = static_cast<FFmpegVideoDecoder*>(context->opaque);
    auto renderer = static_cast<PlVkRenderer*>(decoder->getBackendRenderer());

    // FFmpegVideoDecoder owns AVCodecContext::opaque for ffGetFormat(). Give
    // libplacebo an isolated view of the context with the GPU pointer it
    // expects instead of mutating the live context from decoder worker threads.
    AVCodecContext mappedContext = *context;
    mappedContext.opaque = const_cast<pl_gpu*>(&renderer->m_Vulkan->gpu);
    return pl_get_buffer2(&mappedContext, frame, flags);
}

AVBufferRef* PlVkRenderer::allocateImportedHostBuffer(void* opaque, size_t size)
{
    auto renderer = static_cast<PlVkRenderer*>(opaque);
    pl_gpu gpu = renderer->m_Vulkan->gpu;
    const size_t hostAlignment = std::max<size_t>({
        gpu->limits.align_host_ptr,
        alignof(void*),
        static_cast<size_t>(64),
    });
    void* base = SDL_aligned_alloc(hostAlignment, size);
    if (base == nullptr) {
        return nullptr;
    }

    auto allocation = new (std::nothrow) ImportedHostAllocation;
    if (allocation == nullptr) {
        SDL_aligned_free(base);
        return nullptr;
    }

    allocation->gpu = gpu;
    allocation->base = base;
    pl_buf_params bufferParams = {};
    bufferParams.size = size;
    bufferParams.import_handle = PL_HANDLE_HOST_PTR;
    bufferParams.shared_mem.handle.ptr = base;
    bufferParams.shared_mem.size = size;
    allocation->buffer = pl_buf_create(gpu, &bufferParams);
    if (allocation->buffer == nullptr) {
        SDL_aligned_free(base);
        delete allocation;
        return nullptr;
    }

    AVBufferRef* buffer = av_buffer_create(static_cast<uint8_t*>(base), size,
                                           freeImportedHostAllocation, allocation, 0);
    if (buffer == nullptr) {
        pl_buf_destroy(gpu, &allocation->buffer);
        SDL_aligned_free(base);
        delete allocation;
    }
    return buffer;
}

AVBufferRef* PlVkRenderer::getImportedHostBufferRef(size_t size)
{
    std::lock_guard<std::mutex> lock(m_ImportedHostPoolMutex);
    AVBufferPool*& pool = m_ImportedHostPools[size];
    if (pool == nullptr) {
        pool = av_buffer_pool_init2(size, this, allocateImportedHostBuffer, nullptr);
        if (pool == nullptr) {
            m_ImportedHostPools.erase(size);
            return nullptr;
        }
    }
    return av_buffer_pool_get(pool);
}

int PlVkRenderer::getImportedHostBuffer(AVCodecContext *context, AVFrame *frame, int flags)
{
    auto decoder = static_cast<FFmpegVideoDecoder*>(context->opaque);
    auto renderer = static_cast<PlVkRenderer*>(decoder->getBackendRenderer());
    pl_gpu gpu = renderer->m_Vulkan->gpu;
    const AVPixelFormat pixelFormat = static_cast<AVPixelFormat>(frame->format);

    if (!(context->codec->capabilities & AV_CODEC_CAP_DR1)) {
        return avcodec_default_get_buffer2(context, frame, flags);
    }

    pl_plane_data planeData[4] = {};
    const int planeCount = pl_plane_data_from_pixfmt(planeData, nullptr, pixelFormat);
    if (planeCount <= 0) {
        return avcodec_default_get_buffer2(context, frame, flags);
    }

    int alignment[AV_NUM_DATA_POINTERS] = {};
    int width = frame->width;
    int height = frame->height;
    size_t planeSizes[4] = {};

    std::memset(frame->data, 0, sizeof(frame->data));
    std::memset(frame->linesize, 0, sizeof(frame->linesize));
    std::memset(frame->buf, 0, sizeof(frame->buf));
    frame->extended_data = frame->data;
    frame->extended_buf = nullptr;

    avcodec_align_dimensions2(context, &width, &height, alignment);
    int result = av_image_fill_linesizes(frame->linesize, pixelFormat, width);
    if (result < 0) {
        return result;
    }

    for (int plane = 0; plane < planeCount; plane++) {
        alignment[plane] = std::lcm(alignment[plane],
                                    static_cast<int>(gpu->limits.align_tex_xfer_pitch));
        alignment[plane] = std::lcm(alignment[plane],
                                    static_cast<int>(gpu->limits.align_tex_xfer_offset));
        alignment[plane] = std::lcm(alignment[plane],
                                    static_cast<int>(planeData[plane].pixel_stride));
        frame->linesize[plane] = static_cast<int>(alignUp(frame->linesize[plane],
                                                          alignment[plane]));
    }

    const ptrdiff_t lineSizes[4] = {
        frame->linesize[0], frame->linesize[1], frame->linesize[2], frame->linesize[3],
    };
    result = av_image_fill_plane_sizes(planeSizes, pixelFormat, height, lineSizes);
    if (result < 0) {
        return result;
    }

    const size_t hostAlignment = std::max<size_t>({
        gpu->limits.align_host_ptr,
        alignof(void*),
        static_cast<size_t>(64),
    });

    for (int plane = 0; plane < planeCount; plane++) {
        const size_t allocationSize = alignUp(planeSizes[plane], hostAlignment);
        frame->buf[plane] = renderer->getImportedHostBufferRef(allocationSize);
        if (frame->buf[plane] == nullptr) {
            av_frame_unref(frame);
            return AVERROR(ENOMEM);
        }
        frame->data[plane] = frame->buf[plane]->data;
    }

    return 0;
}

PlVkRenderer::PlVkRenderer(bool hwaccel, IFFmpegRenderer *backendRenderer) :
    m_Backend(backendRenderer),
    m_HwAccelBackend(hwaccel)
{
    bool ok;

    pl_log_params logParams = pl_log_default_params;
    logParams.log_cb = pl_log_cb;
    logParams.log_level = (pl_log_level)qEnvironmentVariableIntValue("PLVK_LOG_LEVEL", &ok);
    if (!ok) {
#ifdef QT_DEBUG
        logParams.log_level = PL_LOG_DEBUG;
#else
        logParams.log_level = PL_LOG_WARN;
#endif
    }

    m_Log = pl_log_create(PL_API_VER, &logParams);
}

PlVkRenderer::~PlVkRenderer()
{
    // The render context must have been cleaned up by now
    SDL_assert(!m_HasPendingSwapchainFrame);

    {
        std::lock_guard<std::mutex> lock(m_ImportedHostPoolMutex);
        for (auto& entry : m_ImportedHostPools) {
            av_buffer_pool_uninit(&entry.second);
        }
        m_ImportedHostPools.clear();
    }

    if (m_Vulkan != nullptr) {
        for (int i = 0; i < (int)SDL_arraysize(m_Overlays); i++) {
            pl_tex_destroy(m_Vulkan->gpu, &m_Overlays[i].overlay.tex);
            pl_tex_destroy(m_Vulkan->gpu, &m_Overlays[i].stagingOverlay.tex);
        }

        for (int i = 0; i < (int)SDL_arraysize(m_Textures); i++) {
            pl_tex_destroy(m_Vulkan->gpu, &m_Textures[i]);
        }
    }

    pl_renderer_destroy(&m_Renderer);
    pl_swapchain_destroy(&m_Swapchain);
    pl_vulkan_destroy(&m_Vulkan);

    // This surface was created by SDL, so there's no libplacebo API to destroy it
    if (fn_vkDestroySurfaceKHR && m_VkSurface) {
        fn_vkDestroySurfaceKHR(m_PlVkInstance->instance, m_VkSurface, nullptr);
    }

    if (m_HwDeviceCtx != nullptr) {
        av_buffer_unref(&m_HwDeviceCtx);
    }

    pl_vk_inst_destroy(&m_PlVkInstance);

    // m_Log must always be the last object destroyed
    pl_log_destroy(&m_Log);
}

bool PlVkRenderer::chooseVulkanDevice(PDECODER_PARAMETERS params, bool hdrOutputRequired)
{
    uint32_t physicalDeviceCount = 0;
    fn_vkEnumeratePhysicalDevices(m_PlVkInstance->instance, &physicalDeviceCount, nullptr);
    std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
    fn_vkEnumeratePhysicalDevices(m_PlVkInstance->instance, &physicalDeviceCount, physicalDevices.data());

    std::set<uint32_t> devicesTried;
    VkPhysicalDeviceProperties deviceProps;

    if (physicalDeviceCount == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "No Vulkan devices found!");
        return false;
    }

    // First, try the first device in the list to support device selection layers
    // that put the user's preferred GPU in the first slot.
    fn_vkGetPhysicalDeviceProperties(physicalDevices[0], &deviceProps);
    if (tryInitializeDevice(physicalDevices[0], &deviceProps, params, hdrOutputRequired)) {
        return true;
    }
    devicesTried.emplace(0);

    // Next, we'll try to match an integrated GPU, since we want to minimize
    // power consumption and inter-GPU copies.
    for (uint32_t i = 0; i < physicalDeviceCount; i++) {
        // Skip devices we've already tried
        if (devicesTried.find(i) != devicesTried.end()) {
            continue;
        }

        VkPhysicalDeviceProperties deviceProps;
        fn_vkGetPhysicalDeviceProperties(physicalDevices[i], &deviceProps);
        if (deviceProps.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            if (tryInitializeDevice(physicalDevices[i], &deviceProps, params, hdrOutputRequired)) {
                return true;
            }
            devicesTried.emplace(i);
        }
    }

    // Next, we'll try to match a discrete GPU.
    for (uint32_t i = 0; i < physicalDeviceCount; i++) {
        // Skip devices we've already tried
        if (devicesTried.find(i) != devicesTried.end()) {
            continue;
        }

        VkPhysicalDeviceProperties deviceProps;
        fn_vkGetPhysicalDeviceProperties(physicalDevices[i], &deviceProps);
        if (deviceProps.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            if (tryInitializeDevice(physicalDevices[i], &deviceProps, params, hdrOutputRequired)) {
                return true;
            }
            devicesTried.emplace(i);
        }
    }

    // Finally, we'll try matching any non-software device.
    for (uint32_t i = 0; i < physicalDeviceCount; i++) {
        // Skip devices we've already tried
        if (devicesTried.find(i) != devicesTried.end()) {
            continue;
        }

        VkPhysicalDeviceProperties deviceProps;
        fn_vkGetPhysicalDeviceProperties(physicalDevices[i], &deviceProps);
        if (tryInitializeDevice(physicalDevices[i], &deviceProps, params, hdrOutputRequired)) {
            return true;
        }
        devicesTried.emplace(i);
    }

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "No suitable %sVulkan devices found!",
                 hdrOutputRequired ? "HDR-capable " : "");
    return false;
}

bool PlVkRenderer::tryInitializeDevice(VkPhysicalDevice device, VkPhysicalDeviceProperties* deviceProps,
                                       PDECODER_PARAMETERS decoderParams, bool hdrOutputRequired)
{
    // Check the Vulkan API version first to ensure it meets libplacebo's minimum
    if (deviceProps->apiVersion < PL_VK_MIN_VERSION) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Vulkan device '%s' does not meet minimum Vulkan version",
                    deviceProps->deviceName);
        return false;
    }

#ifdef Q_OS_WIN32
    // Intel's Windows drivers seem to have interoperability issues as of FFmpeg 7.0.1
    // when using Vulkan Video decoding. Since they also expose HEVC REXT profiles using
    // D3D11VA, let's reject them here so we can select a different Vulkan device or
    // just allow D3D11VA to take over.
    if (m_HwAccelBackend && deviceProps->vendorID == 0x8086 && !qEnvironmentVariableIntValue("PLVK_ALLOW_INTEL")) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Skipping Intel GPU for Vulkan Video due to broken drivers");
        return false;
    }
#endif

    // If we're acting as the decoder backend, we need a physical device with Vulkan video support
    if (m_HwAccelBackend) {
        const char* videoDecodeExtension;

        if (decoderParams->videoFormat & VIDEO_FORMAT_MASK_H264) {
            videoDecodeExtension = VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME;
        }
        else if (decoderParams->videoFormat & VIDEO_FORMAT_MASK_H265) {
            videoDecodeExtension = VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME;
        }
        else if (decoderParams->videoFormat & VIDEO_FORMAT_MASK_AV1) {
            // FFmpeg 6.1 implemented an early Mesa extension for Vulkan AV1 decoding.
            // FFmpeg 7.0 replaced that implementation with one based on the official extension.
#if LIBAVCODEC_VERSION_MAJOR >= 61
            videoDecodeExtension = VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME;
#else
            videoDecodeExtension = "VK_MESA_video_decode_av1";
#endif
        }
        else {
            SDL_assert(false);
            return false;
        }

        if (!isExtensionSupportedByPhysicalDevice(device, videoDecodeExtension)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Vulkan device '%s' does not support %s",
                        deviceProps->deviceName,
                        videoDecodeExtension);
            return false;
        }
    }

    if (!isSurfacePresentationSupportedByPhysicalDevice(device)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Vulkan device '%s' does not support presenting on window surface",
                    deviceProps->deviceName);
        return false;
    }

    if (hdrOutputRequired && !isColorSpaceSupportedByPhysicalDevice(device, VK_COLOR_SPACE_HDR10_ST2084_EXT)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Vulkan device '%s' does not support HDR10 (ST.2084 PQ)",
                    deviceProps->deviceName);
        return false;
    }

    // Avoid software GPUs
    if (deviceProps->deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && qgetenv("PLVK_ALLOW_SOFTWARE") != "1") {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Vulkan device '%s' is a (probably slow) software renderer. Set PLVK_ALLOW_SOFTWARE=1 to allow using this device.",
                    deviceProps->deviceName);
        return false;
    }

    pl_vulkan_params vkParams = pl_vulkan_default_params;
    vkParams.instance = m_PlVkInstance->instance;
    vkParams.get_proc_addr = m_PlVkInstance->get_proc_addr;
    vkParams.surface = m_VkSurface;
    vkParams.device = device;
    vkParams.opt_extensions = k_OptionalDeviceExtensions;
    vkParams.num_opt_extensions = SDL_arraysize(k_OptionalDeviceExtensions);
    vkParams.extra_queues = VK_QUEUE_VIDEO_DECODE_BIT_KHR;
    m_Vulkan = pl_vulkan_create(m_Log, &vkParams);
    if (m_Vulkan == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "pl_vulkan_create() failed for '%s'",
                     deviceProps->deviceName);
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Vulkan rendering device chosen: %s",
                deviceProps->deviceName);
    return true;
}

bool PlVkRenderer::isExtensionSupportedByPhysicalDevice(VkPhysicalDevice device, const char *extensionName)
{
    uint32_t extensionCount = 0;
    fn_vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    fn_vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());

    for (const VkExtensionProperties& extension : extensions) {
        if (strcmp(extension.extensionName, extensionName) == 0) {
            return true;
        }
    }

    return false;
}

#define POPULATE_FUNCTION(name) \
    fn_##name = (PFN_##name)m_PlVkInstance->get_proc_addr(m_PlVkInstance->instance, #name); \
    if (fn_##name == nullptr) { \
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, \
                     "Missing required Vulkan function: " #name); \
        return false; \
    }

bool PlVkRenderer::initialize(PDECODER_PARAMETERS params)
{
    m_Window = params->window;

    Uint32 instanceExtensionCount = 0;
    const char* const* instanceExtensions = SDL_Vulkan_GetInstanceExtensions(&instanceExtensionCount);
    if (instanceExtensions == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_Vulkan_GetInstanceExtensions() failed: %s",
                     SDL_GetError());
        return false;
    }

    pl_vk_inst_params vkInstParams = pl_vk_inst_default_params;
    {
        bool ok;
        vkInstParams.debug_extra = !!qEnvironmentVariableIntValue("PLVK_DEBUG_EXTRA", &ok);
        vkInstParams.debug = vkInstParams.debug_extra || !!qEnvironmentVariableIntValue("PLVK_DEBUG", &ok);
#ifdef QT_DEBUG
        if (!ok) {
            vkInstParams.debug = true;
        }
#endif
    }
    vkInstParams.get_proc_addr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    vkInstParams.extensions = instanceExtensions;
    vkInstParams.num_extensions = static_cast<int>(instanceExtensionCount);
    m_PlVkInstance = pl_vk_inst_create(m_Log, &vkInstParams);
    if (m_PlVkInstance == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "pl_vk_inst_create() failed");
        return false;
    }

    // Lookup all Vulkan functions we require
    POPULATE_FUNCTION(vkDestroySurfaceKHR);
    POPULATE_FUNCTION(vkGetPhysicalDeviceQueueFamilyProperties);
    POPULATE_FUNCTION(vkGetPhysicalDeviceSurfacePresentModesKHR);
    POPULATE_FUNCTION(vkGetPhysicalDeviceSurfaceFormatsKHR);
    POPULATE_FUNCTION(vkEnumeratePhysicalDevices);
    POPULATE_FUNCTION(vkGetPhysicalDeviceProperties);
    POPULATE_FUNCTION(vkGetPhysicalDeviceSurfaceSupportKHR);
    POPULATE_FUNCTION(vkEnumerateDeviceExtensionProperties);

    if (!SDL_Vulkan_CreateSurface(params->window, m_PlVkInstance->instance, nullptr, &m_VkSurface)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_Vulkan_CreateSurface() failed: %s",
                     SDL_GetError());
        return false;
    }

    // Enumerate physical devices and choose one that is suitable for our needs.
    //
    // For HDR streaming, we try to find an HDR-capable Vulkan device first then
    // try another search without the HDR requirement if the first attempt fails.
    if (!chooseVulkanDevice(params, params->videoFormat & VIDEO_FORMAT_MASK_10BIT) &&
        (!(params->videoFormat & VIDEO_FORMAT_MASK_10BIT) || !chooseVulkanDevice(params, false))) {
        return false;
    }

    VkPresentModeKHR presentMode;
    if (params->enableVsync) {
        // FIFO mode improves frame pacing compared with Mailbox, especially for
        // platforms like X11 that lack a VSyncSource implementation for Pacer.
        presentMode = VK_PRESENT_MODE_FIFO_KHR;
    }
    else {
        // We want immediate mode for V-Sync disabled if possible
        if (isPresentModeSupportedByPhysicalDevice(m_Vulkan->phys_device, VK_PRESENT_MODE_IMMEDIATE_KHR)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Using Immediate present mode with V-Sync disabled");
            presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Immediate present mode is not supported by the Vulkan driver. Latency may be higher than normal with V-Sync disabled.");

            // FIFO Relaxed can tear if the frame is running late
            if (isPresentModeSupportedByPhysicalDevice(m_Vulkan->phys_device, VK_PRESENT_MODE_FIFO_RELAXED_KHR)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Using FIFO Relaxed present mode with V-Sync disabled");
                presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
            }
            // Mailbox at least provides non-blocking behavior
            else if (isPresentModeSupportedByPhysicalDevice(m_Vulkan->phys_device, VK_PRESENT_MODE_MAILBOX_KHR)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Using Mailbox present mode with V-Sync disabled");
                presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
            }
            // FIFO is always supported
            else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Using FIFO present mode with V-Sync disabled");
                presentMode = VK_PRESENT_MODE_FIFO_KHR;
            }
        }
    }

    pl_vulkan_swapchain_params vkSwapchainParams = {};
    vkSwapchainParams.surface = m_VkSurface;
    vkSwapchainParams.present_mode = presentMode;
    vkSwapchainParams.swapchain_depth = 1; // No queued frames
#if PL_API_VER >= 338
    vkSwapchainParams.disable_10bit_sdr = true; // Some drivers don't dither 10-bit SDR output correctly
#endif
    m_Swapchain = pl_vulkan_create_swapchain(m_Vulkan, &vkSwapchainParams);
    if (m_Swapchain == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "pl_vulkan_create_swapchain() failed");
        return false;
    }

    m_Renderer = pl_renderer_create(m_Log, m_Vulkan->gpu);
    if (m_Renderer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "pl_renderer_create() failed");
        return false;
    }

    // We only need an hwaccel device context if we're going to act as the backend renderer too
    if (m_HwAccelBackend) {
        m_HwDeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
        if (m_HwDeviceCtx == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN) failed");
            return false;
        }

        auto hwDeviceContext = ((AVHWDeviceContext *)m_HwDeviceCtx->data);
        hwDeviceContext->user_opaque = this; // Used by lockQueue()/unlockQueue()

        auto vkDeviceContext = (AVVulkanDeviceContext*)((AVHWDeviceContext *)m_HwDeviceCtx->data)->hwctx;
        vkDeviceContext->get_proc_addr = m_PlVkInstance->get_proc_addr;
        vkDeviceContext->inst = m_PlVkInstance->instance;
        vkDeviceContext->phys_dev = m_Vulkan->phys_device;
        vkDeviceContext->act_dev = m_Vulkan->device;
        vkDeviceContext->device_features = *m_Vulkan->features;
        vkDeviceContext->enabled_inst_extensions = m_PlVkInstance->extensions;
        vkDeviceContext->nb_enabled_inst_extensions = m_PlVkInstance->num_extensions;
        vkDeviceContext->enabled_dev_extensions = m_Vulkan->extensions;
        vkDeviceContext->nb_enabled_dev_extensions = m_Vulkan->num_extensions;
#if LIBAVUTIL_VERSION_MAJOR >= 61
        auto addQueueFamily = [vkDeviceContext](int index, int count, VkQueueFlagBits flags) {
            for (int i = 0; i < vkDeviceContext->nb_qf; i++) {
                if (vkDeviceContext->qf[i].idx == index) {
                    vkDeviceContext->qf[i].flags = static_cast<VkQueueFlagBits>(
                            vkDeviceContext->qf[i].flags | flags);
                    return;
                }
            }

            SDL_assert(vkDeviceContext->nb_qf < static_cast<int>(SDL_arraysize(vkDeviceContext->qf)));
            auto& queueFamily = vkDeviceContext->qf[vkDeviceContext->nb_qf++];
            queueFamily.idx = index;
            queueFamily.num = count;
            queueFamily.flags = flags;
            queueFamily.video_caps = VK_VIDEO_CODEC_OPERATION_NONE_KHR;
        };

        addQueueFamily(m_Vulkan->queue_graphics.index, m_Vulkan->queue_graphics.count,
                       VK_QUEUE_GRAPHICS_BIT);
        addQueueFamily(m_Vulkan->queue_transfer.index, m_Vulkan->queue_transfer.count,
                       VK_QUEUE_TRANSFER_BIT);
        addQueueFamily(m_Vulkan->queue_compute.index, m_Vulkan->queue_compute.count,
                       VK_QUEUE_COMPUTE_BIT);
#else
        vkDeviceContext->queue_family_index = m_Vulkan->queue_graphics.index;
        vkDeviceContext->nb_graphics_queues = m_Vulkan->queue_graphics.count;
        vkDeviceContext->queue_family_tx_index = m_Vulkan->queue_transfer.index;
        vkDeviceContext->nb_tx_queues = m_Vulkan->queue_transfer.count;
        vkDeviceContext->queue_family_comp_index = m_Vulkan->queue_compute.index;
        vkDeviceContext->nb_comp_queues = m_Vulkan->queue_compute.count;
#endif
#if LIBAVUTIL_VERSION_INT > AV_VERSION_INT(58, 9, 100)
        vkDeviceContext->lock_queue = lockQueue;
        vkDeviceContext->unlock_queue = unlockQueue;
#endif

#if LIBAVUTIL_VERSION_MAJOR >= 61
        uint32_t decodeQueueIndex;
        uint32_t decodeQueueCount;
        if (!getQueue(VK_QUEUE_VIDEO_DECODE_BIT_KHR, &decodeQueueIndex, &decodeQueueCount)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Vulkan video decoding is not supported by the Vulkan device");
            return false;
        }
        addQueueFamily(decodeQueueIndex, decodeQueueCount, VK_QUEUE_VIDEO_DECODE_BIT_KHR);
#else
        static_assert(sizeof(vkDeviceContext->queue_family_decode_index) == sizeof(uint32_t), "sizeof(int) != sizeof(uint32_t)");
        static_assert(sizeof(vkDeviceContext->nb_decode_queues) == sizeof(uint32_t), "sizeof(int) != sizeof(uint32_t)");
        if (!getQueue(VK_QUEUE_VIDEO_DECODE_BIT_KHR, (uint32_t*)&vkDeviceContext->queue_family_decode_index, (uint32_t*)&vkDeviceContext->nb_decode_queues)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Vulkan video decoding is not supported by the Vulkan device");
            return false;
        }
#endif

        int err = av_hwdevice_ctx_init(m_HwDeviceCtx);
        if (err < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "av_hwdevice_ctx_init() failed: %d",
                         err);
            return false;
        }
    }

    return true;
}

bool PlVkRenderer::prepareDecoderContext(AVCodecContext *context, AVDictionary **)
{
    if (m_HwAccelBackend) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Using Vulkan video decoding");

        context->hw_device_ctx = av_buffer_ref(m_HwDeviceCtx);
    }
    else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Using Vulkan renderer");

        const pl_gpu_limits& limits = m_Vulkan->gpu->limits;
        const bool canUseMappedVulkan = limits.thread_safe &&
            limits.max_mapped_size && limits.host_cached;
        const bool canImportHostMemory = limits.thread_safe && limits.buf_transfer &&
            limits.align_host_ptr &&
            (m_Vulkan->gpu->import_caps.buf & PL_HANDLE_HOST_PTR);

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Vulkan software-frame capabilities: mapped=%s host-import=%s "
                    "host-import-slow=%s host-alignment=%zu",
                    canUseMappedVulkan ? "yes" : "no",
                    canImportHostMemory ? "yes" : "no",
                    limits.host_ptr_slow ? "yes" : "no",
                    limits.align_host_ptr);

        const QByteArray requestedAllocator =
            qgetenv("STATIONCONNECT_VULKAN_FRAME_ALLOCATOR").trimmed().toLower();
        if (requestedAllocator.isEmpty()) {
            if (canImportHostMemory && !limits.host_ptr_slow) {
                m_SoftwareFrameAllocator = SoftwareFrameAllocator::ImportedHost;
                context->get_buffer2 = getImportedHostBuffer;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Using pooled cacheable FFmpeg decode buffers imported into Vulkan");
            }
            else if (canUseMappedVulkan) {
                m_SoftwareFrameAllocator = SoftwareFrameAllocator::MappedVulkan;
                context->get_buffer2 = getMappedBuffer;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Using persistently mapped Vulkan decode buffers");
            }
            else {
                m_SoftwareFrameAllocator = SoftwareFrameAllocator::System;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Using system-memory FFmpeg decode buffers");
            }
        }
        else if (requestedAllocator == "system") {
            m_SoftwareFrameAllocator = SoftwareFrameAllocator::System;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Using system-memory FFmpeg decode buffers");
        }
        else if (requestedAllocator == "mapped") {
            if (!canUseMappedVulkan) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Mapped Vulkan decode buffers were requested but are unavailable; "
                            "using system-memory frames");
                m_SoftwareFrameAllocator = SoftwareFrameAllocator::System;
            }
            else {
                m_SoftwareFrameAllocator = SoftwareFrameAllocator::MappedVulkan;
                context->get_buffer2 = getMappedBuffer;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Using persistently mapped Vulkan decode buffers");
            }
        }
        else if (requestedAllocator == "host-import") {
            if (!canImportHostMemory) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Imported host decode buffers were requested but are unavailable; "
                            "using system-memory frames");
                m_SoftwareFrameAllocator = SoftwareFrameAllocator::System;
            }
            else {
                m_SoftwareFrameAllocator = SoftwareFrameAllocator::ImportedHost;
                context->get_buffer2 = getImportedHostBuffer;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Using pooled cacheable FFmpeg decode buffers imported into Vulkan%s",
                            limits.host_ptr_slow ? " (driver reports slow host-pointer import)" : "");
            }
        }
        else {
            m_SoftwareFrameAllocator = SoftwareFrameAllocator::System;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unknown STATIONCONNECT_VULKAN_FRAME_ALLOCATOR value '%s'; "
                        "using system-memory frames",
                        requestedAllocator.constData());
        }

    }

    return true;
}

bool PlVkRenderer::mapImportedHostFrameToPlacebo(const AVFrame *frame,
                                                  pl_frame* mappedFrame)
{
    const AVPixelFormat pixelFormat = static_cast<AVPixelFormat>(frame->format);
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(pixelFormat);
    pl_plane_data planeData[4] = {};

    pl_frame_from_avframe(mappedFrame, frame);
    mappedFrame->user_data = nullptr;

    const int planeCount = pl_plane_data_from_pixfmt(planeData,
                                                      &mappedFrame->repr.bits,
                                                      pixelFormat);
    if (planeCount <= 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "pl_plane_data_from_pixfmt() failed for imported host frame");
        return false;
    }

    for (int plane = 0; plane < planeCount; plane++) {
        ImportedHostAllocation* allocation = getImportedHostAllocation(frame, plane);
        if (allocation == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Imported host frame is missing plane %d allocation metadata",
                         plane);
            return false;
        }

        const bool isChroma = plane == 1 || plane == 2;
        planeData[plane].width = AV_CEIL_RSHIFT(
            frame->width, isChroma ? descriptor->log2_chroma_w : 0);
        planeData[plane].height = AV_CEIL_RSHIFT(
            frame->height, isChroma ? descriptor->log2_chroma_h : 0);
        planeData[plane].row_stride = static_cast<size_t>(std::abs(frame->linesize[plane]));
        planeData[plane].buf = allocation->buffer;
        planeData[plane].buf_offset = static_cast<size_t>(
            frame->data[plane] - static_cast<uint8_t*>(allocation->base));

        if (frame->linesize[plane] < 0) {
            planeData[plane].buf_offset += planeData[plane].row_stride *
                (planeData[plane].height - 1);
            mappedFrame->planes[plane].flipped = true;
        }

        if (!pl_upload_plane(m_Vulkan->gpu, &mappedFrame->planes[plane],
                             &m_Textures[plane], &planeData[plane])) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "pl_upload_plane() failed for imported host plane %d",
                         plane);
            return false;
        }
        mappedFrame->planes[plane].texture = m_Textures[plane];
    }

    return true;
}

bool PlVkRenderer::mapAvFrameToPlacebo(const AVFrame *frame, pl_frame* mappedFrame,
                                       bool* importedHostFrame)
{
    *importedHostFrame = m_SoftwareFrameAllocator == SoftwareFrameAllocator::ImportedHost;
    if (*importedHostFrame) {
        if (!mapImportedHostFrameToPlacebo(frame, mappedFrame)) {
            return false;
        }
    }
    else {
        pl_avframe_params mapParams = {};
        mapParams.frame = frame;
        mapParams.tex = m_Textures;
        if (!pl_map_avframe_ex(m_Vulkan->gpu, mappedFrame, &mapParams)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "pl_map_avframe_ex() failed");
            return false;
        }
    }

    // libplacebo assumes a minimum luminance value of 0 means the actual value was unknown.
    // Since we assume the host values are correct, we use the PL_COLOR_HDR_BLACK constant to
    // indicate infinite contrast.
    //
    // NB: We also have to check that the AVFrame actually had metadata in the first place,
    // because libplacebo may infer metadata if the frame didn't have any.
    if (av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA) && !mappedFrame->color.hdr.min_luma) {
        mappedFrame->color.hdr.min_luma = PL_COLOR_HDR_BLACK;
    }

    // HACK: AMF AV1 encoding on the host PC does not set full color range properly in the
    // bitstream data, so libplacebo incorrectly renders the content as limited range.
    //
    // As a workaround, set full range manually in the mapped frame ourselves.
    mappedFrame->repr.levels = PL_COLOR_LEVELS_FULL;

    return true;
}

void PlVkRenderer::unmapAvFrameFromPlacebo(pl_frame* mappedFrame,
                                            bool importedHostFrame)
{
    if (!importedHostFrame) {
        pl_unmap_avframe(m_Vulkan->gpu, mappedFrame);
    }
}

bool PlVkRenderer::getQueue(VkQueueFlags requiredFlags, uint32_t *queueIndex, uint32_t *queueCount)
{
    uint32_t queueFamilyCount = 0;
    fn_vkGetPhysicalDeviceQueueFamilyProperties(m_Vulkan->phys_device, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    fn_vkGetPhysicalDeviceQueueFamilyProperties(m_Vulkan->phys_device, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if ((queueFamilies[i].queueFlags & requiredFlags) == requiredFlags) {
            *queueIndex = i;
            *queueCount = queueFamilies[i].queueCount;
            return true;
        }
    }

    return false;
}

bool PlVkRenderer::isPresentModeSupportedByPhysicalDevice(VkPhysicalDevice device, VkPresentModeKHR presentMode)
{
    uint32_t presentModeCount = 0;
    fn_vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_VkSurface, &presentModeCount, nullptr);

    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    fn_vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_VkSurface, &presentModeCount, presentModes.data());

    for (uint32_t i = 0; i < presentModeCount; i++) {
        if (presentModes[i] == presentMode) {
            return true;
        }
    }

    return false;
}

bool PlVkRenderer::isColorSpaceSupportedByPhysicalDevice(VkPhysicalDevice device, VkColorSpaceKHR colorSpace)
{
    uint32_t formatCount = 0;
    fn_vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_VkSurface, &formatCount, nullptr);

    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    fn_vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_VkSurface, &formatCount, formats.data());

    for (uint32_t i = 0; i < formatCount; i++) {
        if (formats[i].colorSpace == colorSpace) {
            return true;
        }
    }

    return false;
}

bool PlVkRenderer::isSurfacePresentationSupportedByPhysicalDevice(VkPhysicalDevice device)
{
    uint32_t queueFamilyCount = 0;
    fn_vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        VkBool32 supported = VK_FALSE;
        if (fn_vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_VkSurface, &supported) == VK_SUCCESS && supported == VK_TRUE) {
            return true;
        }
    }

    return false;
}

void PlVkRenderer::waitToRender()
{
    // Check if the GPU has failed before doing anything else
    if (pl_gpu_is_failed(m_Vulkan->gpu)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "GPU is in failed state. Recreating renderer.");
        SDL_Event event;
        event.type = SDL_EVENT_RENDER_DEVICE_RESET;
        SDL_PushEvent(&event);
        return;
    }

#ifndef Q_OS_WIN32
    // With libplacebo's Vulkan backend, all swap_buffers does is wait for queued
    // presents to finish. This happens to be exactly what we want to do here, since
    // it lets us wait to select a queued frame for rendering until we know that we
    // can present without blocking in renderFrame().
    //
    // NB: This seems to cause performance problems with the Windows display stack
    // (particularly on Nvidia) so we will only do this for non-Windows platforms.
    pl_swapchain_swap_buffers(m_Swapchain);
#endif

    // Handle the swapchain being resized
    int vkDrawableW, vkDrawableH;
    SDL_GetWindowSizeInPixels(m_Window, &vkDrawableW, &vkDrawableH);
    if (!pl_swapchain_resize(m_Swapchain, &vkDrawableW, &vkDrawableH)) {
        // Swapchain (re)creation can fail if the window is occluded
        return;
    }

    // Get the next swapchain buffer for rendering. If this fails, renderFrame()
    // will try again.
    //
    // NB: After calling this successfully, we *MUST* call pl_swapchain_submit_frame(),
    // hence the implementation of cleanupRenderContext() which does just this in case
    // renderFrame() wasn't called after waitToRender().
    if (pl_swapchain_start_frame(m_Swapchain, &m_SwapchainFrame)) {
        m_HasPendingSwapchainFrame = true;
    }
}

void PlVkRenderer::cleanupRenderContext()
{
    // We have to submit a pending swapchain frame before shutting down
    // in order to release a mutex that pl_swapchain_start_frame() acquires.
    if (m_HasPendingSwapchainFrame) {
        pl_swapchain_submit_frame(m_Swapchain);
        m_HasPendingSwapchainFrame = false;
    }
}

void PlVkRenderer::renderFrame(AVFrame *frame)
{
    pl_frame mappedFrame, targetFrame;
    bool importedHostFrame = false;

    // If waitToRender() failed to get the next swapchain frame, skip
    // rendering this frame. It probably means the window is occluded.
    if (!m_HasPendingSwapchainFrame) {
        return;
    }

    if (!mapAvFrameToPlacebo(frame, &mappedFrame, &importedHostFrame)) {
        // This function logs internally
        return;
    }

    // Adjust the swapchain if the colorspace of incoming frames has changed
    if (!pl_color_space_equal(&mappedFrame.color, &m_LastColorspace)) {
        m_LastColorspace = mappedFrame.color;
        SDL_assert(pl_color_space_equal(&mappedFrame.color, &m_LastColorspace));
        pl_swapchain_colorspace_hint(m_Swapchain, &mappedFrame.color);
    }

    // Reserve enough space to avoid allocating under the overlay lock
    pl_overlay_part overlayParts[Overlay::OverlayMax] = {};
    std::vector<pl_tex> texturesToDestroy;
    std::vector<pl_overlay> overlays;
    texturesToDestroy.reserve(Overlay::OverlayMax);
    overlays.reserve(Overlay::OverlayMax);

    pl_frame_from_swapchain(&targetFrame, &m_SwapchainFrame);

    // We perform minimal processing under the overlay lock to avoid blocking threads updating the overlay
    SDL_LockSpinlock(&m_OverlayLock);
    for (int i = 0; i < Overlay::OverlayMax; i++) {
        // If we have a staging overlay, we need to transfer ownership to us
        if (m_Overlays[i].hasStagingOverlay) {
            if (m_Overlays[i].hasOverlay) {
                texturesToDestroy.push_back(m_Overlays[i].overlay.tex);
            }

            // Copy the overlay fields from the staging area
            m_Overlays[i].overlay = m_Overlays[i].stagingOverlay;

            // We now own the staging overlay
            m_Overlays[i].hasStagingOverlay = false;
            SDL_zero(m_Overlays[i].stagingOverlay);
            m_Overlays[i].hasOverlay = true;
        }

        // If we have an overlay but it's been disabled, free the overlay texture
        if (m_Overlays[i].hasOverlay && !Session::get()->getOverlayManager().isOverlayEnabled((Overlay::OverlayType)i)) {
            texturesToDestroy.push_back(m_Overlays[i].overlay.tex);
            SDL_zero(m_Overlays[i].overlay);
            m_Overlays[i].hasOverlay = false;
        }

        // We have an overlay to draw
        if (m_Overlays[i].hasOverlay) {
            // Position the overlay
            overlayParts[i].src = { 0, 0, (float)m_Overlays[i].overlay.tex->params.w, (float)m_Overlays[i].overlay.tex->params.h };
            if (i == Overlay::OverlayStatusUpdate) {
                // Bottom Left
                overlayParts[i].dst.x0 = 0;
                overlayParts[i].dst.y0 = SDL_max(0, targetFrame.crop.y1 - overlayParts[i].src.y1);
            }
            else if (i == Overlay::OverlayDebug) {
                // Top left
                overlayParts[i].dst.x0 = 0;
                overlayParts[i].dst.y0 = 0;
            }
            else if (i == Overlay::OverlayToolbar) {
                // User-positionable along the top edge
                overlayParts[i].dst.x0 = targetFrame.crop.x0 +
                    Session::get()->getOverlayManager().getOverlayHorizontalPosition(
                        (Overlay::OverlayType)i) *
                    SDL_max(0.0f, targetFrame.crop.x1 - targetFrame.crop.x0 -
                                      overlayParts[i].src.x1);
                overlayParts[i].dst.y0 = 0;
            }
            overlayParts[i].dst.x1 = overlayParts[i].dst.x0 + overlayParts[i].src.x1;
            overlayParts[i].dst.y1 = overlayParts[i].dst.y0 + overlayParts[i].src.y1;

            m_Overlays[i].overlay.parts = &overlayParts[i];
            m_Overlays[i].overlay.num_parts = 1;

            overlays.push_back(m_Overlays[i].overlay);
        }
    }
    SDL_UnlockSpinlock(&m_OverlayLock);

    SDL_Rect src;
    src.x = mappedFrame.crop.x0;
    src.y = mappedFrame.crop.y0;
    src.w = mappedFrame.crop.x1 - mappedFrame.crop.x0;
    src.h = mappedFrame.crop.y1 - mappedFrame.crop.y0;

    SDL_Rect dst;
    dst.x = targetFrame.crop.x0;
    dst.y = targetFrame.crop.y0;
    dst.w = targetFrame.crop.x1 - targetFrame.crop.x0;
    dst.h = targetFrame.crop.y1 - targetFrame.crop.y0;

    // Scale the video to the surface size while preserving the aspect ratio
    StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

    targetFrame.crop.x0 = dst.x;
    targetFrame.crop.y0 = dst.y;
    targetFrame.crop.x1 = dst.x + dst.w;
    targetFrame.crop.y1 = dst.y + dst.h;

    // Render the video image and overlays into the swapchain buffer
    targetFrame.num_overlays = (int)overlays.size();
    targetFrame.overlays = overlays.data();
    if (!pl_render_image(m_Renderer, &mappedFrame, &targetFrame, &pl_render_fast_params)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "pl_render_image() failed");
        // NB: We must fallthrough to call pl_swapchain_submit_frame()
    }

    // Submit the frame for display and swap buffers
    m_HasPendingSwapchainFrame = false;
    if (!pl_swapchain_submit_frame(m_Swapchain)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "pl_swapchain_submit_frame() failed");

        // Recreate the renderer
        SDL_Event event;
        event.type = SDL_EVENT_RENDER_DEVICE_RESET;
        SDL_PushEvent(&event);
        goto UnmapExit;
    }

#ifdef Q_OS_WIN32
    // On Windows, we swap buffers here instead of waitToRender()
    // to avoid some performance problems on Nvidia GPUs.
    pl_swapchain_swap_buffers(m_Swapchain);
#endif

UnmapExit:
    // Delete any textures that need to be destroyed
    for (pl_tex& texture : texturesToDestroy) {
        pl_tex_destroy(m_Vulkan->gpu, &texture);
    }

    unmapAvFrameFromPlacebo(&mappedFrame, importedHostFrame);
}

bool PlVkRenderer::testRenderFrame(AVFrame *frame)
{
    // Test if the frame can be mapped to libplacebo
    pl_frame mappedFrame;
    bool importedHostFrame = false;
    if (!mapAvFrameToPlacebo(frame, &mappedFrame, &importedHostFrame)) {
        return false;
    }

    unmapAvFrameFromPlacebo(&mappedFrame, importedHostFrame);
    return true;
}

void PlVkRenderer::notifyOverlayUpdated(Overlay::OverlayType type)
{
    SDL_Surface* newSurface = Session::get()->getOverlayManager().getUpdatedOverlaySurface(type);
    if (newSurface == nullptr && Session::get()->getOverlayManager().isOverlayEnabled(type)) {
        // The overlay is enabled and there is no new surface. Leave the old texture alone.
        return;
    }

    SDL_LockSpinlock(&m_OverlayLock);
    // We want to clear the staging overlay flag even if a staging overlay is still present,
    // since this ensures the render thread will not read from a partially initialized pl_tex
    // as we modify or recreate the staging overlay texture outside the overlay lock.
    m_Overlays[type].hasStagingOverlay = false;
    SDL_UnlockSpinlock(&m_OverlayLock);

    // If there's no new staging overlay, free the old staging overlay texture.
    // NB: This is safe to do outside the overlay lock because we're guaranteed
    // to not have racing readers/writers if hasStagingOverlay is false.
    if (newSurface == nullptr) {
        pl_tex_destroy(m_Vulkan->gpu, &m_Overlays[type].stagingOverlay.tex);
        SDL_zero(m_Overlays[type].stagingOverlay);
        return;
    }

    // Find a compatible texture format
    SDL_assert(newSurface->format == SDL_PIXELFORMAT_ARGB8888);
    pl_fmt texFormat = pl_find_named_fmt(m_Vulkan->gpu, "bgra8");
    if (!texFormat) {
        SDL_DestroySurface(newSurface);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "pl_find_named_fmt(bgra8) failed");
        return;
    }

    // Create a new texture for this overlay if necessary, otherwise reuse the existing texture.
    // NB: We're guaranteed that the render thread won't be reading this concurrently because
    // we set hasStagingOverlay to false above.
    pl_tex_params texParams = {};
    texParams.w = newSurface->w;
    texParams.h = newSurface->h;
    texParams.format = texFormat;
    texParams.sampleable = true;
    texParams.host_writable = true;
    texParams.blit_src = !!(texFormat->caps & PL_FMT_CAP_BLITTABLE);
    texParams.debug_tag = PL_DEBUG_TAG;
    if (!pl_tex_recreate(m_Vulkan->gpu, &m_Overlays[type].stagingOverlay.tex, &texParams)) {
        pl_tex_destroy(m_Vulkan->gpu, &m_Overlays[type].stagingOverlay.tex);
        SDL_zero(m_Overlays[type].stagingOverlay);
        SDL_DestroySurface(newSurface);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "pl_tex_recreate() failed");
        return;
    }

    // Upload the surface data to the new texture
    SDL_assert(!SDL_MUSTLOCK(newSurface));
    pl_tex_transfer_params xferParams = {};
    xferParams.tex = m_Overlays[type].stagingOverlay.tex;
    xferParams.row_pitch = (size_t)newSurface->pitch;
    xferParams.ptr = newSurface->pixels;
    xferParams.callback = overlayUploadComplete;
    xferParams.priv = newSurface;
    if (!pl_tex_upload(m_Vulkan->gpu, &xferParams)) {
        pl_tex_destroy(m_Vulkan->gpu, &m_Overlays[type].stagingOverlay.tex);
        SDL_zero(m_Overlays[type].stagingOverlay);
        SDL_DestroySurface(newSurface);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "pl_tex_upload() failed");
        return;
    }

    // newSurface is now owned by the texture upload process. It will be freed in overlayUploadComplete()
    newSurface = nullptr;

    // Initialize the rest of the overlay params
    m_Overlays[type].stagingOverlay.mode = PL_OVERLAY_NORMAL;
    m_Overlays[type].stagingOverlay.coords = PL_OVERLAY_COORDS_DST_FRAME;
    m_Overlays[type].stagingOverlay.repr = pl_color_repr_rgb;
    m_Overlays[type].stagingOverlay.color = pl_color_space_srgb;

    // Make this staging overlay visible to the render thread
    SDL_LockSpinlock(&m_OverlayLock);
    SDL_assert(!m_Overlays[type].hasStagingOverlay);
    m_Overlays[type].hasStagingOverlay = true;
    SDL_UnlockSpinlock(&m_OverlayLock);
}

bool PlVkRenderer::notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info)
{
    // We can transparently handle size and display changes
    return !(info->stateChangeFlags & ~(WINDOW_STATE_CHANGE_SIZE | WINDOW_STATE_CHANGE_DISPLAY));
}

int PlVkRenderer::getRendererAttributes()
{
    // This renderer supports HDR (including tone mapping to SDR displays)
    return RENDERER_ATTRIBUTE_HDR_SUPPORT;
}

int PlVkRenderer::getDecoderColorspace()
{
    // We rely on libplacebo for color conversion, pick colorspace with the same primaries as sRGB
    return COLORSPACE_REC_709;
}

int PlVkRenderer::getDecoderColorRange()
{
    // Explicitly set the color range to full to fix raised black levels on OLED displays,
    // should also reduce banding artifacts in all situations
    return COLOR_RANGE_FULL;
}

int PlVkRenderer::getDecoderCapabilities()
{
    return CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC |
           CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1;
}

bool PlVkRenderer::needsTestFrame()
{
    // We need a test frame to verify that Vulkan video decoding is working
    return true;
}

bool PlVkRenderer::isPixelFormatSupported(int videoFormat, AVPixelFormat pixelFormat)
{
    if (m_HwAccelBackend) {
        return pixelFormat == AV_PIX_FMT_VULKAN;
    }
    else if (m_Backend) {
        return m_Backend->isPixelFormatSupported(videoFormat, pixelFormat);
    }
    else {
        if (pixelFormat == AV_PIX_FMT_VULKAN) {
            // Vulkan frames are always supported
            return true;
        }
        else if (videoFormat & VIDEO_FORMAT_MASK_YUV444) {
            if (videoFormat & VIDEO_FORMAT_MASK_10BIT) {
                switch (pixelFormat) {
                case AV_PIX_FMT_P410:
                case AV_PIX_FMT_YUV444P10:
                case AV_PIX_FMT_GBRP10:
                    return true;
                default:
                    return false;
                }
            }
            else {
                switch (pixelFormat) {
                case AV_PIX_FMT_NV24:
                case AV_PIX_FMT_NV42:
                case AV_PIX_FMT_YUV444P:
                case AV_PIX_FMT_YUVJ444P:
                case AV_PIX_FMT_GBRP:
                    return true;
                default:
                    return false;
                }
            }
        }
        else if (videoFormat & VIDEO_FORMAT_MASK_YUV422) {
            if (videoFormat & VIDEO_FORMAT_MASK_10BIT) {
                switch (pixelFormat) {
                case AV_PIX_FMT_P210:
                case AV_PIX_FMT_YUV422P10:
                    return true;
                default:
                    return false;
                }
            }
            else {
                switch (pixelFormat) {
                case AV_PIX_FMT_NV16:
                case AV_PIX_FMT_YUV422P:
                case AV_PIX_FMT_YUVJ422P:
                    return true;
                default:
                    return false;
                }
            }
        }
        else if (videoFormat & VIDEO_FORMAT_MASK_10BIT) {
            switch (pixelFormat) {
            case AV_PIX_FMT_P010:
            case AV_PIX_FMT_YUV420P10:
                return true;
            default:
                return false;
            }
        }
        else {
            switch (pixelFormat) {
            case AV_PIX_FMT_NV12:
            case AV_PIX_FMT_NV21:
            case AV_PIX_FMT_YUV420P:
            case AV_PIX_FMT_YUVJ420P:
                return true;
            default:
                return false;
            }
        }
    }
}

AVPixelFormat PlVkRenderer::getPreferredPixelFormat(int videoFormat)
{
    if (m_Backend) {
        return m_Backend->getPreferredPixelFormat(videoFormat);
    }
    else {
        return AV_PIX_FMT_VULKAN;
    }
}

IFFmpegRenderer::RendererType PlVkRenderer::getRendererType()
{
    return IFFmpegRenderer::RendererType::Vulkan;
}

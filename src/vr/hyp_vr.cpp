// OpenXR session for the VR builds: implements RT64::VRInterface (see lib/rt64/src/hle/rt64_vr.h).
//
// Threads: RT64's workload thread calls latestViews() once per display frame; its present thread calls
// beginPresent()/endPresent() for every frame it shows. xrWaitFrame in beginPresent paces the present thread.
//
// Coordinates: OpenXR LOCAL space (metres, -Z forward, +Y up) matches the N64 camera space's axes. The
// head's pose relative to where it was at start (yaw only) rides on top of the game's camera, scaled to
// game units (HYP_VR_SCALE units per metre).

#include "hyp_vr.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "vulkan/rt64_vulkan.h"     // RT64's Vulkan layer (volk + vulkan headers, VulkanExternalHooks, VulkanTexture)
#include "hle/rt64_vr.h"

#define XR_USE_GRAPHICS_API_VULKAN
#ifdef __ANDROID__
#define XR_USE_PLATFORM_ANDROID
#include <jni.h>
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace {
    // Small column-vector 3x3 helpers.
    struct M3 { float m[3][3]; };
    struct V3 { float x, y, z; };

    M3 quat_to_m3(const XrQuaternionf &q) {
        const float x = q.x, y = q.y, z = q.z, w = q.w;
        M3 r;
        r.m[0][0] = 1 - 2 * (y * y + z * z); r.m[0][1] = 2 * (x * y - z * w);     r.m[0][2] = 2 * (x * z + y * w);
        r.m[1][0] = 2 * (x * y + z * w);     r.m[1][1] = 1 - 2 * (x * x + z * z); r.m[1][2] = 2 * (y * z - x * w);
        r.m[2][0] = 2 * (x * z - y * w);     r.m[2][1] = 2 * (y * z + x * w);     r.m[2][2] = 1 - 2 * (x * x + y * y);
        return r;
    }

    M3 rot_y(float a) {
        M3 r = {};
        r.m[0][0] = std::cos(a);  r.m[0][2] = std::sin(a);
        r.m[1][1] = 1;
        r.m[2][0] = -std::sin(a); r.m[2][2] = std::cos(a);
        return r;
    }

    M3 mul(const M3 &a, const M3 &b) {
        M3 r = {};
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                for (int k = 0; k < 3; k++) r.m[i][j] += a.m[i][k] * b.m[k][j];
        return r;
    }

    V3 mul(const M3 &a, const V3 &v) {
        return { a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z,
                 a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
                 a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z };
    }

    void check(XrResult r, const char* what) {
        if (XR_FAILED(r)) std::printf("[vr] %s failed: %d\n", what, (int)r);
    }

    std::vector<std::string> split_spaces(const std::string& s) {
        std::vector<std::string> out;
        std::istringstream in(s);
        std::string w;
        while (in >> w) out.push_back(w);
        return out;
    }

    struct EyeSwapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        uint32_t width = 0, height = 0;
        std::vector<XrSwapchainImageVulkanKHR> images;
        std::vector<std::unique_ptr<RT64::VulkanTexture>> textures;
        std::vector<std::unique_ptr<RT64::RenderFramebuffer>> framebuffers;
        uint32_t acquired = UINT32_MAX;
    };

    struct ViewSet {                       // one located head pose: what it rendered with and what gets submitted
        uint64_t token = 0;
        XrView views[2];
        RT64::VRFrameViews rt64;
    };

    class OpenXRSession final : public RT64::VRInterface {
    public:
        XrInstance instance = XR_NULL_HANDLE;
        XrSystemId system = XR_NULL_SYSTEM_ID;
        XrSession session = XR_NULL_HANDLE;
        XrSpace localSpace = XR_NULL_HANDLE;
        XrSpace viewSpace = XR_NULL_HANDLE;
        XrSessionState state = XR_SESSION_STATE_UNKNOWN;
        std::atomic<bool> running{ false };
        std::atomic<bool> quit{ false };
        uint32_t recommendedWidth = 1440, recommendedHeight = 1584;
        float unitsPerMetre = 100.0f;

        // Vulkan, from RT64's device creation.
        VkInstance vkInstance = VK_NULL_HANDLE;
        VkPhysicalDevice vkPhysical = VK_NULL_HANDLE;
        VkDevice vkDevice = VK_NULL_HANDLE;
        uint32_t queueFamily = 0;
        std::mutex* queueMutex = nullptr;

        PFN_xrGetVulkanInstanceExtensionsKHR pGetInstanceExtensions = nullptr;
        PFN_xrGetVulkanDeviceExtensionsKHR pGetDeviceExtensions = nullptr;
        PFN_xrGetVulkanGraphicsDeviceKHR pGetGraphicsDevice = nullptr;
        PFN_xrGetVulkanGraphicsRequirementsKHR pGetGraphicsRequirements = nullptr;

        EyeSwapchain eyes[2];
        bool swapchainsReady = false;
        bool frameBegun = false;
        bool shouldRender = false;
        XrTime predictedTime = 0;
        XrDuration predictedPeriod = 13888888;

        std::mutex viewsMutex;            // latest + history (workload thread reads, present thread writes)
        ViewSet latest;
        ViewSet history[16];
        ViewSet submitting;
        uint64_t nextToken = 1;
        bool haveReference = false;
        float refYaw = 0;
        V3 refPos = { 0, 0, 0 };

        std::mutex eventsMutex;

        bool create() {
            if (const char* s = std::getenv("HYP_VR_SCALE")) unitsPerMetre = (float)std::atof(s);

#ifdef __ANDROID__
            // The Android loader must be initialised with the app's JavaVM and activity first (see android_glue.cpp).
#endif
            std::vector<const char*> exts = { XR_KHR_VULKAN_ENABLE_EXTENSION_NAME };
#ifdef __ANDROID__
            exts.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
#endif
            XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
            std::strcpy(ci.applicationInfo.applicationName, "Hey You Pikachu VR");
            ci.applicationInfo.applicationVersion = 1;
            std::strcpy(ci.applicationInfo.engineName, "RT64");
            ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
            ci.enabledExtensionCount = (uint32_t)exts.size();
            ci.enabledExtensionNames = exts.data();
            XrResult r = xrCreateInstance(&ci, &instance);
            if (XR_FAILED(r)) {
                std::printf("[vr] xrCreateInstance failed: %d (no OpenXR runtime?)\n", (int)r);
                return false;
            }

            XrInstanceProperties ip{ XR_TYPE_INSTANCE_PROPERTIES };
            xrGetInstanceProperties(instance, &ip);
            std::printf("[vr] runtime: %s %u.%u.%u\n", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
                XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

            XrSystemGetInfo si{ XR_TYPE_SYSTEM_GET_INFO };
            si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
            r = xrGetSystem(instance, &si, &system);
            if (XR_FAILED(r)) {
                std::printf("[vr] no headset found: %d\n", (int)r);
                return false;
            }

            uint32_t viewCount = 0;
            xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
            std::vector<XrViewConfigurationView> views(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
            xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, views.data());
            if (viewCount >= 1) {
                recommendedWidth = views[0].recommendedImageRectWidth;
                recommendedHeight = views[0].recommendedImageRectHeight;
            }
            std::printf("[vr] eye image %ux%u recommended\n", recommendedWidth, recommendedHeight);

            xrGetInstanceProcAddr(instance, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction*)&pGetInstanceExtensions);
            xrGetInstanceProcAddr(instance, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction*)&pGetDeviceExtensions);
            xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction*)&pGetGraphicsDevice);
            xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&pGetGraphicsRequirements);
            if (!pGetInstanceExtensions || !pGetDeviceExtensions || !pGetGraphicsDevice || !pGetGraphicsRequirements) {
                std::printf("[vr] runtime lacks XR_KHR_vulkan_enable\n");
                return false;
            }

            XrGraphicsRequirementsVulkanKHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
            check(pGetGraphicsRequirements(instance, system, &req), "xrGetVulkanGraphicsRequirementsKHR");

            // Tell RT64's Vulkan layer what the runtime needs.
            RT64::VulkanExternalHooks& hooks = RT64::GetVulkanExternalHooks();
            uint32_t len = 0;
            pGetInstanceExtensions(instance, system, 0, &len, nullptr);
            std::string buf(len, '\0');
            pGetInstanceExtensions(instance, system, len, &len, buf.data());
            hooks.instanceExtensions = split_spaces(buf.c_str());
            pGetDeviceExtensions(instance, system, 0, &len, nullptr);
            buf.assign(len, '\0');
            pGetDeviceExtensions(instance, system, len, &len, buf.data());
            hooks.deviceExtensions = split_spaces(buf.c_str());
            std::printf("[vr] Vulkan needs %zu instance and %zu device extensions\n", hooks.instanceExtensions.size(), hooks.deviceExtensions.size());

            hooks.pickPhysicalDevice = [this](VkInstance vki) {
                VkPhysicalDevice pd = VK_NULL_HANDLE;
                check(pGetGraphicsDevice(instance, system, vki, &pd), "xrGetVulkanGraphicsDeviceKHR");
                return pd;
            };
            hooks.deviceCreated = [this](VkInstance vki, VkPhysicalDevice pd, VkDevice dev, uint32_t family, VkQueue, std::mutex* mutex) {
                vkInstance = vki; vkPhysical = pd; vkDevice = dev; queueFamily = family; queueMutex = mutex;
                createSession();
            };
            return true;
        }

        void createSession() {
            XrGraphicsBindingVulkanKHR binding{ XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };
            binding.instance = vkInstance;
            binding.physicalDevice = vkPhysical;
            binding.device = vkDevice;
            binding.queueFamilyIndex = queueFamily;
            binding.queueIndex = 0;
            XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
            sci.next = &binding;
            sci.systemId = system;
            XrResult r = xrCreateSession(instance, &sci, &session);
            if (XR_FAILED(r)) {
                std::printf("[vr] xrCreateSession failed: %d\n", (int)r);
                session = XR_NULL_HANDLE;
                return;
            }

            XrReferenceSpaceCreateInfo rs{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
            rs.poseInReferenceSpace.orientation.w = 1.0f;
            rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            check(xrCreateReferenceSpace(session, &rs, &localSpace), "xrCreateReferenceSpace LOCAL");
            rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
            check(xrCreateReferenceSpace(session, &rs, &viewSpace), "xrCreateReferenceSpace VIEW");
            std::printf("[vr] session created\n");
        }

        void pollEvents() {
            if (instance == XR_NULL_HANDLE) return;
            std::scoped_lock lock(eventsMutex);
            XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
            while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
                if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                    const auto& sc = *reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                    state = sc.state;
                    std::printf("[vr] session state %d\n", (int)state);
                    if (state == XR_SESSION_STATE_READY) {
                        XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
                        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        if (XR_SUCCEEDED(xrBeginSession(session, &bi))) running = true;
                    }
                    else if (state == XR_SESSION_STATE_STOPPING) {
                        running = false;
                        xrEndSession(session);
                    }
                    else if ((state == XR_SESSION_STATE_EXITING) || (state == XR_SESSION_STATE_LOSS_PENDING)) {
                        running = false;
                        quit = true;
                    }
                }
                else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                    running = false;
                    quit = true;
                }
                ev = { XR_TYPE_EVENT_DATA_BUFFER };
            }
        }

        // Eye images are 4:3 like the N64 picture: full recommended width, matching height.
        void createSwapchains(RT64::RenderDevice* device) {
            uint32_t count = 0;
            xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
            std::vector<int64_t> formats(count);
            xrEnumerateSwapchainFormats(session, count, &count, formats.data());
#ifdef __ANDROID__
            const VkFormat viewFormat = VK_FORMAT_R8G8B8A8_UNORM, srgbFormat = VK_FORMAT_R8G8B8A8_SRGB;
#else
            const VkFormat viewFormat = VK_FORMAT_B8G8R8A8_UNORM, srgbFormat = VK_FORMAT_B8G8R8A8_SRGB;
#endif
            // The VI pass writes gamma-encoded values: store them in an sRGB image through a UNORM view so the
            // compositor decodes them once. Fall back to a plain UNORM image (a bit washed out).
            int64_t format = 0;
            bool mutableView = false;
            for (int64_t f : formats) if (f == srgbFormat) { format = f; mutableView = true; }
            if (format == 0) for (int64_t f : formats) if (f == viewFormat) format = f;
            if (format == 0) {
                std::printf("[vr] no usable swapchain format\n");
                return;
            }

            const uint32_t w = recommendedWidth;
            const uint32_t h = (recommendedWidth * 3) / 4;
            for (int e = 0; e < 2; e++) {
                EyeSwapchain& sc = eyes[e];
                XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
                ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
                if (mutableView) ci.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
                ci.format = format;
                ci.sampleCount = 1;
                ci.width = w;
                ci.height = h;
                ci.faceCount = 1;
                ci.arraySize = 1;
                ci.mipCount = 1;
                XrResult r = xrCreateSwapchain(session, &ci, &sc.handle);
                if (XR_FAILED(r)) {
                    std::printf("[vr] xrCreateSwapchain failed: %d\n", (int)r);
                    return;
                }

                sc.width = w;
                sc.height = h;
                uint32_t n = 0;
                xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr);
                sc.images.assign(n, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });
                xrEnumerateSwapchainImages(sc.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data()));
                for (uint32_t i = 0; i < n; i++) {
                    auto tex = std::make_unique<RT64::VulkanTexture>(static_cast<RT64::VulkanDevice*>(device), sc.images[i].image);
                    tex->desc.dimension = RT64::RenderTextureDimension::TEXTURE_2D;
                    tex->desc.format = RT64::SwapChainFormat;
                    tex->desc.width = w;
                    tex->desc.height = h;
                    tex->desc.depth = 1;
                    tex->desc.mipLevels = 1;
                    tex->desc.flags = RT64::RenderTextureFlag::RENDER_TARGET;
                    tex->fillSubresourceRange();
                    tex->createImageView(viewFormat);
                    const RT64::RenderTexture* attachment = tex.get();
                    sc.framebuffers.push_back(device->createFramebuffer(RT64::RenderFramebufferDesc(&attachment, 1)));
                    sc.textures.push_back(std::move(tex));
                }
            }

            swapchainsReady = true;
            std::printf("[vr] swapchains %ux%u (format %lld%s)\n", w, h, (long long)format, mutableView ? ", sRGB" : "");
        }

        // Head pose -> per-eye matrices for RT64 (relative to the starting head position and facing).
        void locate(XrTime time) {
            XrViewLocateInfo li{ XR_TYPE_VIEW_LOCATE_INFO };
            li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            li.displayTime = time;
            li.space = localSpace;
            XrViewState vs{ XR_TYPE_VIEW_STATE };
            XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
            uint32_t n = 0;
            if (XR_FAILED(xrLocateViews(session, &li, &vs, 2, &n, views)) || (n < 2)) return;
            if (!(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return;

            if (!haveReference) {
                XrSpaceLocation head{ XR_TYPE_SPACE_LOCATION };
                if (XR_SUCCEEDED(xrLocateSpace(viewSpace, localSpace, time, &head)) && (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                    const M3 r = quat_to_m3(head.pose.orientation);
                    const V3 fwd = mul(r, V3{ 0, 0, -1 });
                    refYaw = std::atan2(-fwd.x, -fwd.z);
                    refPos = { head.pose.position.x, head.pose.position.y, head.pose.position.z };
                    haveReference = true;
                    std::printf("[vr] recentred: yaw %.1f deg\n", refYaw * 57.2958f);
                    for (int e = 0; e < 2; e++) {
                        const XrView& v = views[e];
                        std::printf("[vr] eye %d at (%.3f %.3f %.3f) m, fov L%.1f R%.1f U%.1f D%.1f deg\n", e,
                            v.pose.position.x, v.pose.position.y, v.pose.position.z,
                            v.fov.angleLeft * 57.2958f, v.fov.angleRight * 57.2958f, v.fov.angleUp * 57.2958f, v.fov.angleDown * 57.2958f);
                    }
                }
            }

            const M3 undoYaw = rot_y(-refYaw);
            ViewSet set;
            set.views[0] = views[0];
            set.views[1] = views[1];
            for (int e = 0; e < 2; e++) {
                const XrPosef& p = views[e].pose;
                M3 rc = mul(undoYaw, quat_to_m3(p.orientation));                      // eye -> camera rotation (column vectors)
                static const float debugRoll = std::getenv("HYP_VR_DEBUG_ROLL") ? (float)std::atof(std::getenv("HYP_VR_DEBUG_ROLL")) : 0.0f;
                if ((debugRoll != 0.0f) && (e == 0)) {                                 // debug: tilt the left eye (degrees)
                    const float a = debugRoll / 57.2958f;
                    M3 roll = {};
                    roll.m[0][0] = std::cos(a); roll.m[0][1] = -std::sin(a);
                    roll.m[1][0] = std::sin(a); roll.m[1][1] = std::cos(a);
                    roll.m[2][2] = 1;
                    rc = mul(rc, roll);
                }
                V3 t = mul(undoYaw, V3{ p.position.x - refPos.x, p.position.y - refPos.y, p.position.z - refPos.z });
                t = { t.x * unitsPerMetre, t.y * unitsPerMetre, t.z * unitsPerMetre };

                // Row-vector inverse of (eye -> camera): upper 3x3 = rc, translation row = -t * rc.
                float* m = set.rt64.eyes[e].eyeFromCamera;
                for (int i = 0; i < 3; i++) {
                    for (int j = 0; j < 3; j++) m[i * 4 + j] = rc.m[i][j];
                    m[i * 4 + 3] = 0.0f;
                }
                for (int j = 0; j < 3; j++) m[12 + j] = -(t.x * rc.m[0][j] + t.y * rc.m[1][j] + t.z * rc.m[2][j]);
                m[15] = 1.0f;

                // 4:3 frustum: the headset's horizontal field, the matching vertical one around its centre.
                const float l = std::tan(views[e].fov.angleLeft), r = std::tan(views[e].fov.angleRight);
                const float uc = 0.5f * (std::tan(views[e].fov.angleUp) + std::tan(views[e].fov.angleDown));
                const float halfH = 0.5f * (r - l) * 0.75f;
                set.rt64.eyes[e].tanLeft = l;
                set.rt64.eyes[e].tanRight = r;
                set.rt64.eyes[e].tanUp = uc + halfH;
                set.rt64.eyes[e].tanDown = uc - halfH;
            }

            std::scoped_lock lock(viewsMutex);
            set.token = nextToken++;
            set.rt64.token = set.token;
            latest = set;
            history[set.token % 16] = set;
        }

        // RT64::VRInterface

        bool active() override {
            pollEvents();
            return running && (session != XR_NULL_HANDLE);
        }

        void latestViews(RT64::VRFrameViews& out) override {
            std::scoped_lock lock(viewsMutex);
            if (latest.token == 0) {
                static bool logged = false;
                if (!logged) { logged = true; std::printf("[vr] views: no head pose yet, using the default view\n"); std::fflush(stdout); }
                // No pose yet: straight ahead with a 90 degree wide 4:3 view.
                for (int e = 0; e < 2; e++) {
                    float* m = out.eyes[e].eyeFromCamera;
                    for (int i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
                    out.eyes[e].tanLeft = -1.0f; out.eyes[e].tanRight = 1.0f;
                    out.eyes[e].tanUp = 0.75f; out.eyes[e].tanDown = -0.75f;
                }
                out.token = 0;
                return;
            }
            out = latest.rt64;
            static uint32_t logCounter = 0;
            if ((logCounter++ % 300) == 0) {
                std::printf("[vr] views token %llu: eye0 tanL %.3f tx %.2f, eye1 tanL %.3f tx %.2f\n", (unsigned long long)out.token,
                    out.eyes[0].tanLeft, out.eyes[0].eyeFromCamera[12], out.eyes[1].tanLeft, out.eyes[1].eyeFromCamera[12]);
                std::fflush(stdout);
            }

            // Debug: HYP_VR_SWAP_EYES=1 or HYP_CONTROL "vrswap" draws the right eye into the game's own framebuffer (fbshot).
            if (swapEyes) std::swap(out.eyes[0], out.eyes[1]);
        }

        std::atomic<bool> swapEyes{ std::getenv("HYP_VR_SWAP_EYES") != nullptr };

        std::mutex dumpMutex;
        std::string dumpPrefix;

        int dumpFramesLeft = 0;

        // Eight frames in a row (<prefix>_f0.._f7): many HYP frames only paste a saved picture of the scene,
        // so a single frame may not show live 3D.
        bool takeDumpRequest(std::string& prefix) override {
            std::scoped_lock lock(dumpMutex);
            if (dumpPrefix.empty()) return false;
            if (dumpFramesLeft <= 0) dumpFramesLeft = 8;
            prefix = dumpPrefix + "_f" + std::to_string(8 - dumpFramesLeft);
            if (--dumpFramesLeft == 0) dumpPrefix.clear();
            return true;
        }

        bool beginPresent(RT64::RenderDevice* device, uint64_t token, RT64::VRTarget out[2]) override {
            frameBegun = false;
            pollEvents();
            if (!running) return false;

            XrFrameState fs{ XR_TYPE_FRAME_STATE };
            if (XR_FAILED(xrWaitFrame(session, nullptr, &fs))) return false;
            if (XR_FAILED(xrBeginFrame(session, nullptr))) return false;
            frameBegun = true;
            shouldRender = fs.shouldRender;
            predictedTime = fs.predictedDisplayTime;
            if (fs.predictedDisplayPeriod > 0) predictedPeriod = fs.predictedDisplayPeriod;

            // Pose for the next frames RT64 renders (about one frame ahead of this one).
            locate(predictedTime + predictedPeriod);

            if (!shouldRender) return false;
            if (!swapchainsReady) {
                createSwapchains(device);
                if (!swapchainsReady) return false;
            }

            {
                std::scoped_lock lock(viewsMutex);
                const ViewSet& h = history[token % 16];
                submitting = ((token != 0) && (h.token == token)) ? h : latest;
            }
            if (submitting.token == 0) return false;

            for (int e = 0; e < 2; e++) {
                EyeSwapchain& sc = eyes[e];
                XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
                XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                wi.timeout = XR_INFINITE_DURATION;
                XrResult r;
                {
                    std::scoped_lock lock(*queueMutex);         // the runtime may use the queue here
                    r = xrAcquireSwapchainImage(sc.handle, &ai, &sc.acquired);
                }
                if (XR_FAILED(r)) {
                    sc.acquired = UINT32_MAX;
                    return false;
                }
                if (XR_FAILED(xrWaitSwapchainImage(sc.handle, &wi))) return false;   // released in endPresent

                out[e].texture = sc.textures[sc.acquired].get();
                out[e].framebuffer = sc.framebuffers[sc.acquired].get();
                out[e].width = sc.width;
                out[e].height = sc.height;
            }
            return true;
        }

        void endPresent(bool drawn) override {
            if (!frameBegun) return;
            frameBegun = false;
            std::scoped_lock lock(*queueMutex);
            bool released = true;
            for (int e = 0; e < 2; e++) {
                if (eyes[e].acquired != UINT32_MAX) {
                    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                    xrReleaseSwapchainImage(eyes[e].handle, &ri);
                    eyes[e].acquired = UINT32_MAX;
                }
                else {
                    released = false;
                }
            }

            XrCompositionLayerProjectionView pv[2];
            XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
            const XrCompositionLayerBaseHeader* layers[] = { reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer) };
            XrFrameEndInfo fe{ XR_TYPE_FRAME_END_INFO };
            fe.displayTime = predictedTime;
            fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            if (drawn && released && shouldRender) {
                for (int e = 0; e < 2; e++) {
                    const RT64::VREye& eye = submitting.rt64.eyes[e];
                    pv[e] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
                    pv[e].pose = submitting.views[e].pose;
                    pv[e].fov.angleLeft = std::atan(eye.tanLeft);
                    pv[e].fov.angleRight = std::atan(eye.tanRight);
                    pv[e].fov.angleUp = std::atan(eye.tanUp);
                    pv[e].fov.angleDown = std::atan(eye.tanDown);
                    pv[e].subImage.swapchain = eyes[e].handle;
                    pv[e].subImage.imageRect = { { 0, 0 }, { (int32_t)eyes[e].width, (int32_t)eyes[e].height } };
                }
                layer.space = localSpace;
                layer.viewCount = 2;
                layer.views = pv;
                fe.layerCount = 1;
                fe.layers = layers;
            }
            check(xrEndFrame(session, &fe), "xrEndFrame");
        }

        void releaseDeviceResources() override {
            running = false;
            swapchainsReady = false;
            for (auto& sc : eyes) {
                sc.framebuffers.clear();
                sc.textures.clear();
                if (sc.handle != XR_NULL_HANDLE) xrDestroySwapchain(sc.handle);
                sc.handle = XR_NULL_HANDLE;
            }
            if (viewSpace != XR_NULL_HANDLE) xrDestroySpace(viewSpace);
            if (localSpace != XR_NULL_HANDLE) xrDestroySpace(localSpace);
            if (session != XR_NULL_HANDLE) xrDestroySession(session);
            viewSpace = localSpace = XR_NULL_HANDLE;
            session = XR_NULL_HANDLE;
            std::printf("[vr] session closed\n");
        }

        void destroy() {
            for (auto& sc : eyes) {
                sc.framebuffers.clear();
                sc.textures.clear();
                if (sc.handle != XR_NULL_HANDLE) xrDestroySwapchain(sc.handle);
                sc.handle = XR_NULL_HANDLE;
            }
            if (viewSpace != XR_NULL_HANDLE) xrDestroySpace(viewSpace);
            if (localSpace != XR_NULL_HANDLE) xrDestroySpace(localSpace);
            if (session != XR_NULL_HANDLE) xrDestroySession(session);
            if (instance != XR_NULL_HANDLE) xrDestroyInstance(instance);
            viewSpace = localSpace = XR_NULL_HANDLE;
            session = XR_NULL_HANDLE;
            instance = XR_NULL_HANDLE;
        }
    };

    OpenXRSession* g_xr = nullptr;
}

bool hyp::vr::requested() {
#ifdef HYP_VR_BUILD
    return true;
#else
    const char* v = std::getenv("HYP_VR");
    return (v != nullptr) && (v[0] == '1');
#endif
}

bool hyp::vr::init() {
    if (g_xr != nullptr) return true;
    auto* xr = new OpenXRSession();
    if (!xr->create()) {
        xr->destroy();
        delete xr;
        RT64::GetVulkanExternalHooks() = {};
        return false;
    }
    g_xr = xr;
    RT64::SetVRInterface(xr);
    return true;
}

bool hyp::vr::enabled() { return g_xr != nullptr; }

unsigned hyp::vr::refresh_rate() {
    if (const char* s = std::getenv("HYP_VR_HZ")) return (unsigned)std::atoi(s);
    return 72;
}

unsigned hyp::vr::eye_width() { return (g_xr != nullptr) ? g_xr->recommendedWidth : 1440; }

bool hyp::vr::quit_requested() { return (g_xr != nullptr) && g_xr->quit; }

void hyp::vr::toggle_swap_eyes() {
    if (g_xr != nullptr) g_xr->swapEyes = !g_xr->swapEyes;
}

void hyp::vr::request_dump(const std::string& prefix) {
    if (g_xr == nullptr) return;
    std::scoped_lock lock(g_xr->dumpMutex);
    g_xr->dumpPrefix = prefix;
}

void hyp::vr::shutdown() {
    if (g_xr == nullptr) return;
    RT64::SetVRInterface(nullptr);
    g_xr->destroy();
    delete g_xr;
    g_xr = nullptr;
}

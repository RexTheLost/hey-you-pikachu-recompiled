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

#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include "vulkan/rt64_vulkan.h"     // RT64's Vulkan layer (volk + vulkan headers, VulkanExternalHooks, VulkanTexture)
#include "hle/rt64_vr.h"
#include "hyp_settings.h"
#include "vr_ui.h"

#define XR_USE_GRAPHICS_API_VULKAN
#ifdef __ANDROID__
#define XR_USE_PLATFORM_ANDROID
#include <jni.h>
#include <SDL_system.h>     // SDL_AndroidGetJNIEnv / SDL_AndroidGetActivity
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
        float unitsPerMetre = 12.0f;      // game units per metre: Pikachu (~0.4 m) is ~5 units tall

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

        // ---------- controllers (OpenXR actions) ----------
        XrActionSet actionSet = XR_NULL_HANDLE;
        XrAction aAim = XR_NULL_HANDLE, aTrigger = XR_NULL_HANDLE, aSqueeze = XR_NULL_HANDLE, aStick = XR_NULL_HANDLE;
        XrAction aA = XR_NULL_HANDLE, aB = XR_NULL_HANDLE, aX = XR_NULL_HANDLE, aY = XR_NULL_HANDLE, aMenu = XR_NULL_HANDLE;
        XrPath hands[2] = { XR_NULL_PATH, XR_NULL_PATH };
        XrSpace aimSpace[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
        bool actionsAttached = false;

        struct Pad {                  // one frame of both controllers
            float trigger[2] = {}, squeeze[2] = {};
            float stickX[2] = {}, stickY[2] = {};
            bool a = false, b = false, x = false, y = false, menu = false;
            bool aimValid[2] = {};
            XrPosef aim[2] = {};
        };
        Pad pad, prevPad;
        std::mutex padMutex;          // the game thread reads the N64 mapping
        uint16_t n64Buttons = 0;
        float n64X = 0, n64Y = 0;
        bool panelOpen = false;
        XrTime lastInputTime = 0;
        bool eyePopupDone = false;

        XrAction makeAction(const char* name, const char* label, XrActionType type, bool perHand) {
            XrActionCreateInfo ci{ XR_TYPE_ACTION_CREATE_INFO };
            std::strcpy(ci.actionName, name);
            std::strcpy(ci.localizedActionName, label);
            ci.actionType = type;
            if (perHand) {
                ci.countSubactionPaths = 2;
                ci.subactionPaths = hands;
            }
            XrAction a = XR_NULL_HANDLE;
            check(xrCreateAction(actionSet, &ci, &a), name);
            return a;
        }

        XrPath path(const char* s) {
            XrPath p = XR_NULL_PATH;
            xrStringToPath(instance, s, &p);
            return p;
        }

        void createActions() {
            XrActionSetCreateInfo si{ XR_TYPE_ACTION_SET_CREATE_INFO };
            std::strcpy(si.actionSetName, "hyp");
            std::strcpy(si.localizedActionSetName, "Hey You Pikachu");
            if (XR_FAILED(xrCreateActionSet(instance, &si, &actionSet))) return;
            hands[0] = path("/user/hand/left");
            hands[1] = path("/user/hand/right");
            aAim = makeAction("aim", "Aim", XR_ACTION_TYPE_POSE_INPUT, true);
            aTrigger = makeAction("trigger", "Trigger (talk)", XR_ACTION_TYPE_FLOAT_INPUT, true);
            aSqueeze = makeAction("squeeze", "Grip", XR_ACTION_TYPE_FLOAT_INPUT, true);
            aStick = makeAction("stick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, true);
            aA = makeAction("a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
            aB = makeAction("b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
            aX = makeAction("x", "X (Start)", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
            aY = makeAction("y", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
            aMenu = makeAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false);

            std::vector<XrActionSuggestedBinding> b = {
                { aAim, path("/user/hand/left/input/aim/pose") }, { aAim, path("/user/hand/right/input/aim/pose") },
                { aTrigger, path("/user/hand/left/input/trigger/value") }, { aTrigger, path("/user/hand/right/input/trigger/value") },
                { aSqueeze, path("/user/hand/left/input/squeeze/value") }, { aSqueeze, path("/user/hand/right/input/squeeze/value") },
                { aStick, path("/user/hand/left/input/thumbstick") }, { aStick, path("/user/hand/right/input/thumbstick") },
                { aA, path("/user/hand/right/input/a/click") }, { aB, path("/user/hand/right/input/b/click") },
                { aX, path("/user/hand/left/input/x/click") }, { aY, path("/user/hand/left/input/y/click") },
                { aMenu, path("/user/hand/left/input/menu/click") },
            };
            XrInteractionProfileSuggestedBinding sb{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
            sb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
            sb.suggestedBindings = b.data();
            sb.countSuggestedBindings = (uint32_t)b.size();
            check(xrSuggestInteractionProfileBindings(instance, &sb), "touch controller bindings");

            // Fallback for runtimes without Touch bindings: trigger = select, menu = menu.
            std::vector<XrActionSuggestedBinding> simple = {
                { aAim, path("/user/hand/left/input/aim/pose") }, { aAim, path("/user/hand/right/input/aim/pose") },
                { aA, path("/user/hand/right/input/select/click") }, { aMenu, path("/user/hand/left/input/menu/click") },
            };
            sb.interactionProfile = path("/interaction_profiles/khr/simple_controller");
            sb.suggestedBindings = simple.data();
            sb.countSuggestedBindings = (uint32_t)simple.size();
            check(xrSuggestInteractionProfileBindings(instance, &sb), "simple controller bindings");
        }

        void attachActions() {
            if (actionSet == XR_NULL_HANDLE || session == XR_NULL_HANDLE) return;
            XrSessionActionSetsAttachInfo ai{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
            ai.countActionSets = 1;
            ai.actionSets = &actionSet;
            if (XR_FAILED(xrAttachSessionActionSets(session, &ai))) return;
            for (int h = 0; h < 2; h++) {
                XrActionSpaceCreateInfo sc{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
                sc.action = aAim;
                sc.subactionPath = hands[h];
                sc.poseInActionSpace.orientation.w = 1.0f;
                xrCreateActionSpace(session, &sc, &aimSpace[h]);
            }
            actionsAttached = true;
        }

        float getFloat(XrAction a, int hand) {
            XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
            gi.action = a;
            gi.subactionPath = hand >= 0 ? hands[hand] : XR_NULL_PATH;
            XrActionStateFloat st{ XR_TYPE_ACTION_STATE_FLOAT };
            return XR_SUCCEEDED(xrGetActionStateFloat(session, &gi, &st)) && st.isActive ? st.currentState : 0.0f;
        }

        bool getBool(XrAction a) {
            XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
            gi.action = a;
            XrActionStateBoolean st{ XR_TYPE_ACTION_STATE_BOOLEAN };
            return XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &st)) && st.isActive && st.currentState;
        }

        void readPad(XrTime time) {
            prevPad = pad;
            pad = Pad{};
            if (!actionsAttached || state != XR_SESSION_STATE_FOCUSED) return;
            const XrActiveActionSet active{ actionSet, XR_NULL_PATH };
            XrActionsSyncInfo sync{ XR_TYPE_ACTIONS_SYNC_INFO };
            sync.countActiveActionSets = 1;
            sync.activeActionSets = &active;
            if (XR_FAILED(xrSyncActions(session, &sync))) return;
            for (int h = 0; h < 2; h++) {
                pad.trigger[h] = getFloat(aTrigger, h);
                pad.squeeze[h] = getFloat(aSqueeze, h);
                XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
                gi.action = aStick;
                gi.subactionPath = hands[h];
                XrActionStateVector2f st{ XR_TYPE_ACTION_STATE_VECTOR2F };
                if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &gi, &st)) && st.isActive) {
                    pad.stickX[h] = st.currentState.x;
                    pad.stickY[h] = st.currentState.y;
                }
                XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
                if (aimSpace[h] != XR_NULL_HANDLE && XR_SUCCEEDED(xrLocateSpace(aimSpace[h], localSpace, time, &loc))) {
                    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
                    pad.aimValid[h] = (loc.locationFlags & need) == need;
                    pad.aim[h] = loc.pose;
                }
            }
            pad.a = getBool(aA);
            pad.b = getBool(aB);
            pad.x = getBool(aX);
            pad.y = getBool(aY);
            pad.menu = getBool(aMenu);
            applyDebugInput();
        }

        // Debug (HYP_CONTROL "vrin <name> [seconds] [x y]"): fake controller input for hidden tests.
        std::mutex debugMutex;
        std::chrono::steady_clock::time_point dbgUntil[7];     // menu, a, b, x, y, trigger, stick
        float dbgStickX = 0, dbgStickY = 0;

        void applyDebugInput() {
            std::scoped_lock lock(debugMutex);
            const auto now = std::chrono::steady_clock::now();
            if (now < dbgUntil[0]) pad.menu = true;
            if (now < dbgUntil[1]) pad.a = true;
            if (now < dbgUntil[2]) pad.b = true;
            if (now < dbgUntil[3]) pad.x = true;
            if (now < dbgUntil[4]) pad.y = true;
            if (now < dbgUntil[5]) pad.trigger[1] = 1.0f;
            if (now < dbgUntil[6]) { pad.stickX[1] = dbgStickX; pad.stickY[1] = dbgStickY; }
        }

        void debugInput(const std::string& name, float seconds, float x, float y) {
            if (name == "words") { hyp::vr::ui::open_words_current(); return; }    // as if the Words button was pressed
            static const char* names[] = { "menu", "a", "b", "x", "y", "trigger", "stick" };
            std::scoped_lock lock(debugMutex);
            for (int i = 0; i < 7; i++) {
                if (name == names[i]) {
                    dbgUntil[i] = std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(seconds * 1000));
                    if (i == 6) { dbgStickX = x; dbgStickY = y; }
                }
            }
        }

        // ---------- quad layers: the floating panel and the Words button ----------
        struct QuadLayer {
            const char* name = "";
            int w = 0, h = 0;
            XrSwapchain handle = XR_NULL_HANDLE;
            std::vector<XrSwapchainImageVulkanKHR> images;
            std::vector<std::unique_ptr<RT64::VulkanTexture>> textures;
            std::unique_ptr<RT64::RenderBuffer> upload;
            std::vector<uint8_t> pixels;      // RGBA w*h
            bool pending = false;             // pixels changed: upload on the next frame
            uint32_t acquired = UINT32_MAX;
            bool everReleased = false;
        };
        QuadLayer quad{ "panel", hyp::vr::ui::W, hyp::vr::ui::H };
        QuadLayer dock{ "words button", hyp::vr::ui::DOCK_W, hyp::vr::ui::DOCK_H };
        XrPosef panelPose{};          // in LOCAL space
        bool panelPlaced = false;
        float panelHeight = hyp::vr::ui::WIDTH_M * hyp::vr::ui::H / hyp::vr::ui::W;
        XrPosef dockPose{};           // above the left controller
        bool dockVisible = false;
        bool dockHovered = false;
        int dockDrawnHover = -1;

        void createQuad(QuadLayer& q, RT64::RenderDevice* device) {
            uint32_t count = 0;
            xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
            std::vector<int64_t> formats(count);
            xrEnumerateSwapchainFormats(session, count, &count, formats.data());
            int64_t format = 0;
            for (int64_t f : formats) if (f == VK_FORMAT_R8G8B8A8_SRGB) format = f;
            if (format == 0) for (int64_t f : formats) if (f == VK_FORMAT_R8G8B8A8_UNORM) format = f;
            if (format == 0) { std::printf("[vr] no RGBA format for the %s\n", q.name); return; }

            XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
            ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            ci.format = format;
            ci.sampleCount = 1;
            ci.width = q.w;
            ci.height = q.h;
            ci.faceCount = 1;
            ci.arraySize = 1;
            ci.mipCount = 1;
            if (XR_FAILED(xrCreateSwapchain(session, &ci, &q.handle))) { std::printf("[vr] %s swapchain failed\n", q.name); return; }
            uint32_t n = 0;
            xrEnumerateSwapchainImages(q.handle, 0, &n, nullptr);
            q.images.assign(n, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });
            xrEnumerateSwapchainImages(q.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(q.images.data()));
            for (uint32_t i = 0; i < n; i++) {
                auto tex = std::make_unique<RT64::VulkanTexture>(static_cast<RT64::VulkanDevice*>(device), q.images[i].image);
                tex->desc.dimension = RT64::RenderTextureDimension::TEXTURE_2D;
                tex->desc.format = RT64::RenderFormat::R8G8B8A8_UNORM;
                tex->desc.width = q.w;
                tex->desc.height = q.h;
                tex->desc.depth = 1;
                tex->desc.mipLevels = 1;
                tex->fillSubresourceRange();
                q.textures.push_back(std::move(tex));
            }
            q.upload = device->createBuffer(RT64::RenderBufferDesc::UploadBuffer(uint64_t(q.w) * q.h * 4));
            std::printf("[vr] %s swapchain %dx%d\n", q.name, q.w, q.h);
        }

        // Records the upload of q's pending pixels (acquires an image; endPresent releases it).
        bool uploadQuad(QuadLayer& q, RT64::RenderDevice* device, RT64::RenderCommandList* commandList) {
            if (!q.pending) return false;
            if (q.handle == XR_NULL_HANDLE) {
                createQuad(q, device);
                if (q.handle == XR_NULL_HANDLE || !q.upload) { q.pending = false; return false; }
            }
            XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            XrResult r;
            {
                std::scoped_lock lock(*queueMutex);
                r = xrAcquireSwapchainImage(q.handle, &ai, &q.acquired);
            }
            if (XR_FAILED(r)) { q.acquired = UINT32_MAX; return false; }
            XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            wi.timeout = XR_INFINITE_DURATION;
            if (XR_FAILED(xrWaitSwapchainImage(q.handle, &wi))) return false;     // released in endPresent

            void* dst = q.upload->map();
            std::memcpy(dst, q.pixels.data(), std::min(q.pixels.size(), size_t(q.w) * q.h * 4));
            q.upload->unmap();
            q.pending = false;

            RT64::VulkanTexture* tex = q.textures[q.acquired].get();
            commandList->barriers(RT64::RenderBarrierStage::COPY, RT64::RenderTextureBarrier(tex, RT64::RenderTextureLayout::COPY_DEST));
            commandList->copyTextureRegion(RT64::RenderTextureCopyLocation::Subresource(tex),
                RT64::RenderTextureCopyLocation::PlacedFootprint(q.upload.get(), RT64::RenderFormat::R8G8B8A8_UNORM, q.w, q.h, 1, q.w));
            commandList->barriers(RT64::RenderBarrierStage::GRAPHICS, RT64::RenderTextureBarrier(tex, RT64::RenderTextureLayout::COLOR_WRITE));
            return true;
        }

        void releaseQuad(QuadLayer& q) {        // under the queue lock
            if (q.acquired == UINT32_MAX) return;
            XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(q.handle, &ri);
            q.acquired = UINT32_MAX;
            q.everReleased = true;
        }

        void destroyQuad(QuadLayer& q) {
            q.textures.clear();
            q.upload.reset();
            if (q.handle != XR_NULL_HANDLE) xrDestroySwapchain(q.handle);
            q.handle = XR_NULL_HANDLE;
            q.everReleased = false;
        }

        // Place the panel 1 m in front of the head, a little below eye level, facing it (yaw only).
        void placePanel(XrTime time) {
            XrSpaceLocation head{ XR_TYPE_SPACE_LOCATION };
            if (XR_FAILED(xrLocateSpace(viewSpace, localSpace, time, &head)) || !(head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return;
            const M3 r = quat_to_m3(head.pose.orientation);
            const V3 fwd = mul(r, V3{ 0, 0, -1 });
            const float yaw = std::atan2(-fwd.x, -fwd.z);
            panelPose.orientation = { 0.0f, std::sin(yaw / 2), 0.0f, std::cos(yaw / 2) };
            panelPose.position = { head.pose.position.x - std::sin(yaw) * 1.0f, head.pose.position.y - 0.15f, head.pose.position.z - std::cos(yaw) * 1.0f };
            panelPlaced = true;
        }

        bool hitPanel(const XrPosef& aim, float& px, float& py) {
            return hitQuad(aim, panelPose, hyp::vr::ui::WIDTH_M, panelHeight, hyp::vr::ui::W, hyp::vr::ui::H, px, py);
        }

        // Ray from an aim pose against a quad (pose, size in metres): canvas pixel (cw x ch canvas), or false.
        bool hitQuad(const XrPosef& aim, const XrPosef& quadPose, float qw, float qh, int cw, int chh, float& px, float& py) {
            const M3 ra = quat_to_m3(aim.orientation);
            const V3 dir = mul(ra, V3{ 0, 0, -1 });
            const M3 rp = quat_to_m3(quadPose.orientation);
            const V3 n = mul(rp, V3{ 0, 0, 1 });
            const V3 rel = { aim.position.x - quadPose.position.x, aim.position.y - quadPose.position.y, aim.position.z - quadPose.position.z };
            const float denom = dir.x * n.x + dir.y * n.y + dir.z * n.z;
            if (std::fabs(denom) < 1e-4f) return false;
            const float t = -(rel.x * n.x + rel.y * n.y + rel.z * n.z) / denom;
            if (t <= 0) return false;
            const V3 hit = { rel.x + dir.x * t, rel.y + dir.y * t, rel.z + dir.z * t };
            const V3 ax = mul(rp, V3{ 1, 0, 0 }), ay = mul(rp, V3{ 0, 1, 0 });
            const float lx = hit.x * ax.x + hit.y * ax.y + hit.z * ax.z;
            const float ly = hit.x * ay.x + hit.y * ay.y + hit.z * ay.z;
            if (std::fabs(lx) > qw / 2 || std::fabs(ly) > qh / 2) return false;
            px = (lx / qw + 0.5f) * cw;
            py = (0.5f - ly / qh) * chh;
            return true;
        }

        // The Words button: 7 cm above the left controller, facing the player, while the panel is closed.
        void updateDock(bool panelOpen, bool& triggerTaken) {
            triggerTaken = false;
            const bool show = !panelOpen && pad.aimValid[0];
            if (show) {
                const M3 r = quat_to_m3(pad.aim[0].orientation);
                const V3 up = mul(r, V3{ 0, 1, 0 }), fwd = mul(r, V3{ 0, 0, -1 });
                dockPose.orientation = pad.aim[0].orientation;
                dockPose.position = { pad.aim[0].position.x + up.x * 0.07f + fwd.x * 0.02f,
                                      pad.aim[0].position.y + up.y * 0.07f + fwd.y * 0.02f,
                                      pad.aim[0].position.z + up.z * 0.07f + fwd.z * 0.02f };
            }
            dockVisible = show;

            bool hovered = false;
            if (show && pad.aimValid[1]) {
                float px, py;
                const float w = hyp::vr::ui::DOCK_WIDTH_M, h = w * hyp::vr::ui::DOCK_H / hyp::vr::ui::DOCK_W;
                hovered = hitQuad(pad.aim[1], dockPose, w, h, hyp::vr::ui::DOCK_W, hyp::vr::ui::DOCK_H, px, py);
            }
            dockHovered = hovered;
            if (show && (dockDrawnHover != (int)hovered || dock.pixels.empty())) {
                hyp::vr::ui::dock_pixels(hovered, dock.pixels);
                dock.pending = true;
                dockDrawnHover = (int)hovered;
            }

            // Pointing at the button: the right trigger presses it instead of talking.
            if (hovered) {
                triggerTaken = true;
                if (pad.trigger[1] > 0.55f && prevPad.trigger[1] <= 0.55f) hyp::vr::ui::open_words_current();
            }
        }

        // Per frame on the present thread: controllers -> panel or the N64 pad.
        void updateInput(XrTime time) {
            readPad(time);
            const float dt = (lastInputTime != 0) ? std::clamp((time - lastInputTime) / 1e9f, 0.0f, 0.1f) : 0.0f;
            lastInputTime = time;

            if (!eyePopupDone && state == XR_SESSION_STATE_FOCUSED) {
                eyePopupDone = true;
                if (!hyp::settings::get().vr_eye_set) hyp::vr::ui::open_eye_distance(true);
            }

            hyp::vr::ui::Input in;
            in.dt = dt;
            in.menuPressed = pad.menu && !prevPad.menu;
            const int stickHand = (std::fabs(pad.stickX[1]) + std::fabs(pad.stickY[1]) > std::fabs(pad.stickX[0]) + std::fabs(pad.stickY[0])) ? 1 : 0;
            in.stickX = pad.stickX[stickHand];
            in.stickY = pad.stickY[stickHand];
            in.selectPressed = (pad.a && !prevPad.a) || (pad.x && !prevPad.x);
            in.selectHeld = pad.a || pad.x;
            in.backPressed = (pad.b && !prevPad.b) || (pad.y && !prevPad.y);
            const bool trig[2] = { pad.trigger[0] > 0.55f, pad.trigger[1] > 0.55f };
            const bool prevTrig[2] = { prevPad.trigger[0] > 0.55f, prevPad.trigger[1] > 0.55f };
            in.triggerHeld = trig[0] || trig[1];
            in.triggerPressed = (trig[0] && !prevTrig[0]) || (trig[1] && !prevTrig[1]);
            in.triggerReleased = !in.triggerHeld && (prevTrig[0] || prevTrig[1]);

            const bool wasOpen = hyp::vr::ui::visible();
            if (wasOpen && !panelPlaced) placePanel(time);
            if (wasOpen) {
                for (int h : { 1, 0 }) {           // right hand first
                    if (pad.aimValid[h] && hitPanel(pad.aim[h], in.px, in.py)) { in.pointing = true; break; }
                }
            }
            const bool open = hyp::vr::ui::update(in);
            if (open && !wasOpen) placePanel(time);
            if (!open) panelPlaced = false;
            if (hyp::vr::ui::take_recenter()) haveReference = false;
            if (open && hyp::vr::ui::take_pixels(quad.pixels)) quad.pending = true;

            bool rightTriggerTaken = false;
            updateDock(open, rightTriggerTaken);

            // The N64 pad (nothing while the panel is open).
            uint16_t b = 0;
            float x = 0, y = 0;
            if (!open && !wasOpen && !hyp::vr::ui::visible()) {
                x = pad.stickX[0];
                y = pad.stickY[0];
                if (pad.a) b |= 0x8000;                                   // A
                if (pad.b) b |= 0x4000;                                   // B
                if (pad.trigger[0] > 0.55f || (pad.trigger[1] > 0.55f && !rightTriggerTaken)) b |= 0x2000;  // Z = talk
                if (pad.x) b |= 0x1000;                                   // Start
                if (pad.squeeze[0] > 0.6f) b |= 0x0020;                   // L
                if (pad.squeeze[1] > 0.6f) b |= 0x0010;                   // R
                if (pad.stickY[1] > 0.5f) b |= 0x0008;                    // C up
                if (pad.stickY[1] < -0.5f) b |= 0x0004;                   // C down
                if (pad.stickX[1] < -0.5f) b |= 0x0002;                   // C left
                if (pad.stickX[1] > 0.5f) b |= 0x0001;                    // C right
            }
            std::scoped_lock lock(padMutex);
            n64Buttons = b;
            n64X = x;
            n64Y = y;
            panelOpen = open;
        }

        bool create() {
            if (const char* s = std::getenv("HYP_VR_SCALE")) unitsPerMetre = (float)std::atof(s);

            std::vector<const char*> exts = { XR_KHR_VULKAN_ENABLE_EXTENSION_NAME };
            XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
#ifdef __ANDROID__
            // The Android loader needs the app's Java VM and activity before anything else.
            JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
            JavaVM* vm = nullptr;
            if (env != nullptr) env->GetJavaVM(&vm);
            jobject activity = env != nullptr ? env->NewGlobalRef(static_cast<jobject>(SDL_AndroidGetActivity())) : nullptr;
            if (vm == nullptr || activity == nullptr) {
                std::printf("[vr] no Java VM / activity for OpenXR\n");
                return false;
            }
            PFN_xrInitializeLoaderKHR initLoader = nullptr;
            xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&initLoader);
            if (initLoader != nullptr) {
                XrLoaderInitInfoAndroidKHR li{ XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
                li.applicationVM = vm;
                li.applicationContext = activity;
                check(initLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&li)), "xrInitializeLoaderKHR");
            }
            exts.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
            static XrInstanceCreateInfoAndroidKHR androidInfo{ XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
            androidInfo.applicationVM = vm;
            androidInfo.applicationActivity = activity;
            ci.next = &androidInfo;
#endif
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
            createActions();
            hyp::vr::ui::init();
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
            attachActions();
            std::printf("[vr] session created%s\n", actionsAttached ? " (controllers ready)" : "");
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

            // Eye distance slider: move each eye toward/away from the point between them.
            const V3 centre = { (views[0].pose.position.x + views[1].pose.position.x) * 0.5f,
                                (views[0].pose.position.y + views[1].pose.position.y) * 0.5f,
                                (views[0].pose.position.z + views[1].pose.position.z) * 0.5f };
            const float dx = views[1].pose.position.x - views[0].pose.position.x;
            const float dy = views[1].pose.position.y - views[0].pose.position.y;
            const float dz = views[1].pose.position.z - views[0].pose.position.z;
            hyp::vr::ui::set_ipd_metres(std::sqrt(dx * dx + dy * dy + dz * dz));
            const float eyeScale = hyp::vr::ui::eye_scale();

            for (int e = 0; e < 2; e++) {
                XrPosef p = views[e].pose;
                p.position.x = centre.x + (p.position.x - centre.x) * eyeScale;
                p.position.y = centre.y + (p.position.y - centre.y) * eyeScale;
                p.position.z = centre.z + (p.position.z - centre.z) * eyeScale;
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
            if (dumpFramesLeft == 8 && !dock.pixels.empty()) {
                FILE* f = std::fopen((dumpPrefix + "_dock.ppm").c_str(), "wb");
                if (f != nullptr) {
                    std::fprintf(f, "P6\n%d %d\n255\n", dock.w, dock.h);
                    for (size_t i = 0; i + 3 < dock.pixels.size(); i += 4) {
                        const float a = dock.pixels[i + 3] / 255.0f;      // over a grey background
                        uint8_t rgb[3];
                        for (int c = 0; c < 3; c++) rgb[c] = (uint8_t)(dock.pixels[i + c] * a + 90 * (1 - a));
                        std::fwrite(rgb, 1, 3, f);
                    }
                    std::fclose(f);
                }
            }
            if (dumpFramesLeft == 8 && hyp::vr::ui::visible() && !quad.pixels.empty()) {
                // The panel is a separate layer (composited by the headset): save its canvas too.
                FILE* f = std::fopen((dumpPrefix + "_panel.ppm").c_str(), "wb");
                if (f != nullptr) {
                    std::fprintf(f, "P6\n%d %d\n255\n", hyp::vr::ui::W, hyp::vr::ui::H);
                    for (size_t i = 0; i + 3 < quad.pixels.size(); i += 4) std::fwrite(&quad.pixels[i], 1, 3, f);
                    std::fclose(f);
                }
            }
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
            updateInput(predictedTime);

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

        bool recordOverlay(RT64::RenderDevice* device, RT64::RenderCommandList* commandList) override {
            if (!shouldRender) return false;
            bool recorded = false;
            if (hyp::vr::ui::visible()) recorded |= uploadQuad(quad, device, commandList);
            if (dockVisible) recorded |= uploadQuad(dock, device, commandList);
            return recorded;
        }

        void endPresent(bool drawn) override {
            if (!frameBegun) return;
            frameBegun = false;
            std::scoped_lock lock(*queueMutex);
            releaseQuad(quad);
            releaseQuad(dock);
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
            XrCompositionLayerQuad panel{ XR_TYPE_COMPOSITION_LAYER_QUAD };
            XrCompositionLayerQuad button{ XR_TYPE_COMPOSITION_LAYER_QUAD };
            const XrCompositionLayerBaseHeader* layers[3] = { reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer) };
            uint32_t layerCount = 1;
            const bool showPanel = hyp::vr::ui::visible() && panelPlaced && quad.everReleased;
            if (showPanel) {
                panel.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                panel.space = localSpace;
                panel.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                panel.subImage.swapchain = quad.handle;
                panel.subImage.imageRect = { { 0, 0 }, { hyp::vr::ui::W, hyp::vr::ui::H } };
                panel.pose = panelPose;
                panel.size = { hyp::vr::ui::WIDTH_M, panelHeight };
                layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&panel);
            }
            if (dockVisible && dock.everReleased && !hyp::vr::ui::visible()) {
                button.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                button.space = localSpace;
                button.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                button.subImage.swapchain = dock.handle;
                button.subImage.imageRect = { { 0, 0 }, { dock.w, dock.h } };
                button.pose = dockPose;
                button.size = { hyp::vr::ui::DOCK_WIDTH_M, hyp::vr::ui::DOCK_WIDTH_M * dock.h / dock.w };
                layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&button);
            }
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
                fe.layerCount = layerCount;
                fe.layers = layers;
            }
            check(xrEndFrame(session, &fe), "xrEndFrame");
        }

        void releaseDeviceResources() override {
            running = false;
            swapchainsReady = false;
            destroyQuad(quad);
            destroyQuad(dock);
            for (auto& s : aimSpace) {
                if (s != XR_NULL_HANDLE) xrDestroySpace(s);
                s = XR_NULL_HANDLE;
            }
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

bool hyp::vr::get_n64_input(uint16_t* buttons, float* x, float* y, bool* panel_open) {
    if (g_xr == nullptr || !g_xr->running) return false;
    std::scoped_lock lock(g_xr->padMutex);
    *buttons = g_xr->n64Buttons;
    *x = g_xr->n64X;
    *y = g_xr->n64Y;
    *panel_open = g_xr->panelOpen;
    return true;
}

// ---------- bug reports ----------

namespace {
    const char* REPORT_HOST = "hyp-reports.pinball-leaderboard.workers.dev";

    std::string json_string(const std::string& s) {
        std::string out = "\"";
        for (unsigned char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
                    else out += (char)c;
            }
        }
        return out + "\"";
    }

    std::string report_json(const std::string& text) {
        return "{\"kind\":\"vr-bug\",\"version\":" + json_string(hyp::vr::version_string()) +
               ",\"device\":" + json_string(hyp::vr::device_string()) + ",\"text\":" + json_string(text) + "}";
    }

#ifdef __ANDROID__
    std::mutex report_mutex;
    std::vector<std::string> report_queue;     // Java (HypNative.takeReport) posts these
#endif
}

#ifdef _WIN32
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

void hyp::vr::send_report(const std::string& text) {
    const std::string body = report_json(text);
    std::thread([body]() {
        bool ok = false;
        std::string reply;
        HINTERNET s = WinHttpOpen(L"hyp-vr", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        std::wstring host(REPORT_HOST, REPORT_HOST + std::strlen(REPORT_HOST));
        HINTERNET c = s ? WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
        HINTERNET r = c ? WinHttpOpenRequest(c, L"POST", L"/report", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
        const wchar_t* headers = L"Content-Type: application/json\r\nX-HYP-App: hyp-android\r\n";
        if (r && WinHttpSendRequest(r, headers, (DWORD)-1L, (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) && WinHttpReceiveResponse(r, nullptr)) {
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(r, &avail) && avail > 0) {
                std::string chunk(avail, '\0');
                DWORD got = 0;
                WinHttpReadData(r, chunk.data(), avail, &got);
                reply.append(chunk.data(), got);
            }
            ok = (status == 200) && reply.find("\"ok\":true") != std::string::npos;
        }
        if (r) WinHttpCloseHandle(r);
        if (c) WinHttpCloseHandle(c);
        if (s) WinHttpCloseHandle(s);
        std::string id;
        size_t p = reply.find("\"id\":");
        if (p != std::string::npos) id = reply.substr(p + 5, reply.find_first_of(",}", p) - p - 5);
        std::printf("[vr] report %s %s\n", ok ? "sent" : "failed", reply.c_str());
        hyp::vr::ui::report_result(ok, ok ? "Report #" + id + " is in the inbox." : "No connection to the report inbox. Try again later.");
    }).detach();
}
#elif defined(__ANDROID__)
void hyp::vr::send_report(const std::string& text) {
    std::scoped_lock lock(report_mutex);
    report_queue.push_back(report_json(text));
}

std::string hyp::vr::take_report_json() {
    std::scoped_lock lock(report_mutex);
    if (report_queue.empty()) return {};
    std::string j = report_queue.front();
    report_queue.erase(report_queue.begin());
    return j;
}
#else
void hyp::vr::send_report(const std::string&) {
    hyp::vr::ui::report_result(false, "Reports aren't supported on this platform.");
}
#endif

std::string hyp::vr::version_string() {
#ifdef HYP_VERSION_NAME
    return std::string(HYP_VERSION_NAME) + " VR";
#else
    return std::string("PC VR dev (") + __DATE__ + ")";
#endif
}

std::string hyp::vr::device_string() {
    std::string runtime = "unknown runtime";
    if (g_xr != nullptr && g_xr->instance != XR_NULL_HANDLE) {
        XrInstanceProperties ip{ XR_TYPE_INSTANCE_PROPERTIES };
        if (XR_SUCCEEDED(xrGetInstanceProperties(g_xr->instance, &ip))) runtime = ip.runtimeName;
    }
#ifdef __ANDROID__
    return "Quest (" + runtime + ")";
#else
    return "PC (" + runtime + ")";
#endif
}

void hyp::vr::debug_input(const std::string& name, float seconds, float x, float y) {
    if (g_xr != nullptr) g_xr->debugInput(name, seconds, x, y);
}

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

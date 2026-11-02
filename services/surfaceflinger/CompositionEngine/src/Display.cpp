/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <android-base/stringprintf.h>
#include <sys/system_properties.h>

#include <cstdio>
#include <string_view>
#include <common/trace.h>
#include <compositionengine/CompositionEngine.h>
#include <compositionengine/CompositionRefreshArgs.h>
#include <compositionengine/DisplayCreationArgs.h>
#include <compositionengine/DisplaySurface.h>
#include <compositionengine/LayerFE.h>
#include <compositionengine/impl/Display.h>
#include <compositionengine/impl/DisplayColorProfile.h>
#include <compositionengine/impl/DumpHelpers.h>
#include <compositionengine/impl/OutputLayer.h>
#include <compositionengine/impl/RenderSurface.h>
#include <ui/DisplayId.h>

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"

#include "DisplayHardware/HWComposer.h"

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic pop // ignored "-Wconversion"

#include "PowerAdvisor/PowerAdvisor.h"

using aidl::android::hardware::graphics::composer3::Capability;
using aidl::android::hardware::graphics::composer3::DisplayCapability;

namespace android::compositionengine::impl {

std::shared_ptr<Display> createDisplay(
        const compositionengine::CompositionEngine& compositionEngine,
        const compositionengine::DisplayCreationArgs& args) {
    return createDisplayTemplated<Display>(compositionEngine, args);
}

Display::~Display() = default;

void Display::setConfiguration(const compositionengine::DisplayCreationArgs& args) {
    mIdVariant = args.idVariant;
    mPowerAdvisor = args.powerAdvisor;
    mMaxLayerPictureProfiles = args.maxLayerPictureProfiles;
    editState().isSecure = args.isSecure;
    editState().isProtected = args.isProtected;
    editState().displaySpace.setBounds(args.pixels);
    setName(args.name);
}

bool Display::isValid() const {
    return Output::isValid() && mPowerAdvisor;
}

DisplayId Display::getId() const {
    return asDisplayId(mIdVariant);
}

bool Display::hasSecureLayers() const {
    const auto layers = getOutputLayersOrderedByZ();
    return std::any_of(layers.begin(), layers.end(), [](const auto& layer) {
        const auto* state = layer->getLayerFE().getCompositionState();
        return state && (state->isSecure || state->hasProtectedContent);
    });
}

bool Display::isSecure() const {
    return getState().isSecure;
}

void Display::setSecure(bool secure) {
    editState().isSecure = secure;
}

bool Display::isVirtual() const {
    return !std::holds_alternative<PhysicalDisplayId>(mIdVariant);
}

ftl::Optional<DisplayId> Display::getDisplayId() const {
    return getId();
}

ftl::Optional<DisplayIdVariant> Display::getDisplayIdVariant() const {
    return mIdVariant;
}

void Display::disconnect() {
    if (mIsDisconnected) {
        return;
    }

    mIsDisconnected = true;

    if (const auto id = getDisplayIdVariant().and_then(asHalDisplayId<DisplayIdVariant>)) {
        getCompositionEngine().getHwComposer().disconnectDisplay(*id);
    }
}

namespace {
// IchthysOS: per-channel gains "r g b" from persist.ichthys.udfps_rgb, applied to the whole
// display only on frames that carry SystemUI's UDFPS mask layer ("VRI-UdfpsFodMask#N"). The
// panel's fingerprint mask HBM uses other colour tables than the normal brightness range (its
// white is greener); the matched dim fixes the light, these gains fix the colour, in the same
// commit as the dim and the HBM. Unset or invalid = identity. Read through the property's serial,
// so the per-frame cost is one atomic load.
mat4 ichthysUdfpsGains() {
    static const prop_info* info = nullptr;
    static uint32_t lastSerial = 0;
    static mat4 gains;
    if (!info) {
        info = __system_property_find("persist.ichthys.udfps_rgb");
        if (!info) return mat4();
        lastSerial = __system_property_serial(info) + 1;
    }
    const uint32_t serial = __system_property_serial(info);
    if (serial != lastSerial) {
        lastSerial = serial;
        __system_property_read_callback(
                info,
                [](void* cookie, const char*, const char* value, uint32_t) {
                    float r = 1.f, g = 1.f, b = 1.f;
                    mat4 m;
                    if (sscanf(value, "%f %f %f", &r, &g, &b) == 3 && r > 0.5f && r <= 1.f &&
                        g > 0.5f && g <= 1.f && b > 0.5f && b <= 1.f) {
                        m[0][0] = r;
                        m[1][1] = g;
                        m[2][2] = b;
                    }
                    *static_cast<mat4*>(cookie) = m;
                },
                &gains);
    }
    return gains;
}

bool ichthysHasUdfpsMask(const Output& output) {
    for (const auto* layer : output.getOutputLayersOrderedByZ()) {
        if (std::string_view(layer->getLayerFE().getDebugName()).find("UdfpsFodMask#") !=
            std::string_view::npos) {
            return true;
        }
    }
    return false;
}
} // namespace

void Display::setColorTransform(const compositionengine::CompositionRefreshArgs& args) {
    Output::setColorTransform(args);
    const auto halDisplayId = getDisplayIdVariant().and_then(asHalDisplayId<DisplayIdVariant>);
    if (mIsDisconnected || !halDisplayId) {
        return;
    }
    if (args.colorTransformMatrix) {
        mIchthysBaseColorTransform = *args.colorTransformMatrix;
    }
    // IchthysOS: the UDFPS mask colour gains on top of the system's colour transform.
    const mat4 gains = ichthysUdfpsGains();
    const bool tint = gains != mat4() && ichthysHasUdfpsMask(*this);
    const bool tintChanged =
            tint != mIchthysUdfpsTinted || (tint && gains != mIchthysAppliedGains);
    if (CC_LIKELY(!args.colorTransformMatrix && !tintChanged)) {
        return;
    }
    const mat4 base = mIchthysBaseColorTransform.value_or(getState().colorTransformMatrix);
    const mat4 effective = tint ? gains * base : base;
    mIchthysUdfpsTinted = tint;
    mIchthysAppliedGains = gains;
    if (getState().colorTransformMatrix != effective) {
        auto& state = editState();
        state.colorTransformMatrix = effective;
        state.dirtyRegion.set(state.displaySpace.getBoundsAsRect()); // as dirtyEntireOutput()
    }

    auto& hwc = getCompositionEngine().getHwComposer();
    status_t result = hwc.setColorTransform(*halDisplayId, effective);
    ALOGE_IF(result != NO_ERROR, "Failed to set color transform on display \"%s\": %d",
             to_string(*halDisplayId).c_str(), result);
}

void Display::setColorProfile(const ColorProfile& colorProfile) {
    if (colorProfile.mode == getState().colorMode &&
        colorProfile.dataspace == getState().dataspace &&
        colorProfile.renderIntent == getState().renderIntent) {
        return;
    }

    if (isVirtual()) {
        ALOGW("%s: Invalid operation on virtual display", __func__);
        return;
    }

    Output::setColorProfile(colorProfile);

    const auto physicalId = getDisplayIdVariant().and_then(asPhysicalDisplayId);
    LOG_FATAL_IF(!physicalId);
    getCompositionEngine().getHwComposer().setActiveColorMode(*physicalId, colorProfile.mode,
                                                              colorProfile.renderIntent);
}

void Display::dump(std::string& out) const {
    const char* const type = isVirtual() ? "virtual" : "physical";
    base::StringAppendF(&out, "Display %s (%s, \"%s\")", to_string(getId()).c_str(), type,
                        getName().c_str());

    out.append("\n   Composition Display State:\n");
    Output::dumpBase(out);
}

void Display::createDisplayColorProfile(const DisplayColorProfileCreationArgs& args) {
    setDisplayColorProfile(compositionengine::impl::createDisplayColorProfile(args));
}

void Display::createRenderSurface(const RenderSurfaceCreationArgs& args) {
    setRenderSurface(
            compositionengine::impl::createRenderSurface(getCompositionEngine(), *this, args));
}

void Display::createClientCompositionCache(uint32_t cacheSize) {
    cacheClientCompositionRequests(cacheSize);
}

std::unique_ptr<compositionengine::OutputLayer> Display::createOutputLayer(
        const sp<compositionengine::LayerFE>& layerFE) const {
    auto outputLayer = impl::createOutputLayer(*this, layerFE);

    if (const auto halDisplayId = getDisplayIdVariant().and_then(asHalDisplayId<DisplayIdVariant>);
        outputLayer && !mIsDisconnected && halDisplayId) {
        auto& hwc = getCompositionEngine().getHwComposer();
        auto hwcLayer = hwc.createLayer(*halDisplayId);
        ALOGE_IF(!hwcLayer, "Failed to create a HWC layer for a HWC supported display %s",
                 getName().c_str());
        outputLayer->setHwcLayer(std::move(hwcLayer));
    }
    return outputLayer;
}

void Display::setReleasedLayers(const compositionengine::CompositionRefreshArgs& refreshArgs) {
    Output::setReleasedLayers(refreshArgs);

    if (mIsDisconnected || isGpuVirtualDisplay() || refreshArgs.layersWithQueuedFrames.empty()) {
        return;
    }

    // For layers that are being removed from a HWC display, and that have
    // queued frames, add them to a a list of released layers so we can properly
    // set a fence.
    compositionengine::Output::ReleasedLayers releasedLayers;

    // Any non-null entries in the current list of layers are layers that are no
    // longer going to be visible
    for (auto* outputLayer : getOutputLayersOrderedByZ()) {
        if (!outputLayer) {
            continue;
        }

        compositionengine::LayerFE* layerFE = &outputLayer->getLayerFE();
        const bool hasQueuedFrames =
                std::any_of(refreshArgs.layersWithQueuedFrames.cbegin(),
                            refreshArgs.layersWithQueuedFrames.cend(),
                            [layerFE](sp<compositionengine::LayerFE> layerWithQueuedFrames) {
                                return layerFE == layerWithQueuedFrames.get();
                            });

        if (hasQueuedFrames) {
            releasedLayers.emplace_back(wp<LayerFE>::fromExisting(layerFE));
        }
    }

    setReleasedLayers(std::move(releasedLayers));
}

void Display::applyDisplayBrightness(bool applyImmediately) {
    if (!getState().displayBrightness) {
        return;
    }
    if (auto displayId = getDisplayIdVariant().and_then(asPhysicalDisplayId)) {
        auto& hwc = getCompositionEngine().getHwComposer();
        status_t result = hwc.setDisplayBrightness(*displayId, *getState().displayBrightness,
                                                   getState().displayBrightnessNits,
                                                   Hwc2::Composer::DisplayBrightnessOptions{
                                                           .applyImmediately = applyImmediately})
                                  .get();
        ALOGE_IF(result != NO_ERROR, "setDisplayBrightness failed for %s: %d, (%s)",
                 getName().c_str(), result, strerror(-result));
    }
    // Clear out the display brightness now that it's been communicated to composer.
    editState().displayBrightness.reset();
}

void Display::beginFrame() {
    Output::beginFrame();

    // If we don't have a HWC display, then we are done.
    const auto halDisplayId = getDisplayIdVariant().and_then(asHalDisplayId<DisplayIdVariant>);
    if (!halDisplayId) {
        return;
    }

    applyDisplayBrightness(false);
}

bool Display::chooseCompositionStrategy(
        std::optional<android::HWComposer::DeviceRequestedChanges>* outChanges) {
    SFTRACE_FORMAT("%s for %s", __func__, getNamePlusId().c_str());
    ALOGV(__FUNCTION__);

    if (mIsDisconnected) {
        return false;
    }

    // If we don't have a HWC display, then we are done.
    const auto halDisplayId = getDisplayIdVariant().and_then(asHalDisplayId<DisplayIdVariant>);
    if (!halDisplayId) {
        return false;
    }

    // Get any composition changes requested by the HWC device, and apply them.
    auto& hwc = getCompositionEngine().getHwComposer();
    const bool requiresClientComposition = anyLayersRequireClientComposition();

    const TimePoint hwcValidateStartTime = TimePoint::now();

    const auto physicalDisplayId = getDisplayIdVariant().and_then(asPhysicalDisplayId);

    if (physicalDisplayId && getState().readbackBuffer) {
        hwc.setReadbackBuffer(*physicalDisplayId, getState().readbackBuffer, Fence::NO_FENCE);
    }

    if (status_t result = hwc.getDeviceCompositionChanges(*halDisplayId, requiresClientComposition,
                                                          getState().earliestPresentTime,
                                                          getState().expectedPresentTime,
                                                          getState().frameInterval, outChanges);
        result != NO_ERROR) {
        ALOGE("chooseCompositionStrategy failed for %s: %d (%s)", getName().c_str(), result,
              strerror(-result));
        return false;
    }

    if (isPowerHintSessionEnabled()) {
        mPowerAdvisor->setHwcValidateTiming(getId(), hwcValidateStartTime, TimePoint::now());
        if (auto halDisplayId = getDisplayIdVariant().and_then(asHalDisplayId<DisplayIdVariant>)) {
            mPowerAdvisor->setSkippedValidate(*halDisplayId, hwc.getValidateSkipped(*halDisplayId));
        }
    }

    return true;
}

void Display::applyCompositionStrategy(const std::optional<DeviceRequestedChanges>& changes) {
    if (changes) {
        applyChangedTypesToLayers(changes->changedTypes);
        applyDisplayRequests(changes->displayRequests);
        applyLayerRequestsToLayers(changes->layerRequests);
        applyClientTargetRequests(changes->clientTargetProperty);
        applyLayerLutsToLayers(changes->layerLuts);
    }

    // Determine what type of composition we are doing from the final state
    auto& state = editState();
    state.usesClientComposition = anyLayersRequireClientComposition();
    state.usesDeviceComposition = !allLayersRequireClientComposition();
}

bool Display::getSkipColorTransform() const {
    auto& hwc = getCompositionEngine().getHwComposer();
    if (auto halDisplayId = getDisplayIdVariant().and_then(asHalDisplayId<DisplayIdVariant>)) {
        return hwc.hasDisplayCapability(*halDisplayId,
                                        DisplayCapability::SKIP_CLIENT_COLOR_TRANSFORM);
    }

    return Output::getSkipColorTransform();
}

bool Display::allLayersRequireClientComposition() const {
    const auto layers = getOutputLayersOrderedByZ();
    return std::all_of(layers.begin(), layers.end(),
                       [](const auto& layer) { return layer->requiresClientComposition(); });
}

void Display::applyChangedTypesToLayers(const ChangedTypes& changedTypes) {
    if (changedTypes.empty()) {
        return;
    }

    for (auto* layer : getOutputLayersOrderedByZ()) {
        auto hwcLayer = layer->getHwcLayer();
        if (!hwcLayer) {
            continue;
        }

        if (auto it = changedTypes.find(hwcLayer); it != changedTypes.end()) {
            layer->applyDeviceCompositionTypeChange(
                    static_cast<aidl::android::hardware::graphics::composer3::Composition>(
                            it->second));
        }
    }
}

void Display::applyDisplayRequests(const DisplayRequests& displayRequests) {
    auto& state = editState();
    state.flipClientTarget = (static_cast<uint32_t>(displayRequests) &
                              static_cast<uint32_t>(hal::DisplayRequest::FLIP_CLIENT_TARGET)) != 0;
    // Note: HWC2::DisplayRequest::WriteClientTargetToOutput is currently ignored.
}

void Display::applyLayerRequestsToLayers(const LayerRequests& layerRequests) {
    for (auto* layer : getOutputLayersOrderedByZ()) {
        layer->prepareForDeviceLayerRequests();

        auto hwcLayer = layer->getHwcLayer();
        if (!hwcLayer) {
            continue;
        }

        if (auto it = layerRequests.find(hwcLayer); it != layerRequests.end()) {
            layer->applyDeviceLayerRequest(
                    static_cast<Hwc2::IComposerClient::LayerRequest>(it->second));
        }
    }
}

void Display::applyClientTargetRequests(const ClientTargetProperty& clientTargetProperty) {
    if (static_cast<ui::Dataspace>(clientTargetProperty.clientTargetProperty.dataspace) ==
        ui::Dataspace::UNKNOWN) {
        return;
    }

    editState().dataspace =
            static_cast<ui::Dataspace>(clientTargetProperty.clientTargetProperty.dataspace);
    editState().clientTargetBrightness = clientTargetProperty.brightness;
    editState().clientTargetDimmingStage = clientTargetProperty.dimmingStage;
    getRenderSurface()->setBufferDataspace(editState().dataspace);
    getRenderSurface()->setBufferPixelFormat(
            static_cast<ui::PixelFormat>(clientTargetProperty.clientTargetProperty.pixelFormat));
}

void Display::applyLayerLutsToLayers(const LayerLuts& layerLuts) {
    auto& mapper = getCompositionEngine().getHwComposer().getLutFileDescriptorMapper();
    for (auto* layer : getOutputLayersOrderedByZ()) {
        auto hwcLayer = layer->getHwcLayer();
        if (!hwcLayer) {
            continue;
        }

        if (auto lutsIt = layerLuts.find(hwcLayer); lutsIt != layerLuts.end()) {
            if (auto mapperIt = mapper.find(hwcLayer); mapperIt != mapper.end()) {
                if (mapperIt->second.ok()) {
                    layer->applyDeviceLayerLut(::android::base::unique_fd(
                                                       mapperIt->second.release()),
                                               lutsIt->second);
                } else {
                    layer->applyDeviceLayerLut(::android::base::unique_fd(), lutsIt->second);
                }
            }
        }
    }

    mapper.clear();
}

void Display::executeCommands() {
    const auto halDisplayIdOpt = getDisplayIdVariant().and_then(asHalDisplayId<DisplayIdVariant>);
    if (mIsDisconnected || !halDisplayIdOpt) {
        return;
    }

    getCompositionEngine().getHwComposer().executeCommands(*halDisplayIdOpt);
}

compositionengine::Output::FrameFences Display::presentFrame() {
    auto fences = impl::Output::presentFrame();

    const auto halDisplayIdOpt = getDisplayIdVariant().and_then(asHalDisplayId<DisplayIdVariant>);
    if (mIsDisconnected || !halDisplayIdOpt) {
        return fences;
    }

    auto& hwc = getCompositionEngine().getHwComposer();

    const TimePoint startTime = TimePoint::now();

    if (isPowerHintSessionEnabled() && getState().earliestPresentTime) {
        mPowerAdvisor->setHwcPresentDelayedTime(*halDisplayIdOpt, *getState().earliestPresentTime);
    }

    hwc.presentAndGetReleaseFences(*halDisplayIdOpt, getState().earliestPresentTime);

    if (isPowerHintSessionEnabled()) {
        mPowerAdvisor->setHwcPresentTiming(*halDisplayIdOpt, startTime, TimePoint::now());
    }

    fences.presentFence = hwc.getPresentFence(*halDisplayIdOpt);

    // TODO(b/121291683): Change HWComposer call to return entire map
    for (const auto* layer : getOutputLayersOrderedByZ()) {
        auto hwcLayer = layer->getHwcLayer();
        if (!hwcLayer) {
            continue;
        }

        fences.layerFences.emplace(hwcLayer, hwc.getLayerReleaseFence(*halDisplayIdOpt, hwcLayer));
    }

    hwc.clearReleaseFences(*halDisplayIdOpt);

    return fences;
}

void Display::setExpensiveRenderingExpected(bool enabled) {
    Output::setExpensiveRenderingExpected(enabled);

    if (mPowerAdvisor && !isGpuVirtualDisplay()) {
        mPowerAdvisor->setExpensiveRenderingExpected(getId(), enabled);
    }
}

bool Display::isPowerHintSessionEnabled() {
    return getCompositionEngine().isPowerHintSessionEnabled() && mPowerAdvisor != nullptr &&
            mPowerAdvisor->usePowerHintSession();
}

bool Display::isPowerHintSessionGpuReportingEnabled() {
    return mPowerAdvisor != nullptr && mPowerAdvisor->supportsGpuReporting();
}

// For ADPF GPU v0 this is expected to set start time to when the GPU commands are submitted with
// fence returned, i.e. when RenderEngine flushes the commands and returns the draw fence.
void Display::setHintSessionGpuStart(TimePoint startTime) {
    mPowerAdvisor->setGpuStartTime(getId(), startTime);
}

void Display::setHintSessionGpuFence(std::unique_ptr<FenceTime>&& gpuFence) {
    mPowerAdvisor->setGpuFenceTime(getId(), std::move(gpuFence));
}

void Display::setHintSessionRequiresRenderEngine(bool requiresRenderEngine) {
    mPowerAdvisor->setRequiresRenderEngine(getId(), requiresRenderEngine);
}

const aidl::android::hardware::graphics::composer3::OverlayProperties*
Display::getOverlaySupport() {
    return &getCompositionEngine().getHwComposer().getOverlaySupport();
}

bool Display::hasPictureProcessing() const {
    const auto halDisplayIdOpt = getDisplayIdVariant().and_then(asHalDisplayId<DisplayIdVariant>);
    if (!halDisplayIdOpt) {
        return false;
    }
    return getCompositionEngine().getHwComposer().hasDisplayCapability(
            *halDisplayIdOpt, DisplayCapability::PICTURE_PROCESSING);
}

int32_t Display::getMaxLayerPictureProfiles() const {
    return mMaxLayerPictureProfiles;
}

void Display::finishFrame(GpuCompositionResult&& result) {
    // We only need to actually compose the display if:
    // 1) It is being handled by hardware composer, which may need this to
    //    keep its virtual display state machine in sync, or
    // 2) There is work to be done (the dirty region isn't empty)
    if (isGpuVirtualDisplay() && !mustRecompose()) {
        ALOGV("Skipping display composition");
        return;
    }

    impl::Output::finishFrame(std::move(result));
}

bool Display::supportsOffloadPresent() const {
    if (auto halDisplayId = getDisplayIdVariant().and_then(asHalDisplayId<DisplayIdVariant>)) {
        auto& hwc = getCompositionEngine().getHwComposer();
        return hwc.hasDisplayCapability(*halDisplayId, DisplayCapability::MULTI_THREADED_PRESENT);
    }

    return false;
}

} // namespace android::compositionengine::impl

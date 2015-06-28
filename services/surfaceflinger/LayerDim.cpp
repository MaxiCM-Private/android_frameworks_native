/*
 * Copyright (C) 2007 The Android Open Source Project
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

#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>

#include <utils/Errors.h>
#include <utils/Log.h>

#include <ui/GraphicBuffer.h>

#include "LayerDim.h"
#include "SurfaceFlinger.h"
#include "DisplayDevice.h"
#include "RenderEngine/RenderEngine.h"

namespace android {
// ---------------------------------------------------------------------------

LayerDim::LayerDim(SurfaceFlinger* flinger, const sp<Client>& client,
        const String8& name, uint32_t w, uint32_t h, uint32_t flags)
    : Layer(flinger, client, name, w, h, flags) {
}

LayerDim::~LayerDim() {
}

void LayerDim::onDraw(const sp<const DisplayDevice>& hw, const Region& clip) const
{
    const State& s(getDrawingState());
    if (s.alpha>0) {
        Mesh mesh(Mesh::TRIANGLE_FAN, 4, 2);
        computeGeometry(hw, mesh);
        RenderEngine& engine(mFlinger->getRenderEngine());
        engine.setupDimLayerBlending(s.alpha);
        engine.drawMesh(mesh);
        engine.disableBlending();
    }
}

bool LayerDim::isVisible() const {
    const Layer::State& s(getDrawingState());
    return !(s.flags & layer_state_t::eLayerHidden) && s.alpha;
}

void LayerDim::setGeometry(const sp<const DisplayDevice>& hw,
            HWComposer::HWCLayerInterface& layer)
{
    layer.setDefaultState();

    layer.setSkip(false);

    // this gives us only the "orientation" component of the transform
    const State& s(getDrawingState());
    const uint32_t finalTransform = s.transform.getOrientation();
    // we can only handle simple transformation
    if (finalTransform & Transform::ROT_INVALID) {
        layer.setTransform(0);
    } else {
        layer.setTransform(finalTransform);
    }

    // Dim. use higher 16 bit to store alpha
    layer.setBlending(HWC_BLENDING_DIM | (getDrawingState().alpha << 16));

    const Transform& tr = hw->getTransform();
    Rect transformedBounds(computeBounds());
    transformedBounds = tr.transform(transformedBounds);

    // scaling is already applied in transformedBounds
    layer.setFrame(transformedBounds);
    layer.setVisibleRegionScreen(tr.transform(visibleRegion));
}

void LayerDim::setPerFrameData(const sp<const DisplayDevice>& hw,
            HWComposer::HWCLayerInterface& layer) {
    Rect crop;
    crop.clear();

    layer.setCrop(crop);
    layer.setBuffer(NULL);


// ---------------------------------------------------------------------------

}; // namespace android

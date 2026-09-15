// Copyright (C) 2015 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "FbConfig.h"

#include "EGLDispatch.h"
#include "GraphicsDiagnostics.h"

#include <stdio.h>
#include <string.h>
#include <vector>

namespace {

#define E(...)  fprintf(stderr, __VA_ARGS__)

const GLuint kConfigAttributes[] = {
    EGL_DEPTH_SIZE,     // must be first - see getDepthSize()
    EGL_STENCIL_SIZE,   // must be second - see getStencilSize()
    EGL_RENDERABLE_TYPE,// must be third - see getRenderableType()
    EGL_SURFACE_TYPE,   // must be fourth - see getSurfaceType()
    EGL_CONFIG_ID,      // must be fifth  - see chooseConfig()
    EGL_BUFFER_SIZE,
    EGL_ALPHA_SIZE,
    EGL_BLUE_SIZE,
    EGL_GREEN_SIZE,
    EGL_RED_SIZE,
    EGL_CONFIG_CAVEAT,
    EGL_LEVEL,
    EGL_MAX_PBUFFER_HEIGHT,
    EGL_MAX_PBUFFER_PIXELS,
    EGL_MAX_PBUFFER_WIDTH,
    EGL_NATIVE_RENDERABLE,
    EGL_NATIVE_VISUAL_ID,
    EGL_NATIVE_VISUAL_TYPE,
    EGL_SAMPLES,
    EGL_SAMPLE_BUFFERS,
    EGL_TRANSPARENT_TYPE,
    EGL_TRANSPARENT_BLUE_VALUE,
    EGL_TRANSPARENT_GREEN_VALUE,
    EGL_TRANSPARENT_RED_VALUE,
    EGL_BIND_TO_TEXTURE_RGB,
    EGL_BIND_TO_TEXTURE_RGBA,
    EGL_MIN_SWAP_INTERVAL,
    EGL_MAX_SWAP_INTERVAL,
    EGL_LUMINANCE_SIZE,
    EGL_ALPHA_MASK_SIZE,
    EGL_COLOR_BUFFER_TYPE,
    //EGL_MATCH_NATIVE_PIXMAP,
    EGL_CONFORMANT
};

const size_t kConfigAttributesLen =
        sizeof(kConfigAttributes) / sizeof(kConfigAttributes[0]);

bool isCompatibleHostConfig(EGLConfig config, EGLDisplay display) {
    // Filter out configs which do not support pbuffers, since they
    // are used to implement window surfaces.
    EGLint surfaceType;
    s_egl.eglGetConfigAttrib(
            display, config, EGL_SURFACE_TYPE, &surfaceType);
    if (!(surfaceType & EGL_PBUFFER_BIT)) {
        return false;
    }

    // Filter out configs that do not support RGB pixel values.
    EGLint redSize = 0, greenSize = 0, blueSize = 0;
    s_egl.eglGetConfigAttrib(
            display, config,EGL_RED_SIZE, &redSize);
    s_egl.eglGetConfigAttrib(
            display, config, EGL_GREEN_SIZE, &greenSize);
    s_egl.eglGetConfigAttrib(
            display, config, EGL_BLUE_SIZE, &blueSize);
    if (!redSize || !greenSize || !blueSize) {
        return false;
    }

    return true;
}

}  // namespace

FbConfig::~FbConfig() {
    delete [] mAttribValues;
}

FbConfig::FbConfig(EGLConfig hostConfig, EGLDisplay hostDisplay) :
        mEglConfig(hostConfig), mAttribValues(NULL) {
    mAttribValues = new GLint[kConfigAttributesLen];
    for (size_t i = 0; i < kConfigAttributesLen; ++i) {
        mAttribValues[i] = 0;
        s_egl.eglGetConfigAttrib(hostDisplay,
                                 hostConfig,
                                 kConfigAttributes[i],
                                 &mAttribValues[i]);

        // This implementation supports guest window surfaces by wrapping
        // them around host Pbuffers, so report EGL_WINDOW_BIT. ANGLE Metal's
        // pbuffer surface does not implement preserved swap semantics, however,
        // so never advertise EGL_SWAP_BEHAVIOR_PRESERVED_BIT to the guest.
        if (kConfigAttributes[i] == EGL_SURFACE_TYPE) {
            mAttribValues[i] |= EGL_WINDOW_BIT;
            if (aeGraphicsDiagEnabled("AE_DIAG_ADVERTISE_PRESERVED")) {
                mAttribValues[i] |= EGL_SWAP_BEHAVIOR_PRESERVED_BIT;
            } else {
                mAttribValues[i] &= ~EGL_SWAP_BEHAVIOR_PRESERVED_BIT;
            }
        }
    }
}

FbConfigList::FbConfigList(EGLDisplay display, bool hasGLES1) :
        mCount(0), mConfigs(NULL), mDisplay(display) {
    if (display == EGL_NO_DISPLAY) {
        E("%s: Invalid display value %p (EGL_NO_DISPLAY)\n",
          __FUNCTION__, (void*)display);
        return;
    }

    EGLint numHostConfigs = 0;
    if (!s_egl.eglGetConfigs(display, NULL, 0, &numHostConfigs)) {
        E("%s: Could not get number of host EGL configs\n", __FUNCTION__);
        return;
    }
    EGLConfig* hostConfigs = new EGLConfig[numHostConfigs];
    s_egl.eglGetConfigs(display, hostConfigs, numHostConfigs, &numHostConfigs);

    mConfigs = new FbConfig*[numHostConfigs];
    for (EGLint i = 0;  i < numHostConfigs; ++i) {
        // Filter out configs that are not compatible with our implementation.
        if (!isCompatibleHostConfig(hostConfigs[i], display)) {
            continue;
        }
        mConfigs[mCount] = new FbConfig(hostConfigs[i], display);
        if (!hasGLES1) {
            mConfigs[mCount]->mAttribValues[2] &= ~EGL_OPENGL_ES_BIT;
            mConfigs[mCount]->mAttribValues[kConfigAttributesLen - 1] &= ~EGL_OPENGL_ES_BIT;
        }
        mCount++;
    }

    delete [] hostConfigs;
}

FbConfigList::~FbConfigList() {
    for (int n = 0; n < mCount; ++n) {
        delete mConfigs[n];
    }
    delete [] mConfigs;
}

int FbConfigList::chooseConfig(const EGLint* attribs,
                               EGLint* configs,
                               EGLint configsSize) const {
    EGLint numHostConfigs = 0;
    if (!s_egl.eglGetConfigs(mDisplay, NULL, 0, &numHostConfigs)) {
        E("%s: Could not get number of host EGL configs\n", __FUNCTION__);
        return 0;
    }

    std::vector<EGLint> hostAttribs;
    EGLint requestedSurfaceType = EGL_DONT_CARE;
    EGLint requestedAPI = EGL_OPENGL_ES_BIT;
    bool hasSurfaceType = false;

    if (attribs) {
        for (const EGLint* a = attribs; a[0] != EGL_NONE; a += 2) {
            const EGLint name = a[0];
            const EGLint value = a[1];

            hostAttribs.push_back(name);
            if (name == EGL_SURFACE_TYPE) {
                hasSurfaceType = true;
                requestedSurfaceType = value;

                // The guest's WINDOW surface is implemented by a host pbuffer.
                // Translate only that bit. Keep PRESERVED and every other bit
                // so unsupported semantics correctly cause chooseConfig to fail.
                if (value == EGL_DONT_CARE) {
                    hostAttribs.push_back(EGL_PBUFFER_BIT);
                } else {
                    EGLint translated = value;
                    translated &= ~EGL_WINDOW_BIT;
                    translated |= EGL_PBUFFER_BIT;
                    if (aeGraphicsDiagEnabled("AE_DIAG_ADVERTISE_PRESERVED")) {
                        // Diagnostic legacy behavior: let the host choose a plain
                        // pbuffer while telling the guest the contents are preserved.
                        translated &= ~EGL_SWAP_BEHAVIOR_PRESERVED_BIT;
                    }
                    hostAttribs.push_back(translated);
                }
            } else {
                hostAttribs.push_back(value);
                if (name == EGL_RENDERABLE_TYPE) {
                    requestedAPI = value;
                }
            }
        }
    }

    if (!hasSurfaceType) {
        hostAttribs.push_back(EGL_SURFACE_TYPE);
        hostAttribs.push_back(EGL_PBUFFER_BIT);
    }
    hostAttribs.push_back(EGL_NONE);

    std::vector<EGLConfig> matchedConfigs(
            static_cast<size_t>(numHostConfigs));
    EGLint matchedCount = 0;
    if (!s_egl.eglChooseConfig(mDisplay,
                               hostAttribs.data(),
                               matchedConfigs.data(),
                               numHostConfigs,
                               &matchedCount)) {
        return 0;
    }

    int result = 0;
    for (EGLint n = 0; n < matchedCount; ++n) {
        if (configs && configsSize > 0 && result >= configsSize) {
            break;
        }
        if (!isCompatibleHostConfig(matchedConfigs[n], mDisplay)) {
            continue;
        }

        EGLint hostConfigId = 0;
        if (!s_egl.eglGetConfigAttrib(
                mDisplay, matchedConfigs[n], EGL_CONFIG_ID, &hostConfigId)) {
            continue;
        }

        for (int k = 0; k < mCount; ++k) {
            FbConfig* guest = mConfigs[k];
            if (guest->getConfigId() != hostConfigId) {
                continue;
            }
            if (requestedAPI != EGL_DONT_CARE &&
                (guest->getRenderableType() & requestedAPI) != requestedAPI) {
                continue;
            }
            if (requestedSurfaceType != EGL_DONT_CARE &&
                (static_cast<EGLint>(guest->getSurfaceType()) &
                 requestedSurfaceType) != requestedSurfaceType) {
                continue;
            }

            if (configs && result < configsSize) {
                configs[result] = static_cast<EGLint>(k);
            }
            ++result;
            break;
        }
    }

    return result;
}


void FbConfigList::getPackInfo(EGLint* numConfigs,
                               EGLint* numAttributes) const {
    if (numConfigs) {
        *numConfigs = mCount;
    }
    if (numAttributes) {
        *numAttributes = static_cast<EGLint>(kConfigAttributesLen);
    }
}

EGLint FbConfigList::packConfigs(GLuint bufferByteSize, GLuint* buffer) const {
    GLuint numAttribs = static_cast<GLuint>(kConfigAttributesLen);
    GLuint kGLuintSize = static_cast<GLuint>(sizeof(GLuint));
    GLuint neededByteSize = (mCount + 1) * numAttribs * kGLuintSize;
    if (!buffer || bufferByteSize < neededByteSize) {
        return -neededByteSize;
    }
    // Write to the buffer the config attribute ids, followed for each one
    // of the configs, their values.
    memcpy(buffer, kConfigAttributes, kConfigAttributesLen * kGLuintSize);

    for (int i = 0; i < mCount; ++i) {
        memcpy(buffer + (i + 1) * kConfigAttributesLen,
               mConfigs[i]->mAttribValues,
               kConfigAttributesLen * kGLuintSize);
    }
    return mCount;
}

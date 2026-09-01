// vim: noai:ts=4:sw=4:softtabstop=4:expandtab
#include "eglvid.h"

#include "path.h"
#include "streaming/session.h"
#include "streaming/streamutils.h"

#include <QDir>

#include <Limelight.h>
#include <unistd.h>

#include <algorithm>

#include <SDL3/SDL_render.h>
#include <SDL3/SDL_system.h>

// These are extensions, so some platform headers may not provide them
#ifndef EGL_PLATFORM_WAYLAND_KHR
#define EGL_PLATFORM_WAYLAND_KHR 0x31D8
#endif
#ifndef EGL_PLATFORM_X11_KHR
#define EGL_PLATFORM_X11_KHR 0x31D5
#endif
#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif
#ifndef GL_UNPACK_ROW_LENGTH_EXT
#define GL_UNPACK_ROW_LENGTH_EXT 0x0CF2
#endif

typedef struct _OVERLAY_VERTEX
{
    float x, y;
    float u, v;
} OVERLAY_VERTEX, *POVERLAY_VERTEX;

/* TODO:
 *  - handle more pixel formats
 *  - handle software decoding
 */

/* DOC/misc:
 *  - https://kernel-recipes.org/en/2016/talks/video-and-colorspaces/
 *  - http://www.brucelindbloom.com/
 *  - https://learnopengl.com/Getting-started/Shaders
 *  - https://github.com/stunpix/yuvit
 *  - https://en.wikipedia.org/wiki/YCbCr#ITU-R_BT.601_conversion
 *  - https://www.renesas.com/eu/en/www/doc/application-note/an9717.pdf
 *  - https://www.xilinx.com/support/documentation/application_notes/xapp283.pdf
 *  - https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.709-6-201506-I!!PDF-E.pdf
 *  - https://www.khronos.org/registry/OpenGL/extensions/OES/OES_EGL_image_external.txt
 *  - https://gist.github.com/rexguo/6696123
 *  - https://wiki.libsdl.org/CategoryVideo
 */

#define EGL_LOG(Category, ...) SDL_Log ## Category(\
        SDL_LOG_CATEGORY_APPLICATION, \
        "EGLRenderer: " __VA_ARGS__)

SDL_Window* EGLRenderer::s_LastFailedWindow = nullptr;
int EGLRenderer::s_LastFailedVideoFormat = 0;

EGLRenderer::EGLRenderer(IFFmpegRenderer *backendRenderer)
    :
        m_EGLImagePixelFormat(AV_PIX_FMT_NONE),
        m_EGLDisplay(EGL_NO_DISPLAY),
        m_Textures{0},
        m_OverlayTextures{0},
        m_OverlayVbos{0},
        m_OverlayHasValidData{},
        m_ShaderProgram(0),
        m_OverlayShaderProgram(0),
        m_Context(0),
        m_Window(nullptr),
        m_Backend(backendRenderer),
        m_VAO(0),
        m_VideoVbo(0),
        m_BlockingSwapBuffers(false),
        m_LastRenderSync(EGL_NO_SYNC),
        m_LastFrame(av_frame_alloc()),
        m_glEGLImageTargetTexture2DOES(nullptr),
        m_glGenVertexArraysOES(nullptr),
        m_glBindVertexArrayOES(nullptr),
        m_glDeleteVertexArraysOES(nullptr),
        m_eglCreateSync(nullptr),
        m_eglCreateSyncKHR(nullptr),
        m_eglDestroySync(nullptr),
        m_eglClientWaitSync(nullptr),
        m_GlesMajorVersion(0),
        m_GlesMinorVersion(0),
        m_HasExtUnpackSubimage(false),
        m_IdentityGbr8Bit(false)
{
    SDL_assert(backendRenderer);
    SDL_assert(backendRenderer->canExportEGL());

    // Save these global parameters so we can restore them in our destructor
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &m_OldContextProfileMask);
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &m_OldContextMajorVersion);
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &m_OldContextMinorVersion);
}

EGLRenderer::~EGLRenderer()
{
    if (m_Context) {
        // Reattach the GL context to the main thread for destruction
        SDL_GL_MakeCurrent(m_Window, m_Context);
        if (m_LastRenderSync != EGL_NO_SYNC) {
            SDL_assert(m_eglDestroySync != nullptr);
            m_eglDestroySync(m_EGLDisplay, m_LastRenderSync);
        }
        if (m_ShaderProgram) {
            glDeleteProgram(m_ShaderProgram);
        }
        if (m_OverlayShaderProgram) {
            glDeleteProgram(m_OverlayShaderProgram);
        }
        if (m_VAO) {
            SDL_assert(m_glDeleteVertexArraysOES != nullptr);
            m_glDeleteVertexArraysOES(1, &m_VAO);
        }
        if (m_VideoVbo) {
            glDeleteBuffers(1, &m_VideoVbo);
        }
        for (int i = 0; i < EGL_MAX_PLANES; i++) {
            if (m_Textures[i] != 0) {
                glDeleteTextures(1, &m_Textures[i]);
            }
        }
        for (int i = 0; i < Overlay::OverlayMax; i++) {
            if (m_OverlayTextures[i] != 0) {
                glDeleteTextures(1, &m_OverlayTextures[i]);
            }
            if (m_OverlayVbos[i] != 0) {
                glDeleteBuffers(1, &m_OverlayVbos[i]);
            }
        }
        SDL_GL_DestroyContext(m_Context);
    }

    for (SDL_Renderer* renderer : m_DummyRenderers) {
        SDL_DestroyRenderer(renderer);
    }

    av_frame_free(&m_LastFrame);

    // Reset the global properties back to what they were before
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "0");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, m_OldContextProfileMask);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, m_OldContextMajorVersion);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, m_OldContextMinorVersion);
}

bool EGLRenderer::prepareDecoderContext(AVCodecContext*, AVDictionary**)
{
    /* Nothing to do */

    EGL_LOG(Info, "Using EGL renderer");

    return true;
}

void EGLRenderer::notifyOverlayUpdated(Overlay::OverlayType type)
{
    // We handle uploading the updated overlay texture in renderOverlay().
    // notifyOverlayUpdated() is called on an arbitrary thread, which may
    // not be have the OpenGL context current on it.

    if (!Session::get()->getOverlayManager().isOverlayEnabled(type)) {
        // If the overlay has been disabled, mark the data as invalid/stale.
        SDL_SetAtomicInt(&m_OverlayHasValidData[type], 0);
        return;
    }
}

bool EGLRenderer::notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info)
{
    // We can transparently handle size and display changes
    return !(info->stateChangeFlags & ~(WINDOW_STATE_CHANGE_SIZE | WINDOW_STATE_CHANGE_DISPLAY));
}

bool EGLRenderer::isPixelFormatSupported(int videoFormat, AVPixelFormat pixelFormat)
{
    // Pixel format support should be determined by the backend renderer
    return m_Backend->isPixelFormatSupported(videoFormat, pixelFormat);
}

AVPixelFormat EGLRenderer::getPreferredPixelFormat(int videoFormat)
{
    // Pixel format preference should be determined by the backend renderer
    return m_Backend->getPreferredPixelFormat(videoFormat);
}

int EGLRenderer::getDecoderColorspace()
{
    return m_Backend->getDecoderColorspace();
}

int EGLRenderer::getDecoderColorRange()
{
    return m_Backend->getDecoderColorRange();
}

void EGLRenderer::renderOverlay(Overlay::OverlayType type, int viewportWidth, int viewportHeight)
{
    // Do nothing if this overlay is disabled
    if (!Session::get()->getOverlayManager().isOverlayEnabled(type)) {
        return;
    }

    // Upload a new overlay texture if needed
    SDL_Surface* newSurface = Session::get()->getOverlayManager().getUpdatedOverlaySurface(type);
    if (newSurface != nullptr) {
        SDL_assert(!SDL_MUSTLOCK(newSurface));
        SDL_assert(newSurface->format == SDL_PIXELFORMAT_ARGB8888);

        const SDL_PixelFormatDetails* formatDetails =
            SDL_GetPixelFormatDetails(newSurface->format);
        SDL_assert(formatDetails != nullptr);
        const int bytesPerPixel = formatDetails->bytes_per_pixel;

        glBindTexture(GL_TEXTURE_2D, m_OverlayTextures[type]);

        void* packedPixelData = nullptr;
        if (m_GlesMajorVersion >= 3 || m_HasExtUnpackSubimage) {
            // If we are GLES 3.0+ or have GL_EXT_unpack_subimage, GL can handle any pitch
            SDL_assert(newSurface->pitch % bytesPerPixel == 0);
            glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, newSurface->pitch / bytesPerPixel);
        }
        else if (newSurface->pitch != newSurface->w * bytesPerPixel) {
            // If we can't use GL_UNPACK_ROW_LENGTH and the surface isn't tightly packed,
            // we must allocate a tightly packed buffer and copy our pixels there.
            packedPixelData = malloc(newSurface->w * newSurface->h * bytesPerPixel);
            if (!packedPixelData) {
                SDL_DestroySurface(newSurface);
                return;
            }

            SDL_ConvertPixels(newSurface->w, newSurface->h,
                              newSurface->format, newSurface->pixels, newSurface->pitch,
                              newSurface->format, packedPixelData, newSurface->w * bytesPerPixel);
        }

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, newSurface->w, newSurface->h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     packedPixelData ? packedPixelData : newSurface->pixels);

        if (packedPixelData) {
            free(packedPixelData);
        }

        SDL_FRect overlayRect;

        // These overlay positions differ from the other renderers because OpenGL
        // places the origin in the lower-left corner instead of the upper-left.
        if (type == Overlay::OverlayStatusUpdate) {
            // Bottom Left
            overlayRect.x = 0;
            overlayRect.y = 0;
        }
        else if (type == Overlay::OverlayDebug) {
            // Top left
            overlayRect.x = 0;
            overlayRect.y = viewportHeight - newSurface->h;
        }
        else if (type == Overlay::OverlayToolbar) {
            // User-positionable along the top edge (OpenGL origin is lower-left)
            overlayRect.x = SDL_round(
                Session::get()->getOverlayManager().getOverlayHorizontalPosition(type) *
                SDL_max(0, viewportWidth - newSurface->w));
            overlayRect.y = viewportHeight - newSurface->h;
        }
        else {
            SDL_assert(false);
        }

        overlayRect.w = newSurface->w;
        overlayRect.h = newSurface->h;

        SDL_DestroySurface(newSurface);

        // Convert screen space to normalized device coordinates
        StreamUtils::screenSpaceToNormalizedDeviceCoords(&overlayRect, viewportWidth, viewportHeight);

        OVERLAY_VERTEX verts[] =
        {
            {overlayRect.x + overlayRect.w, overlayRect.y + overlayRect.h, 1.0f, 0.0f},
            {overlayRect.x, overlayRect.y + overlayRect.h, 0.0f, 0.0f},
            {overlayRect.x, overlayRect.y, 0.0f, 1.0f},
            {overlayRect.x, overlayRect.y, 0.0f, 1.0f},
            {overlayRect.x + overlayRect.w, overlayRect.y, 1.0f, 1.0f},
            {overlayRect.x + overlayRect.w, overlayRect.y + overlayRect.h, 1.0f, 0.0f}
        };

        glBindBuffer(GL_ARRAY_BUFFER, m_OverlayVbos[type]);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

        SDL_SetAtomicInt(&m_OverlayHasValidData[type], 1);
    }

    if (!SDL_GetAtomicInt(&m_OverlayHasValidData[type])) {
        // If the overlay is not populated yet or is stale, don't render it.
        return;
    }

    glUseProgram(m_OverlayShaderProgram);

    glBindBuffer(GL_ARRAY_BUFFER, m_OverlayVbos[type]);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(OVERLAY_VERTEX), (void*)offsetof(OVERLAY_VERTEX, x));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(OVERLAY_VERTEX), (void*)offsetof(OVERLAY_VERTEX, u));
    glEnableVertexAttribArray(1);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_OverlayTextures[type]);
    glUniform1i(m_OverlayShaderProgramParams[OVERLAY_PARAM_TEXTURE], 0);

    glDrawArrays(GL_TRIANGLES, 0, 6);
}

int EGLRenderer::loadAndBuildShader(int shaderType,
                                    const char *file) {
    GLuint shader = glCreateShader(shaderType);
    if (!shader || shader == GL_INVALID_ENUM) {
        EGL_LOG(Error, "Can't create shader: %d", glGetError());
        return 0;
    }

    auto sourceData = Path::readDataFile(file);
    GLint len = sourceData.size();
    const char *buf = sourceData.data();

    glShaderSource(shader, 1, &buf, &len);
    glCompileShader(shader);
    GLint status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char shaderLog[512];
        glGetShaderInfoLog(shader, sizeof (shaderLog), nullptr, shaderLog);
        EGL_LOG(Error, "Cannot load shader \"%s\": %s", file, shaderLog);
        return 0;
    }

    return shader;
}

unsigned EGLRenderer::compileShader(const char* vertexShaderSrc, const char* fragmentShaderSrc) {
    unsigned shader = 0;

    GLuint vertexShader = loadAndBuildShader(GL_VERTEX_SHADER, vertexShaderSrc);
    if (!vertexShader)
        return false;

    GLuint fragmentShader = loadAndBuildShader(GL_FRAGMENT_SHADER, fragmentShaderSrc);
    if (!fragmentShader)
        goto fragError;

    shader = glCreateProgram();
    if (!shader) {
        EGL_LOG(Error, "Cannot create shader program");
        goto progFailCreate;
    }

    glAttachShader(shader, vertexShader);
    glAttachShader(shader, fragmentShader);
    glLinkProgram(shader);
    int status;
    glGetProgramiv(shader, GL_LINK_STATUS, &status);
    if (!status) {
        char shader_log[512];
        glGetProgramInfoLog(shader, sizeof (shader_log), nullptr, shader_log);
        EGL_LOG(Error, "Cannot link shader program: %s", shader_log);
        glDeleteProgram(shader);
        shader = 0;
    }

progFailCreate:
    glDeleteShader(fragmentShader);
fragError:
    glDeleteShader(vertexShader);
    return shader;
}

bool EGLRenderer::compileShaders() {
    SDL_assert(!m_ShaderProgram);
    SDL_assert(!m_OverlayShaderProgram);

    SDL_assert(m_EGLImagePixelFormat != AV_PIX_FMT_NONE);

    // XXX: TODO: other formats
    if (m_EGLImagePixelFormat == AV_PIX_FMT_NV12 || m_EGLImagePixelFormat == AV_PIX_FMT_P010) {
        m_ShaderProgram = compileShader("egl.vert", "egl_nv12.frag");
        if (!m_ShaderProgram) {
            return false;
        }

        m_ShaderProgramParams[NV12_PARAM_YUVMAT] = glGetUniformLocation(m_ShaderProgram, "yuvmat");
        m_ShaderProgramParams[NV12_PARAM_OFFSET] = glGetUniformLocation(m_ShaderProgram, "offset");
        m_ShaderProgramParams[NV12_PARAM_PLANE1] = glGetUniformLocation(m_ShaderProgram, "plane1");
        m_ShaderProgramParams[NV12_PARAM_PLANE2] = glGetUniformLocation(m_ShaderProgram, "plane2");
    }
    else if (m_EGLImagePixelFormat == AV_PIX_FMT_DRM_PRIME) {
        m_ShaderProgram = compileShader("egl.vert", "egl_opaque.frag");
        if (!m_ShaderProgram) {
            return false;
        }

        m_ShaderProgramParams[OPAQUE_PARAM_TEXTURE] = glGetUniformLocation(m_ShaderProgram, "uTexture");
        m_ShaderProgramParams[OPAQUE_PARAM_IDENTITY_GBR_8] = glGetUniformLocation(m_ShaderProgram, "uIdentityGbr8");
    }
    else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unsupported EGL pixel format: %d",
                     m_EGLImagePixelFormat);
        SDL_assert(false);
        return false;
    }

    m_OverlayShaderProgram = compileShader("egl.vert", "egl_overlay.frag");
    if (!m_OverlayShaderProgram) {
        return false;
    }

    m_OverlayShaderProgramParams[OVERLAY_PARAM_TEXTURE] = glGetUniformLocation(m_OverlayShaderProgram, "uTexture");

    return true;
}

bool EGLRenderer::initialize(PDECODER_PARAMETERS params)
{
    m_Window = params->window;
    if (params->presentationLayout != nullptr &&
            params->presentationLayout->canvasSize.isValid() &&
            !params->presentationLayout->outputs.isEmpty()) {
        m_PresentationCanvasSize = params->presentationLayout->canvasSize;
        for (const auto& output : params->presentationLayout->outputs) {
            m_PresentationTargets.push_back(output);
        }
        std::stable_sort(m_PresentationTargets.begin(),
                         m_PresentationTargets.end(),
                         [](const auto& left, const auto& right) {
            return left.primary && !right.primary;
        });
    }
    else {
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(m_Window, &width, &height);
        m_PresentationCanvasSize = QSize(qMax(1, width), qMax(1, height));
        m_PresentationTargets.push_back(
            {m_Window,
             QRect(QPoint(0, 0), m_PresentationCanvasSize),
             true});
    }
    m_IdentityGbr8Bit = params->enableIdentityGbr &&
                        !(params->videoFormat & VIDEO_FORMAT_MASK_10BIT);

    // This renderer doesn't support HDR, so pick a different one.
    // HACK: This avoids a deadlock in SDL_CreateRenderer() if
    // Vulkan was used before and SDL is trying to load EGL.
    if ((params->videoFormat & VIDEO_FORMAT_MASK_10BIT) && !params->enableIdentityGbr) {
        EGL_LOG(Info, "EGL doesn't support HDR rendering");
        return false;
    }

    if (params->enableIdentityGbr) {
        EGL_LOG(Info,
                "Enabling %s identity GBR presentation",
                m_IdentityGbr8Bit ? "8-bit" : "10-bit");
    }

    // HACK: Work around bug where renderer will repeatedly fail with:
    // SDL_CreateRenderer() failed: Could not create GLES window surface
    // Don't retry if we've already failed to create a renderer for this
    // window *unless* the format has changed from 10-bit to 8-bit.
    if (m_Window == s_LastFailedWindow) {
        EGL_LOG(Error, "SDL_CreateRenderer() already failed on this window!");
        return false;
    }

    // This hint will ensure we use EGL to retrieve our GL context,
    // even on X11 where that is not the default. EGL is required
    // to avoid a crash in Mesa.
    // https://gitlab.freedesktop.org/mesa/mesa/issues/1011
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    const char* renderDriver = nullptr;
    int maxRenderers = SDL_GetNumRenderDrivers();
    SDL_assert(maxRenderers >= 0);

    for (int renderIndex = 0; renderIndex < maxRenderers; ++renderIndex) {
        const char* candidate = SDL_GetRenderDriver(renderIndex);
        if (candidate != nullptr && !strcmp(candidate, "opengles2")) {
            renderDriver = candidate;
            break;
        }
    }
    if (renderDriver == nullptr) {
        EGL_LOG(Error, "Could not find a suitable SDL_Renderer");
        return false;
    }

    for (const auto& target : m_PresentationTargets) {
        SDL_Renderer* renderer = SDL_CreateRenderer(target.window,
                                                    renderDriver);
        if (!renderer) {
            // Print the error here (before it gets clobbered), but ensure that
            // we flush window events just in case SDL re-created a window
            // before eventually failing.
            EGL_LOG(Error,
                    "SDL_CreateRenderer() failed for PLANK output: %s",
                    SDL_GetError());
            s_LastFailedWindow = target.window;
            break;
        }
        m_DummyRenderers.push_back(renderer);
    }

    // SDL_CreateRenderer() can end up having to recreate our window (SDL_RecreateWindow())
    // to ensure it's compatible with the renderer's OpenGL context. If that happens, we
    // can get spurious SDL_WINDOWEVENT events that will cause us to (again) recreate our
    // renderer. This can lead to an infinite to renderer recreation, so discard all
    // SDL_WINDOWEVENT events after SDL_CreateRenderer().
    Session* session = Session::get();
    if (session != nullptr) {
        // If we get here during a session, we need to synchronize with the event loop
        // to ensure we don't drop any important events.
        session->flushWindowEvents();
    }
    else {
        // If we get here prior to the start of a session, just pump and flush ourselves.
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_EVENT_WINDOW_FIRST, SDL_EVENT_WINDOW_LAST);
    }

    // Now we finally bail if we failed during SDL_CreateRenderer() above.
    if (m_DummyRenderers.size() != m_PresentationTargets.size()) {
        s_LastFailedVideoFormat = params->videoFormat;
        return false;
    }

    const bool isWayland = strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0;
    const bool isKmsDrm = strcmp(SDL_GetCurrentVideoDriver(), "kmsdrm") == 0 ||
                          strcmp(SDL_GetCurrentVideoDriver(), "KMSDRM") == 0;

    if (!(m_Context = SDL_GL_CreateContext(params->window))) {
        EGL_LOG(Error, "Cannot create OpenGL context: %s", SDL_GetError());
        return false;
    }
    if (!SDL_GL_MakeCurrent(params->window, m_Context)) {
        EGL_LOG(Error, "Cannot use created EGL context: %s", SDL_GetError());
        return false;
    }
    for (const auto& target : m_PresentationTargets) {
        if (!SDL_GL_MakeCurrent(target.window, m_Context)) {
            EGL_LOG(Error,
                    "Cannot bind EGL context to PLANK output: %s",
                    SDL_GetError());
            return false;
        }
    }
    SDL_GL_MakeCurrent(m_Window, m_Context);

    {
        int r, g, b, a;
        SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &r);
        SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &g);
        SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &b);
        SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &a);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Color buffer is: R%dG%dB%dA%d",
                    r, g, b, a);
    }

    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &m_GlesMajorVersion);
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &m_GlesMinorVersion);

    // We can use GL_UNPACK_ROW_LENGTH for a more optimized upload of non-tightly-packed textures
    m_HasExtUnpackSubimage = SDL_GL_ExtensionSupported("GL_EXT_unpack_subimage");

    m_EGLDisplay = eglGetCurrentDisplay();
    if (m_EGLDisplay == EGL_NO_DISPLAY) {
        EGL_LOG(Error, "Cannot get EGL display: %d", eglGetError());
        return false;
    }

    const EGLExtensions eglExtensions(m_EGLDisplay);
    if (!eglExtensions.isSupported("EGL_KHR_image_base") &&
        !eglExtensions.isSupported("EGL_KHR_image")) {
        EGL_LOG(Error, "EGL_KHR_image unsupported");
        return false;
    }
    else if (!SDL_GL_ExtensionSupported("GL_OES_EGL_image")) {
        EGL_LOG(Error, "GL_OES_EGL_image unsupported");
        return false;
    }

    if (!m_Backend->initializeEGL(m_EGLDisplay, eglExtensions))
        return false;

    if (!(m_glEGLImageTargetTexture2DOES = (typeof(m_glEGLImageTargetTexture2DOES))eglGetProcAddress("glEGLImageTargetTexture2DOES"))) {
        EGL_LOG(Error,
                "EGL: cannot retrieve `glEGLImageTargetTexture2DOES` address");
        return false;
    }

    // Vertex arrays are an extension on OpenGL ES 2.0
    if (SDL_GL_ExtensionSupported("GL_OES_vertex_array_object")) {
        m_glGenVertexArraysOES = (typeof(m_glGenVertexArraysOES))eglGetProcAddress("glGenVertexArraysOES");
        m_glBindVertexArrayOES = (typeof(m_glBindVertexArrayOES))eglGetProcAddress("glBindVertexArrayOES");
        m_glDeleteVertexArraysOES = (typeof(m_glDeleteVertexArraysOES))eglGetProcAddress("glDeleteVertexArraysOES");
    }
    else {
        // They are included in OpenGL ES 3.0 as part of the standard
        m_glGenVertexArraysOES = (typeof(m_glGenVertexArraysOES))eglGetProcAddress("glGenVertexArrays");
        m_glBindVertexArrayOES = (typeof(m_glBindVertexArrayOES))eglGetProcAddress("glBindVertexArray");
        m_glDeleteVertexArraysOES = (typeof(m_glDeleteVertexArraysOES))eglGetProcAddress("glDeleteVertexArrays");
    }

    if (!m_glGenVertexArraysOES || !m_glBindVertexArrayOES || !m_glDeleteVertexArraysOES) {
        EGL_LOG(Error, "Failed to find VAO functions");
        return false;
    }

    // EGL_KHR_fence_sync is an extension for EGL 1.1+
    if (eglExtensions.isSupported("EGL_KHR_fence_sync")) {
        // eglCreateSyncKHR() has a slightly different prototype to eglCreateSync()
        m_eglCreateSyncKHR = (typeof(m_eglCreateSyncKHR))eglGetProcAddress("eglCreateSyncKHR");
        m_eglDestroySync = (typeof(m_eglDestroySync))eglGetProcAddress("eglDestroySyncKHR");
        m_eglClientWaitSync = (typeof(m_eglClientWaitSync))eglGetProcAddress("eglClientWaitSyncKHR");
    }
    else {
        // EGL 1.5 introduced sync support to the core specification
        m_eglCreateSync = (typeof(m_eglCreateSync))eglGetProcAddress("eglCreateSync");
        m_eglDestroySync = (typeof(m_eglDestroySync))eglGetProcAddress("eglDestroySync");
        m_eglClientWaitSync = (typeof(m_eglClientWaitSync))eglGetProcAddress("eglClientWaitSync");
    }

    if (!(m_eglCreateSync || m_eglCreateSyncKHR) || !m_eglDestroySync || !m_eglClientWaitSync) {
        EGL_LOG(Warn, "Failed to find sync functions");

        // Sub-optimal, but not fatal
        m_eglCreateSync = nullptr;
        m_eglCreateSyncKHR = nullptr;
        m_eglDestroySync = nullptr;
        m_eglClientWaitSync = nullptr;
    }

    // SDL always uses swap interval 0 under the hood on Wayland systems,
    // because the compositor guarantees tear-free rendering. In this
    // situation, swap interval > 0 behaves as a frame pacing option
    // rather than a way to eliminate tearing as SDL will block in
    // SwapBuffers until the compositor consumes the frame. This will
    // needlessly increases latency, so we should avoid it.
    //
    // HACK: In SDL 2.0.22+ on GNOME systems with fractional DPI scaling,
    // the Wayland viewport can be stale when using Super+Left/Right/Up
    // to resize the window. This seems to happen significantly more often
    // with vsync enabled, so this also mitigates that problem too.
    if (params->enableVsync
#ifdef HAS_WAYLAND
            && !isWayland
#endif
            ) {
        SDL_GL_SetSwapInterval(1);

#if defined(HAVE_DRM)
        // The SDL KMSDRM backend already enforces double buffering (due to
        // SDL_HINT_VIDEO_DOUBLE_BUFFER=1), so calling glFinish() after
        // SDL_GL_SwapWindow() will block an extra frame and lock rendering
        // at 1/2 the display refresh rate.
        if (!isKmsDrm)
#endif
        {
            m_BlockingSwapBuffers = true;
        }
    } else {
        SDL_GL_SetSwapInterval(0);
    }

    glGenTextures(EGL_MAX_PLANES, m_Textures);
    for (size_t i = 0; i < EGL_MAX_PLANES; ++i) {
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_Textures[i]);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    glGenBuffers(Overlay::OverlayMax, m_OverlayVbos);
    glGenTextures(Overlay::OverlayMax, m_OverlayTextures);
    for (size_t i = 0; i < Overlay::OverlayMax; ++i) {
        glBindTexture(GL_TEXTURE_2D, m_OverlayTextures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR)
        EGL_LOG(Error, "OpenGL error: %d", err);

    // Detach the context from this thread, so the render thread can attach it
    SDL_GL_MakeCurrent(m_Window, nullptr);

#ifdef SDL_HINT_VIDEO_X11_FORCE_EGL
    if (err == GL_NO_ERROR) {
        // If we got a working GL implementation via EGL, avoid using GLX from now on.
        // GLX will cause problems if we later want to use EGL again on this window.
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "EGL passed preflight checks. Using EGL for GL context creation.");
        SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1");
    }
#endif

    EGL_LOG(Info,
            "PLANK presentation initialized with %zu output surface%s on a %dx%d canvas",
            m_PresentationTargets.size(),
            m_PresentationTargets.size() == 1 ? "" : "s",
            m_PresentationCanvasSize.width(),
            m_PresentationCanvasSize.height());

    return err == GL_NO_ERROR;
}

const float *EGLRenderer::getColorOffsets(const AVFrame* frame) {
    static const float limitedOffsets[] = { 16.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f };
    static const float fullOffsets[] = { 0.0f, 128.0f / 255.0f, 128.0f / 255.0f };

    return isFrameFullRange(frame) ? fullOffsets : limitedOffsets;
}

const float *EGLRenderer::getColorMatrix(const AVFrame* frame) {
    /* The conversion matrices are shamelessly stolen from linux:
     * drivers/media/platform/imx-pxp.c:pxp_setup_csc
     */
    static const float bt601Lim[] = {
        1.1644f, 1.1644f, 1.1644f,
        0.0f, -0.3917f, 2.0172f,
        1.5960f, -0.8129f, 0.0f
    };
    static const float bt601Full[] = {
        1.0f, 1.0f, 1.0f,
        0.0f, -0.3441f, 1.7720f,
        1.4020f, -0.7141f, 0.0f
    };
    static const float bt709Lim[] = {
        1.1644f, 1.1644f, 1.1644f,
        0.0f, -0.2132f, 2.1124f,
        1.7927f, -0.5329f, 0.0f
    };
    static const float bt709Full[] = {
        1.0f, 1.0f, 1.0f,
        0.0f, -0.1873f, 1.8556f,
        1.5748f, -0.4681f, 0.0f
    };
    static const float bt2020Lim[] = {
        1.1644f, 1.1644f, 1.1644f,
        0.0f, -0.1874f, 2.1418f,
        1.6781f, -0.6505f, 0.0f
    };
    static const float bt2020Full[] = {
        1.0f, 1.0f, 1.0f,
        0.0f, -0.1646f, 1.8814f,
        1.4746f, -0.5714f, 0.0f
    };

    bool fullRange = isFrameFullRange(frame);
    switch (getFrameColorspace(frame)) {
        case COLORSPACE_REC_601:
            return fullRange ? bt601Full : bt601Lim;
        case COLORSPACE_REC_709:
            return fullRange ? bt709Full : bt709Lim;
        case COLORSPACE_REC_2020:
            return fullRange ? bt2020Full : bt2020Lim;
        default:
            SDL_assert(false);
    }

    return bt601Lim;
}

bool EGLRenderer::specialize() {
    SDL_assert(!m_VAO);

    if (!compileShaders())
        return false;

    // The viewport should have the aspect ratio of the video stream
    static const float vertices[] = {
        // pos .... // tex coords
        1.0f, 1.0f, 1.0f, 0.0f,
        1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 1.0f,
        -1.0f, 1.0f, 0.0f, 0.0f,

    };
    static const unsigned int indices[] = {
        0, 1, 3,
        1, 2, 3,
    };

    glUseProgram(m_ShaderProgram);

    unsigned int EBO;
    m_glGenVertexArraysOES(1, &m_VAO);
    glGenBuffers(1, &m_VideoVbo);
    glGenBuffers(1, &EBO);

    m_glBindVertexArrayOES(m_VAO);

    glBindBuffer(GL_ARRAY_BUFFER, m_VideoVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof (vertices), vertices, GL_DYNAMIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof (indices), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof (float)));
    glEnableVertexAttribArray(1);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_glBindVertexArrayOES(0);

    glDeleteBuffers(1, &EBO);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        EGL_LOG(Error, "OpenGL error: %d", err);
    }

    return err == GL_NO_ERROR;
}

void EGLRenderer::cleanupRenderContext()
{
    // Detach the context from the render thread so the destructor can attach it
    SDL_GL_MakeCurrent(m_Window, nullptr);
}

void EGLRenderer::waitToRender()
{
    // Ensure our GL context is active on this thread
    // See comment in renderFrame() for more details.
    SDL_GL_MakeCurrent(m_Window, m_Context);

    // Wait for the previous buffer swap to finish before picking the next frame to render.
    // This way we'll get the latest available frame and render it without blocking.
    if (m_BlockingSwapBuffers) {
        // Try to use eglClientWaitSync() if the driver supports it
        if (m_LastRenderSync != EGL_NO_SYNC) {
            SDL_assert(m_eglClientWaitSync != nullptr);
            m_eglClientWaitSync(m_EGLDisplay, m_LastRenderSync, EGL_SYNC_FLUSH_COMMANDS_BIT, EGL_FOREVER);
        }
        else {
            // Use glFinish() if fences aren't available
            glFinish();
        }
    }
}

void EGLRenderer::prepareToRender()
{
    for (const auto& target : m_PresentationTargets) {
        if (!SDL_GL_MakeCurrent(target.window, m_Context)) {
            continue;
        }
        // Draw a black frame until the video stream starts rendering
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapWindow(target.window);
    }
    SDL_GL_MakeCurrent(m_Window, nullptr);
}

void EGLRenderer::renderFrame(AVFrame* frame)
{
    EGLImage imgs[EGL_MAX_PLANES];

    // Attach our GL context to the render thread
    // NB: It should already be current, unless the SDL render event watcher
    // performs a rendering operation (like a viewport update on resize) on
    // our fake SDL_Renderer. If it's already current, this is a no-op.
    SDL_GL_MakeCurrent(m_Window, m_Context);

    // Find the native read-back format and load the shaders
    if (m_EGLImagePixelFormat == AV_PIX_FMT_NONE) {
        m_EGLImagePixelFormat = m_Backend->getEGLImagePixelFormat();
        EGL_LOG(Info, "EGLImage pixel format: %d", m_EGLImagePixelFormat);

        SDL_assert(m_EGLImagePixelFormat != AV_PIX_FMT_NONE);

        if (!specialize()) {
            m_EGLImagePixelFormat = AV_PIX_FMT_NONE;

            // Failure to specialize is fatal. We must reset the renderer
            // to recover successfully.
            //
            // Note: This seems to be easy to trigger when transitioning from
            // maximized mode by dragging the window down on GNOME 42 using
            // XWayland. Other strategies like calling glGetError() don't seem
            // to be able to detect this situation for some reason.
            SDL_Event event;
            event.type = SDL_EVENT_RENDER_DEVICE_RESET;
            SDL_PushEvent(&event);

            return;
        }
    }

    ssize_t plane_count = m_Backend->exportEGLImages(frame, m_EGLDisplay, imgs);
    if (plane_count < 0)
        return;
    for (ssize_t i = 0; i < plane_count; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_Textures[i]);
        m_glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, imgs[i]);
    }

    const QSize streamSize(frame->width, frame->height);
    bool rendererResetRequired = false;
    for (const auto& target : m_PresentationTargets) {
        if (!SDL_GL_MakeCurrent(target.window, m_Context)) {
            EGL_LOG(Error,
                    "Cannot bind EGL context to PLANK output while rendering: %s",
                    SDL_GetError());
            rendererResetRequired = true;
            continue;
        }

        int drawableWidth = 0;
        int drawableHeight = 0;
        SDL_GetWindowSizeInPixels(target.window,
                                  &drawableWidth, &drawableHeight);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        const auto slice = PlankPresentation::sliceForOutput(
                    streamSize, m_PresentationCanvasSize,
                    target.canvasRect);
        if (slice.visible) {
            const float u0 = slice.sourceRect.left() / frame->width;
            const float v0 = slice.sourceRect.top() / frame->height;
            const float u1 = slice.sourceRect.right() / frame->width;
            const float v1 = slice.sourceRect.bottom() / frame->height;
            const float vertices[] = {
                1.0f, 1.0f, u1, v0,
                1.0f, -1.0f, u1, v1,
                -1.0f, -1.0f, u0, v1,
                -1.0f, 1.0f, u0, v0,
            };

            glBindBuffer(GL_ARRAY_BUFFER, m_VideoVbo);
            glBufferSubData(GL_ARRAY_BUFFER, 0,
                            sizeof(vertices), vertices);
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            const float targetScaleX = drawableWidth /
                    static_cast<float>(target.canvasRect.width());
            const float targetScaleY = drawableHeight /
                    static_cast<float>(target.canvasRect.height());
            const QRect destination = slice.destinationRect;
            glViewport(qRound(destination.x() * targetScaleX),
                       qRound(drawableHeight -
                              (destination.y() + destination.height()) *
                              targetScaleY),
                       qRound(destination.width() * targetScaleX),
                       qRound(destination.height() * targetScaleY));

            glUseProgram(m_ShaderProgram);
            m_glBindVertexArrayOES(m_VAO);

            // Bind parameters for the shaders
            if (m_EGLImagePixelFormat == AV_PIX_FMT_NV12 ||
                    m_EGLImagePixelFormat == AV_PIX_FMT_P010) {
                glUniformMatrix3fv(
                    m_ShaderProgramParams[NV12_PARAM_YUVMAT],
                    1, GL_FALSE, getColorMatrix(frame));
                glUniform3fv(m_ShaderProgramParams[NV12_PARAM_OFFSET],
                             1, getColorOffsets(frame));
                glUniform1i(m_ShaderProgramParams[NV12_PARAM_PLANE1], 0);
                glUniform1i(m_ShaderProgramParams[NV12_PARAM_PLANE2], 1);
            }
            else if (m_EGLImagePixelFormat == AV_PIX_FMT_DRM_PRIME) {
                glUniform1i(m_ShaderProgramParams[OPAQUE_PARAM_TEXTURE], 0);
                glUniform1i(
                    m_ShaderProgramParams[OPAQUE_PARAM_IDENTITY_GBR_8],
                    m_IdentityGbr8Bit ? 1 : 0);
            }

            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
            m_glBindVertexArrayOES(0);
        }

        // The toolbar and on-screen statistics belong to the primary output.
        if (target.primary) {
            glViewport(0, 0, drawableWidth, drawableHeight);
            for (int i = 0; i < Overlay::OverlayMax; i++) {
                renderOverlay((Overlay::OverlayType)i,
                              drawableWidth, drawableHeight);
            }
        }

        if (!SDL_GL_SwapWindow(target.window)) {
            EGL_LOG(Error,
                    "Failed to swap PLANK output: %s",
                    SDL_GetError());
            rendererResetRequired = true;
        }
    }

    if (m_BlockingSwapBuffers) {
        // This glClear() requires the new back buffer to complete. This ensures
        // our eglClientWaitSync() or glFinish() call in waitToRender() will not
        // return before the new buffer is actually ready for rendering.
        glClear(GL_COLOR_BUFFER_BIT);

        // If we this EGL implementation supports fences, use those to delay
        // rendering the next frame until this one is completed. If not, we'll
        // have to just use glFinish().
        if (m_eglClientWaitSync != nullptr) {
            // Delete the sync object from last render
            if (m_LastRenderSync != EGL_NO_SYNC) {
                m_eglDestroySync(m_EGLDisplay, m_LastRenderSync);
            }

            // Create a new sync object that will be signalled when the buffer swap is completed
            if (m_eglCreateSync != nullptr) {
                m_LastRenderSync = m_eglCreateSync(m_EGLDisplay, EGL_SYNC_FENCE, nullptr);
            }
            else {
                SDL_assert(m_eglCreateSyncKHR != nullptr);
                m_LastRenderSync = m_eglCreateSyncKHR(m_EGLDisplay, EGL_SYNC_FENCE, nullptr);
            }
        }
    }

    m_Backend->freeEGLImages(m_EGLDisplay, imgs);

    // Free the DMA-BUF backing the last frame now that it is definitely
    // no longer being used anymore. While the PRIME FD stays around until
    // EGL is done with it, the memory backing it may be reused by FFmpeg
    // before the GPU has read it. This is particularly noticeable on the
    // RK3288-based TinkerBoard when V-Sync is disabled.
    av_frame_unref(m_LastFrame);
    av_frame_move_ref(m_LastFrame, frame);

    if (rendererResetRequired) {
        SDL_Event event = {};
        event.type = SDL_EVENT_RENDER_DEVICE_RESET;
        SDL_PushEvent(&event);
    }
}

bool EGLRenderer::testRenderFrame(AVFrame* frame)
{
    EGLImage imgs[EGL_MAX_PLANES];

    // Make sure we can get working EGLImages from the backend renderer.
    // Some devices (Raspberry Pi) will happily decode into DRM formats that
    // its own GL implementation won't accept in eglCreateImage().
    ssize_t plane_count = m_Backend->exportEGLImages(frame, m_EGLDisplay, imgs);
    if (plane_count <= 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Backend failed to export EGL image for test frame");
        return false;
    }

    m_Backend->freeEGLImages(m_EGLDisplay, imgs);
    return true;
}

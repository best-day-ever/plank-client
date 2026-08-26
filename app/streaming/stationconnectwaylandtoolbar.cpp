#include "stationconnectwaylandtoolbar.h"

#ifdef HAS_WAYLAND

#include <QPainter>

#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <wayland-client.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

namespace {

int createAnonymousFile(size_t size)
{
    const int fd = static_cast<int>(syscall(
            SYS_memfd_create, "stationconnect-toolbar", MFD_CLOEXEC));
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    return fd;
}

} // namespace

class StationConnectWaylandToolbar::Impl
{
public:
    struct Buffer {
        Impl* owner = nullptr;
        wl_buffer* object = nullptr;
        void* mapping = MAP_FAILED;
        size_t size = 0;
        bool released = false;
    };

    explicit Impl(SDL_Window* parentWindow)
    {
        const SDL_PropertiesID properties = SDL_GetWindowProperties(parentWindow);
        m_Display = static_cast<wl_display*>(SDL_GetPointerProperty(
                properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
        m_ParentSurface = static_cast<wl_surface*>(SDL_GetPointerProperty(
                properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
    }

    ~Impl()
    {
        if (m_InputWindow != nullptr) {
            SDL_DestroyWindow(m_InputWindow);
        }
        if (m_Subsurface != nullptr) {
            wl_subsurface_destroy(m_Subsurface);
        }
        if (m_Surface != nullptr) {
            wl_surface_destroy(m_Surface);
        }
        for (Buffer* buffer : m_Buffers) {
            destroyBuffer(buffer);
        }
        if (m_Shm != nullptr) {
            wl_shm_destroy(m_Shm);
        }
        if (m_Subcompositor != nullptr) {
            wl_subcompositor_destroy(m_Subcompositor);
        }
        if (m_Compositor != nullptr) {
            wl_compositor_destroy(m_Compositor);
        }
        if (m_Registry != nullptr) {
            wl_registry_destroy(m_Registry);
        }
        if (m_Queue != nullptr) {
            wl_event_queue_destroy(m_Queue);
        }
    }

    bool initialize()
    {
        if (m_Display == nullptr || m_ParentSurface == nullptr) {
            return false;
        }

        m_Queue = wl_display_create_queue(m_Display);
        m_Registry = wl_display_get_registry(m_Display);
        if (m_Queue == nullptr || m_Registry == nullptr) {
            return false;
        }
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(m_Registry), m_Queue);
        wl_registry_add_listener(m_Registry, &RegistryListener, this);
        if (wl_display_roundtrip_queue(m_Display, m_Queue) < 0 ||
                m_Compositor == nullptr || m_Subcompositor == nullptr ||
                m_Shm == nullptr) {
            return false;
        }

        m_Surface = wl_compositor_create_surface(m_Compositor);
        if (m_Surface == nullptr) {
            return false;
        }
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(m_Surface), m_Queue);

        m_Subsurface = wl_subcompositor_get_subsurface(
                m_Subcompositor, m_Surface, m_ParentSurface);
        if (m_Subsurface == nullptr) {
            return false;
        }
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(m_Subsurface), m_Queue);
        wl_subsurface_set_desync(m_Subsurface);
        wl_subsurface_set_position(m_Subsurface, 0, 0);

        SDL_PropertiesID properties = SDL_CreateProperties();
        if (properties == 0) {
            return false;
        }
        SDL_SetPointerProperty(
                    properties,
                    SDL_PROP_WINDOW_CREATE_WAYLAND_WL_SURFACE_POINTER,
                    m_Surface);
        SDL_SetBooleanProperty(
                    properties,
                    SDL_PROP_WINDOW_CREATE_WAYLAND_SURFACE_ROLE_CUSTOM_BOOLEAN,
                    true);
        SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 1);
        SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 1);
        m_InputWindow = SDL_CreateWindowWithProperties(properties);
        SDL_DestroyProperties(properties);
        if (m_InputWindow == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to register Wayland toolbar surface with SDL: %s",
                        SDL_GetError());
            return false;
        }

        wl_display_flush(m_Display);
        return true;
    }

    void dispatchPending()
    {
        if (m_Display != nullptr && m_Queue != nullptr) {
            wl_display_dispatch_queue_pending(m_Display, m_Queue);
        }
    }

    bool isAttachedTo(SDL_Window* parentWindow) const
    {
        const SDL_PropertiesID properties = SDL_GetWindowProperties(parentWindow);
        return m_ParentSurface == static_cast<wl_surface*>(SDL_GetPointerProperty(
                    properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER,
                    nullptr));
    }

    SDL_WindowID windowId() const
    {
        return m_InputWindow != nullptr ? SDL_GetWindowID(m_InputWindow) : 0;
    }

    void setLayout(int parentWidth, int toolbarX,
                   int toolbarWidth, int toolbarHeight)
    {
        m_ParentWidth = std::max(1, parentWidth);
        m_ToolbarWidth = std::clamp(toolbarWidth, 1, m_ParentWidth);
        m_ToolbarX = std::clamp(toolbarX, 0,
                                m_ParentWidth - m_ToolbarWidth);
        m_Height = std::max(1, toolbarHeight);
        if (m_InputWindow != nullptr) {
            int inputWidth = 0;
            int inputHeight = 0;
            SDL_GetWindowSize(m_InputWindow, &inputWidth, &inputHeight);
            if (inputWidth != m_ParentWidth || inputHeight != m_Height) {
                if (!SDL_SetWindowSize(m_InputWindow,
                                       m_ParentWidth, m_Height)) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Unable to size SDL toolbar input window: %s",
                                SDL_GetError());
                }
            }
        }
        updateInputRegion();
        wl_display_flush(m_Display);
    }

    void setVisible(bool visible)
    {
        if (m_Visible == visible || m_Surface == nullptr) {
            return;
        }
        m_Visible = visible;
        if (!m_Visible) {
            wl_surface_attach(m_Surface, nullptr, 0, 0);
            wl_surface_commit(m_Surface);
        } else if (!m_LatestToolbarImage.isNull()) {
            publishLatestImage();
        }
        wl_display_flush(m_Display);
    }

    void present(const QImage& image)
    {
        m_LatestToolbarImage = image.convertToFormat(
                QImage::Format_ARGB32_Premultiplied);
        if (m_Visible) {
            publishLatestImage();
        }
    }

private:
    static void registryGlobal(void* data, wl_registry* registry,
                               uint32_t name, const char* interface,
                               uint32_t version)
    {
        auto* self = static_cast<Impl*>(data);
        if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
            self->m_Compositor = static_cast<wl_compositor*>(wl_registry_bind(
                    registry, name, &wl_compositor_interface,
                    std::min(version, 4u)));
        } else if (std::strcmp(interface,
                               wl_subcompositor_interface.name) == 0) {
            self->m_Subcompositor = static_cast<wl_subcompositor*>(
                    wl_registry_bind(registry, name,
                                     &wl_subcompositor_interface, 1));
        } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
            self->m_Shm = static_cast<wl_shm*>(wl_registry_bind(
                    registry, name, &wl_shm_interface, 1));
        }
    }

    static void registryRemove(void*, wl_registry*, uint32_t) {}

    static void bufferRelease(void* data, wl_buffer*)
    {
        static_cast<Buffer*>(data)->released = true;
    }

    Buffer* createBuffer(const QImage& source)
    {
        const QImage image = source.convertToFormat(
                QImage::Format_ARGB32_Premultiplied);
        const int stride = image.bytesPerLine();
        const size_t size = static_cast<size_t>(stride) * image.height();
        const int fd = createAnonymousFile(size);
        if (fd < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to create Wayland toolbar shared memory: %s",
                        std::strerror(errno));
            return nullptr;
        }
        void* mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, 0);
        if (mapping == MAP_FAILED) {
            close(fd);
            return nullptr;
        }
        std::memcpy(mapping, image.constBits(), size);

        wl_shm_pool* pool = wl_shm_create_pool(m_Shm, fd,
                                                static_cast<int32_t>(size));
        wl_buffer* object = wl_shm_pool_create_buffer(
                pool, 0, image.width(), image.height(), stride,
                WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        if (object == nullptr) {
            munmap(mapping, size);
            return nullptr;
        }

        Buffer* buffer = new Buffer{this, object, mapping, size, false};
        wl_buffer_add_listener(object, &BufferListener, buffer);
        m_Buffers.push_back(buffer);
        return buffer;
    }

    static void destroyBuffer(Buffer* buffer)
    {
        if (buffer->object != nullptr) {
            wl_buffer_destroy(buffer->object);
        }
        if (buffer->mapping != MAP_FAILED) {
            munmap(buffer->mapping, buffer->size);
        }
        delete buffer;
    }

    void collectReleasedBuffers()
    {
        auto it = m_Buffers.begin();
        while (it != m_Buffers.end()) {
            Buffer* buffer = *it;
            if (buffer->released) {
                destroyBuffer(buffer);
                it = m_Buffers.erase(it);
            } else {
                ++it;
            }
        }
    }

    void updateInputRegion()
    {
        if (m_Surface == nullptr || m_Compositor == nullptr) {
            return;
        }
        wl_region* region = wl_compositor_create_region(m_Compositor);
        if (region == nullptr) {
            return;
        }
        wl_region_add(region, m_ToolbarX, 0, m_ToolbarWidth, m_Height);
        wl_surface_set_input_region(m_Surface, region);
        wl_region_destroy(region);
    }

    void publishLatestImage()
    {
        if (m_ParentWidth <= 0 || m_Height <= 0 ||
                m_LatestToolbarImage.isNull()) {
            return;
        }
        collectReleasedBuffers();
        QImage surfaceImage(m_ParentWidth, m_Height,
                            QImage::Format_ARGB32_Premultiplied);
        surfaceImage.fill(Qt::transparent);
        QPainter painter(&surfaceImage);
        painter.drawImage(m_ToolbarX, 0, m_LatestToolbarImage);
        painter.end();
        Buffer* buffer = createBuffer(surfaceImage);
        if (buffer == nullptr) {
            return;
        }
        wl_surface_attach(m_Surface, buffer->object, 0, 0);
        wl_surface_damage(m_Surface, 0, 0,
                          surfaceImage.width(), surfaceImage.height());
        wl_surface_commit(m_Surface);
        wl_display_flush(m_Display);
    }

    static const wl_registry_listener RegistryListener;
    static const wl_buffer_listener BufferListener;

    wl_display* m_Display = nullptr;
    wl_surface* m_ParentSurface = nullptr;
    wl_event_queue* m_Queue = nullptr;
    wl_registry* m_Registry = nullptr;
    wl_compositor* m_Compositor = nullptr;
    wl_subcompositor* m_Subcompositor = nullptr;
    wl_shm* m_Shm = nullptr;
    wl_surface* m_Surface = nullptr;
    wl_subsurface* m_Subsurface = nullptr;
    SDL_Window* m_InputWindow = nullptr;
    std::vector<Buffer*> m_Buffers;
    QImage m_LatestToolbarImage;
    bool m_Visible = false;
    int m_ParentWidth = 0;
    int m_ToolbarX = 0;
    int m_ToolbarWidth = 0;
    int m_Height = 0;
};

const wl_registry_listener StationConnectWaylandToolbar::Impl::RegistryListener = {
    registryGlobal,
    registryRemove,
};

const wl_buffer_listener StationConnectWaylandToolbar::Impl::BufferListener = {
    bufferRelease,
};

#else

class StationConnectWaylandToolbar::Impl {};

#endif

StationConnectWaylandToolbar::StationConnectWaylandToolbar(
        std::unique_ptr<Impl> impl)
    : m_Impl(std::move(impl))
{
}

StationConnectWaylandToolbar::~StationConnectWaylandToolbar() = default;

std::unique_ptr<StationConnectWaylandToolbar>
StationConnectWaylandToolbar::create(SDL_Window* parentWindow)
{
#ifdef HAS_WAYLAND
    std::unique_ptr<Impl> impl(new Impl(parentWindow));
    if (!impl->initialize()) {
        return nullptr;
    }
    return std::unique_ptr<StationConnectWaylandToolbar>(
            new StationConnectWaylandToolbar(std::move(impl)));
#else
    (void) parentWindow;
    return nullptr;
#endif
}

void StationConnectWaylandToolbar::dispatchPending()
{
#ifdef HAS_WAYLAND
    m_Impl->dispatchPending();
#endif
}

bool StationConnectWaylandToolbar::isAttachedTo(SDL_Window* parentWindow) const
{
#ifdef HAS_WAYLAND
    return m_Impl->isAttachedTo(parentWindow);
#else
    (void) parentWindow;
    return false;
#endif
}

SDL_WindowID StationConnectWaylandToolbar::windowId() const
{
#ifdef HAS_WAYLAND
    return m_Impl->windowId();
#else
    return 0;
#endif
}

void StationConnectWaylandToolbar::setLayout(
        int parentWidth, int toolbarX,
        int toolbarWidth, int toolbarHeight)
{
#ifdef HAS_WAYLAND
    m_Impl->setLayout(parentWidth, toolbarX,
                      toolbarWidth, toolbarHeight);
#else
    (void) parentWidth;
    (void) toolbarX;
    (void) toolbarWidth;
    (void) toolbarHeight;
#endif
}

void StationConnectWaylandToolbar::setVisible(bool visible)
{
#ifdef HAS_WAYLAND
    m_Impl->setVisible(visible);
#else
    (void) visible;
#endif
}

void StationConnectWaylandToolbar::present(const QImage& image)
{
#ifdef HAS_WAYLAND
    m_Impl->present(image);
#else
    (void) image;
#endif
}

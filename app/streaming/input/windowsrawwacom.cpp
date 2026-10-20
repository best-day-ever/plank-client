#include "windowsrawwacom.h"
#include "windowswacomlogic.h"
#include "macrawwacomasync.h" // Shared bounded lifecycle barrier.
#include "macrawwacomlogic.h" // Platform-independent bounded wire parser.
#include "linuxwacomreportworker.h" // Epoch-isolated asynchronous report operations.

#include <Limelight.h>
#include <plank.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_hidapi.h>
#include <QString>
#include <QtEndian>
#ifdef Q_OS_WIN
#include <windows.h>
#include <cfgmgr32.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace {
std::atomic_uint32_t nextGeneration{0};
using Clock = std::chrono::steady_clock;
using HidHandle = std::shared_ptr<SDL_hid_device>;

std::string physicalDevice(const char* path)
{
#ifdef Q_OS_WIN
    // HIDAPI exposes top-level collections separately. Group all collections
    // and USB interfaces by their real USB parent, never just by VID/PID (two
    // identical tablets may be connected simultaneously).
    QString instance = QString::fromUtf8(path).toUpper();
    if (!instance.startsWith(QStringLiteral("\\\\?\\"))) return {};
    instance = instance.mid(4).section(QLatin1Char('{'), 0, 0);
    if (instance.endsWith(QLatin1Char('#'))) instance.chop(1);
    instance.replace(QLatin1Char('#'), QLatin1Char('\\'));
    DEVINST node;
    auto wide = instance.toStdWString();
    if (CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(wide.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return {};
    for (unsigned depth = 0; depth < 12; ++depth) {
        wchar_t id[MAX_DEVICE_ID_LEN]{};
        if (CM_Get_Device_IDW(node, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return {};
        const auto value = QString::fromWCharArray(id).toUpper();
        if (value.startsWith(QStringLiteral("USB\\VID_056A&")) &&
                !value.section(QLatin1Char('\\'), 1, 1).contains(QStringLiteral("&MI_"))) {
            return value.toUtf8().toStdString();
        }
        DEVINST parent;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) return {};
        node = parent;
    }
    return {};
#else
    Q_UNUSED(path);
    return {}; // Test builds exercise the protocol/parser without USB ownership.
#endif
}
}

class WindowsRawWacomInput::Impl : public std::enable_shared_from_this<Impl> {
public:
    explicit Impl(std::function<void()> callback) : activity(std::move(callback)) {}
    void start() { thread = std::thread([self = shared_from_this()] { self->run(); }); }
    ~Impl() { if (thread.joinable()) thread.detach(); }
    bool barrier(std::uint64_t ticket) {
        changed.notify_all();
        if (lifecycle.wait(ticket, std::chrono::seconds(2))) return true;
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "Windows Wacom release barrier timed out");
        return false;
    }
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(activityMutex);
            activity = nullptr;
        }
        barrier(lifecycle.stop());
        shutdownRequested = true;
        stopping = true;
        changed.notify_all();
        const bool exited = lifecycle.waitExited(std::chrono::seconds(2));
        if (thread.joinable()) {
            if (exited) thread.join();
            else thread.detach(); // Worker owns its state until Windows returns.
        }
        if (!exited) SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "Windows Wacom shutdown exceeded deadline");
    }
    void tabletActivity() {
        std::lock_guard<std::mutex> lock(activityMutex);
        if (activity && lifecycle.canForward()) activity();
    }
    struct Interface {
        HidHandle handle;
        std::vector<unsigned char> descriptor;
        WindowsWacom::ReportSizes sizes;
        std::string path;
        int interfaceNumber;
    };
    bool frame(std::uint16_t type, std::uint16_t interfaceId = 0,
               std::uint32_t transaction = 0, const unsigned char* payload = nullptr,
               std::size_t length = 0) {
        if (shutdownRequested || length > PLANK_RAW_HID_MAX_PAYLOAD_SIZE ||
                (type != PLANK_RAW_HID_SUSPEND && type != PLANK_RAW_HID_DETACH &&
                 !lifecycle.canForward())) return false;
        PLANK_RAW_HID_WIRE_HEADER h{};
        h.magic = qToLittleEndian<std::uint32_t>(PLANK_RAW_HID_WIRE_MAGIC);
        h.version = qToLittleEndian<std::uint16_t>(PLANK_RAW_HID_WIRE_VERSION);
        h.type = qToLittleEndian(type);
        h.interfaceId = qToLittleEndian(interfaceId);
        h.generation = qToLittleEndian(generation);
        h.transactionId = qToLittleEndian(transaction);
        h.payloadLength = qToLittleEndian(static_cast<std::uint32_t>(length));
        std::vector<unsigned char> bytes(sizeof(h) + length);
        std::memcpy(bytes.data(), &h, sizeof(h));
        if (length) std::memcpy(bytes.data() + sizeof(h), payload, length);
        return LiSendRawHidEvent(bytes.data(), static_cast<unsigned>(bytes.size())) == 0;
    }
    void release(bool unplug = false) {
        if (pending || attached) frame(unplug ? PLANK_RAW_HID_DETACH : PLANK_RAW_HID_SUSPEND);
        attached = false;
        pending = false;
        reports.invalidate();
        interfaces.clear();
    }
    bool discover() {
        SDL_hid_device_info* list = SDL_hid_enumerate(0x056a, 0);
        std::map<std::string, std::vector<std::string>> groups;
        for (auto* info = list; info; info = info->next) {
            if (!info->path || info->bus_type != SDL_HID_API_BUS_USB ||
                    WindowsWacom::normalizedOnlyProduct(info->product_id) ||
                    WindowsWacom::compatibilityCollection(info->usage_page, info->usage)) continue;
            const auto parent = physicalDevice(info->path);
            if (!parent.empty()) groups[parent].push_back(info->path);
        }
        SDL_hid_free_enumeration(list);
        if (groups.empty()) return false;
        if (!lifecycle.canForward()) return false;
        auto paths = groups.begin()->second;
        std::sort(paths.begin(), paths.end());
        paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
        if (paths.size() > PLANK_RAW_HID_MAX_INTERFACES) return false;
        PLANK_RAW_HID_DEVICE_MESSAGE device{};
        device.bus = qToLittleEndian<std::uint16_t>(3); // Linux BUS_USB
        device.vendor = qToLittleEndian<std::uint32_t>(0x056a);
        std::vector<Interface> opened;
        for (const auto& path : paths) {
            HidHandle handle(SDL_hid_open_path(path.c_str()), SDL_hid_close);
            if (!handle || !lifecycle.canForward()) return false; // Never publish a partially opened tablet.
            std::vector<unsigned char> descriptor(PLANK_RAW_HID_MAX_DESCRIPTOR_SIZE);
            const int size = SDL_hid_get_report_descriptor(handle.get(), descriptor.data(), descriptor.size());
            if (size <= 0 || static_cast<std::size_t>(size) > descriptor.size()) return false;
            if (!lifecycle.canForward()) return false;
            descriptor.resize(size);
            WindowsWacom::ReportSizes sizes;
            if (!WindowsWacom::reportSizes(descriptor.data(), descriptor.size(), sizes)) return false;
            if (SDL_hid_set_nonblocking(handle.get(), 1) < 0) return false;
            const auto* info = SDL_hid_get_device_info(handle.get());
            if (!info) return false;
            if (opened.empty()) {
                device.product = qToLittleEndian<std::uint32_t>(info->product_id);
                device.version = qToLittleEndian<std::uint32_t>(info->release_number);
                const auto name = info->product_string ? QString::fromWCharArray(info->product_string).toUtf8() : QByteArray("Wacom tablet");
                std::memcpy(device.name, name.constData(), std::min<std::size_t>(name.size(), sizeof(device.name) - 1));
                // No Windows device path or physical serial is forwarded.
                std::strcpy(device.physical, "windows-usb-wacom");
            }
            opened.push_back({std::move(handle), std::move(descriptor), sizes, path, info->interface_number});
        }
        if (!lifecycle.canForward()) return false;
        interfaces = std::move(opened);
        do { generation = static_cast<std::uint16_t>(++nextGeneration); } while (!generation);
        sequence = 0;
        device.interfaceCount = qToLittleEndian(static_cast<std::uint16_t>(interfaces.size()));
        pending = true;
        deadline = Clock::now() + std::chrono::seconds(15);
        if (!frame(PLANK_RAW_HID_DEVICE, 0, 0, reinterpret_cast<const unsigned char*>(&device), sizeof(device))) return false;
        for (std::size_t i = 0; i < interfaces.size(); ++i) {
            if (!frame(PLANK_RAW_HID_DESCRIPTOR, static_cast<std::uint16_t>(i), 0,
                       interfaces[i].descriptor.data(), interfaces[i].descriptor.size())) return false;
        }
        return true;
    }
    void control(const std::vector<unsigned char>& bytes) {
        MacWacomWire::Control c;
        if (!MacWacomWire::parse(bytes.data(), bytes.size(), c) ||
                c.generation != generation || interfaces.empty()) return;
        const auto* payload = bytes.data() + sizeof(PLANK_RAW_HID_WIRE_HEADER);
        if (c.type == PLANK_RAW_HID_ATTACH_RESULT) {
            if (!pending) return;
            std::int32_t status;
            std::memcpy(&status, payload, sizeof(status));
            pending = false;
            if (qFromLittleEndian(status) != 0) { release(); retry = Clock::now() + std::chrono::seconds(5); return; }
            attached = true;
            tabletActivity();
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT, "Windows Wacom raw HID attached (%u collections)", unsigned(interfaces.size()));
            return;
        }
        const bool get = c.type == PLANK_RAW_HID_GET_REPORT;
        const auto replyType = c.type == PLANK_RAW_HID_OUTPUT ? std::uint16_t(0) :
            std::uint16_t(get ? PLANK_RAW_HID_GET_REPORT_REPLY : PLANK_RAW_HID_SET_REPORT_REPLY);
        if (c.interfaceId < interfaces.size()) {
            // Open a separate handle for feature/output work. Driver control
            // transfers never stall the input loop or transport callback.
            // A physical USB HID interface can contain multiple Windows
            // top-level collections. hid-wacom mode requests belong to that
            // USB interface, so route a feature ID to its owning collection.
            const unsigned type = get ? payload[1] : payload[0];
            const unsigned id = get ? payload[0] : payload[1];
            const auto index = type == 0 ? 2 : type == 1 ? 1 : 0;
            auto selected = c.interfaceId;
            if (type <= 2 && !interfaces[selected].sizes.bytes[index][id]) {
                for (std::size_t i = 0; i < interfaces.size(); ++i) {
                    if (interfaces[i].interfaceNumber == interfaces[selected].interfaceNumber &&
                            interfaces[i].sizes.bytes[index][id]) {
                        selected = static_cast<std::uint16_t>(i);
                        break;
                    }
                }
            }
            const auto path = interfaces[selected].path;
            const auto sizes = interfaces[selected].sizes;
            std::vector<unsigned char> request(payload, payload + c.size);
            if (reports.submit([c, get, replyType, path, sizes, request = std::move(request)] {
                int result = -1;
                auto transfer = WindowsWacom::reportRequest(sizes, get, request.data(), request.size());
                const auto type = transfer.type;
                const auto length = transfer.reportLength;
                auto& report = transfer.buffer;
                HidHandle handle(SDL_hid_open_path(path.c_str()), SDL_hid_close);
                if (handle && transfer.valid) {
                    if (get && type == 0) result = SDL_hid_get_feature_report(handle.get(), report.data(), report.size());
                    else if (get && type == 2) result = SDL_hid_get_input_report(handle.get(), report.data(), report.size());
                    else if (!get && type == 0) result = SDL_hid_send_feature_report(handle.get(), report.data(), report.size());
                    else if (!get && type == 1) result = SDL_hid_write(handle.get(), report.data(), report.size());
                }
                const bool okay = result >= 0 && (!get || (std::size_t(result) >= length));
                const auto status = qToLittleEndian<std::int32_t>(okay ? 0 : EIO);
                const auto returned = get && okay ? length : 0;
                LinuxWacomReportWorker::Result reply{replyType, c.interfaceId, c.transaction,
                    std::vector<unsigned char>(sizeof(status) + returned)};
                std::memcpy(reply.payload.data(), &status, sizeof(status));
                if (returned) std::memcpy(reply.payload.data() + sizeof(status), report.data(), returned);
                return reply;
            })) return;
        }
        if (replyType) {
            const auto status = qToLittleEndian<std::int32_t>(EBUSY);
            frame(replyType, c.interfaceId, c.transaction, reinterpret_cast<const unsigned char*>(&status), sizeof(status));
        }
    }
    void run() {
        // SDL defaults to enumerating controllers only on some platforms.
        SDL_SetHint(SDL_HINT_HIDAPI_ENUMERATE_ONLY_CONTROLLERS, "0");
        while (!stopping) {
            std::deque<std::vector<unsigned char>> queued;
            {
                std::unique_lock<std::mutex> lock(mutex);
                const auto ticket = lifecycle.pendingTicket();
                if (ticket || overflow) {
                    const bool wasOverflow = overflow;
                    overflow = false;
                    controls.clear();
                    lock.unlock();
                    release();
                    if (wasOverflow) retry = Clock::now() + std::chrono::seconds(1);
                    if (ticket) lifecycle.complete(ticket);
                    lock.lock();
                }
                queued.swap(controls);
            }
            if (lifecycle.canForward()) {
                for (const auto& bytes : queued) {
                    if (!lifecycle.canForward()) break;
                    control(bytes);
                }
                for (const auto& reply : reports.take()) {
                    if (!lifecycle.canForward()) break;
                    if (reply.type && !frame(reply.type, reply.interfaceId, reply.transaction,
                                             reply.payload.data(), reply.payload.size())) release();
                }
                if (pending && Clock::now() >= deadline) {
                    release();
                    retry = Clock::now() + std::chrono::seconds(5);
                }
                if (interfaces.empty() && Clock::now() >= retry) {
                    if (!discover()) { release(); retry = Clock::now() + std::chrono::seconds(2); }
                }
                if (attached) {
                    unsigned char report[PLANK_RAW_HID_MAX_REPORT_SIZE]{};
                    bool failed = false;
                    for (std::size_t i = 0; i < interfaces.size() && !failed; ++i) {
                        const auto& inputSizes = interfaces[i].sizes.bytes[0];
                        if (std::none_of(inputSizes.begin(), inputSizes.end(), [](std::size_t size) { return size != 0; })) continue;
                        for (unsigned count = 0; count < 32 && lifecycle.canForward(); ++count) {
                            const int received = SDL_hid_read_timeout(interfaces[i].handle.get(), report, sizeof(report), 0);
                            if (received == 0) break;
                            if (received < 0) { failed = true; break; }
                            const auto length = WindowsWacom::inputLength(interfaces[i].sizes, report, received);
                            if (!length || !frame(PLANK_RAW_HID_INPUT, static_cast<std::uint16_t>(i), ++sequence, report, length)) {
                                failed = true; break;
                            }
                            tabletActivity();
                        }
                    }
                    if (failed) { release(true); retry = Clock::now() + std::chrono::seconds(2); }
                }
            }
            else if (!interfaces.empty()) release();
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait_for(lock, std::chrono::milliseconds(attached ? 2 : 50), [&] {
                return stopping || lifecycle.pendingTicket() || !controls.empty();
            });
        }
        release();
        lifecycle.markExited();
    }
    std::atomic_bool stopping{false}, shutdownRequested{false}, attached{false}, activeRequested{false};
    MacWacomLifecycle lifecycle;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::vector<unsigned char>> controls;
    bool overflow = false, pending = false;
    std::uint16_t generation = 0;
    std::uint32_t sequence = 0;
    std::vector<Interface> interfaces;
    Clock::time_point deadline{}, retry{};
    LinuxWacomReportWorker reports;
    std::mutex activityMutex;
    std::function<void()> activity;
    std::thread thread;
};
WindowsRawWacomInput::WindowsRawWacomInput(std::function<void()> activity)
    : m_Impl(std::make_shared<Impl>(std::move(activity))) { m_Impl->start(); }
WindowsRawWacomInput::~WindowsRawWacomInput() { m_Impl->shutdown(); }
void WindowsRawWacomInput::setActive(bool active) {
    if (m_Impl->activeRequested.exchange(active) == active) return;
    m_Impl->barrier(m_Impl->lifecycle.setActive(active));
}
void WindowsRawWacomInput::beginReconnect() {
    m_Impl->barrier(m_Impl->lifecycle.beginReconnect());
}
void WindowsRawWacomInput::finishReconnect() {
    m_Impl->barrier(m_Impl->lifecycle.finishReconnect());
}
void WindowsRawWacomInput::handleControl(const unsigned char* data, unsigned int length) {
    MacWacomWire::Control parsed;
    if (!MacWacomWire::parse(data, length, parsed)) return;
    std::lock_guard<std::mutex> lock(m_Impl->mutex);
    if (!m_Impl->lifecycle.canForward()) return;
    if (m_Impl->controls.size() == 64) m_Impl->overflow = true;
    else m_Impl->controls.emplace_back(data, data + length);
    m_Impl->changed.notify_all();
}
bool WindowsRawWacomInput::attached() const {
    return m_Impl->attached.load() && m_Impl->lifecycle.canForward();
}

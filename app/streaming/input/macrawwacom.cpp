#include "macrawwacom.h"
#include "macrawwacomlogic.h"
#include "linuxrawwacom.h" // shared device-family policy; no Linux dependencies
#include <Limelight.h>
#include <SDL3/SDL.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
std::atomic<unsigned> nextGeneration{0};
long number(IOHIDDeviceRef device, CFStringRef key)
{
    CFTypeRef value = IOHIDDeviceGetProperty(device, key);
    long result = 0;
    if (value && CFGetTypeID(value) == CFNumberGetTypeID())
        CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberLongType, &result);
    return result;
}
void stringProperty(IOHIDDeviceRef device, CFStringRef key, char* target, std::size_t capacity)
{
    CFTypeRef value = IOHIDDeviceGetProperty(device, key);
    if (value && CFGetTypeID(value) == CFStringGetTypeID())
        CFStringGetCString(static_cast<CFStringRef>(value), target, capacity, kCFStringEncodingUTF8);
    target[capacity - 1] = 0;
}
std::uint64_t usbGroup(IOHIDDeviceRef device, long& interfaceNumber)
{
    io_registry_entry_t entry = IOHIDDeviceGetService(device);
    IOObjectRetain(entry);
    std::uint64_t group = 0;
    interfaceNumber = -1;
    for (int depth = 0; entry && depth < 16; ++depth) {
        CFTypeRef value = IORegistryEntryCreateCFProperty(entry, CFSTR("bInterfaceNumber"), kCFAllocatorDefault, 0);
        if (value) {
            if (interfaceNumber < 0 && CFGetTypeID(value) == CFNumberGetTypeID())
                CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberLongType, &interfaceNumber);
            CFRelease(value);
        }
        if (IOObjectConformsTo(entry, "IOUSBHostDevice")) {
            IORegistryEntryGetRegistryEntryID(entry, &group);
            break;
        }
        io_registry_entry_t parent = 0;
        IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent);
        IOObjectRelease(entry);
        entry = parent;
    }
    if (entry) IOObjectRelease(entry);
    return group;
}
int errorNumber(IOReturn result)
{
    switch (result) {
    case kIOReturnSuccess: return 0;
    case kIOReturnNotPermitted: case kIOReturnNotPrivileged: return EACCES;
    case kIOReturnExclusiveAccess: case kIOReturnBusy: return EBUSY;
    case kIOReturnNoDevice: case kIOReturnNotAttached: return ENODEV;
    case kIOReturnBadArgument: return EINVAL;
    // Replies carry Linux errno values, not Darwin's different numeric values.
    case kIOReturnUnsupported: return 95; // Linux EOPNOTSUPP
    case kIOReturnTimeout: return 110; // Linux ETIMEDOUT
    case kIOReturnNoMemory: return ENOMEM;
    default: return EIO;
    }
}
}

class MacRawWacomInput::Impl
{
public:
    explicit Impl(std::function<void()> activity) : activity(std::move(activity))
    {
        // Constructor runs on the Client UI thread. Only the normal OS prompt
        // may grant access; never edit privacy databases or run as root.
        if (IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) == kIOHIDAccessTypeUnknown)
            IOHIDRequestAccess(kIOHIDRequestTypeListenEvent);
        worker = std::thread([this] { run(); });
    }
    ~Impl()
    {
        active = false;
        barrier();
        stopping = true;
        worker.join();
    }
    void setActive(bool value)
    {
        active = value;
        if (!value) barrier();
    }
    void beginReconnect() { reconnecting = true; barrier(); }
    void finishReconnect() { barrier(); reconnecting = false; }
    void control(const unsigned char* data, unsigned length)
    {
        MacWacomWire::Control parsed;
        if (!MacWacomWire::parse(data, length, parsed)) return;
        std::lock_guard<std::mutex> lock(mutex);
        if (queue.size() >= 128) { overflow = true; return; }
        queue.emplace_back(data, data + length);
    }
private:
    struct Interface {
        Impl* owner;
        std::uint16_t index;
        IOHIDDeviceRef device;
        std::array<unsigned char, PLANK_RAW_HID_MAX_REPORT_SIZE> buffer{};
        std::array<bool, 256> activityReports{};
        std::vector<unsigned char> descriptor;
    };
    std::function<void()> activity;
    std::atomic<bool> active{false}, reconnecting{false}, stopping{false}, overflow{false};
    std::mutex mutex;
    std::condition_variable completed;
    std::deque<std::vector<unsigned char>> queue;
    std::uint64_t barrierRequested = 0, barrierCompleted = 0;
    std::thread worker;
    std::vector<std::unique_ptr<Interface>> interfaces;
    std::uint16_t generation = 0;
    std::uint32_t sequence = 0;
    bool pending = false, attached = false, ioFailed = false;
    Clock::time_point retry{}, deadline{};

    void barrier()
    {
        std::unique_lock<std::mutex> lock(mutex);
        const auto ticket = ++barrierRequested;
        completed.wait(lock, [this, ticket] { return barrierCompleted >= ticket; });
    }
    bool send(std::uint16_t type, std::uint16_t index, std::uint32_t transaction,
              const unsigned char* payload = nullptr, std::size_t size = 0)
    {
        if (size > PLANK_RAW_HID_MAX_PAYLOAD_SIZE || (size && !payload)) return false;
        PLANK_RAW_HID_WIRE_HEADER h{};
        h.magic = qToLittleEndian(std::uint32_t(PLANK_RAW_HID_WIRE_MAGIC));
        h.version = qToLittleEndian(std::uint16_t(PLANK_RAW_HID_WIRE_VERSION));
        h.type = qToLittleEndian(type); h.interfaceId = qToLittleEndian(index);
        h.generation = qToLittleEndian(generation); h.transactionId = qToLittleEndian(transaction);
        h.payloadLength = qToLittleEndian(std::uint32_t(size));
        std::vector<unsigned char> frame(sizeof(h) + size);
        std::memcpy(frame.data(), &h, sizeof(h));
        if (size) std::memcpy(frame.data() + sizeof(h), payload, size);
        return LiSendRawHidEvent(frame.data(), static_cast<unsigned>(frame.size())) == 0;
    }
    static void input(void* context, IOReturn result, void*, IOHIDReportType,
                      std::uint32_t reportId, unsigned char* bytes, CFIndex size)
    {
        auto& interface = *static_cast<Interface*>(context);
        auto& self = *interface.owner;
        if (result != kIOReturnSuccess || size <= 0 || size > PLANK_RAW_HID_MAX_REPORT_SIZE) {
            self.ioFailed = true; return;
        }
        if (!self.attached || !self.active || self.reconnecting) return;
        if (!self.send(PLANK_RAW_HID_INPUT, interface.index, ++self.sequence, bytes, size)) {
            self.ioFailed = true; return;
        }
        // Battery/status packets must not reclaim the cursor from a real mouse.
        if (reportId < interface.activityReports.size() && interface.activityReports[reportId] && self.activity)
            self.activity();
    }
    static void removed(void* context, IOReturn, void*)
    {
        static_cast<Interface*>(context)->owner->ioFailed = true;
    }
    void release(bool destructive)
    {
        if (pending || attached)
            send(destructive ? PLANK_RAW_HID_DETACH : PLANK_RAW_HID_SUSPEND, 0, 0);
        pending = attached = false;
        for (const auto& i : interfaces) {
            IOHIDDeviceRegisterInputReportCallback(i->device, i->buffer.data(), i->buffer.size(), nullptr, nullptr);
            IOHIDDeviceRegisterRemovalCallback(i->device, nullptr, nullptr);
            IOHIDDeviceUnscheduleFromRunLoop(i->device, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
            IOHIDDeviceClose(i->device, kIOHIDOptionsTypeSeizeDevice);
            CFRelease(i->device);
        }
        if (!interfaces.empty()) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Mac Wacom ownership released");
        interfaces.clear();
        ioFailed = false;
    }
    bool discover()
    {
        if (IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) != kIOHIDAccessTypeGranted) return false;
        IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, 0);
        if (!manager) return false;
        const int vendor = 0x056a;
        CFNumberRef vendorValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vendor);
        const void* keys[] = {CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDTransportKey)};
        const void* values[] = {vendorValue, CFSTR("USB")};
        CFDictionaryRef match = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        IOHIDManagerSetDeviceMatching(manager, match);
        CFRelease(match); CFRelease(vendorValue);
        CFSetRef devices = IOHIDManagerCopyDevices(manager);
        struct Entry { IOHIDDeviceRef device; std::uint64_t parent; long number; };
        std::vector<Entry> candidates;
        if (devices) {
            std::vector<const void*> members(CFSetGetCount(devices));
            CFSetGetValues(devices, members.data());
            for (const void* member : members) {
                IOHIDDeviceRef device = static_cast<IOHIDDeviceRef>(const_cast<void*>(member));
                long index;
                const auto parent = usbGroup(device, index);
                if (parent && index >= 0) candidates.push_back({device, parent, index});
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const Entry& a, const Entry& b) {
            return a.parent == b.parent ? a.number < b.number : a.parent < b.parent;
        });
        bool success = !candidates.empty();
        if (success) {
            const auto parent = candidates.front().parent;
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                [parent](const Entry& e) { return e.parent != parent; }), candidates.end());
            success = candidates.size() <= PLANK_RAW_HID_MAX_INTERFACES;
            const auto product = number(candidates.front().device, CFSTR(kIOHIDProductIDKey));
            success &= plankWacomTransportForUsbDevice(vendor, product) == PlankWacomTransport::ExactRawHid;
            for (const auto& candidate : candidates) {
                if (!success) break;
                CFTypeRef descriptor = IOHIDDeviceGetProperty(candidate.device, CFSTR(kIOHIDReportDescriptorKey));
                if (!descriptor || CFGetTypeID(descriptor) != CFDataGetTypeID() ||
                    CFDataGetLength(static_cast<CFDataRef>(descriptor)) <= 0 ||
                    CFDataGetLength(static_cast<CFDataRef>(descriptor)) > PLANK_RAW_HID_MAX_DESCRIPTOR_SIZE ||
                    number(candidate.device, CFSTR(kIOHIDProductIDKey)) != product) { success = false; break; }
                auto i = std::unique_ptr<Interface>(new Interface{});
                i->owner = this; i->index = static_cast<std::uint16_t>(interfaces.size());
                i->device = candidate.device;
                const auto data = static_cast<CFDataRef>(descriptor);
                i->descriptor.assign(CFDataGetBytePtr(data), CFDataGetBytePtr(data) + CFDataGetLength(data));
                CFArrayRef elements = IOHIDDeviceCopyMatchingElements(i->device, nullptr, 0);
                if (elements) {
                    for (CFIndex n = 0; n < CFArrayGetCount(elements); ++n) {
                        auto element = static_cast<IOHIDElementRef>(const_cast<void*>(CFArrayGetValueAtIndex(elements, n)));
                        const auto kind = IOHIDElementGetType(element);
                        if (kind < kIOHIDElementTypeInput_Misc || kind > kIOHIDElementTypeInput_ScanCodes) continue;
                        const auto page = IOHIDElementGetUsagePage(element), usage = IOHIDElementGetUsage(element);
                        const auto id = IOHIDElementGetReportID(element);
                        const bool control = (page == 1 && (usage == 0x30 || usage == 0x31)) ||
                            page == 9 || page == 0xd || page == 0xff00 ||
                            (page == 0xff0d && (usage < 0x100 || usage == 0x130 || usage == 0x131 ||
                             usage == 0x138 || (usage >= 0x910 && usage <= 0x92f) || usage == 0x995));
                        if (control && id < i->activityReports.size()) i->activityReports[id] = true;
                    }
                    CFRelease(elements);
                }
                const IOReturn result = IOHIDDeviceOpen(i->device, kIOHIDOptionsTypeSeizeDevice);
                if (result != kIOReturnSuccess) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Mac Wacom exclusive open failed: 0x%x", unsigned(result));
                    success = false; break;
                }
                CFRetain(i->device);
                IOHIDDeviceRegisterInputReportCallback(i->device, i->buffer.data(), i->buffer.size(), input, i.get());
                IOHIDDeviceRegisterRemovalCallback(i->device, removed, i.get());
                IOHIDDeviceScheduleWithRunLoop(i->device, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
                interfaces.push_back(std::move(i));
            }
        }
        if (devices) CFRelease(devices);
        CFRelease(manager);
        if (!success) release(false);
        return success;
    }
    bool attach()
    {
        PLANK_RAW_HID_DEVICE_MESSAGE device{};
        IOHIDDeviceRef first = interfaces.front()->device;
        device.interfaceCount = qToLittleEndian(std::uint16_t(interfaces.size()));
        device.bus = qToLittleEndian(std::uint16_t(3)); // Linux BUS_USB
        device.vendor = qToLittleEndian(std::uint32_t(number(first, CFSTR(kIOHIDVendorIDKey))));
        device.product = qToLittleEndian(std::uint32_t(number(first, CFSTR(kIOHIDProductIDKey))));
        device.version = qToLittleEndian(std::uint32_t(number(first, CFSTR(kIOHIDVersionNumberKey))));
        device.country = qToLittleEndian(std::uint32_t(number(first, CFSTR(kIOHIDCountryCodeKey))));
        stringProperty(first, CFSTR(kIOHIDProductKey), device.name, sizeof(device.name));
        stringProperty(first, CFSTR(kIOHIDSerialNumberKey), device.unique, sizeof(device.unique));
        std::snprintf(device.physical, sizeof(device.physical), "plank/mac-usb/%08lx",
                      number(first, CFSTR(kIOHIDLocationIDKey)));
        do { generation = static_cast<std::uint16_t>(++nextGeneration); } while (!generation);
        sequence = 0;
        pending = true;
        deadline = Clock::now() + std::chrono::seconds(3);
        if (!send(PLANK_RAW_HID_DEVICE, 0, 0, reinterpret_cast<unsigned char*>(&device), sizeof(device))) return false;
        for (const auto& i : interfaces)
            if (!send(PLANK_RAW_HID_DESCRIPTOR, i->index, 0, i->descriptor.data(), i->descriptor.size())) return false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Mac Wacom attach sent: %u interfaces, generation %u",
                    unsigned(interfaces.size()), unsigned(generation));
        return true;
    }
    void process(const std::vector<unsigned char>& bytes)
    {
        MacWacomWire::Control c;
        if (!MacWacomWire::parse(bytes.data(), bytes.size(), c) || c.generation != generation ||
            interfaces.empty() || !active || reconnecting) return;
        const auto* payload = bytes.data() + sizeof(PLANK_RAW_HID_WIRE_HEADER);
        if (c.type == PLANK_RAW_HID_ATTACH_RESULT) {
            if (!pending) return;
            std::int32_t status;
            std::memcpy(&status, payload, sizeof(status));
            status = qFromLittleEndian(status);
            if (status != 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Mac Wacom host attach rejected: %d", int(status));
                release(false); retry = Clock::now() + std::chrono::seconds(1); return;
            }
            pending = false; attached = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Mac Wacom attached; exclusive raw HID forwarding active");
            return;
        }
        if (c.interfaceId >= interfaces.size()) return;
        auto& i = *interfaces[c.interfaceId];
        IOReturn result = kIOReturnBadArgument;
        std::vector<unsigned char> reply(sizeof(std::int32_t));
        if (c.type == PLANK_RAW_HID_GET_REPORT) {
            std::array<unsigned char, PLANK_RAW_HID_MAX_REPORT_SIZE> report{};
            const auto prefix = MacWacomWire::reportPrefix(payload[0]);
            report[0] = payload[0];
            CFIndex length = report.size() - prefix;
            const int type = MacWacomWire::ioReportType(payload[1]);
            if (type >= 0) result = IOHIDDeviceGetReport(i.device, static_cast<IOHIDReportType>(type),
                payload[0], report.data() + prefix, &length);
            if (result == kIOReturnSuccess) {
                if (length < 0 || std::size_t(length) > report.size() - prefix) result = kIOReturnOverrun;
                else reply.insert(reply.end(), report.data(), report.data() + length + prefix);
            }
        } else {
            const int type = MacWacomWire::ioReportType(payload[0]);
            const auto prefix = MacWacomWire::reportPrefix(payload[1]);
            if (type >= 0 && c.size > 1 + prefix)
                result = IOHIDDeviceSetReport(i.device, static_cast<IOHIDReportType>(type), payload[1],
                    payload + 1 + prefix, c.size - 1 - prefix);
        }
        const auto error = qToLittleEndian(std::int32_t(errorNumber(result)));
        std::memcpy(reply.data(), &error, sizeof(error));
        if (c.type != PLANK_RAW_HID_OUTPUT)
            send(c.type == PLANK_RAW_HID_GET_REPORT ? PLANK_RAW_HID_GET_REPORT_REPLY : PLANK_RAW_HID_SET_REPORT_REPLY,
                 c.interfaceId, c.transaction, reply.data(), reply.size());
        if (result != kIOReturnSuccess)
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Mac Wacom report I/O failed: interface %u type %u result 0x%x",
                        unsigned(c.interfaceId), unsigned(c.type), unsigned(result));
        if (errorNumber(result) == ENODEV) ioFailed = true;
    }
    void run()
    {
        while (!stopping) {
            std::deque<std::vector<unsigned char>> controls;
            std::uint64_t barrierTicket;
            {
                std::lock_guard<std::mutex> lock(mutex);
                barrierTicket = barrierRequested;
                controls.swap(queue);
            }
            if (barrierTicket != barrierCompleted) {
                release(false);
                controls.clear();
                std::lock_guard<std::mutex> lock(mutex);
                barrierCompleted = barrierTicket;
                completed.notify_all();
            }
            if (!active || reconnecting) release(false);
            else {
                if (overflow.exchange(false)) { release(true); retry = Clock::now() + std::chrono::seconds(1); }
                for (const auto& bytes : controls) process(bytes);
                if (ioFailed) release(true);
                if (pending && Clock::now() >= deadline) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Mac Wacom attachment timed out");
                    release(false); retry = Clock::now() + std::chrono::seconds(1);
                }
                if (interfaces.empty() && Clock::now() >= retry) {
                    if (discover() && !attach()) release(false);
                    retry = Clock::now() + std::chrono::seconds(1);
                }
            }
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        release(false);
    }
};

MacRawWacomInput::MacRawWacomInput(std::function<void()> activity) : m_Impl(new Impl(std::move(activity))) {}
MacRawWacomInput::~MacRawWacomInput() = default;
void MacRawWacomInput::setActive(bool active) { m_Impl->setActive(active); }
void MacRawWacomInput::beginReconnect() { m_Impl->beginReconnect(); }
void MacRawWacomInput::finishReconnect() { m_Impl->finishReconnect(); }
void MacRawWacomInput::handleControl(const unsigned char* data, unsigned int length) { m_Impl->control(data, length); }

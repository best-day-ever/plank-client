#include "linuxrawwacom.h"

#include <Limelight.h>
#include <StationConnect.h>
#include <SDL.h>
#include <QtEndian>
#include <libudev.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <linux/uhid.h>
#include <poll.h>
#include <set>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

const unsigned int kWacomVendorId = 0x056a;
std::atomic<std::uint32_t> s_NextGeneration(0);

std::uint16_t nextGeneration()
{
    std::uint16_t generation;
    do {
        generation = static_cast<std::uint16_t>(
            s_NextGeneration.fetch_add(1, std::memory_order_relaxed) + 1);
    } while (generation == 0);
    return generation;
}

std::string usbParentPath(udev_device* device)
{
    udev_device* parent = udev_device_get_parent_with_subsystem_devtype(
        device, "usb", "usb_device");
    const char* path = parent != nullptr ? udev_device_get_syspath(parent) : nullptr;
    return path != nullptr ? path : "";
}

bool isWacomUsbDevice(udev_device* device)
{
    udev_device* parent = udev_device_get_parent_with_subsystem_devtype(
        device, "usb", "usb_device");
    const char* vendor = parent != nullptr ?
        udev_device_get_sysattr_value(parent, "idVendor") : nullptr;
    if (vendor == nullptr) {
        return false;
    }
    char* end = nullptr;
    const unsigned long value = std::strtoul(vendor, &end, 16);
    return end != vendor && *end == '\0' && value == kWacomVendorId;
}

unsigned long getReportIoctl(std::uint8_t type, std::size_t size)
{
    switch (type) {
    case UHID_FEATURE_REPORT:
        return HIDIOCGFEATURE(size);
    case UHID_OUTPUT_REPORT:
        return HIDIOCGOUTPUT(size);
    case UHID_INPUT_REPORT:
        return HIDIOCGINPUT(size);
    default:
        return 0;
    }
}

unsigned long setReportIoctl(std::uint8_t type, std::size_t size)
{
    switch (type) {
    case UHID_FEATURE_REPORT:
        return HIDIOCSFEATURE(size);
    case UHID_OUTPUT_REPORT:
        return HIDIOCSOUTPUT(size);
    case UHID_INPUT_REPORT:
        return HIDIOCSINPUT(size);
    default:
        return 0;
    }
}

template<typename T>
T readLittle(const T& source)
{
    T wire;
    std::memcpy(&wire, &source, sizeof(wire));
    return qFromLittleEndian(wire);
}

template<typename T>
void writeLittle(T& destination, T value)
{
    value = qToLittleEndian(value);
    std::memcpy(&destination, &value, sizeof(value));
}

} // namespace

LinuxRawWacomInput::LinuxRawWacomInput()
    : m_Active(false),
      m_Stopping(false),
      m_AttachFailed(false),
      m_Generation(0),
      m_InputSequence(0),
      m_AttachPending(false),
      m_Attached(false)
{
    m_Thread = std::thread(&LinuxRawWacomInput::run, this);
}

LinuxRawWacomInput::~LinuxRawWacomInput()
{
    m_Active.store(false);
    m_Stopping.store(true);
    if (m_Thread.joinable()) {
        m_Thread.join();
    }
}

void LinuxRawWacomInput::setActive(bool active)
{
    if (!active) {
        m_Active.store(false);
        std::lock_guard<std::recursive_mutex> lock(m_Mutex);
        suspendForFocusLoss();
        return;
    }

    if (!m_Active.exchange(true)) {
        m_AttachFailed.store(false);
    }
}

void LinuxRawWacomInput::resetAfterReconnect()
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);

    // A StationConnect desktop handoff replaces the host media worker and its
    // UHID devices. The physical tablet remains open on the client, so force a
    // fresh discovery/attach transaction on the replacement control stream.
    // Do not send DETACH: the new host generation has never seen this device.
    release(false);
    m_AttachFailed.store(false);
}

void LinuxRawWacomInput::run()
{
    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                "StationConnect exact raw Wacom capture initialized");
    while (!m_Stopping.load()) {
        if (!m_Active.load()) {
            {
                std::lock_guard<std::recursive_mutex> lock(m_Mutex);
                if (!m_Interfaces.empty()) {
                    suspendForFocusLoss();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        bool delayRetry = false;
        {
            std::lock_guard<std::recursive_mutex> lock(m_Mutex);
            if (m_Interfaces.empty()) {
                if (m_AttachFailed.exchange(false)) {
                    delayRetry = true;
                }
                else if (!discover() || !sendAttach()) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                                "Unable to attach exact Wacom device; will retry after checking hidraw and input permissions");
                    release(false);
                    m_AttachFailed.store(true);
                }
            }
        }

        if (delayRetry) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        handlePhysicalReports();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    std::lock_guard<std::recursive_mutex> lock(m_Mutex);

    // Normal stream teardown may be followed by a resume into the same host
    // application. Release the physical tablet locally, but let the host keep
    // its stable UHID/XInput endpoints.
    release(false);
}

bool LinuxRawWacomInput::discover()
{
    udev* context = udev_new();
    if (context == nullptr) {
        return false;
    }

    std::vector<std::pair<std::string, std::string> > candidates;
    udev_enumerate* enumerate = udev_enumerate_new(context);
    udev_enumerate_add_match_subsystem(enumerate, "hidraw");
    udev_enumerate_scan_devices(enumerate);
    udev_list_entry* devices = udev_enumerate_get_list_entry(enumerate);
    udev_list_entry* entry = nullptr;
    udev_list_entry_foreach(entry, devices) {
        udev_device* device = udev_device_new_from_syspath(
            context, udev_list_entry_get_name(entry));
        const char* node = device != nullptr ? udev_device_get_devnode(device) : nullptr;
        if (device != nullptr && node != nullptr && isWacomUsbDevice(device)) {
            candidates.push_back(std::make_pair(usbParentPath(device), node));
        }
        if (device != nullptr) {
            udev_device_unref(device);
        }
    }
    udev_enumerate_unref(enumerate);

    if (candidates.empty()) {
        udev_unref(context);
        return false;
    }
    std::sort(candidates.begin(), candidates.end());
    const std::string selectedParent = candidates.front().first;
    const std::size_t selectedCount = static_cast<std::size_t>(std::count_if(
        candidates.begin(), candidates.end(),
        [&selectedParent](const std::pair<std::string, std::string>& candidate) {
            return candidate.first == selectedParent;
        }));
    if (selectedCount == 0 || selectedCount > SC_RAW_HID_MAX_INTERFACES) {
        udev_unref(context);
        return false;
    }

    hidraw_devinfo expectedInfo = {};
    for (const auto& candidate : candidates) {
        if (candidate.first != selectedParent) {
            continue;
        }
        const int fd = open(candidate.second.c_str(),
                            O_RDWR | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) {
            udev_unref(context);
            return false;
        }
        hidraw_devinfo info = {};
        if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0 ||
                (m_Interfaces.empty() ? false :
                 info.bustype != expectedInfo.bustype ||
                 info.vendor != expectedInfo.vendor ||
                 info.product != expectedInfo.product)) {
            close(fd);
            udev_unref(context);
            return false;
        }
        if (m_Interfaces.empty()) {
            expectedInfo = info;
        }
        int descriptorSize = 0;
        if (ioctl(fd, HIDIOCGRDESCSIZE, &descriptorSize) < 0 ||
                descriptorSize <= 0 ||
                descriptorSize > static_cast<int>(SC_RAW_HID_MAX_DESCRIPTOR_SIZE)) {
            close(fd);
            udev_unref(context);
            return false;
        }
        hidraw_report_descriptor descriptor = {};
        descriptor.size = descriptorSize;
        if (ioctl(fd, HIDIOCGRDESC, &descriptor) < 0) {
            close(fd);
            udev_unref(context);
            return false;
        }
        HidInterface interface;
        interface.fd = fd;
        interface.descriptor.assign(descriptor.value,
                                    descriptor.value + descriptor.size);
        m_Interfaces.push_back(interface);
    }

    enumerate = udev_enumerate_new(context);
    udev_enumerate_add_match_subsystem(enumerate, "input");
    udev_enumerate_scan_devices(enumerate);
    devices = udev_enumerate_get_list_entry(enumerate);
    udev_list_entry_foreach(entry, devices) {
        udev_device* device = udev_device_new_from_syspath(
            context, udev_list_entry_get_name(entry));
        const char* node = device != nullptr ? udev_device_get_devnode(device) : nullptr;
        if (device != nullptr && node != nullptr &&
                usbParentPath(device) == selectedParent &&
                std::strncmp(node, "/dev/input/event", 16) == 0) {
            const int fd = open(node, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
            if (fd >= 0) {
                m_EventFds.push_back(fd);
            }
        }
        if (device != nullptr) {
            udev_device_unref(device);
        }
    }
    udev_enumerate_unref(enumerate);
    udev_unref(context);

    return !m_Interfaces.empty() && !m_EventFds.empty();
}

bool LinuxRawWacomInput::sendAttach()
{
    SC_RAW_HID_DEVICE_MESSAGE device = {};
    device.interfaceCount = qToLittleEndian(
        static_cast<std::uint16_t>(m_Interfaces.size()));

    hidraw_devinfo info = {};
    if (ioctl(m_Interfaces.front().fd, HIDIOCGRAWINFO, &info) < 0) {
        return false;
    }
    device.bus = qToLittleEndian(static_cast<std::uint16_t>(info.bustype));
    device.vendor = qToLittleEndian(static_cast<std::uint32_t>(info.vendor));
    device.product = qToLittleEndian(static_cast<std::uint32_t>(info.product));

    std::array<char, 128> name = {};
    std::array<char, 64> physical = {};
    std::array<char, 64> unique = {};
    ioctl(m_Interfaces.front().fd, HIDIOCGRAWNAME(name.size()), name.data());
    ioctl(m_Interfaces.front().fd, HIDIOCGRAWPHYS(physical.size()), physical.data());
    ioctl(m_Interfaces.front().fd, HIDIOCGRAWUNIQ(unique.size()), unique.data());
    std::memcpy(device.name, name.data(), sizeof(device.name));
    std::memcpy(device.physical, physical.data(), sizeof(device.physical));
    std::memcpy(device.unique, unique.data(), sizeof(device.unique));

    input_id inputIdentity = {};
    if (!m_EventFds.empty() &&
            ioctl(m_EventFds.front(), EVIOCGID, &inputIdentity) == 0) {
        device.version = qToLittleEndian(
            static_cast<std::uint32_t>(inputIdentity.version));
    }

    m_Generation = nextGeneration();
    m_InputSequence = 0;
    if (!sendFrame(SC_RAW_HID_DEVICE, 0, 0,
                   reinterpret_cast<const unsigned char*>(&device),
                   sizeof(device))) {
        return false;
    }
    for (std::size_t index = 0; index < m_Interfaces.size(); ++index) {
        const std::vector<unsigned char>& descriptor = m_Interfaces[index].descriptor;
        if (!sendFrame(SC_RAW_HID_DESCRIPTOR,
                       static_cast<std::uint16_t>(index), 0,
                       descriptor.data(), descriptor.size())) {
            return false;
        }
    }
    m_AttachPending = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                "Sent exact Wacom attach: %u interfaces, generation %u",
                static_cast<unsigned int>(m_Interfaces.size()),
                static_cast<unsigned int>(m_Generation));
    return true;
}

bool LinuxRawWacomInput::sendFrame(std::uint16_t type,
                                   std::uint16_t interfaceId,
                                   std::uint32_t transactionId,
                                   const unsigned char* payload,
                                   std::size_t payloadLength)
{
    if (payloadLength > SC_RAW_HID_MAX_PAYLOAD_SIZE ||
            (payloadLength != 0 && payload == nullptr)) {
        return false;
    }
    std::vector<unsigned char> frame(sizeof(SC_RAW_HID_WIRE_HEADER) + payloadLength);
    SC_RAW_HID_WIRE_HEADER header = {};
    writeLittle(header.magic, static_cast<std::uint32_t>(SC_RAW_HID_WIRE_MAGIC));
    writeLittle(header.version, static_cast<std::uint16_t>(SC_RAW_HID_WIRE_VERSION));
    writeLittle(header.type, type);
    writeLittle(header.interfaceId, interfaceId);
    writeLittle(header.generation, m_Generation);
    writeLittle(header.transactionId, transactionId);
    writeLittle(header.payloadLength, static_cast<std::uint32_t>(payloadLength));
    std::memcpy(frame.data(), &header, sizeof(header));
    if (payloadLength != 0) {
        std::memcpy(frame.data() + sizeof(header), payload, payloadLength);
    }
    return LiSendRawHidEvent(frame.data(), static_cast<unsigned int>(frame.size())) == 0;
}

void LinuxRawWacomInput::handlePhysicalReports()
{
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    if (!m_Attached || m_Interfaces.empty()) {
        return;
    }

    std::vector<pollfd> pollFds;
    for (const HidInterface& interface : m_Interfaces) {
        pollfd descriptor = {interface.fd, POLLIN, 0};
        pollFds.push_back(descriptor);
    }
    const int result = poll(pollFds.data(), pollFds.size(), 0);
    if (result < 0 && errno != EINTR) {
        release(true);
        m_AttachFailed.store(false);
        return;
    }

    std::array<unsigned char, SC_RAW_HID_MAX_REPORT_SIZE> report = {};
    for (std::size_t index = 0; index < pollFds.size(); ++index) {
        if ((pollFds[index].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            release(true);
            m_AttachFailed.store(false);
            return;
        }
        if ((pollFds[index].revents & POLLIN) == 0) {
            continue;
        }
        const ssize_t bytes = read(pollFds[index].fd, report.data(), report.size());
        if (bytes > 0) {
            sendFrame(SC_RAW_HID_INPUT, static_cast<std::uint16_t>(index),
                      ++m_InputSequence, report.data(),
                      static_cast<std::size_t>(bytes));
        }
    }
}

void LinuxRawWacomInput::handleControl(const unsigned char* data,
                                       unsigned int length)
{
    if (data == nullptr || length < sizeof(SC_RAW_HID_WIRE_HEADER)) {
        return;
    }
    SC_RAW_HID_WIRE_HEADER header;
    std::memcpy(&header, data, sizeof(header));
    const std::uint32_t payloadLength = readLittle(header.payloadLength);
    if (readLittle(header.magic) != SC_RAW_HID_WIRE_MAGIC ||
            readLittle(header.version) != SC_RAW_HID_WIRE_VERSION ||
            payloadLength > SC_RAW_HID_MAX_PAYLOAD_SIZE ||
            length != sizeof(header) + payloadLength) {
        return;
    }

    const std::uint16_t type = readLittle(header.type);
    const std::uint16_t interfaceId = readLittle(header.interfaceId);
    const std::uint16_t generation = readLittle(header.generation);
    const std::uint32_t transactionId = readLittle(header.transactionId);
    const unsigned char* payload = data + sizeof(header);

    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    if (generation != m_Generation || m_Interfaces.empty()) {
        return;
    }
    if (type == SC_RAW_HID_ATTACH_RESULT && payloadLength == sizeof(std::int32_t)) {
        std::int32_t result;
        std::memcpy(&result, payload, sizeof(result));
        result = qFromLittleEndian(result);
        m_AttachPending = false;
        if (result == 0) {
            setGrabbed(true);
            m_Attached = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                        "Exact Wacom device attached to host");
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                        "Host rejected exact Wacom attach: %s",
                        std::strerror(result));
            release(false);
            m_AttachFailed.store(true);
        }
    }
    else if (type == SC_RAW_HID_GET_REPORT) {
        handleGetReport(interfaceId, transactionId, payload, payloadLength);
    }
    else if (type == SC_RAW_HID_SET_REPORT || type == SC_RAW_HID_OUTPUT) {
        handleSetReport(type, interfaceId, transactionId, payload, payloadLength);
    }
}

void LinuxRawWacomInput::handleGetReport(std::uint16_t interfaceId,
                                         std::uint32_t transactionId,
                                         const unsigned char* payload,
                                         std::size_t payloadLength)
{
    if (interfaceId >= m_Interfaces.size() || payloadLength != 2) {
        return;
    }
    std::array<unsigned char, SC_RAW_HID_MAX_REPORT_SIZE> report = {};
    report[0] = payload[0];
    const unsigned long request = getReportIoctl(payload[1], report.size());
    errno = 0;
    const int result = request != 0 ?
        ioctl(m_Interfaces[interfaceId].fd, request, report.data()) : -1;
    const std::int32_t error = result < 0 ? (request == 0 ? EINVAL : errno) : 0;
    const std::int32_t littleError = qToLittleEndian(error);
    std::vector<unsigned char> reply(sizeof(littleError) + std::max(result, 0));
    std::memcpy(reply.data(), &littleError, sizeof(littleError));
    if (result > 0) {
        std::memcpy(reply.data() + sizeof(littleError), report.data(), result);
    }
    sendFrame(SC_RAW_HID_GET_REPORT_REPLY, interfaceId, transactionId,
              reply.data(), reply.size());
}

void LinuxRawWacomInput::handleSetReport(std::uint16_t type,
                                         std::uint16_t interfaceId,
                                         std::uint32_t transactionId,
                                         const unsigned char* payload,
                                         std::size_t payloadLength)
{
    if (interfaceId >= m_Interfaces.size() || payloadLength < 2) {
        return;
    }
    const unsigned long request = setReportIoctl(payload[0], payloadLength - 1);
    errno = 0;
    const int result = request != 0 ?
        ioctl(m_Interfaces[interfaceId].fd, request,
              const_cast<unsigned char*>(payload + 1)) : -1;
    if (type == SC_RAW_HID_SET_REPORT) {
        const std::int32_t error = result < 0 ?
            (request == 0 ? EINVAL : errno) : 0;
        const std::int32_t littleError = qToLittleEndian(error);
        sendFrame(SC_RAW_HID_SET_REPORT_REPLY, interfaceId, transactionId,
                  reinterpret_cast<const unsigned char*>(&littleError),
                  sizeof(littleError));
    }
}

void LinuxRawWacomInput::setGrabbed(bool grabbed)
{
    const int value = grabbed ? 1 : 0;
    for (int fd : m_EventFds) {
        if (ioctl(fd, EVIOCGRAB, value) < 0 && errno != ENODEV) {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                        "Unable to %s Wacom event node: %s",
                        grabbed ? "grab" : "release", std::strerror(errno));
        }
    }
}

void LinuxRawWacomInput::suspendForFocusLoss()
{
    if (m_AttachPending || m_Attached) {
        if (sendFrame(SC_RAW_HID_SUSPEND, 0, 0, nullptr, 0)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                        "Suspended exact Wacom forwarding while preserving host endpoints");
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                        "Unable to suspend exact Wacom forwarding cleanly");
        }
    }
    release(false);
}

void LinuxRawWacomInput::release(bool notifyHost)
{
    if (notifyHost && (m_AttachPending || m_Attached)) {
        sendFrame(SC_RAW_HID_DETACH, 0, 0, nullptr, 0);
    }
    if (m_Attached) {
        setGrabbed(false);
    }
    for (int fd : m_EventFds) {
        close(fd);
    }
    for (const HidInterface& interface : m_Interfaces) {
        close(interface.fd);
    }
    m_EventFds.clear();
    m_Interfaces.clear();
    m_AttachPending = false;
    m_Attached = false;
}

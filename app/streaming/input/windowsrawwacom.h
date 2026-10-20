#pragma once
#include <functional>
#include <memory>

// Uses SDL3's Windows HIDAPI backend, including report-descriptor reconstruction.
// The capture worker owns HID handles; the network callback only queues records.
class WindowsRawWacomInput
{
public:
    explicit WindowsRawWacomInput(std::function<void()> tabletActivity);
    ~WindowsRawWacomInput();
    void setActive(bool active);
    void beginReconnect();
    void finishReconnect();
    void handleControl(const unsigned char* data, unsigned int length);
    bool attached() const;
private:
    class Impl;
    std::shared_ptr<Impl> m_Impl;
};

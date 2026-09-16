#pragma once

#include <functional>
#include <memory>

// Physical HID ownership and I/O run on one private CFRunLoop. Lifecycle calls
// complete their release barrier before the stream is allowed to disconnect.
class MacRawWacomInput
{
public:
    explicit MacRawWacomInput(std::function<void()> tabletActivity);
    ~MacRawWacomInput();
    void setActive(bool active);
    void beginReconnect();
    void finishReconnect();
    void handleControl(const unsigned char* data, unsigned int length);
private:
    class Impl;
    std::unique_ptr<Impl> m_Impl;
};

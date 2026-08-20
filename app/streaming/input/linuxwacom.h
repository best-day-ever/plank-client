#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

struct libinput_event_tablet_tool;

class LinuxWacomInput
{
public:
    LinuxWacomInput();
    ~LinuxWacomInput();

    LinuxWacomInput(const LinuxWacomInput&) = delete;
    LinuxWacomInput& operator=(const LinuxWacomInput&) = delete;

    void setActive(bool active);

private:
    static int openRestricted(const char* path, int flags, void* userData);
    static void closeRestricted(int fd, void* userData);

    void run();
    void handleTabletEvent(libinput_event_tablet_tool* event, unsigned int type);
    void updateGrab(int fd, bool grabbed);
    bool isWacomPenNode(int fd) const;
    void cancelRemotePen();

    std::atomic<bool> m_Active;
    std::atomic<bool> m_Stopping;
    std::thread m_Thread;
    std::mutex m_DeviceMutex;
    std::mutex m_StateMutex;
    std::vector<int> m_PenFds;

    bool m_TipDown;
    unsigned char m_ToolType;
    unsigned char m_Buttons;
    float m_X;
    float m_Y;
    float m_Pressure;
    float m_Distance;
    unsigned short m_Rotation;
    unsigned char m_Tilt;
};

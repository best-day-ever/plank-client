#pragma once
#include "videoitem.h"
#include "vaapivideoframe.h"
#include "softwarevideodevice.h"

class QtEglVideoItem final : public QtVideoItem
{
public:
    using QtVideoItem::QtVideoItem;
    void setSoftwareDevice(QtSoftwareVideoDevice::Ptr device);
protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
private:
    QtSoftwareVideoDevice::Ptr m_SoftwareDevice;
};

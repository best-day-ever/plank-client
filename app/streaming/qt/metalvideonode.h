#pragma once
#include "videoitem.h"

class QtMetalVideoItem final : public QtVideoItem
{
public:
    using QtVideoItem::QtVideoItem;
protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
};

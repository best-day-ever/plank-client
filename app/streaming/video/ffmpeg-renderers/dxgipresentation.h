#pragma once

#include <dxgi1_6.h>

// A full swap-chain queue is backpressure, not a lost graphics device. Keep
// the context serialized, but never ask DXGI to sleep waiting for that queue.
class DxgiPresentation
{
public:
    enum Result { Accepted, Deferred, Occluded, RequestReset, ResetPending };

    template<typename Present>
    Result submit(bool allowTearing, Present present)
    {
        const UINT flags = DXGI_PRESENT_DO_NOT_WAIT |
                (allowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
        m_LastResult = present(0, flags);
        if (m_LastResult == DXGI_ERROR_WAS_STILL_DRAWING) return Deferred;
        if (m_LastResult == DXGI_STATUS_OCCLUDED) return Occluded;
        if (FAILED(m_LastResult)) {
            if (m_ResetRequested) return ResetPending;
            return RequestReset;
        }
        return Accepted;
    }

    HRESULT lastResult() const { return m_LastResult; }
    // A full SDL event queue must not consume the reset request.
    void resetQueued() { m_ResetRequested = true; }

private:
    HRESULT m_LastResult = S_OK;
    bool m_ResetRequested = false;
};

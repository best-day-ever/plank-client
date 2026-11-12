#include <QtTest>
#include <dxgi1_6.h>
#include "streaming/video/ffmpeg-renderers/dxgipresentation.h"

class TestDxgiPresentation : public QObject
{
    Q_OBJECT
private slots:
    void queueBackpressureIsNotDeviceFailure()
    {
        DxgiPresentation policy;
        unsigned calls = 0;
        UINT observedInterval = 99, observedFlags = 0;
        auto busy = [&](UINT interval, UINT flags) {
            ++calls;
            observedInterval = interval;
            observedFlags = flags;
            return DXGI_ERROR_WAS_STILL_DRAWING;
        };
        for (int i = 0; i < 100; ++i) {
            QCOMPARE(policy.submit(false, busy), DxgiPresentation::Deferred);
        }
        QCOMPARE(calls, 100u);
        QCOMPARE(observedInterval, 0u);
        QCOMPARE(observedFlags, UINT(DXGI_PRESENT_DO_NOT_WAIT));
        QCOMPARE(policy.submit(false, [](UINT, UINT) { return S_OK; }), DxgiPresentation::Accepted);
    }

    void tearingAndOcclusionPreservePresentationPolicy()
    {
        DxgiPresentation policy;
        UINT observed = 0;
        QCOMPARE(policy.submit(true, [&](UINT, UINT flags) {
            observed = flags;
            return DXGI_STATUS_OCCLUDED;
        }), DxgiPresentation::Occluded);
        QCOMPARE(observed, UINT(DXGI_PRESENT_DO_NOT_WAIT | DXGI_PRESENT_ALLOW_TEARING));
        QCOMPARE(policy.submit(true, [](UINT, UINT) { return S_OK; }), DxgiPresentation::Accepted);
    }

    void oneResetPerRenderer()
    {
        DxgiPresentation policy;
        QCOMPARE(policy.submit(false, [](UINT, UINT) { return DXGI_ERROR_DEVICE_REMOVED; }), DxgiPresentation::RequestReset);
        QCOMPARE(policy.lastResult(), HRESULT(DXGI_ERROR_DEVICE_REMOVED));
        // Failure to enqueue a reset leaves it pending for the next frame.
        QCOMPARE(policy.submit(false, [](UINT, UINT) { return DXGI_ERROR_DEVICE_RESET; }), DxgiPresentation::RequestReset);
        policy.resetQueued();
        QCOMPARE(policy.submit(false, [](UINT, UINT) { return DXGI_ERROR_DEVICE_RESET; }), DxgiPresentation::ResetPending);
        DxgiPresentation replacement;
        QCOMPARE(replacement.submit(false, [](UINT, UINT) { return S_OK; }), DxgiPresentation::Accepted);
        QCOMPARE(replacement.submit(false, [](UINT, UINT) { return E_FAIL; }), DxgiPresentation::RequestReset);
    }
};

QTEST_GUILESS_MAIN(TestDxgiPresentation)
#include "test_dxgipresentation.moc"

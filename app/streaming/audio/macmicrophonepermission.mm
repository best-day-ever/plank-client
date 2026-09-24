#include "macmicrophonepermission.h"
#import <AVFoundation/AVFoundation.h>
#include <atomic>

int plankMacMicrophonePermission(bool request)
{
    @autoreleasepool {
        const auto status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
        if (status == AVAuthorizationStatusAuthorized) return 1;
        if (status != AVAuthorizationStatusNotDetermined) return -1;
        static std::atomic<bool> pending {false};
        if (request && !pending.exchange(true)) {
            // No Qt event-loop wait: the stream's native SDL event pump remains
            // responsive while macOS presents its normal permission prompt.
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL allowed) {
                (void)allowed;
                pending.store(false);
            }];
        }
        return 0;
    }
}

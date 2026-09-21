#pragma once

// Request settings shared by every PLANK host HTTPS call. Header-only so the
// broker test suite can exercise them without linking NvHTTP.

#include <QNetworkRequest>

namespace PlankHttp {

// The PLANK host closes the TLS connection after every response but still
// answers HTTP/1.1 without "Connection: close". Qt then treats the socket as
// reusable and can hand it to a request issued a few milliseconds later (the
// brokered flow posts /plank/auth/start right after /serverinfo), which fails
// with RemoteHostClosedError. Asking for one request per connection matches
// what the host does anyway and keeps every request on its own handshake, so
// the pin check on QNetworkAccessManager::encrypted always runs before bytes
// such as a GSSAPI token or bearer are written.
inline void prepareOneShotRequest(QNetworkRequest& request)
{
    request.setRawHeader("Connection", "close");
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setAttribute(QNetworkRequest::ConnectionCacheExpiryTimeoutSecondsAttribute, 0);
}

}

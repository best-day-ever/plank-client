#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Native clipboard calls run on the SDL main thread. A missing snapshot means
// temporary access failure, so polling retries instead of consuming the change.
namespace ClipboardPlatform {
std::optional<std::string> readText();
bool writeText(const std::vector<std::uint8_t>& bytes);
std::int64_t changeCount();
void clearIfMatches(std::int64_t sequence, const std::string& text);
void runOnMainThread(std::function<void()> callback);
}

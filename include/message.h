#pragma once

/// @file message.h
/// @brief WebSocket message types used across the project.

#include <cstdint>
#include <string>
#include <vector>

namespace ws {

/// @brief Represents a WebSocket message (text or binary).
///
/// Uses a tagged union approach via bool flag. The payload is stored
/// as a string for simplicity (binary data fits in std::string since
/// it's just a byte container).
struct Message {
    std::string payload;     ///< Message content (text or raw bytes)
    bool        is_text;     ///< true = text frame, false = binary frame

    Message() : is_text(true) {}
    Message(std::string data, bool text)
        : payload(std::move(data)), is_text(text) {}
};

}  // namespace ws

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ws {

struct Message {
    std::string payload;
    bool        is_text;

    Message() : is_text(true) {}
    Message(std::string data, bool text)
        : payload(std::move(data)), is_text(text) {}
};

}  // namespace ws

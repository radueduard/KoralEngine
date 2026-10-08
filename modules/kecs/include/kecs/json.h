#pragma once

// Just enough JSON for a saved world: read into a tree whose every value remembers the text it came from, so a
// field's value can be handed to its codec as written.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kecs::json
{
    struct Value {
        enum class Kind { eNull, eBool, eNumber, eString, eArray, eObject };
        Kind kind = Kind::eNull;
        std::string_view raw;       ///< The value's own text, in what was parsed.
        bool boolean = false;
        double number = 0.0;
        std::string string;         ///< Unescaped.
        std::vector<Value> items;
        std::vector<std::pair<std::string, Value>> members;

        [[nodiscard]] const Value* Find(std::string_view key) const;
    };

    /** Parses @p text, which must outlive @p out. False, with @p error saying where, when it is not JSON. */
    bool Parse(std::string_view text, Value& out, std::string& error);
    /** @p text as a JSON string, quotes and all. */
    std::string Quote(std::string_view text);
}

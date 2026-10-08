#pragma once

/**
 * @file value.h
 * @brief A value a node's input holds, or a value node makes: nothing, a truth, a number, text, a list, or named
 *        members. What JSON can say, and no more, so a graph is saved as it is.
 *
 * A vector is a list of numbers. An enumeration is the name of one of its values ("eRGBA8_UNORM"), flags a list of
 * names; a struct is named members.
 */

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace kecs::json { struct Value; }

namespace kgraph
{
    struct Value {
        using List = std::vector<Value>;
        using Members = std::vector<std::pair<std::string, Value>>;
        std::variant<std::monostate, bool, double, std::string, List, Members> data;

        Value() = default;
        Value(const bool v) : data(v) {}                                    // NOLINT(*-explicit-constructor)
        template <typename T> requires (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
        Value(const T v) : data(static_cast<double>(v)) {}                  // NOLINT(*-explicit-constructor)
        Value(const char* v) : data(std::string(v)) {}                      // NOLINT(*-explicit-constructor)
        Value(std::string v) : data(std::move(v)) {}                        // NOLINT(*-explicit-constructor)
        Value(List v) : data(std::move(v)) {}                               // NOLINT(*-explicit-constructor)
        Value(Members v) : data(std::move(v)) {}                            // NOLINT(*-explicit-constructor)

        [[nodiscard]] bool IsNull() const { return std::holds_alternative<std::monostate>(data); }
        [[nodiscard]] const bool* Bool() const { return std::get_if<bool>(&data); }
        [[nodiscard]] const double* Number() const { return std::get_if<double>(&data); }
        [[nodiscard]] const std::string* String() const { return std::get_if<std::string>(&data); }
        [[nodiscard]] const List* Items() const { return std::get_if<List>(&data); }
        [[nodiscard]] const Members* Named() const { return std::get_if<Members>(&data); }
        /** @brief The member so named, or null. */
        [[nodiscard]] const Value* Find(std::string_view name) const;
        bool operator==(const Value&) const = default;

        /** @brief As JSON text, on one line. */
        [[nodiscard]] std::string ToJson() const;
        [[nodiscard]] static Value FromJson(const kecs::json::Value& json);
    };
}

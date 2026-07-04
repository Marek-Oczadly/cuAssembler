#pragma once

#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace CuAsm {

/**
 * @brief Minimal recursive JSON value type (null/bool/int/double/string/array/object),
 *        used as the generic container that IntVal2Hex/HexVal2Int recurse over, mirroring
 *        the dynamically-typed dict/list/int/str values the Python implementation operates on.
 */
class JsonValue {
public:
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;

    JsonValue() : m_data(nullptr) {}
    JsonValue(std::nullptr_t) : m_data(nullptr) {}
    JsonValue(bool v) : m_data(v) {}
    JsonValue(long long v) : m_data(v) {}
    JsonValue(int v) : m_data(static_cast<long long>(v)) {}
    JsonValue(double v) : m_data(v) {}
    JsonValue(const std::string& v) : m_data(v) {}
    JsonValue(const char* v) : m_data(std::string(v)) {}
    JsonValue(const Array& v) : m_data(v) {}
    JsonValue(const Object& v) : m_data(v) {}

    bool isNull() const { return std::holds_alternative<std::nullptr_t>(m_data); }
    bool isBool() const { return std::holds_alternative<bool>(m_data); }
    bool isInt() const { return std::holds_alternative<long long>(m_data); }
    bool isDouble() const { return std::holds_alternative<double>(m_data); }
    bool isString() const { return std::holds_alternative<std::string>(m_data); }
    bool isArray() const { return std::holds_alternative<Array>(m_data); }
    bool isObject() const { return std::holds_alternative<Object>(m_data); }

    bool asBool() const { return std::get<bool>(m_data); }
    long long asInt() const { return std::get<long long>(m_data); }
    double asDouble() const { return std::get<double>(m_data); }
    const std::string& asString() const { return std::get<std::string>(m_data); }
    const Array& asArray() const { return std::get<Array>(m_data); }
    const Object& asObject() const { return std::get<Object>(m_data); }

private:
    std::variant<std::nullptr_t, bool, long long, double, std::string, Array, Object> m_data;
};

/**
 * @brief Matches decimal-agnostic hex integer strings, e.g. "0x1f" or "-0X2A".
 */
inline const std::regex& hexIntPattern() {
    static const std::regex pattern(R"(^[+-]?0[xX][0-9a-fA-F]+$)");
    return pattern;
}

/**
 * @brief Recursively translate all integer values in a JSON-like structure to hex strings.
 *        Used when dumping an object to a JSON string.
 * @param d Value to translate (object, array, or scalar).
 * @return A new JsonValue with every integer (but not bool) replaced by its hex string form.
 */
inline JsonValue IntVal2Hex(const JsonValue& d) {
    if (d.isObject()) {
        JsonValue::Object result;
        for (const auto& [k, v] : d.asObject()) {
            result[k] = IntVal2Hex(v);
        }
        return JsonValue(result);
    } else if (d.isInt()) {
        std::ostringstream oss;
        long long v = d.asInt();
        if (v < 0) {
            oss << "-0x" << std::hex << -v;
        } else {
            oss << "0x" << std::hex << v;
        }
        return JsonValue(oss.str());
    } else if (d.isArray()) {
        JsonValue::Array result;
        result.reserve(d.asArray().size());
        for (const auto& dv : d.asArray()) {
            result.push_back(IntVal2Hex(dv));
        }
        return JsonValue(result);
    } else {
        return d;
    }
}

/**
 * @brief Recursively translate all hex string values in a JSON-like structure to int values.
 *        Used when loading an object from a JSON string.
 * @param d Value to translate (object, array, or scalar).
 * @return A new JsonValue with every hex string replaced by its integer value.
 */
inline JsonValue HexVal2Int(const JsonValue& d) {
    if (d.isObject()) {
        JsonValue::Object result;
        for (const auto& [k, v] : d.asObject()) {
            result[k] = HexVal2Int(v);
        }
        return JsonValue(result);
    } else if (d.isString() && std::regex_match(d.asString(), hexIntPattern())) {
        return JsonValue(static_cast<long long>(std::stoll(d.asString(), nullptr, 16)));
    } else if (d.isArray()) {
        JsonValue::Array result;
        result.reserve(d.asArray().size());
        for (const auto& dv : d.asArray()) {
            result.push_back(HexVal2Int(dv));
        }
        return JsonValue(result);
    } else {
        return d;
    }
}

} // namespace CuAsm

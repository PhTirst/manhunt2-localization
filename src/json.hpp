#pragma once

#include <map>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace mh2
{
    struct JsonValue
    {
        enum class Type
        {
            Null,
            Bool,
            Number,
            String,
            Array,
            Object
        };

        Type type = Type::Null;
        bool boolValue = false;
        double numberValue = 0.0;
        std::string stringValue;
        std::vector<JsonValue> arrayValue;
        std::map<std::string, JsonValue> objectValue;
        size_t sourceOffset = 0;
        size_t sourceLine = 0;
        size_t sourceColumn = 0;

        static JsonValue Null();
        static JsonValue Bool(bool value);
        static JsonValue Number(double value);
        static JsonValue String(std::string value);
        static JsonValue Array(std::vector<JsonValue> value);
        static JsonValue Object(std::map<std::string, JsonValue> value);

        bool IsNull() const;
        bool IsBool() const;
        bool IsNumber() const;
        bool IsString() const;
        bool IsArray() const;
        bool IsObject() const;

        const JsonValue *Find(const std::string &key) const;
    };

    class JsonError : public std::runtime_error
    {
    public:
        JsonError(
            const std::string &message,
            std::string sourceName,
            size_t byteOffset,
            size_t line,
            size_t column);

        const std::string &SourceName() const { return sourceName_; }
        size_t ByteOffset() const { return byteOffset_; }
        size_t Line() const { return line_; }
        size_t Column() const { return column_; }

    private:
        std::string sourceName_;
        size_t byteOffset_ = 0;
        size_t line_ = 0;
        size_t column_ = 0;
    };

    JsonValue ParseJson(const std::string &text, const std::string &sourceName = {});
    std::string WriteJson(const JsonValue &value, int indent = 0);
    std::string JsonEscape(const std::string &text);
}

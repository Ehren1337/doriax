// (c) Eduardo Doria Lima
// SPDX-License-Identifier: MIT

#pragma once

#include "ScriptProperty.h"
#include <filesystem>
#include <vector>
#include <string>

namespace doriax::editor {

    class ScriptParser {
    private:
        static doriax::ScriptPropertyType inferTypeFromCppType(const std::string& cppType, std::string& ptrTypeName);
        static doriax::ScriptPropertyType parseExplicitType(const std::string& typeStr, const std::string& cppType);
        static std::string removeComments(const std::string& content);

    public:
        static std::vector<doriax::ScriptProperty> parseScriptProperties(const std::filesystem::path& scriptPath);
        static std::vector<doriax::ScriptProperty> parseScriptPropertiesFromString(const std::string& content, const std::string& sourceName = "memory");
    };

}
#include "AiEngineApiContext.h"

#include "engine_api_suggestions.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace doriax::editor::ai {

namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::vector<std::string> tokenize(const std::string& value) {
    std::vector<std::string> tokens;
    std::string current;
    for (char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return !needle.empty() && haystack.find(needle) != std::string::npos;
}

struct ScoredSymbol {
    int score = 0;
    Json data;
};

std::unordered_map<std::string, std::string> buildInheritanceMap() {
    std::unordered_map<std::string, std::string> inheritance;
    for (const auto& symbol : doriax::editor::getEngineAPISymbols()) {
        const std::string kind = symbol.kind ? symbol.kind : "";
        if (kind != "Class") continue;

        const std::string name = lower(symbol.name ? symbol.name : "");
        const std::string detail = symbol.detail ? symbol.detail : "";
        const size_t basePos = detail.find(" : ");
        if (name.empty() || basePos == std::string::npos) continue;

        inheritance[name] = lower(detail.substr(basePos + 3));
    }
    return inheritance;
}

bool isSameOrBaseClass(const std::string& candidateBase,
                       const std::string& className,
                       const std::unordered_map<std::string, std::string>& inheritance) {
    if (candidateBase.empty() || className.empty()) return false;
    std::string current = className;
    for (int depth = 0; depth < 32; ++depth) {
        if (current == candidateBase) return true;
        auto it = inheritance.find(current);
        if (it == inheritance.end()) break;
        current = it->second;
    }
    return false;
}

std::string luaCallHint(const std::string& detail, const std::string& parent) {
    const std::string detailLower = lower(detail);
    const std::string parentLower = lower(parent);
    if (detailLower.find(":setcolor()") != std::string::npos &&
        (parentLower == "mesh" || parentLower == "shape" || parentLower == "image" ||
         parentLower == "text" || parentLower == "polygon" || parentLower == "skybox")) {
        return "Lua: local shape = Shape(self.scene, self.entity); "
               "shape:setColor(1.0, 0.0, 0.0, 1.0) or shape:setColor(Vector4(1.0, 0.0, 0.0, 1.0)). "
               "C++ cpp_subclass on Mesh: setColor(1.0f, 0.0f, 0.0f, 1.0f) in constructor or onUpdate; include \"Mesh.h\" (not <core/Mesh.h>) and unregister engine events in the destructor.";
    }
    if (parentLower == "shape" && detailLower.find(":setcolor()") == std::string::npos &&
        detailLower.find("shape") != std::string::npos && detailLower.find("constructor") != std::string::npos) {
        return "Construct with Shape(self.scene, self.entity) for an existing entity.";
    }
    return {};
}

// Script-authoring macros live in engine headers (ScriptProperty.h, FunctionSubscribe.h),
// not in the LuaBridge bindings, so the generated suggestions never include them. Curate
// them here so search_engine_api can surface the exact macro and signature for C++ scripts.
const std::vector<doriax::editor::EngineAPISymbol>& getScriptMacroSymbols() {
    static const std::vector<doriax::editor::EngineAPISymbol> macros = {
        {"DPROPERTY", "Macro",
         "DPROPERTY(\"Display Name\") on the line above a public member exposes it as an editor property; "
         "DPROPERTY(\"Display Name\", Type) also forces the editor type. Types: Bool, Int, Float, String, "
         "Vector2, Vector3, Vector4, Color3, Color4, EntityReference. From \"ScriptProperty.h\".",
         ""},
        {"REGISTER_ENGINE_EVENT", "Macro",
         "REGISTER_ENGINE_EVENT(onUpdate); in the constructor subscribes to a global Engine event; the class "
         "must declare a method named exactly like the event, e.g. void onUpdate(). Engine events: onUpdate, "
         "onFixedUpdate, onDraw, onMouseDown, onMouseUp, onMouseMove, onKeyDown, onKeyUp, onTouchStart, onTouchMove, "
         "onTouchEnd, onGamepadConnect, onGamepadDisconnect, onGamepadButtonDown, onGamepadButtonUp, onGamepadAxisMove. "
         "onUpdate is variable-frame (UI, camera, animation); onFixedUpdate runs on the physics step, so continuous "
         "forces and torques belong there, undivided by delta time. Available via \"Engine.h\".",
         ""},
        {"UNREGISTER_ENGINE_EVENT", "Macro",
         "UNREGISTER_ENGINE_EVENT(onUpdate); in the destructor; pair one with every REGISTER_ENGINE_EVENT.",
         ""},
        {"REGISTER_COMPONENT_EVENT", "Macro",
         "REGISTER_COMPONENT_EVENT(ComponentType, eventName, methodName); in the constructor subscribes "
         "this->methodName to a component's event, e.g. REGISTER_COMPONENT_EVENT(ButtonComponent, onPress, handlePress). "
         "The method signature must match the event. Search the component (e.g. ButtonComponent, UIComponent) for its "
         "event member names. Destructor counterpart: UNREGISTER_COMPONENT_EVENT(ComponentType, eventName, methodName).",
         ""},
        {"UNREGISTER_COMPONENT_EVENT", "Macro",
         "UNREGISTER_COMPONENT_EVENT(ComponentType, eventName, methodName); in the destructor; mirrors "
         "REGISTER_COMPONENT_EVENT.",
         ""},
        {"REGISTER_UI_EVENT", "Macro",
         "REGISTER_UI_EVENT(eventName, methodName); shortcut for REGISTER_COMPONENT_EVENT(UIComponent, ...). "
         "UIComponent events: onClick, onDoubleClick, onPointerDown, onPointerUp, onPointerMove, onPointerEnter, "
         "onPointerLeave, onGetFocus, onLostFocus, onDragStart, onDrag, onDragEnd. Pointer/click handlers take "
         "(float x, float y); focus handlers take no args. Destructor counterpart: UNREGISTER_UI_EVENT(eventName, methodName).",
         ""},
        {"REGISTER_BUTTON_EVENT", "Macro",
         "REGISTER_BUTTON_EVENT(eventName, methodName); shortcut for REGISTER_COMPONENT_EVENT(ButtonComponent, ...). "
         "ButtonComponent events: onPress, onRelease (handlers take no args). For a click use REGISTER_UI_EVENT(onClick, ...). "
         "Destructor counterpart: UNREGISTER_BUTTON_EVENT(eventName, methodName).",
         ""},
        {"REGISTER_SCROLLBAR_EVENT", "Macro",
         "REGISTER_SCROLLBAR_EVENT(eventName, methodName); shortcut for REGISTER_COMPONENT_EVENT(ScrollbarComponent, ...). "
         "ScrollbarComponent event: onChange (handler takes float value). Destructor counterpart: "
         "UNREGISTER_SCROLLBAR_EVENT(eventName, methodName).",
         ""},
        {"REGISTER_PANEL_EVENT", "Macro",
         "REGISTER_PANEL_EVENT(eventName, methodName); shortcut for REGISTER_COMPONENT_EVENT(PanelComponent, ...). "
         "PanelComponent events: onMove (no args), onResize (int width, int height). Destructor counterpart: "
         "UNREGISTER_PANEL_EVENT(eventName, methodName).",
         ""},
        {"REGISTER_EVENT", "Macro",
         "REGISTER_EVENT(eventObject, methodName); generic subscribe of this->methodName to any FunctionSubscribe "
         "event object; pair with UNREGISTER_EVENT(eventObject, methodName) in the destructor.",
         ""},
    };
    return macros;
}

// Lua script helpers that are registered as globals (lua_setglobal) or are table
// conventions, not LuaBridge class members, so the generated suggestions never include
// them. Curate them here so search_engine_api surfaces them for Lua scripts. These are
// the Lua analog of the C++ script macros above.
const std::vector<doriax::editor::EngineAPISymbol>& getLuaGlobalSymbols() {
    static const std::vector<doriax::editor::EngineAPISymbol> globals = {
        {"RegisterEngineEvent", "LuaGlobal",
         "Lua global script helper. In :init() call RegisterEngineEvent(self, \"onUpdate\") to subscribe to a "
         "global Engine event, and define a matching function Name:onUpdate(...) end. Engine events: onUpdate, "
         "onFixedUpdate, onDraw, onMouseDown, onMouseUp, onMouseMove, onKeyDown, onKeyUp, onTouchStart, onTouchMove, "
         "onTouchEnd, onGamepadConnect, onGamepadDisconnect, onGamepadButtonDown, onGamepadButtonUp, onGamepadAxisMove. "
         "onUpdate is variable-frame (UI, camera, animation); onFixedUpdate runs on the physics step, so continuous "
         "forces and torques belong there, unscaled by Engine.deltatime. "
         "No manual unregister is needed; the engine cleans up when the script is destroyed.",
         ""},
        {"RegisterEvent", "LuaGlobal",
         "Lua global script helper. RegisterEvent(self, eventObject, \"methodName\") subscribes self:methodName to any "
         "engine event object (a FunctionSubscribe with :add), and define function Name:methodName(...) end. For "
         "component/UI events get the event object from a runtime wrapper, e.g. local b = Button(self.scene, self.entity); "
         "RegisterEvent(self, b:getButtonComponent().onPress, \"onPress\"). The handler arg count must match the event "
         "(ButtonComponent onPress/onRelease take none; UIComponent onClick/onPointer* take x, y; ScrollbarComponent "
         "onChange takes a value).",
         ""},
        {"properties", "LuaScript",
         "Lua editor property declaration. In the returned module table set "
         "properties = { { name = \"speed\", displayName = \"Speed\", type = \"float\", default = 5.0 } }. Accepted "
         "type strings: bool, int, float, string, vector2, vector3, vector4, color3, color4, entity. Each property is "
         "read and written at runtime as self.<name>, e.g. self.speed.",
         ""},
    };
    return globals;
}

} // namespace

Json AiEngineApiContext::search(const std::string& query,
                                const std::string& parentFilter,
                                int maxResults) {
    maxResults = std::max(1, std::min(50, maxResults));
    const std::string queryLower = lower(query);
    const std::string parentLower = lower(parentFilter);
    const std::vector<std::string> tokens = tokenize(query);
    const auto inheritance = buildInheritanceMap();

    std::vector<ScoredSymbol> scored;
    auto consider = [&](const char* symName, const char* symKind,
                        const char* symDetail, const char* symParent) {
        const std::string name = symName ? symName : "";
        const std::string kind = symKind ? symKind : "";
        const std::string detail = symDetail ? symDetail : "";
        const std::string parent = symParent ? symParent : "";

        const std::string nameLower = lower(name);
        const std::string kindLower = lower(kind);
        const std::string detailLower = lower(detail);
        const std::string parentSymbolLower = lower(parent);

        if (!parentLower.empty() &&
            nameLower != parentLower &&
            !isSameOrBaseClass(parentSymbolLower, parentLower, inheritance)) {
            return;
        }

        int score = 0;
        if (nameLower == queryLower) score += 120;
        if (parentSymbolLower == queryLower) score += 90;
        if (detailLower == queryLower) score += 70;
        if (contains(nameLower, queryLower)) score += 55;
        if (contains(parentSymbolLower, queryLower)) score += 45;
        if (contains(detailLower, queryLower)) score += 30;
        if (contains(kindLower, queryLower)) score += 10;

        int matchedTokens = 0;
        for (const std::string& token : tokens) {
            if (contains(nameLower, token)) {
                score += 18;
                matchedTokens++;
            } else if (contains(parentSymbolLower, token)) {
                score += 14;
                matchedTokens++;
            } else if (isSameOrBaseClass(parentSymbolLower, token, inheritance)) {
                score += 12;
                matchedTokens++;
            } else if (contains(detailLower, token)) {
                score += 10;
                matchedTokens++;
            } else if (contains(kindLower, token)) {
                score += 4;
                matchedTokens++;
            }
        }
        if (!tokens.empty() && matchedTokens == static_cast<int>(tokens.size())) {
            score += 35;
        }
        if (!parentLower.empty()) {
            score += 20;
        }

        if (score <= 0) {
            return;
        }

        scored.push_back({
            score,
            Json{
                {"name", name},
                {"kind", kind},
                {"parent", parent},
                {"detail", detail}
            }
        });
    };

    for (const auto& symbol : doriax::editor::getEngineAPISymbols()) {
        consider(symbol.name, symbol.kind, symbol.detail, symbol.parent);
    }
    for (const auto& macro : getScriptMacroSymbols()) {
        consider(macro.name, macro.kind, macro.detail, macro.parent);
    }
    for (const auto& global : getLuaGlobalSymbols()) {
        consider(global.name, global.kind, global.detail, global.parent);
    }

    std::sort(scored.begin(), scored.end(), [](const ScoredSymbol& a, const ScoredSymbol& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.data.value("detail", "") < b.data.value("detail", "");
    });

    Json results = Json::array();
    for (const ScoredSymbol& item : scored) {
        if (static_cast<int>(results.size()) >= maxResults) break;
        Json entry = item.data;
        entry["score"] = item.score;
        const std::string hint = luaCallHint(entry.value("detail", ""), entry.value("parent", ""));
        if (!hint.empty()) {
            entry["lua_call_hint"] = hint;
        }
        results.push_back(std::move(entry));
    }

    return Json{
        {"source", "engine_api_suggestions.h generated from engine/core Lua bindings and headers"},
        {"query", query},
        {"parent", parentFilter},
        {"results", results},
        {"detail_format", "detail reads Parent<sep>name(params) -> ReturnType. The ':' separator marks an instance method, '.' a static method, and the part after '->' is the C++ return type (absent when it returns void). Param entries are 'Type name'. For kind 'Macro' (C++ script macros like DPROPERTY and REGISTER_*/UNREGISTER_* event subscriptions), 'LuaGlobal' (global Lua helpers like RegisterEngineEvent/RegisterEvent), and 'LuaScript' (Lua script-table conventions), the detail is the usage description; follow it verbatim."},
        {"language_mapping", "Lua: instance methods obj:method(args), static/namespaced Class.method(args), and bound properties Class.prop (e.g. Engine.deltatime). C++: instance methods obj.method(args) or a bare method(args)/this->method(args) inside a cpp_subclass, static/namespaced Class::method(args) (e.g. Engine::getDeltatime()), and use getters/setters not the Lua property shortcut."},
        {"usage_note", "Construct wrappers with the signatures returned here, e.g. Shape(self.scene, self.entity). For color on Mesh/Shape, Lua uses setColor with RGBA floats or Vector4; C++ cpp_subclass scripts inherit Mesh and call setColor(1,0,0,1) with flat headers like \"Mesh.h\". In C++ register engine callbacks with REGISTER_ENGINE_EVENT in the constructor and unregister with UNREGISTER_ENGINE_EVENT in the destructor; in Lua register with RegisterEngineEvent(self, \"onUpdate\") in :init() with no manual unregister. Read lua_call_hint when present."}
    };
}

} // namespace doriax::editor::ai

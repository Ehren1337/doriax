#include "ScriptCreateDialog.h"
#include "external/IconsFontAwesome6.h"
#include "Scene.h"
#include "Factory.h"
#include "util/ProjectUtils.h"
#include "util/UIUtils.h"
#include "util/Util.h"
#include <fstream>

namespace doriax {
namespace editor {

// ImGui input filter: while typing, restrict the class/base name to characters
// that are legal in a C++ identifier - any other character (space, punctuation,
// ...) is converted to '_' immediately. Structural normalization that cannot be
// done per-character without fighting the typist - stripping leading/trailing
// underscores, prefixing a leading digit, escaping C++ keywords - is deferred to
// Factory::toIdentifier(), applied when editing finishes, so the committed name
// always equals the identifier the code generator emits.
int ScriptCreateDialog::classNameCharFilter(ImGuiInputTextCallbackData* data) {
    ImWchar c = data->EventChar;
    bool isValid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_';
    if (!isValid) {
        data->EventChar = '_';
    }
    return 0;
}

void ScriptCreateDialog::open(Scene* scene,
                              Entity entity,
                              const fs::path& projectPath,
                              const fs::path& luaPath,
                              const std::string& defaultBaseName,
                              std::function<void(const fs::path&, const fs::path&, const std::string&, ScriptType)> onCreate,
                              std::function<void()> onCancel) {
    m_isOpen = true;
    m_scene = scene;
    m_entity = entity;
    m_projectPath = projectPath;
    m_luaPath = luaPath;
    m_selectedPath = projectPath.string();
    m_scriptType = ScriptType::SUBCLASS;
    m_onCreate = onCreate;
    m_onCancel = onCancel;
    std::string base = defaultBaseName.empty() ? "NewScript" : sanitizeClassName(defaultBaseName);
    strncpy(m_baseNameBuffer, base.c_str(), sizeof(m_baseNameBuffer) - 1);
    m_baseNameBuffer[sizeof(m_baseNameBuffer) - 1] = '\0';
}

std::string ScriptCreateDialog::sanitizeClassName(const std::string& in) const {
    if (in.empty()) return "NewScript";
    // Reuse the same identifier validation the code generator/loader rely on
    // (Factory::toIdentifier): collapses invalid characters to underscores,
    // strips leading/trailing underscores, ensures a valid identifier start and
    // avoids C++ keywords - so the class name always compiles.
    return Factory::toIdentifier(in);
}

fs::path ScriptCreateDialog::makeHeaderPath(const std::string& className) const {
    return fs::path(m_selectedPath) / (className + ".h");
}

fs::path ScriptCreateDialog::makeSourcePath(const std::string& className) const {
    return fs::path(m_selectedPath) / (className + ".cpp");
}

fs::path ScriptCreateDialog::makeLuaPath(const std::string& moduleName) const {
    return fs::path(m_selectedPath) / (moduleName + ".lua");
}

void ScriptCreateDialog::writeFiles(const fs::path& headerPath,
                                      const fs::path& sourcePath,
                                      const std::string& classOrModuleName,
                                      ScriptType type) {
    bool isCppSubclass = (type == ScriptType::SUBCLASS);
    bool isCppScriptBase = (type == ScriptType::SCRIPT_CLASS);
    bool isLua = (type == ScriptType::SCRIPT_LUA);

    if (isCppSubclass || isCppScriptBase) {
        fs::create_directories(headerPath.parent_path());

        if (isCppSubclass) {
            ProjectUtils::EntityClassInfo parentInfo = ProjectUtils::getEntityClassInfo(m_scene, m_entity);
            std::string parentClass = parentInfo.name;
            bool hasTransform = parentInfo.derivesObject;
            bool isMesh = parentInfo.derivesMesh;

            // Header
            {
                std::ofstream h(headerPath, std::ios::trunc);
                if (h) {
                    h << "#pragma once\n\n";
                    h << "#include \"Shape.h\"\n";
                    h << "#include \"" << parentClass << ".h\"\n";
                    h << "#include \"ScriptProperty.h\"\n\n";
                    h << "class " << classOrModuleName << " : public doriax::" << parentClass << " {\n";
                    h << "public:\n";
                    h << "    // Example properties\n";
                    h << "    DPROPERTY(\"Is Active\")\n";
                    h << "    bool isActive = true;\n\n";
                    if (hasTransform){
                        h << "    DPROPERTY(\"Speed\")\n";
                        h << "    float speed = 5.0f;\n\n";
                        h << "    DPROPERTY(\"Target Position\")\n";
                        h << "    doriax::Vector3 targetPosition = doriax::Vector3(0, 0, 0);\n\n";
                        if (isMesh){
                            h << "    DPROPERTY(\"Mesh Color\", Color4)\n";
                            h << "    doriax::Vector4 meshColor = doriax::Vector4(1, 1, 1, 1);\n\n";
                        }
                    }
                    h << "    " << classOrModuleName << "(doriax::Scene* scene, doriax::Entity entity);\n";
                    h << "    virtual ~" << classOrModuleName << "();\n\n";
                    h << "    void onUpdate();\n";
                    h << "};\n";
                }
            }

            // Source
            {
                std::ofstream c(sourcePath, std::ios::trunc);
                if (c) {
                    c << "#include \"" << headerPath.filename().string() << "\"\n\n";
                    c << "using namespace doriax;\n\n";
                    c << classOrModuleName << "::" << classOrModuleName << "(Scene* scene, Entity entity): " << parentClass << "(scene, entity) {\n";
                    c << "    REGISTER_ENGINE_EVENT(onUpdate);\n\n";
                    c << "}\n\n";
                    c << classOrModuleName << "::~" << classOrModuleName << "() {\n";
                    c << "\n";
                    c << "}\n\n";
                    c << "void " << classOrModuleName << "::onUpdate() {\n";
                    c << "    if (!isActive) return;\n\n";
                    if (hasTransform){
                        c << "    // Example: move toward the target at 'speed' units per second.\n";
                        c << "    // moveTowards() clamps the step to the remaining distance, so it\n";
                        c << "    // never overshoots - even on the first frame where deltaTime is large.\n";
                        c << "    float deltaTime = Engine::getDeltatime();\n";
                        c << "    Vector3 currentPos = getPosition();\n";
                        c << "    setPosition(currentPos.moveTowards(targetPosition, speed * deltaTime));\n";
                        if (isMesh){
                            c << "    setColor(meshColor);\n";
                        }
                    }
                    c << "}\n\n";
                }
            }
        } else if (isCppScriptBase) {
            // Script Class
            // Header
            {
                std::ofstream h(headerPath, std::ios::trunc);
                if (h) {
                    h << "#pragma once\n\n";
                    h << "#include \"ScriptBase.h\"\n";
                    h << "#include \"Engine.h\"\n";
                    h << "#include \"ScriptProperty.h\"\n\n";
                    h << "class " << classOrModuleName << " : public doriax::ScriptBase {\n";
                    h << "public:\n";
                    h << "    // Example properties - you can add more!\n";
                    h << "    DPROPERTY(\"Speed\")\n";
                    h << "    float speed = 5.0f;\n\n";
                    h << "    DPROPERTY(\"Is Active\")\n";
                    h << "    bool isActive = true;\n\n";
                    h << "    DPROPERTY(\"Counter\")\n";
                    h << "    int counter = 0;\n\n";
                    h << "    " << classOrModuleName << "(doriax::Scene* scene, doriax::Entity entity);\n";
                    h << "    ~" << classOrModuleName << "();\n\n";
                    h << "    void onUpdate();\n";
                    h << "};\n";
                }
            }

            // Source
            {
                std::ofstream c(sourcePath, std::ios::trunc);
                if (c) {
                    c << "#include \"" << headerPath.filename().string() << "\"\n\n";
                    c << "using namespace doriax;\n\n";
                    c << classOrModuleName << "::" << classOrModuleName << "(Scene* scene, Entity entity): ScriptBase(scene, entity) {\n";
                    c << "    REGISTER_ENGINE_EVENT(onUpdate);\n\n";
                    c << "}\n\n";
                    c << classOrModuleName << "::~" << classOrModuleName << "() {\n";
                    c << "\n";
                    c << "}\n\n";
                    c << "void " << classOrModuleName << "::onUpdate() {\n";
                    c << "    if (!isActive) return;\n\n";
                    c << "    // Example: Increment counter every frame\n";
                    c << "    counter++;\n";
                    c << "    if (counter % 60 == 0) {\n";
                    c << "        Log::print(\"Counter: %d\\n\", counter);\n";
                    c << "    }\n";
                    c << "}\n\n";
                }
            }
        }
    }

    // Lua generation
    if (isLua) {
        fs::create_directories(sourcePath.parent_path());
        std::ofstream f(sourcePath, std::ios::trunc);
        if (f) {
            f << "-- " << classOrModuleName << ".lua\n";
            f << "-- Auto-generated by Doriax Editor\n\n";

            // Script table used as the instance prototype
            f << "local " << classOrModuleName << " = {\n";
            f << "    -- Editor-exposed properties\n";
            f << "    properties = {\n";
            f << "        {\n";
            f << "            name = \"speed\",\n";
            f << "            displayName = \"Speed\",\n";
            f << "            type = \"float\",\n";
            f << "            default = 5.0\n";
            f << "        },\n";
            f << "        {\n";
            f << "            name = \"isActive\",\n";
            f << "            displayName = \"Is Active\",\n";
            f << "            type = \"bool\",\n";
            f << "            default = true\n";
            f << "        }\n";
            f << "    }\n";
            f << "}\n\n";

            // init(self) – called once by the engine (Project::initializeLuaScripts)
            f << "function " << classOrModuleName << ":init()\n";
            f << "    -- 'self.scene' and 'self.entity' are provided by the engine\n";
            f << "    RegisterEngineEvent(self, \"onUpdate\")\n";
            f << "    self.counter = 0\n";
            f << "end\n\n";

            // onUpdate(self) – optional; engine may call this every frame
            f << "function " << classOrModuleName << ":onUpdate()\n";
            f << "    if not self.isActive then return end\n\n";
            f << "    self.counter = self.counter + 1\n";
            f << "    if self.counter % 60 == 0 then\n";
            f << "        Log.print(\"[\" .. tostring(self.entity) .. \"] Counter: \" .. self.counter)\n";
            f << "    end\n";
            f << "end\n\n";

            f << "return " << classOrModuleName << "\n";
        }
    }
}

void ScriptCreateDialog::finalizeCreation(const fs::path& headerPath,
                                          const fs::path& sourcePath,
                                          const std::string& name) {
    fs::path useHeader, useSource;
    bool isCpp = (m_scriptType == ScriptType::SUBCLASS || m_scriptType == ScriptType::SCRIPT_CLASS);
    bool isLua = (m_scriptType == ScriptType::SCRIPT_LUA);

    if (isCpp) {
        useHeader = headerPath;
        useSource = sourcePath;
    }
    if (isLua) {
        useSource = sourcePath;
    }

    writeFiles(useHeader, useSource, name, m_scriptType);

    if (m_onCreate) {
        // Lua entries resolve through "lua://", C++ sources stay project-relative
        const fs::path sourceRoot = (m_scriptType == ScriptType::SCRIPT_LUA) ? m_luaPath : m_projectPath;

        fs::path relHeader, relSource;
        if (!useHeader.empty()) relHeader = fs::relative(useHeader, m_projectPath);
        if (!useSource.empty()) relSource = fs::relative(useSource, sourceRoot);

        m_onCreate(relHeader, relSource, name, m_scriptType);
    }

    m_isOpen = false;
}

void ScriptCreateDialog::show() {
    if (!m_isOpen) return;

    ImGui::OpenPopup("Create Script##CreateScriptModal");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_Modal;

    bool popupOpen = ImGui::BeginPopupModal("Create Script##CreateScriptModal", nullptr, flags);
    if (!popupOpen) {
        if (m_isOpen) {
            m_isOpen = false;
            if (m_onCancel) m_onCancel();
        }
        return;
    }

    // Script Type Selection
    ImGui::Text("Script Type:");

    // C++ Subclass
    if (ImGui::RadioButton("C++ Subclass", (int*)&m_scriptType, (int)ScriptType::SUBCLASS)) {}

    if (ImGui::RadioButton("C++ Script Class", (int*)&m_scriptType, (int)ScriptType::SCRIPT_CLASS)) {}

    if (ImGui::RadioButton("Lua Script", (int*)&m_scriptType, (int)ScriptType::SCRIPT_LUA)) {}

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Browsing is limited to the root the new script is stored relative to: a folder
    // above it would be saved as an unresolvable "../" path.
    const fs::path rootPath = (m_scriptType == ScriptType::SCRIPT_LUA) ? m_luaPath : m_projectPath;
    if (!Util::isInsidePath(fs::path(m_selectedPath), rootPath)) {
        m_selectedPath = rootPath.string();
    }

    if (ImGui::BeginChild("DirBrowser", ImVec2(300, 200), true)) {
        if (ImGui::BeginTable("DirTree", 1, ImGuiTableFlags_Resizable)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);

            bool rootOpen = true;
            ImGui::SetNextItemOpen(rootOpen, ImGuiCond_Always);
            bool isRootSelected = (m_selectedPath == rootPath.string());

            if (ImGui::TreeNodeEx("##root",
                ImGuiTreeNodeFlags_OpenOnArrow |
                ImGuiTreeNodeFlags_SpanFullWidth |
                (isRootSelected ? ImGuiTreeNodeFlags_Selected : 0))) {

                ImGui::SameLine(0, 0);
                ImGui::TextColored(ImVec4(1.f, 0.8f, 0.f, 1.f), "%s", ICON_FA_FOLDER_OPEN);
                ImGui::SameLine();
                ImGui::Text("%s", rootPath == m_projectPath ? "Project Root" : rootPath.filename().string().c_str());
                if (ImGui::IsItemClicked() ||
                    (ImGui::IsMouseClicked(0) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))) {
                    m_selectedPath = rootPath.string();
                }

                UIUtils::directoryTreeBrowser(rootPath, m_selectedPath);
                ImGui::TreePop();
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    ImGui::Text("Class / Base Name:");
    ImGui::SetNextItemWidth(-1);

    bool enterPressed = ImGui::InputText(
        "##basename",
        m_baseNameBuffer,
        sizeof(m_baseNameBuffer),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter,
        classNameCharFilter
    );

    // When editing finishes (Enter or focus loss), snap the field to the exact
    // identifier that will be generated - applying the leading/trailing '_' strip,
    // leading-digit prefix and keyword escaping that the per-char filter can't do
    // mid-typing - so the visible name stays identical to the created class name.
    // Skipped while empty so an intentionally cleared field isn't auto-refilled.
    if (ImGui::IsItemDeactivatedAfterEdit() && m_baseNameBuffer[0] != '\0') {
        std::string canonical = sanitizeClassName(m_baseNameBuffer);
        strncpy(m_baseNameBuffer, canonical.c_str(), sizeof(m_baseNameBuffer) - 1);
        m_baseNameBuffer[sizeof(m_baseNameBuffer) - 1] = '\0';
    }

    std::string baseName = m_baseNameBuffer;
    bool hasBase = !baseName.empty();

    std::string name = sanitizeClassName(baseName);

    // Compute possible paths
    fs::path headerPath;
    fs::path sourcePath;

    bool isCpp = (m_scriptType == ScriptType::SUBCLASS ||
                  m_scriptType == ScriptType::SCRIPT_CLASS);
    bool isLua = (m_scriptType == ScriptType::SCRIPT_LUA);

    if (isCpp) {
        headerPath = makeHeaderPath(name);
        sourcePath = makeSourcePath(name);
    }

    if (isLua) {
        sourcePath = makeLuaPath(name);
    }

    // If Enter was pressed inside the input, try to create (same logic as Create button)
    if (enterPressed && hasBase) {
        bool headerExists = isCpp && fs::exists(headerPath);
        bool sourceExists = isCpp && fs::exists(sourcePath);
        bool luaExists    = isLua && fs::exists(sourcePath);

        if (headerExists || sourceExists || luaExists) {
            ImGui::OpenPopup("Confirm Overwrite");
        } else {
            finalizeCreation(headerPath, sourcePath, name);
            ImGui::CloseCurrentPopup(); // Close main modal
        }
    }

    // Display
    if (isCpp && isLua) {
        // (You probably won’t have a type that is both; kept for completeness)
    } else if (isCpp) {
        ImGui::TextWrapped("Will create:\n  %s\n  %s",
            fs::relative(headerPath, m_projectPath).string().c_str(),
            fs::relative(sourcePath, m_projectPath).string().c_str());

        bool headerExists = fs::exists(headerPath);
        bool sourceExists = fs::exists(sourcePath);
        if (headerExists || sourceExists) {
            ImGui::TextColored(ImVec4(1.f, 0.5f, 0.f, 1.f),
                "Warning: Existing file(s) will be overwritten.");
        }
    } else if (isLua) {
        ImGui::TextWrapped("Will create:\n  %s",
            fs::relative(sourcePath, m_projectPath).string().c_str());

        bool luaExists = fs::exists(sourcePath);
        if (luaExists) {
            ImGui::TextColored(ImVec4(1.f, 0.5f, 0.f, 1.f),
                "Warning: Existing file will be overwritten.");
        }
    }

    ImGui::Separator();

    float windowWidth = ImGui::GetWindowSize().x;
    float buttonsWidth = 250;
    ImGui::SetCursorPosX((windowWidth - buttonsWidth) * 0.5f);

    ImGui::BeginDisabled(!hasBase);
    if (ImGui::Button("Create", ImVec2(120, 0))) {
        bool headerExists = isCpp && fs::exists(headerPath);
        bool sourceExists = isCpp && fs::exists(sourcePath);
        bool luaExists = isLua && fs::exists(sourcePath);

        if (headerExists || sourceExists || luaExists) {
            ImGui::OpenPopup("Confirm Overwrite");
        } else {
            finalizeCreation(headerPath, sourcePath, name);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        m_isOpen = false;
        if (m_onCancel) m_onCancel();
        ImGui::CloseCurrentPopup();
    }

    // Overwrite confirmation modal
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Confirm Overwrite", NULL, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("One or more files already exist.\nThis operation will overwrite them.\n\nAre you sure?");
        ImGui::Separator();

        if (ImGui::Button("Yes", ImVec2(120, 0))) {
             finalizeCreation(headerPath, sourcePath, name);
             ImGui::CloseCurrentPopup(); // Close Confirm Overwrite
        }
        ImGui::SameLine();
        if (ImGui::Button("No", ImVec2(120, 0))) { 
            ImGui::CloseCurrentPopup(); 
        }
        ImGui::EndPopup();
    }

    // If creation finished (m_isOpen set to false in finalizeCreation), close the main dialog
    if (!m_isOpen && ImGui::IsPopupOpen("Create Script##CreateScriptModal")) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

} // namespace editor
} // namespace doriax

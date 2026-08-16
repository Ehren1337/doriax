// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "Project.h"
#include "Catalog.h"
#include <string>
#include <map>
#include <functional>

namespace doriax::editor{

    class UnlinkMaterialCmd: public Command{

    private:
        Project* project;
        uint32_t sceneId;
        ComponentType componentType;
        std::string propertyName;
        unsigned int submeshIndex;
        bool wasModified;
        std::function<void()> onValueChanged;

        struct EntityData {
            Material oldMaterial;
            std::string linkedFilePath;
        };
        std::map<Entity, EntityData> entities;
        // Mesh children of one model share an override list, so it is snapshotted once per model.
        std::map<Entity, std::vector<SubmeshOverride>> oldOverrides;

    public:
        UnlinkMaterialCmd(Project* project, uint32_t sceneId, ComponentType componentType,
                          std::string propertyName, unsigned int submeshIndex,
                          const std::vector<Entity>& entityList,
                          std::function<void()> onValueChanged = nullptr);

        bool execute() override;
        void undo() override;

        bool mergeWith(Command* otherCommand) override;
    };

}

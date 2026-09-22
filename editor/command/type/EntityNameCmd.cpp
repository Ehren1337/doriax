// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EntityNameCmd.h"

using namespace doriax;

editor::EntityNameCmd::EntityNameCmd(Project* project, uint32_t sceneId, Entity entity, std::string name){
    this->project = project;
    this->sceneId = sceneId;
    this->entity = entity;
    this->newName = name;
}

bool editor::EntityNameCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);
    // A Play-created entity is gone after Stop, and naming it would recreate the id
    if (!sceneProject || !sceneProject->scene->isEntityCreated(entity)){
        return false;
    }

    oldName = sceneProject->scene->getEntityName(entity);
    wasModified = project->getScene(sceneId)->isModified;

    if (project->isEntityInBundle(sceneId, entity)){
        project->bundleNameChanged(sceneId, entity, newName, false);
    }
    sceneProject->scene->setEntityName(entity, newName);

    sceneProject->isModified = true;

    return true;
}

void editor::EntityNameCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene->isEntityCreated(entity)){
        return;
    }

    if (project->isEntityInBundle(sceneId, entity)){
        project->bundleNameChanged(sceneId, entity, oldName, false);
    }
    sceneProject->scene->setEntityName(entity, oldName);

    sceneProject->isModified = wasModified;
}

bool editor::EntityNameCmd::mergeWith(editor::Command* otherCommand){
    EntityNameCmd* otherCmd = dynamic_cast<EntityNameCmd*>(otherCommand);
    if (otherCmd != nullptr){
        if (sceneId == otherCmd->sceneId && entity == otherCmd->entity){
            this->oldName = otherCmd->oldName;

            this->wasModified = this->wasModified && otherCmd->wasModified;
            return true;
        }
    }
    return false;
}
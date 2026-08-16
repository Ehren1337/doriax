// (c) Eduardo Doria Lima
// SPDX-License-Identifier: MIT

#pragma once

#include "CommandHistory.h"
#include <map>

namespace doriax::editor{

    class CommandHandle{

    private:
        static std::map<size_t, CommandHistory*> historys;

    public:
        static CommandHistory* get(size_t sceneId);

        static void remove(size_t sceneId);
    };

}


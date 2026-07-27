#ifndef COMMANDHISTORY_H
#define COMMANDHISTORY_H

#include "Command.h"
#include <vector>
#include <cstddef>
#include <functional>

namespace doriax::editor{

    class CommandHistory{

    private:
        std::vector<Command*> list;
        size_t index = 0; // real index is (index-1)
        size_t sceneId = 0;

    public:
        // Called with the owning scene id after any command apply/undo/redo.
        static std::function<void(size_t)> onSceneModified;

        explicit CommandHistory(size_t sceneId = 0);
        virtual ~CommandHistory();

        void addCommand(Command* cmd);
        void addCommandNoMerge(Command* cmd);

        void undo();
        void redo();

        bool canUndo() const;
        bool canRedo() const;

        void clear();
    };

}

#endif /* COMMANDHISTORY_H */
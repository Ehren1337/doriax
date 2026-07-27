#ifndef COMMAND_H
#define COMMAND_H

namespace doriax::editor{

    class Command{
    protected:
        bool mergeable = true;

    public:
        virtual ~Command() {}

        virtual bool execute() = 0;
        virtual void undo() = 0;

        virtual bool mergeWith(Command* olderCommand) = 0;

        // True if this can change what the Structure tree shows. Defaults to true so a
        // new command is never silently stale.
        virtual bool affectsStructure() const { return true; }

        void setNoMerge() { mergeable = false; }
        bool canMerge() const { return mergeable; }
    };

}

#endif /* COMMAND_H */
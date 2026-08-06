//
// (c) 2026 Eduardo Doria.
//

#ifndef TEXT_COMPONENT_H
#define TEXT_COMPONENT_H

#include "Engine.h"
#include "math/Vector2.h"

namespace doriax{

    class STBText;

    struct DORIAX_API TextComponent{
        bool loaded = false;

        std::string font = "";
        std::string text = "";
        unsigned int fontSize = 20;
        bool multiline = true;
        unsigned int maxTextSize = 100;

        std::vector<Vector2> charPositions;

        bool fixedWidth = false;
        bool fixedHeight = false;

        bool pivotBaseline = false;
        bool pivotCentered = false;

        std::shared_ptr<STBText> stbtext = NULL;

        //atlas state this text was built with, see STBText
        unsigned long atlasVersion = 0;
        unsigned long atlasGeneration = 0;

        bool needReloadAtlas = false;
        bool needUpdateText = true;
    };

}

#endif //TEXT_COMPONENT_H
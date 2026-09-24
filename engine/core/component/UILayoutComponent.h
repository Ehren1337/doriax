// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LAYOUT_COMPONENT_H
#define UI_LAYOUT_COMPONENT_H

#include "util/FunctionSubscribe.h"
#include "math/Rect.h"
#include "math/Quaternion.h"
#include "math/Matrix4.h"
#include "math/Vector2.h"

namespace doriax{

    enum class AnchorPreset{
        NONE,
        TOP_LEFT,
        TOP_RIGHT,
        BOTTOM_LEFT,
        BOTTOM_RIGHT,
        CENTER_LEFT,
        CENTER_TOP,
        CENTER_RIGHT,
        CENTER_BOTTOM,
        CENTER,
        LEFT_WIDE,
        TOP_WIDE,
        RIGHT_WIDE,
        BOTTOM_WIDE,
        VERTICAL_CENTER_WIDE,
        HORIZONTAL_CENTER_WIDE,
        FULL_LAYOUT
    };

    struct DORIAX_API UILayoutComponent{
        // UI part
        unsigned int width = 0;
        unsigned int height = 0;

        float anchorPointLeft = 0;
        float anchorPointTop = 0;
        float anchorPointRight = 0;
        float anchorPointBottom = 0;

        int anchorOffsetLeft = 0;
        int anchorOffsetTop = 0;
        int anchorOffsetRight = 0;
        int anchorOffsetBottom = 0;

        Vector2 positionOffset = Vector2(0, 0); // for anchors

        Vector2 pivot = Vector2(0, 0); // scale and rotation center, (0, 0) top-left to (1, 1) bottom-right

        AnchorPreset anchorPreset = AnchorPreset::NONE;
        bool usingAnchors = false;

        Entity panel = NULL_ENTITY;

        int containerBoxIndex = -1;

        Rect scissor = Rect(0, 0, 0, 0);
        bool scissorActive = false; // scissor is valid, empty rect included
        bool ignoreScissor = false; // ignore parent scissor
        bool ignoreEvents = false;

        bool needUpdateSizes = false;
        bool needUpdateAnchorOffsets = false;
    };

    // how far a scaled or rotated element's local origin moves from its position
    inline Vector3 getUIPivotShift(const Vector2& pivot, const Vector2& size, const Quaternion& rotation, const Vector3& scale){
        Vector3 point(pivot.x * size.x, pivot.y * size.y, 0);
        return point - rotation * (scale * point);
    }

    inline Vector3 getUIPivotShift(const UILayoutComponent& layout, const Quaternion& rotation, const Vector3& scale){
        return getUIPivotShift(layout.pivot, Vector2(layout.width, layout.height), rotation, scale);
    }
    
}

#endif //UI_LAYOUT_COMPONENT_H
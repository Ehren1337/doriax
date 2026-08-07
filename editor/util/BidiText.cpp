#include "BidiText.h"

#include "util/StringUtils.h"

#include <SheenBidi/SheenBidi.h>

namespace doriax::editor {

// Presentation form of each letter, 0 where that form does not exist. A letter with no
// initial form does not connect to the next one.
struct ArabicForms {
    uint32_t letter;
    uint32_t isolated;
    uint32_t final;
    uint32_t initial;
    uint32_t medial;
};

static const ArabicForms arabicForms[] = {
    {0x0621, 0xFE80, 0,      0,      0     },
    {0x0622, 0xFE81, 0xFE82, 0,      0     },
    {0x0623, 0xFE83, 0xFE84, 0,      0     },
    {0x0624, 0xFE85, 0xFE86, 0,      0     },
    {0x0625, 0xFE87, 0xFE88, 0,      0     },
    {0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C},
    {0x0627, 0xFE8D, 0xFE8E, 0,      0     },
    {0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92},
    {0x0629, 0xFE93, 0xFE94, 0,      0     },
    {0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98},
    {0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C},
    {0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0},
    {0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4},
    {0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8},
    {0x062F, 0xFEA9, 0xFEAA, 0,      0     },
    {0x0630, 0xFEAB, 0xFEAC, 0,      0     },
    {0x0631, 0xFEAD, 0xFEAE, 0,      0     },
    {0x0632, 0xFEAF, 0xFEB0, 0,      0     },
    {0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4},
    {0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8},
    {0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC},
    {0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0},
    {0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4},
    {0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8},
    {0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC},
    {0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0},
    {0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4},
    {0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8},
    {0x0643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC},
    {0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0},
    {0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4},
    {0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8},
    {0x0647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC},
    {0x0648, 0xFEED, 0xFEEE, 0,      0     },
    {0x0649, 0xFEEF, 0xFEF0, 0,      0     },
    {0x064A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4}
};

// Lam followed by one of these collapses into a single glyph.
struct LamAlef {
    uint32_t alef;
    uint32_t isolated;
    uint32_t final;
};

static const LamAlef lamAlefs[] = {
    {0x0622, 0xFEF5, 0xFEF6},
    {0x0623, 0xFEF7, 0xFEF8},
    {0x0625, 0xFEF9, 0xFEFA},
    {0x0627, 0xFEFB, 0xFEFC}
};

static const uint32_t TATWEEL = 0x0640;

static const ArabicForms* findForms(uint32_t codepoint) {
    if (codepoint < 0x0621 || codepoint > 0x064A) {
        return nullptr;
    }

    for (const ArabicForms& forms : arabicForms) {
        if (forms.letter == codepoint) {
            return &forms;
        }
    }
    return nullptr;
}

static const LamAlef* findLamAlef(uint32_t codepoint) {
    for (const LamAlef& ligature : lamAlefs) {
        if (ligature.alef == codepoint) {
            return &ligature;
        }
    }
    return nullptr;
}

// Harakat sit above or below a letter without breaking the join.
static bool isTransparent(uint32_t codepoint) {
    return (codepoint >= 0x064B && codepoint <= 0x065F) || codepoint == 0x0670
        || (codepoint >= 0x06D6 && codepoint <= 0x06ED);
}

static bool joinsForward(uint32_t codepoint) {
    const ArabicForms* forms = findForms(codepoint);
    return codepoint == TATWEEL || (forms && forms->initial != 0);
}

static bool joinsBackward(uint32_t codepoint) {
    const ArabicForms* forms = findForms(codepoint);
    return codepoint == TATWEEL || (forms && forms->final != 0);
}

// Picks the form of every letter in the run, still in logical order.
static void shapeArabic(const std::vector<uint32_t>& codepoints, size_t offset, size_t length, std::vector<uint32_t>& out) {
    for (size_t i = offset; i < offset + length; i++) {
        uint32_t codepoint = codepoints[i];

        if (isTransparent(codepoint)) {
            out.push_back(codepoint);
            continue;
        }

        size_t previous = i;
        while (previous > offset && isTransparent(codepoints[previous - 1])) {
            previous--;
        }
        bool afterJoiner = previous > offset && joinsForward(codepoints[previous - 1]);

        size_t next = i + 1;
        while (next < offset + length && isTransparent(codepoints[next])) {
            next++;
        }
        bool beforeJoinable = next < offset + length && joinsBackward(codepoints[next]);

        if (codepoint == 0x0644 && next < offset + length) {
            if (const LamAlef* ligature = findLamAlef(codepoints[next])) {
                out.push_back(afterJoiner ? ligature->final : ligature->isolated);
                i = next;
                continue;
            }
        }

        const ArabicForms* forms = findForms(codepoint);
        if (!forms) {
            out.push_back(codepoint);
            continue;
        }

        bool joinPrevious = afterJoiner && forms->final != 0;
        bool joinNext = beforeJoinable && forms->initial != 0;

        uint32_t form = forms->isolated;
        if (joinPrevious && joinNext) {
            form = forms->medial;
        } else if (joinPrevious) {
            form = forms->final;
        } else if (joinNext) {
            form = forms->initial;
        }

        out.push_back(form != 0 ? form : codepoint);
    }
}

std::string BidiText::toVisual(const std::string& text) {
    bool hadInvalid = false;
    std::vector<uint32_t> codepoints = StringUtils::decodeUtf8ToCodepoints(text, hadInvalid);

    bool rightToLeft = false;
    for (uint32_t codepoint : codepoints) {
        // rebuilding the string goes through wchar_t, which loses astral characters
        // where it is 16 bits, so those strings are left alone
        if (codepoint > 0xFFFF) {
            return text;
        }
        if (codepoint >= 0x0590 && codepoint <= 0x08FF) {
            rightToLeft = true;
        }
    }

    if (!rightToLeft) {
        return text;
    }

    SBCodepointSequence sequence;
    sequence.stringEncoding = SBStringEncodingUTF32;
    sequence.stringBuffer = (void*)codepoints.data();
    sequence.stringLength = (SBUInteger)codepoints.size();

    SBAlgorithmRef algorithm = SBAlgorithmCreate(&sequence);
    if (!algorithm) {
        return text;
    }

    std::vector<uint32_t> visual;
    visual.reserve(codepoints.size());

    // One paragraph per line, so a newline does not drag the next line into its order.
    SBUInteger start = 0;
    while (start < codepoints.size()) {
        SBParagraphRef paragraph = SBAlgorithmCreateParagraph(algorithm, start, INT32_MAX, SBLevelDefaultLTR);
        if (!paragraph) {
            break;
        }

        SBUInteger paragraphLength = SBParagraphGetLength(paragraph);
        SBLineRef line = paragraphLength > 0 ? SBParagraphCreateLine(paragraph, start, paragraphLength) : nullptr;

        if (line) {
            SBUInteger runCount = SBLineGetRunCount(line);
            const SBRun* runs = SBLineGetRunsPtr(line);

            for (SBUInteger i = 0; i < runCount; i++) {
                size_t offset = (size_t)runs[i].offset;
                size_t length = (size_t)runs[i].length;

                if ((runs[i].level & 1) == 0) {
                    visual.insert(visual.end(), codepoints.begin() + offset, codepoints.begin() + offset + length);
                    continue;
                }

                std::vector<uint32_t> shaped;
                shapeArabic(codepoints, offset, length, shaped);
                visual.insert(visual.end(), shaped.rbegin(), shaped.rend());
            }

            SBLineRelease(line);
        }

        SBParagraphRelease(paragraph);

        if (paragraphLength == 0) {
            break;
        }
        start += paragraphLength;
    }

    SBAlgorithmRelease(algorithm);

    std::string out;
    out.reserve(text.size());
    for (uint32_t codepoint : visual) {
        out += StringUtils::toUTF8((wchar_t)codepoint);
    }

    return out;
}

}

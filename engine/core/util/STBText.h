//
// (c) 2026 Eduardo Doria.
//

#ifndef STBText_h
#define STBText_h

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "buffer/InterleavedBuffer.h"
#include "io/Data.h"
#include "stb_truetype.h"
#include "texture/Texture.h"

namespace doriax {

    //glyph atlas of a font in a single size. Glyphs are keyed by glyph index and
    //rasterized on demand, so any script the font covers can be rendered
    class DORIAX_API STBText {

    private:

        struct FontGlyph {
            //quad corners relative to the pen position, y-down from the baseline
            float xoff;
            float yoff;
            float xoff2;
            float yoff2;

            float s0;
            float t0;
            float s1;
            float t1;

            float xadvance;
        };

        //a row of the atlas, shared by glyphs of similar height
        struct Shelf {
            int x;
            int y;
            int h;
        };

        //max texture size guaranteed by GLES3 and WebGL2
        const unsigned int atlasLimit = 4096;
        const int atlasPadding = 1;

        //stb_truetype reads from this while rasterizing, it must outlive the glyphs
        Data fontData;
        stbtt_fontinfo fontInfo;
        bool fontLoaded;
        float scale;

        std::unordered_map<uint32_t, FontGlyph> glyphMap;
        std::unordered_map<uint32_t, uint32_t> codepointMap;

        std::vector<unsigned char> atlasPixels;
        std::vector<Shelf> shelves;
        unsigned int atlasWidth;
        unsigned int atlasHeight;

        //buffers of previous atlas sizes, TextureData points to them without owning
        std::vector<std::vector<unsigned char>> retiredAtlases;

        //bumped on every atlas change, texts built with an older one are outdated
        unsigned long atlasVersion;

        float ascent;
        float descent;
        float lineGap;
        int lineHeight;

        TextureData* textureData;

        void resetAtlas(unsigned int width, unsigned int height);
        bool packRect(int width, int height, int& outX, int& outY);
        bool growAtlas();
        void rasterizeGlyph(uint32_t glyphIndex, FontGlyph& glyph, int x, int y);
        const FontGlyph* getGlyph(uint32_t glyphIndex);
        const FontGlyph* getGlyphForCodepoint(uint32_t codepoint);
        void refreshTextureData();

    public:
        STBText();
        virtual ~STBText();

        float getAscent();
        float getDescent();
        float getLineGap();
        int getLineHeight();
        float getCharWidth(uint32_t codepoint);

        unsigned long getAtlasVersion() const;

        TextureData* load(const std::string& fontpath, unsigned int fontSize);
        void createText(const std::string& text, Buffer* buffer, std::vector<uint16_t>& indices, std::vector<Vector2>& charPositions,
                        unsigned int& width, unsigned int& height, bool fixedWidth, bool fixedHeight, bool multiline, bool invert);

        TextureData* getTextureData();
        
    };
    
}

#endif /* STBText_h */

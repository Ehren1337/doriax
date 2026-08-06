//
// (c) 2026 Eduardo Doria.
//

#include "STBText.h"

#include <cmath>
#include <string>
#include "Log.h"
#include "io/Data.h"
#include "DefaultFont.h"
#include "StringUtils.h"

using namespace doriax;

STBText::STBText() {
    fontLoaded = false;
    scale = 0;

    atlasWidth = 0;
    atlasHeight = 0;
    atlasVersion = 0;

    ascent = 0;
    descent = 0;
    lineGap = 0;
    lineHeight = 0;

    textureData = NULL;
}

STBText::~STBText() {
    if (textureData){
        delete textureData;
    }
}

void STBText::resetAtlas(unsigned int width, unsigned int height){
    if (!atlasPixels.empty()){
        retiredAtlases.push_back(std::move(atlasPixels));
    }

    atlasWidth = width;
    atlasHeight = height;

    atlasPixels.assign((size_t)atlasWidth * (size_t)atlasHeight, 0);
    shelves.clear();

    atlasVersion++;
}

bool STBText::packRect(int width, int height, int& outX, int& outY){
    outX = 0;
    outY = 0;

    if (width <= 0 || height <= 0){
        return true;
    }

    width += atlasPadding;
    height += atlasPadding;

    //shortest shelf that still fits, to not waste the taller ones
    Shelf* best = NULL;
    for (Shelf& shelf : shelves){
        if (shelf.h >= height && (shelf.x + width) <= (int)atlasWidth){
            if (!best || shelf.h < best->h)
                best = &shelf;
        }
    }

    if (best){
        outX = best->x;
        outY = best->y;
        best->x += width;

        return true;
    }

    int shelfY = 0;
    if (!shelves.empty()){
        shelfY = shelves.back().y + shelves.back().h;
    }

    if (width > (int)atlasWidth || (shelfY + height) > (int)atlasHeight){
        return false;
    }

    shelves.push_back({width, shelfY, height});

    outY = shelfY;

    return true;
}

bool STBText::growAtlas(){
    if (atlasWidth * 2 > atlasLimit || atlasHeight * 2 > atlasLimit){
        return false;
    }

    std::vector<uint32_t> cached;
    cached.reserve(glyphMap.size());
    for (const auto& [glyphIndex, _] : glyphMap){
        cached.push_back(glyphIndex);
    }

    resetAtlas(atlasWidth * 2, atlasHeight * 2);
    glyphMap.clear();

    for (uint32_t glyphIndex : cached){
        getGlyph(glyphIndex);
    }

    return true;
}

void STBText::rasterizeGlyph(uint32_t glyphIndex, FontGlyph& glyph, int x, int y){
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&fontInfo, glyphIndex, scale, scale, &x0, &y0, &x1, &y1);

    int gw = x1 - x0;
    int gh = y1 - y0;

    if (gw > 0 && gh > 0){
        stbtt_MakeGlyphBitmap(&fontInfo, &atlasPixels[(size_t)y * atlasWidth + x], gw, gh, atlasWidth, scale, scale, glyphIndex);
    }

    int advance, leftSideBearing;
    stbtt_GetGlyphHMetrics(&fontInfo, glyphIndex, &advance, &leftSideBearing);

    glyph.xoff = x0;
    glyph.yoff = y0;
    glyph.xoff2 = x1;
    glyph.yoff2 = y1;

    glyph.s0 = (float)x / atlasWidth;
    glyph.t0 = (float)y / atlasHeight;
    glyph.s1 = (float)(x + gw) / atlasWidth;
    glyph.t1 = (float)(y + gh) / atlasHeight;

    glyph.xadvance = advance * scale;

    atlasVersion++;
}

const STBText::FontGlyph* STBText::getGlyph(uint32_t glyphIndex){
    if (!fontLoaded)
        return NULL;

    auto it = glyphMap.find(glyphIndex);
    if (it != glyphMap.end()){
        return &it->second;
    }

    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&fontInfo, glyphIndex, scale, scale, &x0, &y0, &x1, &y1);

    int x, y;
    if (!packRect(x1 - x0, y1 - y0, x, y)){
        //growAtlas() repacks the cached glyphs, this one is still missing
        if (!growAtlas() || !packRect(x1 - x0, y1 - y0, x, y)){
            Log::error("Failed to pack glyph in font atlas");
            return NULL;
        }
    }

    FontGlyph& glyph = glyphMap[glyphIndex];
    rasterizeGlyph(glyphIndex, glyph, x, y);

    return &glyph;
}

const STBText::FontGlyph* STBText::getGlyphForCodepoint(uint32_t codepoint){
    if (!fontLoaded)
        return NULL;

    auto it = codepointMap.find(codepoint);
    if (it != codepointMap.end()){
        return getGlyph(it->second);
    }

    //a codepoint the font does not map resolves to glyph 0, the .notdef box
    uint32_t glyphIndex = stbtt_FindGlyphIndex(&fontInfo, codepoint);
    codepointMap[codepoint] = glyphIndex;

    return getGlyph(glyphIndex);
}

void STBText::refreshTextureData(){
    unsigned int textureSize = atlasWidth * atlasHeight * sizeof(unsigned char);

    if (textureData){
        delete textureData;
    }
    textureData = new TextureData(atlasWidth, atlasHeight, textureSize, ColorFormat::RED, 1, (void*)atlasPixels.data());
}

float STBText::getAscent(){
    return ascent;
}

float STBText::getDescent(){
    return descent;
}

float STBText::getLineGap(){
    return lineGap;
}

int STBText::getLineHeight(){
    return lineHeight;
}

float STBText::getCharWidth(uint32_t codepoint){
    const FontGlyph* glyph = getGlyphForCodepoint(codepoint);
    if (!glyph)
        return 0;

    return glyph->xadvance;
}

unsigned long STBText::getAtlasVersion() const{
    return atlasVersion;
}

TextureData* STBText::load(const std::string& fontpath, unsigned int fontSize){

    fontLoaded = false;

    if (!fontpath.empty()) {
        if (fontData.open(fontpath.c_str()) != FileErrors::FILEDATA_OK) {
            Log::error("Font file not found: %s", fontpath.c_str());
            return NULL;
        }
    }else{
        if (fontData.open(roboto_v20_latin_regular_ttf, roboto_v20_latin_regular_ttf_len, false, false) != FileErrors::FILEDATA_OK) {
            Log::error("Can't open default font");
            return NULL;
        }
    }

    //font collections (.ttc) keep the first table directory past the start of the file
    int fontOffset = stbtt_GetFontOffsetForIndex(fontData.getMemPtr(), 0);
    if (fontOffset < 0) {
        fontOffset = 0;
    }

    if (!stbtt_InitFont(&fontInfo, fontData.getMemPtr(), fontOffset)) {
        Log::error("Failed to initialize font: %s", fontpath.c_str());
        return NULL;
    }
    scale = stbtt_ScaleForPixelHeight(&fontInfo, fontSize);

    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&fontInfo, &ascent, &descent, &lineGap);

    this->ascent = ascent * scale;
    this->descent = descent * scale;
    this->lineGap = lineGap * scale;
    this->lineHeight = (ascent - descent + lineGap) * scale;

    glyphMap.clear();
    codepointMap.clear();

    resetAtlas(512, 512);

    fontLoaded = true;

    refreshTextureData();

    return textureData;
}

void STBText::createText(const std::string& text, Buffer* buffer, std::vector<uint16_t>& indices, std::vector<Vector2>& charPositions,
                         unsigned int& width, unsigned int& height, bool fixedWidth, bool fixedHeight, bool multiline, bool invert){

    bool hadInvalid = false;
    std::vector<uint32_t> codepoints = StringUtils::decodeUtf8ToCodepoints(text, hadInvalid);
    if (hadInvalid) {
        Log::warn("Invalid character");
    }

    unsigned long startVersion = atlasVersion;

    //cache every glyph first, a grow in the middle of the layout would repack the
    //ones already written in the buffer
    for (uint32_t codepoint : codepoints){
        getGlyphForCodepoint(codepoint);
    }

    float offsetX = 0;
    float offsetY = 0;

    Attribute* atrVertice = buffer->getAttribute(AttributeType::POSITION);
    Attribute* atrTexcoord = buffer->getAttribute(AttributeType::TEXCOORD1);

    if (multiline && fixedWidth){

        int lastSpace = 0;
        for (int i = 0; i < (int)codepoints.size(); i++){
            uint32_t intchar = codepoints[(size_t)i];
            if (intchar == 32){ //space
                lastSpace = i;
            }
            if (intchar == 10){ //\n
                offsetX = 0;
                continue;
            }

            const FontGlyph* glyph = getGlyphForCodepoint(intchar);
            if (glyph) {
                offsetX += glyph->xadvance;

                if (offsetX > (float)width){
                    if (lastSpace > 0){
                        codepoints[(size_t)lastSpace] = '\n';
                        i = lastSpace;
                        lastSpace = 0;
                    }else{
                        codepoints.insert(codepoints.begin() + i, (uint32_t)'\n');
                    }
                    offsetX = 0;
                }
            }
        }

        offsetX = 0;
        offsetY = 0;
    }

    int minX0 = 0, maxX1 = 0, minY0 = 0, maxY1 = 0;
    int ind = 0;
    int lineCount = 1;
    charPositions.clear();

    for (int i = 0; i < (int)codepoints.size(); i++){

        uint32_t intchar = codepoints[(size_t)i];

        if (intchar == 10){ //\n
            offsetY += lineHeight;
            offsetX = 0;
            lineCount++;

            continue;
        }

        const FontGlyph* glyph = getGlyphForCodepoint(intchar);
        if (!glyph)
            continue;

        //aligned to integer, like stbtt_GetPackedQuad does
        float quadX = std::floor(offsetX + glyph->xoff + 0.5f);
        float quadY = std::floor(offsetY + glyph->yoff + 0.5f);

        stbtt_aligned_quad quad;
        quad.x0 = quadX;
        quad.y0 = quadY;
        quad.x1 = quadX + (glyph->xoff2 - glyph->xoff);
        quad.y1 = quadY + (glyph->yoff2 - glyph->yoff);
        quad.s0 = glyph->s0;
        quad.t0 = glyph->t0;
        quad.s1 = glyph->s1;
        quad.t1 = glyph->t1;

        offsetX += glyph->xadvance;

        charPositions.push_back(Vector2(offsetX, offsetY));
            
        if (invert) {
            float auxt0 = quad.t0;
            quad.t0 = quad.t1;
            quad.t1 = auxt0;

            float auxy0 = quad.y0;
            quad.y0 = -quad.y1;
            quad.y1 = -auxy0;
        }
            
        if (quad.x0 < minX0)
            minX0 = quad.x0;
        if (quad.y0 < minY0)
            minY0 = quad.y0;
        if (quad.x1 > maxX1)
            maxX1 = quad.x1;
        if (quad.y1 > maxY1)
            maxY1 = quad.y1;
        if (offsetX > maxX1)
            maxX1 = offsetX;
            
        if ((!fixedWidth || offsetX <= width) && (!fixedHeight || offsetY <= height)){
            buffer->addVector3(atrVertice, Vector3(quad.x0, quad.y0, 0));
            buffer->addVector3(atrVertice, Vector3(quad.x1, quad.y0, 0));
            buffer->addVector3(atrVertice, Vector3(quad.x1, quad.y1, 0));
            buffer->addVector3(atrVertice, Vector3(quad.x0, quad.y1, 0));

            buffer->addVector2(atrTexcoord, Vector2(quad.s0, quad.t0));
            buffer->addVector2(atrTexcoord, Vector2(quad.s1, quad.t0));
            buffer->addVector2(atrTexcoord, Vector2(quad.s1, quad.t1));
            buffer->addVector2(atrTexcoord, Vector2(quad.s0, quad.t1));
                
            indices.push_back(ind);
            indices.push_back(ind+1);
            indices.push_back(ind+2);
            indices.push_back(ind);
            indices.push_back(ind+2);
            indices.push_back(ind+3);
            ind = ind + 4;
        }

    }
    //Empty text
    if (codepoints.size() == 0){
        buffer->addVector3(atrVertice, Vector3(0.0f, 0.0f, 0.0f));
        buffer->addVector3(atrVertice, Vector3(0.0f, 0.0f, 0.0f));
        buffer->addVector3(atrVertice, Vector3(0.0f, 0.0f, 0.0f));

        buffer->addVector2(atrTexcoord, Vector2(0.0f, 0.0f));
        buffer->addVector2(atrTexcoord, Vector2(0.0f, 0.0f));
        buffer->addVector2(atrTexcoord, Vector2(0.0f, 0.0f));
        
        indices.push_back(0);
        indices.push_back(1);
        indices.push_back(2);
    }
    if (!fixedWidth)
        width = maxX1 - minX0;
    if (!fixedHeight)
        height = lineCount * lineHeight;

    if (atlasVersion != startVersion){
        refreshTextureData();
    }
}

TextureData* STBText::getTextureData(){
    return textureData;
}

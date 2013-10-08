//
// Copyright (c) 2008-2013 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "Precompiled.h"
#include "AreaAllocator.h"
#include "Context.h"
#include "Deserializer.h"
#include "FileSystem.h"
#include "Font.h"
#include "Graphics.h"
#include "Log.h"
#include "MemoryBuffer.h"
#include "Profiler.h"
#include "ResourceCache.h"
#include "StringUtils.h"
#include "Texture2D.h"
#include "XMLFile.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include "DebugNew.h"

namespace Urho3D
{

static const int MIN_POINT_SIZE = 1;
static const int MAX_POINT_SIZE = 96;

/// FreeType library subsystem.
class FreeTypeLibrary : public Object
{
    OBJECT(FreeTypeLibrary);
    
public:
    /// Construct.
    FreeTypeLibrary(Context* context) :
        Object(context)
    {
        FT_Error error = FT_Init_FreeType(&library_);
        if (error)
            LOGERROR("Could not initialize FreeType library");
    }
    
    /// Destruct.
    virtual ~FreeTypeLibrary()
    {
        FT_Done_FreeType(library_);
    }
    
    FT_Library GetLibrary() const { return library_; }
    
private:
    /// FreeType library.
    FT_Library library_;
};

FontGlyph::FontGlyph()
{
}

FontFace::FontFace(Font* font, int pointSize) : font_(font), 
    pointSize_(pointSize), 
    hasKerning_(false)
{
}

FontFace::~FontFace()
{
}

const FontGlyph* FontFace::GetGlyph(unsigned c) const
{
    HashMap<unsigned, unsigned>::ConstIterator i = glyphMapping_.Find(c);
    if (i != glyphMapping_.End())
    {
        const FontGlyph& glyph = glyphs_[i->second_];
        return &glyph;
    }
    else
        return NULL;
}

short FontFace::GetKerning(unsigned c, unsigned d) const
{
    if (!hasKerning_)
        return 0;
    
    if (c == '\n' || d == '\n')
        return 0;
    
    unsigned leftIndex = 0;
    unsigned rightIndex = 0;
    HashMap<unsigned, unsigned>::ConstIterator leftIt = glyphMapping_.Find(c);
    if (leftIt != glyphMapping_.End())
        leftIndex = leftIt->second_;
    else
        return 0;
    HashMap<unsigned, unsigned>::ConstIterator rightIt = glyphMapping_.Find(d);
    if (rightIt != glyphMapping_.End())
        rightIndex = rightIt->second_;
    else
        return 0;
    
    HashMap<unsigned, unsigned>::ConstIterator kerningIt = glyphs_[leftIndex].kerning_.Find(rightIndex);
    if (kerningIt != glyphs_[leftIndex].kerning_.End())
        return kerningIt->second_;
    else
        return 0;
}

bool FontFace::IsDataLost() const
{
    for (unsigned i = 0; i < textures_.Size(); ++i)
    {
        if (textures_[i]->IsDataLost())
            return true;
    }
    return false;
}

SharedPtr<Texture> FontFace::LoadFaceTexture(SharedPtr<Image> image, bool staticTexture)
{
    Texture2D* texture = new Texture2D(font_->GetContext());
    texture->SetMipsToSkip(QUALITY_LOW, 0); // No quality reduction
    texture->SetNumLevels(1); // No mipmaps
    texture->SetAddressMode(COORD_U, ADDRESS_BORDER);
    texture->SetAddressMode(COORD_V, ADDRESS_BORDER),
        texture->SetBorderColor(Color(0.0f, 0.0f, 0.0f, 0.0f));
    if (!texture->Load(image, true, staticTexture ? TEXTURE_STATIC : TEXTURE_DYNAMIC))
    {
        delete texture;
        LOGERROR("Could not load texture from image resource");
        return SharedPtr<Texture>();
    }
    return SharedPtr<Texture>(texture);
}

unsigned FontFace::GetTotalTextureSize() const
{
    unsigned totalTextureSize = 0;
    for (unsigned i = 0; i < textures_.Size(); ++i)
        totalTextureSize += textures_[i]->GetWidth() * textures_[i]->GetHeight();

    return totalTextureSize;
}

FontFaceTTF::FontFaceTTF(Font* font, int pointSize) : FontFace(font, pointSize)
{

}

FontFaceTTF::~FontFaceTTF()
{

}

bool FontFaceTTF::Load(const unsigned char* fontData, unsigned fontDataSize)
{
    if (!font_)
        return false;

    if (pointSize_ <= 0)
    {
        LOGERROR("Zero or negative point size");
        return false;
    }

    if (!fontDataSize)
    {
        LOGERROR("Font not loaded");
        return false;
    }

    Context* context_ = font_->GetContext();

    // Create & initialize FreeType library if it does not exist yet
    FreeTypeLibrary* freeType = context_->GetSubsystem<FreeTypeLibrary>();
    if (!freeType)
        context_->RegisterSubsystem(freeType = new FreeTypeLibrary(context_));

    FT_Face face;
    FT_Error error;
    FT_Library library = freeType->GetLibrary();

    error = FT_New_Memory_Face(library, &fontData[0], fontDataSize, 0, &face);
    if (error)
    {
        LOGERROR("Could not create font face");
        return false;
    }
    error = FT_Set_Char_Size(face, 0, pointSize_ * 64, FONT_DPI, FONT_DPI);
    if (error)
    {
        FT_Done_Face(face);
        LOGERROR("Could not set font point size " + String(pointSize_));
        return false;
    }

    FT_GlyphSlot slot = face->glyph;
    unsigned numGlyphs = 0;

    // Build glyph mapping
    FT_UInt glyphIndex;
    FT_ULong charCode = FT_Get_First_Char(face, &glyphIndex);
    while (glyphIndex != 0)
    {
        numGlyphs = Max((int)glyphIndex + 1, (int)numGlyphs);
        glyphMapping_[charCode] = glyphIndex;
        charCode = FT_Get_Next_Char(face, charCode, &glyphIndex);
    }

    LOGDEBUG(ToString("Font face %s (%dpt) has %d glyphs", GetFileName(font_->GetName()).CString(), pointSize_, numGlyphs));

    // Load each of the glyphs to see the sizes & store other information
    int maxHeight = 0;
    FT_Pos ascender = face->size->metrics.ascender;

    glyphs_.Reserve(numGlyphs);

    for (unsigned i = 0; i < numGlyphs; ++i)
    {
        FontGlyph newGlyph;

        error = FT_Load_Glyph(face, i, FT_LOAD_DEFAULT);
        if (!error)
        {
            // Note: position within texture will be filled later
            newGlyph.width_ = (short)((slot->metrics.width) >> 6);
            newGlyph.height_ = (short)((slot->metrics.height) >> 6);
            newGlyph.offsetX_ = (short)((slot->metrics.horiBearingX) >> 6);
            newGlyph.offsetY_ = (short)((ascender - slot->metrics.horiBearingY) >> 6);
            newGlyph.advanceX_ = (short)((slot->metrics.horiAdvance) >> 6);

            maxHeight = Max(maxHeight, newGlyph.height_);
        }
        else
        {
            newGlyph.width_ = 0;
            newGlyph.height_ = 0;
            newGlyph.offsetX_ = 0;
            newGlyph.offsetY_ = 0;
            newGlyph.advanceX_ = 0;
        }

        glyphs_.Push(newGlyph);
    }

    // Store kerning if face has kerning information
    if (FT_HAS_KERNING(face))
    {
        hasKerning_ = true;

        for (unsigned i = 0; i < numGlyphs; ++i)
        {
            for (unsigned j = 0; j < numGlyphs; ++j)
            {
                FT_Vector vector;
                FT_Get_Kerning(face, i, j, FT_KERNING_DEFAULT, &vector);
                glyphs_[i].kerning_[j] = (short)(vector.x >> 6);
            }
        }
    }

    // Store point size and the height of a row. Use the height of the tallest font if taller than the specified row height
    rowHeight_ = Max((face->size->metrics.height + 63) >> 6, maxHeight);

    // Now try to pack into the smallest possible texture(s)
    Vector<SharedPtr<Image> > images;
    unsigned totalTextureSize = 0;
    unsigned page = 0;
    unsigned startIndex = 0;
    unsigned index;
    unsigned sumMaxOpacity = 0;
    unsigned samples = 0;

    while (startIndex < numGlyphs)
    {
        AreaAllocator allocator(FONT_TEXTURE_MIN_SIZE, FONT_TEXTURE_MIN_SIZE, FONT_TEXTURE_MAX_SIZE, FONT_TEXTURE_MAX_SIZE);
        for (index = startIndex; index < numGlyphs; ++index)
        {
            if (glyphs_[index].width_ && glyphs_[index].height_)
            {
                int x, y;
                // Reserve an empty border between glyphs for filtering
                if (allocator.Allocate(glyphs_[index].width_ + 1, glyphs_[index].height_ + 1, x, y))
                {
                    glyphs_[index].x_ = x;
                    glyphs_[index].y_ = y;
                    glyphs_[index].page_ = page;
                }
                else
                    break;
            }
            else
            {
                glyphs_[index].x_ = 0;
                glyphs_[index].y_ = 0;
                glyphs_[index].page_ = 0;
            }
        }

        int texWidth = allocator.GetWidth();
        int texHeight = allocator.GetHeight();

        // Create the image for rendering the fonts
        SharedPtr<Image> image(new Image(context_));
        images.Push(image);
        image->SetSize(texWidth, texHeight, 1);

        // First clear the whole image
        unsigned char* imageData = image->GetData();
        for (int y = 0; y < texHeight; ++y)
        {
            unsigned char* dest = imageData + texWidth * y;
            memset(dest, 0, texWidth);
        }

        // Render glyphs into texture, and find out a scaling value in case font uses less than full opacity (thin outlines)
        for (unsigned i = startIndex; i < index; ++i)
        {
            if (!glyphs_[i].width_ || !glyphs_[i].height_)
                continue;

            FT_Load_Glyph(face, i, FT_LOAD_DEFAULT);
            FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL);

            unsigned char glyphOpacity = 0;
            for (int y = 0; y < glyphs_[i].height_; ++y)
            {
                unsigned char* src = slot->bitmap.buffer + slot->bitmap.pitch * y;
                unsigned char* dest = imageData + texWidth * (y + glyphs_[i].y_) + glyphs_[i].x_;

                for (int x = 0; x < glyphs_[i].width_; ++x)
                {
                    dest[x] = src[x];
                    glyphOpacity = Max(glyphOpacity, src[x]);
                }
            }
            if (glyphOpacity)
            {
                sumMaxOpacity += glyphOpacity;
                ++samples;
            }
        }

        ++page;
        startIndex = index;
    }

    // Clamp the minimum possible value to avoid overbrightening
    unsigned char avgMaxOpacity = 255;
    if (samples)
        avgMaxOpacity = Max(sumMaxOpacity / samples, 128);

    if (avgMaxOpacity < 255)
    {
        // Apply the scaling value if necessary
        float scale = 255.0f / avgMaxOpacity;
        for (unsigned i = 0; i < numGlyphs; ++i)
        {
            Image* image = images[glyphs_[i].page_];
            unsigned char* imageData = image->GetData();
            for (int y = 0; y < glyphs_[i].height_; ++y)
            {
                unsigned char* dest = imageData + image->GetWidth() * (y + glyphs_[i].y_) + glyphs_[i].x_;
                for (int x = 0; x < glyphs_[i].width_; ++x)
                {
                    int pixel = dest[x];
                    dest[x] = Min((int)(pixel * scale), 255);
                }
            }
        }
    }

    // Create the texture and load the image into it
    for (Vector<SharedPtr<Image> >::ConstIterator i = images.Begin(); i != images.End(); ++i)
    {
        int texWidth = i->Get()->GetWidth();
        int texHeight = i->Get()->GetHeight();

        SharedPtr<Texture> texture = LoadFaceTexture(*i);
        if (!texture)
            return false;
        textures_.Push(texture);
        totalTextureSize += texWidth * texHeight;
    }

    FT_Done_Face(face);

    return true;
}

FontFaceBitmap::FontFaceBitmap(Font* font, int pointSize) : FontFace(font, pointSize)
{

}

FontFaceBitmap::~FontFaceBitmap()
{

}

bool FontFaceBitmap::Load(const unsigned char* fontData, unsigned fontDataSize)
{
    if (!font_)
        return false;

    Context* context_ = font_->GetContext();
    SharedPtr<XMLFile> xmlReader(new XMLFile(context_));
    MemoryBuffer memoryBuffer(fontData, fontDataSize);
    if (!xmlReader->Load(memoryBuffer))
    {
        LOGERROR("Could not load XML file");
        return false;
    }

    XMLElement root = xmlReader->GetRoot("font");
    if (root.IsNull())
    {
        LOGERROR("Could not find Font element");
        return false;
    }

    XMLElement pagesElem = root.GetChild("pages");
    if (pagesElem.IsNull())
    {
        LOGERROR("Could not find Pages element");
        return false;
    }

    XMLElement infoElem = root.GetChild("info");
    if (!infoElem.IsNull())
        pointSize_ = infoElem.GetInt("size");

    XMLElement commonElem = root.GetChild("common");
    rowHeight_ = commonElem.GetInt("lineHeight");
    unsigned pages = commonElem.GetInt("pages");
    textures_.Reserve(pages);

    ResourceCache* resourceCache = context_->GetSubsystem<ResourceCache>();
    String fontPath = GetPath(font_->GetName());
    unsigned totalTextureSize = 0;

    XMLElement pageElem = pagesElem.GetChild("page");
    for (unsigned i = 0; i < pages; ++i)
    {
        if (pageElem.IsNull())
        {
            LOGERROR("Could not find Page element for page: " + String(i));
            return false;
        }

        // Assume the font image is in the same directory as the font description file
        String textureFile = fontPath + pageElem.GetAttribute("file");

        // Load texture manually to allow controlling the alpha channel mode
        SharedPtr<File> fontFile = resourceCache->GetFile(textureFile);
        SharedPtr<Image> fontImage(new Image(context_));
        if (!fontFile || !fontImage->Load(*fontFile))
        {
            LOGERROR("Failed to load font image file");
            return false;
        }
        SharedPtr<Texture> texture = LoadFaceTexture(fontImage);
        if (!texture)
            return false;
        textures_.Push(texture);
        totalTextureSize += fontImage->GetWidth() * fontImage->GetHeight() * fontImage->GetComponents();

        pageElem = pageElem.GetNext("page");
    }

    XMLElement charsElem = root.GetChild("chars");
    int count = charsElem.GetInt("count");
    glyphs_.Reserve(count);
    unsigned index = 0;

    XMLElement charElem = charsElem.GetChild("char");
    while (!charElem.IsNull())
    {
        int id = charElem.GetInt("id");
        FontGlyph glyph;
        glyph.x_ = charElem.GetInt("x");
        glyph.y_ = charElem.GetInt("y");
        glyph.width_ = charElem.GetInt("width");
        glyph.height_ = charElem.GetInt("height");
        glyph.offsetX_ = charElem.GetInt("xoffset");
        glyph.offsetY_ = charElem.GetInt("yoffset");
        glyph.advanceX_ = charElem.GetInt("xadvance");
        glyph.page_ = charElem.GetInt("page");
        glyphs_.Push(glyph);
        glyphMapping_[id] = index++;

        charElem = charElem.GetNext("char");
    }

    XMLElement kerningsElem = root.GetChild("kernings");
    if (kerningsElem.IsNull())
        hasKerning_ = false;
    else
    {
        XMLElement kerningElem = kerningsElem.GetChild("kerning");
        while (!kerningElem.IsNull())
        {
            int first = kerningElem.GetInt("first");
            HashMap<unsigned, unsigned>::Iterator i = glyphMapping_.Find(first);
            if (i != glyphMapping_.End())
            {
                int second = kerningElem.GetInt("second");
                int amount = kerningElem.GetInt("amount");

                FontGlyph& glyph = glyphs_[i->second_];
                glyph.kerning_[second] = amount;
            }

            kerningElem = kerningElem.GetNext("kerning");
        }
    }

    LOGDEBUG(ToString("Bitmap font face %s has %d glyphs", GetFileName(font_->GetName()).CString(), count));

    return true;
}

Font::Font(Context* context) :
    Resource(context),
    fontDataSize_(0),
    fontType_(FONT_NONE)
{
}

Font::~Font()
{
}

void Font::RegisterObject(Context* context)
{
    context->RegisterFactory<Font>();
}

bool Font::Load(Deserializer& source)
{
    PROFILE(LoadFont);
    
    // In headless mode, do not actually load, just return success
    Graphics* graphics = GetSubsystem<Graphics>();
    if (!graphics)
        return true;
        
    faces_.Clear();
    
    fontDataSize_ = source.GetSize();
    if (fontDataSize_)
    {
        fontData_ = new unsigned char[fontDataSize_];
        if (source.Read(&fontData_[0], fontDataSize_) != fontDataSize_)
            return false;
    }
    else
    {
        fontData_.Reset();
        return false;
    }

    String ext = GetExtension(GetName());
    if (ext == ".ttf")
        fontType_ = FONT_TTF;
    else if (ext == ".xml" || ext == ".fnt")
        fontType_ = FONT_BITMAP;

    SetMemoryUse(fontDataSize_);
    return true;
}

const FontFace* Font::GetFace(int pointSize)
{
    // In headless mode, always return null
    Graphics* graphics = GetSubsystem<Graphics>();
    if (!graphics)
        return 0;
    
    // For bitmap font type, always return the same font face provided by the font's bitmap file regardless of the actual requested point size
    if (fontType_ == FONT_BITMAP)
        pointSize = 0;
    else
        pointSize = Clamp(pointSize, MIN_POINT_SIZE, MAX_POINT_SIZE);
    
    HashMap<int, SharedPtr<FontFace> >::Iterator i = faces_.Find(pointSize);
    if (i != faces_.End())
    {
        if (!i->second_->IsDataLost())
            return i->second_;
        else
        {
            // Erase and reload face if texture data lost (OpenGL mode only)
            faces_.Erase(i);
        }
    }
    
    PROFILE(GetFontFace);
    
    switch (fontType_)
    {
    case FONT_TTF:
        return GetFaceTTF(pointSize);

    case FONT_BITMAP:
        return GetFaceBitmap(pointSize);
    
    default:
        return 0;
    }
}

const FontFace* Font::GetFaceTTF(int pointSize)
{
    SharedPtr<FontFace> newFace(new FontFaceTTF(this, pointSize));
    if (!newFace->Load(fontData_, fontDataSize_))
        return 0;

    SetMemoryUse(GetMemoryUse() + newFace->GetTotalTextureSize());
    faces_[pointSize] = newFace;
    return newFace;
}

const FontFace* Font::GetFaceBitmap(int pointSize)
{
    SharedPtr<FontFace> newFace(new FontFaceBitmap(this, pointSize));
    if (!newFace->Load(fontData_, fontDataSize_))
        return 0;

    SetMemoryUse(GetMemoryUse() + newFace->GetTotalTextureSize());
    faces_[pointSize] = newFace;
    return newFace;
}

}

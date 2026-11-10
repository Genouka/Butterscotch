#include "font_runtime.h"

#include "common.h"
#include "runner.h"
#include "renderer.h"
#include "file_system.h"
#include "utils.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32

// TODO: Runtime TTF rasterisation is currently Windows-only. Other platforms need
// a vendored rasteriser (stb_truetype/FreeType) or a platform font API.
int32_t RuntimeFont_add(Runner* runner, const char* name, double size, bool bold, bool italic, int32_t first, int32_t last) {
    (void) runner;
    (void) name;
    (void) size;
    (void) bold;
    (void) italic;
    (void) first;
    (void) last;
    logWarn("[font_add] TODO: runtime TTF font loading is not implemented on this platform\n");
    return -1;
}

#else

#include <windows.h>
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

// The GDI font resource must stay alive for at least as long as the HFONT.
// We never RemoveFontMemResourceEx: fonts live for the process lifetime.
typedef struct {
    HANDLE resource;
    uint8_t* data;
} RuntimeFontResource;

static RuntimeFontResource* g_fontResources;
static int32_t g_fontResourceCount;
static int32_t g_fontResourceCapacity;

// A TrueType font can map nearly every BMP codepoint (Unifont: ~65k, CJK fonts:
// ~30k), and font_add(..., 0, 65535) asks for exactly that. The previous 16384
// cap silently dropped the tail of such fonts (most CJK, all Hangul). Whether
// the rasterised glyphs actually fit one texture is decided during packing.
#define RUNTIME_FONT_MAX_GLYPHS 65536
#define RUNTIME_FONT_MAX_ATLAS 8192
#define RUNTIME_FONT_RANGE_THRESHOLD 4096

typedef struct {
    uint32_t codepoint;
    UINT glyphIndex;
    int32_t advance;
    int32_t bearingX;
    int32_t bitmapTop;
    int32_t bitmapWidth;
    int32_t bitmapHeight;
    int32_t cellX;
    int32_t cellY;
    int32_t cellWidth;
    int32_t cellHeight;
    int32_t drawX;
    int32_t drawY;
} RuntimeFontGlyph;

static uint16_t readBe16(const uint8_t* p) {
    return (uint16_t) (((uint16_t) p[0] << 8) | p[1]);
}

static uint32_t readBe32(const uint8_t* p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

static int32_t utf8Encode(uint32_t codepoint, char* out) {
    if (codepoint <= 0x7F) {
        out[0] = (char) codepoint;
        return 1;
    }
    if (codepoint <= 0x7FF) {
        out[0] = (char) (0xC0 | (codepoint >> 6));
        out[1] = (char) (0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint <= 0xFFFF) {
        out[0] = (char) (0xE0 | (codepoint >> 12));
        out[1] = (char) (0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char) (0x80 | (codepoint & 0x3F));
        return 3;
    }
    out[0] = (char) (0xF0 | (codepoint >> 18));
    out[1] = (char) (0x80 | ((codepoint >> 12) & 0x3F));
    out[2] = (char) (0x80 | ((codepoint >> 6) & 0x3F));
    out[3] = (char) (0x80 | (codepoint & 0x3F));
    return 4;
}

static char* utf16BeToUtf8(const uint8_t* data, size_t byteLength) {
    char* out = (char*) safeMalloc(byteLength * 2 + 1);
    size_t outPos = 0;
    for (size_t i = 0; i + 1 < byteLength; i += 2) {
        uint32_t codepoint = readBe16(data + i);
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF && i + 3 < byteLength) {
            uint32_t low = readBe16(data + i + 2);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            } else {
                codepoint = 0xFFFD;
            }
        } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
            codepoint = 0xFFFD;
        }
        outPos += (size_t) utf8Encode(codepoint, out + outPos);
    }
    out[outPos] = '\0';
    return out;
}

static char* macRomanToAscii(const uint8_t* data, size_t byteLength) {
    char* out = (char*) safeMalloc(byteLength + 1);
    for (size_t i = 0; i < byteLength; i++) {
        uint8_t c = data[i];
        out[i] = (c >= 0x20 && c < 0x7F) ? (char) c : '?';
    }
    out[byteLength] = '\0';
    return out;
}

static char* ttfNameLookupTable(const uint8_t* nameTable, uint32_t nameTableLength, uint16_t wantPlatform, uint16_t wantNameId) {
    if (nameTableLength < 6) return NULL;
    uint16_t count = readBe16(nameTable + 2);
    uint16_t stringOffset = readBe16(nameTable + 4);
    if ((uint32_t) 6 + (uint32_t) count * 12 > nameTableLength) return NULL;
    if (stringOffset > nameTableLength) return NULL;

    const uint8_t* strings = nameTable + stringOffset;
    uint32_t stringsLength = nameTableLength - stringOffset;

    for (uint16_t i = 0; i < count; i++) {
        const uint8_t* record = nameTable + 6 + (size_t) i * 12;
        uint16_t platformId = readBe16(record + 0);
        uint16_t encodingId = readBe16(record + 2);
        uint16_t languageId = readBe16(record + 4);
        uint16_t nameId = readBe16(record + 6);
        uint16_t length = readBe16(record + 8);
        uint16_t offset = readBe16(record + 10);
        (void) languageId;
        if (platformId != wantPlatform || nameId != wantNameId) continue;
        if (offset > stringsLength || (uint32_t) offset + length > stringsLength) continue;

        char* decoded;
        if (platformId == 3 || (platformId == 0 && (encodingId == 1 || encodingId == 3 || encodingId == 4 || encodingId == 6))) {
            decoded = utf16BeToUtf8(strings + offset, length);
        } else if (platformId == 1) {
            decoded = macRomanToAscii(strings + offset, length);
        } else {
            continue;
        }

        // Drop empty names.
        if (decoded[0] == '\0') {
            free(decoded);
            continue;
        }
        return decoded;
    }
    return NULL;
}

static char* ttfFamilyName(const uint8_t* data, int32_t size) {
    if (data == NULL || size < 12) return NULL;
    uint16_t tableCount = readBe16(data + 4);
    if ((uint32_t) 12 + (uint32_t) tableCount * 16 > (uint32_t) size) return NULL;

    const uint8_t* nameTable = NULL;
    uint32_t nameTableLength = 0;
    for (uint16_t i = 0; i < tableCount; i++) {
        const uint8_t* record = data + 12 + (size_t) i * 16;
        if (memcmp(record, "name", 4) == 0) {
            uint32_t offset = readBe32(record + 8);
            uint32_t length = readBe32(record + 12);
            if (offset <= (uint32_t) size && length <= (uint32_t) size - offset) {
                nameTable = data + offset;
                nameTableLength = length;
            }
            break;
        }
    }
    if (nameTable == NULL) return NULL;

    static const uint16_t platforms[] = {3, 0, 1};
    for (size_t p = 0; p < sizeof(platforms) / sizeof(platforms[0]); p++) {
        char* name = ttfNameLookupTable(nameTable, nameTableLength, platforms[p], 1);
        if (name != NULL) return name;
    }
    for (size_t p = 0; p < sizeof(platforms) / sizeof(platforms[0]); p++) {
        char* name = ttfNameLookupTable(nameTable, nameTableLength, platforms[p], 4);
        if (name != NULL) return name;
    }
    return NULL;
}

static char* familyNameFromPath(const char* path) {
    const char* base = path;
    for (const char* p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    size_t length = strlen(base);
    const char* dot = strrchr(base, '.');
    if (dot != NULL && dot != base) length = (size_t) (dot - base);
    char* out = (char*) safeMalloc(length + 1);
    memcpy(out, base, length);
    out[length] = '\0';
    return out;
}

static void keepFontResource(HANDLE resource, uint8_t* data) {
    if (g_fontResourceCount == g_fontResourceCapacity) {
        g_fontResourceCapacity = g_fontResourceCapacity == 0 ? 4 : g_fontResourceCapacity * 2;
        g_fontResources = (RuntimeFontResource*) safeRealloc(g_fontResources, (size_t) g_fontResourceCapacity * sizeof(RuntimeFontResource));
    }
    g_fontResources[g_fontResourceCount].resource = resource;
    g_fontResources[g_fontResourceCount].data = data;
    g_fontResourceCount++;
}

static bool probeGlyph(HDC hdc, const MAT2* matrix, uint32_t codepoint, bool skipMissing, UINT* outGlyphIndex, GLYPHMETRICS* outMetrics) {
    if (codepoint > 0xFFFF) return false;

    WCHAR character = (WCHAR) codepoint;
    WORD glyphIndex = 0;
    if (!GetGlyphIndicesW(hdc, &character, 1, &glyphIndex, GGI_MARK_NONEXISTING_GLYPHS)) return false;

    bool missing = glyphIndex == 0xFFFF;
    if (missing && skipMissing) return false;
    if (missing) glyphIndex = 0;

    GLYPHMETRICS metrics;
    DWORD size = GetGlyphOutlineW(hdc, glyphIndex, GGO_GRAY8_BITMAP | GGO_GLYPH_INDEX, &metrics, 0, NULL, matrix);
    if (size == GDI_ERROR) return false;

    *outGlyphIndex = glyphIndex;
    *outMetrics = metrics;
    return true;
}

int32_t RuntimeFont_add(Runner* runner, const char* name, double size, bool bold, bool italic, int32_t first, int32_t last) {
    if (runner == NULL || name == NULL || runner->dataWin == NULL || runner->renderer == NULL) return -1;
    if (runner->renderer->vtable == NULL || runner->renderer->vtable->createFontTexture == NULL) {
        logWarn("[font_add] TODO: the active renderer does not support runtime font textures\n");
        return -1;
    }

    HDC hdc = CreateCompatibleDC(NULL);
    if (hdc == NULL) {
        logWarn("[font_add] Failed to create a GDI device context\n");
        return -1;
    }

    char* familyName = NULL;
    uint8_t* fontData = NULL;
    int32_t fontDataSize = 0;
    bool fromFile = false;

    if (runner->fileSystem != NULL && runner->fileSystem->vtable != NULL &&
        runner->fileSystem->vtable->readFileBinary != NULL &&
        runner->fileSystem->vtable->readFileBinary(runner->fileSystem, name, &fontData, &fontDataSize) &&
        fontData != NULL && fontDataSize > 0) {
        fromFile = true;
        familyName = ttfFamilyName(fontData, fontDataSize);
        if (familyName == NULL) familyName = familyNameFromPath(name);

        DWORD fontCount = 0;
        HANDLE resource = AddFontMemResourceEx(fontData, (DWORD) fontDataSize, NULL, &fontCount);
        if (resource == NULL || fontCount == 0) {
            logWarn("[font_add] Failed to register font data from '%s'\n", name);
            free(familyName);
            free(fontData);
            DeleteDC(hdc);
            return -1;
        }
        keepFontResource(resource, fontData);
        fontData = NULL;
    } else {
        familyName = safeStrdup(name);
    }

    int32_t pixelSize = (int32_t) (size + 0.5);
    if (pixelSize < 1) pixelSize = 1;
    if (pixelSize > 1024) pixelSize = 1024;

    int32_t requestedHeight = pixelSize;
    if (!fromFile) {
        // System font names are specified in points; TTF files use pixels.
        int32_t dpi = GetDeviceCaps(hdc, LOGPIXELSY);
        if (dpi <= 0) dpi = 96;
        requestedHeight = MulDiv(pixelSize, dpi, 72);
        if (requestedHeight < 1) requestedHeight = 1;
    }

    WCHAR* wideFamily = NULL;
    int32_t wideFamilyLength = MultiByteToWideChar(CP_UTF8, 0, familyName, -1, NULL, 0);
    if (wideFamilyLength > 0) {
        wideFamily = (WCHAR*) safeMalloc((size_t) wideFamilyLength * sizeof(WCHAR));
        MultiByteToWideChar(CP_UTF8, 0, familyName, -1, wideFamily, wideFamilyLength);
    }

    HFONT font = CreateFontW(
        -requestedHeight,
        0, 0, 0,
        bold ? FW_BOLD : FW_NORMAL,
        italic ? TRUE : FALSE,
        FALSE, FALSE,
        DEFAULT_CHARSET,
        OUT_TT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        wideFamily != NULL ? wideFamily : L"Arial"
    );
    free(wideFamily);
    free(familyName);

    if (font == NULL) {
        logWarn("[font_add] Failed to create a GDI font for '%s'\n", name);
        DeleteDC(hdc);
        return -1;
    }
    HGDIOBJ previousFont = SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));

    TEXTMETRICW textMetrics;
    if (!GetTextMetricsW(hdc, &textMetrics)) {
        SelectObject(hdc, previousFont);
        DeleteObject(font);
        DeleteDC(hdc);
        return -1;
    }

    MAT2 identityMatrix = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};

    int32_t rangeStart = first;
    int32_t rangeEnd = last;
    // The manual says first/last are ignored for TTF files (all glyphs are added);
    // installed system fonts honour the requested range.
    bool scanAll = fromFile || rangeStart < 0 || rangeEnd <= rangeStart || (rangeEnd - rangeStart) > RUNTIME_FONT_RANGE_THRESHOLD;
    if (scanAll) {
        rangeStart = 0x20;
        rangeEnd = 0xFFFF;
    }

    RuntimeFontGlyph* glyphs = (RuntimeFontGlyph*) safeMalloc((size_t) RUNTIME_FONT_MAX_GLYPHS * sizeof(RuntimeFontGlyph));
    int32_t glyphCount = 0;
    bool hasSpace = false;

    for (int32_t codepoint = rangeStart; codepoint <= rangeEnd && codepoint <= 0xFFFF; codepoint++) {
        if (codepoint == 0) continue;
        if (glyphCount >= RUNTIME_FONT_MAX_GLYPHS) {
            logWarn("[font_add] font '%s' exceeds the %d glyph limit; remaining glyphs were skipped\n", name, RUNTIME_FONT_MAX_GLYPHS);
            break;
        }

        UINT glyphIndex;
        GLYPHMETRICS metrics;
        if (!probeGlyph(hdc, &identityMatrix, (uint32_t) codepoint, scanAll, &glyphIndex, &metrics)) continue;

        RuntimeFontGlyph* glyph = &glyphs[glyphCount++];
        memset(glyph, 0, sizeof(*glyph));
        glyph->codepoint = (uint32_t) codepoint;
        glyph->glyphIndex = glyphIndex;
        glyph->advance = (int32_t) metrics.gmCellIncX;
        glyph->bearingX = (int32_t) metrics.gmptGlyphOrigin.x;
        glyph->bitmapTop = (int32_t) metrics.gmptGlyphOrigin.y;
        glyph->bitmapWidth = (int32_t) metrics.gmBlackBoxX;
        glyph->bitmapHeight = (int32_t) metrics.gmBlackBoxY;
        if (codepoint == 0x20) hasSpace = true;
    }

    if (!hasSpace && glyphCount < RUNTIME_FONT_MAX_GLYPHS) {
        UINT glyphIndex;
        GLYPHMETRICS metrics;
        RuntimeFontGlyph* glyph = &glyphs[glyphCount++];
        memset(glyph, 0, sizeof(*glyph));
        glyph->codepoint = 0x20;
        if (probeGlyph(hdc, &identityMatrix, 0x20, false, &glyphIndex, &metrics)) {
            glyph->glyphIndex = glyphIndex;
            glyph->advance = (int32_t) metrics.gmCellIncX;
            glyph->bearingX = (int32_t) metrics.gmptGlyphOrigin.x;
            glyph->bitmapTop = (int32_t) metrics.gmptGlyphOrigin.y;
            glyph->bitmapWidth = (int32_t) metrics.gmBlackBoxX;
            glyph->bitmapHeight = (int32_t) metrics.gmBlackBoxY;
        } else {
            glyph->advance = pixelSize / 3 > 0 ? pixelSize / 3 : 1;
        }
    }

    if (glyphCount == 0) {
        logWarn("[font_add] Font '%s' produced no rasterisable glyphs\n", name);
        free(glyphs);
        SelectObject(hdc, previousFont);
        DeleteObject(font);
        DeleteDC(hdc);
        return -1;
    }

    int32_t ascent = (int32_t) textMetrics.tmAscent;
    int32_t lineTop = 0;
    int32_t lineBottom = (int32_t) textMetrics.tmHeight;
    for (int32_t i = 0; i < glyphCount; i++) {
        RuntimeFontGlyph* glyph = &glyphs[i];
        int32_t top = ascent - glyph->bitmapTop;
        int32_t bottom = top + glyph->bitmapHeight;
        if (top < lineTop) lineTop = top;
        if (bottom > lineBottom) lineBottom = bottom;
    }
    int32_t lineHeight = lineBottom - lineTop;
    if (lineHeight <= 0) lineHeight = requestedHeight;

    int32_t atlasWidth = 1024;
    size_t totalArea = 0;
    for (int32_t i = 0; i < glyphCount; i++) {
        RuntimeFontGlyph* glyph = &glyphs[i];
        int32_t minX = glyph->bearingX < 0 ? glyph->bearingX : 0;
        int32_t maxX = glyph->bearingX + glyph->bitmapWidth;
        if (glyph->advance > maxX) maxX = glyph->advance;
        if (maxX <= minX) maxX = minX + 1;
        glyph->cellWidth = maxX - minX;
        glyph->cellHeight = lineHeight;
        glyph->drawX = glyph->bearingX - minX;
        glyph->drawY = (ascent - glyph->bitmapTop) - lineTop;
        totalArea += (size_t) (glyph->cellWidth + 1) * (size_t) (glyph->cellHeight + 1);
    }
    // Grow up to the full atlas width; the old half-atlas ceiling forced
    // many-glyph fonts to overflow the height even when a wider page fits.
    while (atlasWidth < RUNTIME_FONT_MAX_ATLAS && (size_t) atlasWidth * (size_t) atlasWidth < totalArea) {
        atlasWidth *= 2;
    }
    for (int32_t i = 0; i < glyphCount; i++) {
        if (glyphs[i].cellWidth + 1 > atlasWidth) atlasWidth = glyphs[i].cellWidth + 1;
    }
    if (atlasWidth > RUNTIME_FONT_MAX_ATLAS) {
        logWarn("[font_add] Font '%s' has a glyph wider than the maximum atlas (%d px)\n", name, RUNTIME_FONT_MAX_ATLAS);
        free(glyphs);
        SelectObject(hdc, previousFont);
        DeleteObject(font);
        DeleteDC(hdc);
        return -1;
    }

    int32_t penX = 0;
    int32_t penY = 0;
    int32_t rowHeight = 0;
    int32_t placedGlyphs = 0;
    for (int32_t i = 0; i < glyphCount; i++) {
        RuntimeFontGlyph* glyph = &glyphs[i];
        if (penX + glyph->cellWidth > atlasWidth) {
            penX = 0;
            penY += rowHeight + 1;
            rowHeight = 0;
        }
        if (penY + glyph->cellHeight > RUNTIME_FONT_MAX_ATLAS) {
            logWarn("[font_add] font '%s' atlas exceeded %d px; %d glyphs were skipped\n",
                    name, RUNTIME_FONT_MAX_ATLAS, glyphCount - placedGlyphs);
            break;
        }
        glyph->cellX = penX;
        glyph->cellY = penY;
        penX += glyph->cellWidth + 1;
        if (glyph->cellHeight > rowHeight) rowHeight = glyph->cellHeight;
        placedGlyphs++;
    }
    int32_t atlasHeight = penY + rowHeight;
    if (placedGlyphs == 0 || atlasHeight <= 0) {
        free(glyphs);
        SelectObject(hdc, previousFont);
        DeleteObject(font);
        DeleteDC(hdc);
        return -1;
    }

    size_t pixelCount = (size_t) atlasWidth * (size_t) atlasHeight;
    uint8_t* rgba = (uint8_t*) safeMalloc(pixelCount * 4);
    memset(rgba, 0xFF, pixelCount * 4);
    for (size_t i = 0; i < pixelCount; i++) rgba[i * 4 + 3] = 0;

    for (int32_t i = 0; i < placedGlyphs; i++) {
        RuntimeFontGlyph* glyph = &glyphs[i];
        if (glyph->bitmapWidth <= 0 || glyph->bitmapHeight <= 0) continue;

        GLYPHMETRICS ignoredMetrics;
        DWORD dataSize = GetGlyphOutlineW(hdc, glyph->glyphIndex, GGO_GRAY8_BITMAP | GGO_GLYPH_INDEX, &ignoredMetrics, 0, NULL, &identityMatrix);
        if (dataSize == GDI_ERROR || dataSize == 0) continue;
        uint8_t* bitmap = (uint8_t*) safeMalloc(dataSize);
        if (GetGlyphOutlineW(hdc, glyph->glyphIndex, GGO_GRAY8_BITMAP | GGO_GLYPH_INDEX, &ignoredMetrics, dataSize, bitmap, &identityMatrix) == GDI_ERROR) {
            free(bitmap);
            continue;
        }

        DWORD stride = ((DWORD) glyph->bitmapWidth + 3u) & ~3u;
        for (int32_t row = 0; row < glyph->bitmapHeight; row++) {
            for (int32_t col = 0; col < glyph->bitmapWidth; col++) {
                uint8_t coverage = bitmap[(size_t) row * stride + (size_t) col];
                if (coverage == 0) continue;
                if (coverage > 64) coverage = 64;
                uint8_t alpha = (uint8_t) (((uint32_t) coverage * 255u + 32u) / 64u);
                int32_t x = glyph->cellX + glyph->drawX + col;
                int32_t y = glyph->cellY + glyph->drawY + row;
                if (x < 0 || x >= atlasWidth || y < 0 || y >= atlasHeight) continue;
                rgba[((size_t) y * (size_t) atlasWidth + (size_t) x) * 4 + 3] = alpha;
            }
        }
        free(bitmap);
    }

    SelectObject(hdc, previousFont);
    DeleteObject(font);
    DeleteDC(hdc);

    int32_t tpagIndex = -1;
    int32_t pageId = runner->renderer->vtable->createFontTexture(runner->renderer, atlasWidth, atlasHeight, rgba, &tpagIndex);
    free(rgba);
    if (pageId < 0 || tpagIndex < 0) {
        logWarn("[font_add] Failed to create a texture page for '%s'\n", name);
        free(glyphs);
        return -1;
    }
    logInfo("[font_add] '%s': %d glyphs on a %dx%d atlas\n", name, placedGlyphs, atlasWidth, atlasHeight);

    FontGlyph* fontGlyphs = (FontGlyph*) safeMalloc((size_t) placedGlyphs * sizeof(FontGlyph));
    memset(fontGlyphs, 0, (size_t) placedGlyphs * sizeof(FontGlyph));
    for (int32_t i = 0; i < placedGlyphs; i++) {
        RuntimeFontGlyph* glyph = &glyphs[i];
        FontGlyph* out = &fontGlyphs[i];
        out->character = (uint16_t) glyph->codepoint;
        out->sourceX = (uint16_t) glyph->cellX;
        out->sourceY = (uint16_t) glyph->cellY;
        out->sourceWidth = (uint16_t) glyph->cellWidth;
        out->sourceHeight = (uint16_t) glyph->cellHeight;
        out->shift = (int16_t) glyph->advance;
        out->offset = (int16_t) (glyph->bearingX < 0 ? glyph->bearingX : 0);
        out->kerningCount = 0;
        out->kerning = NULL;
    }

    DataWin* dataWin = runner->dataWin;
    uint32_t newFontIndex = dataWin->font.count;
    dataWin->font.count = newFontIndex + 1;
    dataWin->font.fonts = (Font*) safeRealloc(dataWin->font.fonts, (size_t) dataWin->font.count * sizeof(Font));
    Font* outFont = &dataWin->font.fonts[newFontIndex];
    memset(outFont, 0, sizeof(*outFont));
    outFont->present = true;
    outFont->name = safeStrdup(name);
    outFont->displayName = safeStrdup(name);
    outFont->emSize = (float) (size > 0.0 ? size : (double) requestedHeight);
    outFont->bold = bold;
    outFont->italic = italic;
    outFont->rangeStart = (uint16_t) (rangeStart < 0 ? 0 : rangeStart);
    outFont->rangeEnd = (uint32_t) (rangeEnd < 0 ? 0 : rangeEnd);
    outFont->charset = 0;
    outFont->antiAliasing = 1;
    outFont->tpagIndex = tpagIndex;
    outFont->scaleX = 1.0f;
    outFont->scaleY = 1.0f;
    outFont->ascenderOffset = 0;
    outFont->glyphCount = (uint32_t) placedGlyphs;
    outFont->glyphs = fontGlyphs;
    outFont->maxGlyphHeight = (uint32_t) lineHeight;
    outFont->isSpriteFont = false;
    outFont->spriteIndex = -1;
    outFont->spriteOriginYAdjust = 0;
    Font_buildGlyphLUT(outFont);

    free(glyphs);
    return (int32_t) newFontIndex;
}

#endif /* _WIN32 */

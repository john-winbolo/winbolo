/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          ImGui Fonts
 *Filename:      imgui_fonts.h
 *Purpose:
 *  Shared font loading for all WinBolo ImGui contexts.
 *  Call imguiLoadBoloFont() once per context, after
 *  ImGui::CreateContext() and before the first NewFrame().
 *  Loads a TTF font with oversampling for crisp rendering
 *  on the SDL software renderer.
 *********************************************************/

#ifndef IMGUI_FONTS_H
#define IMGUI_FONTS_H

#include <SDL3/SDL.h>
#include "imgui.h"

/* Try to load a TTF font via SDL_IOFromFile (works on Android assets
 * and desktop).  Returns the buffer allocated with IM_ALLOC — ImGui
 * takes ownership via AddFontFromMemoryTTF. */
static inline unsigned char *imguiFontLoadData(const char *path, int *outSize) {
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) return nullptr;
    Sint64 size = SDL_GetIOSize(io);
    if (size <= 0) { SDL_CloseIO(io); return nullptr; }
    unsigned char *buf = (unsigned char *)IM_ALLOC((size_t)size);
    if (!buf) { SDL_CloseIO(io); return nullptr; }
    size_t bytesRead = SDL_ReadIO(io, buf, (size_t)size);
    SDL_CloseIO(io);
    if ((Sint64)bytesRead != size) { IM_FREE(buf); return nullptr; }
    *outSize = (int)size;
    return buf;
}

/* Build the merged glyph range covering every script that Inter
 * Variable actually carries: Default (0x0020-0x00FF) + Latin Extended
 * A/B + Cyrillic + Greek + Vietnamese. Without this, Polish/Czech/
 * Russian/Greek/Vietnamese strings render as boxes even though the
 * TTF has the glyphs.
 *
 * The returned pointer must outlive the font atlas — ImGui keeps a
 * reference, it does not copy. We back it with a function-local
 * `static ImVector<ImWchar>` so the storage lasts for the program
 * lifetime; the builder runs exactly once. */
static inline const ImWchar *imguiBoloGlyphRanges() {
    static ImVector<ImWchar> ranges;
    if (!ranges.empty()) return ranges.Data;

    ImFontAtlas *atlas = ImGui::GetIO().Fonts;
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(atlas->GetGlyphRangesDefault());

    /* Latin Extended A (0x0100-0x017F) and Latin Extended B
     * (0x0180-0x024F). These need to be added explicitly; ImGui has
     * no canned helper. The trailing 0 terminates the range list. */
    static const ImWchar kLatinExtA[] = { 0x0100, 0x017F, 0 };
    static const ImWchar kLatinExtB[] = { 0x0180, 0x024F, 0 };
    builder.AddRanges(kLatinExtA);
    builder.AddRanges(kLatinExtB);

    builder.AddRanges(atlas->GetGlyphRangesCyrillic());
    builder.AddRanges(atlas->GetGlyphRangesGreek());
    builder.AddRanges(atlas->GetGlyphRangesVietnamese());

    builder.BuildRanges(&ranges);
    return ranges.Data;
}

/* Load the Bolo UI font at the given pixel size.
 * Uses oversampling for crisp text on the SDL renderer.
 * Falls back to a scaled default font if the TTF is missing.
 * Returns the ImFont* so callers can load multiple sizes. */
static inline ImFont *imguiLoadBoloFontSized(float sizePixels) {
    static const char *fontPaths[] = {
        "data/fonts/InterVariable.ttf",
        "data/InterVariable.ttf",
        "InterVariable.ttf",
    };

    ImGuiIO &io = ImGui::GetIO();

    int fontDataSize = 0;
    unsigned char *fontData = nullptr;
    for (int i = 0; i < 3; i++) {
        fontData = imguiFontLoadData(fontPaths[i], &fontDataSize);
        if (fontData) break;
    }

    if (fontData) {
        ImFontConfig config;
        config.OversampleH = 3;
        config.OversampleV = 2;
        config.GlyphRanges = imguiBoloGlyphRanges();
        return io.Fonts->AddFontFromMemoryTTF(fontData, fontDataSize, sizePixels, &config);
    } else {
        /* Fallback: scale the built-in font with oversampling */
        ImFontConfig config;
        config.SizePixels = sizePixels;
        config.OversampleH = 3;
        config.OversampleV = 2;
        return io.Fonts->AddFontDefault(&config);
    }
}

static inline void imguiLoadBoloFont(float sizePixels) {
    imguiLoadBoloFontSized(sizePixels);
}

#endif /* IMGUI_FONTS_H */

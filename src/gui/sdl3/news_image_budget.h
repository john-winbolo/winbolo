/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          news_image_budget
 * Filename:      news_image_budget.h
 * Purpose:
 *   Decides which decoded news images news_image_cache keeps.
 *   Holds no textures or pixels, only each image's URL, size
 *   in bytes and the frame it was last drawn in, so the unit
 *   tests can drive it with no renderer.
 *
 *   Images are limited by their total decoded size. An image
 *   drawn in the current or the previous frame is never
 *   evicted: imgui_markdown asks for every image in a post on
 *   every frame, so evicting one in use would make it look
 *   new on the next frame and be downloaded again, and its
 *   texture may already be queued in this frame's draw list.
 *********************************************************/

#ifndef NEWS_IMAGE_BUDGET_H
#define NEWS_IMAGE_BUDGET_H

#include <cstddef>
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

class NewsImageBudget {
public:
    explicit NewsImageBudget(size_t maxBytes) : m_maxBytes(maxBytes) {}

    /* Starts a new frame. Call once per frame before any touch(). */
    void newFrame() { m_frame++; }

    /* Records that url's image was drawn this frame. No effect for a
     * URL that is not admitted. */
    void touch(const std::string &url);

    /* Makes room for a new image of `bytes` bytes and records it as
     * drawn this frame. On success returns true and fills `evict` with
     * the URLs whose textures the caller must now destroy, least
     * recently drawn first. Returns false, evicting nothing, when the
     * image is larger than the whole budget or would only fit by
     * evicting an image drawn in this frame or the previous one. An
     * admitted URL admitted again is replaced, not counted twice. */
    bool admit(const std::string &url, size_t bytes,
               std::vector<std::string> &evict);

    /* Removes url, if admitted, without anything else changing. */
    void forget(const std::string &url);

    void clear();

    size_t   usedBytes() const { return m_usedBytes; }
    size_t   count()     const { return m_map.size(); }
    bool     contains(const std::string &url) const {
        return m_map.find(url) != m_map.end();
    }

private:
    struct Slot {
        std::string url;
        size_t      bytes;
        uint64_t    lastDrawn;
    };
    using List = std::list<Slot>; /* most recently drawn at the front */

    bool evictable(const Slot &s) const { return s.lastDrawn + 1 < m_frame; }

    size_t   m_maxBytes;
    size_t   m_usedBytes = 0;
    uint64_t m_frame     = 0;
    List     m_lru;
    std::unordered_map<std::string, List::iterator> m_map;
};

#endif /* NEWS_IMAGE_BUDGET_H */

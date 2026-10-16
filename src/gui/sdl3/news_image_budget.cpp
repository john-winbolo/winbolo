/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          news_image_budget
 * Filename:      news_image_budget.cpp
 * Purpose:
 *   Size-limited, draw-aware eviction for the news image
 *   cache. See news_image_budget.h.
 *********************************************************/

#include "news_image_budget.h"

void NewsImageBudget::touch(const std::string &url) {
    auto it = m_map.find(url);
    if (it == m_map.end()) return;
    it->second->lastDrawn = m_frame;
    m_lru.splice(m_lru.begin(), m_lru, it->second);
}

bool NewsImageBudget::admit(const std::string &url, size_t bytes,
                            std::vector<std::string> &evict) {
    evict.clear();
    if (bytes > m_maxBytes) return false;

    /* Bytes the image already holds, if it is being replaced. */
    size_t replaced = 0;
    auto existing = m_map.find(url);
    if (existing != m_map.end()) replaced = existing->second->bytes;

    /* Walk from the least recently drawn end, picking evictable images
     * until the new one fits. Nothing is removed until it is known to
     * fit, so a refusal leaves the budget as it was. */
    size_t used = m_usedBytes - replaced;
    std::vector<List::iterator> victims;
    for (auto it = m_lru.end(); used + bytes > m_maxBytes && it != m_lru.begin();) {
        --it;
        if (it->url == url || !evictable(*it)) continue;
        victims.push_back(it);
        used -= it->bytes;
    }
    if (used + bytes > m_maxBytes) return false;

    for (auto victim : victims) {
        evict.push_back(victim->url);
        m_usedBytes -= victim->bytes;
        m_map.erase(victim->url);
        m_lru.erase(victim);
    }
    if (existing != m_map.end()) forget(url);

    m_lru.push_front(Slot{url, bytes, m_frame});
    m_map[url] = m_lru.begin();
    m_usedBytes += bytes;
    return true;
}

void NewsImageBudget::forget(const std::string &url) {
    auto it = m_map.find(url);
    if (it == m_map.end()) return;
    m_usedBytes -= it->second->bytes;
    m_lru.erase(it->second);
    m_map.erase(it);
}

void NewsImageBudget::clear() {
    m_lru.clear();
    m_map.clear();
    m_usedBytes = 0;
}

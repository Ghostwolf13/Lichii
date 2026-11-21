// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <unordered_map>

#include <QString>

class GameListPatchCache {
public:
    QString Get(quint64 title_id, bool enabled, const std::function<QString()>& generator) {
        if (!enabled || title_id == 0) {
            return generator();
        }
        if (const auto it = entries.find(title_id); it != entries.end()) {
            return it->second;
        }
        auto text = generator();
        if (!text.isEmpty()) {
            entries.emplace(title_id, text);
        }
        return text;
    }

    void Clear() {
        entries.clear();
    }

private:
    std::unordered_map<quint64, QString> entries;
};

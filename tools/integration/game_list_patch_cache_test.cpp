// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <iostream>
#include <source_location>

#include "qt_common/game_list/patch_cache.h"

namespace {

void Check(bool condition, std::source_location location = std::source_location::current()) {
    if (!condition) {
        std::cerr << "Patch cache regression failed at line " << location.line() << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    for (const quint64 title_id : {0x04004D300C5AE001ULL, 0x040015100B514001ULL}) {
        GameListPatchCache cache;
        QString installed;
        unsigned calls{};
        const auto generate = [&] {
            ++calls;
            return installed;
        };
        const auto read = [&] { return cache.Get(title_id, true, generate); };

        Check(read().isEmpty());
        installed = QStringLiteral("DLC (1)");
        Check(read() == installed);
        Check(calls == 2);
        Check(read() == installed);
        Check(calls == 2);

        installed = QStringLiteral("DLC (1, 2)");
        cache.Clear();
        Check(read() == installed);
        installed = QStringLiteral("DLC (2)");
        cache.Clear();
        Check(read() == installed);
        installed.clear();
        cache.Clear();
        Check(read().isEmpty());
        Check(read().isEmpty());
        Check(calls == 6);

        installed = QStringLiteral("Update (NAND) (1.0.0)");
        Check(read() == installed);
        installed = QStringLiteral("Update (NAND) (1.1.0)\nUpdate (SDMC) (1.2.0)");
        cache.Clear();
        Check(read() == installed);

        installed = QStringLiteral("[D] DLC (1)");
        Check(cache.Get(title_id, false, generate) == installed);
        Check(cache.Get(0, true, generate) == installed);
        installed.clear();
        Check(cache.Get(0, true, generate).isEmpty());

        GameListPatchCache next_scan;
        Check(next_scan.Get(title_id, true, generate).isEmpty());
    }
    std::cout << "Patch cache: empty results, scan invalidation, DLC changes and storage changes passed\n";
}

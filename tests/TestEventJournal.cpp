#include "core/state/EventJournalJsonl.h"

#include <filesystem>
#include <iostream>

int main() {
    const auto path = std::filesystem::path("build/Release/logs/test_event_journal.jsonl");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    autolife::core::EventJournalJsonl journal(path);

    autolife::core::JournalEvent first;
    first.ts_ms = 1000;
    first.type = autolife::core::JournalEventType::ORDER_SUBMITTED;
    first.market = "KRW-BTC";
    first.entity_id = "order-1";
    first.payload["price"] = 100000.0;

    autolife::core::JournalEvent second;
    second.ts_ms = 2000;
    second.type = autolife::core::JournalEventType::POSITION_OPENED;
    second.market = "KRW-BTC";
    second.entity_id = "pos-1";
    second.payload["quantity"] = 0.01;

    if (!journal.append(first)) {
        std::cerr << "[TEST] append(first) failed\n";
        return 1;
    }
    if (!journal.append(second)) {
        std::cerr << "[TEST] append(second) failed\n";
        return 1;
    }

    if (journal.lastSeq() < 2) {
        std::cerr << "[TEST] lastSeq should be >= 2, got " << journal.lastSeq() << "\n";
        return 1;
    }

    const auto rows = journal.readFrom(2);
    if (rows.empty()) {
        std::cerr << "[TEST] readFrom(2) should not be empty\n";
        return 1;
    }

    if (rows.front().market != "KRW-BTC") {
        std::cerr << "[TEST] unexpected market: " << rows.front().market << "\n";
        return 1;
    }

    std::cout << "[TEST] EventJournal PASSED\n";
    return 0;
}


#include "core/state/EventJournalJsonl.h"

#include <algorithm>
#include <fstream>

namespace autolife {
namespace core {

namespace {
std::uint64_t parseSeq(const nlohmann::json& line) {
    return line.value("seq", static_cast<std::uint64_t>(0));
}
}

EventJournalJsonl::EventJournalJsonl(std::filesystem::path file_path)
    : file_path_(std::move(file_path)) {
    std::ifstream in(file_path_, std::ios::binary);
    if (!in.is_open()) {
        return;
    }

    std::string row;
    while (std::getline(in, row)) {
        if (row.empty()) {
            continue;
        }
        try {
            nlohmann::json line = nlohmann::json::parse(row);
            last_seq_ = (std::max)(last_seq_, parseSeq(line));
        } catch (...) {
            // Ignore malformed line and continue scanning.
        }
    }
}

bool EventJournalJsonl::append(const JournalEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::filesystem::create_directories(file_path_.parent_path());
    std::ofstream out(file_path_, std::ios::binary | std::ios::app);
    if (!out.is_open()) {
        return false;
    }

    const std::uint64_t next_seq = last_seq_ + 1;
    nlohmann::json line;
    line["seq"] = next_seq;
    line["ts_ms"] = event.ts_ms;
    line["type"] = toString(event.type);
    line["market"] = event.market;
    line["entity_id"] = event.entity_id;
    line["payload"] = event.payload;

    out << line.dump() << "\n";
    last_seq_ = next_seq;
    return true;
}

std::vector<JournalEvent> EventJournalJsonl::readFrom(std::uint64_t seq_inclusive) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<JournalEvent> out;
    std::ifstream in(file_path_, std::ios::binary);
    if (!in.is_open()) {
        return out;
    }

    std::string row;
    while (std::getline(in, row)) {
        if (row.empty()) {
            continue;
        }

        nlohmann::json line;
        try {
            line = nlohmann::json::parse(row);
        } catch (...) {
            continue;
        }

        const auto seq = parseSeq(line);
        if (seq < seq_inclusive) {
            continue;
        }

        JournalEvent event;
        event.seq = seq;
        event.ts_ms = line.value("ts_ms", 0LL);
        event.type = fromString(line.value("type", std::string("ORDER_UPDATED")));
        event.market = line.value("market", std::string());
        event.entity_id = line.value("entity_id", std::string());
        event.payload = line.value("payload", nlohmann::json::object());
        out.push_back(std::move(event));
    }

    return out;
}

std::uint64_t EventJournalJsonl::lastSeq() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_seq_;
}

std::string EventJournalJsonl::toString(JournalEventType type) {
    switch (type) {
        case JournalEventType::ORDER_SUBMITTED: return "ORDER_SUBMITTED";
        case JournalEventType::ORDER_UPDATED: return "ORDER_UPDATED";
        case JournalEventType::FILL_APPLIED: return "FILL_APPLIED";
        case JournalEventType::POSITION_OPENED: return "POSITION_OPENED";
        case JournalEventType::POSITION_REDUCED: return "POSITION_REDUCED";
        case JournalEventType::POSITION_CLOSED: return "POSITION_CLOSED";
        case JournalEventType::POLICY_CHANGED: return "POLICY_CHANGED";
    }
    return "ORDER_UPDATED";
}

JournalEventType EventJournalJsonl::fromString(const std::string& value) {
    if (value == "ORDER_SUBMITTED") return JournalEventType::ORDER_SUBMITTED;
    if (value == "ORDER_UPDATED") return JournalEventType::ORDER_UPDATED;
    if (value == "FILL_APPLIED") return JournalEventType::FILL_APPLIED;
    if (value == "POSITION_OPENED") return JournalEventType::POSITION_OPENED;
    if (value == "POSITION_REDUCED") return JournalEventType::POSITION_REDUCED;
    if (value == "POSITION_CLOSED") return JournalEventType::POSITION_CLOSED;
    if (value == "POLICY_CHANGED") return JournalEventType::POLICY_CHANGED;
    return JournalEventType::ORDER_UPDATED;
}

} // namespace core
} // namespace autolife

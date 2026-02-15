#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>

#include "core/contracts/IEventJournal.h"

namespace autolife {
namespace core {

class EventJournalJsonl : public IEventJournal {
public:
    explicit EventJournalJsonl(std::filesystem::path file_path);

    bool append(const JournalEvent& event) override;
    std::vector<JournalEvent> readFrom(std::uint64_t seq_inclusive) override;
    std::uint64_t lastSeq() const override;

private:
    static std::string toString(JournalEventType type);
    static JournalEventType fromString(const std::string& value);

    std::filesystem::path file_path_;
    mutable std::mutex mutex_;
    std::uint64_t last_seq_ = 0;
};

} // namespace core
} // namespace autolife


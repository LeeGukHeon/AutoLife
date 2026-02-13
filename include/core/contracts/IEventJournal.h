#pragma once

#include <cstdint>
#include <vector>

#include "core/model/PlaneTypes.h"

namespace autolife {
namespace core {

class IEventJournal {
public:
    virtual ~IEventJournal() = default;

    virtual bool append(const JournalEvent& event) = 0;
    virtual std::vector<JournalEvent> readFrom(std::uint64_t seq_inclusive) = 0;
    virtual std::uint64_t lastSeq() const = 0;
};

} // namespace core
} // namespace autolife


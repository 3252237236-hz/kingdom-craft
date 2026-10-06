#pragma once

#include <cstdint>
#include <string>

namespace skycraft
{
    // Save reloads can reuse both the level and native player ID. A load callback
    // revision and the dead/loading -> playable edge are independent signals.
    class HostSession
    {
    public:
        bool Observe(bool playable, const std::string& identity,
            std::uint32_t loadRevision, std::uint32_t world)
        {
            if (!playable) {
                playable_ = false;
                return false;
            }
            const bool changed = !playable_ || identity != identity_
                || loadRevision != loadRevision_ || world != world_;
            playable_ = true;
            identity_ = identity;
            loadRevision_ = loadRevision;
            world_ = world;
            return changed;
        }

    private:
        bool playable_ = false;
        std::string identity_;
        std::uint32_t loadRevision_ = 0;
        std::uint32_t world_ = 0;
    };
}

#pragma once
#include <cstdint>
#include <optional>

namespace mortar {
// Visibility is independent of the saved measurement and of application focus.
class OverlayVisibility {
public:
    void mapStateChanged(bool wasOpen, bool open, std::uint64_t now) {
        if (open) closedAt.reset();
        else if (wasOpen) closedAt = now;
    }
    void reset() { closedAt.reset(); }
    bool visible(bool mapOpen, std::uint64_t now) const {
        return mapOpen || (closedAt && now >= *closedAt && now - *closedAt < 3000);
    }
private:
    std::optional<std::uint64_t> closedAt;
};
}

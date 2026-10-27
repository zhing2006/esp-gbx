#include "gbx/jit/cache.h"

namespace gbx::jit {

BlockEntry CodeCache::find(BlockKey key) const {
    if (!index_) return nullptr;
    for (size_t at = slot_for(key); index_[at] != vacant; at = (at + 1) & index_mask_) {
        const auto slot = index_[at];
        if (slots_[slot].key == key) {
            recent_[slot] = 1;
            return reinterpret_cast<BlockEntry>(memory_.words + slots_[slot].offset);
        }
    }
    return nullptr;
}

}  // namespace gbx::jit

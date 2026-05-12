#include "chain.hh"

#include <cassert>

void NodeChainView::seed(const Block& g) {
    blocks_.clear();
    blocks_[g.hash] = g;
    tip_ = g.hash;
}

bool NodeChainView::add(const Block& b) {
    auto [it, inserted] = blocks_.emplace(b.hash, b);
    if (!inserted) return false;

    // First-seen tiebreak: only switch tips on strictly greater height.
    auto tip_it = blocks_.find(tip_);
    if (tip_it == blocks_.end() || b.height > tip_it->second.height) {
        tip_ = b.hash;
    }
    return true;
}

bool NodeChainView::knows(BlockHash h) const {
    return blocks_.find(h) != blocks_.end();
}

BlockHeight NodeChainView::tip_height() const {
    auto it = blocks_.find(tip_);
    return (it == blocks_.end()) ? 0 : it->second.height;
}

const Block& NodeChainView::get(BlockHash h) const {
    auto it = blocks_.find(h);
    assert(it != blocks_.end());
    return it->second;
}

std::vector<BlockHash> NodeChainView::walk_to_genesis() const {
    std::vector<BlockHash> path;
    BlockHash cur = tip_;
    while (cur != GENESIS_HASH) {
        path.push_back(cur);
        cur = blocks_.at(cur).parent;
    }
    path.push_back(GENESIS_HASH);
    return path;
}

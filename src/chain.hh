#pragma once

#include "types.hh"

#include <unordered_map>
#include <vector>

// A block in the chain. Hashes are simulator-assigned uint64 counters
// rather than real SHA-256 outputs (we don't model cryptographic hashing).
struct Block {
    BlockHash   hash;
    BlockHash   parent;
    BlockHeight height;
    NodeId      miner;
    Time        mined_at;
};

// One node's local view of the chain.
//
// In v0 the only metric for chain selection is block height (constant
// difficulty implies cumulative_work == height). Tiebreaks are first-seen:
// add() only updates the tip when the new block is *strictly* heavier.
class NodeChainView {
public:
    void seed(const Block& genesis);

    // Returns true if the block was new and added.
    // May update tip; first-seen tiebreak (strictly-greater height required).
    bool add(const Block& b);

    bool        knows(BlockHash h) const;
    BlockHash   tip() const { return tip_; }
    BlockHeight tip_height() const;
    std::size_t block_count() const { return blocks_.size(); }
    const Block& get(BlockHash h) const;

    // Walk from tip back to genesis, returning hashes tip-first.
    std::vector<BlockHash> walk_to_genesis() const;

private:
    std::unordered_map<BlockHash, Block> blocks_;
    BlockHash                            tip_ = GENESIS_HASH;
};

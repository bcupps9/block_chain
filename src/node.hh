#pragma once

#include "chain.hh"
#include "types.hh"

#include <vector>

// Mining behavior. Strategy is the only thing that varies between honest,
// colluding, and (later) attacker miners.
enum class MiningStrategy {
    None,            // non-mining full node
    Honest,
    // SelfishMining,   // (v1)
};

// Network-layer behavior for poison "signpost" nodes.
enum class PoisonStrategy {
    None,
    // Eclipse,         // (v1) selectively drop/substitute messages for victims
};

struct Node {
    NodeId          id                = 0;
    double          compute           = 0.0;   // 0.0 for non-mining
    MiningStrategy  mining_strategy   = MiningStrategy::None;
    PoisonStrategy  poison_strategy   = PoisonStrategy::None;

    // Two-channel peering. Public is the normal gossip graph; coalition is
    // the out-of-band channel between colluders (unused in v0).
    std::vector<NodeId> public_peers;
    std::vector<NodeId> coalition_peers;

    NodeChainView chain;

    // Records the parent the miner committed to when scheduling its current
    // BlockFound event. Used so that fire-time block assembly knows which tip
    // the work was done against (memorylessness lets us simply resample on
    // tip changes, but we still need the parent at schedule time).
    BlockHash mining_on     = GENESIS_HASH;
    EventId   pending_mining = 0;   // 0 == no outstanding mining event
};

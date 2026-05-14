#pragma once

#include "chain.hh"
#include "types.hh"

#include <unordered_set>
#include <vector>

// Mining behavior. Strategy is the only thing that varies between honest,
// colluding, and (later) attacker miners.
enum class MiningStrategy {
    None,             // non-mining full node
    Honest,
    SelfishMining,    // Eyal-Sirer 2014 (arXiv:1311.0243), Algorithm 1
};

// Network-layer behavior for poison "signpost" nodes.
enum class PoisonStrategy {
    None,
    // Eclipse,         // (v1) selectively drop/substitute messages for victims
};

// Explicit state machine for selfish mining, one state per decision point in
// Eyal-Sirer Algorithm 1. Tracked alongside private_branch_length so we can
// assert state consistency at every transition.
//
//                 selfish mines              honest mines
// Even        →   Ahead1                     Even (accept public block)
// Ahead1      →   Ahead2                     Race  (publish 1 private)
// Ahead2      →   AheadFar                   Even  (publish all, lock in)
// AheadFar    →   AheadFar                   AheadFar or Ahead2 (publish 1)
// Race        →   Even  (publish all, win)   Even  (race resolves)
enum class SelfishState {
    Even,        // private == public, branch_length == 0, lead == 0
    Ahead1,      // 1 private block ahead, branch_length == 1, lead == 1
    Ahead2,      // 2 private blocks ahead, branch_length == 2, lead == 2
    AheadFar,    // 3+ private blocks ahead, branch_length >= 3, lead >= 3
    Race,        // state 0' in paper: published 1 private at same height as honest
};

struct Node {
    NodeId          id                = 0;
    double          compute           = 0.0;   // 0.0 for non-mining
    MiningStrategy  mining_strategy   = MiningStrategy::None;
    PoisonStrategy  poison_strategy   = PoisonStrategy::None;

    // Two-channel peering. Public is the normal gossip graph; coalition is
    // the out-of-band channel between colluders (unused in v0/v1).
    std::vector<NodeId> public_peers;
    std::vector<NodeId> coalition_peers;

    NodeChainView chain;

    // Records the parent the miner committed to when scheduling its current
    // BlockFound event. Used so that fire-time block assembly knows which tip
    // the work was done against (memorylessness lets us simply resample on
    // tip changes, but we still need the parent at schedule time).
    BlockHash mining_on     = GENESIS_HASH;
    EventId   pending_mining = 0;   // 0 == no outstanding mining event

    // --- Selfish-mining state (used only when mining_strategy == SelfishMining) ---
    //
    // selfish_state         explicit decision-point state (see SelfishState).
    // private_branch_length Eyal-Sirer's fork-length-from-common-ancestor count;
    //                       incremented when selfish mines, reset on full publish
    //                       or public catch-up. Stays at 1 in Race state per the
    //                       paper (the fork persists even though the block was
    //                       broadcast).
    // public_height         highest block height the coalition believes the public
    //                       network has seen. The coalition's own published
    //                       blocks count as public.
    // unpublished_private   blocks the coalition has mined but not yet broadcast.
    //                       Used to find what to publish in each transition.
    SelfishState                  selfish_state          = SelfishState::Even;
    int                           private_branch_length  = 0;
    BlockHeight                   public_height          = 0;
    std::unordered_set<BlockHash> unpublished_private;
};

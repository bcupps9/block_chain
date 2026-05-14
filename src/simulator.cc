#include "simulator.hh"

#include <algorithm>
#include <cassert>
#include <iomanip>
#include <iostream>

Simulator::Simulator(std::uint64_t seed) : rng_(seed) {}

EventId Simulator::schedule(Time delay, std::function<void()> action) {
    EventId id = next_id_++;
    queue_.push(PQEntry{current_time_ + delay, id, std::move(action)});
    return id;
}

void Simulator::cancel(EventId id) {
    if (id != 0) cancelled_.insert(id);
}

void Simulator::run(Time horizon) {
    while (!queue_.empty() && queue_.top().t <= horizon) {
        // std::priority_queue exposes top() as const; copy the entry out and
        // pop. The action is std::function (cheap-ish to copy); revisit if
        // hot.
        PQEntry entry = queue_.top();
        queue_.pop();

        if (cancelled_.erase(entry.id)) continue;

        current_time_ = entry.t;
        entry.action();
    }
}

NodeId Simulator::add_node(Node n) {
    n.id = static_cast<NodeId>(nodes_.size());
    nodes_.push_back(std::move(n));
    return nodes_.back().id;
}

void Simulator::seed_genesis() {
    genesis_ = Block{GENESIS_HASH, GENESIS_HASH, 0, NO_MINER, 0.0};
    for (auto& n : nodes_) n.chain.seed(genesis_);
}

void Simulator::start_all_mining() {
    for (auto& n : nodes_) {
        if (n.mining_strategy != MiningStrategy::None && n.compute > 0.0) {
            start_mining_for(n.id);
        }
    }
}

void Simulator::start_mining_for(NodeId i) {
    auto& n = nodes_[i];

    // Cancel any outstanding attempt — memorylessness lets us resample without
    // tracking partial progress.
    if (n.pending_mining != 0) {
        cancel(n.pending_mining);
        n.pending_mining = 0;
    }

    BlockHash parent = n.chain.tip();
    n.mining_on = parent;

    std::exponential_distribution<double> dist(n.compute);
    Time tau = dist(rng_);

    n.pending_mining = schedule(tau, [this, i, parent]() {
        on_block_found(i, parent);
    });
}

void Simulator::on_block_found(NodeId miner, BlockHash parent) {
    auto& n = nodes_[miner];
    n.pending_mining = 0;

    // Block contents are assembled at fire time, not schedule time. v0 has no
    // mempool, so the block is just (parent, miner, timestamp).
    Block b;
    b.hash     = next_block_hash_++;
    b.parent   = parent;
    b.height   = n.chain.get(parent).height + 1;
    b.miner    = miner;
    b.mined_at = current_time_;

    n.chain.add(b);

    // Restart mining on the (possibly new) tip.
    start_mining_for(miner);

    // Dispatch on strategy: honest publishes immediately; selfish withholds
    // and decides via the state machine.
    switch (n.mining_strategy) {
        case MiningStrategy::Honest:
            gossip_block(miner, b);
            break;
        case MiningStrategy::SelfishMining:
            n.unpublished_private.insert(b.hash);
            handle_selfish_mined(miner, b);
            break;
        case MiningStrategy::None:
            // Shouldn't happen — non-miners have no mining timer.
            break;
    }
}

void Simulator::on_block_received(NodeId receiver, Block b, NodeId from) {
    auto& n = nodes_[receiver];

    // Already know it? Drop — this also terminates the gossip cascade.
    if (n.chain.knows(b.hash)) return;

    BlockHash tip_before = n.chain.tip();
    n.chain.add(b);

    // Selfish coalition reacts to public-arrived blocks via its state machine.
    if (n.mining_strategy == MiningStrategy::SelfishMining) {
        handle_selfish_received(receiver, b);
    }

    // If the tip moved (either because the received block extended it, or
    // because the selfish handler abandoned a private branch), restart mining.
    if (n.chain.tip() != tip_before && n.mining_strategy != MiningStrategy::None) {
        start_mining_for(receiver);
    }

    // Relay to all public peers except the sender.
    for (NodeId peer : n.public_peers) {
        if (peer == from) continue;
        schedule(public_latency_, [this, peer, b, receiver]() {
            on_block_received(peer, b, receiver);
        });
    }
}

void Simulator::gossip_block(NodeId from, const Block& b) {
    const auto& n = nodes_[from];
    for (NodeId peer : n.public_peers) {
        schedule(public_latency_, [this, peer, b, from]() {
            on_block_received(peer, b, from);
        });
    }
}

// Eyal-Sirer Algorithm 1: "on selfish pool mines a block" branch.
// Pre: b has been appended to n.chain and inserted into n.unpublished_private.
// We update private_branch_length and selfish_state, possibly publishing.
void Simulator::handle_selfish_mined(NodeId miner, const Block& b) {
    auto& n = nodes_[miner];

    n.private_branch_length += 1;
    const int lead = static_cast<int>(b.height) - static_cast<int>(n.public_height);

    switch (n.selfish_state) {
        case SelfishState::Even:
            assert(n.private_branch_length == 1 && lead == 1);
            n.selfish_state = SelfishState::Ahead1;
            break;

        case SelfishState::Ahead1:
            assert(n.private_branch_length == 2 && lead == 2);
            n.selfish_state = SelfishState::Ahead2;
            break;

        case SelfishState::Ahead2:
            assert(n.private_branch_length == 3 && lead == 3);
            n.selfish_state = SelfishState::AheadFar;
            break;

        case SelfishState::AheadFar:
            assert(n.private_branch_length >= 4 && lead >= 4);
            // Stay in AheadFar; keep mining privately.
            break;

        case SelfishState::Race: {
            // We were racing (private branch length 1 published, public tied).
            // Mining one more puts us at branch_length == 2 and lead == 1 — the
            // paper's "Δprev = 0 and private branch length = 2" trigger.
            // Publish everything to win the race.
            assert(n.private_branch_length == 2 && lead == 1);
            auto to_publish = unpublished_private_blocks(miner);
            publish_private_blocks(miner, to_publish);
            n.private_branch_length = 0;
            n.selfish_state         = SelfishState::Even;
            break;
        }
    }
}

// Eyal-Sirer Algorithm 1: "on others mine a block" branch.
// Pre: b (the honest block) has been appended to n.chain.
//
// The paper's algorithm assumes each "honest mines" event extends the public
// chain by exactly 1. In our simulator we observe every gossiped block,
// including (a) siblings at a height we've already seen and (b) stale blocks
// at lower heights. Neither advances the coalition's view of the public
// chain, so we silently absorb them — chain.add already recorded the block
// in the local view, and we just skip the state machine. Without this guard,
// the Ahead2 case's `assert(lead == 1)` fires whenever a second honest miner
// produces a sibling at a height we already counted.
void Simulator::handle_selfish_received(NodeId receiver, const Block& honest_block) {
    auto& n = nodes_[receiver];

    if (honest_block.height <= n.public_height) return;
    n.public_height = honest_block.height;

    const BlockHeight priv_h = n.chain.get(n.chain.tip()).height;
    const int lead = static_cast<int>(priv_h) - static_cast<int>(n.public_height);

    switch (n.selfish_state) {
        case SelfishState::Even:
            // No private fork; public chain extended — we accept it.
            // chain.tip() should have moved to honest_block (or beyond, if a
            // higher chain arrived). private_branch_length was already 0.
            assert(lead == 0);
            assert(n.private_branch_length == 0);
            assert(n.unpublished_private.empty());
            break;

        case SelfishState::Ahead1: {
            // Honest tied our 1-block lead. Publish our private block and race.
            // Branch length stays at 1 per the paper (the fork is still 1 block
            // long; the block is just public now).
            assert(lead == 0);
            assert(n.private_branch_length == 1);
            auto to_publish = unpublished_private_blocks(receiver);
            assert(to_publish.size() == 1);
            publish_private_blocks(receiver, to_publish);
            n.selfish_state = SelfishState::Race;
            break;
        }

        case SelfishState::Ahead2: {
            // Was 2 ahead; honest got 1; we're now 1 ahead. Publish all to
            // lock in the win — public chain becomes ours.
            assert(lead == 1);
            assert(n.private_branch_length == 2);
            auto to_publish = unpublished_private_blocks(receiver);
            publish_private_blocks(receiver, to_publish);
            n.private_branch_length = 0;
            n.selfish_state         = SelfishState::Even;
            break;
        }

        case SelfishState::AheadFar: {
            // Was 3+ ahead; honest got 1; still 2+ ahead. Publish the lowest
            // unpublished private block to keep the coalition's lead intact.
            assert(lead >= 2);
            assert(n.private_branch_length >= 3);
            auto to_publish = unpublished_private_blocks(receiver);
            assert(!to_publish.empty());
            publish_private_blocks(receiver, { to_publish.front() });
            n.private_branch_length -= 1;
            if (n.private_branch_length == 2) {
                n.selfish_state = SelfishState::Ahead2;
            }
            break;
        }

        case SelfishState::Race:
            // Race resolves on any new public block (paper's "Δprev = 0"
            // branch). chain.tip() may or may not have moved; either way,
            // branch_length goes to 0 and state to Even. Any leftover entries
            // in unpublished_private at this point are blocks that got
            // orphaned by losing the race; discard them.
            n.private_branch_length = 0;
            n.unpublished_private.clear();
            n.selfish_state         = SelfishState::Even;
            break;
    }
}

std::vector<BlockHash> Simulator::unpublished_private_blocks(NodeId selfish) const {
    const auto& n = nodes_[selfish];
    std::vector<BlockHash> blocks;
    BlockHash cur = n.chain.tip();
    while (cur != GENESIS_HASH && n.unpublished_private.count(cur) > 0) {
        blocks.push_back(cur);
        cur = n.chain.get(cur).parent;
    }
    std::reverse(blocks.begin(), blocks.end());   // genesis-first for in-order broadcast
    return blocks;
}

void Simulator::publish_private_blocks(NodeId selfish,
                                       const std::vector<BlockHash>& blocks) {
    auto& n = nodes_[selfish];
    for (BlockHash h : blocks) {
        const Block& b = n.chain.get(h);
        gossip_block(selfish, b);
        n.unpublished_private.erase(h);
        if (b.height > n.public_height) {
            n.public_height = b.height;
        }
    }
}

Simulator::Stats Simulator::stats() const {
    Stats s;
    s.blocks_per_miner.assign(nodes_.size(), 0);
    if (nodes_.empty()) return s;

    // Use a public/honest observer's view so withheld selfish blocks at the
    // horizon are not counted as canonical rewards.
    const Node* observer = &nodes_[0];
    for (const auto& n : nodes_) {
        if (n.mining_strategy != MiningStrategy::SelfishMining) {
            observer = &n;
            break;
        }
    }

    const auto& view = observer->chain;
    auto path = view.walk_to_genesis();

    for (BlockHash h : path) {
        if (h == GENESIS_HASH) continue;
        NodeId m = view.get(h).miner;
        if (m < nodes_.size()) s.blocks_per_miner[m]++;
    }

    s.canonical_blocks =
        path.empty() ? 0 : static_cast<std::uint64_t>(path.size() - 1);
    s.observer_known_blocks =
        view.block_count() == 0 ? 0 : static_cast<std::uint64_t>(view.block_count() - 1);
    s.stale_blocks = s.observer_known_blocks > s.canonical_blocks
        ? s.observer_known_blocks - s.canonical_blocks
        : 0;
    s.stale_rate = s.observer_known_blocks > 0
        ? static_cast<double>(s.stale_blocks) / static_cast<double>(s.observer_known_blocks)
        : 0.0;
    return s;
}

void Simulator::print_leaderboard() const {
    if (nodes_.empty()) return;

    const Stats s = stats();

    double total_compute = 0.0;
    for (const auto& n : nodes_) total_compute += n.compute;

    std::cout << "\n=== Leaderboard (canonical chain length "
              << s.canonical_blocks << ") ===\n";
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "node | compute |   blocks |  fair share |  score\n";
    std::cout << "-----+---------+----------+-------------+-------\n";
    for (const auto& n : nodes_) {
        double fair = (total_compute > 0.0)
            ? (n.compute / total_compute) * static_cast<double>(s.canonical_blocks)
            : 0.0;
        double score = (fair > 0.0)
            ? static_cast<double>(s.blocks_per_miner[n.id]) / fair
            : 0.0;
        std::cout << std::setw(4)  << n.id                     << " | "
                  << std::setw(7)  << n.compute                << " | "
                  << std::setw(8)  << s.blocks_per_miner[n.id] << " | "
                  << std::setw(11) << fair                     << " | "
                  << std::setw(5)  << score                    << "\n";
    }
    std::cout << std::flush;
}

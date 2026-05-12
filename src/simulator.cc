#include "simulator.hh"

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
        if (n.mining_strategy == MiningStrategy::Honest && n.compute > 0.0) {
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

    // Gossip to public peers.
    for (NodeId peer : n.public_peers) {
        schedule(public_latency_, [this, peer, b, miner]() {
            on_block_received(peer, b, miner);
        });
    }
}

void Simulator::on_block_received(NodeId receiver, Block b, NodeId from) {
    auto& n = nodes_[receiver];

    // Already know it? Drop — this also terminates the gossip cascade.
    if (n.chain.knows(b.hash)) return;

    n.chain.add(b);

    // If the tip moved to this block, restart mining on the new tip.
    if (n.chain.tip() == b.hash && n.mining_strategy != MiningStrategy::None) {
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

void Simulator::print_leaderboard() const {
    if (nodes_.empty()) return;

    // Use node 0's view as a proxy for the canonical chain. With honest play
    // and gossip latency << block interval, all honest nodes converge on the
    // same tip — verify this experimentally before trusting the proxy in
    // attack scenarios.
    const auto& view = nodes_[0].chain;
    auto path = view.walk_to_genesis();

    std::vector<std::uint64_t> blocks_per_miner(nodes_.size(), 0);
    for (BlockHash h : path) {
        if (h == GENESIS_HASH) continue;
        NodeId m = view.get(h).miner;
        if (m < nodes_.size()) blocks_per_miner[m]++;
    }

    double total_compute = 0.0;
    for (const auto& n : nodes_) total_compute += n.compute;

    const std::uint64_t total_blocks =
        path.empty() ? 0 : static_cast<std::uint64_t>(path.size() - 1);

    std::cout << "\n=== Leaderboard (canonical chain length "
              << total_blocks << ") ===\n";
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "node | compute |   blocks |  fair share |  score\n";
    std::cout << "-----+---------+----------+-------------+-------\n";
    for (const auto& n : nodes_) {
        double fair = (total_compute > 0.0)
            ? (n.compute / total_compute) * static_cast<double>(total_blocks)
            : 0.0;
        double score = (fair > 0.0)
            ? static_cast<double>(blocks_per_miner[n.id]) / fair
            : 0.0;
        std::cout << std::setw(4)  << n.id                     << " | "
                  << std::setw(7)  << n.compute                << " | "
                  << std::setw(8)  << blocks_per_miner[n.id]   << " | "
                  << std::setw(11) << fair                     << " | "
                  << std::setw(5)  << score                    << "\n";
    }
    std::cout << std::flush;
}

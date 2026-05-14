#pragma once

#include "chain.hh"
#include "node.hh"
#include "types.hh"

#include <functional>
#include <queue>
#include <random>
#include <unordered_set>
#include <vector>

// Discrete-event simulator. Virtual-time priority queue with lazy
// cancellation. No real-time waiting — runs as fast as the CPU consumes
// events.
class Simulator {
public:
    struct Stats {
        std::uint64_t              canonical_blocks = 0;
        std::vector<std::uint64_t> blocks_per_miner;
        std::uint64_t              observer_known_blocks = 0;
        std::uint64_t              stale_blocks = 0;
        double                     stale_rate = 0.0;
    };

    explicit Simulator(std::uint64_t seed = 1);

    // --- DES core ---
    Time     now() const { return current_time_; }
    EventId  schedule(Time delay, std::function<void()> action);
    void     cancel(EventId id);
    void     run(Time horizon);
    std::mt19937& rng() { return rng_; }

    // --- Node management ---
    NodeId      add_node(Node n);
    Node&       node(NodeId id)       { return nodes_[id]; }
    const Node& node(NodeId id) const { return nodes_[id]; }
    std::size_t num_nodes() const     { return nodes_.size(); }

    // --- Network ---
    Time public_latency() const     { return public_latency_; }
    void set_public_latency(Time l) { public_latency_ = l; }

    // --- Simulation lifecycle ---
    void seed_genesis();
    void start_all_mining();

    // --- Stats ---
    Stats stats() const;
    void print_leaderboard() const;

private:
    // Dispatch entry points invoked by scheduled events.
    void on_block_found(NodeId miner, BlockHash parent_at_schedule);
    void on_block_received(NodeId receiver, Block b, NodeId from);

    void start_mining_for(NodeId i);

    // Helpers for the strategy dispatch.
    void gossip_block(NodeId from, const Block& b);
    void handle_selfish_mined(NodeId miner, const Block& b);
    void handle_selfish_received(NodeId receiver, const Block& honest_block);

    // Walk chain.tip() backward collecting blocks in unpublished_private,
    // returned genesis-first so they can be broadcast in order.
    std::vector<BlockHash> unpublished_private_blocks(NodeId selfish) const;
    void publish_private_blocks(NodeId selfish, const std::vector<BlockHash>& blocks);

    struct PQEntry {
        Time                   t;
        EventId                id;
        std::function<void()>  action;
        bool operator>(const PQEntry& o) const {
            if (t != o.t) return t > o.t;
            return id > o.id;     // deterministic tiebreak for repeatable runs
        }
    };

    Time      current_time_ = 0.0;
    EventId   next_id_      = 1;
    std::priority_queue<PQEntry, std::vector<PQEntry>, std::greater<PQEntry>> queue_;
    std::unordered_set<EventId> cancelled_;

    std::vector<Node> nodes_;
    std::mt19937      rng_;

    Block     genesis_{};
    Time      public_latency_  = 1.0;
    BlockHash next_block_hash_ = 1;   // 0 reserved for genesis
};

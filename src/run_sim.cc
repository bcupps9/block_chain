#include "node.hh"
#include "simulator.hh"
#include "types.hh"

#include <iostream>

// v0 driver: N honest miners on a fully-connected peering graph.
//
// Block interval ≈ 1 / sum(compute); latency << interval so natural forks are
// rare. The leaderboard should show scores converging to 1.0 for every node
// as the horizon grows (variance ~ 1/sqrt(T)).
int main() {
    constexpr Time   HORIZON = 10000.0;
    constexpr Time   LATENCY = 0.01;       // gossip latency, in time units
    constexpr std::uint64_t SEED = 42;

    // Compute shares: total 1.0, so mean block interval is 1.0 time unit.
    const std::vector<double> computes = {0.10, 0.20, 0.30, 0.40};

    Simulator sim(SEED);
    sim.set_public_latency(LATENCY);

    for (double c : computes) {
        Node n;
        n.compute         = c;
        n.mining_strategy = MiningStrategy::Honest;
        sim.add_node(std::move(n));
    }

    // Fully-connected public peering. Replace with topology variants in v2.
    for (NodeId i = 0; i < sim.num_nodes(); ++i) {
        for (NodeId j = 0; j < sim.num_nodes(); ++j) {
            if (i != j) sim.node(i).public_peers.push_back(j);
        }
    }

    sim.seed_genesis();
    sim.start_all_mining();

    std::cout << "Running v0 honest baseline: " << sim.num_nodes()
              << " miners, horizon " << HORIZON
              << ", latency " << LATENCY << "\n";

    sim.run(HORIZON);
    sim.print_leaderboard();
    return 0;
}

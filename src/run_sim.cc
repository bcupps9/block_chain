#include "node.hh"
#include "simulator.hh"
#include "types.hh"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <queue>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr Time          HORIZON = 5000.0;
constexpr Time          LATENCY = 0.01;
constexpr int           V1_HONEST_MINERS = 3;
constexpr int           V2_HONEST_MINERS = 8;
constexpr int           REPLICATES = 20;
constexpr std::uint64_t SEED_BASE = 42;

enum class TopologyKind {
    Complete,
    Ring,
    RandomRegular,
    TwoCommunityBridge,
    HubSpoke,
};

struct TopologySpec {
    std::string  name;
    std::string  parameter;
    TopologyKind kind;
    int          value = 0;
};

struct RunResult {
    std::string   experiment;
    std::string   topology;
    std::string   parameter;
    double        alpha = 0.0;
    std::uint64_t seed = 0;
    std::uint64_t canonical_blocks = 0;
    std::uint64_t selfish_blocks = 0;
    double        selfish_revenue_share = 0.0;
    double        selfish_relative_revenue = 0.0;
    double        stale_rate = 0.0;
};

struct Summary {
    std::string topology;
    std::string parameter;
    double      alpha = 0.0;
    int         samples = 0;
    double      mean_revenue_share = 0.0;
    double      mean_relative_revenue = 0.0;
    double      relative_revenue_stderr = 0.0;
    double      mean_stale_rate = 0.0;
};

void add_undirected_edge(Simulator& sim, NodeId a, NodeId b) {
    if (a == b) return;
    auto& pa = sim.node(a).public_peers;
    auto& pb = sim.node(b).public_peers;
    if (std::find(pa.begin(), pa.end(), b) == pa.end()) pa.push_back(b);
    if (std::find(pb.begin(), pb.end(), a) == pb.end()) pb.push_back(a);
}

void clear_peers(Simulator& sim) {
    for (NodeId i = 0; i < sim.num_nodes(); ++i) {
        sim.node(i).public_peers.clear();
    }
}

bool is_connected(const Simulator& sim) {
    if (sim.num_nodes() == 0) return true;

    std::vector<bool> seen(sim.num_nodes(), false);
    std::queue<NodeId> q;
    seen[0] = true;
    q.push(0);

    while (!q.empty()) {
        NodeId cur = q.front();
        q.pop();
        for (NodeId next : sim.node(cur).public_peers) {
            if (!seen[next]) {
                seen[next] = true;
                q.push(next);
            }
        }
    }

    return std::all_of(seen.begin(), seen.end(), [](bool v) { return v; });
}

void apply_complete(Simulator& sim) {
    for (NodeId i = 0; i < sim.num_nodes(); ++i) {
        for (NodeId j = i + 1; j < sim.num_nodes(); ++j) {
            add_undirected_edge(sim, i, j);
        }
    }
}

void apply_ring(Simulator& sim) {
    const auto n = static_cast<NodeId>(sim.num_nodes());
    for (NodeId i = 0; i < n; ++i) {
        add_undirected_edge(sim, i, (i + 1) % n);
    }
}

void apply_circulant_regular(Simulator& sim, int degree) {
    const auto n = static_cast<NodeId>(sim.num_nodes());
    const int half = degree / 2;
    for (NodeId i = 0; i < n; ++i) {
        for (int d = 1; d <= half; ++d) {
            add_undirected_edge(sim, i, (i + static_cast<NodeId>(d)) % n);
        }
    }
}

void apply_random_regular(Simulator& sim, int degree, std::uint64_t seed) {
    const auto n = static_cast<int>(sim.num_nodes());
    if ((n * degree) % 2 != 0 || degree >= n) {
        apply_circulant_regular(sim, std::min(degree, n - 1));
        return;
    }

    std::mt19937 rng(seed);
    for (int attempt = 0; attempt < 200; ++attempt) {
        clear_peers(sim);

        std::vector<int> stubs;
        stubs.reserve(static_cast<std::size_t>(n * degree));
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < degree; ++j) stubs.push_back(i);
        }
        std::shuffle(stubs.begin(), stubs.end(), rng);

        bool ok = true;
        std::vector<std::vector<bool>> edge(
            static_cast<std::size_t>(n),
            std::vector<bool>(static_cast<std::size_t>(n), false));

        for (std::size_t i = 0; i < stubs.size(); i += 2) {
            int a = stubs[i];
            int b = stubs[i + 1];
            if (a == b || edge[a][b]) {
                ok = false;
                break;
            }
            edge[a][b] = true;
            edge[b][a] = true;
            add_undirected_edge(sim, static_cast<NodeId>(a), static_cast<NodeId>(b));
        }

        if (ok && is_connected(sim)) return;
    }

    clear_peers(sim);
    apply_circulant_regular(sim, degree);
}

void apply_two_community_bridge(Simulator& sim, int bridges) {
    const auto n = static_cast<NodeId>(sim.num_nodes());
    const NodeId split = n / 2;

    for (NodeId i = 0; i < split; ++i) {
        for (NodeId j = i + 1; j < split; ++j) add_undirected_edge(sim, i, j);
    }
    for (NodeId i = split; i < n; ++i) {
        for (NodeId j = i + 1; j < n; ++j) add_undirected_edge(sim, i, j);
    }
    for (int b = 0; b < bridges; ++b) {
        add_undirected_edge(
            sim,
            static_cast<NodeId>(b % static_cast<int>(split)),
            static_cast<NodeId>(split + (b % static_cast<int>(n - split))));
    }
}

void apply_hub_spoke(Simulator& sim) {
    // Attacker-favorable hub placement: node 0 is the selfish coalition.
    for (NodeId i = 1; i < sim.num_nodes(); ++i) {
        add_undirected_edge(sim, 0, i);
    }
}

void apply_topology(Simulator& sim, const TopologySpec& spec, std::uint64_t seed) {
    clear_peers(sim);
    switch (spec.kind) {
        case TopologyKind::Complete:
            apply_complete(sim);
            break;
        case TopologyKind::Ring:
            apply_ring(sim);
            break;
        case TopologyKind::RandomRegular:
            apply_random_regular(sim, spec.value, seed);
            break;
        case TopologyKind::TwoCommunityBridge:
            apply_two_community_bridge(sim, spec.value);
            break;
        case TopologyKind::HubSpoke:
            apply_hub_spoke(sim);
            break;
    }
}

Simulator make_sim(std::uint64_t seed, double alpha, int honest_miners) {
    Simulator sim(seed);
    sim.set_public_latency(LATENCY);

    Node selfish;
    selfish.compute = alpha;
    selfish.mining_strategy = MiningStrategy::SelfishMining;
    sim.add_node(std::move(selfish));

    for (int i = 0; i < honest_miners; ++i) {
        Node honest;
        honest.compute = (1.0 - alpha) / static_cast<double>(honest_miners);
        honest.mining_strategy = MiningStrategy::Honest;
        sim.add_node(std::move(honest));
    }

    return sim;
}

RunResult run_one(const std::string& experiment,
                  const TopologySpec& topology,
                  double alpha,
                  std::uint64_t seed,
                  int honest_miners) {
    Simulator sim = make_sim(seed, alpha, honest_miners);
    apply_topology(sim, topology, seed + 1000003);

    sim.seed_genesis();
    sim.start_all_mining();
    sim.run(HORIZON);

    const auto stats = sim.stats();
    const std::uint64_t selfish_blocks =
        stats.blocks_per_miner.empty() ? 0 : stats.blocks_per_miner[0];
    const double revenue_share = stats.canonical_blocks > 0
        ? static_cast<double>(selfish_blocks) / static_cast<double>(stats.canonical_blocks)
        : 0.0;

    return RunResult{
        experiment,
        topology.name,
        topology.parameter,
        alpha,
        seed,
        stats.canonical_blocks,
        selfish_blocks,
        revenue_share,
        alpha > 0.0 ? revenue_share / alpha : 0.0,
        stats.stale_rate,
    };
}

double mean_of(const std::vector<double>& xs) {
    if (xs.empty()) return 0.0;
    return std::accumulate(xs.begin(), xs.end(), 0.0) / static_cast<double>(xs.size());
}

double stderr_of(const std::vector<double>& xs) {
    if (xs.size() < 2) return 0.0;
    const double mean = mean_of(xs);
    double ss = 0.0;
    for (double x : xs) ss += (x - mean) * (x - mean);
    const double variance = ss / static_cast<double>(xs.size() - 1);
    return std::sqrt(variance / static_cast<double>(xs.size()));
}

Summary summarize(const std::vector<RunResult>& rows,
                  const std::string& topology,
                  const std::string& parameter,
                  double alpha) {
    std::vector<double> revenue_shares;
    std::vector<double> relative_revenues;
    std::vector<double> stale_rates;

    for (const auto& row : rows) {
        if (row.topology == topology && row.parameter == parameter && row.alpha == alpha) {
            revenue_shares.push_back(row.selfish_revenue_share);
            relative_revenues.push_back(row.selfish_relative_revenue);
            stale_rates.push_back(row.stale_rate);
        }
    }

    return Summary{
        topology,
        parameter,
        alpha,
        static_cast<int>(relative_revenues.size()),
        mean_of(revenue_shares),
        mean_of(relative_revenues),
        stderr_of(relative_revenues),
        mean_of(stale_rates),
    };
}

void write_raw_csv(const std::filesystem::path& path, const std::vector<RunResult>& rows) {
    std::ofstream out(path);
    out << "experiment,topology,parameter,alpha,seed,canonical_blocks,selfish_blocks,"
           "selfish_revenue_share,selfish_relative_revenue,stale_rate\n";
    out << std::fixed << std::setprecision(6);
    for (const auto& row : rows) {
        out << row.experiment << ','
            << row.topology << ','
            << row.parameter << ','
            << row.alpha << ','
            << row.seed << ','
            << row.canonical_blocks << ','
            << row.selfish_blocks << ','
            << row.selfish_revenue_share << ','
            << row.selfish_relative_revenue << ','
            << row.stale_rate << '\n';
    }
}

void write_summary_csv(const std::filesystem::path& path, const std::vector<Summary>& rows) {
    std::ofstream out(path);
    out << "topology,parameter,alpha,samples,mean_selfish_revenue_share,"
           "mean_relative_revenue,relative_revenue_stderr,mean_stale_rate\n";
    out << std::fixed << std::setprecision(6);
    for (const auto& row : rows) {
        out << row.topology << ','
            << row.parameter << ','
            << row.alpha << ','
            << row.samples << ','
            << row.mean_revenue_share << ','
            << row.mean_relative_revenue << ','
            << row.relative_revenue_stderr << ','
            << row.mean_stale_rate << '\n';
    }
}

double svg_x(double x, double x_min, double x_max, double left, double width) {
    return left + ((x - x_min) / (x_max - x_min)) * width;
}

double svg_y(double y, double y_min, double y_max, double top, double height) {
    return top + (1.0 - ((y - y_min) / (y_max - y_min))) * height;
}

void write_v1_svg(const std::filesystem::path& path, const std::vector<Summary>& rows) {
    const double w = 760.0;
    const double h = 440.0;
    const double left = 68.0;
    const double top = 28.0;
    const double plot_w = 640.0;
    const double plot_h = 330.0;
    const double x_min = rows.front().alpha;
    const double x_max = rows.back().alpha;
    const double y_min = 0.0;
    double y_max = 0.0;
    for (const auto& row : rows) {
        y_max = std::max({y_max, row.alpha, row.mean_revenue_share});
    }
    y_max = std::ceil((y_max + 0.05) * 10.0) / 10.0;

    std::ofstream out(path);
    out << std::fixed << std::setprecision(2);
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << w
        << "\" height=\"" << h << "\" viewBox=\"0 0 " << w << ' ' << h << "\">\n";
    out << "<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/>\n";
    out << "<text x=\"24\" y=\"24\" font-family=\"Arial\" font-size=\"18\" font-weight=\"700\">"
        << "v1 selfish-mining alpha sweep</text>\n";
    out << "<line x1=\"" << left << "\" y1=\"" << top + plot_h
        << "\" x2=\"" << left + plot_w << "\" y2=\"" << top + plot_h
        << "\" stroke=\"#222\"/>\n";
    out << "<line x1=\"" << left << "\" y1=\"" << top
        << "\" x2=\"" << left << "\" y2=\"" << top + plot_h
        << "\" stroke=\"#222\"/>\n";

    for (double y = 0.0; y <= y_max + 0.0001; y += 0.1) {
        const double py = svg_y(y, y_min, y_max, top, plot_h);
        out << "<line x1=\"" << left << "\" y1=\"" << py
            << "\" x2=\"" << left + plot_w << "\" y2=\"" << py
            << "\" stroke=\"#e5e5e5\"/>\n";
        out << "<text x=\"24\" y=\"" << py + 4
            << "\" font-family=\"Arial\" font-size=\"11\" fill=\"#555\">" << y << "</text>\n";
    }

    out << "<polyline fill=\"none\" stroke=\"#888\" stroke-width=\"2\" stroke-dasharray=\"6 5\" points=\"";
    for (const auto& row : rows) {
        out << svg_x(row.alpha, x_min, x_max, left, plot_w) << ','
            << svg_y(row.alpha, y_min, y_max, top, plot_h) << ' ';
    }
    out << "\"/>\n";

    out << "<polyline fill=\"none\" stroke=\"#0b5cad\" stroke-width=\"3\" points=\"";
    for (const auto& row : rows) {
        out << svg_x(row.alpha, x_min, x_max, left, plot_w) << ','
            << svg_y(row.mean_revenue_share, y_min, y_max, top, plot_h) << ' ';
    }
    out << "\"/>\n";

    for (const auto& row : rows) {
        const double px = svg_x(row.alpha, x_min, x_max, left, plot_w);
        const double py = svg_y(row.mean_revenue_share, y_min, y_max, top, plot_h);
        out << "<circle cx=\"" << px << "\" cy=\"" << py
            << "\" r=\"4\" fill=\"#0b5cad\"/>\n";
        out << "<text x=\"" << px - 10 << "\" y=\"" << top + plot_h + 24
            << "\" font-family=\"Arial\" font-size=\"11\" fill=\"#555\">"
            << row.alpha << "</text>\n";
    }

    out << "<text x=\"" << left + plot_w / 2 - 45 << "\" y=\""
        << h - 18 << "\" font-family=\"Arial\" font-size=\"13\">alpha</text>\n";
    out << "<text x=\"-250\" y=\"16\" transform=\"rotate(-90)\" font-family=\"Arial\" font-size=\"13\">"
        << "selfish revenue share</text>\n";
    out << "<rect x=\"500\" y=\"40\" width=\"14\" height=\"3\" fill=\"#0b5cad\"/>"
        << "<text x=\"522\" y=\"46\" font-family=\"Arial\" font-size=\"12\">observed mean</text>\n";
    out << "<line x1=\"500\" y1=\"64\" x2=\"514\" y2=\"64\" stroke=\"#888\" stroke-width=\"2\" stroke-dasharray=\"6 5\"/>"
        << "<text x=\"522\" y=\"68\" font-family=\"Arial\" font-size=\"12\">fair share</text>\n";
    out << "</svg>\n";
}

void write_v2_svg(const std::filesystem::path& path, const std::vector<Summary>& rows) {
    const double w = 840.0;
    const double h = 440.0;
    const double left = 70.0;
    const double top = 32.0;
    const double plot_w = 700.0;
    const double plot_h = 320.0;
    const double bar_w = plot_w / static_cast<double>(rows.size()) * 0.62;
    double y_max = 1.0;
    for (const auto& row : rows) y_max = std::max(y_max, row.mean_relative_revenue);
    y_max = std::ceil((y_max + 0.1) * 10.0) / 10.0;

    std::ofstream out(path);
    out << std::fixed << std::setprecision(2);
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << w
        << "\" height=\"" << h << "\" viewBox=\"0 0 " << w << ' ' << h << "\">\n";
    out << "<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/>\n";
    out << "<text x=\"24\" y=\"24\" font-family=\"Arial\" font-size=\"18\" font-weight=\"700\">"
        << "v2 topology comparison</text>\n";
    out << "<line x1=\"" << left << "\" y1=\"" << top + plot_h
        << "\" x2=\"" << left + plot_w << "\" y2=\"" << top + plot_h
        << "\" stroke=\"#222\"/>\n";
    out << "<line x1=\"" << left << "\" y1=\"" << top
        << "\" x2=\"" << left << "\" y2=\"" << top + plot_h
        << "\" stroke=\"#222\"/>\n";

    for (double y = 0.0; y <= y_max + 0.0001; y += 0.2) {
        const double py = svg_y(y, 0.0, y_max, top, plot_h);
        out << "<line x1=\"" << left << "\" y1=\"" << py
            << "\" x2=\"" << left + plot_w << "\" y2=\"" << py
            << "\" stroke=\"#e5e5e5\"/>\n";
        out << "<text x=\"28\" y=\"" << py + 4
            << "\" font-family=\"Arial\" font-size=\"11\" fill=\"#555\">" << y << "</text>\n";
    }

    const double fair_y = svg_y(1.0, 0.0, y_max, top, plot_h);
    out << "<line x1=\"" << left << "\" y1=\"" << fair_y
        << "\" x2=\"" << left + plot_w << "\" y2=\"" << fair_y
        << "\" stroke=\"#888\" stroke-width=\"2\" stroke-dasharray=\"6 5\"/>\n";

    for (std::size_t i = 0; i < rows.size(); ++i) {
        const double slot = plot_w / static_cast<double>(rows.size());
        const double x = left + slot * static_cast<double>(i) + (slot - bar_w) / 2.0;
        const double y = svg_y(rows[i].mean_relative_revenue, 0.0, y_max, top, plot_h);
        const double height = top + plot_h - y;
        out << "<rect x=\"" << x << "\" y=\"" << y << "\" width=\"" << bar_w
            << "\" height=\"" << height << "\" fill=\"#127c56\"/>\n";
        out << "<text x=\"" << x + bar_w / 2.0 - 16 << "\" y=\"" << y - 6
            << "\" font-family=\"Arial\" font-size=\"11\" fill=\"#333\">"
            << rows[i].mean_relative_revenue << "</text>\n";
        out << "<text x=\"" << x - 10 << "\" y=\"" << top + plot_h + 22
            << "\" transform=\"rotate(25 " << x - 10 << ' ' << top + plot_h + 22
            << ")\" font-family=\"Arial\" font-size=\"11\" fill=\"#555\">"
            << rows[i].topology << "</text>\n";
    }

    out << "<text x=\"-255\" y=\"16\" transform=\"rotate(-90)\" font-family=\"Arial\" font-size=\"13\">"
        << "selfish relative revenue</text>\n";
    out << "</svg>\n";
}

std::vector<RunResult> run_v1(const std::vector<double>& alphas,
                              const TopologySpec& complete) {
    std::vector<RunResult> rows;
    for (double alpha : alphas) {
        for (int i = 0; i < REPLICATES; ++i) {
            rows.push_back(run_one(
                "v1_alpha_sweep",
                complete,
                alpha,
                SEED_BASE + static_cast<std::uint64_t>(i),
                V1_HONEST_MINERS));
        }
    }
    return rows;
}

std::vector<RunResult> run_v2(const std::vector<TopologySpec>& topologies,
                              double alpha) {
    std::vector<RunResult> rows;
    for (const auto& topology : topologies) {
        for (int i = 0; i < REPLICATES; ++i) {
            rows.push_back(run_one(
                "v2_topology_sweep",
                topology,
                alpha,
                SEED_BASE + static_cast<std::uint64_t>(i),
                V2_HONEST_MINERS));
        }
    }
    return rows;
}

void print_summary_table(const std::string& title, const std::vector<Summary>& rows) {
    std::cout << "\n" << title << "\n";
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "topology                 param       alpha samples revenue relative stderr stale\n";
    for (const auto& row : rows) {
        std::cout << std::left << std::setw(24) << row.topology
                  << std::setw(12) << row.parameter
                  << std::right << std::setw(6) << row.alpha
                  << std::setw(8) << row.samples
                  << std::setw(8) << row.mean_revenue_share
                  << std::setw(9) << row.mean_relative_revenue
                  << std::setw(7) << row.relative_revenue_stderr
                  << std::setw(7) << row.mean_stale_rate
                  << "\n";
    }
}

} // namespace

int main() {
    std::filesystem::create_directories("results");

    const TopologySpec complete{ "complete", "", TopologyKind::Complete, 0 };
    const std::vector<double> alphas{ 0.05, 0.10, 0.15, 0.20, 0.25, 0.30, 0.35, 0.40, 0.45 };
    const std::vector<TopologySpec> topologies{
        complete,
        { "ring", "", TopologyKind::Ring, 0 },
        { "random_regular", "k=4", TopologyKind::RandomRegular, 4 },
        { "two_community_bridge", "bridges=2", TopologyKind::TwoCommunityBridge, 2 },
        { "hub_spoke", "selfish_hub", TopologyKind::HubSpoke, 1 },
    };

    std::cout << "Running v1 alpha sweep: " << alphas.size()
              << " alphas x " << REPLICATES << " seeds, horizon=" << HORIZON
              << ", latency=" << LATENCY << "\n";
    auto v1_rows = run_v1(alphas, complete);

    std::vector<Summary> v1_summary;
    for (double alpha : alphas) {
        v1_summary.push_back(summarize(v1_rows, complete.name, complete.parameter, alpha));
    }

    write_raw_csv("results/v1_alpha_sweep.csv", v1_rows);
    write_summary_csv("results/v1_alpha_summary.csv", v1_summary);
    write_v1_svg("results/v1_alpha_sweep.svg", v1_summary);
    print_summary_table("v1 alpha sweep summary", v1_summary);

    constexpr double V2_ALPHA = 0.35;
    std::cout << "\nRunning v2 topology sweep: " << topologies.size()
              << " topologies x " << REPLICATES << " seeds, alpha=" << V2_ALPHA
              << ", horizon=" << HORIZON << ", latency=" << LATENCY << "\n";
    auto v2_rows = run_v2(topologies, V2_ALPHA);

    std::vector<Summary> v2_summary;
    for (const auto& topology : topologies) {
        v2_summary.push_back(summarize(v2_rows, topology.name, topology.parameter, V2_ALPHA));
    }

    write_raw_csv("results/v2_topology_sweep.csv", v2_rows);
    write_summary_csv("results/v2_topology_summary.csv", v2_summary);
    write_v2_svg("results/v2_topology_relative_revenue.svg", v2_summary);
    print_summary_table("v2 topology sweep summary", v2_summary);

    const auto best = std::max_element(
        v2_summary.begin(),
        v2_summary.end(),
        [](const Summary& a, const Summary& b) {
            return a.mean_relative_revenue < b.mean_relative_revenue;
        });

    if (best != v2_summary.end()) {
        std::cout << "\nBest v2 attacker configuration: " << best->topology;
        if (!best->parameter.empty()) std::cout << " (" << best->parameter << ")";
        std::cout << " with mean relative revenue " << std::fixed << std::setprecision(3)
                  << best->mean_relative_revenue << "\n";
    }

    std::cout << "\nWrote results/*.csv and results/*.svg\n";
    return 0;
}

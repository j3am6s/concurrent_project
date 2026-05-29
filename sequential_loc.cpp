#include <chrono>
#include "simulation_common.hpp"

//sequential

pair<vector<int>, vector<double>> simulate_exposures_sequential(const vector<Agent>& agents, const Params& params, mt19937& rng, const LocationHours& loc_hours, const unordered_map<int, size_t>& id_to_index) {
    vector<double> daily_no_infection_probability(agents.size(), 1.0);
    for (const auto& [loc, occupants] : loc_hours) {
        vector<pair<int, int>> infectious_here;
        vector<pair<int, size_t>> healthy_here;
        for (const auto& [agent_id, hours] : occupants) {
            size_t idx = id_to_index.at(agent_id);
            const Agent& agent = agents[idx];
            if (agent.disease_state == INFECTIOUS) {
                infectious_here.emplace_back(agent_id, agent.days_infectious);
            } else if (agent.disease_state == HEALTHY) {
                healthy_here.emplace_back(agent_id, idx);
            }
        }
        if (infectious_here.empty() || healthy_here.empty()) {
            continue;
        }
        for (const auto& [healthy_id, healthy_idx] : healthy_here) {
            HourMask healthy_hours = occupants.at(healthy_id);
            for (const auto& [infectious_id, days_infectious] : infectious_here) {
                HourMask infectious_hours = occupants.at(infectious_id);
                int contact_hours = count_overlap_hours(healthy_hours, infectious_hours);
                if (contact_hours < params.min_contact_hours) {
                    continue;
                }
                double p_infect = compute_transmission_prob(days_infectious, contact_hours, params.p_transmission_base, params.infectivity_decay);
                p_infect = apply_vaccine_protection(p_infect, agents[healthy_idx], params);
                daily_no_infection_probability[healthy_idx]*=(1.0-p_infect);
            }
        }
    }

    vector<double> daily_infection_probability(agents.size(), 0.0);
    vector<int> newly_exposed;
    uniform_real_distribution<double> dist(0.0, 1.0);

    for (size_t i = 0; i < agents.size(); ++i) {
        double p = 1.0 - daily_no_infection_probability[i];
        daily_infection_probability[i] = p;
        if (p > 0.0 && dist(rng) < p) {
            newly_exposed.push_back(agents[i].id);
        }
    }
    return {newly_exposed, daily_infection_probability};
}

struct CliArgs {
    string agents_path = "agents.csv";
    int days = 30;
    string output_file = "sequential_summary.csv";
    unsigned int seed = 42;
};

CliArgs parse_args(int argc, char* argv[]) {
    CliArgs args;
    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        auto require_value = [&](const string& name) -> string {
            if (i + 1 >= argc) {
                throw runtime_error("Missing value for " + name);
            }
            return string(argv[++i]);
        };

        if (arg == "--agents") {
            args.agents_path = require_value(arg);
        } else if (arg == "--days") {
            args.days = stoi(require_value(arg));
        } else if (arg == "--output-dir") {
            args.output_file = require_value(arg);
        } else if (arg == "--seed") {
            args.seed = static_cast<unsigned int>(stoul(require_value(arg)));
        } else {
            throw runtime_error("Unknown argument: " + arg);
        }
    }
    return args;
}

// asked An for ways to implement this

int main(int argc, char* argv[]) {
    try {
        CliArgs cli = parse_args(argc, argv);
        auto simulation_start = chrono::high_resolution_clock::now();
        Params params;
        params.random_seed = cli.seed;
        mt19937 rng(params.random_seed);
        vector<Agent> agents = load_agents(cli.agents_path);
        initialise_existing_infected_agents(agents, params, rng);
        fs::path output_file(cli.output_file);
        LocationHours loc_hours = build_location_hours(agents);
        unordered_map<int, size_t> id_to_index;
        id_to_index.reserve(agents.size());
        for (size_t i = 0; i < agents.size(); ++i) {
            id_to_index[agents[i].id] = i;
        }
        int final_day = 0;
        fs::path output_dir = "daily_snapshots";
        fs::create_directories(output_dir);
        write_daily_snapshot_csv(agents, output_dir / "day_0.csv");
        for (int day = 1; day <= cli.days; ++day) {
            auto exposure_result = simulate_exposures_sequential(agents, params, rng, loc_hours, id_to_index);
            vector<int> newly_exposed = exposure_result.first;
            vector<double> daily_infection_probability = exposure_result.second;
            update_probability_stats(agents, daily_infection_probability);
            advance_disease(agents, newly_exposed, params, rng);
            write_daily_snapshot_csv(agents, output_dir / ("day_" + to_string(day) + ".csv"));
            final_day = day;
            int active_cases = 0;
            for (const Agent& a : agents) {
                if (a.disease_state == EXPOSED || a.disease_state == INFECTIOUS) {
                    active_cases += 1;
                }
            }
            if (active_cases == 0) {
                cout << "  No active cases remaining after day " << day;
                break;
            }
        }

        Summary summary = make_summary(agents, final_day);
        write_summary_csv(summary, params, output_file);

        auto simulation_end = chrono::high_resolution_clock::now();
        double elapsed = chrono::duration<double>(simulation_end - simulation_start).count();
        print_summary("sequential", output_file, summary, elapsed);

        return 0;

    } catch (const exception& e) {
        cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

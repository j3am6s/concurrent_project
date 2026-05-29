#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <queue>
#include <thread>
#include "simulation_common.hpp"

//concurrent

class LocationTaskQueue {
private:
    queue<size_t> elements;
    mutex lock;
public:
    void push(size_t element) {
        lock_guard<mutex> guard(lock);
        elements.push(element);
    }
    bool pop(size_t& element) {
        lock_guard<mutex> guard(lock);
        if (elements.empty()) {
            return false;
        }
        element = elements.front();
        elements.pop();
        return true;
    }
};

void atomic_multiply(atomic<double>& value, double factor) {
    double avant = value.load(memory_order_relaxed);
    double apres;
    do {
        apres = avant * factor;
    } while (!value.compare_exchange_weak(avant, apres, memory_order_relaxed));
}

void process_one_location(size_t loc_i, const vector<string>& location_names, const LocationHours& loc_hours, const vector<Agent>& agents, const unordered_map<int, size_t>& id_to_index, const Params& params, vector<atomic<double>>& shared_no_infection) {
    const string& loc = location_names[loc_i];
    const Occupants& occupants = loc_hours.at(loc);
    vector<pair<int, int>> infectious_here;
    vector<pair<int, size_t>> healthy_here;
    for (const auto& item : occupants) {
        int agent_id = item.first;
        size_t idx = id_to_index.at(agent_id);
        const Agent& agent = agents[idx];
        if (agent.disease_state == INFECTIOUS) {
            infectious_here.emplace_back(agent_id, agent.days_infectious);
        }
        else if (agent.disease_state == HEALTHY) {
            healthy_here.emplace_back(agent_id, idx);
        }
    }
    if (infectious_here.empty() || healthy_here.empty()) {
        return;
    }
    unordered_map<size_t, double> local_no_infection_factor;

    for (const auto& healthy_pair : healthy_here) {
        int healthy_id = healthy_pair.first;
        size_t healthy_idx = healthy_pair.second;
        HourMask healthy_hours = occupants.at(healthy_id);
        double factor = 1.0;
        for (const auto& infectious_pair : infectious_here) {
            int infectious_id = infectious_pair.first;
            int days_inf = infectious_pair.second;
            HourMask infectious_hours = occupants.at(infectious_id);
            int contact_hours = count_overlap_hours(healthy_hours, infectious_hours);
            if (contact_hours < params.min_contact_hours) {
                continue;
            }
            double p_infect = compute_transmission_prob(days_inf, contact_hours, params.p_transmission_base, params.infectivity_decay);
            p_infect = apply_vaccine_protection(p_infect, agents[healthy_idx], params);
            factor *= (1.0 - p_infect);
        }
        if (factor < 1.0) {
            local_no_infection_factor[healthy_idx] = factor;
        }
    }
    for (const auto& item : local_no_infection_factor) {
        size_t agent_idx = item.first;
        double factor = item.second;
        atomic_multiply(shared_no_infection[agent_idx], factor);
    }
}

void worker_loop(LocationTaskQueue& tasks, const vector<string>& location_names, const LocationHours& loc_hours, const vector<Agent>& agents, const unordered_map<int, size_t>& id_to_index, const Params& params, vector<atomic<double>>& shared_no_infection, atomic<size_t>& processed_locations) {    size_t loc_i;
    while (tasks.pop(loc_i)) {
        process_one_location(loc_i, location_names, loc_hours, agents, id_to_index, params, shared_no_infection);
        processed_locations.fetch_add(1);
    }
}

pair<vector<int>, vector<double>> simulate_exposures_concurrent_by_location(const vector<Agent>& agents, const Params& params, mt19937& rng, const LocationHours& loc_hours, const vector<string>& location_names, const unordered_map<int, size_t>& id_to_index) {
    unsigned int worker_count = params.workers;
    if (worker_count == 0) {
        worker_count = thread::hardware_concurrency();
        if (worker_count == 0) {
            worker_count = 4;
        }
    }
    if (!location_names.empty()) {
        worker_count = min<unsigned int>(
            worker_count,
            static_cast<unsigned int>(location_names.size())
        );
    }
    if (worker_count == 0) {
        worker_count = 1;
    }
    LocationTaskQueue tasks;
    for (size_t i = 0; i < location_names.size(); ++i) {
        tasks.push(i);
    }
    vector<atomic<double>> daily_no_infection(agents.size());
    for (size_t i = 0; i < agents.size(); ++i) {
        daily_no_infection[i].store(1.0, memory_order_relaxed);
    }
    atomic<size_t> processed_locations(0);
    vector<thread> workers;
    workers.reserve(worker_count);

    for (unsigned int w = 0; w < worker_count; ++w) {
        workers.emplace_back(worker_loop, ref(tasks), cref(location_names), cref(loc_hours), cref(agents), cref(id_to_index), cref(params), ref(daily_no_infection), ref(processed_locations));
    }

    for (thread& worker : workers) {
        worker.join();
    }

    vector<double> daily_infection_probability(agents.size(), 0.0);
    vector<int> newly_exposed;
    uniform_real_distribution<double> dist(0.0, 1.0);

    for (size_t i = 0; i < agents.size(); ++i) {
        double p = 1.0 - daily_no_infection[i].load(memory_order_relaxed);
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
    string output_file = "concurrent_summary.csv";
    unsigned int workers = 0;
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
        }
        else if (arg == "--days") {
            args.days = stoi(require_value(arg));
        }
        else if (arg == "--output-dir") {
            args.output_file = require_value(arg);
        }
        else if (arg == "--workers") {
            args.workers = static_cast<unsigned int>(stoul(require_value(arg)));
        }
        else if (arg == "--seed") {
            args.seed = static_cast<unsigned int>(stoul(require_value(arg)));
        }
        else {
            throw runtime_error("Unknown argument: " + arg);
        }
    }
    return args;
}

struct PrecomputedProgression {
    int incubation_days = -1;
    int prodromal_days = -1;
    int will_die = -1;
    int days_until_death = -1;
};

PrecomputedProgression make_precomputed_progression(const Params& params, mt19937& rng) {
    PrecomputedProgression progression;
    progression.incubation_days = draw_incubation_days(rng);
    progression.prodromal_days = draw_prodromal_days(rng);
    progression.will_die = draw_will_die(rng, params) ? 1 : 0;
    if (progression.will_die == 1) {
        progression.days_until_death = draw_death_delay_days(rng);
    } else {
        progression.days_until_death = -1;
    }
    return progression;
}

void advance_disease_chunk(vector<Agent>& agents, size_t start, size_t end, const unordered_map<int, PrecomputedProgression>& nouveau_exposed, const Params& params) {
    for (size_t i = start; i < end; ++i) {
        Agent& agent = agents[i];
        DiseaseState state = agent.disease_state;
        if (state == HEALTHY && nouveau_exposed.find(agent.id) != nouveau_exposed.end()) {
            const PrecomputedProgression& progression = nouveau_exposed.at(agent.id);
            agent.disease_state = EXPOSED;
            agent.days_exposed = 0;
            agent.days_infectious = 0;
            agent.incubation_days = progression.incubation_days;
            agent.prodromal_days = progression.prodromal_days;
            agent.will_die = progression.will_die;
            agent.days_until_death = progression.days_until_death;
            agent.ever_infected = true;
            agent.times_infected += 1;
            agent.cumulative_infection_probability = 1.0;
            agent.cumulative_no_infection_probability = 0.0;
        }
        else if (state == EXPOSED) {
            agent.days_exposed += 1;
            int threshold = agent.incubation_days + agent.prodromal_days;
            if (agent.days_exposed >= threshold) {
                agent.disease_state = INFECTIOUS;
                agent.days_infectious = 0;
            }
        }
        else if (state == INFECTIOUS) {
            agent.days_infectious += 1;
            if (agent.days_infectious >= params.infectious_period) {
                if (agent.will_die == 1) {
                    agent.disease_state = DEAD;
                } else {
                    agent.disease_state = HEALTHY;
                    agent.ever_recovered = true;
                    agent.times_recovered += 1;
                    reset_disease_progression(agent);
                }
            }
        }
    }
}

void advance_disease_parallel(vector<Agent>& agents, const vector<int>& newly_exposed, const Params& params, mt19937& rng, unsigned int worker_count) {
    unordered_map<int, PrecomputedProgression> nouveau_exposed;
    nouveau_exposed.reserve(newly_exposed.size());
    for (int id : newly_exposed) {
        nouveau_exposed[id] = make_precomputed_progression(params, rng);
    }
    if (worker_count == 0) {
        worker_count = thread::hardware_concurrency();
        if (worker_count == 0) {
            worker_count = 4;
        }
    }
    if (!agents.empty()) {
        worker_count = min<unsigned int>(worker_count, static_cast<unsigned int>(agents.size()));
    }
    if (worker_count == 0) {
        worker_count = 1;
    }
    vector<thread> workers;
    workers.reserve(worker_count);
    size_t chunk_size = (agents.size() + worker_count - 1) / worker_count;
    for (unsigned int w = 0; w < worker_count; ++w) {
        size_t start = w * chunk_size;
        size_t end = min(start + chunk_size, agents.size());
        if (start >= end) {
            continue;
        }
        workers.emplace_back(advance_disease_chunk, ref(agents), start, end, cref(nouveau_exposed), cref(params));
    }
    for (thread& worker : workers) {
        worker.join();
    }
}

//again talked with An about ways to implement this

int main(int argc, char* argv[]) {
    try {
        CliArgs cli = parse_args(argc, argv);
        auto simulation_start = chrono::high_resolution_clock::now();
        Params params;
        params.random_seed = cli.seed;
        params.workers = cli.workers;
        mt19937 rng(params.random_seed);
        vector<Agent> agents = load_agents(cli.agents_path);
        initialise_existing_infected_agents(agents, params, rng);
        fs::path output_file(cli.output_file);
        int final_day = 0;
        LocationHours loc_hours = build_location_hours(agents);

        vector<string> location_names;
        location_names.reserve(loc_hours.size());
        for (const auto& item : loc_hours) {
            location_names.push_back(item.first);
        }

        unordered_map<int, size_t> id_to_index;
        id_to_index.reserve(agents.size());
        for (size_t i = 0; i < agents.size(); ++i) {
            id_to_index[agents[i].id] = i;
        }
    
        // write_daily_snapshot_csv(agents, output_dir / "day_0.csv");

        for (int day = 1; day <= cli.days; ++day) {
            auto exposure_result = simulate_exposures_concurrent_by_location(agents, params, rng, loc_hours, location_names, id_to_index);
            vector<int> newly_exposed = exposure_result.first;
            vector<double> daily_infection_probability = exposure_result.second;
            update_probability_stats(agents, daily_infection_probability);
            advance_disease_parallel(agents, newly_exposed, params, rng, params.workers);
            // write_daily_snapshot_csv(agents, output_dir / ("day_" + to_string(day) + ".csv"));
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
        print_summary("concurrent", output_file, summary, elapsed);

        return 0;
    }
    catch (const exception& e) {
        cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std;
namespace fs = std::filesystem;

static const vector<string> HOURS = {
    "06:00", "07:00", "08:00", "09:00", "10:00", "11:00",
    "12:00", "13:00", "14:00", "15:00", "16:00", "17:00",
    "18:00", "19:00", "20:00", "21:00", "22:00", "23:00"
};

struct Params {
    double p_transmission_base = 0.632;
    double infectivity_decay = 0.35;
    int infectious_period = 4;
    int min_contact_hours = 1;
    double death_probability_if_infected = 0.30;
    double recovery_probability_if_infected = 0.70;
    unsigned int random_seed = 42;
};

struct Agent {
    int id = 0;
    string home;
    vector<string> schedule;

    string disease_state;
    bool vaccinated = false;

    int days_exposed = 0;
    int days_infectious = 0;

    int incubation_days = -1;
    int prodromal_days = -1;
    int will_die = -1;
    int days_until_death = -1;

    bool ever_infected = false;
    int times_infected = 0;
    bool ever_recovered = false;
    int times_recovered = 0;

    double last_day_infection_probability = 0.0;
    double max_daily_infection_probability = 0.0;
    double cumulative_infection_probability = 0.0;
    double cumulative_no_infection_probability = 1.0;
};

struct Summary {
    int final_day = 0;
    int total_agents = 0;
    int vaccinated_total = 0;
    int final_healthy = 0;
    int final_exposed = 0;
    int final_infectious = 0;
    int final_dead = 0;
    int total_ever_infected = 0;
    int total_ever_recovered = 0;
    int total_times_infected = 0;
    int total_times_recovered = 0;
};

// I got a lot of help from a BX24 (Tita) for the CSV formatting
// most CSV / formatting functions were written with her

vector<string> split_csv_simple(const string& line) {
    vector<string> result;
    string cell;
    stringstream ss(line);
    while (getline(ss, cell, ',')) {
        result.push_back(cell);
    }
    if (!line.empty() && line.back() == ',') {
        result.push_back("");
    }
    return result;
}

string csv_escape(const string& value) {
    bool needs_quotes = false;
    for (char c : value) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) {
        return value;
    }
    string escaped = "\"";
    for (char c : value) {
        if (c == '"') {
            escaped += "\"\"";
        } else {
            escaped += c;
        }
    }
    escaped += "\"";
    return escaped;
}

bool is_blank(const string& value) {
    return value.empty() || value == "nan" || value == "NaN";
}

bool parse_bool_default(const string& value, bool default_value) {
    if (is_blank(value)) {
        return default_value;
    }

    return value == "True" || value == "true" || value == "1";
}

int parse_int_default(const string& value, int default_value) {
    if (is_blank(value)) {
        return default_value;
    }
    return stoi(value);
}

double parse_double_default(const string& value, double default_value) {
    if (is_blank(value)) {
        return default_value;
    }
    return stod(value);
}

string int_or_blank(int value) {
    if (value < 0) {
        return "";
    }
    return to_string(value);
}

string double_to_string(double value) {
    ostringstream oss;
    oss << fixed << setprecision(6) << value;
    return oss.str();
}


int draw_incubation_days(mt19937& rng) {
    normal_distribution<double> dist(12.0, 2.0);
    int d = static_cast<int>(dist(rng));
    return max(7, min(17, d));
}

int draw_prodromal_days(mt19937& rng) {
    uniform_int_distribution<int> dist(3, 5);
    return dist(rng);
}

int draw_death_delay_days(mt19937& rng) {
    uniform_int_distribution<int> dist(10, 16);
    return dist(rng);
}

bool draw_will_die(mt19937& rng, const Params& params) {
    uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng) < params.death_probability_if_infected;
}

void assign_disease_progression(Agent& agent, const Params& params, mt19937& rng) {
    agent.incubation_days = draw_incubation_days(rng);
    agent.prodromal_days = draw_prodromal_days(rng);
    agent.will_die = draw_will_die(rng, params) ? 1 : 0;
    if (agent.will_die == 1) {
        agent.days_until_death = draw_death_delay_days(rng);
    } else {
        agent.days_until_death = -1;
    }
}

void reset_disease_progression(Agent& agent) {
    agent.incubation_days = -1;
    agent.prodromal_days = -1;
    agent.will_die = -1;
    agent.days_until_death = -1;
    agent.days_exposed = 0;
    agent.days_infectious = 0;
}

vector<Agent> load_agents(const string& path) {
    ifstream file(path);
    string header_line;
    getline(file, header_line);
    vector<string> headers = split_csv_simple(header_line);
    unordered_map<string, size_t> col;
    for (size_t i = 0; i < headers.size(); ++i) {
        col[headers[i]] = i;
    }
    auto get = [&](const vector<string>& row, const string& name) -> string {
        auto it = col.find(name);
        if (it == col.end() || it->second >= row.size()) {
            return "";
        }
        return row[it->second];
    };

    vector<Agent> agents;
    string line;
    while (getline(file, line)) {
        if (line.empty()) {
            continue;
        }
        vector<string> row = split_csv_simple(line);
        Agent agent;
        agent.id = stoi(get(row, "id"));
        agent.home = get(row, "home");
        agent.schedule.reserve(HOURS.size());
        for (const string& h : HOURS) {
            agent.schedule.push_back(get(row, "loc_" + h));
        }
        agent.disease_state = get(row, "disease_state");
        agent.vaccinated = parse_bool_default(get(row, "vaccinated"), false);
        agent.days_exposed = parse_int_default(get(row, "days_exposed"), 0);
        agent.days_infectious = parse_int_default(get(row, "days_infectious"), 0);
        agent.incubation_days = parse_int_default(get(row, "incubation_days"), -1);
        agent.prodromal_days = parse_int_default(get(row, "prodromal_days"), -1);
        string will_die_text = get(row, "will_die");
        if (is_blank(will_die_text)) {
            agent.will_die = -1;
        } else {
            agent.will_die = parse_bool_default(will_die_text, false) ? 1 : 0;
        }
        agent.days_until_death = parse_int_default(get(row, "days_until_death"), -1);

        bool ever_infected_default = agent.disease_state == "exposed" || agent.disease_state == "infectious";
        int times_infected_default = ever_infected_default ? 1 : 0;

        agent.ever_infected = parse_bool_default(get(row, "ever_infected"), ever_infected_default);

        agent.times_infected = parse_int_default(get(row, "times_infected"), times_infected_default);

        agent.ever_recovered = parse_bool_default(get(row, "ever_recovered"), false);

        agent.times_recovered = parse_int_default(get(row, "times_recovered"), 0);

        agent.last_day_infection_probability = parse_double_default(get(row, "last_day_infection_probability"), 0.0);

        agent.max_daily_infection_probability = parse_double_default(get(row, "max_daily_infection_probability"), 0.0);

        agent.cumulative_infection_probability = parse_double_default(get(row, "cumulative_infection_probability"), 0.0);

        agent.cumulative_no_infection_probability = 1.0 - agent.cumulative_infection_probability;

        if (agent.ever_infected) {
            agent.cumulative_infection_probability = 1.0;
            agent.cumulative_no_infection_probability = 0.0;
        }

        agents.push_back(agent);
    }

    return agents;
}

void initialise_existing_infected_agents(vector<Agent>& agents, const Params& params, mt19937& rng) {
    for (Agent& agent : agents) {
        if (agent.disease_state == "infectious") {
            assign_disease_progression(agent, params, rng);
            agent.ever_infected = true;
            if (agent.times_infected == 0) {
                agent.times_infected = 1;
            }
            agent.cumulative_infection_probability = 1.0;
            agent.cumulative_no_infection_probability = 0.0;
        }
    }
}

using Occupants = unordered_map<int, vector<int>>;
using LocationHours = unordered_map<string, Occupants>;

LocationHours build_location_hours(const vector<Agent>& agents) {
    LocationHours loc_hours;
    for (const Agent& agent : agents) {
        for (size_t hour_index = 0; hour_index < agent.schedule.size(); ++hour_index) {
            const string& loc = agent.schedule[hour_index];
            loc_hours[loc][agent.id].push_back(static_cast<int>(hour_index));
        }
    }
    return loc_hours;
}

double compute_transmission_prob(int days_infectious, int contact_hours, double p_base, double decay) {
    double p_eff = p_base * exp(-decay * days_infectious);
    return 1.0 - pow(1.0 - p_eff, contact_hours);
}

int count_overlap_hours(const vector<int>& a, const vector<int>& b) {
    int i = 0;
    int j = 0;
    int count = 0;
    while (i < static_cast<int>(a.size()) && j < static_cast<int>(b.size())) {
        if (a[i] == b[j]) {
            ++count;
            ++i;
            ++j;
        } else if (a[i] < b[j]) {
            ++i;
        } else {
            ++j;
        }
    }
    return count;
}

//sequential

pair<vector<int>, vector<double>> simulate_exposures_sequential(const vector<Agent>& agents, const Params& params, mt19937& rng, const LocationHours& loc_hours, const unordered_map<int, size_t>& id_to_index) {
    vector<double> daily_no_infection_probability(agents.size(), 1.0);
    for (const auto& [loc, occupants] : loc_hours) {
        vector<pair<int, int>> infectious_here;
        vector<pair<int, size_t>> healthy_here;
        for (const auto& [agent_id, hours] : occupants) {
            size_t idx = id_to_index.at(agent_id);
            const Agent& agent = agents[idx];
            if (agent.disease_state == "infectious") {
                infectious_here.emplace_back(agent_id, agent.days_infectious);
            } else if (agent.disease_state == "healthy" && !agent.vaccinated) {
                healthy_here.emplace_back(agent_id, idx);
            }
        }
        if (infectious_here.empty() || healthy_here.empty()) {
            continue;
        }
        for (const auto& [healthy_id, healthy_idx] : healthy_here) {
            const vector<int>& healthy_hours = occupants.at(healthy_id);
            for (const auto& [infectious_id, days_infectious] : infectious_here) {
                const vector<int>& infectious_hours = occupants.at(infectious_id);
                int contact_hours = count_overlap_hours(healthy_hours, infectious_hours);
                if (contact_hours < params.min_contact_hours) {
                    continue;
                }
                double p_infect = compute_transmission_prob(days_infectious, contact_hours, params.p_transmission_base, params.infectivity_decay);
                daily_no_infection_probability[healthy_idx] *= (1.0 - p_infect);
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

void update_probability_stats(vector<Agent>& agents, const vector<double>& daily_infection_probability) {
    for (size_t i = 0; i < agents.size(); ++i) {
        double probability = daily_infection_probability[i];
        agents[i].last_day_infection_probability = probability;
        if (probability > agents[i].max_daily_infection_probability) {
            agents[i].max_daily_infection_probability = probability;
        }
        agents[i].cumulative_no_infection_probability *= (1.0 - probability);
        agents[i].cumulative_infection_probability = 1.0 - agents[i].cumulative_no_infection_probability;
    }
}

void advance_disease(vector<Agent>& agents, const vector<int>& newly_exposed, const Params& params, mt19937& rng) {
    unordered_set<int> newly_exposed_set;
    newly_exposed_set.reserve(newly_exposed.size());
    for (int id : newly_exposed) {
        newly_exposed_set.insert(id);
    }
    for (Agent& agent : agents) {
        string state = agent.disease_state;
        if (state == "healthy" && newly_exposed_set.find(agent.id) != newly_exposed_set.end()) {
            agent.disease_state = "exposed";
            agent.days_exposed = 0;
            agent.days_infectious = 0;
            assign_disease_progression(agent, params, rng);
            agent.ever_infected = true;
            agent.times_infected += 1;
            agent.cumulative_infection_probability = 1.0;
            agent.cumulative_no_infection_probability = 0.0;

        } else if (state == "exposed") {
            agent.days_exposed += 1;
            int threshold = agent.incubation_days + agent.prodromal_days;
            if (agent.days_exposed >= threshold) {
                agent.disease_state = "infectious";
                agent.days_infectious = 0;
            }
        } else if (state == "infectious") {
            agent.days_infectious += 1;
            if (agent.days_infectious >= params.infectious_period) {
                if (agent.will_die == 1) {
                    agent.disease_state = "dead";
                } else {
                    agent.disease_state = "healthy";
                    agent.ever_recovered = true;
                    agent.times_recovered += 1;
                    reset_disease_progression(agent);
                }
            }
        }
    }
}

void write_daily_snapshot_csv(const vector<Agent>& agents, const fs::path& output_path) {
    ofstream out(output_path);
    out << "id,disease_state,days_exposed,days_infectious,incubation_days,"
        << "prodromal_days,will_die,days_until_death,times_infected,"
        << "times_recovered,last_day_infection_probability,"
        << "max_daily_infection_probability\n";
    for (const Agent& agent : agents) {
        out << agent.id << ','
            << csv_escape(agent.disease_state) << ','
            << agent.days_exposed << ','
            << agent.days_infectious << ','
            << int_or_blank(agent.incubation_days) << ','
            << int_or_blank(agent.prodromal_days) << ','
            << int_or_blank(agent.will_die) << ','
            << int_or_blank(agent.days_until_death) << ','
            << agent.times_infected << ','
            << agent.times_recovered << ','
            << double_to_string(agent.last_day_infection_probability) << ','
            << double_to_string(agent.max_daily_infection_probability)
            << '\n';
    }
}

Summary make_summary(const vector<Agent>& agents, int final_day) {
    Summary s;
    s.final_day = final_day;
    s.total_agents = static_cast<int>(agents.size());
    for (const Agent& a : agents) {
        if (a.vaccinated) {
            s.vaccinated_total += 1;
        }
        if (a.disease_state == "healthy") {
            s.final_healthy += 1;
        } else if (a.disease_state == "exposed") {
            s.final_exposed += 1;
        } else if (a.disease_state == "infectious") {
            s.final_infectious += 1;
        } else if (a.disease_state == "dead") {
            s.final_dead += 1;
        }
        if (a.ever_infected) {
            s.total_ever_infected += 1;
        }
        if (a.ever_recovered) {
            s.total_ever_recovered += 1;
        }
        s.total_times_infected += a.times_infected;
        s.total_times_recovered += a.times_recovered;
    }
    return s;
}

void write_summary_csv(const Summary& s, const Params& params, const fs::path& output_path) {
    ofstream out(output_path);
    out << "final_day,total_agents,vaccinated_total,final_healthy,"
        << "final_exposed,final_infectious,final_dead,"
        << "total_ever_infected,total_ever_recovered,"
        << "total_times_infected,total_times_recovered,"
        << "death_probability_if_infected,recovery_probability_if_infected\n";
    out << s.final_day << ','
        << s.total_agents << ','
        << s.vaccinated_total << ','
        << s.final_healthy << ','
        << s.final_exposed << ','
        << s.final_infectious << ','
        << s.final_dead << ','
        << s.total_ever_infected << ','
        << s.total_ever_recovered << ','
        << s.total_times_infected << ','
        << s.total_times_recovered << ','
        << params.death_probability_if_infected << ','
        << params.recovery_probability_if_infected
        << '\n';
}


struct CliArgs {
    string agents_path = "agents.csv";
    int days = 30;
    string output_dir = "sequential_results";
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
            args.output_dir = require_value(arg);
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
        fs::path output_dir(cli.output_dir);
        fs::remove_all(output_dir);
        fs::create_directories(output_dir);
        LocationHours loc_hours = build_location_hours(agents);

        unordered_map<int, size_t> id_to_index;
        id_to_index.reserve(agents.size());
        for (size_t i = 0; i < agents.size(); ++i) {
            id_to_index[agents[i].id] = i;
        }

        int final_day = 0;
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
                if (a.disease_state == "exposed" || a.disease_state == "infectious") {
                    active_cases += 1;
                }
            }
            if (active_cases == 0) {
                cout << "  No active cases remaining after day " << day;
                break;
            }
        }

        Summary summary = make_summary(agents, final_day);
        write_summary_csv(summary, params, output_dir / "total_summary.csv");

        auto simulation_end = chrono::high_resolution_clock::now();
        double elapsed = chrono::duration<double>(simulation_end - simulation_start).count();

        cout << "\nC++ sequential simulation complete.\n";
        cout << "Results written to: " << output_dir.string() << "\n\n";
        cout << "Total agents:          " << summary.total_agents << "\n";
        cout << "Final healthy:         " << summary.final_healthy << "\n";
        cout << "Final exposed:         " << summary.final_exposed << "\n";
        cout << "Final infectious:      " << summary.final_infectious << "\n";
        cout << "Final dead:            " << summary.final_dead << "\n";
        cout << "Ever infected:         " << summary.total_ever_infected << "\n";
        cout << "Ever recovered:        " << summary.total_ever_recovered << "\n";
        cout << "Total infection count: " << summary.total_times_infected << "\n";
        cout << "Total recovery count:  " << summary.total_times_recovered << "\n";
        cout << fixed << setprecision(3);
        cout << "Simulation time:       " << elapsed << "s\n";

        return 0;

    } catch (const exception& e) {
        cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

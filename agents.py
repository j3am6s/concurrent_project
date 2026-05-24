import argparse
import math
import random
import csv


def parse_args():
    parser = argparse.ArgumentParser(description="Generate agents.csv for the disease simulation")
    parser.add_argument("--n", type=int, default=100000, help="Number of agents")
    parser.add_argument("--vaccinated", type=float, default=0.4, help="Fraction of vaccinated agents")
    parser.add_argument("--initial-infected", type=int, default=5000, help="Number of initially infected agents")
    parser.add_argument("--seed", type=int, default=42, help="Random seed")
    parser.add_argument("--output", default="agents.csv", help="Output CSV path")
    return parser.parse_args()


def make_schedule(home, hours, locations):
    schedule = {}
    day_locations = random.sample(locations, k=random.randint(1, 3))
    n_day_locations = len(day_locations)

    for slot in hours:
        h = int(slot[:2])
        if h < 9 or h >= 18:
            schedule[slot] = home
        else:
            idx = (h - 9) * n_day_locations // 9
            schedule[slot] = day_locations[idx]

    return schedule


def main():
    args = parse_args()
    random.seed(args.seed)

    n = args.n
    v = args.vaccinated
    initial_infected = min(args.initial_infected, n)

    n_houses = math.ceil(0.3 * n)
    n_locations = math.ceil(0.2 * n)

    houses = [f"H{i}" for i in range(n_houses)]
    locations = [f"L{i}" for i in range(n_locations)]
    hours = [f"{h:02d}:00" for h in range(6, 24)]

    infected_ids = set(random.sample(range(n), initial_infected))

    agents = []
    for agent_id in range(n):
        home = random.choice(houses)
        schedule = make_schedule(home, hours, locations)
        vaccinated = random.random() < v

        if agent_id in infected_ids and not vaccinated:
            disease_state = "infectious"
            ever_infected = True
            times_infected = 1
        else:
            disease_state = "healthy"
            ever_infected = False
            times_infected = 0

        agents.append({
            "id": agent_id,
            "home": home,
            **{f"loc_{slot}": place for slot, place in schedule.items()},
            "disease_state": disease_state,
            "vaccinated": vaccinated,
            "days_exposed": 0,
            "days_infectious": 0,
            "incubation_days": "",
            "prodromal_days": "",
            "will_die": "",
            "days_until_death": "",
            "ever_infected": ever_infected,
            "times_infected": times_infected,
            "ever_recovered": False,
            "times_recovered": 0,
            "last_day_infection_probability": 0.0,
            "max_daily_infection_probability": 0.0,
            "cumulative_infection_probability": 1.0 if ever_infected else 0.0,
        })

    fieldnames = list(agents[0].keys())
    with open(args.output, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(agents)

    print(f"Generated {n} agents -> {args.output}")
    print(f"Houses:           {n_houses}")
    print(f"Locations:        {n_locations}")
    print(f"Vaccinated:       {sum(a['vaccinated'] for a in agents)}")
    print(f"Initial infected: {sum(1 for a in agents if a['disease_state'] == 'infectious')}")


if __name__ == "__main__":
    main()

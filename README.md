# concurrent_project

________

Simulation of a disease (smallpox) spread based on 

Eubank, H. Guclu, V. S. A. Kumar, M. V. Marathe, A. Srinivasan, Z. Toroczkai, and N. Wang. Modelling
disease outbreaks in realistic urban social networks. Nature, 429, 180–184, 2004

________

1. Generate agents.csv

python3 agents.py
OR
python3 agents.py --n 10000 --vaccinated 0.4 --initial-infected 500 --output agents.csv


2. Compile the C++ files
   
g++ -std=c++17 -O2 sequential_loc.cpp -o sequential_loc
g++ -std=c++17 -O2 -pthread concurrent_loc.cpp -o concurrent_loc


4. Run the sequential implementation

./sequential
OR
./sequential_loc --agents agents.csv --days 30 --output-dir sequential_results


5. Run the concurrent implementation

./concurrent_loc
OR
./concurrent_loc --agents agents.csv --days 30 --output-dir concurrent_results --workers 4

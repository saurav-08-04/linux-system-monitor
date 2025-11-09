Linux System Monitor
This is a simple, 'top'-like system monitor built for Linux. It runs in the terminal and provides real-time
information on system CPU, memory, and running processes.
This project was built in C++ and uses the ncurses library for the terminal UI and reads data directly
from the /proc filesystem.
How to Compile
You will need g++ (build-essential) and the ncurses development library ( libncurses-dev ).
g++ main.cpp -o monitor -lncurses
How to Run
./monitor
Controls
q : Quit the application.
c : Sort the process list by CPU usage (default).
m : Sort the process list by Memory usage.
p : Sort the process list by PID (Process ID).
k : Kill a process. (You will be prompted to enter a PID).

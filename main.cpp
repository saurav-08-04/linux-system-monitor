#include <ncurses.h>      // For terminal UI
#include <unistd.h>       // For usleep(), getpwuid()
#include <sys/types.h>    // For kill(), uid_t
#include <signal.h>       // For kill()
#include <dirent.h>       // For reading /proc
#include <pwd.h>          // For getpwuid()
#include <fstream>        // For reading files
#include <sstream>        // For string parsing
#include <string>         // For std::string
#include <vector>         // For std::vector
#include <map>            // For std::map (to store process times)
#include <algorithm>      // For std::sort
#include <iomanip>        // For std::setw, std::setprecision
#include <cmath>          // For std::round

// --- Data Structures ---

// Stores overall system CPU times from /proc/stat
struct SysCpuTimes {
    long long user;
    long long nice;
    long long system;
    long long idle;
    long long iowait;
    long long irq;
    long long softirq;
    long long steal;
    long long total; // Calculated total
};

// Stores all information for a single process
struct Process {
    int pid;
    std::string user;
    std::string name;
    double cpuPercent;
    double memPercent;
    long memRssKb;     // Memory in KB
    long long utime;   // CPU time (user)
    long long stime;   // CPU time (system)
};

// --- Global Variables ---
enum SortMode { BY_CPU, BY_MEM, BY_PID };
SortMode currentSortMode = BY_CPU;

// Maps to store previous CPU times for delta calculation
std::map<int, std::pair<long long, long long>> prevProcessTimes;
SysCpuTimes prevSysCpuTimes = {0};

// Map to cache Usernames (UID -> Username)
std::map<uid_t, std::string> usernameCache;

// --- Parsing Functions ---

/**
 * @brief Reads /etc/passwd and caches UID -> Username mappings
 */
void loadUsernames() {
    struct passwd *pw;
    // setpwent() opens the password database
    setpwent();
    while ((pw = getpwent()) != NULL) {
        usernameCache[pw->pw_uid] = pw->pw_name;
    }
    // endpwent() closes it
    endpwent();
}

/**
 * @brief Gets username for a PID, using the cache
 */
std::string getUsername(int pid) {
    std::string statusPath = "/proc/" + std::to_string(pid) + "/status";
    std::ifstream file(statusPath);
    std::string line;
    uid_t uid = 0;

    if (!file.is_open()) return "n/a";

    while (std::getline(file, line)) {
        if (line.rfind("Uid:", 0) == 0) {
            std::stringstream ss(line);
            std::string key;
            ss >> key >> uid;
            break;
        }
    }
    file.close();

    // Find in cache
    auto it = usernameCache.find(uid);
    if (it != usernameCache.end()) {
        return it->second;
    }
    return "unknown"; // Should be in cache, but fallback
}

/**
 * @brief Reads /proc/meminfo to get system memory
 * @return A pair of <Total Memory KB, Available Memory KB>
 */
std::pair<long, long> getMemoryInfo() {
    std::ifstream file("/proc/meminfo");
    std::string line;
    long memTotal = 0;
    long memAvailable = 0;

    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string key;
        long value;
        ss >> key >> value;
        if (key == "MemTotal:") {
            memTotal = value;
        } else if (key == "MemAvailable:") {
            memAvailable = value;
        }
        if (memTotal > 0 && memAvailable > 0) {
            break;
        }
    }
    file.close();
    return {memTotal, memAvailable};
}

/**
 * @brief Reads the first line of /proc/stat to get total CPU times
 */
SysCpuTimes getSystemCpuTimes() {
    std::ifstream file("/proc/stat");
    std::string line;
    SysCpuTimes t = {0};

    std::getline(file, line);
    if (line.rfind("cpu", 0) == 0) {
        std::stringstream ss(line);
        std::string cpu;
        ss >> cpu >> t.user >> t.nice >> t.system >> t.idle >> t.iowait >> t.irq >> t.softirq >> t.steal;
        t.total = t.user + t.nice + t.system + t.idle + t.iowait + t.irq + t.softirq + t.steal;
    }
    file.close();
    return t;
}

/**
 * @brief Gets all running processes by scanning /proc
 * @param totalSystemMemKb Total system memory for calculating %
 * @param totalCpuTimeDelta Total CPU time elapsed since last check
 * @return A vector of Process structs
 */
std::vector<Process> getProcesses(long totalSystemMemKb, long long totalCpuTimeDelta) {
    std::vector<Process> processes;
    DIR *dir;
    struct dirent *entry;

    if ((dir = opendir("/proc")) == NULL) {
        return processes; // Cannot open /proc
    }

    while ((entry = readdir(dir)) != NULL) {
        // Check if directory name is a number (PID)
        int pid = 0;
        try {
            pid = std::stoi(entry->d_name);
        } catch (...) {
            continue; // Not a PID
        }

        Process p = {0};
        p.pid = pid;

        // 1. Read /proc/[pid]/stat for CPU times
        std::ifstream statFile("/proc/" + std::to_string(pid) + "/stat");
        if (!statFile.is_open()) continue;

        std::string statLine;
        std::getline(statFile, statLine);
        statFile.close();

        std::stringstream ss(statLine);
        std::string value;
        // Skip values until we get to utime and stime
        // (1) pid (2) comm (3) state ... (14) utime (15) stime
        for (int i = 1; i < 14; ++i) {
            ss >> value;
        }
        ss >> p.utime >> p.stime;
        // (Further values for cstime, cutime are ignored for simplicity)

        // 2. Read /proc/[pid]/status for Name, Memory
        std::ifstream statusFile("/proc/" + std::to_string(pid) + "/status");
        if (!statusFile.is_open()) continue;
        
        std::string line;
        while (std::getline(statusFile, line)) {
            if (line.rfind("Name:", 0) == 0) {
                p.name = line.substr(6); // Get value after "Name: "
                p.name.erase(p.name.find_last_not_of(" \n\r\t")+1); // Trim whitespace
            } else if (line.rfind("VmRSS:", 0) == 0) {
                // VmRSS is the "Resident Set Size", physical memory
                p.memRssKb = std::stol(line.substr(7));
            }
        }
        statusFile.close();
        if (p.name.empty()) continue; // Process might have terminated

        // 3. Get Username
        p.user = getUsername(pid);

        // 4. Calculate CPU %
        long long currentProcessTotalTime = p.utime + p.stime;
        long long prevProcessTotalTime = 0;
        if (prevProcessTimes.count(pid)) {
            prevProcessTotalTime = prevProcessTimes[pid].first + prevProcessTimes[pid].second;
        }

        long long processTimeDelta = currentProcessTotalTime - prevProcessTotalTime;
        if (totalCpuTimeDelta > 0) {
            p.cpuPercent = 100.0 * (double)processTimeDelta / (double)totalCpuTimeDelta;
        } else {
            p.cpuPercent = 0.0;
        }
        
        // 5. Calculate Memory %
        if (totalSystemMemKb > 0) {
            p.memPercent = 100.0 * (double)p.memRssKb / (double)totalSystemMemKb;
        } else {
            p.memPercent = 0.0;
        }

        processes.push_back(p);
    }
    closedir(dir);
    return processes;
}

// --- Process Killing ---

/**
 * @brief Draws a new window to ask for a PID to kill
 */
void killProcessWindow() {
    int y, x;
    getmaxyx(stdscr, y, x); // Get screen dimensions
    // Create a small window in the center
    WINDOW *killWin = newwin(5, 40, y / 2 - 2, x / 2 - 20);
    box(killWin, 0, 0);
    wrefresh(killWin);

    mvwprintw(killWin, 1, 2, "Enter PID to kill (or Esc to_cancel):");
    wattron(killWin, A_REVERSE);
    mvwprintw(killWin, 2, 2, "                         "); // Input area
    wattroff(killWin, A_REVERSE);

    echo(); // Turn echo on to see typing
    keypad(killWin, TRUE); // Enable arrow keys, Esc
    
    char pidStr[20];
    int ch;
    
    wmove(killWin, 2, 2); // Move cursor to input area
    wrefresh(killWin);

    // Get string input, but handle Esc
    int i = 0;
    while(true) {
        ch = wgetch(killWin);
        if (ch == 27) { // 27 is the Esc key
            i = -1; // Signal for cancel
            break;
        }
        if (ch == '\n') { // Enter key
            pidStr[i] = '\0';
            break;
        }
        if (ch == KEY_BACKSPACE || ch == 127) {
            if (i > 0) {
                i--;
                mvwprintw(killWin, 2, 2 + i, " "); // Erase char on screen
                wmove(killWin, 2, 2 + i);
            }
        } else if (isdigit(ch) && i < 19) {
            pidStr[i] = (char)ch;
            waddch(killWin, ch);
            i++;
        }
    }
    
    noecho(); // Turn echo back off
    delwin(killWin); // Delete the window

    if (i == -1) return; // User cancelled

    try {
        int pid = std::stoi(pidStr);
        // Send SIGTERM (15) - a request to terminate
        if (kill(pid, SIGTERM) == 0) {
            // Success
        } else {
            // Failed (e.g., no permissions)
        }
    } catch (...) {
        // Invalid PID
    }
}


// --- Sorting Comparators ---
bool compareByCpu(const Process &a, const Process &b) {
    return a.cpuPercent > b.cpuPercent;
}
bool compareByMem(const Process &a, const Process &b) {
    return a.memPercent > b.memPercent;
}
bool compareByPid(const Process &a, const Process &b) {
    return a.pid < b.pid;
}


// --- Drawing Functions ---

/**
 * @brief Draws the main UI headers
 */
void drawHeader() {
    // Get screen size
    int y, x;
    getmaxyx(stdscr, y, x);

    // Enable color
    attron(COLOR_PAIR(1));
    // Draw top bar
    mvhline(0, 0, ' ', x);
    mvprintw(0, 1, "SysMon (Press 'q' to quit, 'c'/'m'/'p' to sort, 'k' to kill)");
    
    // Draw process list header
    mvhline(4, 0, ' ', x);
    mvprintw(4, 1, "%-6s %-10s %-6s %-6s %s", "PID", "USER", "CPU%", "MEM%", "COMMAND");
    attroff(COLOR_PAIR(1));
}

/**
 * @brief Draws the system summary (CPU, Mem)
 */
void drawSystemInfo(double cpuUsage, long memUsed, long memTotal) {
    // 1. CPU
    int barWidth = 20;
    int cpuBlocks = (int)std::round(cpuUsage / 100.0 * barWidth);
    std::string cpuBar = "";
    for(int i = 0; i < barWidth; ++i) {
        cpuBar += (i < cpuBlocks) ? "|" : " ";
    }
    mvprintw(2, 1, "CPU [%s] %5.1f%%", cpuBar.c_str(), cpuUsage);

    // 2. Memory
    double memPercent = 100.0 * (double)memUsed / (double)memTotal;
    int memBlocks = (int)std::round(memPercent / 100.0 * barWidth);
    std::string memBar = "";
    for(int i = 0; i < barWidth; ++i) {
        memBar += (i < memBlocks) ? "|" : " ";
    }
    mvprintw(3, 1, "Mem [%s] %5.1f%% (%ld/%ld KB)", memBar.c_str(), memPercent, memUsed, memTotal);
}

/**
 * @brief Draws the list of processes
 */
void drawProcessList(const std::vector<Process> &processes) {
    int y, x;
    getmaxyx(stdscr, y, x);
    
    // Max processes to show is screen height minus header lines
    int maxRows = y - 5; 

    for (int i = 0; i < processes.size() && i < maxRows; ++i) {
        const auto &p = processes[i];
        
        // Truncate command name if too long
        std::string name = p.name;
        int maxNameLen = x - 33; // PID(6) + User(10) + CPU(6) + MEM(6) + spaces(5)
        if (name.length() > maxNameLen) {
            name = name.substr(0, maxNameLen - 3) + "...";
        }

        // Format string
        char line[x + 1];
        snprintf(line, x, "%-6d %-10.10s %6.1f %6.1f %s", 
                 p.pid, 
                 p.user.c_str(), 
                 p.cpuPercent, 
                 p.memPercent, 
                 name.c_str());

        // Clear line and print
        mvhline(5 + i, 0, ' ', x);
        mvprintw(5 + i, 1, "%s", line);
    }
}


// --- Main Function ---

int main() {
    // 1. Initialize ncurses
    initscr();              // Start ncurses mode
    cbreak();               // Disable line buffering
    noecho();               // Don't echo user input
    keypad(stdscr, TRUE);   // Enable F-keys, arrows
    timeout(2000);          // Set refresh rate (2000ms = 2s)
    curs_set(0);            // Hide cursor

    // Initialize colors
    if (has_colors()) {
        start_color();
        // Pair 1: White text on Blue background (for headers)
        init_pair(1, COLOR_WHITE, COLOR_BLUE);
    }

    // 2. Initial Data Load
    loadUsernames(); // Load UID->Username map once
    prevSysCpuTimes = getSystemCpuTimes(); // Get first CPU snapshot
    
    // Get first snapshot of process times
    auto tempProcs = getProcesses(1, 1); // Dummy values first
    for(const auto& p : tempProcs) {
        prevProcessTimes[p.pid] = {p.utime, p.stime};
    }
    usleep(100000); // Wait 0.1 sec for a small delta
    

    // 3. Main Loop
    while (true) {
        // --- A. Handle Input ---
        int ch = getch(); // Get user input (non-blocking)
        if (ch == 'q') {
            break; // Quit
        }
        switch (ch) {
            case 'c': currentSortMode = BY_CPU; break;
            case 'm': currentSortMode = BY_MEM; break;
            case 'p': currentSortMode = BY_PID; break;
            case 'k': 
                killProcessWindow();
                // Redraw immediately after kill window closes
                clear(); 
                break;
        }

        // --- B. Gather Data ---
        // 1. System Memory
        auto memInfo = getMemoryInfo();
        long memTotal = memInfo.first;
        long memAvailable = memInfo.second;
        long memUsed = memTotal - memAvailable;

        // 2. System CPU
        SysCpuTimes currentSysCpuTimes = getSystemCpuTimes();
        long long totalDelta = currentSysCpuTimes.total - prevSysCpuTimes.total;
        long long idleDelta = currentSysCpuTimes.idle - prevSysCpuTimes.idle;
        double sysCpuUsage = (totalDelta > 0) ? 100.0 * (double)(totalDelta - idleDelta) / (double)totalDelta : 0.0;
        
        // 3. Processes
        std::vector<Process> processes = getProcesses(memTotal, totalDelta);

        // --- C. Process Data ---
        // 1. Sort
        if (currentSortMode == BY_CPU) {
            std::sort(processes.begin(), processes.end(), compareByCpu);
        } else if (currentSortMode == BY_MEM) {
            std::sort(processes.begin(), processes.end(), compareByMem);
        } else if (currentSortMode == BY_PID) {
            std::sort(processes.begin(), processes.end(), compareByPid);
        }

        // 2. Update previous times for next loop
        prevSysCpuTimes = currentSysCpuTimes;
        prevProcessTimes.clear();
        for (const auto &p : processes) {
            prevProcessTimes[p.pid] = {p.utime, p.stime};
        }
        
        // --- D. Draw UI ---
        clear(); // Clear screen
        drawHeader();
        drawSystemInfo(sysCpuUsage, memUsed, memTotal);
        drawProcessList(processes);
        refresh(); // Show all changes
    }

    // 4. Cleanup
    endwin(); // Exit ncurses mode
    return 0;
}
#include <bits/stdc++.h>
#include <filesystem>
#include <unistd.h>

using namespace std;

struct Proc {
    int pid;
    string name;
    long rss_kb;
    unsigned long long cpu_jiffies;
    double cpu; // computed percent
};

// ---- CPU totals from /proc/stat ----
static inline long long read_total_jiffies() {
    ifstream f("/proc/stat");
    string tag;
    long long user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
    if (!(f >> tag >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal >> guest >> guest_nice))
        return 0;
    return user + nice + system + idle + iowait + irq + softirq + steal + guest + guest_nice;
}

static inline long long read_idle_jiffies() {
    ifstream f("/proc/stat");
    string tag;
    long long user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
    if (!(f >> tag >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal >> guest >> guest_nice))
        return 0;
    return idle + iowait;
}

// ---- Per-process read from /proc/[pid]/stat and /status ----
static bool read_stat(int pid, string& name, unsigned long long& ut_st, long& rss_kb) {
    // Read /proc/[pid]/stat (one line)
    ifstream st("/proc/" + to_string(pid) + "/stat");
    if (!st) return false;

    string line;
    getline(st, line);
    if (line.empty()) return false;

    // Extract name between the last '(' and the last ')'
    size_t lp = line.find('(');
    size_t rp = line.rfind(')');
    if (lp == string::npos || rp == string::npos || rp <= lp) return false;
    name = line.substr(lp + 1, rp - lp - 1);

    // Tokenize after ") "
    string after = line.substr(rp + 2);
    vector<string> tok;
    tok.reserve(64);
    {
        istringstream ss(after);
        string w;
        while (ss >> w) tok.push_back(w);
    }
    // After ')', tokens start at field #3 (state). Need at least up to rss (field #24 -> index 21 here).
    if (tok.size() < 22) return false;

    // Field mapping relative to 'tok[0]' (which is field #3: state)
    // utime = field #14 -> idx 11, stime = field #15 -> idx 12, rss = field #24 -> idx 21
    unsigned long long ut = 0, stt = 0;
    long rss_pages = 0;
    try {
        ut = stoull(tok[11]);
        stt = stoull(tok[12]);
        rss_pages = stol(tok[21]);
    } catch (...) {
        return false;
    }

    ut_st = ut + stt;

    // Prefer VmRSS from /proc/[pid]/status (in kB)
    rss_kb = 0;
    ifstream sm("/proc/" + to_string(pid) + "/status");
    if (sm) {
        string k;
        while (sm >> k) {
            if (k == "VmRSS:") {
                long v;
                sm >> v;
                rss_kb = v; // already in kB
                break;
            }
        }
    }
    if (rss_kb == 0) {
        long page_kb = sysconf(_SC_PAGESIZE) / 1024;
        rss_kb = rss_pages * page_kb;
    }
    return true;
}

static vector<Proc> snapshot() {
    vector<Proc> v;
    for (auto& e : filesystem::directory_iterator("/proc")) {
        const string s = e.path().filename().string();
        if (s.empty() || !all_of(s.begin(), s.end(), ::isdigit)) continue;
        int pid = stoi(s);
        string name; unsigned long long utst; long rss;
        if (!read_stat(pid, name, utst, rss)) continue;
        v.push_back(Proc{pid, name, rss, utst, 0.0});
    }
    return v;
}

static void clear_screen() {
    // ANSI clear + home
    cout << "\033[H\033[J";
}

int main(int argc, char** argv) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    // Parse optional: -i <seconds>
    double interval_sec = 1.5;
    for (int i = 1; i + 1 < argc; ++i) {
        string a = argv[i];
        if ((a == "-i" || a == "--interval")) {
            interval_sec = max(0.05, atof(argv[i + 1]));
        }
    }

    const long ncpu = max(1L, sysconf(_SC_NPROCESSORS_ONLN));

    while (true) {
        // First snapshot
        auto v1 = snapshot();
        long long tot1 = read_total_jiffies();
        long long idle1 = read_idle_jiffies();

        // Sleep for interval
        useconds_t us = static_cast<useconds_t>(interval_sec * 1e6);
        usleep(us);

        // Second snapshot
        auto v2 = snapshot();
        long long tot2 = read_total_jiffies();
        long long idle2 = read_idle_jiffies();

        // Build maps for delta
        unordered_map<int, Proc> m1, m2;
        m1.reserve(v1.size()); m2.reserve(v2.size());
        for (auto& p : v1) m1.emplace(p.pid, p);
        for (auto& p : v2) m2.emplace(p.pid, p);

        long long totald = max(1LL, tot2 - tot1);
        long long idled  = max(0LL, idle2 - idle1);
        double cpu_total_active = 100.0 * (double)(totald - idled) / (double)totald; // 0..100 (all CPUs)

        vector<Proc> out;
        out.reserve(m2.size());
        for (const auto& kv : m2) {
            int pid = kv.first;
            const Proc& p2 = kv.second;

            auto it = m1.find(pid);
            if (it == m1.end()) continue; // skip brand-new processes (no baseline)

            unsigned long long dproc = 0;
            if (p2.cpu_jiffies >= it->second.cpu_jiffies)
                dproc = p2.cpu_jiffies - it->second.cpu_jiffies;

            // Scale to single-CPU 0..100% semantics
            double cpu = 100.0 * (double)ncpu * (double)dproc / (double)totald;

            out.push_back(Proc{p2.pid, p2.name, p2.rss_kb, p2.cpu_jiffies, cpu});
        }

        // Sort: RSS desc, then CPU desc
        sort(out.begin(), out.end(), [](const Proc& a, const Proc& b) {
            if (a.rss_kb != b.rss_kb) return a.rss_kb > b.rss_kb;
            return a.cpu > b.cpu;
        });

        clear_screen();
        cout << "Interval " << fixed << setprecision(2) << interval_sec
             << "s | CPUs: " << ncpu
             << " | Active CPU " << fixed << setprecision(2) << cpu_total_active << "%\n";

        cout << left << setw(8) << "PID"
             << setw(30) << "NAME"
             << setw(12) << "RSS(KB)"
             << setw(8)  << "CPU%" << "\n";

        size_t rows = min<size_t>(30, out.size());
        for (size_t i = 0; i < rows; ++i) {
            const auto& p = out[i];
            string nm = p.name.size() > 28 ? p.name.substr(0, 28) : p.name;
            cout << left << setw(8)  << p.pid
                 << setw(30) << nm
                 << setw(12) << p.rss_kb
                 << setw(8)  << fixed << setprecision(2) << p.cpu
                 << "\n";
        }

        cout.flush();
    }
    return 0;
}

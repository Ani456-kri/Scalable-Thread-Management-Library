// threadlab_web.cpp
// Scalable Thread Management Library + Multi-page Web Dashboard

#include <iostream>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <atomic>
#include <chrono>
#include <memory>
#include <map>
#include <iomanip>
#include <string>
#include <type_traits>
#include <sstream>

// ==== Windows version hack for httplib ====
#ifdef _WIN32
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0A00
#  else
#    undef _WIN32_WINNT
#    define _WIN32_WINNT 0x0A00
#  endif
#endif

#include "httplib.h"

using namespace std;

// ==============================
//  Module 1: Core Thread Runtime
// ==============================

using Task = function<void()>;

struct ThreadStats {
    size_t totalThreads = 0;
    size_t activeThreads = 0;
    size_t queuedTasks = 0;
    size_t completedTasks = 0;
    double avgTaskMs = 0.0;

    // "Logical" resource view (for UI graphs)
    double cpuPercent = 0.0;   // 0-100
    double memPercent = 0.0;   // 0-100
};

// Task for timeline visualisation
struct TaskTimelineEvent {
    size_t workerId;
    double startMs; // relative to poolStart
    double endMs;
};

// ---- Priority task wrapper ----
struct PrioritizedTask {
    int priority;      // smaller = higher priority
    Task task;
    uint64_t seq;      // to preserve FIFO inside same priority
};

struct TaskCompare {
    bool operator()(const PrioritizedTask& a, const PrioritizedTask& b) const {
        if (a.priority == b.priority) return a.seq > b.seq;
        return a.priority > b.priority;    // smaller priority value first
    }
};

class ThreadPool {
public:
    ThreadPool(size_t minThreads, size_t maxThreads)
        : stop_(false),
          completedTasks_(0),
          activeTasks_(0),
          totalExecNs_(0),
          minThreads_(minThreads),
          maxThreads_(maxThreads),
          stopScaler_(false),
          nextSeq_(0),
          poolStart_(chrono::steady_clock::now())
    {
        if (minThreads_ == 0) {
            minThreads_ = std::thread::hardware_concurrency();
            if (minThreads_ == 0) minThreads_ = 4;
        }
        if (maxThreads_ < minThreads_) {
            maxThreads_ = minThreads_;
        }

        for (size_t i = 0; i < minThreads_; ++i) {
            workers_.emplace_back([this, i] { workerLoop(i); });
        }
        scalerThread_ = std::thread([this] { scalerLoop(); });
    }

    ~ThreadPool() {
        shutdown();
    }

    template <class F, class... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        return submitWithPriority(1, std::forward<F>(f), std::forward<Args>(args)...);
    }

    template <class F, class... Args>
    auto submitWithPriority(int priority, F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using ResultT = std::invoke_result_t<F, Args...>;
        auto taskPtr = std::make_shared<std::packaged_task<ResultT()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<ResultT> fut = taskPtr->get_future();

        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (stop_) {
                throw std::runtime_error("ThreadPool is stopping, cannot submit new tasks");
            }
            if (priority < 0) priority = 0;
            if (priority > 2) priority = 2;
            tasks_.push(PrioritizedTask{
                priority,
                [taskPtr]() { (*taskPtr)(); },
                nextSeq_++
            });
        }
        cv_.notify_one();
        return fut;
    }

    // Cancel all pending tasks (do not stop active ones)
    size_t cancelPending() {
        std::lock_guard<std::mutex> lock(queueMutex_);
        size_t removed = tasks_.size();
        tasks_ = decltype(tasks_){}; // clear priority_queue
        return removed;
    }

    void shutdown() {
        {
            std::lock_guard<mutex> lock(queueMutex_);
            if (stop_) return;
            stop_ = true;
        }
        stopScaler_ = true;
        cv_.notify_all();

        if (scalerThread_.joinable()) scalerThread_.join();
        for (auto &t : workers_) {
            if (t.joinable()) t.join();
        }
        workers_.clear();
    }

    ThreadStats getStats() const {
        ThreadStats s;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            s.totalThreads = workers_.size();
            s.queuedTasks = tasks_.size();
        }
        s.activeThreads = activeTasks_.load();
        s.completedTasks = completedTasks_.load();

        uint64_t completed = completedTasks_.load();
        if (completed > 0) {
            std::lock_guard<std::mutex> lock(statsMutex_);
            double avgNs = static_cast<double>(totalExecNs_) / static_cast<double>(completed);
            s.avgTaskMs = avgNs / 1'000'000.0;
        }

        // logical utilisation (for display)
        if (s.totalThreads > 0) {
            s.cpuPercent = std::min(100.0, 100.0 * double(s.activeThreads) / double(s.totalThreads));
        } else {
            s.cpuPercent = 0.0;
        }
        // fake "memory" ~ queued + completed (synthetic metric)
        double load = double(s.queuedTasks) * 0.02 + double(s.completedTasks) * 0.0005;
        s.memPercent = std::max(5.0, std::min(95.0, load));

        return s;
    }

    vector<TaskTimelineEvent> getTimelineSnapshot() const {
        std::lock_guard<std::mutex> lock(timelineMutex_);
        return timeline_;
    }

private:
    void workerLoop(size_t workerId) {
        while (true) {
            PrioritizedTask item;
            {
                unique_lock<mutex> lock(queueMutex_);
                cv_.wait(lock, [this] {
                    return stop_ || !tasks_.empty();
                });

                if (stop_ && tasks_.empty()) {
                    return;
                }
                item = std::move(tasks_.top());
                tasks_.pop();
                activeTasks_++;
            }

            auto start = chrono::steady_clock::now();
            item.task();
            auto end = chrono::steady_clock::now();

            auto durationNs = chrono::duration_cast<chrono::nanoseconds>(end - start).count();
            {
                lock_guard<mutex> lock(statsMutex_);
                totalExecNs_ += durationNs;
            }
            // timeline event
            {
                lock_guard<mutex> lock(timelineMutex_);
                double startMs = chrono::duration_cast<chrono::milliseconds>(start - poolStart_).count();
                double endMs = chrono::duration_cast<chrono::milliseconds>(end - poolStart_).count();
                timeline_.push_back(TaskTimelineEvent{workerId, startMs, endMs});
                if (timeline_.size() > 5000) {
                    timeline_.erase(timeline_.begin(), timeline_.begin() + 1000);
                }
            }

            completedTasks_++;
            activeTasks_--;
        }
    }

    void scalerLoop() {
        using namespace std::chrono_literals;
        while (!stopScaler_) {
            std::this_thread::sleep_for(500ms);
            if (stop_) break;

            ThreadStats s = getStats();
            size_t currentThreads;
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                currentThreads = workers_.size();
            }

            if (currentThreads >= maxThreads_) continue;

            if (s.queuedTasks > currentThreads * 4) {
                size_t toAdd = std::min(maxThreads_ - currentThreads, (size_t)2);
                std::lock_guard<std::mutex> lock(queueMutex_);
                for (size_t i = 0; i < toAdd; ++i) {
                    size_t id = workers_.size();
                    workers_.emplace_back([this, id] { workerLoop(id); });
                }
            }
        }
    }

    vector<thread> workers_;

    mutable mutex queueMutex_;
    condition_variable cv_;
    priority_queue<PrioritizedTask, vector<PrioritizedTask>, TaskCompare> tasks_;

    atomic<bool> stop_;

    atomic<size_t> completedTasks_;
    atomic<size_t> activeTasks_;
    mutable mutex statsMutex_;
    uint64_t totalExecNs_;

    size_t minThreads_;
    size_t maxThreads_;
    thread scalerThread_;
    atomic<bool> stopScaler_;
    uint64_t nextSeq_;

    chrono::steady_clock::time_point poolStart_;
    mutable mutex timelineMutex_;
    vector<TaskTimelineEvent> timeline_;
};

// ================================================
//  Module 2: Concurrency API & Demo Tasks
// ================================================

class Mutex {
public:
    void lock()   { mtx_.lock(); }
    void unlock() { mtx_.unlock(); }
private:
    mutex mtx_;
};

class LockGuard {
public:
    explicit LockGuard(Mutex& m) : m_(m) { m_.lock(); }
    ~LockGuard() { m_.unlock(); }
private:
    Mutex& m_;
};

template<typename Index, typename Func>
void parallel_for(Index begin, Index end, Func f, ThreadPool& pool, Index chunkSize = 1000) {
    if (end <= begin) return;
    if (chunkSize <= 0) chunkSize = 1;

    vector<future<void>> futures;
    for (Index start = begin; start < end; start += chunkSize) {
        Index stop = min(start + chunkSize, end);
        futures.push_back(pool.submit([start, stop, &f]() {
            for (Index i = start; i < stop; ++i) f(i);
        }));
    }
    for (auto &fut : futures) fut.get();
}

// ---- Demo task types ----
void task_heavy_cpu(int) {
    volatile long long x = 0;
    for (int i = 0; i < 3'000'000; ++i) x += i;
}

long long fib(int n) {
    if (n <= 1) return n;
    return fib(n-1) + fib(n-2);
}
void task_fib(int n) {
    fib(28 + (n % 4)); // heavy but bounded
}

void task_sort(int n) {
    vector<int> v(8000);
    for (int i = 0; i < (int)v.size(); ++i) v[i] = (i * 73129 + n * 17) & 0xffff;
    sort(v.begin(), v.end());
}

void task_matrix(int) {
    const int N = 35;
    static thread_local int A[N][N], B[N][N], C[N][N];
    for (int i=0;i<N;i++)
        for (int j=0;j<N;j++) {
            A[i][j]=(i+j)%7; B[i][j]=(i*j)%5; C[i][j]=0;
        }
    for (int i=0;i<N;i++)
        for (int k=0;k<N;k++)
            for (int j=0;j<N;j++)
                C[i][j]+=A[i][k]*B[k][j];
}

bool isPrime(long long x) {
    if (x < 2) return false;
    for (long long i = 2; i*i <= x; ++i)
        if (x % i == 0) return false;
    return true;
}
void task_prime(int n) {
    int found = 0;
    long long x = 100000 + n * 100;
    while (found < 200) {
        if (isPrime(x)) ++found;
        ++x;
    }
}

// I/O-like dummy: just sleep a bit
void task_io_dummy(int n) {
    this_thread::sleep_for(chrono::milliseconds(50 + (n % 50)));
}

// ================================================
//  Module 3: Monitoring & Manager
// ================================================

struct ManagedPool {
    shared_ptr<ThreadPool> pool;
    string name;
};

class ThreadPoolManager {
public:
    int createPool(size_t threads, const string& name) {
        lock_guard<mutex> lock(mtx_);
        int id = ++lastId_;
        size_t minT = threads;
        size_t maxT = max(threads, threads * 4);

        auto tp = make_shared<ThreadPool>(minT, maxT);
        pools_[id] = ManagedPool{tp, name.empty() ? ("Pool " + to_string(id)) : name};
        return id;
    }

    shared_ptr<ThreadPool> getPool(int id) {
        lock_guard<mutex> lock(mtx_);
        auto it = pools_.find(id);
        if (it != pools_.end()) return it->second.pool;
        return nullptr;
    }

    bool renamePool(int id, const string& newName) {
        lock_guard<mutex> lock(mtx_);
        auto it = pools_.find(id);
        if (it == pools_.end()) return false;
        it->second.name = newName;
        return true;
    }

    map<int, ManagedPool> getAll() {
        lock_guard<mutex> lock(mtx_);
        return pools_;
    }

    map<int, ThreadStats> getAllStats() {
        map<int, ThreadStats> result;
        auto copy = getAll();
        for (auto &p : copy) {
            result[p.first] = p.second.pool->getStats();
        }
        return result;
    }

    void shutdownPool(int id) {
        shared_ptr<ThreadPool> tp;
        {
            lock_guard<mutex> lock(mtx_);
            auto it = pools_.find(id);
            if (it != pools_.end()) {
                tp = it->second.pool;
                pools_.erase(it);
            }
        }
        if (tp) tp->shutdown();
    }

    size_t cancelPending(int id) {
        auto p = getPool(id);
        if (!p) return 0;
        return p->cancelPending();
    }

    void shutdownAll() {
        map<int, ManagedPool> local;
        {
            lock_guard<mutex> lock(mtx_);
            local = pools_;
            pools_.clear();
        }
        for (auto &p : local) p.second.pool->shutdown();
    }

private:
    mutex mtx_;
    int lastId_ = 0;
    map<int, ManagedPool> pools_;
};

// ===============================================
//   HTML Dashboard (Frontend)
// ===============================================

string build_dashboard_html() {
    return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>ThreadLab – C++ Thread Pool Monitoring Dashboard</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
   <style>
    :root {
      --bg: #f3f6fc;
      --navbar: #ffffff;
      --card: #ffffff;
      --card-alt: #edf2ff;
      --border: #d7e0f0;
      --accent: #2563eb;
      --accent-soft: #e1ecff;
      --accent-strong: #1d4ed8;
      --danger: #ef4444;
      --text: #0f172a;
      --text-soft: #6b7280;
      --shadow-soft: 0 8px 20px rgba(15, 23, 42, 0.10);
      --shadow-strong: 0 14px 30px rgba(15, 23, 42, 0.20);
      --radius: 16px;
    }

    [data-theme="dark"] {
      --bg: #020617;
      --navbar: #020617;
      --card: #020617;
      --card-alt: #020617;
      --border: #1f2937;
      --accent: #3b82f6;
      --accent-soft: #0b1120;
      --accent-strong: #60a5fa;
      --danger: #f97373;
      --text: #e5e7eb;
      --text-soft: #9ca3af;
      --shadow-soft: 0 10px 28px rgba(0, 0, 0, 0.55);
      --shadow-strong: 0 18px 40px rgba(0, 0, 0, 0.70);
    }

    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
      font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    }

    html, body {
      height: 100%;
    }

    body {
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      background: radial-gradient(circle at top, #e6edff 0, #f3f6fc 40%, #eef2fb 100%);
      color: var(--text);
      transition: background 0.25s ease, color 0.25s ease;
    }

    [data-theme="dark"] body {
      background: radial-gradient(circle at top, #020617 0, #020617 50%, #000 100%);
    }

    /* NAVBAR */
    .navbar {
      position: sticky;
      top: 0;
      z-index: 1000;
      background: var(--navbar);
      box-shadow: 0 2px 10px rgba(15, 23, 42, 0.06);
      backdrop-filter: blur(10px);
    }

    .nav-inner {
      max-width: 1600px;
      width: 100%;
      margin: 0 auto;
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 10px 24px;
    }

    .logo {
      display: flex;
      align-items: center;
      gap: 8px;
      font-weight: 700;
      font-size: 18px;
      color: var(--accent);
    }

    .logo span.icon {
      font-size: 20px;
    }

    .nav-links {
      display: flex;
      gap: 4px;
      align-items: center;
    }

    .nav-btn {
      border: none;
      background: transparent;
      padding: 8px 14px;
      border-radius: 999px;
      font-size: 13px;
      display: flex;
      align-items: center;
      gap: 6px;
      color: var(--text-soft);
      cursor: pointer;
      position: relative;
      transition: color .18s ease, background .18s ease,
                  transform .16s ease, box-shadow .16s ease;
    }

    .nav-btn::after {
      content: "";
      position: absolute;
      left: 18px;
      right: 18px;
      bottom: 3px;
      height: 2px;
      border-radius: 999px;
      background: var(--accent-strong);
      transform: scaleX(0);
      transform-origin: center;
      opacity: 0;
      transition: transform .20s ease, opacity .20s ease;
    }

    .nav-btn:hover {
      background: var(--accent-soft);
      color: var(--accent-strong);
      transform: translateY(-1px);
      box-shadow: 0 8px 18px rgba(37, 99, 235, 0.30);
    }

    .nav-btn:hover::after {
      transform: scaleX(1);
      opacity: 1;
    }

    .nav-btn.active {
      background: var(--accent-soft);
      color: var(--accent-strong);
      box-shadow: 0 10px 22px rgba(37, 99, 235, 0.40);
    }

    .nav-btn.active::after {
      transform: scaleX(1);
      opacity: 1;
    }

    .theme-toggle {
      border-radius: 999px;
      border: 1px solid var(--border);
      padding: 6px 13px;
      font-size: 12px;
      display: flex;
      align-items: center;
      gap: 6px;
      background: var(--card);
      cursor: pointer;
      transition: background .18s ease, transform .16s ease, box-shadow .16s ease;
    }

    .theme-toggle:hover {
      background: var(--accent-soft);
      transform: translateY(-1px);
      box-shadow: 0 8px 18px rgba(15, 23, 42, 0.22);
    }

    /* LAYOUT */
   .app {
   flex: 1;
  max-width: 1600px;        /* wider layout */
  width: 100%;
  margin: 24px auto 0px;
  padding: 0 48px;          /* more breathing space */
}


    .page {
      display: none;
      animation: fadeIn .3s ease-out;
    }

    .page.active {
      display: block;
    }

    @keyframes fadeIn {
      from { opacity: 0; transform: translateY(10px); }
      to   { opacity: 1; transform: translateY(0); }
    }

    .page-header {
      margin-bottom: 18px;
    }

    .page-header h1 {
      font-size: 26px;
      margin-bottom: 4px;
    }

    .page-header p {
      font-size: 13px;
      color: var(--text-soft);
    }

    .page-two-column {
      display: grid;
      grid-template-columns: minmax(0, 1.3fr) minmax(0, 1fr);
      gap: 18px;
      align-items: flex-start;
    }

    @media (max-width: 900px) {
      .page-two-column {
        grid-template-columns: 1fr;
      }
    }

    /* CARDS & GRID */
    .grid-2 {
      display: grid;
      grid-template-columns: minmax(0, 1.3fr) minmax(0, 1fr);
      gap: 18px;
    }

    .grid-3 {
      display: grid;
      grid-template-columns: repeat(3, minmax(0,1fr));
      gap: 16px;
    }

    .grid-4 {
      display: grid;
      grid-template-columns: repeat(4, minmax(0,1fr));
      gap: 28px;           /* stronger grid, not small */
      margin-top: 8px;
    }

    @media (max-width: 900px) {
      .grid-2, .grid-3, .grid-4 {
        grid-template-columns: 1fr;
      }
    }

    .card {
      background: var(--card);
      border-radius: var(--radius);
      border: 1px solid var(--border);
      box-shadow: var(--shadow-soft);
      padding: 26px 28px;
      margin-bottom: 16px;
      transition: transform .18s ease, box-shadow .18s ease,
                  border-color .18s ease, background .18s ease;
    }

    .card:hover {
      transform: translateY(-3px);
      box-shadow: var(--shadow-strong);
      border-color: rgba(37, 99, 235, 0.45);
      background: linear-gradient(180deg, rgba(248,250,252,0.98), var(--card));
    }

    [data-theme="dark"] .card:hover {
      background: #020617;
    }

    .card-header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 10px;
    }

    .card-title {
      font-size: 15px;
      font-weight: 600;
    }

    .card-sub {
      font-size: 12px;
      color: var(--text-soft);
    }

    /* STATS CARDS */
    .stat-card {
      background: var(--card);
      border-radius: 18px;
      border: 1px solid var(--border);
      padding: 22px 24px;
      box-shadow: var(--shadow-soft);
      transition: transform .18s ease, box-shadow .18s ease, border-color .18s ease;
    }

    .stat-card:hover {
      transform: translateY(-4px);
      box-shadow: 0 14px 28px rgba(37, 99, 235, 0.45);
      border-color: var(--accent);
    }

    .stat-label {
      font-size: 11px;
      color: var(--text-soft);
      text-transform: uppercase;
      margin-bottom: 4px;
    }

    .stat-value {
      font-size: 26px;
      font-weight: 700;
    }

    .stat-tag {
      font-size: 11px;
      color: var(--accent-strong);
      background: var(--accent-soft);
      border-radius: 999px;
      padding: 2px 8px;
      display: inline-block;
      margin-top: 4px;
    }

    /* FORMS */
    .field-row {
      display: grid;
      gap: 10px;
      margin-top: 10px;
    }

    .field-row.two {
      grid-template-columns: 1fr 1fr;
    }

    .field-row.three {
      grid-template-columns: 1fr 1fr 1fr;
    }

    label {
      font-size: 12px;
      color: var(--text-soft);
      margin-bottom: 3px;
      display: block;
    }

    input, select {
      width: 100%;
      padding: 8px 10px;
      border-radius: 10px;
      border: 1px solid var(--border);
      background: #f9fafb;
      color: var(--text);
      font-size: 13px;
      outline: none;
      transition: border-color .16s ease, box-shadow .16s ease, transform .16s ease, background .16s ease;
    }

    [data-theme="dark"] input,
    [data-theme="dark"] select {
      background: var(--card-alt);
    }

    input:hover, select:hover {
      border-color: var(--accent);
    }

    input:focus, select:focus {
      border-color: var(--accent);
      box-shadow: 0 0 0 1px var(--accent-soft);
      transform: translateY(-1px);
      background: #ffffff;
    }

    /* BUTTONS */
    .btn-row {
      display: flex;
      gap: 8px;
      margin-top: 12px;
      flex-wrap: wrap;
    }

    .btn {
      border: none;
      border-radius: 999px;
      padding: 9px 16px;
      font-size: 13px;
      font-weight: 600;
      cursor: pointer;
      transition: transform .16s ease, box-shadow .16s ease, background .16s ease, filter .16s ease;
      display: inline-flex;
      align-items: center;
      gap: 6px;
    }

    .btn-primary {
      background: linear-gradient(135deg, #2563eb, #1d4ed8);
      color: #fff;
      box-shadow: 0 10px 20px rgba(37, 99, 235, 0.40);
    }

    .btn-primary:hover {
      transform: translateY(-2px) scale(1.04);
      box-shadow: 0 16px 32px rgba(37, 99, 235, 0.55);
      filter: brightness(1.03);
    }

    .btn-secondary {
      background: var(--accent-soft);
      color: var(--accent-strong);
    }

    .btn-secondary:hover {
      transform: translateY(-2px) scale(1.03);
      box-shadow: 0 10px 22px rgba(37, 99, 235, 0.22);
    }

    .btn-danger {
      background: linear-gradient(135deg, #ef4444, #b91c1c);
      color: #fff;
      box-shadow: 0 10px 20px rgba(239, 68, 68, 0.40);
    }

    .btn-danger:hover {
      transform: translateY(-2px) scale(1.04);
      box-shadow: 0 16px 32px rgba(239, 68, 68, 0.60);
    }

    /* STATUS PILL */
    .status-pill {
      margin-top: 10px;
      padding: 8px 12px;
      border-radius: 10px;
      background: var(--accent-soft);
      font-size: 12px;
      color: var(--text-soft);
      min-height: 34px;
      display: flex;
      align-items: center;
      transition: background .18s ease, color .18s ease, box-shadow .18s ease;
      border: 1px solid rgba(148, 163, 184, 0.4);
    }

    .status-pill:hover {
      box-shadow: 0 6px 16px rgba(148, 163, 184, 0.40);
    }

    .status-pill.status-success {
      background: rgba(34, 197, 94, 0.12);
      color: #16a34a;
      box-shadow: 0 0 0 1px rgba(34, 197, 94, 0.35);
    }

    .status-pill.status-error {
      background: rgba(239, 68, 68, 0.12);
      color: #b91c1c;
      box-shadow: 0 0 0 1px rgba(239, 68, 68, 0.35);
    }

    /* TABLE */
    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 12px;
      margin-top: 8px;
      border-radius: 12px;
      overflow: hidden;
      border: 1px solid var(--border);
    }

    th, td {
      padding: 8px 6px;
      border-bottom: 1px solid var(--border);
      text-align: left;
    }

    th {
      font-size: 11px;
      text-transform: uppercase;
      color: var(--text-soft);
      background: rgba(148, 163, 184, 0.10);
    }

    tr:hover td {
      background: var(--card-alt);
    }

    /* LOG */
    .log {
      max-height: 220px;
      overflow-y: auto;
      font-size: 12px;
      margin-top: 6px;
      border-radius: 10px;
      border: 1px dashed var(--border);
      padding: 6px 8px;
      background: rgba(248, 250, 252, 0.8);
    }

    .log-line {
      display: flex;
      justify-content: space-between;
      border-bottom: 1px dotted var(--border);
      padding: 4px 0;
      color: var(--text-soft);
    }

    .log-time {
      font-size: 10px;
      color: #a855f7;
      white-space: nowrap;
      margin-right: 6px;
    }

    /* TOAST */
    .toast {
      position: fixed;
      right: 20px;
      bottom: 20px;
      z-index: 2000;
      min-width: 220px;
      max-width: 320px;
      background: var(--card);
      border-radius: 12px;
      border: 1px solid var(--border);
      box-shadow: var(--shadow-soft);
      padding: 10px 12px;
      font-size: 13px;
      display: none;
      align-items: center;
      gap: 8px;
    }

    .toast.show {
      display: flex;
      animation: slideUp .3s ease-out;
    }

    @keyframes slideUp {
      from { opacity: 0; transform: translateY(10px); }
      to   { opacity: 1; transform: translateY(0); }
    }

    .toast-icon {
      font-size: 16px;
    }

    .toast-text {
      flex: 1;
    }

    /* GANTT */
    .gantt {
      max-height: 260px;
      overflow-x: auto;
      font-size: 11px;
      border-radius: 12px;
      border: 1px solid var(--border);
      padding: 8px 10px;
      background: rgba(248, 250, 252, 0.9);
    }

    .gantt-row {
      display: flex;
      align-items: center;
      margin-bottom: 6px;
    }

    .gantt-label {
      width: 60px;
      color: var(--text-soft);
    }

    .gantt-track {
      flex: 1;
      height: 16px;
      background: var(--card-alt);
      border-radius: 8px;
      position: relative;
      overflow: hidden;
    }

    .gantt-bar {
      position: absolute;
      height: 100%;
      border-radius: 8px;
      background: linear-gradient(90deg, #2563eb, #38bdf8);
      opacity: 0.9;
      transition: transform .15s ease;
    }

    .gantt-bar:hover {
      transform: scaleY(1.1);
    }

    canvas {
      max-width: 100%;
      background: var(--card-alt);
      border-radius: 12px;
      padding: 6px;
      border: 1px solid var(--border);
    }

    .hint-text {
      font-size: 13px;
      color: var(--text-soft);
      line-height: 1.5;
    }

    .pill {
      display: inline-flex;
      align-items: center;
      padding: 4px 10px;
      border-radius: 999px;
      font-size: 11px;
      background: rgba(37, 99, 235, 0.10);
      color: #2563eb;
      margin-bottom: 8px;
    }

  /* ===== FOOTER ===== */
footer {
  margin-top: auto;                 /* keeps footer at bottom */
  padding: 14px 24px 20px;
  font-size: 11px;
  text-align: center;
  color: var(--text-soft);
  border-top: 1px solid var(--border);
  background: rgba(248, 250, 252, 0.95);

  /* animation */
  animation: footerSlideUp 0.8s ease-out;
  position: relative;
  overflow: hidden;
}

/* Dark mode footer */
[data-theme="dark"] footer {
  background: #020617;
}

/* Footer entrance animation */
@keyframes footerSlideUp {
  from {
    opacity: 0;
    transform: translateY(20px);
  }
  to {
    opacity: 1;
    transform: translateY(0);
  }
}

/* Animated glowing top border */
footer::before {
  content: "";
  position: absolute;
  top: 0;
  left: -100%;
  width: 200%;
  height: 2px;
  background: linear-gradient(
    90deg,
    transparent,
    var(--accent),
    transparent
  );
  animation: footerGlow 6s linear infinite;
}

/* Glow animation */
@keyframes footerGlow {
  from { left: -100%; }
  to   { left: 100%; }
}

/* Hover animation */
footer:hover {
  color: var(--text);
  background: var(--card);
  transition: background 0.3s ease, color 0.3s ease;
}


    footer strong {
      font-weight: 600;
      transition: background 0.3s ease, color 0.3s ease;
    }
  </style>

</head>
<body>
<div class="toast" id="toast"><span class="toast-icon" id="toastIcon">ℹ️</span><span class="toast-text" id="toastText"></span></div>

<nav class="navbar">
  <div class="nav-inner">
    <div class="logo"><span class="icon">⚙️</span><span>ThreadLab Monitor</span></div>
    <div class="nav-links">
      <button class="nav-btn active" data-page="overview">📊 Overview</button>
      <button class="nav-btn" data-page="create">🆕 Create Pool</button>
      <button class="nav-btn" data-page="submit">📤 Submit Tasks</button>
      <button class="nav-btn" data-page="shutdown">🛑 Shutdown</button>
      <button class="nav-btn" data-page="parallel">⚡ Parallel Demo</button>
      <button class="nav-btn" data-page="analytics">📈 Analytics</button>
    </div>
    <button class="theme-toggle" id="themeToggle"><span id="themeIcon">🌞</span><span id="themeLabel">Light</span></button>
  </div>
</nav>

<div class="app">

  <!-- OVERVIEW -->
  <section id="overview" class="page active">
    <div class="page-header">
      <h1>Thread Pool Dashboard</h1>
      <p>Live monitoring of all thread pools, worker threads, and queued tasks.</p>
    </div>

    <div class="grid-4">
      <div class="stat-card">
        <div class="stat-label">Pools</div>
        <div class="stat-value" id="statPools">0</div>
        <div class="stat-tag">Total instances</div>
      </div>
      <div class="stat-card">
        <div class="stat-label">Active Threads</div>
        <div class="stat-value" id="statThreads">0</div>
        <div class="stat-tag">Currently running</div>
      </div>
      <div class="stat-card">
        <div class="stat-label">Queued Tasks</div>
        <div class="stat-value" id="statQueued">0</div>
        <div class="stat-tag">Waiting in queue</div>
      </div>
      <div class="stat-card">
        <div class="stat-label">Completed</div>
        <div class="stat-value" id="statCompleted">0</div>
        <div class="stat-tag">Since start</div>
      </div>
    </div>

    <div class="card">
      <div class="card-header">
        <div class="card-title">Pool Statistics</div>
        <div class="card-sub">Auto refresh every 2 seconds</div>
      </div>
      <table>
        <thead>
          <tr>
            <th>ID</th><th>Name</th><th>Threads</th><th>Active</th><th>Queued</th>
            <th>Completed</th><th>Avg ms</th><th>CPU %</th><th>Mem %</th>
          </tr>
        </thead>
        <tbody id="poolTable">
          <tr><td colspan="9">No pools yet. Create one.</td></tr>
        </tbody>
      </table>
    </div>

    <div class="card">
      <div class="card-header">
        <div class="card-title">Recent Activity</div>
        <div class="card-sub">Log of pool creation, task submission, and demos.</div>
      </div>
      <div class="log" id="log"></div>
    </div>
  </section>

  <!-- CREATE -->
  <section id="create" class="page">
    <div class="page-header">
      <h1>Create Pool</h1>
      <p>Configure and create a new worker thread pool with auto-scaling.</p>
    </div>

    <div class="page-two-column">
      <!-- LEFT: FORM -->
      <div class="card">
        <div class="card-header">
          <div class="card-title">Pool Configuration</div>
        </div>

        <div class="field-row">
          <div>
            <label for="poolName">Pool Name</label>
            <input id="poolName" placeholder="e.g. FileWorker, AnalyticsPool" />
          </div>
        </div>

        <div class="field-row">
          <div>
            <label for="threads">Number of Worker Threads</label>
            <input id="threads" type="number" min="1" value="4" />
          </div>
        </div>

        <div class="btn-row">
          <button class="btn btn-primary" id="btnCreate">Create Pool</button>
        </div>

        <div class="status-pill" id="createStatus">
          Ready to create a new pool.
        </div>
      </div>

      <!-- RIGHT: SUMMARY / TIPS -->
      <div class="card">
        <div class="card-header">
          <div class="card-title">Pool Summary & Tips</div>
        </div>

        <div class="hint-text" style="margin-bottom: 12px;">
          <p id="lastPoolSummary">
            No pool created yet. Create one on the left to see details here.
          </p>
        </div>

        <hr style="border:none;border-top:1px solid var(--border);margin:10px 0;" />

        <div class="hint-text">
          <p><strong>What this page does:</strong></p>
          <ul style="padding-left:18px;margin-top:6px;">
            <li>Creates a named thread pool (e.g. <em>FileWorker</em>).</li>
            <li>Each pool appears on the <strong>Overview</strong> page.</li>
            <li>You can push work from the <strong>Submit Tasks</strong> tab.</li>
            <li>Use this summary to explain how thread pools are managed.</li>
          </ul>
        </div>
      </div>
    </div>
  </section>

  <!-- SUBMIT -->
  <section id="submit" class="page">
    <div class="page-header">
      <h1>Submit Tasks</h1>
      <p>Push different workloads into a selected pool.</p>
    </div>

    <div class="grid-2">
      <div class="card">
        <div class="card-header">
          <div class="card-title">Custom Submission</div>
          <div class="card-sub">Choose pool, load and priority.</div>
        </div>
        <div class="field-row two">
          <div>
            <label for="poolIdSubmit">Pool ID</label>
            <input id="poolIdSubmit" type="number" min="1" value="1" />
          </div>
          <div>
            <label for="taskCount">Task Count</label>
            <input id="taskCount" type="number" min="1" value="10" />
          </div>
        </div>
        <div class="field-row two">
          <div>
            <label for="priority">Priority</label>
            <select id="priority">
              <option value="1">Normal (1)</option>
              <option value="0">High (0)</option>
              <option value="2">Low (2)</option>
            </select>
          </div>
          <div>
            <label for="taskType">Task Type</label>
            <select id="taskType">
              <option value="heavy">Heavy CPU</option>
              <option value="fib">Fibonacci</option>
              <option value="sort">Sorting</option>
              <option value="matrix">Matrix Multiply</option>
              <option value="prime">Prime Search</option>
              <option value="io">I/O Dummy</option>
            </select>
          </div>
        </div>
        <div class="btn-row">
          <button class="btn btn-primary" id="btnSubmit">Submit Tasks</button>
        </div>
        <div class="status-pill" id="submitStatus">Ready to submit tasks.</div>
      </div>

      <div class="card">
        <div class="card-header">
          <div class="card-title">Quick Actions</div>
          <div class="card-sub">Quick load presets for testing the scheduler.</div>
        </div>
        <div class="btn-row">
          <button class="btn btn-secondary" id="btnQuick50">50 Tasks (Light)</button>
          <button class="btn btn-secondary" id="btnQuick1k">1000 Tasks (Stress Test)</button>
          <button class="btn btn-secondary" id="btnQuick5k">5000 Tasks (Heavy Load)</button>
        </div>
        <div class="btn-row">
          <button class="btn btn-danger" id="btnCancelPending">Cancel Pending Tasks</button>
        </div>
        <div class="status-pill" id="quickStatus">Use quick presets or cancel queued work.</div>
      </div>
    </div>
  </section>

  <!-- SHUTDOWN -->
  <section id="shutdown" class="page">
    <div class="page-header">
      <h1>Shutdown</h1>
      <p>Gracefully terminate a pool after it finishes all running tasks (no forced stop).</p>
    </div>

    <div class="card" style="max-width:520px;">
      <div class="card-header">
        <div class="card-title">Terminate Pool</div>
      </div>
      <div class="field-row">
        <div>
          <label for="poolIdShutdown">Pool ID</label>
          <input id="poolIdShutdown" type="number" min="1" value="1" />
        </div>
      </div>
      <div class="btn-row">
        <button class="btn btn-danger" id="btnShutdown">Shutdown Pool</button>
      </div>
      <div class="status-pill" id="shutdownStatus">No shutdown requested.</div>
    </div>
  </section>

  <!-- PARALLEL DEMO -->
  <section id="parallel" class="page">
    <div class="page-header">
      <h1>Parallel For Demo</h1>
      <p>Showcase chunked parallel computation using a temporary pool.</p>
    </div>

    <div class="grid-2">
      <div class="card">
        <div class="card-header">
          <div class="card-title">Computation</div>
          <div class="card-sub">Compute the sum from 0 to N-1 using parallel_for with a configurable chunk size.</div>
        </div>
        <div class="field-row two">
          <div>
            <label for="pfN">Range N</label>
            <input id="pfN" type="number" min="1" value="100000" />
          </div>
          <div>
            <label for="pfChunk">Chunk Size</label>
            <input id="pfChunk" type="number" min="1" value="1000" />
          </div>
        </div>
        <div class="btn-row">
          <button class="btn btn-primary" id="btnParallelDemo">Run Demo</button>
        </div>
        <div class="status-pill" id="parallelStatus">Ready to run parallel_for demo.</div>
      </div>

      <div class="card">
        <div class="card-header">
          <div class="card-title">Result</div>
        </div>
        <div id="parallelResult" style="font-size:13px;color:var(--text-soft);">
          No run yet. Start the demo to see timing and sum.
        </div>
      </div>
    </div>
  </section>

  <!-- ANALYTICS -->
  <section id="analytics" class="page">
    <div class="page-header">
      <h1>Analytics</h1>
      <p>Charts and timeline view of thread pool behavior over time.</p>
    </div>

    <div class="grid-2">
      <div class="card">
        <div class="card-header">
          <div class="card-title">CPU & Queue Over Time</div>
          <div class="card-sub">Based on dashboard refresh samples.</div>
        </div>
        <canvas id="chartCpu"></canvas>
      </div>

      <div class="card">
        <div class="card-header">
          <div class="card-title">Memory & Completed Tasks</div>
          <div class="card-sub">Synthetic memory utilization based on task load.</div>
        </div>
        <canvas id="chartMem"></canvas>
      </div>
    </div>

    <div class="card">
      <div class="card-header">
        <div class="card-title">Thread Timeline (Gantt)</div>
        <div class="card-sub">
          Visualises last tasks in a pool. Pool ID:
          <input id="timelinePoolId" type="number" value="1" style="width:60px;margin-left:4px;" />
          <button class="btn btn-secondary" id="btnLoadTimeline" style="margin-left:6px;padding:4px 10px;font-size:11px;">Load</button>
        </div>
      </div>
      <div class="gantt" id="gantt"></div>
    </div>
  </section>

</div>

<footer>
  &copy; 2025 <strong>ThreadLab</strong>. All rights reserved.
  <span style="display:block;">ThreadLab: C++ Thread Pool Monitoring Dashboard</span>
</footer>

<script>
  // ===== Theme handling (auto-detect + toggle) =====
  const bodyEl = document.documentElement;
  const storedTheme = localStorage.getItem("threadlab-theme");
  if (!storedTheme) {
    const prefersDark = window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches;
    bodyEl.setAttribute("data-theme", prefersDark ? "dark" : "light");
    localStorage.setItem("threadlab-theme", prefersDark ? "dark" : "light");
  } else {
    bodyEl.setAttribute("data-theme", storedTheme);
  }

  const themeToggle = document.getElementById("themeToggle");
  const themeIcon = document.getElementById("themeIcon");
  const themeLabel = document.getElementById("themeLabel");

  function updateThemeUI(theme) {
    if (theme === "dark") {
      themeIcon.textContent = "🌙";
      themeLabel.textContent = "Dark";
    } else {
      themeIcon.textContent = "🌞";
      themeLabel.textContent = "Light";
    }
  }
  updateThemeUI(bodyEl.getAttribute("data-theme"));

  themeToggle.addEventListener("click", () => {
    const current = bodyEl.getAttribute("data-theme") || "light";
    const next = current === "light" ? "dark" : "light";
    bodyEl.setAttribute("data-theme", next);
    localStorage.setItem("threadlab-theme", next);
    updateThemeUI(next);
  });

  // ===== Navigation =====
  const pages = document.querySelectorAll(".page");
  const navBtns = document.querySelectorAll(".nav-btn");

  navBtns.forEach(btn => {
    btn.addEventListener("click", () => {
      const page = btn.dataset.page;
      pages.forEach(p => p.classList.remove("active"));
      document.getElementById(page).classList.add("active");
      navBtns.forEach(b => b.classList.remove("active"));
      btn.classList.add("active");
    });
  });

  // ===== Toast helper =====
  const toast = document.getElementById("toast");
  const toastText = document.getElementById("toastText");
  const toastIcon = document.getElementById("toastIcon");
  let toastTimeout = null;

  function showToast(message, type="info") {
    toastText.textContent = message;
    toastIcon.textContent = type === "error" ? "❌" : (type === "success" ? "✅" : "ℹ️");
    toast.classList.add("show");
    if (toastTimeout) clearTimeout(toastTimeout);
    toastTimeout = setTimeout(() => toast.classList.remove("show"), 2600);
  }

  // ===== Log helper =====
  const logEl = document.getElementById("log");
  function addLog(msg) {
    const div = document.createElement("div");
    div.className = "log-line";
    const time = new Date().toLocaleTimeString();
    div.innerHTML = `<span class="log-time">[${time}]</span><span>${msg}</span>`;
    logEl.prepend(div);
  }

  // ===== API helper =====
  async function api(url, method="GET") {
    try {
      const res = await fetch(url, { method });
      const text = await res.text();
      try { return JSON.parse(text); } catch { return { error: "Invalid JSON", raw: text }; }
    } catch (e) {
      return { error: e.message || "Network error" };
    }
  }

  // ===== CREATE POOL =====
  document.getElementById("btnCreate").addEventListener("click", async () => {
    const threads = document.getElementById("threads").value;
    const name = document.getElementById("poolName").value.trim() || "Unnamed pool";
    const status = document.getElementById("createStatus");
    const summary = document.getElementById("lastPoolSummary");

    status.classList.remove("status-success","status-error");
    status.textContent = "⏳ Creating thread pool...";

    const data = await api(`/api/pools?threads=${encodeURIComponent(threads)}&name=${encodeURIComponent(name)}`, "POST");

    if (data.pool_id !== undefined) {
      status.textContent = `✅ Created pool #${data.pool_id}`;
      status.classList.add("status-success");
      addLog(`Created pool #${data.pool_id} (${name}, ${threads} threads)`);

      summary.innerHTML = `
        <strong>Last created pool</strong><br/>
        ID: <strong>#${data.pool_id}</strong><br/>
        Name: <strong>${name}</strong><br/>
        Threads: <strong>${threads}</strong><br/>
        Auto-scaling: handled internally by the library.
      `;

      showToast(`Pool #${data.pool_id} created`, "success");
      refreshStats();
    } else {
      status.textContent = `❌ ${data.error || "Failed to create pool"}`;
      status.classList.add("status-error");
      showToast("Pool creation failed", "error");
    }
  });

  // ===== SUBMIT TASKS =====
  function buildTaskUrl(poolId, count, priority, type) {
    return `/api/pools/${encodeURIComponent(poolId)}/tasks?count=${encodeURIComponent(count)}&priority=${encodeURIComponent(priority)}&type=${encodeURIComponent(type)}`;
  }

  document.getElementById("btnSubmit").addEventListener("click", async () => {
    const poolId = document.getElementById("poolIdSubmit").value;
    const count = document.getElementById("taskCount").value;
    const priority = document.getElementById("priority").value;
    const type = document.getElementById("taskType").value;
    const status = document.getElementById("submitStatus");

    status.classList.remove("status-success","status-error");

    if (!poolId) {
      status.textContent = "Please enter a pool ID.";
      status.classList.add("status-error");
      return;
    }
    status.textContent = "Submitting tasks...";
    const data = await api(buildTaskUrl(poolId, count, priority, type), "POST");
    if (data.tasks_added !== undefined) {
      status.textContent = `Queued ${data.tasks_added} ${type} tasks to pool ${poolId}.`;
      status.classList.add("status-success");
      showToast(`Queued ${data.tasks_added} ${type} tasks`, "success");
      addLog(`Queued ${data.tasks_added} ${type} tasks to pool #${poolId}.`);
      refreshStats();
    } else {
      status.textContent = `Error: ${data.error || "Failed to submit tasks."}`;
      status.classList.add("status-error");
      showToast("Task submission failed", "error");
    }
  });

  async function quickSubmit(count) {
    const poolId = document.getElementById("poolIdSubmit").value || "1";
    const type = document.getElementById("taskType").value || "heavy";
    const status = document.getElementById("quickStatus");
    status.classList.remove("status-success","status-error");
    status.textContent = `Submitting ${count} ${type} tasks...`;
    const data = await api(buildTaskUrl(poolId, count, 1, type), "POST");
    if (data.tasks_added !== undefined) {
      status.textContent = `Queued ${data.tasks_added} tasks to pool ${poolId}.`;
      status.classList.add("status-success");
      showToast(`Quick: ${data.tasks_added} tasks queued`, "success");
      addLog(`Quick submit: ${data.tasks_added} tasks to pool #${poolId}.`);
      refreshStats();
    } else {
      status.textContent = `Error: ${data.error || "Quick submit failed."}`;
      status.classList.add("status-error");
      showToast("Quick submit failed", "error");
    }
  }

  document.getElementById("btnQuick50").addEventListener("click", () => quickSubmit(50));
  document.getElementById("btnQuick1k").addEventListener("click", () => quickSubmit(1000));
  document.getElementById("btnQuick5k").addEventListener("click", () => quickSubmit(5000));

  document.getElementById("btnCancelPending").addEventListener("click", async () => {
    const poolId = document.getElementById("poolIdSubmit").value || "1";
    const status = document.getElementById("quickStatus");
    status.classList.remove("status-success","status-error");
    status.textContent = "Cancelling pending tasks...";
    const data = await api(`/api/pools/${encodeURIComponent(poolId)}/cancel`, "POST");
    if (data.removed !== undefined) {
      status.textContent = `Cancelled ${data.removed} pending tasks in pool ${poolId}.`;
      status.classList.add("status-success");
      showToast(`Cancelled ${data.removed} pending`, "success");
      addLog(`Cancelled ${data.removed} pending tasks in pool #${poolId}.`);
      refreshStats();
    } else {
      status.textContent = `Error: ${data.error || "Cancel failed."}`;
      status.classList.add("status-error");
      showToast("Cancel failed", "error");
    }
  });

  // ===== SHUTDOWN =====
  document.getElementById("btnShutdown").addEventListener("click", async () => {
    const poolId = document.getElementById("poolIdShutdown").value;
    const status = document.getElementById("shutdownStatus");
    status.classList.remove("status-success","status-error");
    if (!poolId) {
      status.textContent = "Enter a pool ID.";
      status.classList.add("status-error");
      return;
    }
    status.textContent = "Shutting down...";
    const data = await api(`/api/pools/${encodeURIComponent(poolId)}/shutdown`, "POST");
    if (data.status === "shutdown") {
      status.textContent = `Pool ${poolId} shutdown.`;
      status.classList.add("status-success");
      showToast(`Pool #${poolId} shutdown`, "success");
      addLog(`Pool #${poolId} shutdown.`);
      refreshStats();
    } else {
      status.textContent = `Error: ${data.error || "Shutdown failed."}`;
      status.classList.add("status-error");
      showToast("Shutdown failed", "error");
    }
  });

  // ===== PARALLEL DEMO =====
  document.getElementById("btnParallelDemo").addEventListener("click", async () => {
    const N = document.getElementById("pfN").value || "100000";
    const chunk = document.getElementById("pfChunk").value || "1000";
    const status = document.getElementById("parallelStatus");
    status.classList.remove("status-success","status-error");
    status.textContent = "Running parallel_for demo...";
    const data = await api(`/api/parallel_for_demo?N=${encodeURIComponent(N)}&chunk=${encodeURIComponent(chunk)}`, "POST");
    if (data.status === "ok") {
      status.textContent = "Completed.";
      status.classList.add("status-success");
      const out = document.getElementById("parallelResult");
      out.innerHTML = `
        <p><strong>Sum:</strong> ${data.sum}</p>
        <p><strong>N:</strong> ${data.N}</p>
        <p><strong>Chunk:</strong> ${data.chunk}</p>
        <p><strong>Time:</strong> ${data.ms} ms</p>`;
      addLog(`parallel_for demo N=${data.N}, chunk=${data.chunk}, ms=${data.ms}`);
      showToast("Parallel demo complete", "success");
    } else {
      status.textContent = "Error running demo.";
      status.classList.add("status-error");
      showToast("Parallel demo failed", "error");
    }
  });

  // ===== STATS & ANALYTICS =====
  const statPools = document.getElementById("statPools");
  const statThreads = document.getElementById("statThreads");
  const statQueued = document.getElementById("statQueued");
  const statCompleted = document.getElementById("statCompleted");
  const poolTable = document.getElementById("poolTable");

  let historyLabels = [];
  let historyCpu = [];
  let historyQueue = [];
  let historyMem = [];
  let historyCompleted = [];

  const cpuCtx = document.getElementById("chartCpu").getContext("2d");
  const memCtx = document.getElementById("chartMem").getContext("2d");

  const cpuChart = new Chart(cpuCtx, {
    type: "line",
    data: {
      labels: historyLabels,
      datasets: [
        { label:"CPU %", data: historyCpu, borderWidth:1, tension:0.25 },
        { label:"Queued Tasks", data: historyQueue, borderWidth:1, tension:0.25 }
      ]
    },
    options:{responsive:true,maintainAspectRatio:false,scales:{y:{beginAtZero:true}}}
  });

  const memChart = new Chart(memCtx, {
    type: "line",
    data: {
      labels: historyLabels,
      datasets: [
        { label:"Mem %", data: historyMem, borderWidth:1, tension:0.25 },
        { label:"Completed", data: historyCompleted, borderWidth:1, tension:0.25 }
      ]
    },
    options:{responsive:true,maintainAspectRatio:false,scales:{y:{beginAtZero:true}}}
  });

  async function refreshStats() {
    const data = await api("/api/pools", "GET");
    if (!Array.isArray(data)) {
      poolTable.innerHTML = `<tr><td colspan="9">Error loading stats.</td></tr>`;
      return;
    }
    if (data.length === 0) {
      poolTable.innerHTML = `<tr><td colspan="9">No pools yet. Create one.</td></tr>`;
      statPools.textContent = "0";
      statThreads.textContent = "0";
      statQueued.textContent = "0";
      statCompleted.textContent = "0";
      return;
    }

    let totalPools = data.length;
    let totalThreads = 0, totalQueued = 0, totalCompleted = 0, totalActive = 0;
    let avgCpu = 0, avgMem = 0;

    poolTable.innerHTML = "";
    data.forEach(item => {
      totalThreads += item.total_threads;
      totalQueued += item.queued_tasks;
      totalCompleted += item.completed_tasks;
      totalActive += item.active_threads;
      avgCpu += item.cpu;
      avgMem += item.mem;

      const tr = document.createElement("tr");
      tr.innerHTML = `
        <td>${item.id}</td>
        <td>${item.name}</td>
        <td>${item.total_threads}</td>
        <td>${item.active_threads}</td>
        <td>${item.queued_tasks}</td>
        <td>${item.completed_tasks}</td>
        <td>${item.avg_task_ms.toFixed(2)}</td>
        <td>${item.cpu.toFixed(1)}</td>
        <td>${item.mem.toFixed(1)}</td>`;
      poolTable.appendChild(tr);
    });

    statPools.textContent = totalPools;
    statThreads.textContent = totalThreads;
    statQueued.textContent = totalQueued;
    statCompleted.textContent = totalCompleted;

    const nowLabel = new Date().toLocaleTimeString();
    historyLabels.push(nowLabel);
    historyCpu.push(data.length ? avgCpu / data.length : 0);
    historyQueue.push(totalQueued);
    historyMem.push(data.length ? avgMem / data.length : 0);
    historyCompleted.push(totalCompleted);

    if (historyLabels.length > 30) {
      historyLabels.shift();
      historyCpu.shift();
      historyQueue.shift();
      historyMem.shift();
      historyCompleted.shift();
    }
    cpuChart.update();
    memChart.update();
  }

  setInterval(refreshStats, 2000);
  refreshStats();

  // ===== Timeline (Gantt) =====
  document.getElementById("btnLoadTimeline").addEventListener("click", async () => {
    const id = document.getElementById("timelinePoolId").value || "1";
    const data = await api(`/api/timeline?pool_id=${encodeURIComponent(id)}`, "GET");
    const gantt = document.getElementById("gantt");
    gantt.innerHTML = "";
    if (!Array.isArray(data) || data.length === 0) {
      gantt.innerHTML = "<p style='font-size:12px;color:var(--text-soft);'>No timeline data. Submit some tasks first.</p>";
      return;
    }
    let maxEnd = 0;
    data.forEach(ev => { if (ev.end_ms > maxEnd) maxEnd = ev.end_ms; });
    data.forEach(ev => {
      const row = document.createElement("div");
      row.className = "gantt-row";
      row.innerHTML = `<div class="gantt-label">T${ev.worker}</div><div class="gantt-track"></div>`;
      const track = row.querySelector(".gantt-track");
      const bar = document.createElement("div");
      bar.className = "gantt-bar";
      const startPct = (ev.start_ms / maxEnd) * 100;
      const widthPct = ((ev.end_ms - ev.start_ms) / maxEnd) * 100;
      bar.style.left = startPct + "%";
      bar.style.width = Math.max(widthPct, 2) + "%";
      track.appendChild(bar);
      gantt.appendChild(row);
    });
  });
</script>
</body>
</html>)HTML";
}

// ===============================================
//                   HTTP Backend
// ===============================================

int main() {
    ThreadPoolManager manager;
    httplib::Server svr;

    // Serve dashboard
    svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(build_dashboard_html(), "text/html; charset=UTF-8");
    });

    // Create pool: POST /api/pools?threads=8&name=XYZ
    svr.Post("/api/pools", [&](const httplib::Request& req, httplib::Response& res) {
        auto threadsParam = req.get_param_value("threads");
        auto nameParam = req.get_param_value("name");
        if (threadsParam.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"threads param required\"}", "application/json");
            return;
        }
        int threads = 0;
        try { threads = stoi(threadsParam); }
        catch (...) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid threads value\"}", "application/json");
            return;
        }
        if (threads <= 0) {
            res.status = 400;
            res.set_content("{\"error\":\"threads must be > 0\"}", "application/json");
            return;
        }
        int id = manager.createPool((size_t)threads, nameParam);
        ostringstream oss;
        oss << "{\"pool_id\":" << id
            << ",\"threads\":" << threads
            << ",\"name\":\"" << (nameParam.empty() ? ("Pool " + to_string(id)) : nameParam) << "\"}";
        res.set_content(oss.str(), "application/json");
    });

    // List pools: GET /api/pools
    svr.Get("/api/pools", [&](const httplib::Request&, httplib::Response& res) {
        auto all = manager.getAll();
        auto stats = manager.getAllStats();
        ostringstream oss;
        oss << "[";
        bool first = true;
        for (auto &p : all) {
            if (!first) oss << ",";
            first = false;
            int id = p.first;
            const auto &st = stats[id];
            oss << "{"
                << "\"id\":" << id
                << ",\"name\":\"" << p.second.name << "\""
                << ",\"total_threads\":" << st.totalThreads
                << ",\"active_threads\":" << st.activeThreads
                << ",\"queued_tasks\":" << st.queuedTasks
                << ",\"completed_tasks\":" << st.completedTasks
                << ",\"avg_task_ms\":" << st.avgTaskMs
                << ",\"cpu\":" << st.cpuPercent
                << ",\"mem\":" << st.memPercent
                << "}";
        }
        oss << "]";
        res.set_content(oss.str(), "application/json");
    });

    // Add tasks: POST /api/pools/<id>/tasks?count=100&priority=1&type=heavy
    svr.Post(R"(/api/pools/(\d+)/tasks)", [&](const httplib::Request& req, httplib::Response& res) {
        int id = stoi(req.matches[1].str());
        auto pool = manager.getPool(id);
        if (!pool) {
            res.status = 404;
            res.set_content("{\"error\":\"pool not found\"}", "application/json");
            return;
        }
        int count = 1;
        auto countParam = req.get_param_value("count");
        if (!countParam.empty()) {
            try { count = stoi(countParam); } catch (...) {
                res.status = 400;
                res.set_content("{\"error\":\"invalid count\"}", "application/json");
                return;
            }
        }
        if (count <= 0) count = 1;

        int priority = 1;
        auto prioParam = req.get_param_value("priority");
        if (!prioParam.empty()) {
            try { priority = stoi(prioParam); } catch (...) { priority = 1; }
        }
        string type = req.get_param_value("type");
        if (type.empty()) type = "heavy";

        for (int i = 0; i < count; ++i) {
            if (type == "fib") {
                pool->submitWithPriority(priority, task_fib, i);
            } else if (type == "sort") {
                pool->submitWithPriority(priority, task_sort, i);
            } else if (type == "matrix") {
                pool->submitWithPriority(priority, task_matrix, i);
            } else if (type == "prime") {
                pool->submitWithPriority(priority, task_prime, i);
            } else if (type == "io") {
                pool->submitWithPriority(priority, task_io_dummy, i);
            } else { // heavy
                pool->submitWithPriority(priority, task_heavy_cpu, i);
            }
        }

        ostringstream oss;
        oss << "{\"status\":\"queued\",\"tasks_added\":" << count << "}";
        res.set_content(oss.str(), "application/json");
    });

    // Cancel pending: POST /api/pools/<id>/cancel
    svr.Post(R"(/api/pools/(\d+)/cancel)", [&](const httplib::Request& req, httplib::Response& res) {
        int id = stoi(req.matches[1].str());
        auto pool = manager.getPool(id);
        if (!pool) {
            res.status = 404;
            res.set_content("{\"error\":\"pool not found\"}", "application/json");
            return;
        }
        size_t removed = manager.cancelPending(id);
        ostringstream oss;
        oss << "{\"status\":\"ok\",\"removed\":" << removed << "}";
        res.set_content(oss.str(), "application/json");
    });

    // Shutdown: POST /api/pools/<id>/shutdown
    svr.Post(R"(/api/pools/(\d+)/shutdown)", [&](const httplib::Request& req, httplib::Response& res) {
        int id = stoi(req.matches[1].str());
        auto pool = manager.getPool(id);
        if (!pool) {
            res.status = 404;
            res.set_content("{\"error\":\"pool not found\"}", "application/json");
            return;
        }
        manager.shutdownPool(id);
        res.set_content("{\"status\":\"shutdown\"}", "application/json");
    });

    // Timeline: GET /api/timeline?pool_id=X
    svr.Get("/api/timeline", [&](const httplib::Request& req, httplib::Response& res) {
        auto idParam = req.get_param_value("pool_id");
        if (idParam.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"pool_id required\"}", "application/json");
            return;
        }
        int id = stoi(idParam);
        auto pool = manager.getPool(id);
        if (!pool) {
            res.status = 404;
            res.set_content("{\"error\":\"pool not found\"}", "application/json");
            return;
        }
        auto events = pool->getTimelineSnapshot();
        ostringstream oss;
        oss << "[";
        bool first = true;
        for (auto &e : events) {
            if (!first) oss << ",";
            first = false;
            oss << "{"
                << "\"worker\":" << e.workerId
                << ",\"start_ms\":" << e.startMs
                << ",\"end_ms\":" << e.endMs
                << "}";
        }
        oss << "]";
        res.set_content(oss.str(), "application/json");
    });

    // Parallel_for demo
    svr.Post("/api/parallel_for_demo", [&](const httplib::Request& req, httplib::Response& res) {
        long long N = 100000;
        long long chunk = 1000;
        auto Nparam = req.get_param_value("N");
        auto Cparam = req.get_param_value("chunk");
        try {
            if (!Nparam.empty()) N = stoll(Nparam);
            if (!Cparam.empty()) chunk = stoll(Cparam);
        } catch (...) {}
        if (N <= 0) N = 1;
        if (chunk <= 0) chunk = 1;

        ThreadPool temp(4, 16);
        atomic<long long> sum{0};
        auto start = chrono::steady_clock::now();
        parallel_for(0LL, N, [&sum](long long i){ sum += i; }, temp, chunk);
        auto end = chrono::steady_clock::now();
        auto ms = chrono::duration_cast<chrono::milliseconds>(end - start).count();
        temp.shutdown();

        ostringstream oss;
        oss << "{\"status\":\"ok\",\"N\":" << N
            << ",\"chunk\":" << chunk
            << ",\"sum\":" << sum.load()
            << ",\"ms\":" << ms << "}";
        res.set_content(oss.str(), "application/json");
    });

    cout << "=========================================\n";
    cout << "  ThreadLab  Scalable Thread Management\n";
    cout << "  Backend: C++17 Thread Pool + REST API\n";
    cout << "  Frontend: Multi-page Web Dashboard\n";
    cout << "=========================================\n";
    cout << "Open your browser at: http://localhost:8080\n";

    svr.listen("0.0.0.0", 8080);
    manager.shutdownAll();
    return 0;
}

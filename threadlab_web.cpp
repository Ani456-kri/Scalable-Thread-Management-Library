// threadlab_web.cpp
// Single-file project: Thread pool backend + HTTP server + web dashboard

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

// ==== Windows version hack for httplib (works even if your real OS is Windows 8) ====
#ifdef _WIN32
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0A00   // pretend to target Windows 10
  #else
    #undef _WIN32_WINNT
    #define _WIN32_WINNT 0x0A00
  #endif
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
    // minThreads = initial threads, maxThreads = upper bound for auto-scaling
    ThreadPool(size_t minThreads, size_t maxThreads)
        : stop_(false),
          completedTasks_(0),
          activeTasks_(0),
          totalExecNs_(0),
          minThreads_(minThreads),
          maxThreads_(maxThreads),
          stopScaler_(false),
          nextSeq_(0)
    {
        if (minThreads_ == 0) {
            minThreads_ = std::thread::hardware_concurrency();
            if (minThreads_ == 0) minThreads_ = 4;
        }
        if (maxThreads_ < minThreads_) {
            maxThreads_ = minThreads_;
        }

        // Start initial workers
        for (size_t i = 0; i < minThreads_; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }

        // Start auto-scaler (only scales UP based on queue length)
        scalerThread_ = std::thread([this] { scalerLoop(); });
    }

    ~ThreadPool() {
        shutdown();
    }

    // Submit a task with NORMAL priority (1)
    template <class F, class... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        return submitWithPriority(1, std::forward<F>(f), std::forward<Args>(args)...);
    }

    // Submit a task with an explicit priority (0 = high, 1 = normal, 2 = low)
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
            // clamp priority into [0,2]
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

    // Graceful shutdown: finish queued tasks then stop threads
    void shutdown() {
        {
            std::lock_guard<mutex> lock(queueMutex_);
            if (stop_) return;
            stop_ = true;
        }

        // stop scaler + wake everything
        stopScaler_ = true;
        cv_.notify_all();

        if (scalerThread_.joinable()) {
            scalerThread_.join();
        }

        for (auto &t : workers_) {
            if (t.joinable()) {
                t.join();
            }
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
            s.avgTaskMs = avgNs / 1'000'000.0; // ns -> ms
        } else {
            s.avgTaskMs = 0.0;
        }
        return s;
    }

private:
    void workerLoop() {
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

            completedTasks_++;
            activeTasks_--;
        }
    }

    // Simple auto-scaler that only scales UP based on queuedTasks
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

            if (currentThreads >= maxThreads_) {
                continue; // already at cap
            }

            // Rule: if queue is much longer than currentThreads, spawn a few more
            if (s.queuedTasks > currentThreads * 4) {
                size_t toAdd = std::min(maxThreads_ - currentThreads, (size_t)2);

                std::lock_guard<std::mutex> lock(queueMutex_);
                for (size_t i = 0; i < toAdd; ++i) {
                    workers_.emplace_back([this] { workerLoop(); });
                }
            }
        }
    }

    // Worker threads
    vector<thread> workers_;

    // Task priority queue
    mutable mutex queueMutex_;
    condition_variable cv_;
    std::priority_queue<PrioritizedTask,
                        std::vector<PrioritizedTask>,
                        TaskCompare> tasks_;

    // Control flags
    atomic<bool> stop_;

    // Stats
    atomic<size_t> completedTasks_;
    atomic<size_t> activeTasks_;
    mutable mutex statsMutex_;
    uint64_t totalExecNs_;

    // Auto-scaling
    size_t minThreads_;
    size_t maxThreads_;
    std::thread scalerThread_;
    std::atomic<bool> stopScaler_;
    uint64_t nextSeq_;
};

// ================================================
//  Module 2: Concurrency API & Sync Layer (CASL)
// ================================================

class Mutex {
public:
    void lock()   { mtx_.lock(); }
    void unlock() { mtx_.unlock(); }
private:
    std::mutex mtx_;
};

class LockGuard {
public:
    explicit LockGuard(Mutex& m) : m_(m) { m_.lock(); }
    ~LockGuard() { m_.unlock(); }
private:
    Mutex& m_;
};

// Improved parallel_for with chunking for scalability
template<typename Index, typename Func>
void parallel_for(Index begin, Index end, Func f, ThreadPool& pool, Index chunkSize = 1000) {
    if (end <= begin) return;
    if (chunkSize <= 0) chunkSize = 1;

    std::vector<std::future<void>> futures;

    for (Index start = begin; start < end; start += chunkSize) {
        Index stop = std::min(start + chunkSize, end);
        futures.push_back(pool.submit([start, stop, &f]() {
            for (Index i = start; i < stop; ++i) {
                f(i);
            }
        }));
    }

    for (auto &fut : futures) {
        fut.get();
    }
}

// ================================================
//  Module 3: Monitoring & Manager
// ================================================

class ThreadPoolManager {
public:
    int createPool(size_t threads) {
        lock_guard<mutex> lock(mtx_);
        int id = ++lastId_;

        // minThreads = requested, maxThreads = up to 4x requested (simple policy)
        size_t minT = threads;
        size_t maxT = std::max(threads, threads * 4);

        pools_[id] = make_shared<ThreadPool>(minT, maxT);
        return id;
    }

    shared_ptr<ThreadPool> getPool(int id) {
        lock_guard<mutex> lock(mtx_);
        auto it = pools_.find(id);
        if (it != pools_.end()) return it->second;
        return nullptr;
    }

    map<int, ThreadStats> getAllStats() {
        lock_guard<mutex> lock(mtx_);
        map<int, ThreadStats> result;
        for (auto &p : pools_) {
            result[p.first] = p.second->getStats();
        }
        return result;
    }

    void shutdownPool(int id) {
        shared_ptr<ThreadPool> pool;
        {
            lock_guard<mutex> lock(mtx_);
            auto it = pools_.find(id);
            if (it != pools_.end()) {
                pool = it->second;
                pools_.erase(it);
            }
        }
        if (pool) {
            pool->shutdown();
        }
    }

    void shutdownAll() {
        map<int, shared_ptr<ThreadPool>> local;
        {
            lock_guard<mutex> lock(mtx_);
            local = pools_;
            pools_.clear();
        }
        for (auto &p : local) {
            p.second->shutdown();
        }
    }

private:
    mutex mtx_;
    int lastId_ = 0;
    map<int, shared_ptr<ThreadPool>> pools_;
};

// ===============================================
//   Demo helper functions ("use-cases")
// ===============================================

void simulateHeavyTask(int /*id*/) {
    volatile long long x = 0;
    for (int i = 0; i < 2'000'000; ++i) {
        x += i;
    }
}

long long slowFib(int n) {
    if (n <= 1) return n;
    return slowFib(n-1) + slowFib(n-2);
}

// ===============================================
//   HTML Dashboard (Frontend) – with priority + demo
// ===============================================
std::string build_dashboard_html() {
    return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <title>ThreadLab – Scalable Thread Management Library</title>
  <style>
    :root {
      --bg: #020617;
      --bg-elevated: #020617ee;
      --bg-card: #020617cc;
      --bg-soft: #0b1120;
      --border-subtle: #1f2937;
      --border-strong: #111827;
      --accent: #3b82f6;
      --accent-soft: #1d4ed8;
      --accent-pill: #0f172a;
      --danger: #dc2626;
      --text: #e5e7eb;
      --text-soft: #9ca3af;
      --text-faint: #6b7280;
      --shadow-main: 0 18px 45px rgba(0, 0, 0, 0.65);
      --shadow-hover: 0 22px 55px rgba(0, 0, 0, 0.85);
      --glass-blur: blur(10px);
    }

    [data-theme="light"] {
      --bg: #f3f4f6;
      --bg-elevated: #f9fafb;
      --bg-card: #ffffff;
      --bg-soft: #e5e7eb;
      --border-subtle: #d1d5db;
      --border-strong: #9ca3af;
      --accent: #2563eb;
      --accent-soft: #1d4ed8;
      --accent-pill: #e0f2fe;
      --danger: #dc2626;
      --text: #111827;
      --text-soft: #4b5563;
      --text-faint: #6b7280;
      --shadow-main: 0 14px 35px rgba(15, 23, 42, 0.16);
      --shadow-hover: 0 18px 45px rgba(15, 23, 42, 0.22);
      --glass-blur: blur(4px);
    }

    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
      font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    }

    body {
      background: radial-gradient(circle at top, #1e293b 0, var(--bg) 55%);
      color: var(--text);
      min-height: 100vh;
      padding: 16px 32px 32px;
      transition: background 0.25s ease, color 0.25s ease;
      animation: fadeIn 0.4s ease-out;
    }

    .app {
      max-width: 1200px;
      margin: 0 auto;
      animation: slideUp 0.5s ease-out;
    }

    @keyframes fadeIn {
      from { opacity: 0; }
      to   { opacity: 1; }
    }

    @keyframes slideUp {
      from { opacity: 0; transform: translateY(12px); }
      to   { opacity: 1; transform: translateY(0); }
    }

    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      margin-bottom: 18px;
      gap: 12px;
    }

    .title-group h1 {
      font-size: 28px;
      font-weight: 700;
      letter-spacing: 0.02em;
    }

    .title-group p {
      font-size: 13px;
      color: var(--text-soft);
      margin-top: 4px;
    }

    .tag-row {
      display: flex;
      align-items: center;
      gap: 8px;
      flex-wrap: wrap;
      margin-top: 4px;
    }

    .tag {
      padding: 4px 10px;
      border-radius: 999px;
      border: 1px solid rgba(34,197,94,0.4);
      font-size: 11px;
      color: #bbf7d0;
      background: rgba(22,163,74,0.18);
      display: inline-flex;
      align-items: center;
      gap: 4px;
    }

    [data-theme="light"] .tag {
      color: #166534;
      background: #dcfce7;
      border-color: #4ade80;
    }

    .chip {
      padding: 4px 9px;
      font-size: 11px;
      border-radius: 999px;
      border: 1px solid var(--border-subtle);
      background: var(--bg-elevated);
      color: var(--text-soft);
    }

    .theme-toggle {
      padding: 6px 11px;
      border-radius: 999px;
      border: 1px solid var(--border-subtle);
      background: var(--bg-elevated);
      color: var(--text-soft);
      font-size: 12px;
      display: inline-flex;
      align-items: center;
      gap: 6px;
      cursor: pointer;
      box-shadow: var(--shadow-main);
      transition: transform 0.15s ease, box-shadow 0.15s ease, background 0.2s ease;
    }

    .theme-toggle span.icon {
      font-size: 14px;
    }

    .theme-toggle:hover {
      transform: translateY(-1px);
      box-shadow: var(--shadow-hover);
    }

    .theme-toggle:active {
      transform: translateY(0);
      box-shadow: var(--shadow-main);
    }

    .stats {
      display: grid;
      grid-template-columns: repeat(4, minmax(0, 1fr));
      gap: 10px;
      margin-bottom: 16px;
    }

    .stat-card {
      background: var(--bg-card);
      border-radius: 14px;
      border: 1px solid var(--border-subtle);
      padding: 10px 12px;
      font-size: 12px;
      display: flex;
      flex-direction: column;
      gap: 4px;
      box-shadow: var(--shadow-main);
      backdrop-filter: var(--glass-blur);
      animation: slideUp 0.5s ease-out;
    }

    .stat-label {
      color: var(--text-soft);
    }

    .stat-row {
      display: flex;
      justify-content: space-between;
      align-items: baseline;
      gap: 6px;
    }

    .stat-value {
      font-size: 20px;
      font-weight: 600;
    }

    .stat-pip {
      font-size: 11px;
      padding: 3px 8px;
      border-radius: 999px;
      background: var(--accent-pill);
      color: var(--accent-soft);
      white-space: nowrap;
    }

    [data-theme="light"] .stat-pip {
      background: #dbeafe;
      color: #1d4ed8;
    }

    /* ======= your sketch layout ======= */

    /* 2x2 grid for control cards */
    .grid-2x2 {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 18px;
      margin-top: 6px;
      margin-bottom: 12px;
    }

    /* full-width cards (Pool stats + Activity log) */
    .full-width {
      margin-top: 10px;
    }

    .card {
      background: var(--bg-card);
      border-radius: 14px;
      border: 1px solid var(--border-subtle);
      padding: 14px 16px;
      box-shadow: var(--shadow-main);
      backdrop-filter: var(--glass-blur);
      transition: transform 0.18s ease, box-shadow 0.18s ease, border-color 0.18s ease;
      animation: slideUp 0.5s ease-out;
    }

    .card:hover {
      transform: translateY(-2px);
      border-color: #3b82f6aa;
      box-shadow: var(--shadow-hover);
    }

    .card-title {
      font-size: 14px;
      font-weight: 600;
      margin-bottom: 8px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 8px;
    }

    .card-sub {
      font-size: 11px;
      color: var(--text-soft);
      white-space: nowrap;
    }

    label {
      font-size: 12px;
      color: var(--text-soft);
      margin-bottom: 3px;
      display: block;
    }

    input, select {
      width: 100%;
      padding: 7px 9px;
      border-radius: 999px;
      border: 1px solid var(--border-subtle);
      background: var(--bg-soft);
      color: var(--text);
      font-size: 13px;
      outline: none;
      transition: border-color 0.15s ease, box-shadow 0.15s ease, background 0.15s ease, transform 0.1s ease;
    }

    [data-theme="light"] input,
    [data-theme="light"] select {
      background: #f9fafb;
    }

    input:focus, select:focus {
      border-color: var(--accent);
      box-shadow: 0 0 0 1px var(--accent-soft);
      background: #020617;
      transform: translateY(-1px);
    }

    [data-theme="light"] input:focus,
    [data-theme="light"] select:focus {
      background: #ffffff;
    }

    .field-row,
    .field-row-3 {
      display: grid;
      gap: 10px;
      margin-top: 6px;
      align-items: end;
    }

    .field-row {
      grid-template-columns: repeat(2, minmax(0, 1fr));
    }

    .field-row-3 {
      grid-template-columns: repeat(3, minmax(0, 1fr));
    }

    .btn-row {
      display: flex;
      gap: 8px;
      margin-top: 10px;
      flex-wrap: wrap;
    }

    button {
      border: none;
      border-radius: 999px;
      padding: 7px 14px;
      font-size: 13px;
      cursor: pointer;
      background: var(--accent);
      color: #e5e7eb;
      font-weight: 500;
      transition: background 0.15s ease, transform 0.1s ease, box-shadow 0.15s ease;
      box-shadow: 0 10px 20px rgba(37, 99, 235, 0.25);
      display: inline-flex;
      align-items: center;
      gap: 4px;
      white-space: nowrap;
    }

    button.secondary {
      background: var(--bg-soft);
      color: var(--text-soft);
      border: 1px solid var(--border-subtle);
      box-shadow: none;
    }

    button.danger {
      background: var(--danger);
      box-shadow: 0 10px 20px rgba(220, 38, 38, 0.25);
    }

    button:hover {
      transform: translateY(-1px);
      background: var(--accent-soft);
    }

    button.secondary:hover {
      background: var(--bg-elevated);
    }

    button.danger:hover {
      background: #b91c1c;
    }

    button:active {
      transform: translateY(0);
      box-shadow: none;
    }

    .status-text {
      font-size: 12px;
      margin-top: 6px;
      color: #a5b4fc;
    }

    [data-theme="light"] .status-text {
      color: #4f46e5;
    }

    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 12px;
      margin-top: 6px;
    }

    th, td {
      padding: 6px 8px;
      text-align: left;
      border-bottom: 1px solid var(--border-subtle);
    }

    th {
      color: var(--text-soft);
      font-weight: 500;
      font-size: 11px;
      text-transform: uppercase;
      letter-spacing: 0.04em;
    }

    tr:hover td {
      background: var(--bg-soft);
    }

    .log {
      margin-top: 6px;
      max-height: 170px;
      overflow-y: auto;
      font-size: 12px;
    }

    .log-line {
      padding: 3px 0;
      border-bottom: 1px dashed var(--border-subtle);
      color: var(--text-soft);
      display: flex;
      justify-content: space-between;
      gap: 6px;
    }

    .log-line span.time {
      color: #a855f7;
      font-size: 11px;
      white-space: nowrap;
    }

    .log-line span.msg {
      flex: 1;
      text-align: right;
    }

    @media (max-width: 900px) {
      body {
        padding: 12px 12px 24px;
      }
      .grid-2x2 {
        grid-template-columns: 1fr;
      }
      .stats {
        grid-template-columns: repeat(2, minmax(0, 1fr));
      }
      .header {
        flex-direction: column;
        align-items: flex-start;
      }
    }
  </style>
</head>
<body data-theme="dark">
<div class="app">
  <div class="header">
    <div class="title-group">
      <h1>ThreadLab</h1>
      <p>Scalable Thread Management Library · C++ ThreadPool · Live Monitoring</p>
      <div class="tag-row">
        <div class="tag">Backend: C++17 · Frontend: HTML + JS</div>
        <div class="chip">REST · JSON over HTTP</div>
      </div>
    </div>
    <button id="themeToggle" class="theme-toggle">
      <span class="icon">🌙</span>
      <span id="themeLabel">Dark mode</span>
    </button>
  </div>

  <!-- top summary cards -->
  <div class="stats">
    <div class="stat-card">
      <div class="stat-row">
        <div>
          <div class="stat-label">Total Pools</div>
          <div class="stat-value" id="statPools">0</div>
        </div>
        <div class="stat-pip">Instances</div>
      </div>
    </div>
    <div class="stat-card">
      <div class="stat-row">
        <div>
          <div class="stat-label">Active Threads</div>
          <div class="stat-value" id="statThreads">0</div>
        </div>
        <div class="stat-pip">Currently running</div>
      </div>
    </div>
    <div class="stat-card">
      <div class="stat-row">
        <div>
          <div class="stat-label">Queued Tasks</div>
          <div class="stat-value" id="statQueued">0</div>
        </div>
        <div class="stat-pip">Waiting</div>
      </div>
    </div>
    <div class="stat-card">
      <div class="stat-row">
        <div>
          <div class="stat-label">Completed Tasks</div>
          <div class="stat-value" id="statCompleted">0</div>
        </div>
        <div class="stat-pip">Since start</div>
      </div>
    </div>
  </div>

  <!-- ===== 2x2 controls grid (like your drawing) ===== -->
  <div class="grid-2x2">
    <!-- Create Thread Pool -->
    <div class="card">
      <div class="card-title">
        <span>Create Thread Pool</span>
        <span class="card-sub">Step 1 · Configure worker threads</span>
      </div>
      <div class="field-row">
        <div>
          <label for="threads">Threads</label>
          <input id="threads" type="number" min="1" value="2">
        </div>
        <div>
          <label for="poolName">Pool name (optional)</label>
          <input id="poolName" type="text" placeholder="e.g. FileWorker">
        </div>
      </div>
      <div class="btn-row">
        <button id="btnCreate">Create Pool</button>
      </div>
      <div class="status-text" id="createStatus">No pool created yet.</div>
    </div>

    <!-- Submit Tasks -->
    <div class="card">
      <div class="card-title">
        <span>Submit Tasks</span>
        <span class="card-sub">Step 2 · Push work into the queue</span>
      </div>
      <div class="field-row-3">
        <div>
          <label for="poolIdSubmit">Pool ID</label>
          <input id="poolIdSubmit" type="number" min="1" value="1">
        </div>
        <div>
          <label for="taskCount">Task count</label>
          <input id="taskCount" type="number" min="1" value="10">
        </div>
        <div>
          <label for="priority">Priority</label>
          <select id="priority">
            <option value="1" selected>Normal (1)</option>
            <option value="0">High (0)</option>
            <option value="2">Low (2)</option>
          </select>
        </div>
      </div>
      <div class="btn-row">
        <button id="btnSubmit">Submit Tasks</button>
        <button id="btnHeavy" class="secondary">Submit Heavy Tasks</button>
        <button id="btnStress" class="secondary">Stress Test (10,000)</button>
      </div>
      <div class="status-text" id="submitStatus">Waiting for submission…</div>
    </div>

    <!-- Shutdown Pool -->
    <div class="card">
      <div class="card-title">
        <span>Shutdown Pool</span>
        <span class="card-sub">Step 3 · Graceful termination</span>
      </div>
      <div class="field-row">
        <div>
          <label for="poolIdShutdown">Pool ID</label>
          <input id="poolIdShutdown" type="number" min="1" value="1">
        </div>
      </div>
      <div class="btn-row">
        <button id="btnShutdown" class="danger">Shutdown</button>
      </div>
      <div class="status-text" id="shutdownStatus">No shutdown requested.</div>
    </div>

    <!-- Parallel For Demo -->
    <div class="card">
      <div class="card-title">
        <span>Parallel For Demo</span>
        <span class="card-sub">Shows chunked parallel_for speed</span>
      </div>
      <div class="field-row">
        <div>
          <label for="pfN">N (range size)</label>
          <input id="pfN" type="number" min="1" value="100000">
        </div>
        <div>
          <label for="pfChunk">Chunk size</label>
          <input id="pfChunk" type="number" min="1" value="1000">
        </div>
      </div>
      <div class="btn-row">
        <button id="btnParallelDemo">Run Demo</button>
      </div>
      <div class="status-text" id="parallelStatus">
        Uses a temporary pool (4–16 threads) to sum 0..N-1 in parallel.
      </div>
    </div>
  </div>

  <!-- Full-width Pool Statistics -->
  <div class="card full-width">
    <div class="card-title">
      <span>Pool Statistics</span>
      <span class="card-sub">Auto-refresh every 2s</span>
    </div>

    <table>
      <thead>
      <tr>
        <th>Pool ID</th>
        <th>Name</th>
        <th>Total Threads</th>
        <th>Active</th>
        <th>Queued</th>
        <th>Completed</th>
        <th>Avg Task (ms)</th>
      </tr>
      </thead>
      <tbody id="poolTableBody">
      <tr><td colspan="7">No pools yet. Create one above.</td></tr>
      </tbody>
    </table>
  </div>

  <!-- Full-width Activity Log -->
  <div class="card full-width" style="margin-top:10px;">
    <div class="card-title">
      <span>Activity Log</span>
      <span class="card-sub">Recent actions in the system</span>
    </div>
    <div class="log" id="log"></div>
  </div>
</div>

<script>
  // ===== Theme toggle (dark / light) =====
  (function() {
    const body = document.body;
    const toggleBtn = document.getElementById("themeToggle");
    const label = document.getElementById("themeLabel");
    const stored = localStorage.getItem("threadlab-theme") || "dark";

    function applyTheme(theme) {
      body.setAttribute("data-theme", theme);
      if (theme === "dark") {
        toggleBtn.querySelector(".icon").textContent = "🌙";
        label.textContent = "Dark mode";
      } else {
        toggleBtn.querySelector(".icon").textContent = "☀️";
        label.textContent = "Light mode";
      }
    }

    applyTheme(stored);

    toggleBtn.addEventListener("click", () => {
      const current = body.getAttribute("data-theme") || "dark";
      const next = current === "dark" ? "light" : "dark";
      localStorage.setItem("threadlab-theme", next);
      applyTheme(next);
    });
  })();

  // ===== Shared helpers =====
  const logEl = document.getElementById("log");
  function addLog(message) {
    const line = document.createElement("div");
    line.className = "log-line";
    const time = new Date().toLocaleTimeString();
    line.innerHTML =
      `<span class="time">[${time}]</span><span class="msg">${message}</span>`;
    logEl.prepend(line);
  }

  async function fetchJSON(url, options) {
    try {
      const res = await fetch(url, options || {});
      const text = await res.text();
      let data = null;
      try { data = JSON.parse(text); } catch (e) { data = null; }
      return { ok: res.ok, status: res.status, data, raw: text };
    } catch (err) {
      return { ok: false, status: 0, data: null, raw: "" };
    }
  }

  // ===== Create pool =====
  document.getElementById("btnCreate").addEventListener("click", async () => {
    const threads = document.getElementById("threads").value || "0";
    const name = document.getElementById("poolName").value || "DefaultPool";
    const statusEl = document.getElementById("createStatus");
    statusEl.textContent = "Creating pool...";

    const result = await fetchJSON("/api/pools?threads=" + encodeURIComponent(threads), {
      method: "POST"
    });

    if (result.ok && result.data && result.data.pool_id !== undefined) {
      const data = result.data;
      statusEl.textContent =
        `Created pool "${name}" with ID ${data.pool_id} (${data.threads} threads).`;
      addLog(`Created pool "${name}" with ID ${data.pool_id} (${data.threads} threads).`);
      refreshStats();
    } else {
      statusEl.textContent =
        "Error: " + ((result.data && result.data.error) || "Failed to create pool");
      addLog("Error creating pool.");
    }
  });

  function getPriorityValue() {
    return document.getElementById("priority").value || "1";
  }

  // ===== Submit tasks =====
  document.getElementById("btnSubmit").addEventListener("click", async () => {
    const poolId = document.getElementById("poolIdSubmit").value;
    const count = document.getElementById("taskCount").value || "1";
    const priority = getPriorityValue();
    const statusEl = document.getElementById("submitStatus");

    if (!poolId) {
      statusEl.textContent = "Please enter a pool ID.";
      return;
    }

    statusEl.textContent = "Submitting tasks...";

    const url = "/api/pools/" + encodeURIComponent(poolId) +
                "/tasks?count=" + encodeURIComponent(count) +
                "&priority=" + encodeURIComponent(priority);

    const result = await fetchJSON(url, { method: "POST" });

    if (result.ok && result.data && result.data.tasks_added !== undefined) {
      statusEl.textContent =
        `Queued ${result.data.tasks_added} tasks to pool ${poolId} (priority=${priority}).`;
      addLog(`Queued ${result.data.tasks_added} tasks (priority=${priority}) to pool ${poolId}.`);
      refreshStats();
    } else {
      statusEl.textContent =
        "Error: " + ((result.data && result.data.error) || "Failed to submit tasks");
      addLog("Error submitting tasks.");
    }
  });

  document.getElementById("btnHeavy").addEventListener("click", async () => {
    const poolId = document.getElementById("poolIdSubmit").value;
    const priority = getPriorityValue();
    const statusEl = document.getElementById("submitStatus");

    if (!poolId) {
      statusEl.textContent = "Please enter a pool ID.";
      return;
    }

    statusEl.textContent = "Submitting heavy tasks (100)...";

    const url = "/api/pools/" + encodeURIComponent(poolId) +
                "/tasks?count=100&priority=" + encodeURIComponent(priority);

    const result = await fetchJSON(url, { method: "POST" });

    if (result.ok && result.data && result.data.tasks_added !== undefined) {
      statusEl.textContent =
        `Submitted ${result.data.tasks_added} heavy tasks to pool ${poolId} (priority=${priority}).`;
      addLog(`Submitted ${result.data.tasks_added} heavy tasks (priority=${priority}) to pool ${poolId}.`);
      refreshStats();
    } else {
      statusEl.textContent =
        "Error: " + ((result.data && result.data.error) || "Failed to submit heavy tasks");
      addLog("Error submitting heavy tasks.");
    }
  });

  document.getElementById("btnStress").addEventListener("click", async () => {
    const poolId = document.getElementById("poolIdSubmit").value;
    const priority = getPriorityValue();
    const statusEl = document.getElementById("submitStatus");

    if (!poolId) {
      statusEl.textContent = "Please enter a pool ID for stress test.";
      return;
    }

    statusEl.textContent = "Starting stress test (10,000 tasks)...";

    const url = "/api/pools/" + encodeURIComponent(poolId) +
                "/tasks?count=10000&priority=" + encodeURIComponent(priority);

    const result = await fetchJSON(url, { method: "POST" });

    if (result.ok && result.data && result.data.tasks_added !== undefined) {
      statusEl.textContent =
        `Stress test started: ${result.data.tasks_added} tasks queued on pool ${poolId} (priority=${priority}).`;
      addLog(`Stress test: ${result.data.tasks_added} tasks (priority=${priority}) queued on pool ${poolId}.`);
      refreshStats();
    } else {
      statusEl.textContent =
        "Error: " + ((result.data && result.data.error) || "Failed to start stress test");
      addLog("Error starting stress test.");
    }
  });

  // ===== Shutdown =====
  document.getElementById("btnShutdown").addEventListener("click", async () => {
    const poolId = document.getElementById("poolIdShutdown").value;
    const statusEl = document.getElementById("shutdownStatus");

    if (!poolId) {
      statusEl.textContent = "Please enter a pool ID.";
      return;
    }

    statusEl.textContent = "Shutting down pool...";

    const result = await fetchJSON(
      "/api/pools/" + encodeURIComponent(poolId) + "/shutdown",
      { method: "POST" }
    );

    if (result.ok && result.data && result.data.status === "shutdown") {
      statusEl.textContent = `Pool ${poolId} shutdown.`;
      addLog(`Pool ${poolId} shutdown.`);
      refreshStats();
    } else {
      statusEl.textContent =
        "Error: " + ((result.data && result.data.error) || "Failed to shutdown pool");
      addLog("Error shutting down pool.");
    }
  });

  // ===== parallel_for demo =====
  document.getElementById("btnParallelDemo").addEventListener("click", async () => {
    const N = document.getElementById("pfN").value || "100000";
    const chunk = document.getElementById("pfChunk").value || "1000";
    const statusEl = document.getElementById("parallelStatus");

    statusEl.textContent = "Running parallel_for demo...";

    const url = "/api/parallel_for_demo?N=" + encodeURIComponent(N) +
                "&chunk=" + encodeURIComponent(chunk);

    const result = await fetchJSON(url, { method: "POST" });

    if (result.ok && result.data && result.data.status === "ok") {
      statusEl.textContent =
        `parallel_for sum[0..${result.data.N - 1}] = ${result.data.sum} in ${result.data.ms} ms (chunk=${result.data.chunk}).`;
      addLog(`parallel_for demo: N=${result.data.N}, chunk=${result.data.chunk}, ms=${result.data.ms}.`);
    } else {
      statusEl.textContent =
        "Error running parallel_for demo.";
      addLog("Error running parallel_for demo.");
    }
  });

  // ===== Stats refresh =====
  async function refreshStats() {
    const tbody = document.getElementById("poolTableBody"); // FIXED ID

    try {
      const res = await fetch("/api/pools");
      if (!res.ok) {
        tbody.innerHTML = "<tr><td colspan='7'>Error loading stats.</td></tr>";
        return;
      }
      const data = await res.json();

      tbody.innerHTML = "";

      if (!Array.isArray(data) || data.length === 0) {
        tbody.innerHTML = "<tr><td colspan='7'>No pools yet. Create one above.</td></tr>";
        document.getElementById("statPools").textContent = 0;
        document.getElementById("statThreads").textContent = 0;
        document.getElementById("statQueued").textContent = 0;
        document.getElementById("statCompleted").textContent = 0;
        return;
      }

      let totalPools = data.length;
      let totalThreads = 0, totalQueued = 0, totalCompleted = 0, totalActive = 0;

      data.forEach(function(item) {
        totalThreads += item.total_threads;
        totalQueued += item.queued_tasks;
        totalCompleted += item.completed_tasks;
        totalActive += item.active_threads;

        const tr = document.createElement("tr");
        tr.innerHTML =
          "<td>" + item.id + "</td>" +
          "<td>" + ("Pool " + item.id) + "</td>" +
          "<td>" + item.total_threads + "</td>" +
          "<td>" + item.active_threads + "</td>" +
          "<td>" + item.queued_tasks + "</td>" +
          "<td>" + item.completed_tasks + "</td>" +
          "<td>" + item.avg_task_ms.toFixed(3) + "</td>";
        tbody.appendChild(tr);
      });

      document.getElementById("statPools").textContent = totalPools;
      document.getElementById("statThreads").textContent = totalThreads;
      document.getElementById("statQueued").textContent = totalQueued;
      document.getElementById("statCompleted").textContent = totalCompleted;

    } catch (e) {
      tbody.innerHTML = "<tr><td colspan='7'>Error loading stats.</td></tr>";
    }
  }

  setInterval(refreshStats, 2000);
  window.onload = refreshStats;
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

    // Serve the dashboard UI
    svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(build_dashboard_html(), "text/html; charset=UTF-8");
    });

    // Create pool: POST /api/pools?threads=8
    svr.Post("/api/pools", [&](const httplib::Request& req, httplib::Response& res) {
        auto threadsParam = req.get_param_value("threads");
        if (threadsParam.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"threads param required\"}", "application/json");
            return;
        }

        int threads = 0;
        try {
            threads = std::stoi(threadsParam);
        } catch (...) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid threads value\"}", "application/json");
            return;
        }

        if (threads <= 0) {
            res.status = 400;
            res.set_content("{\"error\":\"threads must be > 0\"}", "application/json");
            return;
        }

        int id = manager.createPool(static_cast<size_t>(threads));

        std::ostringstream oss;
        oss << "{"
            << "\"pool_id\":" << id << ","
            << "\"threads\":" << threads
            << "}";
        res.set_content(oss.str(), "application/json");
    });

    // List all pool stats: GET /api/pools
    svr.Get("/api/pools", [&](const httplib::Request&, httplib::Response& res) {
        auto allStats = manager.getAllStats();
        std::ostringstream oss;
        oss << "[";

        bool first = true;
        for (auto &entry : allStats) {
            if (!first) oss << ",";
            first = false;
            int id = entry.first;
            const ThreadStats &s = entry.second;
            oss << "{"
                << "\"id\":" << id << ","
                << "\"total_threads\":" << s.totalThreads << ","
                << "\"active_threads\":" << s.activeThreads << ","
                << "\"queued_tasks\":" << s.queuedTasks << ","
                << "\"completed_tasks\":" << s.completedTasks << ","
                << "\"avg_task_ms\":" << s.avgTaskMs
                << "}";
        }

        oss << "]";
        res.set_content(oss.str(), "application/json");
    });

    // Add tasks: POST /api/pools/<id>/tasks?count=100[&priority=0|1|2]
    svr.Post(R"(/api/pools/(\d+)/tasks)", [&](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1].str());
        auto pool = manager.getPool(id);
        if (!pool) {
            res.status = 404;
            res.set_content("{\"error\":\"pool not found\"}", "application/json");
            return;
        }

        auto countParam = req.get_param_value("count");
        int count = 1;
        if (!countParam.empty()) {
            try {
                count = std::stoi(countParam);
            } catch (...) {
                res.status = 400;
                res.set_content("{\"error\":\"invalid count\"}", "application/json");
                return;
            }
        }
        if (count <= 0) count = 1;

        auto prioParam = req.get_param_value("priority");
        int priority = 1; // normal
        if (!prioParam.empty()) {
            try {
                priority = std::stoi(prioParam);
            } catch (...) {
                priority = 1;
            }
        }

        for (int i = 0; i < count; ++i) {
            pool->submitWithPriority(priority, simulateHeavyTask, i);
        }

        std::ostringstream oss;
        oss << "{"
            << "\"status\":\"queued\","
            << "\"tasks_added\":" << count
            << "}";
        res.set_content(oss.str(), "application/json");
    });

    // Shutdown a pool: POST /api/pools/<id>/shutdown
    svr.Post(R"(/api/pools/(\d+)/shutdown)", [&](const httplib::Request& req, httplib::Response& res) {
        int id = std::stoi(req.matches[1].str());
        auto pool = manager.getPool(id);
        if (!pool) {
            res.status = 404;
            res.set_content("{\"error\":\"pool not found\"}", "application/json");
            return;
        }

        manager.shutdownPool(id);
        res.set_content("{\"status\":\"shutdown\"}", "application/json");
    });

    // Parallel_for demo: POST /api/parallel_for_demo?N=100000&chunk=1000
    svr.Post("/api/parallel_for_demo", [&](const httplib::Request& req, httplib::Response& res) {
        auto Nparam = req.get_param_value("N");
        auto Cparam = req.get_param_value("chunk");

        long long N = 100000;
        long long chunk = 1000;

        try {
            if (!Nparam.empty()) N = std::stoll(Nparam);
            if (!Cparam.empty()) chunk = std::stoll(Cparam);
        } catch (...) {
            // keep defaults
        }

        if (N <= 0) N = 1;
        if (chunk <= 0) chunk = 1;

        ThreadPool testPool(4, 16);
        std::atomic<long long> sum{0};

        auto start = std::chrono::steady_clock::now();
        parallel_for(0LL, N, [&sum](long long i) {
            sum += i;
        }, testPool, chunk);
        auto end = std::chrono::steady_clock::now();

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        testPool.shutdown();

        std::ostringstream oss;
        oss << "{"
            << "\"status\":\"ok\","
            << "\"N\":" << N << ","
            << "\"chunk\":" << chunk << ","
            << "\"sum\":" << sum.load() << ","
            << "\"ms\":" << ms
            << "}";
        res.set_content(oss.str(), "application/json");
    });

    std::cout << "=========================================\n";
    std::cout << "  Project: Scalable Thread Management\n";
    std::cout << "  Backend: C++17 Thread Pool + REST API\n";
    std::cout << "  Frontend: Web Dashboard (HTML + JS)\n";
    std::cout << "=========================================\n";
    std::cout << "Open your browser at: http://localhost:8080\n";

    // Start HTTP server on localhost:8080
    svr.listen("0.0.0.0", 8080);

    // On exit, shutdown all pools
    manager.shutdownAll();
    return 0;
}

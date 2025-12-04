# 🧵 Scalable Thread Management Library (ThreadLab)
A fully functional **C++17-based Scalable ThreadPool Management System** with a live **Web Dashboard UI**.  
This project demonstrates modern concurrency, task scheduling, auto-scaling thread pools, REST APIs, and a dynamic frontend — all in a **single C++ file**.

---

## 🚀 Features

### 🔧 Backend — C++17 ThreadPool Engine  
- Dynamic thread pool creation  
- Auto-scaling worker threads  
- Priority scheduling (High / Normal / Low)  
- High-performance job queue  
- Graceful shutdown  
- Live metrics (active, queued, completed tasks)  
- Chunk-based parallel_for demo for benchmarking  

### 🌐 Frontend — Web Dashboard  
- Fully responsive UI  
- 2×2 grid control panel (Create Pool, Submit Tasks, Shutdown, Demo)  
- Live statistics auto-refresh (every 2 seconds)  
- Activity Log with timestamps  
- Light/Dark mode toggle  
- Smooth UI animations  

### 🛠 API (REST - JSON over HTTP)
Endpoints:
POST /api/pools?threads=N
GET /api/pools
POST /api/pools/<id>/tasks?count=N&priority=P
POST /api/pools/<id>/shutdown
POST /api/parallel_for_demo?N=X&chunk=Y

---

## 📁 Project Structure

threadlab_web.cpp → Main C++ backend + embedded HTML + JS frontend
httplib.h → Header-only HTTP server library (required)
README.md → Project documentation

> ⚠️ Everything runs from a **single .cpp file** — easy to compile and perfect for university projects.

---

## 🔧 How to Build on Windows (MinGW)

### 1️⃣ Compile
```bash
g++ threadlab_web.cpp -std=c++17 -O2 -lws2_32 -o threadlab_web.exe
./threadlab_web.exe

Open dashboard in browser
http://localhost:8080


📸 Dashboard Preview
<img width="1337" height="630" alt="image" src="https://github.com/user-attachments/assets/4b9706fa-91c3-41ef-b691-d4befa66f2bf" />


🧪 Usage Guide
✔ Create a thread pool

Enter number of threads → Create Pool

✔ Submit tasks

Choose:

Task Count

Priority (High/Normal/Low)

Pool ID

Click → Submit Tasks

✔ Stress Testing

Submit 10,000 jobs

Tests dynamic scaling & queue handling

✔ Parallel For Demo

Runs sum(0..N-1) using a temporary pool
Shows speedup with chunking.

.

📝 Example Output Logs
[11:52:26 AM] Stress test: 10000 tasks queued on pool 2.
[11:52:19 AM] parallel_for demo: N=100000, chunk=1000, ms=1.
[11:51:59 AM] Created pool "DefaultPool" with ID 5 (2 threads).


💡 Why This Project?

This project demonstrates key OS + Systems concepts:

Multi-threading
Task scheduling
Real-time monitoring
REST architecture
Web UI integration
Auto-scaling thread pools

Perfect for:

Operating Systems assignment
Computer Architecture
C++ concurrency learning
Systems design practice

⭐ If you like this project

Give the repository a Star ⭐ on GitHub!

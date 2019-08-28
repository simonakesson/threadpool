#pragma once

#include <thread>
#include <mutex>
#include <vector>
#include <queue>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>

class ThreadPool {
public:
    explicit ThreadPool(size_t n_threads = std::thread::hardware_concurrency()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (size_t i = 0; i < n_threads; ++i) {
            m_threads.emplace_back([this]{ work(); });
        }
    }

    template<class F, class ...Args>
    auto push(F&& f, Args&&... args) -> std::future<decltype(f(args...))> {
        using return_type = decltype(f(args...));
        auto f_bound = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        auto task_ptr = std::make_shared<std::packaged_task<return_type()>>(std::move(f_bound));
        auto future = task_ptr->get_future();
        // std::function requires a CopyConstructible Callable, which std::packaged_task is not.
        // It's possible to solve this using move capture from C++14. But to keep C++11 compatibility,
        // here's a workaround using std::shared_ptr, which is CopyConstructible.
        std::function<void()> void_wrapper = [task_ptr]{ (*task_ptr)(); };
        std::unique_lock<std::mutex> lock(m_mutex);
        m_tasks.push(std::move(void_wrapper));
        lock.unlock();
        m_task_cv.notify_one();
        return future;
    }

    void join() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_stop = true;
        lock.unlock();
        m_task_cv.notify_all();
        for (std::thread& t : m_threads) {
            t.join();
        }
    }

    /**
     * Blocks until all pushed tasks have been completed. Threads are kept alive.
     */
    void wait() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_idle_cv.wait(lock, [this]{ return m_tasks.empty() && m_idle_count == m_threads.size(); });
        std::cout << "wait(): " << m_idle_count << " " << m_threads.size() << std::endl;
    }

private:
    void work() {
        while (true) {
            std::unique_lock<std::mutex> lock(m_mutex);
            
            if (++m_idle_count == m_threads.size()) {
                std::cout << "work(): " << m_idle_count << " " << m_threads.size() << std::endl;
                m_idle_cv.notify_one();
            }
            m_task_cv.wait(lock, [this]{ return !m_tasks.empty() || m_stop; });
            --m_idle_count;

            if (m_stop && m_tasks.empty()) {
                lock.unlock();
                return;
            }
        
            auto task = std::move(m_tasks.front());
            m_tasks.pop();
            lock.unlock();
            m_task_cv.notify_one();
            task();
        }
    }

    std::queue<std::function<void()>> m_tasks;
    std::mutex m_mutex;
    std::condition_variable m_task_cv;  // Synchronizes the scheduling and acquisition of tasks
    std::condition_variable m_idle_cv;  // Used to signal the wait method when all threads are idle
    size_t m_idle_count;
    bool m_stop = false;
    std::vector<std::thread> m_threads;
};

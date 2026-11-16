#pragma once

#include "Function.h"
#include <armonik/common/logger/formatter.h>
#include <armonik/common/logger/logger.h>
#include <armonik/common/logger/writer.h>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace ArmoniK {
namespace Sdk {
namespace Client {
namespace Internal {
/**
 * @brief A thread pool to execute tasks in background
 */
class ThreadPool {
public:
  /**
   * @brief A join set to wait for a set of tasks to finish
   */
  class JoinSet;

private:
  /**
   * @brief A task to execute
   */
  class Task {
  private:
    /**
     * @brief The function to execute
     */
    Function<void()> func_;

    /**
     * @brief The join set this task belongs to, optional
     */
    JoinSet *join_set_ = nullptr;

    /**
     * @brief Count this task as finished in its join set, if any, and detach it from the join set
     */
    void ReleaseJoinSet() noexcept;

  private:
    friend class ThreadPool;

  public:
    /**
     * @brief Default constructor
     */
    Task();

    /**
     * @brief Creates a task with the given function and optional join set
     */
    Task(Function<void()> &&func, JoinSet *join_set = nullptr);

    /**
     * @brief Copy constructor
     */
    Task(const Task &) = delete;

    /**
     * @brief Copy assignment operator
     */
    Task &operator=(const Task &) = delete;

    /**
     * @brief Move constructor
     */
    Task(Task &&other) noexcept;

    /**
     * @brief Move assignment operator
     */
    Task &operator=(Task &&other) noexcept;

    /**
     * @brief Destroy the Task object, updating the join set if applicable
     */
    ~Task();

    /**
     * @brief Execute the task
     */
    void Execute(armonik::api::common::logger::ILogger &logger);

    /**
     * @brief Record current error for the join set
     */
    void RecordError();
  };

private:
  /**
   * @brief The maximum number of threads in the pool
   */
  std::size_t max_threads_;

  /**
   * @brief The number of sleeping threads
   */
  std::size_t sleeping_threads_;

  /**
   * @brief Mutex to protect the pool
   */
  std::mutex mutex_;

  /**
   * @brief Condition variable to notify threads of new tasks
   */
  std::condition_variable condition_;

  /**
   * @brief Logger
   */
  armonik::api::common::logger::Logger &logger_;

  /**
   * @brief The threads in the pool
   */
  std::vector<std::thread> threads_;

  /**
   * @brief The pending tasks waiting to be executed by the pool
   */
  std::queue<Task> pending_tasks_;

  /**
   * @brief Flag to stop the pool
   */
  bool stop_;

private:
  /**
   * @brief Create a local logger for the thread pool
   */
  armonik::api::common::logger::LocalLogger Logger(armonik::api::common::logger::Context context = {});

  /**
   * @brief The main loop for each thread
   */
  void Run();

  /**
   * @brief Spawn a task on the pool
   *
   * @param f The task to execute
   */
  void Spawn(Task &&);

public:
  /**
   * @brief Creates a thread pool
   * @param max_threads Maximum number of threads
   * @param logger Logger
   */
  explicit ThreadPool(int max_threads, armonik::api::common::logger::Logger &logger);

  /**
   * @brief Copy constructor
   *
   * @param other Other thread pool
   */
  ThreadPool(const ThreadPool &) = delete;

  /**
   * @brief Copy operator
   *
   * @param other Other thread pool
   */
  ThreadPool &operator=(const ThreadPool &) = delete;

  /**
   * @brief Destroy the thread Pool object, waiting for all threads to finish
   *
   */
  ~ThreadPool();

  /**
   * @brief Spawn a task on the pool
   *
   * @param f The task to execute
   */
  void Spawn(Function<void()> &&f);

  /**
   * @brief Whether the calling thread is currently executing a task dispatched by (any) ThreadPool.
   * Used to reject reentrant calls that would block waiting for pool capacity or resources released by
   * pool work, which can deadlock a bounded pool.
   */
  static bool IsWorkerThread();
};

/**
 * @brief A join set to wait for a set of tasks to finish
 */
class ThreadPool::JoinSet {
private:
  friend class ThreadPool;

private:
  /**
   * @brief The thread pool on which tasks are spawned
   */
  ThreadPool &thread_pool_;

  /**
   * @brief The number of unfinished tasks in the join set
   */
  std::size_t task_count_;

  /**
   * @brief The exception that occurred in any task of the join set, if any
   */
  std::exception_ptr exception_;

  /**
   * @brief Mutex to protect the join set
   */
  std::mutex mutex_;

  /**
   * @brief Condition variable to wait for tasks to finish
   */
  std::condition_variable wake_condition_;

private:
  /**
   * @brief Create a local logger for the join set
   */
  armonik::api::common::logger::LocalLogger Logger(armonik::api::common::logger::Context context = {});

public:
  /**
   * @brief Creates a join set on the given thread pool
   *
   * @param thread_pool The thread pool
   */
  explicit JoinSet(ThreadPool &);

  /**
   * @brief Copy constructor
   */
  JoinSet(const JoinSet &) = delete;

  /**
   * @brief Copy assignment operator
   */
  JoinSet &operator=(const JoinSet &) = delete;

  /**
   * @brief Destroy the Join Set object, waiting for all tasks to finish
   * @note Ignore all exceptions that could have been thrown by the tasks
   */
  ~JoinSet();

  /**
   * @brief Spawn a task on the associated thread pool and add it to the join set
   *
   * @param f The task to execute
   */
  void Spawn(Function<void()> &&f);

  /**
   * @brief Wait for all tasks in the join set to finish
   * @throw If any task has thrown, the exception is thrown by Wait()
   * @note In case of an exception, it does not block until all tasks have finished
   */
  void Wait();
};

} // namespace Internal
} // namespace Client
} // namespace Sdk
} // namespace ArmoniK

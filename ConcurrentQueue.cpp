#include <cmath>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <future>

#include "TestRunner.h"
#include "Profile.h"

using namespace std::chrono_literals;

template<typename T, int64_t SIZE = 4096>
class ConcurrentQueue {
private:
  static constexpr unsigned Log2(unsigned n, unsigned p = 0) {
    return (n <= 1) ? p : Log2(n >> 1, p + 1);
  }

  static constexpr int64_t closestExponentOf2(int64_t x) {
    return (1UL << ((int64_t)(Log2(SIZE - 1)) + 1));
  }

  static constexpr int64_t mRingModMask = closestExponentOf2(SIZE) - 1;
  static constexpr int64_t mSize = closestExponentOf2(SIZE);

  static const T mEmpty;

  T mMem[mSize];
  std::atomic_flag mFlag = ATOMIC_FLAG_INIT;
  std::mutex mLock;
  int64_t mReadPtr = 0;
  int64_t mWritePtr = 0;
  int64_t mCount = 0;
  std::condition_variable mCond;

  void incrementWritePtr() {
    while (mFlag.test_and_set(std::memory_order_acquire));
    ++mCount;
    mWritePtr = ++mWritePtr & mRingModMask;
    mFlag.clear(std::memory_order_release);
  }

public:
  const T& front() {
    if (!peek()) {
      return mEmpty;
    }
    return mMem[mReadPtr];
  }

  void pop() {
    if (!peek()) {
      return;
    }
    while (mFlag.test_and_set(std::memory_order_acquire));
    --mCount;
    mReadPtr = ++mReadPtr & mRingModMask;
    mFlag.clear(std::memory_order_release);
    mCond.notify_one();
  }

  int64_t getCount() const {
    return mCount;
  }

  bool peek() const {
    return mCount > 0;
  }

  bool push(const T& pItem) {
    std::unique_lock<std::mutex> lock(mLock);
    mCond.wait_for(lock, 100ms, [&]() {
      return mCount <= mSize;
    });
    if (mCount >= mSize) {
      return false;
    }
    mMem[mWritePtr] = pItem;
    incrementWritePtr();
    return true;
  }

  bool push(T&& pItem) {
    std::unique_lock<std::mutex> lock(mLock);
    mCond.wait_for(lock, 100ms, [&]() {
      return mCount < mSize;
    });
    if (mCount >= mSize) {
      return false;
    }
    mMem[mWritePtr] = std::move(pItem);
    incrementWritePtr();
    return true;
  }
};

template<typename T, int64_t SIZE>
const T ConcurrentQueue<T, SIZE>::mEmpty = T{ };

void TestOnlyOneConsumer() {
  using Functor = std::function<void()>;

  ConcurrentQueue<std::shared_ptr<Functor>> queue;
  bool no_errors = true;
  std::thread consumer([&] {
    int counter = 1000;
    while (counter) {
      no_errors = (queue.getCount() == 0);
      --counter;
    }
  });

  consumer.join();
  ASSERT(no_errors);
}

void TestOnlyOneProducer() {
  using Functor = std::function<void()>;

  ConcurrentQueue<std::shared_ptr<Functor>> queue;

  bool no_errors = true;
  std::thread producer([&] {
    int64_t counter = 0;
    while (true) {
      auto taskId = ++counter;
      if (queue.getCount() < 4096) {
        queue.push(std::make_unique<Functor>([=] {
          std::cout << "Running task " << taskId << std::endl;
        }));
      }
      else {
        // Check that after push fails
        no_errors = !queue.push(std::make_unique<Functor>([=] {
          std::cout << "Running task " << taskId << std::endl;
        }));
        return;
      }
    }
  });

  producer.join();
  ASSERT(no_errors);
}

void TestOneConsumerOneProducer() {
  using Functor = std::function<int()>;

  ConcurrentQueue<std::shared_ptr<Functor>> queue;

  bool no_writes = false;
  bool no_errors = true;
  std::thread consumer([&] {
    int counter = 0;
    while (!no_writes || queue.peek()) {
      if (queue.peek()) {
        auto task = queue.front();
        no_errors = ++counter == (*task)();
        if (!no_errors) {
          std::cout << counter << " " << (*task)() << std::endl;
        }
        queue.pop();
      }
    }
  });

  std::thread producer([&] {
    int64_t counter = 0;
    while (counter < 10000 && no_errors) {
      int64_t taskId = ++counter;
      queue.push(std::make_shared<Functor>([taskId] {
        return taskId;
      }));
    }
    no_writes = true;
  });

  consumer.join();
  producer.join();
  ASSERT(no_errors);
}

void TestOneConsumerManyProducer() {
  using Functor = std::function<int()>;

  ConcurrentQueue<std::shared_ptr<Functor>, 32> queue;

  bool no_writes = false;
  int32_t tasks_consumed = 0;
  std::thread consumer([&] {
    while (!no_writes || queue.peek()) {
      if (queue.peek()) {
        queue.pop();
        ++tasks_consumed;
      }
    }
  });

  int32_t tasks_per_thread = 10000;
  std::vector<std::future<void>> futures(10);
  for (size_t i = 0; i < 10; ++i) {
    futures[i] = std::async([&queue, i, &tasks_per_thread] {
      int64_t counter = 0;
      while (counter < tasks_per_thread) {
        auto taskId = ++counter + tasks_per_thread * i;
        queue.push(std::make_unique<Functor>([taskId] {
          return taskId;
        }));
      }
    });
  }

  for (auto& f : futures) {
    f.get();
  }
  no_writes = true;
  consumer.join();
  ASSERT_EQUAL(tasks_consumed, 10 * tasks_per_thread);
}

int main() {
  TestRunner tr;
  for (int i = 0; i < 100; ++i) {
    RUN_TEST(tr, TestOnlyOneConsumer);
    RUN_TEST(tr, TestOnlyOneProducer);
    RUN_TEST(tr, TestOneConsumerOneProducer);
    RUN_TEST(tr, TestOneConsumerManyProducer);
  }
  return 0;
}

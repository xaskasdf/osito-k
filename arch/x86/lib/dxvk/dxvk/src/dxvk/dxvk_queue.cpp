#include "dxvk_device.h"
#include "dxvk_queue.h"

extern "C" int printf(const char*, ...);

namespace dxvk {
  
  DxvkSubmissionQueue::DxvkSubmissionQueue(DxvkDevice* device, const DxvkQueueCallback& callback)
  : m_device(device), m_callback(callback),
    m_submitThread([this] () { submitCmdLists(); }),
    m_finishThread([this] () { finishCmdLists(); }) {
    Logger::warn("[DXVKq] submission queue threads created");
    printf("[DXVKq] submission queue threads created\n");
  }
  
  
  DxvkSubmissionQueue::~DxvkSubmissionQueue() {
    auto vk = m_device->vkd();

    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      m_stopped.store(true);
    }

    m_appendCond.notify_all();
    m_submitCond.notify_all();

    m_submitThread.join();
    m_finishThread.join();
  }
  
  
  void DxvkSubmissionQueue::submit(DxvkSubmitInfo submitInfo, DxvkSubmitStatus* status) {
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    m_finishCond.wait(lock, [this] {
      return m_submitQueue.size() + m_finishQueue.size() <= MaxNumQueuedCommandBuffers;
    });

    DxvkSubmitEntry entry = { };
    entry.status = status;
    entry.submit = std::move(submitInfo);

    m_submitQueue.push(std::move(entry));
    m_appendCond.notify_all();
  }


  void DxvkSubmissionQueue::present(DxvkPresentInfo presentInfo, DxvkSubmitStatus* status) {
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    DxvkSubmitEntry entry = { };
    entry.status  = status;
    entry.present = std::move(presentInfo);

    m_submitQueue.push(std::move(entry));
    m_appendCond.notify_all();
  }


  void DxvkSubmissionQueue::synchronizeSubmission(
          DxvkSubmitStatus*   status) {
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    m_submitCond.wait(lock, [status] {
      return status->result.load() != VK_NOT_READY;
    });
  }


  void DxvkSubmissionQueue::synchronize() {
    std::unique_lock<dxvk::mutex> lock(m_mutex);

    m_submitCond.wait(lock, [this] {
      return m_submitQueue.empty();
    });
  }


  void DxvkSubmissionQueue::waitForIdle() {
#ifdef __OSITO_K__
    printf("[DXVKq] waitForIdle lock begin\n");
    std::unique_lock<dxvk::mutex> lock(m_mutex, std::defer_lock);

    for (uint32_t i = 0; !lock.owns_lock() && i < 100000; i++) {
      if (lock.try_lock())
        break;

      if (i == 0)
        printf("[DXVKq] waitForIdle lock busy\n");

      if ((i & 0xff) == 0)
        dxvk::this_thread::yield();
    }

    if (!lock.owns_lock()) {
      Logger::warn("[DXVKq] waitForIdle lock timeout; continuing on OsitoK");
      printf("[DXVKq] waitForIdle lock timeout; continuing\n");
      return;
    }
#else
    std::unique_lock<dxvk::mutex> lock(m_mutex);
#endif

    Logger::warn(str::format("[DXVKq] waitForIdle enter submit=",
      m_submitQueue.size(), " finish=", m_finishQueue.size()));
    printf("[DXVKq] waitForIdle enter submit=%llu finish=%llu\n",
      (unsigned long long)m_submitQueue.size(),
      (unsigned long long)m_finishQueue.size());

    bool submitted = m_submitCond.wait_for(lock, std::chrono::seconds(2), [this] {
      return m_submitQueue.empty();
    });

    if (!submitted) {
      Logger::warn(str::format("[DXVKq] waitForIdle submit timeout; continuing submit=",
        m_submitQueue.size(), " finish=", m_finishQueue.size(),
        " last=", m_lastError.load()));
      printf("[DXVKq] waitForIdle submit timeout; continuing submit=%llu finish=%llu last=%d\n",
        (unsigned long long)m_submitQueue.size(),
        (unsigned long long)m_finishQueue.size(),
        (int)m_lastError.load());
      return;
    }

    Logger::warn(str::format("[DXVKq] waitForIdle submit-drained finish=",
      m_finishQueue.size()));
    printf("[DXVKq] waitForIdle submit-drained finish=%llu\n",
      (unsigned long long)m_finishQueue.size());

    bool finished = m_finishCond.wait_for(lock, std::chrono::seconds(2), [this] {
      return m_finishQueue.empty();
    });

    if (!finished) {
      Logger::warn(str::format("[DXVKq] waitForIdle timeout; continuing submit=",
        m_submitQueue.size(), " finish=", m_finishQueue.size(),
        " last=", m_lastError.load()));
      printf("[DXVKq] waitForIdle finish timeout; continuing submit=%llu finish=%llu last=%d\n",
        (unsigned long long)m_submitQueue.size(),
        (unsigned long long)m_finishQueue.size(),
        (int)m_lastError.load());
      return;
    }

    Logger::warn("[DXVKq] waitForIdle done");
    printf("[DXVKq] waitForIdle done\n");
  }


  void DxvkSubmissionQueue::lockDeviceQueue() {
    m_mutexQueue.lock();

    if (m_callback)
      m_callback(true);
  }


  void DxvkSubmissionQueue::unlockDeviceQueue() {
    if (m_callback)
      m_callback(false);

    m_mutexQueue.unlock();
  }


  void DxvkSubmissionQueue::submitCmdLists() {
    env::setThreadName("dxvk-submit");
    Logger::warn("[DXVKq] submit thread start");
    printf("[DXVKq] submit thread start\n");

    std::unique_lock<dxvk::mutex> lock(m_mutex);

    while (!m_stopped.load()) {
      m_appendCond.wait(lock, [this] {
        return m_stopped.load() || !m_submitQueue.empty();
      });
      
      if (m_stopped.load()) {
        Logger::warn("[DXVKq] submit thread stop");
        printf("[DXVKq] submit thread stop\n");
        return;
      }
      
      DxvkSubmitEntry entry = std::move(m_submitQueue.front());
      lock.unlock();

      // Submit command buffer to device
      if (m_lastError != VK_ERROR_DEVICE_LOST) {
        std::lock_guard<dxvk::mutex> lock(m_mutexQueue);

        if (m_callback)
          m_callback(true);

        if (entry.submit.cmdList != nullptr) {
          entry.result = entry.submit.cmdList->submit();
          printf("[DXVKq] cmd submit result=%d\n", (int)entry.result);
        } else if (entry.present.presenter != nullptr) {
          entry.result = entry.present.presenter->presentImage(entry.present.presentMode, entry.present.frameId);
          printf("[DXVKq] present submit result=%d\n", (int)entry.result);
        }

        if (m_callback)
          m_callback(false);
      } else {
        // Don't submit anything after device loss
        // so that drivers get a chance to recover
        entry.result = VK_ERROR_DEVICE_LOST;
      }

      if (entry.status)
        entry.status->result = entry.result;
      
      // On success, pass it on to the queue thread
      lock = std::unique_lock<dxvk::mutex>(m_mutex);

      bool doForward = (entry.result == VK_SUCCESS) ||
        (entry.present.presenter != nullptr && entry.result != VK_ERROR_DEVICE_LOST);

      if (doForward) {
        m_finishQueue.push(std::move(entry));
        Logger::warn(str::format("[DXVKq] submit forwarded finish=",
          m_finishQueue.size()));
        printf("[DXVKq] submit forwarded finish=%llu\n",
          (unsigned long long)m_finishQueue.size());
      } else {
        Logger::err(str::format("DxvkSubmissionQueue: Command submission failed: ", entry.result));
        printf("[DXVKq] submit failed result=%d\n", (int)entry.result);
        m_lastError = entry.result;

        if (m_lastError != VK_ERROR_DEVICE_LOST)
          m_device->waitForIdle();
      }

      m_submitQueue.pop();
      m_submitCond.notify_all();
    }
  }
  
  
  void DxvkSubmissionQueue::finishCmdLists() {
    env::setThreadName("dxvk-queue");
    Logger::warn("[DXVKq] finish thread start");
    printf("[DXVKq] finish thread start\n");

    while (!m_stopped.load()) {
      std::unique_lock<dxvk::mutex> lock(m_mutex);

      if (m_finishQueue.empty()) {
        auto t0 = dxvk::high_resolution_clock::now();

        m_submitCond.wait(lock, [this] {
          return m_stopped.load() || !m_finishQueue.empty();
        });

        auto t1 = dxvk::high_resolution_clock::now();
        m_gpuIdle += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
      }

      if (m_stopped.load()) {
        Logger::warn("[DXVKq] finish thread stop");
        printf("[DXVKq] finish thread stop\n");
        return;
      }
      
      DxvkSubmitEntry entry = std::move(m_finishQueue.front());
      lock.unlock();
      
      if (entry.submit.cmdList != nullptr) {
        VkResult status = m_lastError.load();
        
        if (status != VK_ERROR_DEVICE_LOST)
          status = entry.submit.cmdList->synchronizeFence();
        printf("[DXVKq] fence status=%d last=%d\n",
          (int)status, (int)m_lastError.load());
        
        if (status != VK_SUCCESS) {
          printf("[DXVKq] finish sets lastError=%d\n", (int)status);
          m_lastError = status;

          if (status != VK_ERROR_DEVICE_LOST)
            m_device->waitForIdle();
        }
      } else if (entry.present.presenter != nullptr) {
        // Signal the frame and then immediately destroy the reference.
        // This is necessary since the front-end may want to explicitly
        // destroy the presenter object. 
        entry.present.presenter->signalFrame(entry.result,
          entry.present.presentMode, entry.present.frameId);
        entry.present.presenter = nullptr;
      }

      // Release resources and signal events, then immediately wake
      // up any thread that's currently waiting on a resource in
      // order to reduce delays as much as possible.
      if (entry.submit.cmdList != nullptr)
        entry.submit.cmdList->notifyObjects();

      lock.lock();
      m_finishQueue.pop();
      Logger::warn(str::format("[DXVKq] finish popped finish=",
        m_finishQueue.size()));
      printf("[DXVKq] finish popped finish=%llu\n",
        (unsigned long long)m_finishQueue.size());
      m_finishCond.notify_all();
      lock.unlock();

      // Free the command list and associated objects now
      if (entry.submit.cmdList != nullptr) {
        entry.submit.cmdList->reset();
        m_device->recycleCommandList(entry.submit.cmdList);
      }
    }
  }
  
}

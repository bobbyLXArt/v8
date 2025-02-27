// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdlib.h>

#include "src/heap/safepoint.h"

#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <utility>

#include "src/handles/global-handles.h"
#include "src/heap/gc-tracer.h"
#include "src/heap/incremental-marking.h"
#include "src/heap/spaces.h"
#include "src/init/v8.h"
#include "src/objects/objects-inl.h"
#include "test/cctest/cctest.h"
#include "test/cctest/heap/heap-utils.h"

using v8::IdleTask;
using v8::Task;
using v8::Isolate;

namespace v8 {
namespace internal {
namespace heap {

class MockPlatform : public TestPlatform {
 public:
  MockPlatform() : taskrunner_(new MockTaskRunner()) { NotifyPlatformReady(); }
  ~MockPlatform() override {
    RemovePlatform();
    for (auto& task : worker_tasks_) {
      old_platform()->CallOnWorkerThread(std::move(task));
    }
    worker_tasks_.clear();
  }

  std::shared_ptr<v8::TaskRunner> GetForegroundTaskRunner(
      v8::Isolate* isolate) override {
    return taskrunner_;
  }

  void CallOnWorkerThread(std::unique_ptr<Task> task) override {
    worker_tasks_.push_back(std::move(task));
  }

  bool IdleTasksEnabled(v8::Isolate* isolate) override { return false; }

  bool PendingTask() { return taskrunner_->PendingTask(); }

  void PerformTask() { taskrunner_->PerformTask(); }

 private:
  class MockTaskRunner : public v8::TaskRunner {
   public:
    void PostTask(std::unique_ptr<v8::Task> task) override {
      task_ = std::move(task);
    }

    void PostNonNestableTask(std::unique_ptr<Task> task) override {
      PostTask(std::move(task));
    }

    void PostDelayedTask(std::unique_ptr<Task> task,
                         double delay_in_seconds) override {
      PostTask(std::move(task));
    }

    void PostNonNestableDelayedTask(std::unique_ptr<Task> task,
                                    double delay_in_seconds) override {
      PostTask(std::move(task));
    }

    void PostIdleTask(std::unique_ptr<IdleTask> task) override {
      UNREACHABLE();
    }

    bool IdleTasksEnabled() override { return false; }
    bool NonNestableTasksEnabled() const override { return true; }
    bool NonNestableDelayedTasksEnabled() const override { return true; }

    bool PendingTask() { return task_ != nullptr; }

    void PerformTask() {
      std::unique_ptr<Task> task = std::move(task_);
      task->Run();
    }

   private:
    std::unique_ptr<Task> task_;
  };

  std::shared_ptr<MockTaskRunner> taskrunner_;
  std::vector<std::unique_ptr<Task>> worker_tasks_;
};

UNINITIALIZED_TEST(IncrementalMarkingUsingTasks) {
  if (!i::FLAG_incremental_marking) return;
  FLAG_stress_concurrent_allocation = false;  // For SimulateFullSpace.
  FLAG_stress_incremental_marking = false;
  MockPlatform platform;
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = CcTest::array_buffer_allocator();
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  {
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = CcTest::NewContext(isolate);
    v8::Context::Scope context_scope(context);
    Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
    Heap* heap = i_isolate->heap();

    i::heap::SimulateFullSpace(heap->old_space());
    i::IncrementalMarking* marking = heap->incremental_marking();
    marking->Stop();
    {
      SafepointScope scope(heap);
      heap->tracer()->StartCycle(
          GarbageCollector::MARK_COMPACTOR, GarbageCollectionReason::kTesting,
          "collector cctest", GCTracer::MarkingType::kIncremental);
      marking->Start(i::GarbageCollectionReason::kTesting);
    }
    CHECK(platform.PendingTask());
    while (platform.PendingTask()) {
      platform.PerformTask();
    }
    CHECK(marking->IsStopped());
  }
  isolate->Dispose();
}

}  // namespace heap
}  // namespace internal
}  // namespace v8

// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/mtl/tasks/message_loop.h"

#include <utility>

#include "lib/ftl/logging.h"

namespace mtl {
namespace {

thread_local MessageLoop* g_current = nullptr;

}  // namespace

MessageLoop::MessageLoop()
    : MessageLoop(ftl::MakeRefCounted<internal::IncomingTaskQueue>()) {}

MessageLoop::MessageLoop(
    ftl::RefPtr<internal::IncomingTaskQueue> incoming_tasks)
    : incoming_tasks_(std::move(incoming_tasks)) {
  FTL_DCHECK(!g_current) << "At most one message loop per thread.";
  incoming_tasks_->InitDelegate(this);
  g_current = this;
}

MessageLoop::~MessageLoop() {
  FTL_DCHECK(g_current == this)
      << "Message loops must be destroyed on their own threads.";

  incoming_tasks_->ClearDelegate();
  ReloadQueue();

  // Destroy the tasks in the order in which they would have run.
  while (!queue_.empty())
    queue_.pop();

  // Finally, remove ourselves from TLS.
  g_current = nullptr;
}

MessageLoop* MessageLoop::GetCurrent() {
  return g_current;
}

void MessageLoop::Run() {
  FTL_DCHECK(!should_quit_);
  FTL_CHECK(!is_running_) << "Cannot run a nested message loop.";
  is_running_ = true;

  for (;;) {
    ftl::TimePoint next_run_time = RunReadyTasks();
    if (should_quit_)
      break;

    if (next_run_time == ftl::TimePoint()) {
      event_.Wait();
    } else {
      ftl::TimeDelta delay = next_run_time - ftl::TimePoint::Now();
      if (delay > ftl::TimeDelta::Zero())
        event_.WaitWithTimeout(delay);
    }
  }

  should_quit_ = false;

  FTL_DCHECK(is_running_);
  is_running_ = false;
}

void MessageLoop::QuitNow() {
  FTL_DCHECK(is_running_);
  should_quit_ = true;
}

void MessageLoop::ScheduleDrainIncomingTasks() {
  event_.Signal();
}

ftl::TimePoint MessageLoop::RunReadyTasks() {
  FTL_DCHECK(!should_quit_);
  ReloadQueue();

  while (!queue_.empty() && !should_quit_) {
    // When we "fall behind", there will be a lot of tasks in the delayed work
    // queue that are ready to run.  To increase efficiency when we fall behind,
    // we will only call Now() intermittently, and then process all tasks that
    // are ready to run before calling it again.  As a result, the more we fall
    // behind (and have a lot of ready-to-run delayed tasks), the more efficient
    // we'll be at handling the tasks.

    ftl::TimePoint next_run_time = queue_.top().target_time();
    if (next_run_time > recent_time_) {
      recent_time_ = ftl::TimePoint::Now();
      if (next_run_time > recent_time_)
        return next_run_time;
    }

    internal::PendingTask task =
        std::move(const_cast<internal::PendingTask&>(queue_.top()));
    queue_.pop();

    RunTask(task);
  }

  return ftl::TimePoint();
}

void MessageLoop::ReloadQueue() {
  for (auto& task : incoming_tasks_->TakeTaskQueue())
    queue_.push(std::move(task));
}

void MessageLoop::RunTask(const internal::PendingTask& pending_task) {
  const ftl::Closure& closure = pending_task.closure();
  closure();
}

}  // namespace mtl

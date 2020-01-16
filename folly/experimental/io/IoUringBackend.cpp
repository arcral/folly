/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/experimental/io/IoUringBackend.h>
#include <folly/Likely.h>
#include <folly/String.h>
#include <folly/portability/Sockets.h>
#include <folly/synchronization/CallOnce.h>

#include <glog/logging.h>

namespace folly {
IoUringBackend::FdRegistry::FdRegistry(struct io_uring& ioRing, size_t n)
    : ioRing_(ioRing), files_(n, -1), inUse_(n), records_(n) {}

int IoUringBackend::FdRegistry::init() {
  if (inUse_) {
    int ret = ::io_uring_register_files(&ioRing_, files_.data(), inUse_);

    if (!ret) {
      // build and set the free list head if we succeed
      for (size_t i = 0; i < records_.size(); i++) {
        records_[i].idx_ = i;
        free_.push_front(records_[i]);
      }
    } else {
      LOG(ERROR) << "io_uring_register_files(" << inUse_ << ") "
                 << "failed errno = " << errno << ":\""
                 << folly::errnoStr(errno) << "\" " << this;
    }

    return ret;
  }

  return 0;
}

IoUringBackend::FdRegistrationRecord* IoUringBackend::FdRegistry::alloc(
    int fd) {
  if (FOLLY_UNLIKELY(free_.empty())) {
    return nullptr;
  }

  auto& ret = free_.front();

  // now we have an idx
  if (::io_uring_register_files_update(&ioRing_, ret.idx_, &fd, 1) != 1) {
    return nullptr;
  }

  ret.fd_ = fd;
  ret.count_ = 1;
  free_.pop_front();
  return &ret;
}

bool IoUringBackend::FdRegistry::free(
    IoUringBackend::FdRegistrationRecord* record) {
  if (record && (--record->count_ == 0)) {
    record->fd_ = -1;
    int ret = ::io_uring_register_files_update(
        &ioRing_, record->idx_, &record->fd_, 1);

    // we add it to the free list anyway here
    free_.push_front(*record);

    return (ret == 1);
  }

  return false;
}

IoUringBackend::IoUringBackend(
    size_t capacity,
    size_t maxSubmit,
    size_t maxGet,
    bool useRegisteredFds)
    : PollIoBackend(capacity, maxSubmit, maxGet),
      fdRegistry_(ioRing_, useRegisteredFds ? capacity : 0) {
  ::memset(&ioRing_, 0, sizeof(ioRing_));
  ::memset(&params_, 0, sizeof(params_));

  params_.flags |= IORING_SETUP_CQSIZE;
  params_.cq_entries = capacity;

  // allocate entries both for poll add and cancel
  if (::io_uring_queue_init_params(2 * maxSubmit_, &ioRing_, &params_)) {
    LOG(ERROR) << "io_uring_queue_init_params(" << 2 * maxSubmit_ << ","
               << params_.cq_entries << ") "
               << "failed errno = " << errno << ":\"" << folly::errnoStr(errno)
               << "\" " << this;
    throw NotAvailable("io_uring_queue_init error");
  }

  sqRingMask_ = *ioRing_.sq.kring_mask;
  cqRingMask_ = *ioRing_.cq.kring_mask;

  numEntries_ *= 2;

  entries_.reset(new IoSqe[numEntries_]);

  timerEntry_ = &entries_[0];
  timerEntry_->backend_ = this;
  timerEntry_->backendCb_ = PollIoBackend::processTimerIoCb;

  // build the free list - first entry is the timer entry
  for (size_t i = 2; i < numEntries_; i++) {
    entries_[i - 1].next_ = &entries_[i];
    entries_[i - 1].backend_ = this;
    entries_[i - 1].backendCb_ = PollIoBackend::processPollIoCb;
  }

  entries_[numEntries_ - 1].backend_ = this;
  entries_[numEntries_ - 1].backendCb_ = PollIoBackend::processPollIoCb;
  freeHead_ = &entries_[1];

  // we need to call the init before adding the timer fd
  // so we avoid a deadlock - waiting for the queue to be drained
  if (useRegisteredFds) {
    // now init the file registry
    // if this fails, we still continue since we
    // can run without registered fds
    fdRegistry_.init();
  }

  // add the timer fd
  if (!addTimerFd()) {
    cleanup();
    entries_.reset();
    throw NotAvailable("io_uring_submit error");
  }
}

IoUringBackend::~IoUringBackend() {
  shuttingDown_ = true;

  cleanup();
}

void IoUringBackend::cleanup() {
  if (ioRing_.ring_fd > 0) {
    // release the nonsubmitted items from the submitList
    while (!submitList_.empty()) {
      auto* ioCb = &submitList_.front();
      submitList_.pop_front();
      releaseIoCb(ioCb);
    }

    // release the active events
    while (!activeEvents_.empty()) {
      auto* ioCb = &activeEvents_.front();
      activeEvents_.pop_front();
      releaseIoCb(ioCb);
    }

    // wait for the outstanding events to finish
    while (numIoCbInUse()) {
      struct io_uring_cqe* cqe = nullptr;
      ::io_uring_wait_cqe(&ioRing_, &cqe);
      if (cqe) {
        IoSqe* sqe = reinterpret_cast<IoSqe*>(io_uring_cqe_get_data(cqe));
        releaseIoCb(sqe);
        ::io_uring_cqe_seen(&ioRing_, cqe);
      }
    }

    // exit now
    ::io_uring_queue_exit(&ioRing_);
    ioRing_.ring_fd = -1;
  }
}

bool IoUringBackend::isAvailable() {
  static bool sAvailable = true;

  static folly::once_flag initFlag;
  folly::call_once(initFlag, [&]() {
    try {
      IoUringBackend backend(1024, 128);
    } catch (const NotAvailable&) {
      sAvailable = false;
    }
  });

  return sAvailable;
}

void* IoUringBackend::allocSubmissionEntry() {
  return ::io_uring_get_sqe(&ioRing_);
}

int IoUringBackend::submitOne(IoCb* /*unused*/) {
  return submitBusyCheck();
}

int IoUringBackend::cancelOne(IoCb* ioCb) {
  auto* rentry = static_cast<IoSqe*>(allocIoCb());
  if (!rentry) {
    return 0;
  }

  auto* sqe = ::io_uring_get_sqe(&ioRing_);
  CHECK(sqe);

  rentry->prepPollRemove(sqe, ioCb); // prev entry

  int ret = submitBusyCheck();

  if (ret < 0) {
    // release the sqe
    releaseIoCb(rentry);
  }

  return ret;
}

int IoUringBackend::getActiveEvents(WaitForEventsMode waitForEvents) {
  size_t i = 0;
  struct io_uring_cqe* cqe = nullptr;
  // we can be called from the submitList() method
  // or with non blocking flags
  if (FOLLY_LIKELY(waitForEvents == WaitForEventsMode::WAIT)) {
    ::io_uring_wait_cqe(&ioRing_, &cqe);
  } else {
    ::io_uring_peek_cqe(&ioRing_, &cqe);
  }
  while (cqe && (i < maxGet_)) {
    i++;
    IoSqe* sqe = reinterpret_cast<IoSqe*>(io_uring_cqe_get_data(cqe));
    sqe->backendCb_(this, sqe, cqe->res);
    ::io_uring_cqe_seen(&ioRing_, cqe);
    cqe = nullptr;
    ::io_uring_peek_cqe(&ioRing_, &cqe);
  }

  return static_cast<int>(i);
}

int IoUringBackend::submitBusyCheck() {
  int num;
  while ((num = ::io_uring_submit(&ioRing_)) == -EBUSY) {
    // if we get EBUSY, try to consume some CQ entries
    getActiveEvents(WaitForEventsMode::DONT_WAIT);
  };
  return num;
}

int IoUringBackend::submitBusyCheckAndWait() {
  int num;
  while ((num = ::io_uring_submit_and_wait(&ioRing_, 1)) == -EBUSY) {
    // if we get EBUSY, try to consume some CQ entries
    getActiveEvents(WaitForEventsMode::DONT_WAIT);
  };
  return num;
}

size_t IoUringBackend::submitList(
    IoCbList& ioCbs,
    WaitForEventsMode waitForEvents) {
  int i = 0;
  size_t ret = 0;

  while (!ioCbs.empty()) {
    auto* entry = &ioCbs.front();
    ioCbs.pop_front();
    auto* sqe = ::io_uring_get_sqe(&ioRing_);
    CHECK(sqe); // this should not happen

    auto* ev = entry->event_->getEvent();
    entry->prepPollAdd(
        sqe,
        ev->ev_fd,
        getPollFlags(ev->ev_events),
        (ev->ev_events & EV_PERSIST) != 0);
    i++;
    if (ioCbs.empty()) {
      int num = (waitForEvents == WaitForEventsMode::WAIT)
          ? submitBusyCheckAndWait()
          : submitBusyCheck();
      CHECK_EQ(num, i);
      ret += i;
    } else {
      if (static_cast<size_t>(i) == maxSubmit_) {
        int num = submitBusyCheck();
        CHECK_EQ(num, i);
        ret += i;
        i = 0;
      }
    }
  }

  return ret;
}
} // namespace folly

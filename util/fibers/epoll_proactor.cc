// Copyright 2023, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "util/fibers/epoll_proactor.h"

#include <absl/time/clock.h>
#include <signal.h>
#include <string.h>

#ifdef __linux__
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#endif

#if defined(__FreeBSD__)
#include <pthread_np.h>
#endif

#include "base/cycle_clock.h"
#include "base/logging.h"
#include "base/proc_util.h"
#include "util/fibers/epoll_socket.h"

#define EV_CHECK(x)                                                           \
  do {                                                                        \
    int __res_val = (x);                                                      \
    if (ABSL_PREDICT_FALSE(__res_val < 0)) {                                  \
      LOG(FATAL) << "Error " << (-__res_val)                                  \
                 << " evaluating '" #x "': " << SafeErrorMessage(-__res_val); \
    }                                                                         \
  } while (false)

#define VPRO(verbosity) VLOG(verbosity) << "PRO[" << GetPoolIndex() << "] "

using namespace std;

namespace util {
namespace fb2 {

using detail::FiberInterface;

namespace {

constexpr uint64_t kIgnoreIndex = 0;
constexpr uint64_t kUserDataCbIndex = 1024;
constexpr size_t kEvBatchSize = 128;

#ifdef __linux__
struct EventsBatch {
  struct epoll_event cqe[kEvBatchSize];
};

int EpollCreate() {
  int res = epoll_create1(EPOLL_CLOEXEC);
  CHECK_GE(res, 0);
  return res;
}

int EpollWait(int epoll_fd, EventsBatch* batch, int timeout) {
  return epoll_wait(epoll_fd, batch->cqe, kEvBatchSize, timeout);
}

void EpollDel(int epoll_fd, int fd) {
  CHECK_EQ(0, epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL));
}

#define USER_DATA(cqe) (cqe).data.u32
#define KEV_MASK(cqe) (cqe).events
#define KEV_ERROR(cqe) (0)

#else
struct EventsBatch {
  struct kevent cqe[kEvBatchSize];
};

int EpollCreate() {
  int res = kqueue();
  CHECK_GE(res, 0);

  // Register an event to wake up the event loop.
  struct kevent kev;

  EV_SET(&kev, 0 /* ident*/, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, (void*)kIgnoreIndex);
  CHECK_EQ(0, kevent(res, &kev, 1, NULL, 0, NULL));

  return res;
}

int EpollWait(int epoll_fd, EventsBatch* batch, int tm_ms) {
  struct timespec ts {
    .tv_sec = tm_ms / 1000, .tv_nsec = (tm_ms % 1000) * 1000000
  };

  int epoll_res = kevent(epoll_fd, NULL, 0, batch->cqe, kEvBatchSize, tm_ms < 0 ? NULL : &ts);
  return epoll_res;
}

void EpollDel(int epoll_fd, int fd) {
  struct kevent kev[2];

  EV_SET(&kev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
  EV_SET(&kev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
  CHECK_EQ(0, kevent(epoll_fd, kev, 2, NULL, 0, NULL));
}

uint32_t KevMask(const struct kevent& kev) {
  DVLOG(2) << "kev: " << kev.ident << " filter(" << kev.filter << ") f(" << kev.flags << ") ff("
           << kev.fflags << ") d" << kev.data;

  if (kev.flags & EV_EOF) {
    return POLLHUP;
  }
  uint32_t ev_mask = 0;

  switch (kev.filter) {
    case EVFILT_READ:
      ev_mask = EpollProactor::EPOLL_IN;
      break;
    case EVFILT_WRITE:
      ev_mask = EpollProactor::EPOLL_OUT;
      break;

    default:
      LOG(FATAL) << "unsupported" << kev.filter;
  }
  return ev_mask;
}

#define USER_DATA(cqe) ((uint64_t)(cqe).udata)
#define KEV_MASK(cqe) KevMask(cqe)
#define KEV_ERROR(cqe) cqe.fflags
#endif

}  // namespace

EpollProactor::EpollProactor() : ProactorBase() {
  epoll_fd_ = EpollCreate();

  VLOG(1) << "Created epoll_fd_ " << epoll_fd_;
}

EpollProactor::~EpollProactor() {
  CHECK(is_stopped_);
  close(epoll_fd_);

  DVLOG(1) << "~EpollProactor";
}

void EpollProactor::Init(unsigned pool_index) {
  pool_index_ = pool_index;
  if (thread_id_ != 0) {
    LOG(FATAL) << "Init was already called";
  }

  centries_.resize(512);  // .index = -1
  next_free_ce_ = 0;
  for (size_t i = 0; i < centries_.size() - 1; ++i) {
    centries_[i].index = i + 1;
  }

  thread_id_ = pthread_self();

#ifdef __linux__
  sys_thread_id_ = syscall(SYS_gettid);
#elif defined(__FreeBSD__)
  sys_thread_id_ = pthread_getthreadid_np();
#endif

  tl_info_.owner = this;

#ifdef __linux__
  auto cb = [ev_fd = wake_fd_](uint32_t mask, int, auto*) {
    DVLOG(2) << "EventFdCb called " << mask;
    uint64_t val;
    CHECK_EQ(8, read(ev_fd, &val, sizeof(val)));
  };
  Arm(wake_fd_, std::move(cb), EPOLLIN);
#endif
}

void EpollProactor::MainLoop(detail::Scheduler* scheduler) {
  VLOG(1) << "EpollProactor::MainLoop";

  EventsBatch ev_batch;
  uint32_t tq_seq = 0;
  uint32_t spin_loops = 0;

  Tasklet task;

  while (true) {
    ++stats_.loop_cnt;
    bool task_queue_exhausted = true;

    tq_seq = tq_seq_.load(memory_order_acquire);

    if (task_queue_.try_dequeue(task)) {
      uint32_t cnt = 0;
      uint64_t task_start = GetClockNanos();

      // update thread-local clock service via GetMonotonicTimeNs().
      tl_info_.monotonic_time = task_start;
      do {
        task();
        ++stats_.num_task_runs;
        ++cnt;
        tl_info_.monotonic_time = GetClockNanos();
        if (task_start + 500000 < tl_info_.monotonic_time) {  // Break after 500usec
          ++stats_.task_interrupts;
          task_queue_exhausted = false;
          break;
        }

        if (cnt == 32) {
          // we notify threads if we unloaded a bunch of tasks.
          // if in parallel they start pushing we may unload them in parallel
          // via this loop thus increasing its efficiency.
          task_queue_avail_.notifyAll();
        }
      } while (task_queue_.try_dequeue(task));

      stats_.num_task_runs += cnt;
      DVLOG(2) << "Tasks runs " << stats_.num_task_runs << "/" << spin_loops;

      // We notify at the end that the queue is not full.
      // Tested by ProactorTest.AsyncCall.
      task_queue_avail_.notifyAll();
    }

    // We process remote fibers inside tq_seq section and also before we check for HasReady().
    scheduler->ProcessRemoteReady(nullptr);

    int timeout = 0;  // By default we do not block on epoll_wait.

    // Check if we can block on I/O.
    // There are few ground rules before we can set timeout=-1 (i.e. block indefinitely)
    // 1. No other fibers are active.
    // 2. Specifically SuspendIoLoop was called and returned true.
    // 3. Task queue is empty otherwise we should spin more to unload it.
    if (task_queue_exhausted && !scheduler->HasReady() && spin_loops >= kMaxSpinLimit) {
      spin_loops = 0;
      if (tq_seq_.compare_exchange_weak(tq_seq, WAIT_SECTION_STATE, memory_order_acquire)) {
        // We check stop condition when all the pending events were processed.
        // It's up to the app-user to make sure that the incoming flow of events is stopped before
        // stopping EpollProactor.
        if (is_stopped_)
          break;
        ++stats_.num_stalls;
        timeout = -1;  // We gonna block on epoll_wait.
      }
    }

    DVLOG(2) << "EpollWait " << timeout << " " << tq_seq;

    if (timeout == -1 && scheduler->HasSleepingFibers()) {
      auto tp = scheduler->NextSleepPoint();
      auto now = chrono::steady_clock::now();
      if (now < tp) {
        auto ns = chrono::duration_cast<chrono::nanoseconds>(tp - now).count();

        // epoll_wait() uses millisecond precision. If we block for less than the precise deadline,
        // we cause unnesessary spinning and an elevated CPU usage. Therefore, we round up.
        timeout = (ns + 1000'000 - 1) / 1000'000;
      } else {
        timeout = 0;
      }
    }

    uint64_t start_cycle = base::CycleClock::Now();
    int epoll_res = EpollWait(epoll_fd_, &ev_batch, timeout);
    IdleEnd(start_cycle);

    if (epoll_res < 0) {
      epoll_res = errno;
      if (epoll_res == EINTR)
        continue;
      LOG(FATAL) << "TBD: " << errno << " " << strerror(errno);
    }
    tq_seq_.store(0, std::memory_order_release);

    uint32_t cqe_count = epoll_res;
    if (cqe_count) {
      ++stats_.completions_fetches;
      tl_info_.monotonic_time = GetClockNanos();

      while (true) {
        VPRO(2) << "Fetched " << cqe_count << " cqes";
        DispatchCompletions(&ev_batch, cqe_count);

        if (cqe_count < kEvBatchSize) {
          break;
        }
        epoll_res = EpollWait(epoll_fd_, &ev_batch, 0);
        if (epoll_res < 0) {
          break;
        }
        cqe_count = epoll_res;
        ++stats_.completions_fetches;
      };
    }

    RunL2Tasks(scheduler);

    // must be if and not while - see uring_proactor.cc for more details.
    if (!scheduler->RunWorkerFibersStep()) {
      cqe_count = 1;
    }

    if (cqe_count) {
      continue;
    }

    scheduler->DestroyTerminated();
    if (!RunOnIdleTasks()) {
      Pause(spin_loops);
      ++spin_loops;
    }
  }

  VPRO(1) << "total/stalls/cqe_fetches/num_suspends: " << stats_.loop_cnt << "/"
          << stats_.num_stalls << "/" << stats_.completions_fetches << "/" << stats_.num_suspends;

  VPRO(1) << "wakeups/stalls/task_int: " << tq_wakeup_ev_.load() << "/" << stats_.task_interrupts;
  VPRO(1) << "centries size: " << centries_.size();
}

unsigned EpollProactor::Arm(int fd, CbType cb, uint32_t event_mask) {
  if (next_free_ce_ < 0) {
    RegrowCentries();
    CHECK_GT(next_free_ce_, 0);
  }
  unsigned ret = next_free_ce_;

  auto& e = centries_[next_free_ce_];
  DCHECK(!e.cb);  // cb is undefined.
  DVLOG(1) << "Arm: " << fd << ", index: " << next_free_ce_;

  next_free_ce_ = e.index;
  e.cb = std::move(cb);
  e.index = -1;

#ifdef __linux__
  epoll_event ev;
  ev.events = event_mask;
  ev.data.u32 = ret + kUserDataCbIndex;
  DCHECK_LT(ret, centries_.size());

  CHECK_EQ(0, epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev));
#else
  // FreeBsd
  struct kevent kev[2];
  unsigned index = 0;
  uint64_t ud = ret + kUserDataCbIndex;
  if (event_mask & EPOLL_IN)
    EV_SET(&kev[index++], fd /* ident*/, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, (void*)ud);
  if (event_mask & EPOLL_OUT)
    EV_SET(&kev[index++], fd /* ident*/, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, (void*)ud);

  CHECK_EQ(0, kevent(epoll_fd_, kev, index, NULL, 0, NULL));
#endif

  return ret;
}

void EpollProactor::Disarm(int fd, unsigned arm_index) {
  DCHECK(pthread_self() == thread_id_);

  DVLOG(2) << "Disarming " << fd << " on " << arm_index;
  CHECK_LT(arm_index, centries_.size());

  centries_[arm_index].cb = nullptr;
  centries_[arm_index].index = next_free_ce_;

  next_free_ce_ = arm_index;
  EpollDel(epoll_fd_, fd);
}

LinuxSocketBase* EpollProactor::CreateSocket() {
  EpollSocket* res = new EpollSocket(-1);
  res->SetProactor(this);

  return res;
}

void EpollProactor::SchedulePeriodic(uint32_t id, PeriodicItem* item) {
#ifdef __linux__
  int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  CHECK_GE(tfd, 0);
  itimerspec ts;
  ts.it_value = item->period;
  ts.it_interval = item->period;
  item->val1 = tfd;

  auto cb = [this, item](uint32_t event_mask, int, EpollProactor*) { this->PeriodicCb(item); };

  unsigned arm_id = Arm(tfd, std::move(cb), EPOLLIN);
  item->val2 = arm_id;

  CHECK_EQ(0, timerfd_settime(tfd, 0, &ts, NULL));
#else
  // FreeBsd
  struct kevent kev;
  int64_t msec = item->period.tv_sec * 1000 + item->period.tv_nsec / 1000000;

  // Create a timer event with EV_SET
  EV_SET(&kev, id, EVFILT_TIMER, EV_ADD | EV_ENABLE, 0, msec, item);
  item->val1 = id;
  CHECK_EQ(0, kevent(epoll_fd_, &kev, 1, NULL, 0, NULL));
#endif
}

void EpollProactor::CancelPeriodicInternal(PeriodicItem* item) {
#ifdef __linux__
  uint32_t tfd = item->val1, arm_id = item->val2;

  Disarm(tfd, arm_id);
  if (close(tfd) == -1) {
    LOG(ERROR) << "Could not close timer, error " << errno;
  }

#else
  // FreeBsd
  struct kevent kev;
  EV_SET(&kev, item->val1, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
  CHECK_EQ(0, kevent(epoll_fd_, &kev, 1, NULL, 0, NULL));
#endif

  // Note - I assume that unlike with io_uring,
  // kevent/epoll do not send late completions after we disarmed the event.
  // If this assumption holds, it's safe to delete this pointer.
  DCHECK_EQ(item->ref_cnt, 0u);
  delete item;
}

void EpollProactor::WakeRing() {
  // Remember, WakeRing is called from external threads.
  DVLOG(2) << "Wake ring " << tq_seq_.load(memory_order_relaxed);

  tq_wakeup_ev_.fetch_add(1, memory_order_relaxed);

#ifdef __linux__
  uint64_t val = 1;
  CHECK_EQ(8, write(wake_fd_, &val, sizeof(uint64_t)));
#else
  struct kevent kev;
  EV_SET(&kev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, (void*)kIgnoreIndex);
  CHECK_EQ(0, kevent(epoll_fd_, &kev, 1, NULL, 0, NULL));
#endif
}

void EpollProactor::PeriodicCb(PeriodicItem* item) {
  CHECK_GT(item->ref_cnt, 0u);

  DCHECK(item->task);
  item->task();

#ifdef __linux__
  uint64_t res;
  if (read(item->val1, &res, sizeof(res)) == -1) {
    LOG(ERROR) << "Error reading from timer, errno " << errno;
  }
#endif
}

void EpollProactor::DispatchCompletions(const void* cevents, unsigned count) {
  DVLOG(2) << "DispatchCompletions " << count << " cqes";
  const EventsBatch& ev_batch = *reinterpret_cast<const EventsBatch*>(cevents);

  for (unsigned i = 0; i < count; ++i) {
    const auto& cqe = ev_batch.cqe[i];

#ifndef __linux__
    // FreeBsd based timer event.
    if (cqe.filter == EVFILT_TIMER) {
      PeriodicItem* item = reinterpret_cast<PeriodicItem*>(cqe.udata);
      PeriodicCb(item);
      continue;
    }
#endif

    // I allocate range of 1024 reserved values for the internal EpollProactor use.
    uint32_t user_data = USER_DATA(cqe);

    if (user_data >= kUserDataCbIndex) {  // our heap range surely starts higher than 1k.
      size_t index = user_data - kUserDataCbIndex;
      DCHECK_LT(index, centries_.size());
      const auto& item = centries_[index];

      // we do not move and reset cb, because epoll events are multishot.
      // We could disarm an event and get this completion afterwards.
      // This is why we check for cb and item.index.
      //
      // TODO: However it's still not enough because someone could arm the same index
      // and we would dispatch the wrong event to the new callback.
      // Solution - we can use another 32bits in cqe.data for generation number.
      if (item.index == -1 && item.cb) {
        uint32_t ev_mask = KEV_MASK(cqe);
        int ev_err = KEV_ERROR(cqe);

        item.cb(ev_mask, ev_err, this);
      }
      continue;
    }

    if (user_data == kIgnoreIndex)
      continue;

    LOG(ERROR) << "Unrecognized user_data " << user_data;
  }
}

void EpollProactor::RegrowCentries() {
  size_t prev = centries_.size();
  VLOG(1) << "RegrowCentries from " << prev << " to " << prev * 2;

  centries_.resize(prev * 2);  // grow by 2.
  next_free_ce_ = prev;
  for (; prev < centries_.size() - 1; ++prev)
    centries_[prev].index = prev + 1;
}

}  // namespace fb2
}  // namespace util

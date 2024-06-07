#include "noneEvent.h"
#include "profiler.h"
#include "tsc.h"
#include <iostream>
#include <signal.h>

// Maximum number of threads sampled in one iteration. This limit serves as a
// throttle when generating profiling signals. Otherwise applications with too
// many threads may suffer from a big profiling overhead. Also, keeping this
// limit low enough helps to avoid contention on a spin lock inside
// Profiler::recordSample().
const int NONE_THREADS_PER_TICK = 8;

// Set the hard limit for thread walking interval to 100 microseconds.
// Smaller intervals are practically unusable due to large overhead.
const long NONE_MIN_INTERVAL = 100000;

long NoneEvent::_interval;
int NoneEvent::_signal;
bool NoneEvent::_sample_idle_threads;

ThreadState NoneEvent::getThreadState(void *ucontext) {
  StackFrame frame(ucontext);
  uintptr_t pc = frame.pc();

  // Consider a thread sleeping, if it has been interrupted in the middle of
  // syscall execution, either when PC points to the syscall instruction, or if
  // syscall has just returned with EINTR
  if (StackFrame::isSyscall((instruction_t *)pc)) {
    return THREAD_SLEEPING;
  }

  // Make sure the previous instruction address is readable
  uintptr_t prev_pc = pc - SYSCALL_SIZE;
  if ((pc & 0xfff) >= SYSCALL_SIZE ||
      Profiler::instance()->findLibraryByAddress((instruction_t *)prev_pc) !=
          NULL) {
    if (StackFrame::isSyscall((instruction_t *)prev_pc) &&
        frame.checkInterruptedSyscall()) {
      return THREAD_SLEEPING;
    }
  }

  return THREAD_RUNNING;
}

long NoneEvent::adjustInterval(long interval, int thread_count) {
  if (thread_count > NONE_THREADS_PER_TICK) {
    interval /=
        (thread_count + NONE_THREADS_PER_TICK - 1) / NONE_THREADS_PER_TICK;
  }
  return interval;
}

void NoneEvent::signalHandler(int signo, siginfo_t *siginfo, void *ucontext) {
  if (!Profiler::instance()->isEventWriterThread()) {
    // std::cerr << "none event singnal handler" << std::endl;
    ExecutionEvent event(TSC::ticks());
    Profiler::instance()->recordSample(ucontext, 1, PERF_SAMPLE, &event);
  } else {
    std::cerr << "unhandling" << std::endl;
  }
}

Error NoneEvent::start(Arguments &args) {

  // Determine the signal to use
  // _signal = SIGPROF;
  _signal = args._signal == 0
                ? OS::getProfilingSignal(1)
                : ((args._signal >> 8) > 0 ? args._signal >> 8 : args._signal);

  // Log the signal being used
  std::cerr << "Using signal: " << _signal << std::endl;

  // Install the signal handler
  SigAction oldAction = OS::installSignalHandler(_signal, signalHandler);

  _running = true;

  if (pthread_create(&_thread, NULL, threadEntry, this) != 0) {
    return Error("Unable to create timer thread");
  }

  return Error::OK;
}

void NoneEvent::stop() {
  // OS::installSignalHandler(SIGPROF, NULL, SIG_IGN);
  _running = false;
  pthread_kill(_thread, WAKEUP_SIGNAL);
  pthread_join(_thread, NULL);
}

void NoneEvent::timerLoop() {
  int self = OS::threadId();
  ThreadFilter *thread_filter = Profiler::instance()->threadFilter();
  bool thread_filter_enabled = thread_filter->enabled();
  bool sample_idle_threads = _sample_idle_threads;

  ThreadList *thread_list = OS::listThreads();
  long long next_cycle_time = OS::nanotime();
  while (_running) {
    if (!_enabled) {
      OS::sleep(_interval);
      continue;
    }

    if (sample_idle_threads) {
      // Try to keep the wall clock interval stable, regardless of the number of
      // profiled threads
      int estimated_thread_count =
          thread_filter_enabled ? thread_filter->size() : thread_list->size();
      next_cycle_time += adjustInterval(_interval, estimated_thread_count);
    }

    for (int count = 0; count < NONE_THREADS_PER_TICK;) {
      int thread_id = thread_list->next();
      if (thread_id == -1) {
        thread_list->rewind();
        break;
      }

      if (thread_id == self ||
          (thread_filter_enabled && !thread_filter->accept(thread_id))) {
        continue;
      }

      if (sample_idle_threads || OS::threadState(thread_id) == THREAD_RUNNING) {
        if (OS::sendSignalToThread(thread_id, _signal)) {
          count++;
        }
      }
    }

    if (sample_idle_threads) {
      long long current_time = OS::nanotime();
      if (next_cycle_time - current_time > NONE_MIN_INTERVAL) {
        OS::sleep(next_cycle_time - current_time);
      } else {
        next_cycle_time = current_time + NONE_MIN_INTERVAL;
        OS::sleep(NONE_MIN_INTERVAL);
      }
    } else {
      OS::sleep(_interval);
    }
  }

  delete thread_list;
}

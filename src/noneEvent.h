#ifndef _NONEVENT_H
#define _NONEVENT_H

#include "engine.h"
#include <jvmti.h>
#include <pthread.h>
#include <signal.h>

class NoneEvent : public Engine {
private:
  static void signalHandler(int signo, siginfo_t *siginfo, void *ucontext);
  static long _interval;
  static int _signal;
  static bool _sample_idle_threads;

  volatile bool _running;
  pthread_t _thread;
  void timerLoop();
  static void *threadEntry(void *wall_clock) {
    ((NoneEvent *)wall_clock)->timerLoop();
    return NULL;
  }

  static ThreadState getThreadState(void *ucontext);
  static long adjustInterval(long interval, int thread_count);

public:
  const char *name() { return EVENT_NONE; }

  const char *units() { return "N/A"; }

  Error start(Arguments &args);
  void stop();
};

#endif // _NONEVENT_H

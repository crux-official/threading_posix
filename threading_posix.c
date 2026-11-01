#include "crux_threading.h"
#include "extension.h"
#include "kernel_types.h"
#include "logging.h"
#include "memory.h"
#include "threading_api.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

// The POSIX threading PRIMITIVE — Linux and Darwin: pthreads are the
// reference on both, so one file serves both (no platform branch in the
// thread calls, the time_posix pattern). This is the PRIMITIVE, not a pool:
// Thread_Create spawns ONE real pthread the caller tracks by the returned
// opaque handle and releases with Thread_Join/Detach; the hardware facts
// (logical/physical core counts, the concurrency ceiling) are reported. The
// worker set + FIFO queue + batch barrier that composes a pool on top is the
// thread_pool standalone (backlog 0037) — it is NOT in this file.
//
// Ownership (race-free by construction):
//  - the WORK ITEM (fn + arg) is owned by the thread after Create and freed
//    by the thread on exit (only the thread touches it after Create);
//  - the HANDLE (the pthread_t the caller tracks) is owned by the caller and
//    freed by the caller — Thread_Join after pthread_join, Thread_Detach
//    after pthread_detach. The thread never touches the handle, so there is
//    no use-after-free and no leak on either the join or detach path.

// The opaque handle: the platform thread id, wrapped so the caller tracks an
// opaque pointer it passes back (never dereferenced).
struct CruxThread {
  pthread_t handle;
};

// The unit of work the thread consumes (fn + arg), owned by the thread after
// Create. Kept separate from the handle so the caller can free the handle
// without the (possibly still-running) thread's work item going stale.
typedef struct TpxWork {
  void (*fn)(void *arg);
  void *arg;
} TpxWork;

// The thread's entry: run the work, then reclaim the work item (the handle is
// the caller's; the thread never frees it).
static void *tpx_thread_main(void *arg) {
  TpxWork *work = (TpxWork *)arg;
  work->fn(work->arg);
  Memory_Free(work);
  return NULL;
}

CruxThread *Thread_Create(void (*func)(void *arg), void *arg) {
  if (func == NULL) {
    return NULL;
  }
  CruxThread *thread = (CruxThread *)Memory_Calloc(1, sizeof *thread);
  TpxWork *work = (TpxWork *)Memory_Calloc(1, sizeof *work);
  if (thread == NULL || work == NULL) {
    if (thread != NULL) {
      Memory_Free(thread);
    }
    if (work != NULL) {
      Memory_Free(work);
    }
    LogError("[threading_posix] Create: allocation failed (handle or work item)");
    return NULL;
  }
  work->fn = func;
  work->arg = arg;
  if (pthread_create(&thread->handle, NULL, tpx_thread_main, work) != 0) {
    Memory_Free(thread);
    Memory_Free(work);
    LogError("[threading_posix] Create: the OS refused the thread");
    return NULL;
  }
  return thread;
}

void Thread_Join(CruxThread *thread) {
  if (thread == NULL) {
    return;
  }
  pthread_join(thread->handle, NULL); // the thread reclaimed its work item on exit
  Memory_Free(thread);                // the caller owns the handle (the thread never frees it)
}

void Thread_Detach(CruxThread *thread) {
  if (thread == NULL) {
    return;
  }
  pthread_detach(thread->handle); // the thread's work item reclaims itself on exit
  Memory_Free(thread);            // the caller owns the handle; the thread no longer needs the id
}

// --- The hardware facts (the "truth in hand").

// Logical cores: always a true fact (no platform estimates it).
int Thread_Count_Logical(void) {
  long count = sysconf(_SC_NPROCESSORS_ONLN);
  return (count > 0) ? (int)count : 1;
}

#ifdef __APPLE__
// Darwin: the physical core count is a direct sysctl (no topology parsing).
static int tpx_physical_cores_darwin(void) {
  int value = 0;
  size_t size = sizeof value;
  if (sysctlbyname("hw.physicalcpu", &value, &size, NULL, 0) == 0 && value > 0) {
    return value;
  }
  return Thread_Count_Logical(); // the honest fallback
}
#else
// Linux: count unique (physical_package_id, core_id) pairs over the online
// CPUs — the sysfs topology. O(n^2) over the online set (n is small: a few
// hundred at most), which is fine for a one-time hardware fact.
static int tpx_read_int(const char *path) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return -1;
  }
  int value = -1;
  // The topology files are single small integers; fscanf is the right tool
  // here (a strtol would need a scratch buffer + its own error dance for no
  // gain). The conversion is checked (the != 1 guard) even though the
  // check's preference for strtol is for the no-checker string case.
  if (fscanf(file, "%d", &value) != 1) { // NOLINT(cert-err34-c, bugprone-unchecked-string-to-number-conversion)
    value = -1;
  }
  (void)fclose(file); // best-effort close on a read-only topology file; the value is already captured
  return value;
}

static int tpx_physical_cores_linux(void) {
  const long online = sysconf(_SC_NPROCESSORS_ONLN);
  if (online <= 0) {
    return 1;
  }
  int *packages = (int *)Memory_Calloc((size_t)online, sizeof(int));
  int *cores = (int *)Memory_Calloc((size_t)online, sizeof(int));
  if (packages == NULL || cores == NULL) {
    if (packages != NULL) {
      Memory_Free(packages);
    }
    if (cores != NULL) {
      Memory_Free(cores);
    }
    return Thread_Count_Logical(); // the honest fallback (a failed read is not a fact)
  }
  char path[128];
  int count = 0;
  for (long cpu = 0; cpu < online; cpu++) {
    (void)snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%ld/topology/physical_package_id", cpu);
    int pkg = tpx_read_int(path);
    (void)snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%ld/topology/core_id", cpu);
    int core = tpx_read_int(path);
    if (pkg < 0 || core < 0) {
      // A topology file missing (an unusual kernel/config): fall back to
      // treating each logical CPU as its own core (an over-count, but a
      // HONEST one — never under-count the cores we can actually see).
      pkg = 0;
      core = (int)cpu;
    }
    int dup = 0;
    for (int i = 0; i < count; i++) {
      if (packages[i] == pkg && cores[i] == core) {
        dup = 1;
        break;
      }
    }
    if (!dup) {
      packages[count] = pkg;
      cores[count] = core;
      count++;
    }
  }
  Memory_Free(packages);
  Memory_Free(cores);
  return (count > 0) ? count : 1;
}
#endif

int Thread_Count_Physical(void) {
  // A one-time hardware fact (it does not change over the process's life), but
  // the pool asks for it AND for the concurrency (== physical on native) at
  // Create — derive it once so the second is free. Create runs single-threaded
  // at boot (before any worker exists), so a plain static is race-free.
  static int cached = 0;
  if (cached == 0) {
    cached =
#ifdef __APPLE__
        tpx_physical_cores_darwin();
#else
        tpx_physical_cores_linux();
#endif
  }
  return cached;
}

// The concurrency ceiling: one real thread per physical core — the
// "probably physical" one-per-core ceiling the pool keys off. (v1 caps the
// pool at physical; capping further by the process's affinity mask — a
// container/cgroup restriction — and allowing a deliberate oversubscription
// beyond it are both later refinements.)
int Thread_Concurrency(void) {
  return Thread_Count_Physical();
}

// The served struct — valid from load, before Construct. The package's
// identity is the manifest's (the implementation targets carry no baked
// identity defines — the org's convention, the time_posix shape).
static ThreadingAPI g_threading_api = {
    .version = {1, 0, 0},
    .Thread_Create = Thread_Create,
    .Thread_Join = Thread_Join,
    .Thread_Detach = Thread_Detach,
    .Thread_Count_Logical = Thread_Count_Logical,
    .Thread_Count_Physical = Thread_Count_Physical,
    .Thread_Concurrency = Thread_Concurrency,
};

static bool threading_posix_construct(void) {
  Engine_RegisterConcern(CRUX_CONCERN_THREADING);
  return true;
}

static void *threading_posix_get_api(void) {
  return &g_threading_api;
}

ExtensionInterface g_extension_threading_posix = {
    "threading_posix",
    {1, 0, 0},
    threading_posix_construct,
    NULL,
    NULL,
    NULL,
    NULL,
    threading_posix_get_api,
};

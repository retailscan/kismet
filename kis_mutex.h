/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/* 
 * So it turns out GCC < 4.9.0 has a broken std::timex_mutex implementation
 * which makes it unusable (well, unusable in a timed fashion).
 *
 * Since Kismet needs to support older implementations, we need to work around
 * this by detecting it and implementing our own mutex on those systems.
 */

#ifndef __KISMET_MUTEX_H__
#define __KISMET_MUTEX_H__

#include "config.h"

#include <mutex>
#include <chrono>

// Seconds a lock is allowed to be held before throwing a timeout error
#define KIS_THREAD_DEADLOCK_TIMEOUT     15

// Optionally force the custom c++ workaround mutex
#define ALWAYS_USE_KISMET_MUTEX         0

// Some compilers (older openwrt CC images, Ubuntu 14.04) are still in use and have a broken
// std::recursive_timed_mutex implementation which uses the wrong precision for the timer leading
// to an instant timer failure;  Optionally re-implement a std::mutex using C pthread 
// primitives and locking
class kis_recursive_pthread_timed_mutex {
public:
    kis_recursive_pthread_timed_mutex() {
        // Make a recursive mutex that the owning thread can lock multiple times;
        // Required to allow a timer event to reschedule itself on completion
        pthread_mutexattr_t mutexattr;
        pthread_mutexattr_init(&mutexattr);
        pthread_mutexattr_settype(&mutexattr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&mutex, &mutexattr);
    }
        
    ~kis_recursive_pthread_timed_mutex() {
        pthread_mutex_destroy(&mutex);
    }

    bool try_lock_for(const std::chrono::seconds& d) {
#if defined(HAVE_PTHREAD_TIMELOCK) && !defined(DISABLE_MUTEX_TIMEOUT)
        // Only use timeouts if a) they're supported and b) not disabled in configure
        struct timespec t;

        clock_gettime(CLOCK_REALTIME, &t); 
        t.tv_sec += d.count();

        if (pthread_mutex_timedlock(&mutex, &t) != 0) {
            return false;
        }
#else
        pthread_mutex_lock(&mutex);
#endif

        return true;
    }

    void lock() {
        pthread_mutex_lock(&mutex);
    }

    void unlock() {
        pthread_mutex_unlock(&mutex);
    }

private:
    pthread_mutex_t mutex;
};

// Use std::recursive_timed_mutex components when we can, unless we're forcing pthread mode; base it
// on the GCC versions to detect broken compilers
#if ALWAYS_USE_KISMET_MUTEX != 0 || \
    (!defined(__clang__) && defined (GCC_VERSION_MAJOR) && (GCC_VERSION_MAJOR < 4 || \
        (GCC_VERSION_MAJOR == 4 && GCC_VERSION_MINOR < 9)))
using kis_recursive_timed_mutex = kis_recursive_pthread_timed_mutex;
#else
using kis_recursive_timed_mutex = std::recursive_timed_mutex;
#endif

// A scoped locker like std::lock_guard that provides RAII scoped locking of a kismet mutex;
// unless disabled in ./configure use a timed lock mutex and throw an exception if unable
// to acquire the lock within KIS_THREAD_DEADLOCK_TIMEOUT seconds, it's better to crash
// than to hang; we allow a short-cut unlock to unlock before the end of scope, in which case
// we no longer unlock AGAIN at descope
class local_locker {
public:
    local_locker(kis_recursive_timed_mutex *in) {
        cpplock = in;
       
#ifdef DISABLE_MUTEX_TIMEOUT
        cpplock->lock();
#else
        if (!cpplock->try_lock_for(std::chrono::seconds(KIS_THREAD_DEADLOCK_TIMEOUT))) {
            throw(std::runtime_error("deadlocked thread: mutex not available w/in timeout"));
        }
#endif
    }

    void unlock() {
        if (cpplock != NULL) {
            cpplock->unlock();
            cpplock = NULL;
        }
    }

    ~local_locker() {
        if (cpplock != NULL)
            cpplock->unlock();
    }

protected:
    kis_recursive_timed_mutex *cpplock;
};


// RAII-style scoped locker, but only locks on demand, not creation
class local_demand_locker {
public:
    local_demand_locker(kis_recursive_timed_mutex *in) {
        cpplock = in;
        hold_lock = false;
    }

    void unlock() {
        if (!hold_lock)
            return;

        if (cpplock != NULL)
            cpplock->unlock();

        hold_lock = false;
    }

    void lock() {
        if (hold_lock)
            throw(std::runtime_error("possible deadlock - demand_locker locking while already holding a lock"));

        if (cpplock != NULL) {
#ifdef DISABLE_MUTEX_TIMEOUT
            cpplock->lock();
#else
            if (!cpplock->try_lock_for(std::chrono::seconds(KIS_THREAD_DEADLOCK_TIMEOUT))) {
                throw(std::runtime_error("deadlocked thread: mutex not available w/in timeout"));
            }
#endif
            hold_lock = true;
        }
    }

    ~local_demand_locker() {
        unlock();
    }

protected:
    kis_recursive_timed_mutex *cpplock;
    bool hold_lock;

};

// Act as a scoped locker on a mutex that never expires; used for performing
// end-of-life mutex maintenance
class local_eol_locker {
public:
    local_eol_locker(kis_recursive_timed_mutex *in) {
#ifdef DISABLE_MUTEX_TIMEOUT
        in->lock();
#else
        if (!in->try_lock_for(std::chrono::seconds(KIS_THREAD_DEADLOCK_TIMEOUT))) {
            throw(std::runtime_error("deadlocked thread: mutex not available w/in timeout"));
        }
#endif
    }

    ~local_eol_locker() { }
};

// Act as a scope-based unlocker; assuming a mutex is already locked, unlock
// when it leaves scope
class local_unlocker {
public:
    local_unlocker(kis_recursive_timed_mutex *in) {
        cpplock = in;
    }

    void unlock() {
        if (cpplock != NULL)
            cpplock->unlock();
    }

    ~local_unlocker() {
        if (cpplock != NULL)
            cpplock->unlock();
    }

protected:
    kis_recursive_timed_mutex *cpplock;
};

#endif


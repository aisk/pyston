// Copyright (c) 2014-2016 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <pthread.h>
#include <stddef.h>

#include "Python.h"
#include "pythread.h"

#include "capi/typeobject.h"
#include "capi/types.h"
#include "core/threading.h"
#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

using namespace pyston::threading;

std::atomic_long nb_threads;

namespace pyston {

static Box* allocateLock();
static Box* releaseLock(Box* lock);

// The lock returned by _thread._set_sentinel(), which gets released when the thread exits.
// threading.Thread.join() waits on it.
static __thread Box* thread_sentinel = NULL;

static void* thread_start(STOLEN(Box*) target, STOLEN(Box*) varargs, STOLEN(Box*) kwargs) {
    assert(target);
    assert(varargs);

    AUTO_DECREF(target);
    AUTO_DECREF(varargs);
    AUTO_XDECREF(kwargs);

#if STAT_TIMERS
    // TODO: maybe we should just not log anything for threads...
    static uint64_t* timer_counter = Stats::getStatCounter("us_timer_thread_start");
    StatTimer timer(timer_counter, 0, true);
    timer.pushTopLevel(getCPUTicks());
#endif

    ++nb_threads;

    try {
        autoDecref(runtimeCall(target, ArgPassSpec(0, 0, true, kwargs != NULL), varargs, kwargs, NULL, NULL, NULL));
    } catch (ExcInfo e) {
        if (!e.matches(SystemExit))
            e.printExcAndTraceback();
        e.clear();
    }

#if STAT_TIMERS
    timer.popTopLevel(getCPUTicks());
#endif

    if (thread_sentinel) {
        releaseLock(thread_sentinel);
        Py_CLEAR(thread_sentinel);
    }

    --nb_threads;

    return NULL;
}

// TODO this should take kwargs, which defaults to empty
Box* startNewThread(Box* target, Box* args, Box* kw) {
    intptr_t thread_id = start_thread(&thread_start, incref(target), incref(args), xincref(kw));
    return boxInt(thread_id);
}

#define CHECK_STATUS(name)                                                                                             \
    if (status != 0) {                                                                                                 \
        perror(name);                                                                                                  \
        error = 1;                                                                                                     \
    }

/*
 * As of February 2002, Cygwin thread implementations mistakenly report error
 * codes in the return value of the sem_ calls (like the pthread_ functions).
 * Correct implementations return -1 and put the code in errno. This supports
 * either.
 *
 * NOTE (2015-05-14): According to `man pthread_mutex_lock` on my system (Ubuntu
 * 14.10), returning the error code is expected behavior. - rntz
 */
static int fix_status(int status) {
    return (status == -1) ? errno : status;
}

static BoxedClass* ThreadError;
static BoxedClass* thread_lock_cls;
class BoxedThreadLock : public Box {
private:
    PyThread_type_lock lock_lock;

public:
    BoxedThreadLock() { lock_lock = PyThread_allocate_lock(); }

    DEFAULT_CLASS_SIMPLE(thread_lock_cls, false);

    static Box* acquire(Box* _self, Box* _blocking, Box* _timeout) {
        RELEASE_ASSERT(_self->cls == thread_lock_cls, "");
        BoxedThreadLock* self = static_cast<BoxedThreadLock*>(_self);
        return boxBool(acquireTimed(self->lock_lock, _blocking, _timeout) == PY_LOCK_ACQUIRED);
    }

    // Parses the blocking and timeout arguments of acquire() and tries to take the lock.
    static PyLockStatus acquireTimed(PyThread_type_lock lock, Box* _blocking, Box* _timeout) {
        int blocking = PyObject_IsTrue(_blocking);
        if (blocking < 0)
            throwCAPIException();
        double timeout = PyFloat_AsDouble(_timeout);
        if (timeout == -1 && PyErr_Occurred())
            throwCAPIException();
        if (!blocking && timeout != -1)
            raiseExcHelper(ValueError, "can't specify a timeout for a non-blocking call");
        if (timeout < 0 && timeout != -1)
            raiseExcHelper(ValueError, "timeout value must be positive");

        PY_TIMEOUT_T microseconds;
        if (!blocking)
            microseconds = 0;
        else if (timeout == -1)
            microseconds = -1;
        else
            microseconds = (PY_TIMEOUT_T)(timeout * 1e6);

        PyLockStatus rtn;
        {
            threading::GLAllowThreadsReadRegion _allow_threads;

            rtn = PyThread_acquire_lock_timed(lock, microseconds, 0);
        }

        return rtn;
    }

    static Box* enter(Box* _self) { return acquire(_self, Py_True, autoDecref(boxInt(-1))); }

    static Box* release(Box* _self) {
        RELEASE_ASSERT(_self->cls == thread_lock_cls, "");
        BoxedThreadLock* self = static_cast<BoxedThreadLock*>(_self);

        if (PyThread_acquire_lock(self->lock_lock, 0)) {
            PyThread_release_lock(self->lock_lock);
            raiseExcHelper(ThreadError, "release unlocked lock");
            return incref(Py_None);
        }

        PyThread_release_lock(self->lock_lock);
        return incref(Py_None);
    }

    static Box* exit(Box* _self, Box* arg1, Box* arg2, Box** args) { return release(_self); }

    static void dealloc(Box* _self) {
        RELEASE_ASSERT(_self->cls == thread_lock_cls, "");
        BoxedThreadLock* self = static_cast<BoxedThreadLock*>(_self);

        if (self->lock_lock != NULL) {
            /* Unlock the lock so it's safe to free it */
            PyThread_acquire_lock(self->lock_lock, 0);
            PyThread_release_lock(self->lock_lock);

            PyThread_free_lock(self->lock_lock);
            self->lock_lock = NULL;
        }

        _self->cls->tp_free(_self);
    }

    static Box* locked(Box* _self) {
        RELEASE_ASSERT(_self->cls == thread_lock_cls, "");
        BoxedThreadLock* self = static_cast<BoxedThreadLock*>(_self);

        if (PyThread_acquire_lock(self->lock_lock, 0)) {
            PyThread_release_lock(self->lock_lock);
            Py_RETURN_FALSE;
        }
        Py_RETURN_TRUE;
    }
};


// Port of CPython's rlockobject from Modules/_threadmodule.c
static BoxedClass* thread_rlock_cls;
class BoxedThreadRLock : public Box {
private:
    PyThread_type_lock rlock_lock;
    unsigned long rlock_owner = 0;
    unsigned long rlock_count = 0;

public:
    BoxedThreadRLock() { rlock_lock = PyThread_allocate_lock(); }

    DEFAULT_CLASS_SIMPLE(thread_rlock_cls, false);

    static BoxedThreadRLock* check(Box* self) {
        if (!PyObject_TypeCheck(self, thread_rlock_cls))
            raiseExcHelper(TypeError, "descriptor requires a '_thread.RLock' object but received a '%s'",
                           getTypeName(self));
        return static_cast<BoxedThreadRLock*>(self);
    }

    static Box* tpNew(BoxedClass* cls) {
        if (cls != thread_rlock_cls)
            raiseExcHelper(TypeError, "subclassing _thread.RLock is not supported");
        BoxedThreadRLock* self = new BoxedThreadRLock();
        if (self->rlock_lock == NULL) {
            Py_DECREF(self);
            raiseExcHelper(ThreadError, "can't allocate lock");
        }
        return self;
    }

    static Box* acquire(Box* _self, Box* _blocking, Box* _timeout) {
        BoxedThreadRLock* self = check(_self);
        unsigned long tid = PyThread_get_thread_ident();
        if (self->rlock_count > 0 && tid == self->rlock_owner) {
            unsigned long count = self->rlock_count + 1;
            if (count <= self->rlock_count)
                raiseExcHelper(OverflowError, "Internal lock count overflowed");
            self->rlock_count = count;
            Py_RETURN_TRUE;
        }
        if (BoxedThreadLock::acquireTimed(self->rlock_lock, _blocking, _timeout) != PY_LOCK_ACQUIRED)
            Py_RETURN_FALSE;
        assert(self->rlock_count == 0);
        self->rlock_owner = tid;
        self->rlock_count = 1;
        Py_RETURN_TRUE;
    }

    static Box* enter(Box* _self) { return acquire(_self, Py_True, autoDecref(boxInt(-1))); }

    static Box* release(Box* _self) {
        BoxedThreadRLock* self = check(_self);
        unsigned long tid = PyThread_get_thread_ident();
        if (self->rlock_count == 0 || self->rlock_owner != tid)
            raiseExcHelper(RuntimeError, "cannot release un-acquired lock");
        if (--self->rlock_count == 0) {
            self->rlock_owner = 0;
            PyThread_release_lock(self->rlock_lock);
        }
        Py_RETURN_NONE;
    }

    static Box* exit(Box* _self, Box* arg1, Box* arg2, Box** args) { return release(_self); }

    static Box* acquireRestore(Box* _self, Box* state) {
        BoxedThreadRLock* self = check(_self);
        unsigned long owner, count;
        if (!PyArg_ParseTuple(state, "kk:_acquire_restore", &count, &owner))
            throwCAPIException();
        int r = PyThread_acquire_lock(self->rlock_lock, 0);
        if (!r) {
            threading::GLAllowThreadsReadRegion _allow_threads;
            r = PyThread_acquire_lock(self->rlock_lock, 1);
        }
        if (!r)
            raiseExcHelper(ThreadError, "couldn't acquire lock");
        assert(self->rlock_count == 0);
        self->rlock_owner = owner;
        self->rlock_count = count;
        Py_RETURN_NONE;
    }

    static Box* releaseSave(Box* _self) {
        BoxedThreadRLock* self = check(_self);
        if (self->rlock_count == 0)
            raiseExcHelper(RuntimeError, "cannot release un-acquired lock");
        unsigned long owner = self->rlock_owner;
        unsigned long count = self->rlock_count;
        self->rlock_count = 0;
        self->rlock_owner = 0;
        PyThread_release_lock(self->rlock_lock);
        return Py_BuildValue("kk", count, owner);
    }

    static Box* isOwned(Box* _self) {
        BoxedThreadRLock* self = check(_self);
        unsigned long tid = PyThread_get_thread_ident();
        return boxBool(self->rlock_count > 0 && self->rlock_owner == tid);
    }

    static Box* repr(Box* _self) {
        BoxedThreadRLock* self = check(_self);
        return PyUnicode_FromFormat("<%s %s object owner=%lu count=%lu at %p>",
                                    self->rlock_count ? "locked" : "unlocked", _self->cls->tp_name,
                                    self->rlock_owner, self->rlock_count, self);
    }

    static void dealloc(Box* _self) {
        BoxedThreadRLock* self = static_cast<BoxedThreadRLock*>(_self);
        if (self->rlock_lock != NULL) {
            /* Unlock the lock so it's safe to free it */
            if (self->rlock_count > 0)
                PyThread_release_lock(self->rlock_lock);
            PyThread_free_lock(self->rlock_lock);
        }
        _self->cls->tp_free(_self);
    }
};

static Box* allocateLock() {
    return new BoxedThreadLock();
}

static Box* releaseLock(Box* lock) {
    Box* is_locked = BoxedThreadLock::locked(lock);
    AUTO_DECREF(is_locked);
    if (is_locked == Py_True)
        autoDecref(BoxedThreadLock::release(lock));
    return lock;
}

static Box* setSentinel() {
    Box* lock = allocateLock();
    Py_XDECREF(thread_sentinel);
    thread_sentinel = incref(lock);
    return lock;
}

Box* getIdent() {
    return boxInt(pthread_self());
}

Box* stackSize(Box* arg) {
    if (arg) {
        if (PyInt_Check(arg) && PyInt_AS_LONG(arg) == 0) {
            Py_RETURN_NONE;
        }
        raiseExcHelper(ThreadError, "Changing initial stack size is not supported in Pyston");
    }
    return boxInt(0);
}

Box* threadCount() {
    return boxInt(nb_threads);
}

static PyObject* thread_PyThread_exit_thread(PyObject* self) noexcept {
    PyErr_SetNone(PyExc_SystemExit);
    return NULL;
}

PyDoc_STRVAR(exit_doc, "exit()\n\
(exit_thread() is an obsolete synonym)\n\
\n\
This is synonymous to ``raise SystemExit''.  It will cause the current\n\
thread to exit silently unless the exception is caught.");

static PyMethodDef thread_methods[] = {
    { "exit_thread", (PyCFunction)thread_PyThread_exit_thread, METH_NOARGS, exit_doc },
    { "exit", (PyCFunction)thread_PyThread_exit_thread, METH_NOARGS, exit_doc },
};
}

using namespace pyston;

extern "C" PyObject* PyInit__thread() {
    PyThread_init_thread();

    BoxedModule* thread_module = createModule(autoDecref(boxString("_thread")));

    thread_module->giveAttr(
        "start_new_thread",
        new BoxedBuiltinFunctionOrMethod(
            BoxedCode::create((void*)startNewThread, BOXED_INT, 3, false, false, "start_new_thread"), { NULL }));
    thread_module->giveAttrBorrowed("start_new", thread_module->getattr(getStaticString("start_new_thread")));

    thread_module->giveAttr("allocate_lock", new BoxedBuiltinFunctionOrMethod(
                                                 BoxedCode::create((void*)allocateLock, UNKNOWN, 0, "allocate_lock")));
    thread_module->giveAttrBorrowed("allocate", thread_module->getattr(getStaticString("allocate_lock")));
    thread_module->giveAttr(
        "get_ident", new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)getIdent, BOXED_INT, 0, "get_ident")));
    thread_module->giveAttr("stack_size", new BoxedBuiltinFunctionOrMethod(
                                              BoxedCode::create((void*)stackSize, UNKNOWN, 1, "stack_size"), { NULL }));
    thread_module->giveAttr(
        "_count", new BoxedBuiltinFunctionOrMethod(BoxedCode::create((void*)threadCount, BOXED_INT, 0, "_count")));
    thread_module->giveAttr("_set_sentinel", new BoxedBuiltinFunctionOrMethod(BoxedCode::create(
                                                 (void*)setSentinel, UNKNOWN, 0, "_set_sentinel")));
    thread_module->giveAttr("TIMEOUT_MAX", boxFloat((double)PY_TIMEOUT_MAX / 1e6));

    thread_lock_cls = BoxedClass::create(type_cls, object_cls, 0, 0, sizeof(BoxedThreadLock), false, "lock", true,
                                         BoxedThreadLock::dealloc, NULL, false);
    thread_lock_cls->instances_are_nonzero = true;

    thread_lock_cls->giveAttr("__module__", boxString("_thread"));
    thread_lock_cls->giveAttr("acquire",
                              new BoxedFunction(BoxedCode::create((void*)BoxedThreadLock::acquire, BOXED_BOOL, 3, false,
                                                                  false, "lock.acquire", "",
                                                                  ParamNames({ "self", "blocking", "timeout" }, "", "")),
                                                { Py_True, autoDecref(boxInt(-1)) }));
    thread_lock_cls->giveAttr("release", new BoxedFunction(BoxedCode::create((void*)BoxedThreadLock::release, NONE, 1,
                                                                             "lock.release")));
    thread_lock_cls->giveAttrBorrowed("acquire_lock", thread_lock_cls->getattr(getStaticString("acquire")));
    thread_lock_cls->giveAttrBorrowed("release_lock", thread_lock_cls->getattr(getStaticString("release")));
    thread_lock_cls->giveAttr("__enter__", new BoxedFunction(BoxedCode::create((void*)BoxedThreadLock::enter,
                                                                               BOXED_BOOL, 1, "lock.__enter__")));
    thread_lock_cls->giveAttr("__exit__", new BoxedFunction(BoxedCode::create((void*)BoxedThreadLock::exit, NONE, 4,
                                                                              "lock.__exit__")));
    thread_lock_cls->giveAttr("locked", new BoxedFunction(BoxedCode::create((void*)BoxedThreadLock::locked, BOXED_BOOL,
                                                                            1, "lock.locked")));
    thread_lock_cls->giveAttrBorrowed("locked_lock", thread_lock_cls->getattr(getStaticString("locked")));
    thread_lock_cls->freeze();
    thread_module->giveAttrBorrowed("LockType", thread_lock_cls);

    thread_rlock_cls = BoxedClass::create(type_cls, object_cls, 0, 0, sizeof(BoxedThreadRLock), false, "RLock", true,
                                          BoxedThreadRLock::dealloc, NULL, false);
    thread_rlock_cls->instances_are_nonzero = true;
    thread_rlock_cls->giveAttr("__module__", boxString("_thread"));
    thread_rlock_cls->giveAttr("__new__", new BoxedFunction(BoxedCode::create((void*)BoxedThreadRLock::tpNew, UNKNOWN,
                                                                              1, "RLock.__new__")));
    thread_rlock_cls->giveAttr("acquire",
                               new BoxedFunction(BoxedCode::create((void*)BoxedThreadRLock::acquire, BOXED_BOOL, 3, false,
                                                                   false, "RLock.acquire", "",
                                                                   ParamNames({ "self", "blocking", "timeout" }, "", "")),
                                                 { Py_True, autoDecref(boxInt(-1)) }));
    thread_rlock_cls->giveAttr("release", new BoxedFunction(BoxedCode::create((void*)BoxedThreadRLock::release, NONE, 1,
                                                                              "RLock.release")));
    thread_rlock_cls->giveAttr("__enter__", new BoxedFunction(BoxedCode::create((void*)BoxedThreadRLock::enter,
                                                                                BOXED_BOOL, 1, "RLock.__enter__")));
    thread_rlock_cls->giveAttr("__exit__", new BoxedFunction(BoxedCode::create((void*)BoxedThreadRLock::exit, NONE, 4,
                                                                               "RLock.__exit__")));
    thread_rlock_cls->giveAttr("_acquire_restore",
                               new BoxedFunction(BoxedCode::create((void*)BoxedThreadRLock::acquireRestore, NONE, 2,
                                                                   "RLock._acquire_restore")));
    thread_rlock_cls->giveAttr("_release_save", new BoxedFunction(BoxedCode::create(
                                                    (void*)BoxedThreadRLock::releaseSave, UNKNOWN, 1,
                                                    "RLock._release_save")));
    thread_rlock_cls->giveAttr("_is_owned", new BoxedFunction(BoxedCode::create((void*)BoxedThreadRLock::isOwned,
                                                                                BOXED_BOOL, 1, "RLock._is_owned")));
    thread_rlock_cls->giveAttr("__repr__", new BoxedFunction(BoxedCode::create((void*)BoxedThreadRLock::repr, STR, 1,
                                                                               "RLock.__repr__")));
    thread_rlock_cls->freeze();
    thread_module->giveAttrBorrowed("RLock", thread_rlock_cls);

    // In Python 3, thread.error is RuntimeError
    ThreadError = (BoxedClass*)PyExc_RuntimeError;
    thread_module->giveAttrBorrowed("error", ThreadError);

    auto thread_str = getStaticString("_thread");
    for (auto& md : thread_methods) {
        thread_module->giveAttr(md.ml_name, new BoxedCApiFunction(&md, NULL, thread_str));
    }
    return incref(thread_module);
}

// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SKY_ENGINE_BINDINGS_CORE_V8_SCRIPTPROMISERESOLVER_H_
#define SKY_ENGINE_BINDINGS_CORE_V8_SCRIPTPROMISERESOLVER_H_

#include "sky/engine/bindings/core/v8/ScopedPersistent.h"
#include "sky/engine/bindings/core/v8/ScriptPromise.h"
#include "sky/engine/bindings/core/v8/ScriptState.h"
#include "sky/engine/bindings/core/v8/V8Binding.h"
#include "sky/engine/core/dom/ActiveDOMObject.h"
#include "sky/engine/core/dom/ExecutionContext.h"
#include "sky/engine/platform/Timer.h"
#include "sky/engine/wtf/RefCounted.h"
#include "v8/include/v8.h"

namespace blink {

// This class wraps v8::Promise::Resolver and provides the following
// functionalities.
//  - A ScriptPromiseResolver retains a ScriptState. A caller
//    can call resolve or reject from outside of a V8 context.
//  - This class is an ActiveDOMObject and keeps track of the associated
//    ExecutionContext state. When the ExecutionContext is suspended,
//    resolve or reject will be delayed. When it is stopped, resolve or reject
//    will be ignored.
class ScriptPromiseResolver : public ActiveDOMObject, public RefCounted<ScriptPromiseResolver> {
    WTF_MAKE_NONCOPYABLE(ScriptPromiseResolver);

public:
    static PassRefPtr<ScriptPromiseResolver> create(ScriptState* scriptState)
    {
        RefPtr<ScriptPromiseResolver> resolver = adoptRef(new ScriptPromiseResolver(scriptState));
        resolver->suspendIfNeeded();
        return resolver.release();
    }

    virtual ~ScriptPromiseResolver()
    {
        // This assertion fails if:
        //  - promise() is called at least once and
        //  - this resolver is destructed before it is resolved, rejected or
        //    the associated ExecutionContext is stopped.
        ASSERT(m_state == ResolvedOrRejected || !m_isPromiseCalled);
    }

    // Anything that can be passed to toV8Value can be passed to this function.
    template <typename T>
    void resolve(T value)
    {
        resolveOrReject(value, Resolving);
    }

    // Anything that can be passed to toV8Value can be passed to this function.
    template <typename T>
    void reject(T value)
    {
        resolveOrReject(value, Rejecting);
    }

    void resolve() { resolve(V8UndefinedType()); }
    void reject() { reject(V8UndefinedType()); }

    ScriptState* scriptState() { return m_scriptState.get(); }

    // Note that an empty ScriptPromise will be returned after resolve or
    // reject is called.
    ScriptPromise promise()
    {
#if ENABLE(ASSERT)
        m_isPromiseCalled = true;
#endif
        return m_resolver.promise();
    }

    ScriptState* scriptState() const { return m_scriptState.get(); }

    // ActiveDOMObject implementation.
    virtual void suspend() override;
    virtual void resume() override;
    virtual void stop() override;

    // Once this function is called this resolver stays alive while the
    // promise is pending and the associated ExecutionContext isn't stopped.
    void keepAliveWhilePending();

protected:
    // You need to call suspendIfNeeded after the construction because
    // this is an ActiveDOMObject.
    explicit ScriptPromiseResolver(ScriptState*);

private:
    typedef ScriptPromise::InternalResolver Resolver;
    enum ResolutionState {
        Pending,
        Resolving,
        Rejecting,
        ResolvedOrRejected,
    };
    enum LifetimeMode {
        Default,
        KeepAliveWhilePending,
    };

    template<typename T>
    v8::Handle<v8::Value> toV8Value(const T& value)
    {
        return V8ValueTraits<T>::toV8Value(value, m_scriptState->context()->Global(), m_scriptState->isolate());
    }

    template <typename T>
    void resolveOrReject(T value, ResolutionState newState)
    {
        if (m_state != Pending || !executionContext() || executionContext()->activeDOMObjectsAreStopped())
            return;
        ASSERT(newState == Resolving || newState == Rejecting);
        m_state = newState;
        // Retain this object until it is actually resolved or rejected.
        // |deref| will be called in |clear|.
        ref();

        ScriptState::Scope scope(m_scriptState.get());
        m_value.set(m_scriptState->isolate(), toV8Value(value));
        if (!executionContext()->activeDOMObjectsAreSuspended())
            resolveOrRejectImmediately();
    }

    void resolveOrRejectImmediately();
    void onTimerFired(Timer<ScriptPromiseResolver>*);
    void clear();

    ResolutionState m_state;
    const RefPtr<ScriptState> m_scriptState;
    LifetimeMode m_mode;
    Timer<ScriptPromiseResolver> m_timer;
    Resolver m_resolver;
    ScopedPersistent<v8::Value> m_value;
#if ENABLE(ASSERT)
    // True if promise() is called.
    bool m_isPromiseCalled;
#endif
};

} // namespace blink

#endif  // SKY_ENGINE_BINDINGS_CORE_V8_SCRIPTPROMISERESOLVER_H_

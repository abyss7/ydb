#pragma once

#include <util/generic/intrlist.h>

namespace NActors {

    class IActor;

    /**
     * A type erased runnable item for local execution
     */
    class TActorRunnableItem : public TIntrusiveListItem<TActorRunnableItem> {
    public:
        template<class TDerived>
        class TImpl;

        inline void Run(IActor* actor) noexcept {
            (*RunFn)(this, actor);
        }

    private:
        // All subclasses must use TImpl
        TActorRunnableItem() = default;
        ~TActorRunnableItem() = default;

    protected:
        // A single function pointer is cheaper than a vtable, may be changed at runtime and allows multiple instances in a class
        void (*RunFn)(TActorRunnableItem*, IActor*) noexcept;
    };

}

#include "events.h"

namespace NFq {

NActors::TActorId MakeYqPrivateProxyId() {
    constexpr TStringBuf name = "YQPRIVPROXY";
    return NActors::TActorId(0, name);
}

} // namespace NFq

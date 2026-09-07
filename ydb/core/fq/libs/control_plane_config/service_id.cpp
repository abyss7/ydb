#include "control_plane_config.h"

namespace NFq {

NActors::TActorId ControlPlaneConfigActorId() {
    constexpr TStringBuf name = "FQCTLCFG";
    return NActors::TActorId(0, name);
}

} // namespace NFq

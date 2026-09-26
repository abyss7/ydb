LIBRARY()

SRCS(
    scan_diagnostics_actor.cpp  # gn: into ydb/core/tx/columnshard
)

PEERDIR(
    contrib/libs/apache/arrow
    contrib/libs/opentelemetry-proto
    ydb/core/base/generated
    ydb/core/control/lib/generated
    ydb/library/aclib/protos
    ydb/library/actors/core
    yql/essentials/core/issue/protos
)

RESOURCE(
    viz-global.js viz-global.js
)

END()

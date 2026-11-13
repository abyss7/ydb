LIBRARY()

# gn: into ydb/core/tx/schemeshard

SRCS(
    table.cpp
)

PEERDIR(
    ydb/core/protos
    ydb/core/tablet_flat
)

END()

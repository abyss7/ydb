LIBRARY()

# gn: into ydb/core/tx/columnshard/engines

SRCS(
    scheme.cpp
    counters.cpp
)

PEERDIR(
    ydb/core/tx/columnshard/engines/scheme/versions
)

END()

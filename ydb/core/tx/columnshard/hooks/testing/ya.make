LIBRARY()

SRCS(
    controller.cpp
    ro_controller.cpp
)

PEERDIR(
    ydb/core/testlib/basics
    ydb/core/tx/columnshard/hooks/abstract
    ydb/core/tx/columnshard/engines/changes
)

END()

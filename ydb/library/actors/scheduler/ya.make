LIBRARY()

SRCS(
    scheduler_actor.cpp
    scheduler_actor.h
)

PEERDIR(
    library/cpp/time_provider
    ydb/library/actors/core
    ydb/library/actors/interconnect/poller
)

END()

LIBRARY()

ADDINCL(
    ydb/public/sdk/cpp
)

SRCS(
    pq_ut_common.cpp  # gn: pq_ut_common
    pq_ut_common.h

    autoscaling_ut_common.cpp
    autoscaling_ut_common.h
)
# gn: pq_ut_common headers pq_ut_common.h

PEERDIR(
    ydb/core/persqueue
    ydb/core/testlib
)

YQL_LAST_ABI_VERSION()

END()

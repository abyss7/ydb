LIBRARY()

PROVIDES(YqlServicePolicy)
# gn: default provider

SRCS(
    GLOBAL udf_service.cpp
)

PEERDIR(
    yql/essentials/public/udf
)

YQL_LAST_ABI_VERSION()

END()

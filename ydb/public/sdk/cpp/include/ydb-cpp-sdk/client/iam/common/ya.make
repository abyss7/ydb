LIBRARY(client-iam-common-include)

SRCS(
    generic_provider.h
    types.h
)

PEERDIR(
    contrib/libs/grpc
    library/cpp/threading/future
    ydb/public/sdk/cpp/src/library/jwt
    ydb/public/sdk/cpp/src/client/types/credentials
)

END()

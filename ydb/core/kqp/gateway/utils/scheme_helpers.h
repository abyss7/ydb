#pragma once

// scheme_helpers перенесён в ydb/core/kqp/provider (его настоящий слой: зависит
// от provider'ского TIndexDescription из yql_kikimr_gateway.h и используется в
// самом provider'е). Forwarder оставлен, чтобы потребители по старому пути
// (kqp/gateway, host, executer_actor, query_compiler, behaviour/*) не менялись;
// символы приходят через их зависимость на provider. Перенос разорвал цикл
// provider <-> gateway:utils (provider дёргал CanonizePath/CreateIndexTablePath
// из scheme_helpers, а scheme_helpers.h тянул provider'ский заголовок).
#include <ydb/core/kqp/provider/scheme_helpers.h>

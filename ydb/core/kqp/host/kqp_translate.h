#pragma once

// kqp_translate перенесён в ydb/core/kqp/provider (его настоящий слой: он тянет
// yql_kikimr_provider/results и TKikimrConfiguration из provider, и сам
// используется provider'ом — прямое ребро provider -> host замыкало цикл
// provider <-> host). Forwarder оставлен, чтобы потребители по старому пути
// (kqp/host, kqp/compile_service) не менялись; символы приходят через
// host -> provider (public_deps).
#include <ydb/core/kqp/provider/kqp_translate.h>

#include "put_records_actor.h"

namespace NKikimr::NDataStreams::V1 {

void TPutRecordActor::SendResult(const Ydb::DataStreams::V1::PutRecordsResult& putRecordsResult, const TActorContext& ctx) {
    Ydb::DataStreams::V1::PutRecordResult result;

    if (putRecordsResult.failed_record_count() == 0) {
        result.set_sequence_number(putRecordsResult.records(0).sequence_number());
        result.set_shard_id(putRecordsResult.records(0).shard_id());
        result.set_encryption_type(Ydb::DataStreams::V1::EncryptionType::NONE);
        return ReplyWithResult(Ydb::StatusIds::SUCCESS, result, ctx);
    } else {
        if (putRecordsResult.records(0).error_code() == "ProvisionedThroughputExceededException"
            || putRecordsResult.records(0).error_code() == "ThrottlingException")
        {
            return ReplyWithError(Ydb::StatusIds::OVERLOADED, Ydb::PersQueue::ErrorCode::OVERLOAD, putRecordsResult.records(0).error_message());
        }
        //TODO: other codes - access denied and so on
        return ReplyWithError(Ydb::StatusIds::INTERNAL_ERROR, Ydb::PersQueue::ErrorCode::ERROR, putRecordsResult.records(0).error_message());

    }
}

} // namespace NKikimr::NDataStreams::V1

#include "datashard.h"

#include <ydb/core/formats/arrow/arrow_batch_builder.h>
#include <ydb/core/formats/arrow/arrow_helpers.h>
#include <ydb/core/scheme/scheme_tablecell.h>

#include <contrib/libs/apache/arrow/cpp/src/arrow/api.h>

namespace NKikimr {

TString TEvDataShard::TEvRead::ToString() const {
    TStringStream ss;
    ss << TBase::ToString();
    if (!Keys.empty()) {
        ss << " KeysSize: " << Keys.size();
    }
    if (!Ranges.empty()) {
        ss << " RangesSize: " << Ranges.size();
    }
    return ss.Str();
}

TEvDataShard::TEvRead* TEvDataShard::TEvRead::Load(const TEventSerializedData* data) {
    TEvRead* event = TBase::Load(data);
    auto& record = event->Record;

    event->Keys.reserve(record.KeysSize());
    for (const auto& key: record.GetKeys()) {
        event->Keys.emplace_back(key);
    }

    event->Ranges.reserve(record.RangesSize());
    for (const auto& range: record.GetRanges()) {
        event->Ranges.emplace_back(range);
    }

    return event;
}

// really ugly hacky, because Record is not mutable and calling members are const
void TEvDataShard::TEvRead::FillRecord() {
    if (!Keys.empty()) {
        Record.MutableKeys()->Reserve(Keys.size());
        for (auto& key: Keys) {
            Record.AddKeys(key.ReleaseBuffer());
        }
        Keys.clear();
    }

    if (!Ranges.empty()) {
        Record.MutableRanges()->Reserve(Ranges.size());
        for (auto& range: Ranges) {
            auto* pbRange = Record.AddRanges();
            range.Serialize(*pbRange);
        }
        Ranges.clear();
    }
}

TString TEvDataShard::TEvReadResult::ToString() const {
    TStringStream ss;
    ss << TBase::ToString();

    if (ArrowBatch) {
        ss << " ArrowRows: " << ArrowBatch->num_rows()
           << " ArrowCols: " << ArrowBatch->num_columns();
    }

    if (!Rows.empty()) {
        ss << " RowsSize: " << Rows.size();
    }

    return ss.Str();
}

TEvDataShard::TEvReadResult* TEvDataShard::TEvReadResult::Load(const TEventSerializedData* data) {
    TEvReadResult* event = TBase::Load(data);
    auto& record = event->Record;

    if (record.HasArrowBatch()) {
        const auto& batch = record.GetArrowBatch();
        auto schema = NArrow::DeserializeSchema(batch.GetSchema());
        event->ArrowBatch = NArrow::DeserializeBatch(batch.GetBatch(), schema);
        record.ClearArrowBatch();
    } else if (record.HasCellVec()) {
        auto& batch = *record.MutableCellVec();
        event->RowsSerialized.reserve(batch.RowsSize());
        for (auto& row: *batch.MutableRows()) {
            event->RowsSerialized.emplace_back(std::move(row));
        }
        record.ClearCellVec();
    }

    return event;
}

void TEvDataShard::TEvReadResult::FillRecord() {
    if (ArrowBatch) {
        auto* protoBatch = Record.MutableArrowBatch();
        protoBatch->SetSchema(NArrow::SerializeSchema(*ArrowBatch->schema()));
        protoBatch->SetBatch(NArrow::SerializeBatchNoCompression(ArrowBatch));
        ArrowBatch.reset();
        return;
    }

    if (!Batch.empty()) {
        auto* protoBatch = Record.MutableCellVec();
        protoBatch->MutableRows()->Reserve(Batch.Size());
        for (const auto& row: Batch) {
            protoBatch->AddRows(TSerializedCellVec::Serialize(row));
        }
        Batch = {};
        return;
    }

    if (!Rows.empty()) {
        auto* protoBatch = Record.MutableCellVec();
        protoBatch->MutableRows()->Reserve(Rows.size());
        for (const auto& row: Rows) {
            protoBatch->AddRows(TSerializedCellVec::Serialize(row));
        }
        Rows.clear();
        return;
    }
}

std::shared_ptr<arrow::RecordBatch> TEvDataShard::TEvReadResult::GetArrowBatch() const {
    return const_cast<TEvDataShard::TEvReadResult*>(this)->GetArrowBatch();
}

std::shared_ptr<arrow::RecordBatch> TEvDataShard::TEvReadResult::GetArrowBatch() {
    if (ArrowBatch)
        return ArrowBatch;

    if (Record.GetRowCount() == 0)
        return nullptr;

    ArrowBatch = NArrow::CreateNoColumnsBatch(Record.GetRowCount());
    return ArrowBatch;
}

}   // namespace NKikimr

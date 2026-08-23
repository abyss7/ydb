#include "write_with_blobs.h"

#include <ydb/core/tx/columnshard/engines/scheme/versions/abstract_scheme.h>

#include <ydb/core/formats/arrow/accessor/plain/accessor.h>
#include <ydb/core/formats/arrow/arrow_helpers.h>
#include <ydb/core/formats/arrow/serializer/native.h>
#include <ydb/core/tx/columnshard/engines/scheme/index_info.h>
#include <ydb/core/tx/columnshard/engines/storage/chunks/column.h>
#include <ydb/core/tx/columnshard/hooks/abstract/abstract.h>
#include <ydb/core/tx/columnshard/splitter/batch_slice.h>

namespace NKikimr::NOlap {

TConclusion<TWritePortionInfoWithBlobsResult> PrepareForWrite(const std::shared_ptr<ISnapshotSchema>& schema, const TInternalPathId pathId,
    const std::shared_ptr<arrow::RecordBatch>& incomingBatch, const NEvWrite::EModificationType mType,
    const std::shared_ptr<IStoragesManager>& storagesManager, const std::shared_ptr<NColumnShard::TSplitterCounters>& splitterCounters) {
    AFL_VERIFY(incomingBatch->num_rows());
    auto itIncoming = incomingBatch->schema()->fields().begin();
    auto itIncomingEnd = incomingBatch->schema()->fields().end();
    auto itIndex = schema->GetIndexInfo().ArrowSchema().begin();
    auto itIndexEnd = schema->GetIndexInfo().ArrowSchema().end();
    THashMap<ui32, std::vector<std::shared_ptr<IPortionDataChunk>>> chunks;

    std::shared_ptr<TDefaultSchemaDetails> schemaDetails(
        new TDefaultSchemaDetails(schema, std::make_shared<NArrow::NSplitter::TSerializationStats>()));

    while (itIncoming != itIncomingEnd && itIndex != itIndexEnd) {
        if ((*itIncoming)->name() == (*itIndex)->name()) {
            const ui32 incomingIndex = itIncoming - incomingBatch->schema()->fields().begin();
            const ui32 columnIndex = itIndex - schema->GetIndexInfo().ArrowSchema().begin();
            const ui32 columnId = schema->GetIndexInfo().GetColumnIdByIndexVerified(columnIndex);
            auto loader = schema->GetIndexInfo().GetColumnLoaderVerified(columnId);
            auto saver = schema->GetIndexInfo().GetColumnSaver(columnId);
            saver.AddSerializerWithBorder(100, NArrow::NSerialization::TNativeSerializer::GetUncompressed());
            saver.AddSerializerWithBorder(100000000, NArrow::NSerialization::TNativeSerializer::GetFast());
            const auto& columnFeatures = schema->GetIndexInfo().GetColumnFeaturesVerified(columnId);
            auto accessor = std::make_shared<NArrow::NAccessor::TTrivialArray>(incomingBatch->column(incomingIndex));
            TConclusion<std::shared_ptr<NArrow::NAccessor::IChunkedArray>> arrToWrite =
                loader->GetAccessorConstructor()->Construct(accessor, loader->BuildAccessorContext(accessor->GetRecordsCount()));
            if (arrToWrite.IsFail()) {
                AFL_ERROR(NKikimrServices::TX_COLUMNSHARD)("event", "cannot build accessor")("reason", arrToWrite.GetErrorMessage());
                return arrToWrite;
            }

            std::vector<std::shared_ptr<IPortionDataChunk>> columnChunks = { std::make_shared<NChunks::TChunkPreparation>(
                loader->GetAccessorConstructor()->SerializeToString(*arrToWrite, loader->BuildAccessorContext(accessor->GetRecordsCount())),
                *arrToWrite, TChunkAddress(columnId, 0), columnFeatures) };
            AFL_VERIFY(chunks.emplace(columnId, std::move(columnChunks)).second);
            ++itIncoming;
        }
        ++itIndex;
    }
    AFL_VERIFY(itIncoming == itIncomingEnd);

    TGeneralSerializedSlice slice(chunks, schemaDetails, splitterCounters);
    std::vector<TSplittedBlob> blobs;
    if (!slice.GroupBlobs(blobs, NSplitter::TEntityGroups(NYDBTest::TControllers::GetColumnShardController()->GetBlobSplitSettings(),
                                     NBlobOperations::TGlobal::DefaultStorageId))) {
        return TConclusionStatus::Fail("cannot split data for appropriate blobs size");
    }
    auto constructor = TWritePortionInfoWithBlobsConstructor::BuildByBlobs(
        std::move(blobs), {}, pathId, schema->GetVersion(), schema->GetSnapshot(), storagesManager, EPortionType::Written);

    NArrow::TFirstLastSpecialKeys primaryKeys(slice.GetFirstLastPKBatch(schema->GetIndexInfo().GetReplaceKey()));
    const ui32 deletionsCount = (mType == NEvWrite::EModificationType::Delete) ? incomingBatch->num_rows() : 0;
    constructor.GetPortionConstructor().MutablePortionConstructor().AddMetadata(*schema, deletionsCount, primaryKeys, std::nullopt);
    constructor.GetPortionConstructor().MutablePortionConstructor().MutableMeta().SetTierName(IStoragesManager::DefaultStorageId);
    constructor.GetPortionConstructor().MutablePortionConstructor().MutableMeta().SetCompactionLevel(0);
    return TWritePortionInfoWithBlobsResult(std::move(constructor));
}

}   // namespace NKikimr::NOlap

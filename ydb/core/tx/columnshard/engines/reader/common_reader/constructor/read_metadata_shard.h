#pragma once
#include "read_metadata.h"

namespace NKikimr::NOlap::NReader::NCommon {

class TReadMetadataWithTablet: public TReadMetadata {
    using TBase = TReadMetadata;

public:
    using TBase::TBase;

private:
    virtual void DoOnReadFinished(NColumnShard::TColumnShard& owner) const override;
    virtual void DoOnBeforeStartReading(NColumnShard::TColumnShard& owner) const override;
    virtual void DoOnReplyConstruction(const ui64 tabletId, NKqp::NInternalImplementation::TEvScanData& scanData) const override;
};

}   // namespace NKikimr::NOlap::NReader::NCommon

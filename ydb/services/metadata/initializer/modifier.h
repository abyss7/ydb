#pragma once
#include <ydb/library/accessor/accessor.h>
#include <ydb/public/api/protos/ydb_status_codes.pb.h>
#include <ydb/services/metadata/request/config.h>

#include <util/generic/string.h>
#include <util/generic/vector.h>

#include <memory>

namespace NKikimr::NMetadata::NInitializer {

class IModifierExternalController {
public:
    using TPtr = std::shared_ptr<IModifierExternalController>;
    virtual ~IModifierExternalController() = default;
    virtual void OnModificationFinished(const TString& modificationId) = 0;
    virtual void OnModificationFailed(Ydb::StatusIds::StatusCode status, const TString& errorMessage, const TString& modificationId) = 0;
};

class ITableModifier {
private:
    YDB_READONLY_DEF(TString, ModificationId);
    YDB_READONLY_DEF(bool, SupportDbCache);

protected:
    virtual bool DoExecute(IModifierExternalController::TPtr externalController, const NRequest::TConfig& config) const = 0;

public:
    using TPtr = std::shared_ptr<ITableModifier>;

    virtual ~ITableModifier() = default;

    explicit ITableModifier(const TString& modificationId, bool supportDbCache = true)
        : ModificationId(modificationId)
        , SupportDbCache(supportDbCache)
    {}

    bool Execute(IModifierExternalController::TPtr externalController, const NRequest::TConfig& config) const {
        return DoExecute(externalController, config);
    }
};

class IInitializerInput {
public:
    using TPtr = std::shared_ptr<IInitializerInput>;
    virtual void OnPreparationFinished(const TVector<ITableModifier::TPtr>& modifiers) = 0;
    virtual void OnPreparationProblem(const TString& errorMessage) const = 0;
    virtual ~IInitializerInput() = default;
};

class IInitializerOutput {
public:
    using TPtr = std::shared_ptr<IInitializerOutput>;
    virtual void OnInitializationFinished(const TString& id) const = 0;
    virtual ~IInitializerOutput() = default;
};

}

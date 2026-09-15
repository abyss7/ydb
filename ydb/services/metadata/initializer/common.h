#pragma once
#include "modifier.h"

#include <ydb/services/metadata/request/config.h>
#include <ydb/services/metadata/request/request_actor_cb.h>

namespace NKikimr::NMetadata::NInitializer {

class TACLModifierConstructor;

template <class TDialogPolicy>
class TGenericTableModifier: public ITableModifier {
private:
    using TBase = ITableModifier;
    YDB_READONLY_DEF(typename TDialogPolicy::TRequest, Request);
protected:
    class TAdapterController: public NRequest::IExternalController<TDialogPolicy> {
    private:
        IModifierExternalController::TPtr ExternalController;
        const TString ModificationId;
    public:
        TAdapterController(IModifierExternalController::TPtr externalController, const TString& modificationId)
            : ExternalController(externalController)
            , ModificationId(modificationId)
        {

        }

        virtual void OnRequestResult(typename TDialogPolicy::TResponse&& /*result*/) override {
            ExternalController->OnModificationFinished(ModificationId);
        }
        virtual void OnRequestFailed(Ydb::StatusIds::StatusCode status, const TString& errorMessage) override {
            ExternalController->OnModificationFailed(status, errorMessage, ModificationId);
        }
    };

    virtual bool DoExecute(IModifierExternalController::TPtr externalController, const NRequest::TConfig& /*config*/) const override {
        NRequest::TYDBOneRequestSender<TDialogPolicy> req(Request, NACLib::TSystemUsers::Metadata(), std::make_shared<TAdapterController>(externalController, GetModificationId()));
        req.Start();
        return true;
    }
public:
    TGenericTableModifier(const typename TDialogPolicy::TRequest& request, const TString& modificationId)
        : TBase(modificationId)
        , Request(request)
    {

    }
};

class TACLModifierConstructor {
private:
    const TString Id;
    Ydb::Scheme::ModifyPermissionsRequest Request;
    ITableModifier::TPtr BuildModifier() const;
public:
    TACLModifierConstructor(const TString& path, const TString& id)
        : Id(id) {
        Request.set_path(path);
    }
    Ydb::Scheme::ModifyPermissionsRequest* operator->() {
        return &Request;
    }
    operator ITableModifier::TPtr() const {
        return BuildModifier();
    }
    static TACLModifierConstructor GetNoAccessModifier(const TString& path, const TString& id);
    static TACLModifierConstructor GetReadOnlyModifier(const TString& path, const TString& id);
};

}

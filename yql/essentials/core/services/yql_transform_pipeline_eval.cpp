#include "yql_transform_pipeline.h"
#include "yql_eval_expr.h"
#include "yql_eval_params.h"
#include "yql_lineage.h"

#include <yql/essentials/utils/log/log.h>

#include <library/cpp/yson/writer.h>
#include <library/cpp/yson/node/node_io.h>

namespace NYql {

const TString LineageComponent = "Lineage";
const TString LineageResultLabel = "LineageResult";
const TString StandaloneLineageLabel = "StandaloneLineage";

TTransformationPipeline& TTransformationPipeline::AddParametersEvaluation(const NKikimr::NMiniKQL::IFunctionRegistry& functionRegistry, EYqlIssueCode issueCode) {
    auto& typeCtx = *TypeAnnotationContext_;
    Transformers_.push_back(TTransformStage(CreateFunctorTransformer(
                                                [&](const TExprNode::TPtr& input, TExprNode::TPtr& output, TExprContext& ctx) {
                                                    return EvaluateParameters(input, output, typeCtx, ctx, functionRegistry);
                                                }), "EvaluateParameters", issueCode));

    return *this;
}

TTransformationPipeline& TTransformationPipeline::AddExpressionEvaluation(const NKikimr::NMiniKQL::IFunctionRegistry& functionRegistry,
                                                                          IGraphTransformer* calcTransfomer, EYqlIssueCode issueCode) {
    auto& typeCtx = *TypeAnnotationContext_;
    auto& funcReg = functionRegistry;
    auto typeAnnCallableFactory = TypeAnnCallableFactory_;
    Transformers_.push_back(TTransformStage(CreateFunctorTransformer(
                                                [&typeCtx, &funcReg, calcTransfomer, typeAnnCallableFactory](const TExprNode::TPtr& input, TExprNode::TPtr& output, TExprContext& ctx) {
                                                    return EvaluateExpression(input, output, typeCtx, ctx, funcReg, calcTransfomer, typeAnnCallableFactory);
                                                }), "EvaluateExpression", issueCode));

    return *this;
}

TTransformationPipeline& TTransformationPipeline::AddOptimizationWithLineage(bool enableLineage, bool checkWorld, bool withFinalOptimization, EYqlIssueCode issueCode) {
    AddCommonOptimization(false, issueCode);
    if (enableLineage) {
        Transformers_.push_back(TTransformStage(
            CreateChoiceGraphTransformer(
                [&typesCtx = std::as_const(*TypeAnnotationContext_)](const TExprNode::TPtr&, TExprContext&) {
                    return typesCtx.EnableLineage;
                },
                TTransformStage(
                    CreateSinglePassFunctorTransformer(
                        [typeCtx = TypeAnnotationContext_](const TExprNode::TPtr& input, TExprNode::TPtr& output, TExprContext& ctx) {
                            output = input;
                            TString calculatedLineage, loadedLineage;
                            if (typeCtx->QContext && typeCtx->QContext.CanRead()) {
                                auto loaded = typeCtx->QContext.GetReader()->Get({LineageComponent, LineageResultLabel}).GetValueSync();
                                if (loaded.Defined()) {
                                    loadedLineage = loaded->Value;
                                } else {
                                    YQL_LOG(INFO) << "There is no lineage in QStorage, lineage calculation is skipped in replay mode";
                                    return IGraphTransformer::TStatus::Ok;
                                }
                            }
                            std::exception_ptr lineageError;
                            typeCtx->LineageStats.Correct = true;
                            try {
                                calculatedLineage = CalculateLineage(*input, *typeCtx, ctx, false);
                                typeCtx->LineageStats.Size = calculatedLineage.size();
                            } catch (const std::exception& e) {
                                YQL_LOG(ERROR) << "Lineage calculation error: " << e.what();
                                typeCtx->LineageStats.Correct = false;
                                lineageError = std::current_exception();
                            }
                            if (!loadedLineage.empty()) {
                                // if lineage calculation is failed, but loaded lineage exists, rethrow exception for replay mode
                                if (lineageError) {
                                    std::rethrow_exception(lineageError);
                                }
                                try {
                                    CheckEquvalentLineages(calculatedLineage, loadedLineage);
                                    YQL_LOG(INFO) << "Lineage replay is the same";
                                } catch (const std::exception& e) {
                                    YQL_LOG(ERROR) << "Lineage in replay is different:\n"
                                                   << e.what();
                                    throw yexception() << "Lineage in replay is different";
                                }
                            }
                            if (typeCtx->QContext && typeCtx->QContext.CanWrite() && *typeCtx->LineageStats.Correct) {
                                typeCtx->QContext.GetWriter()->Put({LineageComponent, LineageResultLabel}, calculatedLineage).GetValueSync();
                                YQL_LOG(INFO) << "Lineage is saved to QStorage";
                            }
                            return IGraphTransformer::TStatus::Ok;
                        }),
                    "Lineage",
                    issueCode),
                TTransformStage(
                    new TNullTransformer(),
                    "SkipLineage",
                    issueCode)),
            "LineageCalculation",
            issueCode));
    }
    AddProviderOptimization(issueCode);
    if (withFinalOptimization) {
        AddFinalCommonOptimization(issueCode);
    }
    AddCheckExecution(checkWorld, issueCode);
    return *this;
}

TTransformationPipeline& TTransformationPipeline::AddLineageOptimization(TMaybe<TString>& lineageOut, EYqlIssueCode issueCode) {
    AddCommonOptimization(false, issueCode);
    Transformers_.push_back(TTransformStage(
        CreateSinglePassFunctorTransformer(
            [typeCtx = TypeAnnotationContext_, &lineageOut](const TExprNode::TPtr& input, TExprNode::TPtr& output, TExprContext& ctx) {
                output = input;
                try {
                    lineageOut = CalculateLineage(*input, *typeCtx, ctx, true);
                    typeCtx->LineageStats.Size = lineageOut->size();
                    typeCtx->LineageStats.CorrectStandalone = true;
                } catch (const std::exception& e) {
                    YQL_LOG(ERROR) << "Lineage calculation error: " << e.what();
                    typeCtx->LineageStats.CorrectStandalone = false;
                    TStringStream s;
                    NYson::TYsonWriter writer(&s, NYson::EYsonFormat::Binary);
                    writer.OnBeginMap();
                    writer.OnKeyedItem("Error");
                    writer.OnStringScalar(e.what());
                    writer.OnEndMap();
                    lineageOut = s.Str();
                }
                if (typeCtx->QContext && typeCtx->QContext.CanRead()) {
                    auto loaded = typeCtx->QContext.GetReader()->Get({LineageComponent, StandaloneLineageLabel}).GetValueSync();
                    if (loaded.Defined()) {
                        try {
                            CheckEquvalentLineages(*lineageOut, loaded->Value);
                            YQL_LOG(INFO) << "Lineage replay is the same";
                        } catch (const std::exception& e) {
                            YQL_LOG(ERROR) << "Lineage in replay is different for standalone mode:\n"
                                           << e.what();
                            throw yexception() << "Lineage in replay is different";
                        }
                    }
                }
                if (typeCtx->EnableStandaloneLineage) {
                    if (typeCtx->QContext && typeCtx->QContext.CanWrite()) {
                        try {
                            // need to check correctness of lineage output before saving, e.g. if column-wise lineage section is empty
                            ValidateLineage(*lineageOut);
                            typeCtx->QContext.GetWriter()->Put({LineageComponent, StandaloneLineageLabel}, *lineageOut).GetValueSync();
                            YQL_LOG(INFO) << "Standalone Lineage is saved to QStorage";
                        } catch (const std::exception& e) {
                            typeCtx->LineageStats.CorrectStandalone = false;
                            YQL_LOG(INFO) << "Skip saving to QStorageLineage as lineage is incorrect: "
                                          << e.what()
                                          << ", calculated lineage: "
                                          << NYT::NodeToYsonString(*lineageOut);
                            return IGraphTransformer::TStatus::Ok;
                        }
                    }
                }
                return IGraphTransformer::TStatus::Ok;
            }),
        "LineageScanner",
        issueCode));
    return *this;
}

} // namespace NYql

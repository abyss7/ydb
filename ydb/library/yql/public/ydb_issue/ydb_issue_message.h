#pragma once
#include <yql/essentials/public/issue/yql_issue_message.h>

namespace Ydb::Issue {
class IssueMessage;
} // namespace Ydb::Issue

namespace NYql {

// Instantiated in ydb_issue_message.cpp
extern template TIssue IssueFromMessage<Ydb::Issue::IssueMessage>(const Ydb::Issue::IssueMessage& issueMessage);
extern template void IssuesFromMessage<Ydb::Issue::IssueMessage>(const ::google::protobuf::RepeatedPtrField<Ydb::Issue::IssueMessage>& message, TIssues& issues);
extern template void IssueToMessage<Ydb::Issue::IssueMessage>(const TIssue& topIssue, Ydb::Issue::IssueMessage* issueMessage);
extern template void IssuesToMessage<Ydb::Issue::IssueMessage>(const TIssues& issues, ::google::protobuf::RepeatedPtrField<Ydb::Issue::IssueMessage>* message);

TString IssueToBinaryMessage(const TIssue& issue);
TIssue IssueFromBinaryMessage(const TString& binaryMessage);

}

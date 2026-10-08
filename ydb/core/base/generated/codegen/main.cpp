#include <util/generic/ptr.h>
#include <util/stream/file.h>
#include <util/stream/output.h>
#include <util/string/builder.h>
#include <util/system/src_location.h>
#include <util/system/yassert.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#ifndef YDB_CODEGEN_DESCRIPTOR_SET
#include <ydb/core/protos/feature_flags.pb.h>
#endif

#include <jinja2cpp/template_env.h>
#include <jinja2cpp/template.h>
#include <jinja2cpp/value.h>
#include <jinja2cpp/reflected_value.h>
#include <cstdint>
#include <string>
#include <vector>

struct TSlot;
struct TField;

struct TSlot {
    TString Name;
    uint64_t Index;
    std::vector<const TField*> Fields;
    uint64_t DefaultValue = 0;
    uint64_t RuntimeFlagsMask = 0;

    TSlot(const TString& name, size_t index)
        : Name(name)
        , Index(index)
    {}
};

struct TField {
    TString Name;
    const TSlot* Slot;
    uint64_t HasMask = 0;
    uint64_t ValueMask = 0;
    uint64_t FullMask = 0;
    uint64_t DefaultValue = 0;
    bool IsRuntime = false;

    TField(const TString& name, const TSlot* slot)
        : Name(name)
        , Slot(slot)
    {}
};

namespace jinja2 {

    template<>
    struct TypeReflection<TSlot> : TypeReflected<TSlot> {
        static const auto& GetAccessors() {
            static std::unordered_map<std::string, FieldAccessor> accessors = {
                {"name", [](const TSlot& slot) { return Reflect(std::string(slot.Name)); }},
                {"index", [](const TSlot& slot) { return Reflect(slot.Index); }},
                {"fields", [](const TSlot& slot) { return Reflect(slot.Fields); }},
                {"default_value", [](const TSlot& slot) {
                    return Reflect(std::string(TStringBuilder() << slot.DefaultValue));
                }},
                {"runtime_flags_mask", [](const TSlot& slot) {
                    return Reflect(std::string(TStringBuilder() << slot.RuntimeFlagsMask));
                }},
            };
            return accessors;
        }
    };

    template<>
    struct TypeReflection<TField> : TypeReflected<TField> {
        static const auto& GetAccessors() {
            static std::unordered_map<std::string, FieldAccessor> accessors = {
                {"name", [](const TField& field) { return Reflect(std::string(field.Name)); }},
                {"slot", [](const TField& field) { return Reflect(field.Slot); }},
                {"has_mask", [](const TField& field) {
                    return Reflect(std::string(TStringBuilder() << field.HasMask));
                }},
                {"value_mask", [](const TField& field) {
                    return Reflect(std::string(TStringBuilder() << field.ValueMask));
                }},
                {"full_mask", [](const TField& field) {
                    return Reflect(std::string(TStringBuilder() << field.FullMask));
                }},
                {"default_value", [](const TField& field) {
                    return Reflect(std::string(TStringBuilder() << field.DefaultValue));
                }},
                {"is_runtime", [](const TField& field) { return Reflect(field.IsRuntime); }},
            };
            return accessors;
        }
    };

} // namespace jinja2

// The descriptors of the protos: with YDB_CODEGEN_DESCRIPTOR_SET from a
// FileDescriptorSet (protoc --descriptor_set_out --include_imports), the first
// argument, without the generated code of ydb/core/protos to compile and link;
// else the linked generated ones.
class TProtoDescriptors {
public:
    explicit TProtoDescriptors(const char* descriptorSet)
        : Pool(descriptorSet ? &OwnPool : google::protobuf::DescriptorPool::generated_pool())
        , Factory(Pool)
    {
        if (descriptorSet) {
            google::protobuf::FileDescriptorSet set;
            Y_ABORT_UNLESS(set.ParseFromString(TFileInput(descriptorSet).ReadAll()), "cannot parse %s", descriptorSet);
            for (const auto& file : set.file()) {
                Y_ABORT_UNLESS(OwnPool.BuildFile(file), "cannot build %s", file.name().c_str());
            }
        }
    }

    const google::protobuf::Descriptor& Message(const TString& fullName) const {
        const auto* d = Pool->FindMessageTypeByName(fullName);
        Y_ABORT_UNLESS(d, "no message %s", fullName.c_str());
        return *d;
    }

    const google::protobuf::FieldDescriptor& Extension(const TString& fullName) const {
        const auto* d = Pool->FindExtensionByName(fullName);
        Y_ABORT_UNLESS(d, "no extension %s", fullName.c_str());
        return *d;
    }

    // the options of `field`, with the extensions of the pool known
    THolder<google::protobuf::Message> Options(const google::protobuf::FieldDescriptor& field) {
        THolder<google::protobuf::Message> options(Factory.GetPrototype(&Message("google.protobuf.FieldOptions"))->New());
        Y_ABORT_UNLESS(options->ParseFromString(field.options().SerializeAsString()));
        return options;
    }

private:
    google::protobuf::DescriptorPool OwnPool;
    const google::protobuf::DescriptorPool* Pool;
    google::protobuf::DynamicMessageFactory Factory;
};

int main(int argc, char** argv) {
    const char* descriptorSet = nullptr;
    int first = 1;
#ifdef YDB_CODEGEN_DESCRIPTOR_SET
    descriptorSet = argv[first++];
#else
    NKikimrConfig::TFeatureFlags::descriptor();  // linked
#endif
    if (argc < first + 2) {
        Cerr << "Usage: " << argv[0] << (descriptorSet ? " DESCRIPTOR_SET" : "") << " INPUT OUTPUT ..." << Endl;
        return 1;
    }
    TProtoDescriptors descriptors(descriptorSet);
    const auto& requireRestart = descriptors.Extension("NKikimrConfig.RequireRestart");

    std::deque<TSlot> slots;
    std::deque<TField> fields;
    std::vector<const TSlot*> jinjaSlots;
    std::vector<const TField*> jinjaFields;

    TSlot* slot = nullptr;
    int currentBits = 0;

    const auto* d = &descriptors.Message("NKikimrConfig.TFeatureFlags");
    for (int fieldIndex = 0; fieldIndex < d->field_count(); ++fieldIndex) {
        const auto* protoField = d->field(fieldIndex);
        if (protoField->type() != google::protobuf::FieldDescriptor::TYPE_BOOL) {
            continue;
        }
        if (!slot || currentBits + 2 > 64) {
            size_t index = slots.size();
            TString name = TStringBuilder() << "slot" << index;
            slot = &slots.emplace_back(name, index);
            currentBits = 0;
            jinjaSlots.push_back(slot);
        }
        int shift = currentBits;
        currentBits += 2;
        TField* field = &fields.emplace_back(protoField->name(), slot);
        jinjaFields.push_back(field);
        field->HasMask = 1ULL << shift;
        field->ValueMask = 2ULL << shift;
        field->FullMask = 3ULL << shift;
        if (protoField->default_value_bool()) {
            field->DefaultValue = field->ValueMask;
        }
        auto options = descriptors.Options(*protoField);
        field->IsRuntime = !options->GetReflection()->GetBool(*options, &requireRestart);

        slot->Fields.push_back(field);
        slot->DefaultValue |= field->DefaultValue;
        if (field->IsRuntime) {
            slot->RuntimeFlagsMask |= field->FullMask;
        }
    }

    jinja2::TemplateEnv env;
    env.AddGlobal("generator", jinja2::Reflect(std::string(__SOURCE_FILE__)));
    env.AddGlobal("slots", jinja2::Reflect(jinjaSlots));
    env.AddGlobal("fields", jinja2::Reflect(jinjaFields));

    for (int i = first; i < argc; i += 2) {
        if (!(i + 1 < argc)) {
            Cerr << "ERROR: missing output for " << argv[i] << Endl;
            return 1;
        }

        jinja2::Template t(&env);
        auto loaded = t.Load(TFileInput(argv[i]).ReadAll(), argv[i]);
        if (!loaded) {
            Cerr << "ERROR: " << loaded.error().ToString() << Endl;
            return 1;
        }

        auto rendered = t.RenderAsString({});
        if (!rendered) {
            Cerr << "ERROR: " << rendered.error().ToString() << Endl;
            return 1;
        }

        TFileOutput(argv[i + 1]).Write(rendered.value());
        Cout << "Generated " << argv[i + 1] << " from " << argv[i] << Endl;
    }

    return 0;
}

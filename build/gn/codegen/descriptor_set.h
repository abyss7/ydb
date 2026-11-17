#pragma once

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/descriptor_database.h>
#include <google/protobuf/dynamic_message.h>

#include <fstream>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace NGnCodegen {

// Owns the descriptors and dynamic prototypes used by a code generator.
// Descriptor sets may overlap and need not list imports before their users.
class TDescriptorSet {
public:
    explicit TDescriptorSet(std::initializer_list<const char*> paths)
        : Pool(&Database)
        , Factory(&Pool)
    {
        std::unordered_map<std::string, std::string> files;
        for (const auto* path : paths) {
            std::ifstream input(path, std::ios::binary);
            google::protobuf::FileDescriptorSet descriptors;
            if (!input || !descriptors.ParseFromIstream(&input)) {
                throw std::runtime_error(std::string("Cannot read descriptor set: ") + path);
            }
            for (const auto& file : descriptors.file()) {
                const std::string name(file.name());
                const std::string serialized(file.SerializeAsString());
                const auto [it, added] = files.emplace(name, serialized);
                if (!added) {
                    if (it->second != serialized) {
                        throw std::runtime_error("Conflicting file descriptors: " + name);
                    }
                } else if (!Database.Add(file)) {
                    throw std::runtime_error("Cannot register file descriptor: " + name);
                }
            }
        }
    }

    const google::protobuf::Descriptor* FindMessage(const std::string& name) const {
        const auto* descriptor = Pool.FindMessageTypeByName(name);
        if (!descriptor) {
            throw std::runtime_error("Missing message descriptor: " + name);
        }
        return descriptor;
    }

    const google::protobuf::FieldDescriptor* FindExtension(const std::string& name) const {
        const auto* descriptor = Pool.FindExtensionByName(name);
        if (!descriptor) {
            throw std::runtime_error("Missing extension descriptor: " + name);
        }
        return descriptor;
    }

    std::unique_ptr<google::protobuf::Message> ReadFieldOptions(
        const google::protobuf::FieldDescriptor& field)
    {
        // Descriptor::options() uses generated FieldOptions. Custom options
        // not linked into this executable are unknown fields there. Reparse
        // them as a dynamic message from OUR pool, which knows the extensions.
        const auto* descriptor = FindMessage("google.protobuf.FieldOptions");
        const auto* prototype = Factory.GetPrototype(descriptor);
        if (!prototype) {
            throw std::runtime_error("Cannot create FieldOptions prototype");
        }
        std::unique_ptr<google::protobuf::Message> options(prototype->New());
        if (!options->ParseFromString(field.options().SerializeAsString())) {
            throw std::runtime_error("Cannot parse options of " + std::string(field.full_name()));
        }
        return options;
    }

    bool GetBoolOption(const google::protobuf::FieldDescriptor& field, const std::string& name) {
        const auto* extension = FindExtension(name);
        auto options = ReadFieldOptions(field);
        if (extension->containing_type() != options->GetDescriptor() ||
            extension->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_BOOL ||
            extension->is_repeated()) {
            throw std::runtime_error("Not a boolean field option: " + name);
        }
        return options->GetReflection()->GetBool(*options, extension);
    }

private:
    // Destruction order: messages (owned by callers), factory, pool, database.
    google::protobuf::SimpleDescriptorDatabase Database;
    google::protobuf::DescriptorPool Pool;
    google::protobuf::DynamicMessageFactory Factory;
};

} // namespace NGnCodegen

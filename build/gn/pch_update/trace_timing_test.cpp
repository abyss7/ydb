// Small standalone reader for cross-language timing tests; no compiler jobs.
#include "trace.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <iostream>

int main(int argc, char** argv) {
    using namespace NPchUpdate;
    try {
        for (int i = 1; i < argc; ++i) {
            TTraceSummary summary;
            std::string error;
            if (!SummarizeTraceFile(argv[i], &summary, &error)) {
                Fail(error);
            }
            TBinWriter binary;
            summary.Save(binary);
            TBinReader reader(binary.Data());
            auto loaded = TTraceSummary::Load(reader);
            TBinWriter again;
            loaded.Save(again);
            if (binary.Data() != again.Data()) {
                Fail("trace summary round trip changed");
            }

            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
            out.StartObject();
            const auto number = [&](const char* name, int64_t value) {
                out.Key(name);
                out.Int64(value);
            };
            number("execute", loaded.Execute);
            number("frontend", loaded.Frontend);
            number("raw_execute", loaded.WallExecute);
            number("raw_frontend", loaded.WallFrontend);
            number("outer", loaded.Outer);
            number("unbalanced", loaded.Unbalanced);
            number("numerator", loaded.TimingNumerator);
            number("denominator", loaded.TimingDenominator);
            out.Key("status");
            out.String(loaded.TimingStatus.c_str());
            out.Key("first");
            if (loaded.First < 0) { out.Null(); }
            else { out.String(loaded.Names[loaded.First].c_str()); }
            out.Key("headers");
            out.StartObject();
            for (size_t h = 0; h < loaded.Names.size(); ++h) {
                out.Key(loaded.Names[h].c_str());
                out.StartArray();
                out.Int64(loaded.Parse[h]);
                out.Int64(loaded.Own[h]);
                out.EndArray();
            }
            out.EndObject();
            out.Key("edges");
            out.StartArray();
            for (const auto& [a, b] : loaded.Edges) {
                out.StartArray();
                out.String(loaded.Names[a].c_str());
                out.String(loaded.Names[b].c_str());
                out.EndArray();
            }
            out.EndArray();
            out.EndObject();
            std::cout << buffer.GetString() << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

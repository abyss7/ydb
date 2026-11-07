#include "common.h"
#include "jobs.h"
#include "two_step.h"

#include <cstdio>
#include <exception>

int main(int argc, char** argv) {
    using namespace NPchUpdate;
    try {
        if (argc > 1 && std::string(argv[1]) == "job") {
            return RunJob(std::vector<std::string>(argv + 2, argv + argc));
        }
        return NTwoStep::Command(std::vector<std::string>(argv + 1, argv + argc));
    } catch (const std::exception& e) {
        fprintf(stderr, "pch_update: %s\n", e.what());
        return 1;
    }
}

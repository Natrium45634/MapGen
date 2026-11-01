#pragma once

#include <memory>
#include <string>

#include "core/stages/Pipeline.h"

namespace mapgen::test {

// Small worlds generated once per test process and shared between test cases.
const WorldData& world(const std::string& seed = "A7Test42", int width = 256, const std::string& preset = "earthlike");
WorldData generate(const std::string& seed, const Params& p, int threads = 0);
Params smallParams(int width = 256, const std::string& preset = "earthlike");
ThreadPool& pool();

}  // namespace mapgen::test

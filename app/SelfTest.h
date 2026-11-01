#pragma once

#include <QString>
#include <string>

namespace mapgen {

// Generates a small world twice (1 and N threads), checks P0 invariants, saves,
// reloads, compares hashes and info-card values, exports PNG/GeoTIFF/GeoJSON.
// Prints a log; returns 0 on success ("SELFTEST: OK").
int runSelfTest(const QString& workDir, std::string& log);

}  // namespace mapgen

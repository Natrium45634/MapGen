#pragma once

// Ordered stage graph with partial regeneration:
//   palette/UI change   -> nothing (render only)
//   sea level           -> stages 3..8
//   climate parameters  -> stages 4..8
//   plates / tectonics  -> stages 1..8
// A run is transactional: the caller works on a copy of the world and only
// adopts it when the run finished, so cancellation never leaves a mixed state.

#include <string>
#include <vector>

#include "core/stages/Stage.h"

namespace mapgen {

class Pipeline {
public:
    static const std::vector<StageInfo>& stages();

    // Runs stage 0 (configuration) and then stages [fromStage, 8]. Earlier stage
    // results already present in `world` are reused. Throws CancelledError on cancel.
    static void run(WorldData& world, int fromStage, ThreadPool& pool, const CancelToken& cancel,
                    const ProgressFn& progress = {});

    // Convenience: fresh world from seed + params.
    static WorldData generate(const std::string& seed, const Params& params, ThreadPool& pool,
                              const CancelToken& cancel, const ProgressFn& progress = {});

    // Content hash of one layer (hex) and recomputation of all stage hashes.
    static std::string hashLayer(const LayerData& layer);
    static void computeStageHash(WorldData& world, int stage);
    static std::string worldHash(const WorldData& world);
};

}  // namespace mapgen

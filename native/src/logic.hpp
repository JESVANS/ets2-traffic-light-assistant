#pragma once

#include "types.hpp"
#include <vector>

namespace tlhud {

Phase MapPhase(int32_t sem_type, int32_t state, const StateCodesConfig& codes);

std::vector<SemaphoreParsed> ParseSemaphores(const SemaphoreMemData& mem,
                                             const SourceConfig& src_cfg,
                                             const StateCodesConfig& codes);

void RelativeGeometry(const TruckTelemetry& truck,
                      const SemaphoreParsed& sem,
                      float& out_lon,
                      float& out_lat,
                      float& out_dy,
                      float& out_facing_dot);

class TargetSelector {
public:
    bool Select(const TruckTelemetry& truck,
                const std::vector<SemaphoreParsed>& sems,
                const SelectionConfig& cfg,
                TargetResult& out);
    void Reset() { has_prev_ = false; }

private:
    bool has_prev_ = false;
    int32_t prev_id_ = 0;
    long prev_rx_ = 0;
    long prev_rz_ = 0;
};

AdviceResult ComputeAdvice(const TargetResult& target,
                           const TruckTelemetry& truck,
                           const AdvisorConfig& cfg);

HudSnapshot BuildDemoSnapshot(uint64_t now_ms, const AdvisorConfig& cfg);

} // namespace tlhud

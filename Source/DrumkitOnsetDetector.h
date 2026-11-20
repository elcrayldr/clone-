#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace crayldr
{
struct DetectedOnset
{
    int samplePosition = 0;
    float strength = 0.0f;
};

inline std::vector<DetectedOnset> detectOnsets(
    const std::vector<float>& energy,
    const std::vector<float>& highBandEnergy,
    int frameSize,
    double sampleRate)
{
    std::vector<DetectedOnset> candidates;
    if (energy.size() < 3 || energy.size() != highBandEnergy.size()
        || frameSize <= 0 || sampleRate < 8000.0)
        return candidates;

    const int frameCount = static_cast<int>(energy.size());
    const int localRadius = std::max(2, static_cast<int>(0.12 * sampleRate / frameSize));
    const int refractory = std::max(1, static_cast<int>(0.055 * sampleRate / frameSize));
    std::vector<float> neighbourhood;
    neighbourhood.reserve(static_cast<size_t>(localRadius * 2 + 1));

    for (int i = 1; i + 1 < frameCount; ++i)
    {
        neighbourhood.clear();
        const int first = std::max(0, i - localRadius);
        const int last = std::min(frameCount - 1, i + localRadius);
        for (int j = first; j <= last; ++j)
            neighbourhood.push_back(energy[static_cast<size_t>(j)]);

        const auto middle = neighbourhood.begin() + neighbourhood.size() / 2;
        std::nth_element(neighbourhood.begin(), middle, neighbourhood.end());
        const float localMedian = *middle;

        const float energyRise = std::max(0.0f, energy[static_cast<size_t>(i)] - energy[static_cast<size_t>(i - 1)]);
        const float highRise = std::max(0.0f, highBandEnergy[static_cast<size_t>(i)] - highBandEnergy[static_cast<size_t>(i - 1)]);
        const float strength = energyRise + 0.35f * highRise;
        const float adaptiveFloor = std::max(0.00035f, localMedian * 0.16f);

        if (energy[static_cast<size_t>(i)] >= energy[static_cast<size_t>(i - 1)]
            && energy[static_cast<size_t>(i)] > energy[static_cast<size_t>(i + 1)]
            && energy[static_cast<size_t>(i)] > std::max(0.0008f, localMedian * 1.35f)
            && strength > adaptiveFloor)
        {
            candidates.push_back({ i * frameSize, strength });
        }
    }

    std::sort(candidates.begin(), candidates.end(),
        [](const DetectedOnset& a, const DetectedOnset& b) { return a.strength > b.strength; });

    std::vector<DetectedOnset> selected;
    selected.reserve(candidates.size());
    for (const auto& candidate : candidates)
    {
        const int frame = candidate.samplePosition / frameSize;
        const bool tooClose = std::any_of(selected.begin(), selected.end(),
            [frame, refractory, frameSize](const DetectedOnset& existing)
            {
                return std::abs(existing.samplePosition / frameSize - frame) < refractory;
            });
        if (!tooClose)
            selected.push_back(candidate);
    }

    std::sort(selected.begin(), selected.end(),
        [](const DetectedOnset& a, const DetectedOnset& b) { return a.samplePosition < b.samplePosition; });
    return selected;
}
} // namespace crayldr

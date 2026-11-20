#include "DrumkitOnsetDetector.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

int main()
{
    constexpr int frames = 400;
    std::vector<float> energy(frames, 0.004f);
    std::vector<float> high(frames, 0.002f);

    const auto addHit = [&](int at, float amplitude)
    {
        energy[static_cast<size_t>(at)] = amplitude;
        energy[static_cast<size_t>(at + 1)] = amplitude * 0.72f;
        energy[static_cast<size_t>(at + 2)] = amplitude * 0.40f;
        high[static_cast<size_t>(at)] = amplitude * 0.8f;
        high[static_cast<size_t>(at + 1)] = amplitude * 0.4f;
    };

    addHit(40, 0.85f);
    addHit(120, 0.48f);
    addHit(250, 0.68f);

    const auto detected = crayldr::detectOnsets(energy, high, 220, 44100.0);
    if (detected.size() != 3) return 1;
    if (detected[0].samplePosition != 40 * 220) return 2;
    if (detected[1].samplePosition != 120 * 220) return 3;
    if (detected[2].samplePosition != 250 * 220) return 4;

    const auto silence = crayldr::detectOnsets(std::vector<float>(64, 0.0f),
                                               std::vector<float>(64, 0.0f), 220, 44100.0);
    if (!silence.empty()) return 5;

    const auto invalid = crayldr::detectOnsets(energy, high, 0, 44100.0);
    if (!invalid.empty()) return 6;

    const auto invalidRate = crayldr::detectOnsets(energy, high, 220,
        std::numeric_limits<double>::quiet_NaN());
    if (!invalidRate.empty()) return 7;

    auto corruptEnergy = energy;
    corruptEnergy[12] = std::numeric_limits<float>::quiet_NaN();
    const auto invalidAudio = crayldr::detectOnsets(corruptEnergy, high, 220, 44100.0);
    if (!invalidAudio.empty()) return 8;

    std::vector<float> closeEnergy(100, 0.004f);
    std::vector<float> closeHigh(100, 0.002f);
    closeEnergy[20] = 0.50f;
    closeEnergy[21] = 0.30f;
    closeEnergy[27] = 0.80f;
    closeEnergy[28] = 0.40f;
    closeHigh[20] = 0.35f;
    closeHigh[27] = 0.60f;
    const auto refractory = crayldr::detectOnsets(closeEnergy, closeHigh, 220, 44100.0);
    if (refractory.size() != 1 || refractory.front().samplePosition != 27 * 220) return 9;

    std::cout << "Drumkit onset detector tests passed: 3 synthetic hits, silence, invalid input, non-finite data and refractory suppression.\n";
    return 0;
}

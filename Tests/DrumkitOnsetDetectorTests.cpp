#include "DrumkitOnsetDetector.h"
#include <cassert>
#include <cmath>
#include <iostream>
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
    assert(detected.size() == 3);
    assert(detected[0].samplePosition == 40 * 220);
    assert(detected[1].samplePosition == 120 * 220);
    assert(detected[2].samplePosition == 250 * 220);

    const auto silence = crayldr::detectOnsets(std::vector<float>(64, 0.0f),
                                               std::vector<float>(64, 0.0f), 220, 44100.0);
    assert(silence.empty());

    const auto invalid = crayldr::detectOnsets(energy, high, 0, 44100.0);
    assert(invalid.empty());

    std::cout << "Drumkit onset detector tests passed: 3 synthetic hits, silence, invalid input.\n";
    return 0;
}

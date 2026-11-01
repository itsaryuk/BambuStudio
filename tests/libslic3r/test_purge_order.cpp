#include <catch2/catch.hpp>

#include <algorithm>
#include <random>

#include "libslic3r/GCode/ToolOrderUtils.hpp"

using namespace Slic3r;

// Brute force over every order of every layer: the true minimum flush for the whole stack.
static float brute_force(const FlushMatrix& m, const std::vector<std::vector<unsigned int>>& layers, size_t L, std::optional<unsigned int> prev)
{
    if (L == layers.size()) return 0.f;
    std::vector<unsigned int> seq = layers[L];
    std::sort(seq.begin(), seq.end());
    float best = std::numeric_limits<float>::max();
    do {
        float c = 0.f;
        std::optional<unsigned int> p = prev;
        for (unsigned int f : seq) { if (p) c += m[*p][f]; p = f; }
        best = std::min(best, c + brute_force(m, layers, L + 1, seq.empty() ? prev : std::optional<unsigned int>(seq.back())));
    } while (std::next_permutation(seq.begin(), seq.end()));
    return best;
}

TEST_CASE("Filament order is the global minimum flush over all layers", "[PurgeOrder]")
{
    std::mt19937 rng(7);
    for (int trial = 0; trial < 200; ++trial) {
        const unsigned int n = 4;
        FlushMatrix m(n, std::vector<float>(n, 0.f));
        for (unsigned int a = 0; a < n; ++a)
            for (unsigned int b = 0; b < n; ++b)
                if (a != b) m[a][b] = float(60 + rng() % 500); // whole numbers: the API sums into an int
        std::vector<std::vector<unsigned int>> layers(5);
        for (auto& l : layers)
            for (unsigned int f = 0; f < n; ++f)
                if (rng() % 3 != 0) l.push_back(f);
        std::vector<unsigned int>              all{0, 1, 2, 3};
        std::vector<std::vector<unsigned int>> seqs;
        const int cost = reorder_filaments_for_minimum_flush_volume(all, {0, 0, 0, 0}, layers, {m}, std::nullopt, &seqs);
        REQUIRE(cost == int(brute_force(m, layers, 0, std::nullopt)));
        // The returned orders are permutations of each layer and add up to the reported cost.
        REQUIRE(seqs.size() == layers.size());
        float sum = 0.f;
        std::optional<unsigned int> p;
        for (size_t L = 0; L < layers.size(); ++L) {
            std::vector<unsigned int> a = seqs[L], b = layers[L];
            std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
            REQUIRE(a == b);
            for (unsigned int f : seqs[L]) { if (p) sum += m[*p][f]; p = f; }
        }
        REQUIRE(int(sum) == cost);
    }
}

TEST_CASE("Custom layer sequences are kept as given", "[PurgeOrder]")
{
    FlushMatrix m{{0, 100, 900}, {900, 0, 100}, {100, 900, 0}};
    std::vector<std::vector<unsigned int>> layers{{0, 1, 2}, {0, 1, 2}};
    std::function<bool(int, std::vector<int>&)> custom = [](int layer, std::vector<int>& seq) {
        if (layer != 1) return false;
        seq = {3, 2, 1}; // 1-based: filaments 2, 1, 0
        return true;
    };
    std::vector<std::vector<unsigned int>> seqs;
    reorder_filaments_for_minimum_flush_volume({0, 1, 2}, {0, 0, 0}, layers, {m}, custom, &seqs);
    REQUIRE(seqs[1] == std::vector<unsigned int>{2, 1, 0});
}

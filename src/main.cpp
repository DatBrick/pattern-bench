/*
    Copyright 2018 Brick

    Permission is hereby granted, free of charge, to any person obtaining a copy of this software
    and associated documentation files (the "Software"), to deal in the Software without restriction,
    including without limitation the rights to use, copy, modify, merge, publish, distribute,
    sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all copies or
    substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
    BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
    NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
    DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <random>
#include <unordered_set>
#include <chrono>
#include <cassert>
#include <fstream>

#include <mem/mem.h>
#include <mem/pattern.h>
#include <mem/utils.h>

#include <mem/platform.h>
#include <mem/platform-inl.h>

#include <mem/init_function.h>
#include <mem/init_function-inl.h>

#include <mem/data_buffer.h>

#include <fmt/format.h>

#include "pattern_entry.h"

static constexpr const size_t REGION_SIZE = 64 * 1024 * 1024;
static constexpr const size_t TEST_COUNT  = 512;
static constexpr const size_t LOG_LEVEL   = 0;
static constexpr const uint32_t RNG_SEED  = 0;

using mem::byte;

std::mt19937 create_twister(uint32_t& seed)
{
    if (seed == 0)
    {
        seed = std::random_device{}();
    }

    std::minstd_rand0 source(seed);

    // Magic number 624: The number of unsigned ints the MT uses as state
    std::vector<unsigned int> random_data(624);
    std::generate(begin(random_data), end(random_data), source);

    std::seed_seq seeds(begin(random_data), end(random_data));
    std::mt19937 result(seeds);

    return result;
}

mem::byte_buffer read_file(const char* path)
{
    std::ifstream input(path, std::ifstream::binary | std::ifstream::ate);

    size_t length = static_cast<size_t>(input.tellg());

    input.seekg(0);

    mem::byte_buffer result(length);

    if (!input.read(reinterpret_cast<char*>(result.data()), result.size()))
    {
        result.resize(0);
    }

    return result;
}

struct scan_bench
{
private:
    byte* raw_data_ {nullptr};
    size_t raw_size_ {0};

    byte* full_data_ {nullptr};
    size_t full_size_ {0};

    byte* data_ {nullptr};
    size_t size_ {0};

    uint32_t seed_ {RNG_SEED};
    std::mt19937 rng_ {create_twister(seed_)};

    std::vector<byte> pattern_;
    std::string masks_;
    std::unordered_set<size_t> expected_;

public:
    scan_bench() = default;

    scan_bench(const scan_bench&) = delete;
    scan_bench(scan_bench&&) = delete;

    ~scan_bench()
    {
        mem::protect_free(raw_data_, raw_size_);
    }

    void reset(size_t region_size)
    {
        reset(nullptr, region_size);
    }

    void reset(const char* file_name)
    {
        mem::byte_buffer region_data = read_file(file_name);

        reset(region_data.data(), region_data.size());
    }

    void reset(const byte* region_data, size_t region_size)
    {
        size_t page_size = mem::page_size();

        full_size_ = (region_size + page_size - 1) / page_size * page_size;

        raw_size_ = full_size_ + (page_size * 2);
        raw_data_ = static_cast<byte*>(mem::protect_alloc(raw_size_, mem::prot_flags::RW));

        full_data_ = raw_data_ + page_size;

        mem::protect_modify(raw_data_, page_size, mem::prot_flags::NONE);
        mem::protect_modify(raw_data_ + raw_size_ - page_size, page_size, mem::prot_flags::NONE);

        if (region_data)
        {
            size_t extra = (full_size_ - region_size);

            std::memset(full_data_, 0, extra);
            std::memcpy(full_data_ + extra, region_data, region_size);
        }
        else
        {
            std::uniform_int_distribution<uint32_t> byte_dist(0, 0xFF);

            std::generate_n(full_data_, full_size_, [&] { return (byte) byte_dist(rng_); });
        }
    }

    size_t full_size() const noexcept
    {
        return full_size_;
    }

    const byte* data() const noexcept
    {
        return data_;
    }

    size_t size() const noexcept
    {
        return size_;
    }

    const byte* pattern() const noexcept
    {
        return pattern_.data();
    }

    const char* masks() const noexcept
    {
        return masks_.data();
    }

    uint32_t seed() const noexcept
    {
        return seed_;
    }

    std::unordered_set<size_t> shift_results(const std::vector<const byte*>& results)
    {
        std::unordered_set<size_t> shifted;

        for (const byte* result : results)
        {
            shifted.emplace(result - data());
        }

        return shifted;
    }

    void generate()
    {
        std::uniform_int_distribution<size_t> size_dist(0, 100);

        size_t variation = size_dist(rng_);

        data_ = full_data_ + variation;
        size_ = full_size_ - variation;

        std::uniform_int_distribution<uint32_t> byte_dist(0, 0xFF);

        std::uniform_int_distribution<size_t> length_dist(5, 32);

        size_t pattern_length = length_dist(rng_);

        pattern_.resize(pattern_length);
        masks_.resize(pattern_length);

        std::bernoulli_distribution mask_dist(0.9);

        bool all_masks = true;

        do
        {
            for (size_t i = 0; i < pattern_length; ++i)
            {
                if (mask_dist(rng_))
                {
                    pattern_[i] = (char) byte_dist(rng_);
                    masks_[i] = 'x';

                    all_masks = false;
                }
                else
                {
                    pattern_[i] = 0x00;
                    masks_[i] = '?';
                }
            }
        } while (all_masks);

        std::uniform_int_distribution<size_t> count_dist(2, 10);

        size_t result_count = count_dist(rng_);

        std::uniform_int_distribution<size_t> range_dist(0, size() - pattern_.size());

        for (size_t i = 0; i < result_count; ++i)
        {
            size_t offset = range_dist(rng_);

            for (size_t j = 0; j < pattern_.size(); ++j)
            {
                if (masks_[j] != '?')
                    data_[offset + j] = pattern_[j];
            }
        }

        expected_ = shift_results(FindPatternSimple(data(), size(), pattern(), masks()));
    }

    bool check_results(const pattern_scanner& scanner, const std::vector<const byte*>& results)
    {
        std::unordered_set<size_t> shifted = shift_results(results);

        if (shifted.size() != expected_.size())
        {
            if (LOG_LEVEL > 2)
                fmt::print("{0:<32} - Got {1} results, Expected {2}\n", scanner.GetName(), shifted.size(), expected_.size());

            if (LOG_LEVEL > 3)
            {
                fmt::print("Got:\n");

                for (size_t v : shifted)
                    fmt::print("> 0x{0:X}\n", v);

                fmt::print("Expected:\n");

                for (size_t v : expected_)
                    fmt::print("> 0x{0:X}\n", v);
            }

            return false;
        }

        for (size_t result : shifted)
        {
            if (expected_.find(result) == expected_.end())
            {
                if (LOG_LEVEL > 2)
                    fmt::print("{0:<32} - Wasn't expecting 0x{1:X}\n", scanner.GetName(), result);

                return false;
            }
        }

        return true;
    }
};

int main(int argc, const char* argv[])
{
    mem::init_function::init();

    scan_bench reg;

    if (argc > 1)
    {
        const char* file_name = argv[1];

        fmt::print("Scanning file: {}\n", file_name);

        reg.reset(file_name);
    }
    else
    {
        fmt::print("Scanning random data\n");

        reg.reset(REGION_SIZE);
    }

    fmt::print("Begin Scan: Seed: 0x{0:08X}, Size: 0x{1:X}, Tests: {2}\n", reg.seed(), reg.full_size(), TEST_COUNT);

    for (size_t i = 0; i < TEST_COUNT; ++i)
    {
        reg.generate();

        for (auto& pattern : PATTERNS)
        {
            uint64_t start_clock = mem::rdtsc();

            try
            {
                std::vector<const byte*> results = pattern->Scan(reg.pattern(), reg.masks(), reg.data(), reg.size());

                if (!reg.check_results(*pattern, results))
                {
                    if (LOG_LEVEL > 1)
                        fmt::print("{0:<32} - Failed test {1} ({2}, {3})\n", pattern->GetName(), i, mem::as_hex({ reg.pattern(), strlen(reg.masks()) }), reg.masks());

                    pattern->Failed++;
                }
            }
            catch (...)
            {
                if (LOG_LEVEL > 0)
                    fmt::print("{0:<32} - Failed test {1} (Exception)\n", pattern->GetName(), i);

                pattern->Failed++;
            }

            uint64_t end_clock = mem::rdtsc();

            pattern->Elapsed += end_clock - start_clock;
        }
    }

    std::sort(PATTERNS.begin(), PATTERNS.end(),
        [ ] (const std::unique_ptr<pattern_scanner>& lhs,
             const std::unique_ptr<pattern_scanner>& rhs)
    {
        if (lhs->Failed != rhs->Failed)
            return lhs->Failed < rhs->Failed;

        return lhs->Elapsed < rhs->Elapsed;
    });

    fmt::print("End Scan\n\n");

    const size_t total_scan_length = reg.size() * TEST_COUNT;

    for (size_t i = 0; i < PATTERNS.size(); ++i)
    {
        const auto& pattern = *PATTERNS[i];

        fmt::print("{0} | {1:<32} | {2:>12} cycles = {3:>6.3f} cycles/byte | {4} failed\n", i, pattern.GetName(), pattern.Elapsed, double(pattern.Elapsed) / total_scan_length, pattern.Failed);
    }
}

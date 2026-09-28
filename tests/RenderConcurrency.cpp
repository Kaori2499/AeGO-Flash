// Same harness links to the preserved 1.0.0 renderer for before/after timings.
// Native concurrency coverage complements actual AEX callbacks and AE testing.
#include "Renderer.h"
#include <Windows.h>
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned ready = 0;
    bool open = false;
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        ++ready; changed.notify_all();
        changed.wait(lock, [&] { return open; });
    }
    void start(unsigned count) {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return ready == count; });
        open = true; changed.notify_all();
    }
};
struct Memory { SIZE_T privateBytes = 0, workingBytes = 0, peakWorkingBytes = 0; };
Memory memory() {
    PROCESS_MEMORY_COUNTERS_EX value{}; value.cb = sizeof(value);
    require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&value), sizeof(value)) != 0,
        "Cannot read own-process memory counters.");
    return {value.PrivateUsage, value.WorkingSetSize, value.PeakWorkingSetSize};
}
double megabytes(SIZE_T bytes) { return bytes / (1024.0 * 1024.0); }
#ifdef L2DAE_MFR_DIAGNOSTICS
void statistics(bool requireParallel) {
    const auto stats = l2dae::rendererStats();
    require(stats.workerLimit >= 1 && stats.workerLimit <= 4 && stats.workerCount <= stats.workerLimit,
        "Renderer worker cache exceeds its bounded capacity.");
    require(stats.modelInstances <= stats.workerCount && stats.activeCpuEvaluations == 0,
        "Renderer leaked a model slot or active CPU evaluation.");
    if (requireParallel && stats.workerLimit > 1)
        require(stats.peakCpuConcurrency > 1, "Concurrent callers were serialized instead of overlapping CPU evaluation.");
    std::cout << "STATS limit=" << stats.workerLimit << " workers=" << stats.workerCount << " models=" << stats.modelInstances
        << " peak_cpu=" << stats.peakCpuConcurrency << " texture_bytes=" << stats.sharedTextureBytes
        << " moc_bytes=" << stats.sharedModelBytes << " frames=" << stats.framesRendered << '\n';
}
#endif
std::vector<l2dae::RenderRequest> requests(const std::wstring& path, int width, int height, bool mixedSizes) {
    const auto motions = l2dae::listMotions(path);
    const auto expressions = l2dae::listExpressions(path);
    std::vector<l2dae::RenderRequest> result;
    for (int i = 0; i < 24; ++i) {
        l2dae::RenderRequest request;
        request.modelPath = path;
        request.width = mixedSizes && i % 7 == 0 ? width / 2 : width;
        request.height = mixedSizes && i % 7 == 0 ? height / 2 : height;
        if (!motions.empty()) {
            request.motionPath = motions.front().path;
            request.motionPathB = motions.back().path;
        }
        request.seconds = 0.117 + ((i * 7) % 23) * 0.287;
        request.secondsB = 5.125 - ((i * 3) % 17) * 0.191;
        request.blend = (i % 5) * 0.25f;
        request.loop = i % 3 != 0;
        request.offsetX = (i % 3 - 1) * 4.0f;
        request.offsetY = (i % 5 - 2) * 3.0f;
        request.scale = 0.8f + (i % 3) * 0.1f;
        request.pixelAspect = i % 7 == 0 ? 1.25f : 1;
        if (!expressions.empty()) {
            request.expressionPathA = expressions.front().path;
            request.expressionPathB = expressions.back().path;
            request.expressionWeightA = (i % 3) * 0.5f;
            request.expressionWeightB = (i % 5) * 0.25f;
        }
        request.lipSyncEnabled = i % 3 != 0;
        request.mouthOpen = (i % 5) * 0.25f;
        request.breathingEnabled = i % 2 == 0;
        request.autoBlinkEnabled = i % 3 != 0;
        request.ambientSeconds = 3.125 + ((i % 7) - 3) * 0.333;
        result.push_back(std::move(request));
    }
    return result;
}

template<class Work> double concurrent(unsigned threadCount, Work work) {
    Gate gate;
    std::mutex errorMutex;
    std::exception_ptr error;
    std::vector<std::thread> threads;
    for (unsigned thread = 0; thread < threadCount; ++thread) {
        threads.emplace_back([&, thread] {
            gate.wait();
            try { work(thread); }
            catch (...) { std::lock_guard<std::mutex> lock(errorMutex); if (!error) error = std::current_exception(); }
        });
    }
    const auto started = Clock::now();
    gate.start(threadCount);
    for (auto& thread : threads) thread.join();
    const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
    if (error) std::rethrow_exception(error);
    return elapsed;
}

void dump(const std::vector<l2dae::RenderRequest>& cases, const std::filesystem::path& folder, bool compare) {
    if (!compare) std::filesystem::create_directories(folder);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto frame = l2dae::render(cases[i]);
        const auto path = folder / (std::to_wstring(i) + L".rgba");
        if (compare) {
            std::ifstream input(path, std::ios::binary);
            require(bool(input), "Missing baseline RGBA frame.");
            const std::vector<std::uint8_t> expected((std::istreambuf_iterator<char>(input)), {});
            require(expected == frame.rgba, "New renderer differs from the release 1.0.0 baseline.");
        } else {
            std::ofstream output(path, std::ios::binary);
            output.write(reinterpret_cast<const char*>(frame.rgba.data()), static_cast<std::streamsize>(frame.rgba.size()));
            require(bool(output), "Cannot write baseline RGBA frame.");
        }
    }
    std::cout << "PASS: " << cases.size() << (compare ? " exact baseline frame comparisons.\n" : " baseline frames written.\n");
}

void benchmark(const std::vector<l2dae::RenderRequest>& cases, unsigned threads, unsigned frames) {
    const auto initial = memory();
    const auto cold = Clock::now();
    concurrent(threads, [&](unsigned index) { l2dae::render(cases[index % cases.size()]); });
    const double coldSeconds = std::chrono::duration<double>(Clock::now() - cold).count();
    // Preload the entire workload on concurrent callers before measured rounds.
    concurrent(threads, [&](unsigned index) {
        for (unsigned i = index; i < 48; i += threads) l2dae::render(cases[i % cases.size()]);
    });
    const auto warm = memory();
    std::vector<double> seconds;
    std::atomic<unsigned long long> checksum{0};
    SIZE_T sampledPeakPrivate = warm.privateBytes;
    for (unsigned round = 0; round < 3; ++round) {
        std::atomic<bool> sampleDone{false};
        std::thread sampler([&] {
            while (!sampleDone.load(std::memory_order_relaxed)) {
                sampledPeakPrivate = std::max(sampledPeakPrivate, memory().privateBytes);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
        try {
            seconds.push_back(concurrent(threads, [&](unsigned index) {
                unsigned long long local = 0;
                for (unsigned i = index; i < frames; i += threads) {
                    const auto frame = l2dae::render(cases[(i * 17 + round * 7) % cases.size()]);
                    require(!frame.rgba.empty(), "Empty benchmark frame.");
                    local += frame.rgba[frame.rgba.size() / 2] + frame.rgba.size();
                }
                checksum.fetch_add(local, std::memory_order_relaxed);
            }));
        } catch (...) { sampleDone = true; sampler.join(); throw; }
        sampleDone = true; sampler.join();
    }
    const auto finished = memory();
    std::sort(seconds.begin(), seconds.end());
    const double median = seconds[1];
    std::cout << std::fixed << std::setprecision(3)
        << "BENCH threads=" << threads << " frames=" << frames << " width=" << cases.front().width << " height=" << cases.front().height
        << " cold_s=" << coldSeconds << " median_s=" << median << " fps=" << frames / median
        << " ms_per_frame=" << median * 1000.0 / frames
        << " initial_private_mib=" << megabytes(initial.privateBytes) << " warm_private_mib=" << megabytes(warm.privateBytes)
        << " sampled_peak_private_mib=" << megabytes(sampledPeakPrivate) << " final_private_mib=" << megabytes(finished.privateBytes)
        << " working_mib=" << megabytes(finished.workingBytes) << " checksum=" << checksum << '\n';
#ifdef L2DAE_MFR_DIAGNOSTICS
    statistics(threads > 1);
#endif
    l2dae::releaseRenderer();
    const auto cleared = memory();
    std::cout << "AFTER_RELEASE private_mib=" << megabytes(cleared.privateBytes) << " working_mib=" << megabytes(cleared.workingBytes) << '\n';
}

void stress(std::vector<l2dae::RenderRequest> cases) {
    // Exercise animated settings in each independent MFR snapshot. Dump/compare
    // deliberately retain release defaults for backward-compatibility evidence.
    for (std::size_t i = 0; i < cases.size(); ++i) {
        cases[i].breathingAmount = static_cast<float>(i % 5) * .25f;
        cases[i].breathingPeriod = .5 + (i % 7) * 1.3;
        cases[i].blinkStrength = static_cast<float>(i % 4) / 3;
        cases[i].blinkInterval = .2 + (i % 6) * .7;
        cases[i].blinkDuration = .1 + (i % 4) * .3;
    }
    std::vector<l2dae::RenderResult> reference;
    for (const auto& request : cases) reference.push_back(l2dae::render(request));
#ifdef L2DAE_MFR_DIAGNOSTICS
    const auto singleResources = l2dae::rendererStats();
#endif
    for (unsigned threadCount : {4u, 8u}) {
        concurrent(threadCount, [&](unsigned thread) {
            for (unsigned call = 0; call < 12; ++call) {
                const auto index = (thread * 11 + call * 7) % cases.size();
                const auto frame = l2dae::render(cases[index]);
                require(frame.rgba == reference[index].rgba, "Concurrent frame differs from sequential reference.");
                if (call % 5 == 0) {
                    require(!l2dae::listMotions(cases[index].modelPath).empty(), "Concurrent motion listing failed.");
                    l2dae::listExpressions(cases[index].modelPath);
                }
            }
        });
        std::cout << "PASS: " << threadCount << " concurrent callers, " << 12 * threadCount
            << " exact mixed-size/frame comparisons with concurrent metadata reads.\n";
#ifdef L2DAE_MFR_DIAGNOSTICS
        statistics(true);
        const auto resources = l2dae::rendererStats();
        require(resources.sharedTextureBytes == singleResources.sharedTextureBytes && resources.sharedModelBytes == singleResources.sharedModelBytes,
            "Concurrent workers duplicated immutable texture or Moc resources.");
#endif
    }
    concurrent(4, [&](unsigned thread) {
        for (unsigned call = 0; call < 4; ++call) {
            const auto index = (thread * 11 + call * 7) % cases.size();
            if (thread == 0 && call == 1) {
                auto invalid = cases[index]; invalid.modelPath += L".missing";
                bool rejected = false;
                try { l2dae::render(invalid); } catch (const std::exception&) { rejected = true; }
                require(rejected, "Concurrent invalid model was not rejected.");
            }
            require(l2dae::render(cases[index]).rgba == reference[index].rgba,
                "Failed worker contaminated a concurrent or later frame.");
        }
    });
    std::cout << "PASS: concurrent failure isolation and recovery.\n";
    concurrent(5, [&](unsigned thread) {
        if (thread == 4) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); l2dae::releaseRenderer(); return; }
        for (unsigned call = 0; call < 3; ++call) {
            const auto index = (thread * 5 + call * 3) % cases.size();
            require(l2dae::render(cases[index]).rgba == reference[index].rgba,
                "Concurrent cache release changed frame or caused unsafe lifetime.");
        }
    });
    require(l2dae::render(cases[0]).rgba == reference[0].rgba, "Render after concurrent cache release differs.");
    std::cout << "PASS: cache release waits for active renders and subsequent initialization is deterministic.\n";
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        require(argc >= 2, "Usage: RenderConcurrency model3.json [--benchmark threads frames width height | --dump dir | --compare dir | --stress-pair model3.json]");
        const auto path = std::filesystem::absolute(argv[1]).wstring();
        const std::wstring mode = argc > 2 ? argv[2] : L"--stress";
        if (mode == L"--benchmark") {
            require(argc == 7, "Benchmark requires threads frames width height.");
            const unsigned threads = std::stoul(argv[3]), frames = std::stoul(argv[4]);
            require(threads > 0 && threads <= 32 && frames >= threads && frames <= 10000, "Invalid benchmark size.");
            benchmark(requests(path, std::stoi(argv[5]), std::stoi(argv[6]), false), threads, frames);
        } else if (mode == L"--dump" || mode == L"--compare") {
            require(argc == 4, "Dump/compare requires a folder.");
            dump(requests(path, 768, 768, true), argv[3], mode == L"--compare");
        } else if (mode == L"--stress-pair") {
            require(argc == 4, "Paired stress requires a second model.");
            auto cases = requests(path, 768, 768, true);
            const auto second = requests(std::filesystem::absolute(argv[3]).wstring(), 768, 768, true);
            std::vector<l2dae::RenderRequest> mixed;
            for (std::size_t i = 0; i < cases.size(); ++i) {
                mixed.push_back(cases[i]); mixed.push_back(second[i]);
            }
            stress(mixed);
        } else {
            require(mode == L"--stress", "Unknown harness mode.");
            stress(requests(path, 768, 768, true));
        }
        l2dae::releaseRenderer();
        return 0;
    } catch (const std::exception& error) {
        l2dae::releaseRenderer();
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

// Copyright (c) 2026 HyperCine Contributors.
// Dual-licensed under the MIT License or Apache License, Version 2.0.
// See LICENSE-MIT and LICENSE-APACHE for details.
#include "../tools/filmscan3_output_transaction.h"
#include <fstream>
#include <iostream>
#include <functional>
#include <future>

namespace fs = std::filesystem;
using hypercine_cli::OutputTransaction;
namespace {
void Check(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
void Refused(const std::function<void()>& action, const char* reason) {
    bool refused = false;
    try { action(); } catch (const std::exception&) { refused = true; }
    Check(refused, reason);
}
void Sentinel(const fs::path& path) {
    std::ofstream file(path, std::ios::binary); file << "retained evidence";
    file.close(); Check(bool(file), "cannot create test sentinel");
}
void Unchanged(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    const std::string value((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    Check(value == "retained evidence", "retained evidence changed");
}
fs::path Stage(const fs::path& parent) {
    for (const auto& entry : fs::directory_iterator(parent))
        if (entry.path().filename().string().find(".hypercine-incomplete-") == 0) return entry.path();
    throw std::runtime_error("expected staged output");
}
// Only this test's exclusively created fixture is recursively removed.
struct Fixture {
    fs::path root;
    Fixture() {
        const auto parent = fs::canonical(fs::temp_directory_path());
#ifdef _WIN32
        const auto pid = GetCurrentProcessId();
#else
        const auto pid = getpid();
#endif
        for (unsigned i = 0; i < 256; ++i) {
            const auto candidate = parent / ("hypercine-output-test-" + std::to_string(pid) + "-" + std::to_string(i));
            std::error_code error;
            if (fs::create_directory(candidate, error)) { root = candidate; return; }
            if (error) throw std::runtime_error("cannot create exclusive test fixture");
        }
        throw std::runtime_error("cannot reserve test fixture");
    }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    fs::path Dir(const char* name) {
        const auto path = root / name; Check(fs::create_directory(path), "fixture directory occupied"); return path;
    }
};
}

int main() {
    try {
        Fixture fixture;
        const auto input = fixture.Dir("retained"); Sentinel(input / "sentinel");
        unsigned checks = 0;
        {
            const auto parent = fixture.Dir("success");
            OutputTransaction run(parent / "result", {input});
            run.WriteNew("frame_0000.cfhd", "packet", 6);
            Check(!fs::exists(parent / "result"), "partial run became public");
            run.WriteNew("trace.json", "{}", 2); run.Publish();
            Check(fs::is_regular_file(parent / "result" / "trace.json"), "completed run was not published");
            ++checks;
        }
        {
            const auto parent = fixture.Dir("occupied-at-commit");
            OutputTransaction run(parent / "result", {input});
            run.WriteNew("trace.json", "{}", 2);
            fs::create_directory(parent / "result");
            Refused([&] { run.Publish(); }, "publish replaced an occupied empty directory");
            Check(fs::is_empty(parent / "result"), "empty incumbent directory changed");
            Sentinel(parent / "result" / "sentinel");
            Refused([&] { run.Publish(); }, "publish replaced retained results");
            Unchanged(parent / "result" / "sentinel"); ++checks;
        }
        {
            const auto parent = fixture.Dir("write-failure");
            OutputTransaction run(parent / "result", {input});
            run.WriteNew("frame_0000.cfhd", "packet", 6);
            Sentinel(Stage(parent) / "frame_0001.cfhd");
            Refused([&] { run.WriteNew("frame_0001.cfhd", "new", 3); }, "write replaced an occupied file");
            Unchanged(Stage(parent) / "frame_0001.cfhd");
            Check(!fs::exists(parent / "result"), "failed sequence published partial results"); ++checks;
        }
        {
            const auto parent = fixture.Dir("parent-replacement");
            const auto moved = fixture.root / "parent-moved";
            OutputTransaction run(parent / "result", {input});
            std::error_code error; fs::rename(parent, moved, error);
#ifdef _WIN32
            Check(bool(error), "Windows output parent was not locked against rename");
            run.WriteNew("trace.json", "{}", 2); run.Publish();
#else
            Check(!error, "cannot execute parent substitution test");
            fs::create_directory(parent); Sentinel(parent / "sentinel");
            run.WriteNew("trace.json", "{}", 2);
            Refused([&] { run.Publish(); }, "parent replacement was accepted");
            Check(!fs::exists(parent / "result") && !fs::exists(moved / "result"), "changed parent published results");
            Unchanged(parent / "sentinel");
#endif
            ++checks;
        }
        {
            const auto parent = fixture.Dir("stage-replacement");
            OutputTransaction run(parent / "result", {input});
            const auto stage = Stage(parent); const auto moved = parent / "moved-stage";
            std::error_code error; fs::rename(stage, moved, error);
#ifdef _WIN32
            Check(bool(error), "Windows staging directory was not locked against rename");
            run.WriteNew("trace.json", "{}", 2); run.Publish();
#else
            Check(!error, "cannot execute stage substitution test");
            fs::create_directory_symlink(input, stage);
            run.WriteNew("sentinel", "new packet", 10);
            Refused([&] { run.Publish(); }, "foreign staging name was accepted");
            Check(!fs::exists(parent / "result"), "foreign staging name was published");
            Check(fs::is_symlink(stage), "foreign stage was removed during cleanup");
#endif
            Unchanged(input / "sentinel"); ++checks;
        }
#ifndef _WIN32
        {
            const auto parent = fixture.Dir("parent-link-replacement");
            OutputTransaction run(parent / "result", {input});
            const auto moved = fixture.root / "linked-parent-moved";
            fs::rename(parent, moved); fs::create_directory_symlink(input, parent);
            run.WriteNew("sentinel", "new packet", 10);
            Refused([&] { run.Publish(); }, "parent symlink replacement was accepted");
            Unchanged(input / "sentinel"); Check(!fs::exists(input / "result"), "write escaped to retained root");
            ++checks;
        }
#endif
        {
            const auto parent = fixture.Dir("simultaneous-publication");
            OutputTransaction first(parent / "result", {input});
            OutputTransaction second(parent / "result", {input});
            first.WriteNew("winner", "first", 5);
            second.WriteNew("winner", "second", 6);
            std::promise<void> release; const auto start = release.get_future().share();
            auto publish = [&](OutputTransaction& run) {
                start.wait();
                try { run.Publish(); return true; } catch (const std::exception&) { return false; }
            };
            auto a = std::async(std::launch::async, [&] { return publish(first); });
            auto b = std::async(std::launch::async, [&] { return publish(second); });
            release.set_value();
            const bool won_a = a.get(), won_b = b.get();
            Check(won_a != won_b, "exactly one staged publisher must win");
            std::ifstream file(parent / "result" / "winner", std::ios::binary);
            const std::string winner((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            Check(winner == (won_a ? "first" : "second"), "winner content replaced");
            Check(fs::is_regular_file(Stage(parent) / "winner"), "loser staging evidence removed");
            ++checks;
        }
        {
            const auto parent = fixture.Dir("interrupted-scope");
            fs::path stage;
            {
                OutputTransaction run(parent / "result", {input});
                run.WriteNew("frame_0000.cfhd", "packet", 6);
                stage = Stage(parent);
            }
            Check(!fs::exists(parent / "result"), "unpublished scope became visible");
            Check(fs::file_size(stage / "frame_0000.cfhd") == 6, "interrupted evidence removed");
            ++checks;
        }
        {
            const auto parent = fixture.Dir("unicode-target");
            const auto name = fs::u8path(u8"r\u00e9sultat-\u03bb");
            OutputTransaction run(parent / name, {input});
            run.WriteNew("trace.json", "{}", 2); run.Publish();
            Check(fs::file_size(parent / name / "trace.json") == 2, "UTF-16 target truncated");
            ++checks;
        }
#ifdef _WIN32
        {
            const auto parent = fixture.Dir("case-alias-at-publication");
            OutputTransaction run(parent / "Result", {input});
            run.WriteNew("trace.json", "{}", 2);
            fs::create_directory(parent / "RESULT"); Sentinel(parent / "RESULT" / "sentinel");
            Refused([&] { run.Publish(); }, "case alias incumbent was overwritten");
            Unchanged(parent / "RESULT" / "sentinel"); ++checks;
        }
#endif
        Unchanged(input / "sentinel");
        std::cout << "output transaction safety: " << checks << " scenarios PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "output transaction safety: " << error.what() << '\n'; return 1;
    }
}

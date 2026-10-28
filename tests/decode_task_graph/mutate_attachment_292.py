# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under MIT OR Apache-2.0. See LICENSE-MIT and LICENSE-APACHE.
"""Compile private guard-removal mutants and require a witnessed red predicate."""
import argparse
import json
from pathlib import Path
import re
import subprocess

def run(command, cwd):
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=180)
    if result.returncode:
        raise RuntimeError(result.stdout[-4000:] + result.stderr[-4000:])

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("work", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=False)
    source = (root / "src/core/decode_task_graph.cpp").read_text()
    header = (root / "src/core/decode_s3_plane.h").read_text()
    cmake = ["cmake_minimum_required(VERSION 3.25)", "project(S3Mutants LANGUAGES CXX)",
             "set(CMAKE_CXX_STANDARD 17)", "find_package(Threads REQUIRED)"]
    identity, count = re.subn(
        r"    if \(source.task.key.channel != expected.channel.*?return empty;",
        "", source, count=1, flags=re.S)
    if count != 1:
        raise RuntimeError("identity anchor drift")
    channel_guard = "source.task.key.channel != expected.channel || "
    if source.count(channel_guard) != 1:
        raise RuntimeError("channel anchor drift")
    budget_guard = "if (bytes > generation_budget_ || owned_budget_ > generation_budget_ - bytes) return false;"
    if source.count(budget_guard) != 2:
        raise RuntimeError("budget anchor drift")
    extent_guard = "plane.bytes < need || plane.bytes > kS3MaxPlaneBytes"
    if header.count(extent_guard) != 1:
        raise RuntimeError("extent anchor drift")
    for name in ("identity", "channel", "budget", "extent"):
        directory = work / name
        directory.mkdir()
        (directory / "decode_task_graph.cpp").write_text(
            identity if name == "identity" else
            source.replace(channel_guard, "") if name == "channel" else
            source.replace(budget_guard, "") if name == "budget" else source)
        (directory / "decode_task_graph.h").write_bytes((root / "src/core/decode_task_graph.h").read_bytes())
        (directory / "decode_s3_plane.h").write_text(
            header.replace(extent_guard, "false") if name == "extent" else header)
        # Mirror the complete small boundary: mixing original and copied headers
        # in one TU defeats pragma-once and tests a compile error, not a guard.
        for relative in ("src/core/decode_s3_attachment.h", "src/core/decode_s3_consumer.h",
                         "src/cpu/decode_spatial_scalar.h", "src/core/decode_s3_attachment.cpp",
                         "src/core/decode_s3_consumer.cpp", "src/cpu/decode_spatial_scalar.cpp"):
            (directory / Path(relative).name).write_bytes((root / relative).read_bytes())
        test = "test_scheduler_t4_plane_144.cpp" if name == "budget" else "test_decoder_attachment_292.cpp"
        test_source = (root / "tests/decode_task_graph" / test).read_text()
        test_source = test_source.replace("../reference_scalar_inverse/synthetic_vectors.h",
            (root / "tests/reference_scalar_inverse/synthetic_vectors.h").as_posix())
        (directory / test).write_text(test_source)
        files = [directory / "decode_task_graph.cpp", directory / test]
        if name != "budget":
            files += [directory / "decode_s3_attachment.cpp",
                      directory / "decode_s3_consumer.cpp",
                      directory / "decode_spatial_scalar.cpp"]
        cmake += [f"add_executable({name} " + " ".join('"' + f.as_posix() + '"' for f in files) + ")",
                  f'target_include_directories({name} PRIVATE "{directory.as_posix()}" "{root.as_posix()}/src/core" "{root.as_posix()}/src/cpu")',
                  f"target_link_libraries({name} PRIVATE Threads::Threads)"]
    (work / "CMakeLists.txt").write_text("\n".join(cmake))
    run(["cmake", "-S", str(work), "-B", str(work / "build")], work)
    run(["cmake", "--build", str(work / "build"), "--config", "Release", "--parallel", "4"], work)
    records = []
    for name in ("identity", "channel", "budget", "extent"):
        exe = work / "build" / "Release" / (name + ".exe")
        if not exe.exists():
            exe = work / "build" / name
        result = subprocess.run([str(exe), "4"], capture_output=True, text=True, timeout=30)
        output = result.stdout + result.stderr
        broken = next((line for line in output.splitlines() if "BROKEN" in line), "")
        if result.returncode == 0 or not broken:
            raise RuntimeError("mutant survived or failed without witness: " + name + output)
        records.append(dict(mutant=name, exit=result.returncode, first_predicate=broken))
    (work / "results.json").write_text(json.dumps(records, indent=2) + "\n")
    print(json.dumps(records, indent=2))

if __name__ == "__main__":
    main()

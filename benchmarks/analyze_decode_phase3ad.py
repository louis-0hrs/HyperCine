# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Regret-oriented comparison of metadata-only frame dispatch models.

Selection is fixed before the measured corpus is inspected:
1. Fit rules on development regret. Choose tree and linear hyperparameters by
   leave-one-family-out regret on development only.
2. Refit the chosen configuration on all development cases.
3. A class is eligible when a 1080p-only refit reaches 70 percent capture on
   validation 4K, or beats the baseline there, and when novel validation
   families are no worse than the baseline in mean regret or reach 80 percent
   capture.
4. Among eligible classes, minimize validation total regret. Models within
   max(2 ms, 2 percent) of the best regret yield to the simpler model.
5. Hold-out rows are rejected. This script never reads them.
"""
import hashlib
import json
import math
import sys
from pathlib import Path

ELIGIBLE = [
    "l0_share", "l1_share", "l2_share", "l0_over_l1", "l0_over_l2", "l1_over_l2",
    "y_share", "u_share", "v_share", "chroma_share", "uv_asym", "top1_share", "top3_share", "top6_share",
    "dense_count", "ratio_dense_count", "dense_l0", "dense_l1", "dense_l2", "dense_y", "dense_chroma",
    "bpco_mean", "bpco_max", "bpco_median", "bpco_top3", "bpco_l0", "bpco_l1", "bpco_l2", "bpco_y", "bpco_chroma",
    "n_bpco_ge_50", "n_bpco_ge_150", "n_bpco_ge_400", "n_bpco_ge_800", "gini_milli", "hhi_permille",
    "mad_permille", "cv_permille", "p25_share", "p75_share", "p90_share", "l0_max_share", "l1_max_share",
    "l2_max_share", "y_max_share", "chroma_max_share", "y_top_within", "chroma_top_within", "quant_min",
    "quant_max", "quant_mean", "cb_cs9", "cb_cs17", "cb_cs18", "diff_count", "active_bpco_mean",
]
SHADOW = ["n_bytes_ge_4096", "n_bytes_ge_16384", "n_bytes_ge_32768", "n_bytes_ge_65536",
          "n_bytes_ge_131072", "n_bytes_ge_262144", "width", "height", "total_payload"]
SELECTION_RULE = Path(__file__).read_text(encoding="utf-8").split('"""', 2)[1].strip()


def load_cases(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    cases = [c for c in data["cases"] if c.get("supported")]
    if any(c.get("split") == "holdout" for c in data["cases"]):
        raise SystemExit("refusing to analyze a file that contains hold-out cases")
    for case in cases:
        verify_case(case)
    return data, cases


def verify_case(case):
    features = case["features"]
    bands = case.get("bands")
    if not bands:
        return
    if len(bands) != 27:
        raise SystemExit(f"{case['id']} does not have 27 highpass bands")
    total = sum(b["bytes"] for b in bands)
    ordered = sorted(b["bytes"] for b in bands)
    top = ordered[-1]
    share = (top * 1000 // total) if total else 0
    if features["top1_share"] != share or features["total_payload"] != min(total, 0x7FFFFFFF):
        raise SystemExit(f"{case['id']} feature mismatch against raw bands")
    sum_ix = sum((i + 1) * value for i, value in enumerate(ordered))
    gini = (2000 * sum_ix - 1000 * 28 * total) // (27 * total) if total else 0
    if features["gini_milli"] != gini:
        raise SystemExit(f"{case['id']} gini mismatch {features['gini_milli']} != {gini}")
    level = case["level_bytes"]
    concentrated = case["highpass_bytes"] and level[1] * 100 >= case["highpass_bytes"] * 75
    distributed = case["highpass_bytes"] >= 262144 and case["maximum_band_bytes"] >= 65536
    baseline = "hybrid" if (not concentrated and (features["dense_count"] or distributed)) else "lightweight"
    if baseline != case["baseline_strategy"]:
        raise SystemExit(f"{case['id']} baseline mismatch")


def latency(case, strategy):
    return case["lightweight"]["mean_ms"] if strategy == "lightweight" else case["hybrid"]["mean_ms"]


def oracle_strategy(case):
    return "lightweight" if case["lightweight"]["mean_ms"] < case["hybrid"]["mean_ms"] else "hybrid"


def delta_ms(case):
    return case["hybrid"]["mean_ms"] - case["lightweight"]["mean_ms"]


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, math.ceil(fraction * len(ordered)) - 1))
    return ordered[index]


def metrics(cases, predict):
    if not cases:
        return {"cases": 0, "capture": 1.0, "total_regret_ms": 0.0, "mean_regret_ms": 0.0, "median_regret_ms": 0.0,
                "p95_regret_ms": 0.0, "worst_regret_ms": 0.0, "matches": 0, "misses": 0, "catastrophic": [],
                "sum_selected_ms": 0.0, "sum_oracle_ms": 0.0}
    selected = oracle = light = hybrid = 0.0
    regrets, catastrophic, misses, matches = [], [], [], 0
    for case in cases:
        choice = predict(case)
        best = oracle_strategy(case)
        chosen_ms = latency(case, choice)
        best_ms = latency(case, best)
        regret = chosen_ms - best_ms
        selected += chosen_ms
        oracle += best_ms
        light += case["lightweight"]["mean_ms"]
        hybrid += case["hybrid"]["mean_ms"]
        regrets.append(regret)
        if choice == best:
            matches += 1
        else:
            misses.append({"id": case["id"], "family": case["family"], "choice": choice, "oracle": best,
                           "regret_ms": regret, "delta_ms": delta_ms(case), "width": case["width"]})
        relative = regret / best_ms if best_ms else 0.0
        if regret > 2.0 or relative > 0.20:
            catastrophic.append(misses[-1] if choice != best else {"id": case["id"], "regret_ms": regret, "relative": relative})
    best_fixed = min(light, hybrid)
    span = best_fixed - oracle
    capture = 1.0 if span <= 1e-12 else (best_fixed - selected) / span
    ordered = sorted(regrets)
    return {"cases": len(cases), "capture": capture, "total_regret_ms": selected - oracle,
            "mean_regret_ms": (selected - oracle) / len(cases), "median_regret_ms": ordered[len(ordered) // 2],
            "p95_regret_ms": percentile(ordered, 0.95), "worst_regret_ms": ordered[-1], "matches": matches,
            "misses": len(misses), "miss_list": misses, "catastrophic": catastrophic,
            "catastrophic_count": len(catastrophic), "sum_selected_ms": selected, "sum_oracle_ms": oracle,
            "sum_lightweight_ms": light, "sum_hybrid_ms": hybrid}


def baseline_predict(case):
    return case["baseline_strategy"]


def ambiguity(cases, predict):
    rows = []
    for label, pred in (("absolute_0.10", lambda c: abs(delta_ms(c)) < 0.10),
                        ("absolute_0.25", lambda c: abs(delta_ms(c)) < 0.25),
                        ("absolute_0.50", lambda c: abs(delta_ms(c)) < 0.50),
                        ("relative_0.02", lambda c: abs(delta_ms(c)) < 0.02 * min(c["lightweight"]["mean_ms"], c["hybrid"]["mean_ms"])),
                        ("relative_0.05", lambda c: abs(delta_ms(c)) < 0.05 * min(c["lightweight"]["mean_ms"], c["hybrid"]["mean_ms"])),
                        ("relative_0.10", lambda c: abs(delta_ms(c)) < 0.10 * min(c["lightweight"]["mean_ms"], c["hybrid"]["mean_ms"]))):
        kept = [c for c in cases if not pred(c)]
        item = metrics(kept, predict)
        item["excluded"] = len(cases) - len(kept)
        item["threshold"] = label
        item.pop("miss_list", None)
        item.pop("catastrophic", None)
        rows.append(item)
    return rows


def groups_of(cases):
    found = {}
    for case in cases:
        found.setdefault(case["family"], []).append(case)
    return found


def best_constant(cases):
    light = sum(c["lightweight"]["mean_ms"] for c in cases)
    hybrid = sum(c["hybrid"]["mean_ms"] for c in cases)
    return ("lightweight" if light < hybrid else "hybrid"), min(light, hybrid)


def fit_tree(cases, depth, min_leaf, features):
    prediction, cost = best_constant(cases)
    node = {"feature": None, "threshold": None, "left": None, "right": None, "prediction": prediction}
    if depth <= 0 or len(cases) < min_leaf * 2:
        return node
    best = None
    for feature in features:
        values = sorted({c["features"][feature] for c in cases})
        if len(values) < 2:
            continue
        for threshold in values[:-1]:
            left, right = [], []
            for case in cases:
                (left if case["features"][feature] <= threshold else right).append(case)
            if len(left) < min_leaf or len(right) < min_leaf:
                continue
            gain = cost - best_constant(left)[1] - best_constant(right)[1]
            rank = (-gain, feature, threshold)
            if gain > 0 and (best is None or rank < best[0]):
                best = (rank, feature, threshold, left, right)
    if best is None:
        return node
    node["feature"], node["threshold"] = best[1], best[2]
    node["prediction"] = None
    node["left"] = fit_tree(best[3], depth - 1, min_leaf, features)
    node["right"] = fit_tree(best[4], depth - 1, min_leaf, features)
    return node


def tree_predict(node, case):
    while node["feature"] is not None:
        node = node["left"] if case["features"][node["feature"]] <= node["threshold"] else node["right"]
    return node["prediction"]


def tree_nodes(node):
    if node["feature"] is None:
        return 0
    return 1 + tree_nodes(node["left"]) + tree_nodes(node["right"])


def tree_features(node, found=None):
    found = set() if found is None else found
    if node["feature"] is not None:
        found.add(node["feature"])
        tree_features(node["left"], found)
        tree_features(node["right"], found)
    return sorted(found)


def lofo_total(cases, fit_predict):
    folded = groups_of(cases)
    total = 0.0
    counted = 0
    for family, test in folded.items():
        train = [c for c in cases if c["family"] != family]
        predict = fit_predict(train)
        total += metrics(test, predict)["total_regret_ms"]
        counted += len(test)
    return total, total / counted if counted else 0.0


RULES = []
for bpco in (100, 200, 350, 500, 800):
    for top3 in (450, 600, 750, 900):
        for level in (250, 400, 550, 750):
            for dense in (0, 1, 3, 6):
                RULES.append(("r1", (bpco, top3, level, dense)))
    for level in (250, 400, 550, 750):
        for gini in (200, 350, 500, 700, 900):
            RULES.append(("r2", (bpco, gini, level)))
    for hhi in (80, 150, 250, 400):
        for chroma in (200, 400, 700, 1000):
            RULES.append(("r3", (bpco, hhi, chroma)))


def rule_predict(case, kind, params):
    f = case["features"]
    if kind == "r1":
        hybrid = f["bpco_mean"] >= params[0] and f["top3_share"] <= params[1] and f["l1_share"] <= params[2] and f["ratio_dense_count"] >= params[3]
    elif kind == "r2":
        hybrid = f["bpco_mean"] >= params[0] and f["gini_milli"] <= params[1] and f["l1_share"] <= params[2]
    else:
        hybrid = f["active_bpco_mean"] >= params[0] and f["hhi_permille"] <= params[1] and f["chroma_share"] <= params[2]
    return "hybrid" if hybrid else "lightweight"


def fit_rule(cases):
    best = None
    for kind, params in RULES:
        predict = lambda case, kind=kind, params=params: rule_predict(case, kind, params)
        regret = metrics(cases, predict)["total_regret_ms"]
        complexity = 4 if kind == "r1" else 3
        if best is None or regret < best[0] or (regret == best[0] and complexity < best[1]):
            best = (regret, complexity, kind, params, predict)
    return {"kind": best[2], "params": best[3], "complexity": best[1], "predict": best[4]}


def scale_stats(cases):
    stats = {}
    for feature in ELIGIBLE:
        values = sorted(c["features"][feature] for c in cases)
        n = len(values)
        median = values[n // 2]
        iqr = max(1, values[3 * (n - 1) // 4] - values[(n - 1) // 4])
        stats[feature] = (median, iqr)
    return stats


def fit_linear(cases, weighted, l2, steps=500, lr=0.05):
    if not cases:
        return {"weights": {}, "bias": 0.0, "threshold": 0.0, "stats": {}, "predict": lambda case: "lightweight"}
    stats = scale_stats(cases)
    names = ELIGIBLE
    weights = [0.0] * (len(names) + 1)
    for _ in range(steps):
        grad = [0.0] * len(weights)
        for case in cases:
            values = [(case["features"][name] - stats[name][0]) / stats[name][1] for name in names]
            values.append(1.0)
            score = sum(w * x for w, x in zip(weights, values))
            score = max(-30.0, min(30.0, score))
            prob = 1.0 / (1.0 + math.exp(-score))
            label = 1.0 if oracle_strategy(case) == "hybrid" else 0.0
            weight = abs(delta_ms(case)) if weighted else 1.0
            error = weight * (prob - label)
            for i, value in enumerate(values):
                grad[i] += error * value
        for i in range(len(names)):
            grad[i] += l2 * weights[i]
        inv = lr / len(cases)
        for i in range(len(weights)):
            weights[i] -= inv * grad[i]
    raw = []
    for case in cases:
        values = [(case["features"][name] - stats[name][0]) / stats[name][1] for name in names]
        raw.append((sum(w * x for w, x in zip(weights, values)) + weights[-1], case))
    raw.sort(key=lambda item: item[0])
    best_threshold, best_regret = 0.0, None
    for index, (score, _) in enumerate(raw):
        threshold = score
        def predict(case, threshold=threshold):
            values = [(case["features"][name] - stats[name][0]) / stats[name][1] for name in names]
            got = sum(w * x for w, x in zip(weights, values)) + weights[-1]
            return "hybrid" if got >= threshold else "lightweight"
        regret = metrics(cases, predict)["total_regret_ms"]
        if best_regret is None or regret < best_regret:
            best_regret, best_threshold = regret, threshold
    def final_predict(case, threshold=best_threshold, weights=weights, stats=stats, names=names):
        values = [(case["features"][name] - stats[name][0]) / stats[name][1] for name in names]
        got = sum(w * x for w, x in zip(weights, values)) + weights[-1]
        return "hybrid" if got >= threshold else "lightweight"
    stored = {name: weights[i] for i, name in enumerate(names) if abs(weights[i]) > 1e-6}
    return {"weights": stored, "raw_weights": weights, "bias": weights[-1], "threshold": best_threshold,
            "stats": stats, "predict": final_predict, "names": names}


def integer_linear_matches(model, cases):
    scale = 1 << 20
    stats, names, weights = model["stats"], model["names"], model["raw_weights"]
    int_w = [int(round(weights[i] / stats[name][1] * scale)) for i, name in enumerate(names)]
    bias = int(round(weights[-1] * scale))
    threshold = int(round(model["threshold"] * scale))
    medians = [stats[name][0] for name in names]
    mismatches = 0
    for case in cases:
        score = bias
        for i, name in enumerate(names):
            score += int_w[i] * (case["features"][name] - medians[i])
        integer = "hybrid" if score >= threshold else "lightweight"
        if integer != model["predict"](case):
            mismatches += 1
    return mismatches, {"scale": scale, "weights": int_w, "bias": bias, "threshold": threshold, "medians": medians, "names": names}


def fit_bins(cases, triple):
    if not cases:
        return lambda case: "lightweight"
    edges = []
    for feature in triple:
        values = sorted(c["features"][feature] for c in cases)
        edges.append((values[len(values) // 3], values[2 * len(values) // 3]))
    cells = {}
    for case in cases:
        key = tuple(0 if case["features"][feature] <= edges[i][0] else 1 if case["features"][feature] <= edges[i][1] else 2
                    for i, feature in enumerate(triple))
        cells.setdefault(key, []).append(case)
    table = {key: best_constant(value)[0] for key, value in cells.items()}
    default = best_constant(cases)[0]
    def predict(case, table=table, edges=edges, triple=triple, default=default):
        key = tuple(0 if case["features"][feature] <= edges[i][0] else 1 if case["features"][feature] <= edges[i][1] else 2
                    for i, feature in enumerate(triple))
        return table.get(key, default)
    return {"triple": triple, "edges": edges, "table": {str(k): v for k, v in table.items()}, "default": default, "predict": predict}


BIN_TRIPLES = (
    ("bpco_mean", "top3_share", "l1_share"),
    ("bpco_mean", "gini_milli", "ratio_dense_count"),
    ("active_bpco_mean", "hhi_permille", "chroma_share"),
)


def choose_models(dev):
    rule = fit_rule(dev)
    rule_lofo, rule_mean = lofo_total(dev, lambda train: fit_rule(train)["predict"])
    tree_options = []
    for depth in range(2, 7):
        for min_leaf in (4, 8, 12):
            def fit_one(train, depth=depth, min_leaf=min_leaf):
                fitted = fit_tree(train, depth, min_leaf, ELIGIBLE)
                return lambda case, fitted=fitted: tree_predict(fitted, case)
            total, mean = lofo_total(dev, fit_one)
            tree_options.append((total, depth, -min_leaf, mean))
    tree_options.sort()
    tree_total, tree_depth, tree_leaf_key, tree_mean = tree_options[0]
    tree_leaf = -tree_leaf_key
    tree = fit_tree(dev, tree_depth, tree_leaf, ELIGIBLE)
    linear_options = []
    for weighted in (False, True):
        for l2 in (0.001, 0.1):
            total, mean = lofo_total(dev, lambda train, weighted=weighted, l2=l2: fit_linear(train, weighted, l2)["predict"])
            linear_options.append((total, weighted, l2, mean))
    linear_options.sort()
    linear_total, linear_weighted, linear_l2, linear_mean = linear_options[0]
    linear = fit_linear(dev, linear_weighted, linear_l2)
    bin_options = []
    for triple in BIN_TRIPLES:
        total, mean = lofo_total(dev, lambda train, triple=triple: fit_bins(train, triple)["predict"])
        bin_options.append((total, triple, mean))
    bin_options.sort()
    bins = fit_bins(dev, bin_options[0][1])
    shadow = fit_tree(dev, 4, 8, ELIGIBLE + SHADOW)
    return {
        "baseline": {"name": "baseline", "complexity": 0, "predict": baseline_predict, "lofo_total_regret_ms": metrics(dev, baseline_predict)["total_regret_ms"],
                     "hyperparameters": {"rule": "phase3a fixed thresholds"}, "features": ["highpass", "level1_share", "maximum_band", "dense_count"]},
        "rule": {"name": "rule", "complexity": rule["complexity"], "predict": rule["predict"], "lofo_total_regret_ms": rule_lofo,
                 "hyperparameters": {"family": rule["kind"], "params": rule["params"]}, "features": ["bpco_mean", "top3_share", "l1_share", "ratio_dense_count", "gini_milli", "active_bpco_mean", "hhi_permille", "chroma_share"]},
        "tree": {"name": "tree", "complexity": tree_nodes(tree), "predict": lambda case, tree=tree: tree_predict(tree, case), "tree": tree,
                 "lofo_total_regret_ms": tree_total, "hyperparameters": {"depth": tree_depth, "min_leaf": tree_leaf, "lofo_options": [
                     {"depth": d, "min_leaf": -k, "lofo_total_regret_ms": t} for t, d, k, _ in tree_options]},
                 "features": tree_features(tree)},
        "linear": {"name": "linear", "complexity": len(linear["weights"]), "predict": linear["predict"], "model": linear,
                   "lofo_total_regret_ms": linear_total, "hyperparameters": {"weighted": linear_weighted, "l2": linear_l2},
                   "features": sorted(linear["weights"])},
        "bins": {"name": "bins", "complexity": 27, "predict": bins["predict"], "model": bins, "lofo_total_regret_ms": bin_options[0][0],
                 "hyperparameters": {"features": bins["triple"], "edges": bins["edges"]}, "features": list(bins["triple"])},
        "shadow_tree": {"name": "shadow_tree_ineligible", "complexity": tree_nodes(shadow), "predict": lambda case, tree=shadow: tree_predict(tree, case),
                        "eligible": False, "features": tree_features(shadow), "hyperparameters": {"depth": 4, "min_leaf": 8, "includes_absolute_features": True}},
    }


def refit_width(model, cases):
    if model["name"] == "baseline":
        return baseline_predict
    if model["name"] == "rule":
        return fit_rule(cases)["predict"]
    if model["name"] == "tree":
        depth = model["hyperparameters"]["depth"]
        leaf = model["hyperparameters"]["min_leaf"]
        tree = fit_tree(cases, depth, leaf, ELIGIBLE)
        return lambda case, tree=tree: tree_predict(tree, case)
    if model["name"] == "linear":
        return fit_linear(cases, model["hyperparameters"]["weighted"], model["hyperparameters"]["l2"])["predict"]
    if model["name"] == "bins":
        return fit_bins(cases, tuple(model["hyperparameters"]["features"]))["predict"]
    return model["predict"]


def guards(model, dev, val):
    reasons = []
    dev_families = {c["family"] for c in dev}
    val4 = [c for c in val if 3840 <= c["width"] < 7680]
    if val4:
        predict = refit_width(model, [c for c in dev if c["width"] <= 1920])
        got, base = metrics(val4, predict), metrics(val4, baseline_predict)
        ok = got["capture"] >= 0.70 or got["total_regret_ms"] <= base["total_regret_ms"] + 1e-9
        reasons.append({"guard": "train_1080_test_validation_4k", "pass": ok, "capture": got["capture"], "total_regret_ms": got["total_regret_ms"],
                        "baseline_total_regret_ms": base["total_regret_ms"], "cases": len(val4)})
        if not ok and model["name"] != "baseline":
            return False, reasons
    novel = [c for c in val if c["family"] not in dev_families]
    if novel:
        got, base = metrics(novel, model["predict"]), metrics(novel, baseline_predict)
        ok = got["mean_regret_ms"] <= base["mean_regret_ms"] + 1e-12 or got["capture"] >= 0.80
        reasons.append({"guard": "novel_validation_families", "pass": ok, "capture": got["capture"], "mean_regret_ms": got["mean_regret_ms"],
                        "baseline_mean_regret_ms": base["mean_regret_ms"], "cases": len(novel)})
        if not ok and model["name"] != "baseline":
            return False, reasons
    return True, reasons


def public_metrics(item):
    keep = dict(item)
    keep.pop("miss_list", None)
    return keep


def emit_tree(node, indent="    "):
    if node["feature"] is None:
        kind = "Hybrid" if node["prediction"] == "hybrid" else "Lightweight"
        return f"{indent}return DecodeStrategy::{kind};\n"
    text = f"{indent}if (f.{node['feature']} <= {int(node['threshold'])}) {{\n"
    text += emit_tree(node["left"], indent + "    ")
    text += f"{indent}}} else {{\n"
    text += emit_tree(node["right"], indent + "    ")
    text += f"{indent}}}\n"
    return text


def write_candidate(path, name, body):
    path.write_text(
        "// Copyright (c) 2026 HyperCine Contributors.\n"
        "// Dual-licensed under the MIT License or Apache License, Version 2.0.\n"
        "// Frozen by analyze_decode_phase3ad.py. Do not edit by hand after hold-out starts.\n"
        f"static constexpr bool kPhase3adCandidateFrozen = true;\n"
        f"static constexpr const char* kPhase3adCandidateName = \"{name}\";\n"
        "inline HyperCine::DecodeStrategy Phase3adFrozenCandidate(const HyperCine::Phase3adFeatures& f) {\n"
        "    using HyperCine::DecodeStrategy;\n"
        f"{body}"
        "}\n", encoding="utf-8")


def candidate_body(selected):
    if selected["name"] == "baseline":
        return ("    const bool concentrated = f.total_payload > 0 && f.l1_share >= 750;\n"
                "    const bool distributed = f.total_payload >= 262144 && f.top1_share > 0;\n"
                "    (void)concentrated; (void)distributed;\n"
                "    return f.total_payload >= 262144 && f.l1_share < 750 && (f.dense_count > 0 || f.n_bytes_ge_65536 > 0)\n"
                "        ? DecodeStrategy::Hybrid : DecodeStrategy::Lightweight;\n")
    if selected["name"] == "rule":
        kind, params = selected["hyperparameters"]["family"], selected["hyperparameters"]["params"]
        if kind == "r1":
            test = f"f.bpco_mean >= {params[0]} && f.top3_share <= {params[1]} && f.l1_share <= {params[2]} && f.ratio_dense_count >= {params[3]}"
        elif kind == "r2":
            test = f"f.bpco_mean >= {params[0]} && f.gini_milli <= {params[1]} && f.l1_share <= {params[2]}"
        else:
            test = f"f.active_bpco_mean >= {params[0]} && f.hhi_permille <= {params[1]} && f.chroma_share <= {params[2]}"
        return f"    return ({test}) ? DecodeStrategy::Hybrid : DecodeStrategy::Lightweight;\n"
    if selected["name"] == "tree":
        return emit_tree(selected["tree"])
    if selected["name"] == "bins":
        model = selected["model"]
        lines = ["    int bin0 = f.%s <= %d ? 0 : f.%s <= %d ? 1 : 2;\n" % (model["triple"][0], model["edges"][0][0], model["triple"][0], model["edges"][0][1]),
                 "    int bin1 = f.%s <= %d ? 0 : f.%s <= %d ? 1 : 2;\n" % (model["triple"][1], model["edges"][1][0], model["triple"][1], model["edges"][1][1]),
                 "    int bin2 = f.%s <= %d ? 0 : f.%s <= %d ? 1 : 2;\n" % (model["triple"][2], model["edges"][2][0], model["triple"][2], model["edges"][2][1]),
                 "    const int key = bin0 * 9 + bin1 * 3 + bin2;\n",
                 "    static const int table[27] = {"]
        mapping = {"lightweight": 0, "hybrid": 1}
        values = []
        raw = {eval(k): v for k, v in model["table"].items()}
        for i in range(27):
            key = (i // 9, (i // 3) % 3, i % 3)
            values.append(str(mapping[raw.get(key, model["default"])]))
        lines.append(", ".join(values))
        lines.append("};\n    return table[key] ? DecodeStrategy::Hybrid : DecodeStrategy::Lightweight;\n")
        return "".join(lines)
    raise SystemExit("linear export is handled separately")


def dump(path, payload):
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def self_test():
    def case(name, family, split, width, bpco, top3, level, dense, light, hybrid):
        features = {key: 0 for key in ELIGIBLE + SHADOW}
        features.update({"bpco_mean": bpco, "top3_share": top3, "l1_share": level, "ratio_dense_count": dense,
                         "active_bpco_mean": bpco, "gini_milli": top3, "hhi_permille": top3 // 5, "chroma_share": 300,
                         "width": width, "height": width * 9 // 16, "total_payload": bpco * 1000})
        return {"id": name, "family": family, "split": split, "width": width, "height": features["height"], "supported": True,
                "baseline_strategy": "hybrid" if bpco > 400 else "lightweight", "features": features,
                "lightweight": {"mean_ms": light}, "hybrid": {"mean_ms": hybrid}, "highpass_bytes": features["total_payload"],
                "maximum_band_bytes": 0, "level_bytes": [0, 0, 0]}
    dev, val = [], []
    for i in range(12):
        bpco = 100 + i * 80
        light, hybrid = (12.0, 5.0) if bpco >= 500 and i % 3 != 0 else (4.0, 8.0)
        dev.append(case(f"d{i}", f"fam{i % 4}", "development", 1920, bpco, 800 - i * 20, 400, 2 if bpco > 400 else 0, light, hybrid))
        val.append(case(f"v{i}", f"new{i % 3}", "validation", 3840 if i % 2 else 1920, bpco, 800 - i * 20, 400, 2 if bpco > 400 else 0, light, hybrid))
    models = choose_models(dev)
    got = metrics(val, models["tree"]["predict"])
    if got["total_regret_ms"] > metrics(val, baseline_predict)["total_regret_ms"]:
        raise SystemExit("self-test tree did not reduce validation regret on separable data")
    print("analyze_decode_phase3ad self-test PASS")


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--self-test":
        self_test()
        return
    if len(sys.argv) < 3:
        raise SystemExit("usage: analyze_decode_phase3ad.py measurements.json output_dir [--freeze]")
    source, out = Path(sys.argv[1]), Path(sys.argv[2])
    freeze = "--freeze" in sys.argv
    out.mkdir(parents=True, exist_ok=True)
    raw, cases = load_cases(source)
    dev = [c for c in cases if c["split"] == "development"]
    val = [c for c in cases if c["split"] == "validation"]
    if not dev or not val:
        raise SystemExit("development and validation splits are both required")
    models = choose_models(dev)
    dev_families = {c["family"] for c in dev}
    comparison, validation, cross = [], {"cases": []}, {"resolution": []}
    eligible = []
    for name in ("baseline", "rule", "tree", "linear", "bins", "shadow_tree"):
        model = models[name]
        dev_m, val_m = metrics(dev, model["predict"]), metrics(val, model["predict"])
        passed, reasons = (False, [{"guard": "absolute features are diagnostic only", "pass": False}]) if name == "shadow_tree" else guards(model, dev, val)
        if name == "linear":
            mismatches, integer = integer_linear_matches(model["model"], dev + val)
            model["integer"] = integer
            model["integer_mismatches"] = mismatches
            if mismatches:
                passed = False
                reasons.append({"guard": "integer_equivalence", "pass": False, "mismatches": mismatches})
        row = {"model": name, "eligible": passed, "complexity": model["complexity"], "features": model["features"],
               "hyperparameters": model["hyperparameters"], "development": public_metrics(dev_m), "validation": public_metrics(val_m),
               "lofo_total_regret_ms": model.get("lofo_total_regret_ms"), "guards": reasons,
               "runtime_cost": "allocation-free integer metadata arithmetic; measured after the candidate is compiled",
               "validation_ambiguity": ambiguity(val, model["predict"])}
        comparison.append(row)
        if passed:
            eligible.append((val_m["total_regret_ms"], model["complexity"], name))
        train_1080 = [c for c in dev if c["width"] <= 1920]
        predict_1080 = refit_width(model, train_1080) if name != "shadow_tree" else model["predict"]
        for label, subset in (("development_4k", [c for c in dev if c["width"] == 3840]),
                              ("development_8k", [c for c in dev if c["width"] >= 7680]),
                              ("validation_4k", [c for c in val if c["width"] == 3840])):
            item = public_metrics(metrics(subset, predict_1080))
            item.update({"model": name, "experiment": "train_development_1080_only", "tested": label})
            cross["resolution"].append(item)
    if not eligible:
        raise SystemExit("no eligible model, including baseline")
    eligible.sort()
    best = eligible[0][0]
    slack = max(2.0, 0.02 * best)
    pool = [item for item in eligible if item[0] <= best + slack]
    pool.sort(key=lambda item: (item[1], item[0], item[2]))
    selected_name = pool[0][2]
    selected = models[selected_name]
    val_m = metrics(val, selected["predict"])
    base_m = metrics(val, baseline_predict)
    satisfactory = (selected_name != "baseline" and val_m["capture"] >= 0.90 and val_m["total_regret_ms"] <= base_m["total_regret_ms"] * 0.80)
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    script_hash = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    validation.update({"selected_model": selected_name, "satisfactory_for_production_change": satisfactory,
                       "selection_rule": SELECTION_RULE, "measurement_sha256": digest, "analyzer_sha256": script_hash,
                       "development_cases": len(dev), "validation_cases": len(val),
                       "selected_validation": metrics(val, selected["predict"]),
                       "selected_development": metrics(dev, selected["predict"]),
                       "novel_validation_families": sorted({c["family"] for c in val if c["family"] not in dev_families})})
    dataset = {"license": "MIT OR Apache-2.0", "split_process": raw.get("split_process"), "measurement_sha256": digest,
               "cases": [{"id": c["id"], "split": c["split"], "origin": c["origin"], "family": c["family"], "generator": c["generator"],
                          "sibling": c["sibling"], "previously_in_phase3a_or_rc": c["previously_in_phase3a_or_rc"],
                          "width": c["width"], "height": c["height"], "quality": c["quality"], "seed": c["seed"],
                          "sample_hash": c["sample_hash"], "source_hash": c["source_hash"], "source_identity": c["source_identity"],
                          "license": c.get("license", "")} for c in raw["cases"]]}
    features = {"license": "MIT OR Apache-2.0", "eligible_features": ELIGIBLE, "shadow_features": SHADOW, "cases": [
        {"id": c["id"], "split": c["split"], "family": c["family"], "width": c["width"], "oracle": oracle_strategy(c),
         "delta_ms": delta_ms(c), "baseline": c["baseline_strategy"], "features": c["features"]} for c in cases]}
    oracle = {"license": "MIT OR Apache-2.0", "definition": "Lightweight if lightweight_mean < hybrid_mean else Hybrid",
              "note": "Phase 3A-RC rows are previously observed development data, not hold-out evidence.",
              "cases": [{"id": c["id"], "split": c["split"], "family": c["family"], "width": c["width"],
                         "lightweight_ms": c["lightweight"]["mean_ms"], "hybrid_ms": c["hybrid"]["mean_ms"],
                         "delta_ms": delta_ms(c), "oracle": oracle_strategy(c), "baseline": c["baseline_strategy"],
                         "selected": selected["predict"](c), "regret_ms": latency(c, selected["predict"](c)) - latency(c, oracle_strategy(c))}
                        for c in cases]}
    dump(out / "decode_phase3ad_dataset.json", dataset)
    dump(out / "decode_phase3ad_features.json", features)
    dump(out / "decode_phase3ad_model_comparison.json", {"license": "MIT OR Apache-2.0", "selection_rule": SELECTION_RULE,
         "selected_model": selected_name, "satisfactory_for_production_change": satisfactory, "models": comparison})
    dump(out / "decode_phase3ad_cross_validation.json", {"license": "MIT OR Apache-2.0", "method": "leave one content family out of development; resolution rows refit on development width <= 1920", **cross,
         "tree_lofo_grid": models["tree"]["hyperparameters"].get("lofo_options", [])})
    dump(out / "decode_phase3ad_validation.json", validation)
    dump(out / "decode_phase3ad_oracle.json", oracle)
    if freeze:
        name = f"phase3ad_{selected_name}"
        if selected_name == "linear":
            integer = selected["integer"]
            terms = [f"int64_t({integer['bias']})"]
            for weight, median, feature in zip(integer["weights"], integer["medians"], integer["names"]):
                if weight:
                    terms.append(f"{weight}LL * (int64_t(f.{feature}) - {median}LL)")
            body = "    const int64_t score = " + " + ".join(terms) + ";\n"
            body += f"    return score >= {integer['threshold']}LL ? DecodeStrategy::Hybrid : DecodeStrategy::Lightweight;\n"
        elif selected_name == "baseline":
            body = ("    DecodeFrameInfo info;\n"
                    "    (void)info; (void)f;\n"
                    "    const bool concentrated = f.l1_share >= 750 && f.total_payload > 0;\n"
                    "    const bool distributed = f.total_payload >= 262144 && f.n_bytes_ge_65536 > 0;\n"
                    "    return !concentrated && (f.dense_count > 0 || distributed) ? DecodeStrategy::Hybrid : DecodeStrategy::Lightweight;\n")
        else:
            body = candidate_body(selected)
        write_candidate(Path(__file__).resolve().parent / "phase3ad_frozen_candidate.inc", name, body)
        frozen = {"license": "MIT OR Apache-2.0", "frozen": True, "candidate_name": name, "model": selected_name,
                  "features": selected["features"], "hyperparameters": selected["hyperparameters"],
                  "satisfactory_for_production_change": satisfactory, "measurement_sha256": digest,
                  "analyzer_sha256": script_hash, "selection_rule": SELECTION_RULE,
                  "development_ids": [{"id": c["id"], "sample_hash": c["sample_hash"]} for c in dev],
                  "validation_ids": [{"id": c["id"], "sample_hash": c["sample_hash"]} for c in val]}
        if selected_name == "tree":
            frozen["tree"] = selected["tree"]
        dump(out / "decode_phase3ad_candidate_frozen.json", frozen)
        print(f"FROZEN {name} satisfactory={satisfactory} validation_capture={val_m['capture']:.4f} regret={val_m['total_regret_ms']:.3f}")
    else:
        print(f"selected {selected_name} without freezing; validation capture {metrics(val, selected['predict'])['capture']:.4f}")


if __name__ == "__main__":
    main()

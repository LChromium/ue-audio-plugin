from __future__ import annotations

import argparse
import csv
import math
import statistics
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class MetricSummary:
    samples: int
    median: float
    p95: float
    maximum: float


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "captures",
        nargs="+",
        help="Source-count to CSV mapping, for example 8=D:/Project/Saved/Profiling/CSV/Profile.csv",
    )
    parser.add_argument("--warmup-frames", type=int, default=120)
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def percentile(sorted_values: list[float], ratio: float) -> float:
    if not sorted_values:
        return 0.0
    index = min(max(math.ceil((len(sorted_values) - 1) * ratio), 0), len(sorted_values) - 1)
    return sorted_values[index]


def summarize(values: list[float]) -> MetricSummary:
    positive = sorted(value for value in values if math.isfinite(value) and value > 0.0)
    if not positive:
        return MetricSummary(0, 0.0, 0.0, 0.0)
    return MetricSummary(
        samples=len(positive),
        median=statistics.median(positive),
        p95=percentile(positive, 0.95),
        maximum=positive[-1],
    )


def load_capture(path: Path, warmup_frames: int) -> dict[str, MetricSummary]:
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.reader(handle)
        headers = next(reader)
        audio_column = next(
            (
                index
                for index, name in enumerate(headers)
                if name.startswith("Audio/AudioMixerRenderThread") and name.endswith("/RenderAudio")
            ),
            None,
        )
        required = {
            "game_thread_ms": headers.index("GameThreadTime"),
            "render_thread_ms": headers.index("RenderThreadTime"),
            "gpu_ms": headers.index("GPUTime"),
            "vram_mb": headers.index("GPUMem/LocalUsedMB"),
            "process_cpu_percent": headers.index("CPUUsage_Process"),
        }
        if audio_column is None:
            raise RuntimeError(f"CSV has no AudioMixer RenderAudio column: {path}")
        required["audio_render_ms"] = audio_column

        values: dict[str, list[float]] = {name: [] for name in required}
        frame_index = 0
        for row in reader:
            if len(row) < len(headers):
                continue
            frame_index += 1
            if frame_index <= max(warmup_frames, 0):
                continue
            for metric_name, column_index in required.items():
                try:
                    values[metric_name].append(float(row[column_index]))
                except (ValueError, IndexError):
                    pass

    if frame_index <= warmup_frames:
        raise RuntimeError(
            f"CSV has only {frame_index} data frames, not enough for {warmup_frames} warmup frames: {path}"
        )
    return {name: summarize(metric_values) for name, metric_values in values.items()}


def render_markdown(captures: list[tuple[int, Path]], warmup_frames: int) -> str:
    lines = [
        "# UERayTracingAudio Source Scaling",
        "",
        f"UE CSV captures; first {warmup_frames} frames discarded as warm-up. Times are milliseconds.",
        "",
        "| Sources | Game p50/p95 | Render p50/p95 | GPU p50/p95 | Audio callback p50/p95 | VRAM max MB | Process CPU p50/p95 |",
        "|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for source_count, path in captures:
        metrics = load_capture(path, warmup_frames)
        game = metrics["game_thread_ms"]
        render = metrics["render_thread_ms"]
        gpu = metrics["gpu_ms"]
        audio = metrics["audio_render_ms"]
        vram = metrics["vram_mb"]
        cpu = metrics["process_cpu_percent"]
        lines.append(
            f"| {source_count} | {game.median:.3f}/{game.p95:.3f} | "
            f"{render.median:.3f}/{render.p95:.3f} | {gpu.median:.3f}/{gpu.p95:.3f} | "
            f"{audio.median:.3f}/{audio.p95:.3f} | {vram.maximum:.1f} | "
            f"{cpu.median:.1f}/{cpu.p95:.1f} |"
        )
    lines.extend(
        (
            "",
            "Audio values include only positive AudioMixer RenderAudio callback samples; VRAM is UE's `GPUMem/LocalUsedMB` counter.",
            "",
        )
    )
    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    captures: list[tuple[int, Path]] = []
    for capture in args.captures:
        source_text, separator, path_text = capture.partition("=")
        if not separator:
            raise RuntimeError(f"Capture must use SOURCES=PATH syntax: {capture}")
        path = Path(path_text).expanduser().resolve()
        if not path.is_file():
            raise RuntimeError(f"CSV capture does not exist: {path}")
        captures.append((int(source_text), path))
    captures.sort(key=lambda item: item[0])

    markdown = render_markdown(captures, args.warmup_frames)
    print(markdown)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(markdown, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

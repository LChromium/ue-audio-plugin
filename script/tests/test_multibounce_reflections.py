from __future__ import annotations

import sys
import unittest
from pathlib import Path


SCRIPT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPT_ROOT))

import validate_multibounce_reflections


def make_manifest(bounces: int) -> dict[str, object]:
    is_multi = bounces > 1
    paths = 20000 if is_multi else 4000
    energy = 0.046 if is_multi else 0.040
    late = 0.00002 if is_multi else 0.0
    bins = 1600 if is_multi else 550
    delay = 0.013 if is_multi else 0.011
    return {
        "direct_preset": "clear",
        "reflection_bounce_count": bounces,
        "reflection_ray_count": 4096,
        "hardware_ray_tracing": True,
        "has_cpu_reference": True,
        "automatic_checks_passed": True,
        "audio_safety_checks_passed": True,
        "samples_finite": True,
        "impulse_response_channels": 2,
        "directional_wet_is_distinct": True,
        "wet_stereo_normalized_difference": 0.35,
        "input_asset": "/Game/Audio/Input.Input",
        "scene_signature": "scene",
        "direct_distance_cm": 200.0,
        "hardware_indirect_valid_paths": paths,
        "cpu_reference_indirect_valid_paths": paths,
        "hardware_indirect_gain": energy,
        "cpu_reference_indirect_gain": energy,
        "hardware_early_reflection_gain": energy - late,
        "cpu_reference_early_reflection_gain": energy - late,
        "hardware_late_reverb_gain": late,
        "cpu_reference_late_reverb_gain": late,
        "hardware_impulse_response_energy": energy,
        "cpu_reference_impulse_response_energy": energy,
        "hardware_directional_energy_ratio": 0.66,
        "cpu_reference_directional_energy_ratio": 0.66,
        "hardware_directional_bin_count": bins,
        "cpu_reference_directional_bin_count": bins,
        "hardware_dominant_arrival_direction_x": 0.5,
        "hardware_dominant_arrival_direction_y": 0.7,
        "hardware_dominant_arrival_direction_z": -0.5,
        "cpu_reference_dominant_arrival_direction_x": 0.5,
        "cpu_reference_dominant_arrival_direction_y": 0.7,
        "cpu_reference_dominant_arrival_direction_z": -0.5,
        "hardware_average_delay_seconds": delay,
    }


class MultiBounceReflectionTests(unittest.TestCase):
    def test_accepts_actual_multi_bounce_growth(self) -> None:
        metrics = validate_multibounce_reflections.validate_multibounce_reflections(
            make_manifest(1), make_manifest(8)
        )
        self.assertGreater(float(metrics["multi_paths"]), float(metrics["single_paths"]))
        self.assertGreater(float(metrics["multi_late_reverb_gain"]), 0.0)

    def test_rejects_zero_hardware_directionality(self) -> None:
        multi = make_manifest(8)
        multi["hardware_directional_energy_ratio"] = 0.0
        multi["hardware_directional_bin_count"] = 0
        with self.assertRaisesRegex(RuntimeError, "directional"):
            validate_multibounce_reflections.validate_multibounce_reflections(
                make_manifest(1), multi
            )

    def test_rejects_parameter_only_multi_bounce(self) -> None:
        single = make_manifest(1)
        multi = make_manifest(8)
        multi["hardware_indirect_valid_paths"] = single["hardware_indirect_valid_paths"]
        multi["cpu_reference_indirect_valid_paths"] = single["cpu_reference_indirect_valid_paths"]
        with self.assertRaisesRegex(RuntimeError, "adds valid reflection paths"):
            validate_multibounce_reflections.validate_multibounce_reflections(single, multi)


if __name__ == "__main__":
    unittest.main()

from __future__ import annotations

import sys
import unittest
from pathlib import Path


SCRIPT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPT_ROOT))

import validate_direct_occlusion_pair


def make_manifest(preset: str) -> dict[str, object]:
    is_clear = preset == "clear"
    is_hard = preset == "hard_occluded"
    return {
        "direct_preset": preset,
        "hardware_ray_tracing": True,
        "automatic_checks_passed": True,
        "audio_safety_checks_passed": True,
        "samples_finite": True,
        "impulse_response_channels": 2,
        "directional_wet_is_distinct": is_clear,
        "wet_stereo_normalized_difference": 0.35 if is_clear else 0.0,
        "direct_semantics_passed": True,
        "clipped_sample_count": 0,
        "direct_dropout_window_count": 0,
        "modes_are_distinct": True,
        "direct_dry_correlation": 1.0,
        "input_asset": "/Game/Audio/Input.Input",
        "scene_signature": "ABC123",
        "direct_distance_cm": 200.0,
        "direct_distance_attenuation": 0.5,
        "direct_visibility": 1.0 if is_clear else 0.0,
        "direct_occlusion": 1.0 if is_clear else (0.0 if is_hard else 0.35),
        "direct_gain": 0.498 if is_clear else (0.0 if is_hard else 0.1743),
        "full_to_reference_rms_ratio": 0.02 if is_hard else 0.1,
        "reflection_ray_count": 4096,
        "reflection_bounce_count": 8,
        "has_cpu_reference": True,
        "hardware_indirect_valid_paths": 200 if is_clear else 80,
        "cpu_reference_indirect_valid_paths": 198 if is_clear else 79,
        "hardware_indirect_gain": 0.04 if is_clear else 0.003,
        "cpu_reference_indirect_gain": 0.039 if is_clear else 0.00295,
        "hardware_impulse_response_energy": 0.04 if is_clear else 0.003,
        "cpu_reference_impulse_response_energy": 0.039 if is_clear else 0.00295,
        "hardware_early_reflection_gain": 0.03 if is_clear else 0.002,
        "cpu_reference_early_reflection_gain": 0.03 if is_clear else 0.002,
        "hardware_late_reverb_gain": 0.001 if is_clear else 0.0001,
        "cpu_reference_late_reverb_gain": 0.001 if is_clear else 0.0001,
        "hardware_directional_energy_ratio": 0.65,
        "cpu_reference_directional_energy_ratio": 0.65,
        "hardware_directional_bin_count": 100,
        "cpu_reference_directional_bin_count": 100,
    }


class DirectOcclusionPairTests(unittest.TestCase):
    def test_accepts_same_distance_occlusion_only_pair(self) -> None:
        result = validate_direct_occlusion_pair.validate_direct_occlusion_pair(
            make_manifest("clear"),
            make_manifest("soft_occluded"),
            make_manifest("hard_occluded"),
        )
        self.assertAlmostEqual(
            float(result["soft_to_clear_direct_gain_ratio"]),
            0.35,
        )

    def test_rejects_distance_confounded_pair(self) -> None:
        clear = make_manifest("clear")
        soft = make_manifest("soft_occluded")
        soft["direct_distance_cm"] = 400.0
        soft["direct_distance_attenuation"] = 0.25
        with self.assertRaisesRegex(RuntimeError, "equal positive source/listener distance"):
            validate_direct_occlusion_pair.validate_direct_occlusion_pair(
                clear,
                soft,
                make_manifest("hard_occluded"),
            )

    def test_rejects_missing_cpu_reference_energy(self) -> None:
        clear = make_manifest("clear")
        soft = make_manifest("soft_occluded")
        soft["cpu_reference_impulse_response_energy"] = 0.0
        with self.assertRaisesRegex(RuntimeError, "positive hardware/CPU IR energy"):
            validate_direct_occlusion_pair.validate_direct_occlusion_pair(
                clear,
                soft,
                make_manifest("hard_occluded"),
            )

    def test_rejects_clear_scene_without_directional_stereo_wet(self) -> None:
        clear = make_manifest("clear")
        clear["directional_wet_is_distinct"] = False
        clear["wet_stereo_normalized_difference"] = 0.0
        with self.assertRaisesRegex(RuntimeError, "directional wet distinction"):
            validate_direct_occlusion_pair.validate_direct_occlusion_pair(
                clear,
                make_manifest("soft_occluded"),
                make_manifest("hard_occluded"),
            )

    def test_rejects_hard_occlusion_with_audible_direct_leak(self) -> None:
        hard = make_manifest("hard_occluded")
        hard["direct_gain"] = 0.05
        with self.assertRaisesRegex(RuntimeError, "hard-occluded direct silence"):
            validate_direct_occlusion_pair.validate_direct_occlusion_pair(
                make_manifest("clear"),
                make_manifest("soft_occluded"),
                hard,
            )


if __name__ == "__main__":
    unittest.main()

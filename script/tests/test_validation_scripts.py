from __future__ import annotations

import json
import io
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path


SCRIPT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPT_ROOT))

import launch_runtime_validation
import validation_environment


def make_direct_sweep_summary(
    *,
    passed: int = 1,
    generations: int = 12,
    distance_min: str | float = "199.500",
    distance_max: str | float = "200.500",
    visibility_min: str | float = "0.050000",
    visibility_max: str | float = "0.950000",
    gain_min: str | float = "0.250000",
    gain_max: str | float = "1.000000",
    max_gain_step: str | float = "0.00900000",
    direct_dropouts: int = 0,
    restored: int = 1,
    hardware: int = 1,
) -> str:
    return (
        f"UERayTracingAudio direct sweep: passed={passed} "
        f"generations={generations} "
        f"distance_min_cm={distance_min} distance_max_cm={distance_max} "
        f"visibility_min={visibility_min} visibility_max={visibility_max} "
        f"gain_min={gain_min} gain_max={gain_max} "
        f"max_gain_step={max_gain_step} "
        f"direct_dropouts={direct_dropouts} "
        f"restored={restored} hardware={hardware}"
    )


def make_data_source_summary(
    *,
    hybrid_non_silent: int = 30,
    realtime_wet_input_rms_ratio: float = 0.07,
    realtime_integrated_wet_input_rms_ratio: float = 0.07,
    realtime_full_peak: float = 0.8,
    realtime_over_unit: int = 0,
    realtime_wet_present: int = 27,
    realtime_max_silent_run: int = 2,
    realtime_audible_wet: int = 27,
    realtime_max_inaudible_run: int = 2,
    hard_realtime_passed: int = 1,
    hard_realtime_callbacks: int = 300,
    hard_realtime_capacity_misses: int = 0,
    convolution_prepare_drops: int = 0,
) -> str:
    return (
        make_direct_sweep_summary()
        + "\nUERayTracingAudio validation data sources: passed=1 "
        "baked_buffers=30 baked_input_non_silent=30 baked_non_silent=30 "
        "baked_rms_measured=30 baked_audible_wet=27 "
        "baked_max_inaudible_run=2 baked_wet_present=27 "
        "baked_max_silent_run=2 baked_wet_input_rms_ratio=0.080000 "
        "baked_integrated_wet_input_rms_ratio=0.080000 "
        "baked_full_peak=0.800000 baked_over_unit=0 "
        "realtime_buffers=30 realtime_input_non_silent=30 "
        "realtime_non_silent=30 realtime_rms_measured=30 "
        f"realtime_audible_wet={realtime_audible_wet} "
        f"realtime_max_inaudible_run={realtime_max_inaudible_run} "
        f"realtime_wet_present={realtime_wet_present} "
        f"realtime_max_silent_run={realtime_max_silent_run} "
        f"realtime_wet_input_rms_ratio={realtime_wet_input_rms_ratio:.6f} "
        "realtime_integrated_wet_input_rms_ratio="
        f"{realtime_integrated_wet_input_rms_ratio:.6f} "
        f"realtime_full_peak={realtime_full_peak:.6f} "
        f"realtime_over_unit={realtime_over_unit} "
        "hybrid_buffers=30 hybrid_input_non_silent=30 "
        f"hybrid_non_silent={hybrid_non_silent} "
        "hybrid_rms_measured=30 hybrid_audible_wet=27 "
        "hybrid_max_inaudible_run=2 hybrid_wet_present=27 "
        "hybrid_max_silent_run=2 hybrid_wet_input_rms_ratio=0.060000 "
        "hybrid_integrated_wet_input_rms_ratio=0.060000 "
        "hybrid_full_peak=0.800000 hybrid_over_unit=0 "
        "minimum_wet_input_rms_ratio=0.050000 "
        "minimum_wet_presence_fraction=0.800 non_finite=0 stereo_ir=1 "
        "baked_kernels=2 realtime_kernels=2 hybrid_kernels=4 "
        "audio_playing=1 interactive_hybrid_reverb=1 parametric_tail=1 "
        'reason="passed".\n'
        "UERayTracingAudio hard realtime: "
        f"passed={hard_realtime_passed} "
        f"callbacks={hard_realtime_callbacks} "
        "callback_capacity_misses="
        f"{hard_realtime_capacity_misses} "
        "convolution_prepare_drops="
        f"{convolution_prepare_drops}."
    )


def make_interactive_smoke_summary(
    *,
    passed: int = 1,
    moved_cm: float = 215.250,
    foreign_audio_playing: int = 0,
    muted_foreign_audio: int = 1,
    ab_restart_count: int = 0,
) -> str:
    return (
        f"UERayTracingAudio interactive smoke: passed={passed} "
        f"moved_cm={moved_cm:.3f} "
        "listener_camera_error_cm=0.000 origin_error_cm=0.000 "
        "realtime=1 baked=1 hybrid=1 rendered_ab=1 reference_ab=1 "
        "fixed_view=1 interactive_view=1 audio_playing=1 reference_playing=1 "
        "ab_base_levels_matched=1 "
        f"ab_restart_count={ab_restart_count} "
        f"foreign_audio_playing={foreign_audio_playing} "
        f"muted_foreign_audio={muted_foreign_audio}."
    )


class FakeRunningProcess:
    def poll(self) -> None:
        return None


class ValidationEnvironmentTests(unittest.TestCase):
    def test_resolve_explicit_engine_root(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            engine_root = Path(directory) / "UE_5.7"
            (engine_root / "Engine" / "Build" / "BatchFiles").mkdir(parents=True)
            (engine_root / "Engine" / "Binaries" / "Win64").mkdir(parents=True)
            (engine_root / "Engine" / "Build" / "BatchFiles" / "Build.bat").touch()
            (engine_root / "Engine" / "Binaries" / "Win64" / "UnrealEditor.exe").touch()

            self.assertEqual(validation_environment.resolve_engine_root(engine_root), engine_root.resolve())

    def test_discovers_enabled_workspace_test_project(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo_root = root / "plugin-repo"
            project_path = (
                repo_root
                / "TestProject"
                / "UeVersion1"
                / "UeVersion1.uproject"
            )
            external_project_path = root / "UeVersion1" / "UeVersion1.uproject"
            project_path.parent.mkdir(parents=True)
            external_project_path.parent.mkdir()
            project_path.write_text(
                json.dumps({"Plugins": [{"Name": "UERayTracingAudio", "Enabled": True}]}),
                encoding="utf-8",
            )
            external_project_path.write_text(
                json.dumps({"Plugins": [{"Name": "UERayTracingAudio", "Enabled": True}]}),
                encoding="utf-8",
            )

            self.assertEqual(
                validation_environment.resolve_project_path(None, repo_root, "UERayTracingAudio"),
                project_path.resolve(),
            )


class RuntimeValidationTests(unittest.TestCase):
    def validate_direct_sweep(
        self,
        log_text: str,
    ) -> dict[str, float | int]:
        self.assertTrue(
            hasattr(launch_runtime_validation, "validate_direct_sweep"),
            "runtime launcher must expose the strict Direct sweep gate",
        )
        return launch_runtime_validation.validate_direct_sweep(log_text)

    def test_default_game_window_covers_cold_start(self) -> None:
        self.assertGreaterEqual(launch_runtime_validation.DEFAULT_GAME_SECONDS, 180.0)

    def test_generated_validation_audio_cannot_be_reused_as_input(self) -> None:
        self.assertTrue(
            launch_runtime_validation.is_original_project_input_asset(
                "/Game/FirstPerson/Audio/MarchingBand.MarchingBand"
            )
        )
        self.assertFalse(
            launch_runtime_validation.is_original_project_input_asset(
                "/Game/UERayTracingAudio/ValidationAudio/Run/Direct.Direct"
            )
        )

    def test_game_command_supports_source_scaling_csv_profile(self) -> None:
        command = launch_runtime_validation.build_game_command(
            Path("UnrealEditor.exe"),
            Path("Test.uproject"),
            Path("Game.log"),
            source_count=32,
            csv_profile=True,
            bake_repeatability=True,
        )
        self.assertIn("-UERayTracingAudioValidationSourceCount=32", command)
        self.assertIn("-UERayTracingAudioPerformanceProfile", command)
        self.assertIn("-csvCategories=Audio", command)
        self.assertIn("-UERayTracingAudioValidationBakeRepeatability", command)
        self.assertIn("/Game/FirstPerson/Lvl_FirstPerson", command)
        self.assertNotIn("/Engine/Maps/Entry", command)
        self.assertIn(
            "-ini:Engine:[Audio]:UnfocusedVolumeMultiplier=1.0",
            command,
        )

    def test_game_command_enables_automatic_direct_sweep(self) -> None:
        command = launch_runtime_validation.build_game_command(
            Path("UnrealEditor.exe"),
            Path("Test.uproject"),
            Path("Game.log"),
        )
        self.assertIn("-UERayTracingAudioValidationDirectSweep", command)

    def test_game_command_supports_interactive_smoke(self) -> None:
        command = launch_runtime_validation.build_game_command(
            Path("UnrealEditor.exe"),
            Path("Test.uproject"),
            Path("Game.log"),
            interactive_smoke=True,
        )
        self.assertIn("-UERayTracingAudioInteractiveValidation", command)
        self.assertIn("-UERayTracingAudioInteractiveSmoke", command)

    def test_editor_command_requests_visible_ab_scene(self) -> None:
        command = launch_runtime_validation.build_editor_command(
            Path("UnrealEditor.exe"),
            Path("Test.uproject"),
            Path("Editor.log"),
            editor_ab_artifacts=True,
        )
        self.assertIn("-UERayTracingAudioValidationScenario", command)
        self.assertIn("-UERayTracingAudioValidationEditorBake", command)
        self.assertIn("-UERayTracingAudioValidationDirectPreset=clear", command)
        self.assertIn(
            "-ini:Engine:[Audio]:UnfocusedVolumeMultiplier=1.0",
            command,
        )
        self.assertIn(
            launch_runtime_validation.EDITOR_VISIBLE_SCENE_MARKER,
            launch_runtime_validation.EDITOR_VALIDATION_MARKERS,
        )
        self.assertNotIn("-UERayTracingAudioValidationDirectSweep", command)

    def test_direct_sweep_gate_accepts_one_strict_hardware_marker(self) -> None:
        values = self.validate_direct_sweep(
            "LogUERayTracingAudio: Display: "
            + make_direct_sweep_summary()
        )
        self.assertEqual(values["passed"], 1)
        self.assertEqual(values["generations"], 12)
        self.assertEqual(values["distance_min"], 199.5)
        self.assertEqual(values["distance_max"], 200.5)
        self.assertEqual(values["direct_dropouts"], 0)
        self.assertEqual(values["restored"], 1)
        self.assertEqual(values["hardware"], 1)

    def test_direct_sweep_gate_rejects_missing_partial_and_ambiguous_markers(
        self,
    ) -> None:
        with self.assertRaisesRegex(
            RuntimeError,
            "missing parseable hardware Direct sweep",
        ):
            self.validate_direct_sweep("ordinary runtime output")

        with self.assertRaisesRegex(
            RuntimeError,
            "missing parseable hardware Direct sweep",
        ):
            self.validate_direct_sweep(
                make_direct_sweep_summary() + " trailing_partial_field="
            )

        marker = make_direct_sweep_summary()
        with self.assertRaisesRegex(
            RuntimeError,
            "ambiguous hardware Direct sweep markers",
        ):
            self.validate_direct_sweep(marker + "\n" + marker)

    def test_direct_sweep_gate_rejects_each_failed_requirement(self) -> None:
        cases = (
            (
                "failed terminal state",
                {"passed": 0},
                "passing Direct sweep",
            ),
            (
                "too few generations",
                {"generations": 7},
                "at least eight Direct generations",
            ),
            (
                "distance below range",
                {"distance_min": "197.999"},
                "constant two-metre distance",
            ),
            (
                "distance above range",
                {"distance_max": "202.001"},
                "constant two-metre distance",
            ),
            (
                "missing occluded endpoint",
                {"visibility_min": "0.100001"},
                "Clear and Occluded visibility endpoints",
            ),
            (
                "missing clear endpoint",
                {"visibility_max": "0.899999"},
                "Clear and Occluded visibility endpoints",
            ),
            (
                "zero soft gain",
                {"gain_min": "0.0"},
                "nonzero Soft Occlusion gain",
            ),
            (
                "excessive gain step",
                {"max_gain_step": "0.01000001"},
                "bounded per-sample gain step",
            ),
            (
                "direct dropout",
                {"direct_dropouts": 1},
                "zero Direct dropouts",
            ),
            (
                "state not restored",
                {"restored": 0},
                "restored Source state",
            ),
            (
                "CPU provenance",
                {"hardware": 0},
                "hardware provenance",
            ),
        )
        for name, overrides, expected_reason in cases:
            with self.subTest(name=name):
                with self.assertRaisesRegex(RuntimeError, expected_reason):
                    self.validate_direct_sweep(
                        make_direct_sweep_summary(**overrides)
                    )

    def test_direct_sweep_gate_rejects_non_finite_metrics(self) -> None:
        with self.assertRaisesRegex(
            RuntimeError,
            "finite distance_min",
        ):
            self.validate_direct_sweep(
                make_direct_sweep_summary(distance_min="1e309")
            )

    def test_direct_sweep_gate_combines_failure_reasons(self) -> None:
        with self.assertRaises(RuntimeError) as raised:
            self.validate_direct_sweep(
                make_direct_sweep_summary(
                    passed=0,
                    generations=3,
                    direct_dropouts=2,
                    restored=0,
                    hardware=0,
                )
            )
        message = str(raised.exception)
        for expected_reason in (
            "passing Direct sweep",
            "at least eight Direct generations",
            "zero Direct dropouts",
            "restored Source state",
            "hardware provenance",
        ):
            self.assertIn(expected_reason, message)

    def test_data_source_gate_requires_direct_sweep_first(self) -> None:
        data_source_only = make_data_source_summary().split("\n", 1)[1]
        with redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(
                RuntimeError,
                "missing parseable hardware Direct sweep",
            ):
                launch_runtime_validation.print_audio_path_summary(
                    data_source_only,
                    "Game",
                    require_data_sources=True,
                )

    def test_editor_command_supports_separate_interactive_runtime(self) -> None:
        command = launch_runtime_validation.build_editor_command(
            Path("UnrealEditor.exe"),
            Path("Test.uproject"),
            Path("Editor.log"),
            interactive_runtime=True,
        )
        self.assertIn("-UERayTracingAudioValidationScenario", command)
        self.assertIn("-UERayTracingAudioInteractiveValidation", command)
        self.assertNotIn("-UERayTracingAudioValidationEditorBake", command)
        required_markers = launch_runtime_validation.editor_required_markers(
            editor_ab_artifacts=False,
            interactive_runtime=True,
        )
        self.assertEqual(
            required_markers,
            (launch_runtime_validation.EDITOR_INTERACTIVE_READY_MARKER,),
        )
        self.assertNotIn(
            launch_runtime_validation.EDITOR_VISIBLE_SCENE_MARKER,
            required_markers,
        )

    def test_editor_ab_artifacts_require_enabled_listening_ui(self) -> None:
        required_markers = launch_runtime_validation.editor_required_markers(
            editor_ab_artifacts=True
        )
        self.assertIn(
            launch_runtime_validation.EDITOR_AB_ARTIFACTS_MARKER,
            required_markers,
        )
        self.assertIn(
            launch_runtime_validation.EDITOR_LISTENING_UI_MARKER,
            required_markers,
        )

    def test_editor_command_selects_direct_preset(self) -> None:
        command = launch_runtime_validation.build_editor_command(
            Path("UnrealEditor.exe"),
            Path("Test.uproject"),
            Path("Editor.log"),
            editor_ab_artifacts=True,
            editor_direct_preset="soft_occluded",
        )
        self.assertIn(
            "-UERayTracingAudioValidationDirectPreset=soft_occluded",
            command,
        )
        self.assertIn(
            "-UERayTracingAudioValidationReflectionBounces=8",
            command,
        )

        with self.assertRaisesRegex(ValueError, "Unknown Editor direct preset"):
            launch_runtime_validation.build_editor_command(
                Path("UnrealEditor.exe"),
                Path("Test.uproject"),
                Path("Editor.log"),
                editor_direct_preset="unknown",
            )

    def test_acoustic_summary_requires_occlusion_and_indirect_energy(self) -> None:
        log_text = "\n".join(
            (
                launch_runtime_validation.AUDIO_DIRECT_PATH_MARKERS[0],
                launch_runtime_validation.AUDIO_INDIRECT_PATH_MARKERS[0],
                "UERayTracingAudio validation result: sources=4 direct_batch_sources=4 indirect_batch_sources=4 visibility=1.0000 valid_paths=0 indirect_gain=0.000000",
                "UERayTracingAudio validation CPU reference: hardware_paths=0 cpu_paths=0 path_relative_delta=0.0000 hardware_gain=0.000000 cpu_gain=0.000000 gain_relative_delta=0.0000.",
            )
        )
        with redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "occlusion response"):
                launch_runtime_validation.print_audio_path_summary(
                    log_text,
                    "Game",
                    require_validation=True,
                )

    def test_acoustic_summary_accepts_meaningful_result(self) -> None:
        log_text = "\n".join(
            (
                launch_runtime_validation.AUDIO_DIRECT_PATH_MARKERS[0],
                launch_runtime_validation.AUDIO_INDIRECT_PATH_MARKERS[0],
                "UERayTracingAudio validation result: sources=4 direct_batch_sources=4 indirect_batch_sources=4 visibility=0.0000 valid_paths=12 indirect_gain=0.250000",
                "UERayTracingAudio validation CPU reference: hardware_paths=12 cpu_paths=12 path_relative_delta=0.0000 hardware_gain=0.250000 cpu_gain=0.250000 gain_relative_delta=0.0000.",
            )
        )
        with redirect_stdout(io.StringIO()):
            launch_runtime_validation.print_audio_path_summary(
                log_text,
                "Game",
                require_validation=True,
            )

    def test_runtime_data_source_summary_requires_audible_three_mode_evidence(self) -> None:
        log_text = "\n".join(
            (
                launch_runtime_validation.AUDIO_DIRECT_PATH_MARKERS[0],
                launch_runtime_validation.AUDIO_INDIRECT_PATH_MARKERS[0],
                "UERayTracingAudio validation result: sources=4 direct_batch_sources=4 indirect_batch_sources=4 visibility=0.0000 valid_paths=12 indirect_gain=0.250000",
                "UERayTracingAudio validation CPU reference: hardware_paths=12 cpu_paths=12 path_relative_delta=0.0000 hardware_gain=0.250000 cpu_gain=0.250000 gain_relative_delta=0.0000.",
                "UERayTracingAudio validation primary input: real_soundwave=1 asset=\"/Game/FirstPerson/Audio/MarchingBand.MarchingBand\".",
                "UERayTracingAudio validation audio pipeline: proxy_parses=137 proxy_waves=137 proxy_audiolink_overrides=0 max_actual_volume=0.400000 last_volume=0.400000 last_multiplier=0.400000 last_distance=1.000000 last_occlusion=1.000000 generator_callbacks=0 generator_non_silent=0 pre_distance_buffers=61 pre_distance_non_silent=61.",
                make_data_source_summary(),
            )
        )
        with redirect_stdout(io.StringIO()):
            launch_runtime_validation.print_audio_path_summary(
                log_text,
                "Game",
                require_validation=True,
                require_data_sources=True,
            )

    def test_runtime_data_source_summary_rejects_silent_hybrid(self) -> None:
        log_text = make_data_source_summary(hybrid_non_silent=0)
        with redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "hybrid non-silent wet audio"):
                launch_runtime_validation.print_audio_path_summary(
                    log_text,
                    "Game",
                    require_data_sources=True,
                )

    def test_runtime_data_source_summary_rejects_low_integrated_wet_ratio(self) -> None:
        log_text = make_data_source_summary(
            realtime_integrated_wet_input_rms_ratio=0.001
        )
        with redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "integrated wet/input RMS"):
                launch_runtime_validation.print_audio_path_summary(
                    log_text,
                    "Game",
                    require_data_sources=True,
                )

    def test_runtime_data_source_summary_rejects_clipping_full_peak(self) -> None:
        log_text = make_data_source_summary(realtime_full_peak=1.1)
        with redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "Full peak without clipping"):
                launch_runtime_validation.print_audio_path_summary(
                    log_text,
                    "Game",
                    require_data_sources=True,
                )

    def test_runtime_data_source_summary_rejects_over_unit_full_samples(self) -> None:
        log_text = make_data_source_summary(realtime_over_unit=1)
        with redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "over-unit Full samples"):
                launch_runtime_validation.print_audio_path_summary(
                    log_text,
                    "Game",
                    require_data_sources=True,
                )

    def test_runtime_data_source_summary_rejects_transient_only_wet(self) -> None:
        log_text = make_data_source_summary(
            realtime_wet_present=1,
            realtime_max_silent_run=29,
            realtime_audible_wet=1,
            realtime_max_inaudible_run=29,
        )
        with redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "sustained wet presence"):
                launch_runtime_validation.print_audio_path_summary(
                    log_text,
                    "Game",
                    require_data_sources=True,
                )

    def test_runtime_data_source_summary_rejects_excessive_silent_run(self) -> None:
        log_text = make_data_source_summary(
            realtime_wet_present=27,
            realtime_max_silent_run=7,
        )
        with redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "maximum silent wet run"):
                launch_runtime_validation.print_audio_path_summary(
                    log_text,
                    "Game",
                    require_data_sources=True,
                )

    def test_runtime_data_source_summary_rejects_callback_capacity_miss(
        self,
    ) -> None:
        log_text = make_data_source_summary(
            hard_realtime_passed=0,
            hard_realtime_capacity_misses=1,
        )
        with redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(
                RuntimeError,
                "zero callback capacity misses",
            ):
                launch_runtime_validation.print_audio_path_summary(
                    log_text,
                    "Game",
                    require_data_sources=True,
                )

    def test_runtime_data_source_summary_rejects_prepare_drop(
        self,
    ) -> None:
        log_text = make_data_source_summary(
            hard_realtime_passed=0,
            convolution_prepare_drops=1,
        )
        with redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(
                RuntimeError,
                "zero convolution prepare drops",
            ):
                launch_runtime_validation.print_audio_path_summary(
                    log_text,
                    "Game",
                    require_data_sources=True,
                )

    def test_runtime_data_source_summary_does_not_gate_per_buffer_ratio_count(
        self,
    ) -> None:
        log_text = "\n".join(
            (
                "UERayTracingAudio validation primary input: "
                'real_soundwave=1 asset="/Game/FirstPerson/Audio/MarchingBand.MarchingBand".',
                "UERayTracingAudio validation audio pipeline: "
                "max_actual_volume=0.400000 pre_distance_buffers=30 "
                "pre_distance_non_silent=30.",
                make_data_source_summary(
                    realtime_audible_wet=1,
                    realtime_max_inaudible_run=29,
                ),
            )
        )
        with redirect_stdout(io.StringIO()):
            launch_runtime_validation.print_audio_path_summary(
                log_text,
                "Game",
                require_data_sources=True,
            )

    def test_runtime_data_source_summary_rejects_background_muted_pipeline(self) -> None:
        log_text = "\n".join(
            (
                "UERayTracingAudio validation primary input: real_soundwave=1 asset=\"/Game/FirstPerson/Audio/MarchingBand.MarchingBand\".",
                "UERayTracingAudio validation audio pipeline: proxy_parses=900 proxy_waves=899 proxy_audiolink_overrides=0 max_actual_volume=0.000000 last_volume=0.400000 last_multiplier=0.400000 last_distance=1.000000 last_occlusion=1.000000 generator_callbacks=0 generator_non_silent=0 pre_distance_buffers=376 pre_distance_non_silent=0.",
                make_data_source_summary(),
            )
        )
        with redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "pre-distance"):
                launch_runtime_validation.print_audio_path_summary(
                    log_text,
                    "Game",
                    require_data_sources=True,
                )

    def test_interactive_smoke_summary_requires_movement_modes_and_views(self) -> None:
        log_text = make_interactive_smoke_summary()
        with redirect_stdout(io.StringIO()):
            launch_runtime_validation.print_audio_path_summary(
                log_text,
                "Game",
                require_interactive_smoke=True,
            )

    def test_interactive_smoke_summary_rejects_stationary_pawn(self) -> None:
        log_text = make_interactive_smoke_summary(
            passed=0,
            moved_cm=0.0,
        )
        with redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "Pawn movement"):
                launch_runtime_validation.print_audio_path_summary(
                    log_text,
                    "Game",
                    require_interactive_smoke=True,
                )

    def test_interactive_smoke_summary_rejects_foreign_audio(self) -> None:
        log_text = make_interactive_smoke_summary(
            foreign_audio_playing=1,
            muted_foreign_audio=0,
        )
        with redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "foreign audio"):
                launch_runtime_validation.print_audio_path_summary(
                    log_text,
                    "Game",
                    require_interactive_smoke=True,
                )

    def test_interactive_smoke_summary_rejects_ab_playback_restart(self) -> None:
        log_text = make_interactive_smoke_summary(ab_restart_count=1)
        with redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "A/B playback restarts"):
                launch_runtime_validation.print_audio_path_summary(
                    log_text,
                    "Game",
                    require_interactive_smoke=True,
                )

    def test_acoustic_summary_rejects_unbatched_multi_source_result(self) -> None:
        log_text = "\n".join(
            (
                launch_runtime_validation.AUDIO_DIRECT_PATH_MARKERS[0],
                launch_runtime_validation.AUDIO_INDIRECT_PATH_MARKERS[0],
                "UERayTracingAudio validation result: sources=4 direct_batch_sources=1 indirect_batch_sources=1 visibility=0.0000 valid_paths=12 indirect_gain=0.250000",
                "UERayTracingAudio validation CPU reference: hardware_paths=12 cpu_paths=12 path_relative_delta=0.0000 hardware_gain=0.250000 cpu_gain=0.250000 gain_relative_delta=0.0000.",
            )
        )
        with redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "direct RHI batch"):
                launch_runtime_validation.print_audio_path_summary(
                    log_text,
                    "Game",
                    require_validation=True,
                )

    def test_acoustic_summary_rejects_cpu_reference_outside_tolerance(self) -> None:
        log_text = "\n".join(
            (
                launch_runtime_validation.AUDIO_DIRECT_PATH_MARKERS[0],
                launch_runtime_validation.AUDIO_INDIRECT_PATH_MARKERS[0],
                "UERayTracingAudio validation result: sources=4 direct_batch_sources=4 indirect_batch_sources=4 visibility=0.0000 valid_paths=12 indirect_gain=0.250000",
                "UERayTracingAudio validation CPU reference: hardware_paths=12 cpu_paths=8 path_relative_delta=0.3333 hardware_gain=0.250000 cpu_gain=0.125000 gain_relative_delta=0.5000.",
            )
        )
        with redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "CPU reference path tolerance"):
                launch_runtime_validation.print_audio_path_summary(
                    log_text,
                    "Game",
                    require_validation=True,
                )

    def test_bake_repeatability_summary_accepts_hardware_result(self) -> None:
        log_text = "\n".join(
            (
                launch_runtime_validation.AUDIO_DIRECT_PATH_MARKERS[0],
                launch_runtime_validation.AUDIO_INDIRECT_PATH_MARKERS[0],
                "UERayTracingAudio validation result: sources=4 direct_batch_sources=4 indirect_batch_sources=4 visibility=0.0000 valid_paths=12 indirect_gain=0.250000",
                "UERayTracingAudio validation CPU reference: hardware_paths=12 cpu_paths=12 path_relative_delta=0.0000 hardware_gain=0.250000 cpu_gain=0.250000 gain_relative_delta=0.0000.",
                "UERayTracingAudio validation bake repeatability: passed=1 samples=2000 duration=0.250000 first_energy=1.2e-3 second_energy=1.2e-3 energy_relative_delta=0.000000 sample_relative_rms=0.000000 tolerance=0.050.",
            )
        )
        with redirect_stdout(io.StringIO()):
            launch_runtime_validation.print_audio_path_summary(
                log_text,
                "Game",
                require_validation=True,
                require_bake_repeatability=True,
            )

    def test_bake_repeatability_summary_rejects_sample_delta(self) -> None:
        log_text = "\n".join(
            (
                launch_runtime_validation.AUDIO_DIRECT_PATH_MARKERS[0],
                launch_runtime_validation.AUDIO_INDIRECT_PATH_MARKERS[0],
                "UERayTracingAudio validation result: sources=4 direct_batch_sources=4 indirect_batch_sources=4 visibility=0.0000 valid_paths=12 indirect_gain=0.250000",
                "UERayTracingAudio validation CPU reference: hardware_paths=12 cpu_paths=12 path_relative_delta=0.0000 hardware_gain=0.250000 cpu_gain=0.250000 gain_relative_delta=0.0000.",
                "UERayTracingAudio validation bake repeatability: passed=1 samples=2000 duration=0.250000 first_energy=1.2e-3 second_energy=1.1e-3 energy_relative_delta=0.040000 sample_relative_rms=0.200000 tolerance=0.050.",
            )
        )
        with redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "bake sample tolerance"):
                launch_runtime_validation.print_audio_path_summary(
                    log_text,
                    "Game",
                    require_validation=True,
                    require_bake_repeatability=True,
                )

    def test_runtime_failure_scan_rejects_fatal_log(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            project_path = Path(directory) / "TestProject.uproject"
            logs_root = project_path.parent / "Saved" / "Logs"
            logs_root.mkdir(parents=True)
            project_path.touch()
            before_logs = launch_runtime_validation.snapshot_files(logs_root)
            before_crashes = launch_runtime_validation.snapshot_files(project_path.parent / "Saved" / "Crashes")
            (logs_root / "TestProject.log").write_text("Fatal error: test sentinel", encoding="utf-8")

            with self.assertRaisesRegex(RuntimeError, "Fatal error"):
                launch_runtime_validation.assert_runtime_log_evidence(
                    project_path,
                    before_logs,
                    before_crashes,
                    "Test",
                )

    def test_runtime_log_evidence_requires_module_markers(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            project_path = Path(directory) / "TestProject.uproject"
            logs_root = project_path.parent / "Saved" / "Logs"
            logs_root.mkdir(parents=True)
            project_path.touch()
            before_logs = launch_runtime_validation.snapshot_files(logs_root)
            before_crashes = launch_runtime_validation.snapshot_files(project_path.parent / "Saved" / "Crashes")
            (logs_root / "TestProject.log").write_text("ordinary startup log", encoding="utf-8")

            with self.assertRaisesRegex(RuntimeError, "missing module markers"):
                launch_runtime_validation.assert_runtime_log_evidence(
                    project_path,
                    before_logs,
                    before_crashes,
                    "Test",
                    ("required marker",),
                )

    def test_runtime_log_evidence_accepts_required_markers(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            project_path = Path(directory) / "TestProject.uproject"
            logs_root = project_path.parent / "Saved" / "Logs"
            logs_root.mkdir(parents=True)
            project_path.touch()
            before_logs = launch_runtime_validation.snapshot_files(logs_root)
            before_crashes = launch_runtime_validation.snapshot_files(project_path.parent / "Saved" / "Crashes")
            (logs_root / "TestProject.log").write_text("required marker", encoding="utf-8")

            log_text = launch_runtime_validation.assert_runtime_log_evidence(
                project_path,
                before_logs,
                before_crashes,
                "Test",
                ("required marker",),
            )
            self.assertIn("required marker", log_text)

    def test_editor_ready_wait_uses_phase_log_content(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            project_path = Path(directory) / "TestProject.uproject"
            logs_root = project_path.parent / "Saved" / "Logs"
            logs_root.mkdir(parents=True)
            project_path.touch()
            phase_log_path = logs_root / "validation-editor.log"
            phase_log_path.write_text(
                launch_runtime_validation.EDITOR_READY_PATTERN,
                encoding="utf-8",
            )

            launch_runtime_validation.wait_for_editor_ready(
                FakeRunningProcess(),
                phase_log_path,
                timeout_seconds=0.5,
            )

    def test_editor_ready_wait_requires_ab_scene_after_engine_init(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            phase_log_path = Path(directory) / "validation-editor.log"
            phase_log_path.write_text(
                launch_runtime_validation.EDITOR_READY_PATTERN,
                encoding="utf-8",
            )

            with self.assertRaisesRegex(RuntimeError, "validation scene ready"):
                launch_runtime_validation.wait_for_editor_ready(
                    FakeRunningProcess(),
                    phase_log_path,
                    timeout_seconds=0.05,
                    required_markers=(
                        launch_runtime_validation.EDITOR_VISIBLE_SCENE_MARKER,
                    ),
                )

            phase_log_path.write_text(
                "\n".join(
                    (
                        launch_runtime_validation.EDITOR_READY_PATTERN,
                        launch_runtime_validation.EDITOR_VISIBLE_SCENE_MARKER,
                    )
                ),
                encoding="utf-8",
            )
            launch_runtime_validation.wait_for_editor_ready(
                FakeRunningProcess(),
                phase_log_path,
                timeout_seconds=0.5,
                required_markers=(
                    launch_runtime_validation.EDITOR_VISIBLE_SCENE_MARKER,
                ),
            )

    def test_editor_ready_wait_fails_fast_on_ab_artifact_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            phase_log_path = Path(directory) / "validation-editor.log"
            phase_log_path.write_text(
                launch_runtime_validation.EDITOR_AB_ARTIFACTS_FAILURE_MARKER
                + " test sentinel",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(RuntimeError, "test sentinel"):
                launch_runtime_validation.wait_for_editor_ready(
                    FakeRunningProcess(),
                    phase_log_path,
                    timeout_seconds=0.5,
                    required_markers=(
                        launch_runtime_validation.EDITOR_AB_ARTIFACTS_MARKER,
                    ),
                )

    def test_phase_log_does_not_use_unrelated_rotated_log(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            project_path = Path(directory) / "TestProject.uproject"
            logs_root = project_path.parent / "Saved" / "Logs"
            logs_root.mkdir(parents=True)
            project_path.touch()
            before_logs = launch_runtime_validation.snapshot_files(logs_root)
            before_crashes = launch_runtime_validation.snapshot_files(project_path.parent / "Saved" / "Crashes")
            (logs_root / "OldGame-backup.log").write_text(
                "UERayTracingAudioEditor module initialized.",
                encoding="utf-8",
            )
            phase_log_path = logs_root / "validation-editor.log"
            phase_log_path.write_text("editor started", encoding="utf-8")

            with self.assertRaisesRegex(RuntimeError, "missing module markers"):
                launch_runtime_validation.assert_runtime_log_evidence(
                    project_path,
                    before_logs,
                    before_crashes,
                    "Editor",
                    ("UERayTracingAudioEditor module initialized.",),
                    phase_log_path,
                )


if __name__ == "__main__":
    unittest.main()

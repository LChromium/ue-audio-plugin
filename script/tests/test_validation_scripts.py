from __future__ import annotations

import json
import io
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch


SCRIPT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPT_ROOT))

import launch_runtime_validation
import validation_environment
import validate_visible_editor_ab_scene


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
        + "\nUERayTracingAudio validation data-source bake started "
        "during audible playback: rays=1024 bounces=4 duration=0.250 "
        "sample_rate=8000."
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
    f3_source_preserved: int = 1,
) -> str:
    return (
        f"UERayTracingAudio interactive smoke: passed={passed} "
        f"moved_cm={moved_cm:.3f} "
        "listener_camera_error_cm=0.000 origin_error_cm=0.000 "
        "realtime=1 baked=1 hybrid=1 rendered_ab=1 reference_ab=1 "
        "fixed_view=1 interactive_view=1 audio_playing=1 reference_playing=1 "
        "ab_base_levels_matched=1 "
        f"ab_restart_count={ab_restart_count} "
        f"f3_source_preserved={f3_source_preserved} "
        f"foreign_audio_playing={foreign_audio_playing} "
        f"muted_foreign_audio={muted_foreign_audio}."
    )


def make_editor_ab_artifact_marker(
    *,
    direct_preset: str = "clear",
    reflection_environment: str = "enclosed",
    reflection_bounces: int = 32,
) -> str:
    return (
        "UERayTracingAudioEditor A/B artifacts ready: "
        "hardware=1 auto_checks=1 distinct=1 "
        'input="/Game/FirstPerson/Audio/MarchingBand.MarchingBand" '
        f'direct_preset="{direct_preset}" '
        f'reflection_environment="{reflection_environment}" '
        "distance_cm=200.000 visibility=1.000000 occlusion=1.000000 "
        "distance_attenuation=0.500000 "
        'ir_asset="/Game/UERayTracingAudio/Validation/IR.IR" '
        "imported_assets=4 "
        'reference="C:/Artifacts/reference.wav" '
        'direct="C:/Artifacts/direct.wav" '
        'wet="C:/Artifacts/wet.wav" '
        'full="C:/Artifacts/full.wav" '
        'manifest="C:/Artifacts/manifest.json" '
        "reflection_rays=4096 "
        f"reflection_bounces={reflection_bounces} hw_paths=12 "
        "hw_gain=0.2 direct_level=0.5 wet_level=0.2 full_level=0.6 "
        "direct_wet_difference=0.4 wet_stereo_difference=0.2 "
        "directional_wet=1 common_scale=1.000000."
    )


def make_editor_scene_marker(
    *,
    geometry: int = 1,
    reflection_environment: str = "near_wall",
    reflection_bounces: int = 32,
) -> str:
    return (
        "UERayTracingAudioEditor validation scene ready: "
        f"source=1 listener=1 geometry={geometry} lighting=1 bake_ui=1 "
        "direct_preset=clear "
        f"reflection_environment={reflection_environment} "
        f"reflection_bounces={reflection_bounces} "
        "source_listener_distance_cm=200.00 "
        "air_absorption_profile=default "
        "air_absorption_per_meter=(0.000200,0.000600,0.001200)."
    )


def make_acoustic_validation_summary(
    direct_path_marker: str,
    indirect_path_marker: str,
) -> str:
    return "\n".join(
        (
            direct_path_marker,
            indirect_path_marker,
            "UERayTracingAudio validation result: sources=4 "
            "direct_batch_sources=4 indirect_batch_sources=4 "
            "visibility=0.0000 valid_paths=12 indirect_gain=0.250000",
            "UERayTracingAudio validation CPU reference: hardware_paths=12 "
            "cpu_paths=12 path_relative_delta=0.0000 hardware_gain=0.250000 "
            "cpu_gain=0.250000 gain_relative_delta=0.0000.",
        )
    )


class FakeRunningProcess:
    def poll(self) -> None:
        return None


class FakeVisibleEditorProcess:
    pid = 2468
    returncode = None

    def poll(self) -> None:
        return None


def run_visible_editor_helper(
    root: Path,
    *,
    log_text: str,
    timeout: float = 45.0,
    image_metrics: tuple[int, int, float, float, float] = (
        1280,
        720,
        0.75,
        48.0,
        12.0,
    ),
) -> tuple[int | None, BaseException | None, Path, str, list[str]]:
    project_path = root / "Project" / "Test.uproject"
    project_path.parent.mkdir(parents=True, exist_ok=True)
    screenshot_path = root / "editor.png"
    result_path = root / "result.json"
    process = FakeVisibleEditorProcess()

    def capture_metrics(
        _window_handle: int,
        output_path: Path,
    ) -> tuple[int, int, float, float, float]:
        output_path.write_bytes(b"synthetic screenshot")
        return image_metrics

    stdout = io.StringIO()
    stderr = io.StringIO()
    with patch.object(
        sys,
        "argv",
        [
            "validate_visible_editor_ab_scene.py",
            "--engine-root",
            str(root / "UE_5.7"),
            "--project",
            str(project_path),
            "--timeout",
            str(timeout),
            "--artifacts",
            "--direct-preset",
            "clear",
            "--reflection-environment",
            "near_wall",
            "--reflection-bounces",
            "32",
            "--screenshot",
            str(screenshot_path),
            "--result-json",
            str(result_path),
        ],
    ), patch.object(
        validate_visible_editor_ab_scene.validation_environment,
        "resolve_engine_root",
        return_value=root / "UE_5.7",
    ), patch.object(
        validate_visible_editor_ab_scene.validation_environment,
        "resolve_project_path",
        return_value=project_path,
    ), patch.object(
        validate_visible_editor_ab_scene,
        "visible_unreal_windows",
        side_effect=([], [(101, process.pid, "Unreal Editor")]),
    ), patch.object(
        validate_visible_editor_ab_scene,
        "client_bounds",
        return_value=(0, 0, 1280, 720),
    ), patch.object(
        validate_visible_editor_ab_scene,
        "capture_metrics",
        side_effect=capture_metrics,
    ), patch.object(
        validate_visible_editor_ab_scene,
        "read_log",
        return_value=log_text,
    ), patch.object(
        validate_visible_editor_ab_scene.subprocess,
        "Popen",
        return_value=process,
    ) as popen_mock, patch.object(
        validate_visible_editor_ab_scene,
        "stop_process",
    ), redirect_stdout(stdout), redirect_stderr(stderr):
        try:
            status = validate_visible_editor_ab_scene.main()
            error: BaseException | None = None
        except (Exception, SystemExit) as exc:
            status = None
            error = exc

    command = (
        list(popen_mock.call_args.args[0])
        if popen_mock.call_args is not None
        else []
    )
    return status, error, result_path, stdout.getvalue(), command


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
            "-UERayTracingAudioValidationReflectionEnvironment=enclosed",
            command,
        )
        self.assertIn(
            "-UERayTracingAudioValidationReflectionBounces=8",
            command,
        )
        self.assertIn(
            "-ini:Engine:[Audio]:UnfocusedVolumeMultiplier=1.0",
            command,
        )
        self.assertIn(
            launch_runtime_validation.EDITOR_VISIBLE_SCENE_MARKER,
            launch_runtime_validation.EDITOR_VALIDATION_MARKERS,
        )
        self.assertNotIn("-UERayTracingAudioValidationDirectSweep", command)

    def test_editor_command_configures_environment_and_clamps_bounces(
        self,
    ) -> None:
        command = launch_runtime_validation.build_editor_command(
            Path("UnrealEditor.exe"),
            Path("Test.uproject"),
            Path("Editor.log"),
            editor_reflection_environment="near_wall",
            editor_reflection_bounces=32,
        )
        self.assertIn(
            "-UERayTracingAudioValidationReflectionEnvironment=near_wall",
            command,
        )
        self.assertIn(
            "-UERayTracingAudioValidationReflectionBounces=32",
            command,
        )

        for requested, expected in ((0, 1), (65, 64)):
            with self.subTest(requested=requested):
                clamped = launch_runtime_validation.build_editor_command(
                    Path("UnrealEditor.exe"),
                    Path("Test.uproject"),
                    Path("Editor.log"),
                    editor_reflection_bounces=requested,
                )
                self.assertIn(
                    "-UERayTracingAudioValidationReflectionBounces="
                    f"{expected}",
                    clamped,
                )

        with self.assertRaisesRegex(
            ValueError,
            "Unknown Editor reflection environment",
        ):
            launch_runtime_validation.build_editor_command(
                Path("UnrealEditor.exe"),
                Path("Test.uproject"),
                Path("Editor.log"),
                editor_reflection_environment="warehouse",
            )

    def test_editor_cli_applies_distance_and_air_profile_only_to_editor(
        self,
    ) -> None:
        with patch.object(
            sys,
            "argv",
            [
                "launch_runtime_validation.py",
                "--editor-distance-cm",
                "400",
                "--editor-air-absorption-profile",
                "stress",
            ],
        ):
            args = launch_runtime_validation.parse_args()
        self.assertEqual(args.editor_distance_cm, 400)
        self.assertEqual(args.editor_air_absorption_profile, "stress")

        editor_command = launch_runtime_validation.build_editor_command(
            Path("UnrealEditor.exe"),
            Path("Test.uproject"),
            Path("Editor.log"),
            editor_distance_cm=400,
            editor_air_absorption_profile="stress",
        )
        self.assertIn(
            "-UERayTracingAudioValidationDistanceCm=400",
            editor_command,
        )
        self.assertIn(
            "-UERayTracingAudioValidationAirAbsorptionProfile=stress",
            editor_command,
        )

        game_command = launch_runtime_validation.build_game_command(
            Path("UnrealEditor.exe"),
            Path("Test.uproject"),
            Path("Game.log"),
        )
        self.assertFalse(
            any(
                argument.startswith(
                    "-UERayTracingAudioValidationDistanceCm="
                )
                for argument in game_command
            )
        )
        self.assertFalse(
            any(
                argument.startswith(
                    "-UERayTracingAudioValidationAirAbsorptionProfile="
                )
                for argument in game_command
            )
        )

    def test_editor_scene_ready_marker_requires_requested_fixture_fields(
        self,
    ) -> None:
        marker = (
            "LogTemp: Display: UERayTracingAudioEditor validation scene ready: "
            "source=1 listener=1 geometry=1 lighting=1 bake_ui=1 "
            "direct_preset=clear reflection_environment=near_wall "
            "reflection_bounces=32 source_listener_distance_cm=200.00 "
            "air_absorption_profile=default "
            "air_absorption_per_meter=(0.000200,0.000600,0.001200)."
        )
        values = launch_runtime_validation.validate_editor_scene_ready(
            marker,
            expected_direct_preset="clear",
            expected_reflection_environment="near_wall",
            expected_distance_cm=200,
            expected_air_absorption_profile="default",
            expected_reflection_bounces=32,
        )
        self.assertEqual(values["geometry"], 1)
        self.assertEqual(values["reflection_bounces"], 32)
        self.assertEqual(values["distance_cm"], 200.0)
        self.assertEqual(values["air_absorption_profile"], "default")
        self.assertEqual(
            values["air_absorption_per_meter"],
            (0.0002, 0.0006, 0.0012),
        )

        missing_bounce_marker = marker.replace(
            "reflection_bounces=32 ",
            "",
        )
        with self.assertRaisesRegex(
            RuntimeError,
            "strict Editor validation scene marker",
        ):
            launch_runtime_validation.validate_editor_scene_ready(
                missing_bounce_marker,
                expected_direct_preset="clear",
                expected_reflection_environment="near_wall",
                expected_distance_cm=200,
                expected_air_absorption_profile="default",
                expected_reflection_bounces=32,
            )

        wrong_vector_marker = marker.replace(
            "(0.000200,0.000600,0.001200)",
            "(0.010000,0.040000,0.120000)",
        )
        with self.assertRaisesRegex(
            RuntimeError,
            "air absorption vector",
        ):
            launch_runtime_validation.validate_editor_scene_ready(
                wrong_vector_marker,
                expected_direct_preset="clear",
                expected_reflection_environment="near_wall",
                expected_distance_cm=200,
                expected_air_absorption_profile="default",
                expected_reflection_bounces=32,
            )

        with self.assertRaisesRegex(
            RuntimeError,
            "direct preset",
        ):
            launch_runtime_validation.validate_editor_scene_ready(
                marker,
                expected_direct_preset="hard_occluded",
                expected_reflection_environment="near_wall",
                expected_distance_cm=200,
                expected_air_absorption_profile="default",
                expected_reflection_bounces=32,
            )

        for rejected, reason in (
            (
                marker.replace("geometry=1", "geometry=7"),
                "fixture geometry count",
            ),
            (
                marker.replace("reflection_bounces=32", "reflection_bounces=8"),
                "reflection bounces",
            ),
        ):
            with self.subTest(reason=reason):
                with self.assertRaisesRegex(RuntimeError, reason):
                    launch_runtime_validation.validate_editor_scene_ready(
                        rejected,
                        expected_direct_preset="clear",
                        expected_reflection_environment="near_wall",
                        expected_distance_cm=200,
                        expected_air_absorption_profile="default",
                        expected_reflection_bounces=32,
                    )

        duplicate_marker = marker + "\n" + marker.replace(
            "air_absorption_profile=default ",
            "air_absorption_profile=malformed ",
        )
        with self.assertRaisesRegex(
            RuntimeError,
            "exactly one strict Editor validation scene marker",
        ):
            launch_runtime_validation.validate_editor_scene_ready(
                duplicate_marker,
                expected_direct_preset="clear",
                expected_reflection_environment="near_wall",
                expected_distance_cm=200,
                expected_air_absorption_profile="default",
                expected_reflection_bounces=32,
            )

    def test_editor_ab_artifact_marker_matches_producer_order_and_environment(
        self,
    ) -> None:
        marker = make_editor_ab_artifact_marker(
            reflection_environment="near_wall",
        )
        values = launch_runtime_validation.validate_editor_ab_artifacts_marker(
            marker,
            expected_direct_preset="clear",
            expected_reflection_environment="near_wall",
            expected_reflection_bounces=32,
        )
        self.assertEqual(values["direct_preset"], "clear")
        self.assertEqual(values["reflection_environment"], "near_wall")
        self.assertEqual(values["distance_cm"], 200.0)
        self.assertEqual(values["reflection_rays"], 4096)
        self.assertEqual(values["reflection_bounces"], 32)
        self.assertEqual(values["common_scale"], 1.0)

    def test_editor_ready_waits_for_complete_ab_artifact_marker(
        self,
    ) -> None:
        complete_marker = make_editor_ab_artifact_marker()
        partial_marker = complete_marker.rsplit(" common_scale=", 1)[0]
        partial_log = (
            launch_runtime_validation.EDITOR_READY_PATTERN
            + "\n"
            + partial_marker
        )
        complete_log = (
            launch_runtime_validation.EDITOR_READY_PATTERN
            + "\n"
            + complete_marker
        )
        with patch.object(
            launch_runtime_validation,
            "read_text",
            side_effect=(partial_log, complete_log),
        ) as read_text_mock, patch.object(
            launch_runtime_validation.time,
            "sleep",
        ):
            launch_runtime_validation.wait_for_editor_ready(
                FakeRunningProcess(),
                Path("Editor.log"),
                timeout_seconds=1.0,
                required_markers=(
                    launch_runtime_validation.
                    EDITOR_AB_ARTIFACTS_MARKER,
                ),
            )
        self.assertEqual(read_text_mock.call_count, 2)

    def test_editor_ab_artifact_marker_rejects_duplicate_malformed_and_wrong_environment(
        self,
    ) -> None:
        marker = make_editor_ab_artifact_marker()
        for rejected, reason in (
            (marker + "\n" + marker, "exactly one strict"),
            (marker.replace("distance_cm=200.000", "distance_cm=bad"), "strict"),
        ):
            with self.subTest(reason=reason):
                with self.assertRaisesRegex(RuntimeError, reason):
                    launch_runtime_validation.validate_editor_ab_artifacts_marker(
                        rejected,
                        expected_direct_preset="clear",
                        expected_reflection_environment="enclosed",
                        expected_reflection_bounces=32,
                    )
        with self.assertRaisesRegex(RuntimeError, "reflection environment"):
            launch_runtime_validation.validate_editor_ab_artifacts_marker(
                marker,
                expected_direct_preset="clear",
                expected_reflection_environment="open_space",
                expected_reflection_bounces=32,
            )

        with self.assertRaisesRegex(RuntimeError, "reflection bounces"):
            launch_runtime_validation.validate_editor_ab_artifacts_marker(
                make_editor_ab_artifact_marker(reflection_bounces=8),
                expected_direct_preset="clear",
                expected_reflection_environment="enclosed",
                expected_reflection_bounces=32,
            )

    def test_visible_editor_helper_accepts_environment_and_result_options(
        self,
    ) -> None:
        with patch.object(
            sys,
            "argv",
            [
                "validate_visible_editor_ab_scene.py",
                "--reflection-environment",
                "near_wall",
                "--result-json",
                "Saved/near-wall.json",
            ],
        ):
            args = validate_visible_editor_ab_scene.parse_args()

        self.assertEqual(args.reflection_environment, "near_wall")
        self.assertEqual(args.result_json, Path("Saved/near-wall.json"))

    def test_visible_editor_helper_writes_strict_atomic_result_json(
        self,
    ) -> None:
        scene_marker = make_editor_scene_marker()
        artifact_marker = make_editor_ab_artifact_marker(
            reflection_environment="near_wall",
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            status, error, result_path, output, command = (
                run_visible_editor_helper(
                    root,
                    log_text=scene_marker + "\n" + artifact_marker,
                )
            )

            self.assertEqual(status, 0)
            self.assertIsNone(error)
            self.assertTrue(result_path.is_file())
            self.assertFalse(
                result_path.with_suffix(result_path.suffix + ".tmp").exists()
            )
            payload = json.loads(result_path.read_text(encoding="utf-8"))
            self.assertEqual(payload["schema_version"], 1)
            self.assertTrue(payload["passed"])
            self.assertEqual(
                payload["scene"]["reflection_environment"],
                "near_wall",
            )
            self.assertEqual(payload["scene"]["geometry"], 1)
            self.assertEqual(payload["scene"]["reflection_bounces"], 32)
            self.assertEqual(payload["artifacts"]["reflection_rays"], 4096)
            self.assertEqual(payload["artifacts"]["reflection_bounces"], 32)
            self.assertEqual(
                payload["image_metrics"],
                {
                    "width": 1280,
                    "height": 720,
                    "non_black_ratio": 0.75,
                    "mean_luma": 48.0,
                    "luma_stddev": 12.0,
                },
            )
            self.assertEqual(payload["screenshot"], str(root / "editor.png"))
            self.assertIn("UERayTracingAudioEditorVisible-", payload["log"])
            self.assertEqual(output.count(scene_marker), 1)
            self.assertEqual(output.count(artifact_marker), 1)
            self.assertIn(
                "-UERayTracingAudioValidationReflectionEnvironment=near_wall",
                command,
            )
            self.assertIn(
                "-UERayTracingAudioValidationReflectionBounces=32",
                command,
            )

    def test_visible_editor_helper_never_writes_failed_result_json(self) -> None:
        scene_marker = make_editor_scene_marker()
        artifact_marker = make_editor_ab_artifact_marker(
            reflection_environment="near_wall",
        )
        cases = (
            (
                "timeout",
                scene_marker + "\n" + artifact_marker,
                0.0,
                (1280, 720, 0.75, 48.0, 12.0),
                1,
                None,
            ),
            (
                "artifact failure",
                "UERayTracingAudioEditor A/B artifacts failed: synthetic",
                45.0,
                (1280, 720, 0.75, 48.0, 12.0),
                1,
                None,
            ),
            (
                "malformed scene",
                scene_marker.replace("reflection_bounces=32 ", "")
                + "\n"
                + artifact_marker,
                45.0,
                (1280, 720, 0.75, 48.0, 12.0),
                None,
                "strict Editor validation scene marker",
            ),
            (
                "black frame",
                scene_marker + "\n" + artifact_marker,
                45.0,
                (1280, 720, 0.75, 0.0, 0.0),
                1,
                None,
            ),
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for index, (
                label,
                log_text,
                timeout,
                metrics,
                expected_status,
                expected_error,
            ) in enumerate(cases):
                with self.subTest(label=label):
                    case_root = root / str(index)
                    case_root.mkdir(parents=True, exist_ok=True)
                    stale_result_path = case_root / "result.json"
                    stale_temporary_path = stale_result_path.with_suffix(
                        stale_result_path.suffix + ".tmp"
                    )
                    stale_result_path.write_text(
                        '{"passed": true}\n',
                        encoding="utf-8",
                    )
                    stale_temporary_path.write_text(
                        '{"passed": true}\n',
                        encoding="utf-8",
                    )
                    status, error, result_path, _, _ = run_visible_editor_helper(
                        case_root,
                        log_text=log_text,
                        timeout=timeout,
                        image_metrics=metrics,
                    )
                    self.assertEqual(status, expected_status)
                    if expected_error is None:
                        self.assertIsNone(error)
                    else:
                        self.assertIsInstance(error, RuntimeError)
                        self.assertRegex(str(error), expected_error)
                    self.assertFalse(result_path.exists())
                    self.assertFalse(stale_temporary_path.exists())

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

    def test_direct_sweep_gate_rejects_invalid_visibility_ranges(self) -> None:
        cases = (
            (
                "negative minimum",
                {"visibility_min": "-0.000001"},
            ),
            (
                "maximum above one",
                {"visibility_max": "1.000001"},
            ),
            (
                "inverted endpoints",
                {
                    "visibility_min": "0.950000",
                    "visibility_max": "0.050000",
                },
            ),
        )
        for name, overrides in cases:
            with self.subTest(name=name):
                with self.assertRaisesRegex(
                    RuntimeError,
                    "ordered normalized visibility range",
                ):
                    self.validate_direct_sweep(
                        make_direct_sweep_summary(**overrides)
                    )

    def test_direct_sweep_gate_rejects_invalid_gain_ranges(self) -> None:
        cases = (
            (
                "negative minimum",
                {
                    "gain_min": "-0.200000",
                    "gain_max": "0.500000",
                },
            ),
            (
                "negative maximum",
                {
                    "gain_min": "-0.200000",
                    "gain_max": "-0.100000",
                },
            ),
            (
                "inverted endpoints",
                {
                    "gain_min": "0.800000",
                    "gain_max": "0.700000",
                },
            ),
            (
                "minimum above one",
                {
                    "gain_min": "1.100000",
                    "gain_max": "1.200000",
                },
            ),
            (
                "maximum above one",
                {"gain_max": "1.000001"},
            ),
        )
        for name, overrides in cases:
            with self.subTest(name=name):
                with self.assertRaisesRegex(
                    RuntimeError,
                    "ordered normalized Direct gain range",
                ):
                    self.validate_direct_sweep(
                        make_direct_sweep_summary(**overrides)
                    )

    def test_direct_sweep_gate_rejects_negative_gain_step(self) -> None:
        with self.assertRaisesRegex(
            RuntimeError,
            "non-negative bounded per-sample gain step",
        ):
            self.validate_direct_sweep(
                make_direct_sweep_summary(max_gain_step="-0.000001")
            )

    def test_direct_sweep_gate_requires_terminal_before_runtime_ir_evidence(
        self,
    ) -> None:
        later_markers = (
            (
                "data-source Bake",
                "UERayTracingAudio validation data-source bake started",
            ),
            (
                "hard-real-time result",
                "UERayTracingAudio hard realtime:",
            ),
            (
                "final data-source result",
                "UERayTracingAudio validation data sources:",
            ),
        )
        direct_marker = make_direct_sweep_summary()
        for name, marker in later_markers:
            with self.subTest(name=name):
                with self.assertRaisesRegex(
                    RuntimeError,
                    f"Direct sweep terminal before {name}",
                ):
                    self.validate_direct_sweep(
                        marker + "\n" + direct_marker
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

    def test_acoustic_summary_requires_actual_hardware_submission_markers(
        self,
    ) -> None:
        fallback_only = make_acoustic_validation_summary(
            "falls back to physics line traces for direct sound visibility queries",
            "falls back to CPU acoustic scene queries for indirect sound path tracing",
        )
        with redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "hardware submission"):
                launch_runtime_validation.print_audio_path_summary(
                    fallback_only,
                    "Game",
                    require_validation=True,
                )

        hardware_with_reference_fallback = make_acoustic_validation_summary(
            "submits direct sound visibility queries asynchronously to the render thread\n"
            "falls back to physics line traces for direct sound visibility queries",
            "submits indirect sound energy-field queries asynchronously to the render thread\n"
            "falls back to CPU acoustic scene queries for indirect sound path tracing",
        )
        with redirect_stdout(io.StringIO()):
            launch_runtime_validation.print_audio_path_summary(
                hardware_with_reference_fallback,
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

    def test_runtime_data_source_summary_requires_unique_strict_terminal_markers(
        self,
    ) -> None:
        base = make_data_source_summary()
        hard_line = next(
            line
            for line in base.splitlines()
            if launch_runtime_validation.HARD_REALTIME_MARKER in line
        )
        data_line = next(
            line
            for line in base.splitlines()
            if launch_runtime_validation.DATA_SOURCE_VALIDATION_MARKER in line
        )
        for rejected, reason in (
            (base + "\n" + hard_line, "exactly one strict hard-real-time"),
            (base + "\n" + data_line, "exactly one strict data-source"),
            (
                base.replace("callbacks=300", "callbacks=malformed"),
                "strict hard-real-time",
            ),
            (
                base.replace("baked_buffers=30", "baked_buffers=malformed"),
                "strict data-source",
            ),
        ):
            with self.subTest(reason=reason):
                with redirect_stdout(io.StringIO()):
                    with self.assertRaisesRegex(RuntimeError, reason):
                        launch_runtime_validation.print_audio_path_summary(
                            rejected,
                            "Game",
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

    def test_interactive_smoke_summary_rejects_f3_source_reset(self) -> None:
        log_text = make_interactive_smoke_summary(f3_source_preserved=0)
        with redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "F3 data source preservation"):
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

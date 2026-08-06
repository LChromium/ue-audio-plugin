from __future__ import annotations

import sys
import inspect
import tempfile
import unittest
from pathlib import Path


SCRIPT_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = SCRIPT_ROOT.parent
sys.path.insert(0, str(SCRIPT_ROOT))

import validate_audio_realtime_safety


class AudioRealtimeSafetyAuditTests(unittest.TestCase):
    def test_audits_direct_target_generation_helpers(self) -> None:
        audited_names = {
            spec.qualified_name
            for spec in validate_audio_realtime_safety.AUDIO_CALLBACK_SPECS
        }
        self.assertIn(
            "FUERayTracingAudioAudioDiagnostics::IsEnabledFor",
            audited_names,
        )
        self.assertIn(
            "FUERayTracingAudioAudioDiagnosticsInternal::CaptureTarget",
            audited_names,
        )
        self.assertIn(
            "FUERayTracingAudioAudioDiagnosticsInternal::RecordDirectBuffer",
            audited_names,
        )

    def test_current_audio_callback_chain_has_no_forbidden_operations(self) -> None:
        report = validate_audio_realtime_safety.audit_repo(REPO_ROOT)
        self.assertEqual(report.violations, ())
        self.assertGreaterEqual(report.audited_functions, 30)
        self.assertGreaterEqual(report.audited_bodies, report.audited_functions)
        self.assertGreater(report.audited_lines, 500)

    def test_async_rhi_readback_uses_one_way_weak_publication(self) -> None:
        report = validate_audio_realtime_safety.audit_repo(REPO_ROOT)
        rhi_violations = tuple(
            violation
            for violation in report.violations
            if "UERayTracingAudioRayTracingDevice.cpp"
            in violation.relative_path
        )
        self.assertEqual(rhi_violations, ())

    def test_detects_lock_heap_shared_ownership_blocking_and_uobject_access(
        self,
    ) -> None:
        body = """
            FScopeLock Lock(&Mutex);
            Values.Add(1.0f);
            TSharedPtr<FThing> Thing;
            FlushRenderingCommands();
            GetWorld();
            UE_LOG(LogTemp, Display, TEXT("audio callback"));
        """
        violations = validate_audio_realtime_safety.audit_body(
            "Synthetic.cpp",
            "FSynthetic::ProcessAudio",
            body,
            allow_bounded_resize=False,
        )
        self.assertEqual(
            {violation.category for violation in violations},
            {
                "lock",
                "heap",
                "shared-ownership",
                "blocking",
                "uobject",
                "logging",
            },
        )

    def test_audits_same_file_helpers_called_from_callback_entries(self) -> None:
        if "specs" not in inspect.signature(
            validate_audio_realtime_safety.audit_repo
        ).parameters:
            self.fail(
                "audit_repo must accept focused specs so transitive helper "
                "audit coverage can be tested without mirroring the whole repo"
            )

        with tempfile.TemporaryDirectory() as directory:
            repo_root = Path(directory)
            source_path = (
                repo_root
                / "Source"
                / "UERayTracingAudio"
                / "Private"
                / "Audio"
                / "Synthetic.cpp"
            )
            source_path.parent.mkdir(parents=True)
            source_path.write_text(
                """
                void UnsafeCallbackHelper()
                {
                    UE_LOG(LogTemp, Display, TEXT("transitive logging"));
                }

                void FExample::ProcessAudio()
                {
                    UnsafeCallbackHelper();
                }
                """,
                encoding="utf-8",
            )
            report = validate_audio_realtime_safety.audit_repo(
                repo_root,
                specs=(
                    validate_audio_realtime_safety.AuditSpec(
                        "Source/UERayTracingAudio/Private/Audio/Synthetic.cpp",
                        "FExample::ProcessAudio",
                    ),
                ),
            )

        self.assertEqual(
            {
                (violation.qualified_name, violation.category)
                for violation in report.violations
            },
            {("UnsafeCallbackHelper", "logging")},
        )

    def test_bounded_bridge_resize_requires_capacity_guard(self) -> None:
        safe_body = """
            if (NumFrames > MaxFramesPerCallback
                || NumFrames > State.StereoWet.Max())
            {
                return {};
            }
            State.StereoWet.SetNumUninitialized(
                NumFrames,
                EAllowShrinking::No);
        """
        self.assertEqual(
            validate_audio_realtime_safety.audit_body(
                "Synthetic.cpp",
                "FBridge::BeginWriteInternal",
                safe_body,
                allow_bounded_resize=True,
            ),
            (),
        )

        unsafe_body = """
            State.StereoWet.SetNumUninitialized(NumFrames);
        """
        violations = validate_audio_realtime_safety.audit_body(
            "Synthetic.cpp",
            "FBridge::BeginWriteInternal",
            unsafe_body,
            allow_bounded_resize=True,
        )
        self.assertIn(
            "bounded-resize-invariant",
            {violation.category for violation in violations},
        )
        self.assertIn(
            "heap",
            {violation.category for violation in violations},
        )

    def test_extracts_multiline_qualified_function_body(self) -> None:
        source = """
        void FExample::
            ProcessAudio(
                int Value)
        {
            if (Value > 0)
            {
                --Value;
            }
        }
        """
        bodies = validate_audio_realtime_safety.extract_function_bodies(
            source,
            "FExample::ProcessAudio",
        )
        self.assertEqual(len(bodies), 1)
        self.assertIn("--Value", bodies[0])


if __name__ == "__main__":
    unittest.main()

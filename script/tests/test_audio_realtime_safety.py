from __future__ import annotations

import sys
import unittest
from pathlib import Path


SCRIPT_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = SCRIPT_ROOT.parent
sys.path.insert(0, str(SCRIPT_ROOT))

import validate_audio_realtime_safety


class AudioRealtimeSafetyAuditTests(unittest.TestCase):
    def test_current_audio_callback_chain_has_no_forbidden_operations(self) -> None:
        report = validate_audio_realtime_safety.audit_repo(REPO_ROOT)
        self.assertEqual(report.violations, ())
        self.assertGreaterEqual(report.audited_functions, 30)
        self.assertGreaterEqual(report.audited_bodies, report.audited_functions)
        self.assertGreater(report.audited_lines, 500)

    def test_detects_lock_heap_shared_ownership_blocking_and_uobject_access(
        self,
    ) -> None:
        body = """
            FScopeLock Lock(&Mutex);
            Values.Add(1.0f);
            TSharedPtr<FThing> Thing;
            FlushRenderingCommands();
            GetWorld();
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
            },
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

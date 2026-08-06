from __future__ import annotations

import argparse
import sys
from pathlib import Path

from human_acceptance import HumanAcceptanceError, validate_human_acceptance_record


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate a UE Ray Tracing Audio HumanAcceptance record.",
    )
    parser.add_argument("--record", type=Path, required=True)
    parser.add_argument(
        "--require-pass",
        action="store_true",
        help="Also require PASS, automatic checks, and distinct modes.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        record = validate_human_acceptance_record(
            args.record,
            require_pass=args.require_pass,
        )
    except HumanAcceptanceError as exc:
        print(f"HUMAN_ACCEPTANCE_FAIL {exc}", file=sys.stderr)
        return 1

    verdict = "PASS" if record["human_listening_passed"] else "FAIL"
    confirmed = sum(record["human_confirmations"].values())
    print(
        "HUMAN_ACCEPTANCE_PASS "
        f"verdict={verdict} "
        f"confirmations={confirmed}/5 "
        f'device="{record["target_listening_device"]}" '
        f'record="{args.record.resolve()}" '
        f'manifest="{record["comparison_manifest"]}"'
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

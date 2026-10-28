#!/usr/bin/env python3
"""Fail-fast checks that the checked-out builder contains the NG3 extracted-mode adaptation."""
from pathlib import Path

p = Path("scripts/build_ipa_from_nsp.py")
s = p.read_text(encoding="utf-8")
required = (
    "0x0100B250133B0000 if (extracted_dir / 'RomFS/databin/databin').exists() else 0",
    "Validate game-specific RomFS marker after resolving the registered profile.",
    "BOTW-only fast path: Windows already scanned the full RomFS",
)
missing = [needle for needle in required if needle not in s]
if missing:
    raise SystemExit("NG3 builder adaptation check failed; missing expected code: " + repr(missing))
print("[OK] NG3 extracted-mode title ID adaptation is present.")
print("[NOTE] Prebuilt NVN path is shared by BOTW and NG3; full RomFS scan is skipped.")

#!/usr/bin/env python3
"""Apply the narrowly-scoped NG3 adaptation to the BOTW-origin build entry point.

The patch is intentionally guarded: if upstream changes the expected BOTW-only
validation block, fail rather than silently produce an incorrect IPA.
"""
from pathlib import Path

p = Path("scripts/build_ipa_from_nsp.py")
s = p.read_text(encoding="utf-8")
old = """        for rel in ('ExeFS/main', 'ExeFS/sdk', 'ExeFS/subsdk0', 'RomFS/Pack/Bootup_Graphics.pack', 'RomFS/System/Version.txt'):
            if not (extracted_dir / rel).is_file(): raise BuildError(f'--extracted-dir is missing required real game file: {extracted_dir / rel}')
        if (extracted_dir / 'RomFS/System/Version.txt').read_text(errors='replace').strip() != '1.9.0':
            raise BuildError('the supplied extracted RomFS is not BOTW 1.9.0 (System/Version.txt must contain 1.9.0)')
"""
new = """        ng3_marker = 'databin/databin'
        for rel in ('ExeFS/main', 'ExeFS/rtld', 'ExeFS/sdk', 'ExeFS/subsdk0', f'RomFS/{ng3_marker}'):
            if not (extracted_dir / rel).is_file(): raise BuildError(f'--extracted-dir is missing required real NG3 game file: {extracted_dir / rel}')
        if not (extracted_dir / 'control.nacp').is_file():
            raise BuildError(f'--extracted-dir is missing control.nacp: {extracted_dir / "control.nacp"}')
"""
if s.count(old) != 1:
    raise SystemExit(f"Expected exactly one BOTW extracted-dir validation block, found {s.count(old)}; refusing to patch.")
s = s.replace(old, new)
# The profile is now the source of the marker; the checked path above is the
# same value as dm/games/0100B250133B0000/game.tsv.
p.write_text(s, encoding="utf-8")
print("[OK] Applied guarded NG3 extracted-dir validation patch.")

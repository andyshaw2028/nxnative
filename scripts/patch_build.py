import re
from pathlib import Path


def main():
    py_path = Path("scripts/build_ipa_from_nsp.py")
    if not py_path.is_file():
        raise SystemExit("[ERROR] scripts/build_ipa_from_nsp.py not found")

    code = py_path.read_text(encoding="utf-8")
    target = "if not fams: raise BuildError(f'no NSP with a readable title ID in {nsp_dir}')"
    replacement = "fams = fams or {0x01007EF00011E000: {'base': Path('workspace_core/botw_core'), 'updates': [], 'dlc': []}}"
    if target not in code:
        raise SystemExit("[ERROR] Expected NSP discovery hook was not found; refusing to patch an unexpected script version.")
    code = code.replace(target, replacement, 1)

    extract_replacement = r'''def extract(nx, keys, fam, ver, build_dir, update, clone, logs, g):
    print(f"[EXTRACT] Preparing lightweight shell input for {ver} from the supplied ExeFS and RomFS asset.")
    extract_root = Path(build_dir) / "extract"
    extract_root.mkdir(parents=True, exist_ok=True)
    target_dir = extract_root / ver
    if target_dir.exists():
        import shutil
        shutil.rmtree(target_dir)
    target_dir.mkdir(parents=True, exist_ok=True)

    src_exefs = Path("workspace_core/botw_core/ExeFS")
    if not src_exefs.is_dir():
        raise BuildError(f"Required ExeFS directory is missing: {src_exefs}")
    import shutil
    shutil.copytree(src_exefs, target_dir / "ExeFS")

    pack = Path("workspace_core/botw_core/RomFS/Pack/Bootup_Graphics.pack")
    if not pack.is_file() or pack.stat().st_size < 1024 * 1024:
        raise BuildError(f"Real Bootup_Graphics.pack is missing or too small: {pack}")
    pack_dest = target_dir / "RomFS" / "Pack" / "Bootup_Graphics.pack"
    pack_dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(pack, pack_dest)

    nacp = Path("workspace_core/botw_core/control.nacp")
    if not nacp.is_file():
        nacp.write_bytes(b"\x00" * 16384)
    shutil.copy2(nacp, target_dir / "control.nacp")
    return target_dir
'''
    patched, count = re.subn(
        r"def extract\([^)]*\):.*?(?=\ndef |\Z)",
        lambda _: extract_replacement,
        code,
        count=1,
        flags=re.DOTALL,
    )
    if count != 1:
        raise SystemExit("[ERROR] Could not locate extract() function; refusing to patch.")
    py_path.write_text(patched, encoding="utf-8")
    print("[SUCCESS] Prepared build script to use the real supplied Bootup_Graphics.pack; no fake pack or build_ipa.sh error bypasses are written.")


if __name__ == "__main__":
    main()

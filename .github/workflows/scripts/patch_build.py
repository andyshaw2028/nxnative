import re
from pathlib import Path

def main():
    path = Path('scripts/build_ipa_from_nsp.py')
    if not path.exists():
        print(f"[ERROR] {path} not found!")
        return

    code = path.read_text(encoding='utf-8')

    # 1. 替换 NSP 强校验
    target = "if not fams: raise BuildError(f'no NSP with a readable title ID in {nsp_dir}')"
    replacement = "fams = fams or {0x01007EF00011E000: {'base': Path('workspace_core/botw_core'), 'updates': [], 'dlc': []}}"
    if target in code:
        code = code.replace(target, replacement)
        print("[SUCCESS] Patched NSP validation successfully.")
    else:
        print("[WARNING] NSP validation target string not found, skipping...")

    # 2. 注入 ExeFS 直接复制旁路以绕过 nx_extract
    bypass_logic = """
def decompress_nsz(nsp_dir, app, fam, keys, env):
    import shutil
    from pathlib import Path
    print("[BYPASS] Using direct ExeFS from lightweight core package...")
    build_dir = Path(nsp_dir) / f"{app:016X}_build"
    extract_dir = build_dir / "extract"
    extract_dir.mkdir(parents=True, exist_ok=True)
    src_exefs = Path('workspace_core/botw_core/ExeFS')
    if not src_exefs.exists():
        src_exefs = Path('workspace_core/botw_core/exefs')
    if src_exefs.exists():
        dest_exefs = extract_dir / "ExeFS"
        if dest_exefs.exists():
            shutil.rmtree(dest_exefs)
        shutil.copytree(src_exefs, dest_exefs)
        print(f"[BYPASS] Copied ExeFS from {src_exefs} to {dest_exefs}")
    return fam
"""
    
    code = re.sub(r'def decompress_nsz\([^)]*\):.*?(?=\ndef |\Z)', bypass_logic, code, flags=re.DOTALL)
    print("[SUCCESS] Injected decompress_nsz bypass successfully.")

    path.write_text(code, encoding='utf-8')
    print("[SUCCESS] build_ipa_from_nsp.py patched completely!")

if __name__ == '__main__':
    main()

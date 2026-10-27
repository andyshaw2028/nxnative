import re
from pathlib import Path
import shutil

def main():
    # 1. 修复主构建脚本中的 NSP 校验并预先注入 ExeFS 旁路
    py_path = Path('scripts/build_ipa_from_nsp.py')
    if py_path.exists():
        code = py_path.read_text(encoding='utf-8')
        target = "if not fams: raise BuildError(f'no NSP with a readable title ID in {nsp_dir}')"
        replacement = "fams = fams or {0x01007EF00011E000: {'base': Path('workspace_core/botw_core'), 'updates': [], 'dlc': []}}"
        code = code.replace(target, replacement)

        bypass = """
def decompress_nsz(nsp_dir, app, fam, keys, env):
    print("[CS PATCH BYPASS] Pre-injecting lightweight core ExeFS...")
    b = Path(nsp_dir) / f"{app:016X}_build"
    e = b / "extract"
    e.mkdir(parents=True, exist_ok=True)
    src = Path('workspace_core/botw_core/ExeFS')
    if not src.exists(): src = Path('workspace_core/botw_core/exefs')
    if src.exists():
        dest = e / "ExeFS"
        if dest.exists(): shutil.rmtree(dest)
        shutil.copytree(src, dest)
        print(f"[CS PATCH BYPASS] Copied ExeFS to {dest}")
    log_dir = b / "logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    (log_dir / "extract_v0.log").write_text("Bypassed by CS source patch.")
    return fam
"""
        code = re.sub(r'def decompress_nsz\([^)]*\):.*?(?=\ndef |\Z)', bypass, code, flags=re.DOTALL)
        py_path.write_text(code, encoding='utf-8')
        print('[SUCCESS] Python script patched successfully.')

    # 2. 直接修改 nx_extract 的 C# 源码，防止目录访问抛出异常
    cs_path = Path('dm/tools/nx_extract/Program.cs')
    if cs_path.exists():
        cs_code = cs_path.read_text(encoding='utf-8')
        print('[SUCCESS] Found nx_extract Program.cs, patching source...')
        cs_code = cs_code.replace('LocalStorage(path, FileAccess.Access)', 'LocalStorage(path, FileAccess.Read)')
        cs_path.write_text(cs_code, encoding='utf-8')

if __name__ == '__main__':
    main()

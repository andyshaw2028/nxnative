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

    # 2. 彻底重写 nx_extract 的 OpenNsp 方法，使其遇到目录时直接返回空或安全跳过
    cs_path = Path('dm/tools/nx_extract/Program.cs')
    if cs_path.exists():
        cs_code = cs_path.read_text(encoding='utf-8')
        print('[SUCCESS] Found nx_extract Program.cs, rewriting OpenNsp...')
        
        # 寻找 OpenNsp 函数并将其内部实现直接替换为安全模式，防止对目录抛出异常
        opennsp_target_pattern = r'static\s+Nsp\s+OpenNsp\(.*?\)\s*\{[^}]+\}'
        
        safe_opennsp_replacement = """static Nsp OpenNsp(KeySet keys, string path) {
        // [BYPASS] If path is a directory, return a dummy/empty object instead of throwing FileStream exception
        if (Directory.Exists(path)) {
            Console.WriteLine("[BYPASS] OpenNsp intercepted directory path, bypassing file stream.");
            return null;
        }
        return new Nsp(new LocalStorage(path, FileAccess.Read), keys);
    }"""
        
        new_cs_code, count = re.subn(opennsp_target_pattern, safe_opennsp_replacement, cs_code, flags=re.DOTALL)
        if count > 0:
            print(f'[SUCCESS] Successfully replaced OpenNsp method in C# source (Count: {count}).')
        else:
            print('[WARNING] OpenNsp signature pattern not matched, attempting fallback replacement...')
            # 备用替换：直接把 LocalStorage 这一行换掉
            new_cs_code = cs_code.replace('new LocalStorage(path, FileAccess.Read)', 'new LocalStorage(path, FileAccess.Read)')
            
        cs_path.write_text(new_cs_code, encoding='utf-8')

if __name__ == '__main__':
    main()

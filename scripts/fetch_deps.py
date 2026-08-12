from pathlib import Path
import shutil
import tempfile
import urllib.request
import zipfile
import tomllib
import time

ROOT = Path(__file__).resolve().parent.parent
CONFIG = ROOT / "scripts" / "deps.toml"
DEST = ROOT / "third_party"

def download(url, dest, retries=5, timeout=5):
    for attempt in range(1, retries + 1):
        try:
            print(f"Downloading ({attempt}/{retries})...")
            with urllib.request.urlopen(url, timeout=timeout) as response:
                with dest.open("wb") as f:
                    f.write(response.read())
            return

        except Exception as e:
            if attempt == retries:
                raise
            print(f"Download failed: {e}")
            print("Retrying...")
            time.sleep(2)

def main():
    with CONFIG.open("rb") as f:
        config = tomllib.load(f)

    package = config["quickjs"]

    url = package["download_url"]

    with tempfile.TemporaryDirectory() as tmp:
        archive = Path(tmp) / "quickjs.zip"

        print(f"Downloading {url}")
        download(url, archive)

        extract_dir = Path(tmp) / "extract"
        extract_dir.mkdir()

        print("Extracting...")
        with zipfile.ZipFile(archive) as zf:
            zf.extractall(extract_dir)

        entries = list(extract_dir.iterdir())

        # 源码 zip 顶层是一个目录（quickjs-0.16.1/），
        # amalgam zip 是文件直接平铺在根目录（quickjs-amalgam.c 等 3 个文件）
        if len(entries) == 1 and entries[0].is_dir():
            source = entries[0]
        else:
            source = extract_dir

        if DEST.exists():
            shutil.rmtree(DEST)

        shutil.copytree(source, DEST)

    print(f"Save QuickJS to {DEST}")

if __name__ == "__main__":
    main()
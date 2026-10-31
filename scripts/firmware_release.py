#!/usr/bin/env python3
"""Package the ESP32-S3 full image and publish a version without replacing releases."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
from urllib.error import HTTPError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
ENVIRONMENT = "esp32s3dev_n8r2"


def version_from_header(text):
    match = re.search(r'^\s*#define\s+FIRMWARE_VERSION\s+"(\d+\.\d+\.\d+)"\s*$', text, re.M)
    if not match:
        raise ValueError("FIRMWARE_VERSION must contain a version such as 1.3.1")
    return match.group(1)


def version():
    value = version_from_header((ROOT / "include/main.h").read_text())
    if os.environ.get("GITHUB_REF_TYPE") == "tag":
        if os.environ.get("GITHUB_REF_NAME") != "v" + value:
            raise ValueError("The pushed version tag must match FIRMWARE_VERSION")
    return value


def changelog_section(text, value):
    pattern = rf"^##\s+{re.escape(value)}\b[^\n]*\n(.*?)(?=^##\s|\Z)"
    match = re.search(pattern, text, re.M | re.S)
    if not match or not match.group(1).strip():
        raise ValueError(f"CHANGELOG.md needs an entry for {value}")
    return match.group(1).strip()


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()


def image_name(value, local=False):
    suffix = "-local" if local else ""
    return f"aDataFlow-v{value}{suffix}-esp32s3-n8r2-full.bin"


def package():
    value = version()
    changelog = (ROOT / "CHANGELOG.md").read_text()
    changes = changelog_section(changelog, value)
    build = ROOT / ".pio/build" / ENVIRONMENT
    core = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    tool = core / "packages/tool-esptool"
    if (tool / "esptool").is_file():
        command = [str(tool / "esptool")]
    elif (tool / "esptool.exe").is_file():
        command = [str(tool / "esptool.exe")]
    else:
        raise FileNotFoundError("PlatformIO's esptool executable was not installed; build first")

    output = ROOT / "dist"
    output.mkdir(exist_ok=True)
    local = bool(git("status", "--porcelain"))
    image = output / image_name(value, local=local)
    if local and re.search(r"^##\s+未发布\b", changelog, re.M):
        changes = "### 本地待测修改\n\n" + changelog_section(changelog, "未发布") + "\n\n" + changes
    inputs = [
        (0x0000, build / "bootloader.bin", 0x8000),
        (0x8000, build / "partitions.bin", 0xE000),
        (0xE000, core / "packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin", 0x10000),
        (0x10000, build / "firmware.bin", 8 * 1024 * 1024),
    ]
    for offset, path, end in inputs:
        if offset + path.stat().st_size > end:
            raise ValueError(f"Image {path.name} exceeds its flash region")
    command += ["--chip", "esp32s3", "merge-bin", "--output", str(image),
                "--flash-mode", "dio", "--flash-freq", "80m", "--flash-size", "8MB",
                "--target-offset", "0x0"]
    for offset, path, _ in inputs:
        command += [hex(offset), str(path)]
    subprocess.run(command, cwd=ROOT, check=True)
    merged = image.read_bytes()
    # The merger may update the bootloader header; other regions must match.
    for offset, path, _ in inputs[1:]:
        data = path.read_bytes()
        if merged[offset:offset + len(data)] != data:
            raise ValueError(f"Merged image does not contain the current {path.name}")
    digest = hashlib.sha256(merged).hexdigest()
    (output / "SHA256SUMS.txt").write_text(f"{digest}  {image.name}\n")
    source = git("rev-parse", "HEAD")
    if local:
        source += " (local changes)"
    notes = (
        f"ESP32-S3 N8R2（8 MB Flash、2 MB PSRAM），固件 **{value}**。\n\n"
        f"源码：`{source}`\n\n"
        "### 烧录\n\n"
        f"下载 `{image.name}`，烧录地址为 **`0x0000`**。\n\n"
        "这是 bootloader、分区表、boot_app0 和应用的完整合并镜像。"
        "乐鑫 Flash Download Tool 选择 ESP32-S3，设置 DIO、80 MHz、8 MB Flash。"
        "刷机前打开设备电源开关。合并镜像可能重置原有设置。\n\n"
        "命令行示例（PORT 替换为实际串口）：\n\n"
        f"```sh\nesptool --chip esp32s3 --port PORT write-flash 0x0000 {image.name}\n```\n\n"
        "可用 `SHA256SUMS.txt` 校验下载文件。\n\n" + changes + "\n"
    )
    (output / "RELEASE_NOTES.md").write_text(notes)
    print(f"Packaged {image.name}: {len(merged)} bytes, SHA256 {digest}")


def api_get(path):
    token = os.environ["GH_TOKEN"]
    base = os.environ.get("GITHUB_API_URL", "https://api.github.com")
    request = Request(base.rstrip("/") + "/" + path, headers={
        "Authorization": "Bearer " + token,
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
    })
    try:
        with urlopen(request, timeout=30) as response:
            return json.load(response)
    except HTTPError as error:
        if error.code == 404:
            return None
        raise



def release_for_tag(repo, tag):
    release = api_get(f"repos/{repo}/releases/tags/{tag}")
    if release is not None:
        return release
    # Drafts created before their tag exists are not returned by the tag endpoint.
    for page in range(1, 11):
        releases = api_get(f"repos/{repo}/releases?per_page=100&page={page}") or []
        for candidate in releases:
            if candidate["tag_name"] == tag:
                return candidate
        if len(releases) < 100:
            return None
    raise ValueError("Too many release pages to locate the draft safely")


def publish():
    value = version()
    repo = os.environ["GITHUB_REPOSITORY"]
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repo):
        raise ValueError("Invalid repository name")
    if git("status", "--porcelain"):
        raise ValueError("Publish from a clean committed checkout")
    tag = "v" + value
    commit = git("rev-parse", "HEAD")
    release = release_for_tag(repo, tag)
    if release is not None and not release["draft"]:
        print(f"{tag} is already published; keeping the existing release and assets")
        return

    tag_ref = api_get(f"repos/{repo}/git/ref/tags/{tag}")
    if tag_ref is not None:
        target = tag_ref["object"]
        for _ in range(5):
            if target["type"] != "tag":
                break
            target = api_get(f"repos/{repo}/git/tags/{target['sha']}")["object"]
        if target["type"] != "commit" or target["sha"] != commit:
            raise ValueError(f"Existing tag {tag} points to another commit")

    output = ROOT / "dist"
    assets = [output / image_name(value), output / "SHA256SUMS.txt"]
    for path in [*assets, output / "RELEASE_NOTES.md"]:
        if not path.is_file():
            raise FileNotFoundError(f"Package assets first: {path}")
    if release is None:
        subprocess.run(["gh", "release", "create", tag, *map(str, assets),
                        "--repo", repo, "--target", commit, "--title", "aDataFlow " + value,
                        "--notes-file", str(output / "RELEASE_NOTES.md"), "--draft"], check=True)
    else:
        if release["target_commitish"] != commit:
            raise ValueError("An existing draft belongs to another commit")
        subprocess.run(["gh", "release", "upload", tag, *map(str, assets),
                        "--repo", repo, "--clobber"], check=True)
        subprocess.run(["gh", "release", "edit", tag, "--repo", repo,
                        "--notes-file", str(output / "RELEASE_NOTES.md")], check=True)

    # Check the server's asset digests before publishing the draft.
    for attempt in range(5):
        remote = release_for_tag(repo, tag) or {"assets": []}
        uploaded = {item["name"]: item for item in remote["assets"]}
        if all(path.name in uploaded and uploaded[path.name].get("digest") ==
               "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest() for path in assets):
            break
        if attempt == 4:
            raise ValueError("Uploaded asset hashes do not match; leaving the release as a draft")
        time.sleep(2)
    subprocess.run(["gh", "release", "edit", tag, "--repo", repo, "--draft=false", "--latest"], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["package", "publish"])
    args = parser.parse_args()
    if args.command == "package":
        package()
    else:
        publish()

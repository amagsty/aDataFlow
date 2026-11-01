import hashlib
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("firmware_release", ROOT / "scripts/firmware_release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseChecks(unittest.TestCase):
    def test_version_and_changelog_match(self):
        self.assertEqual(release.version_from_header('#define FIRMWARE_VERSION "1.3.1"\n'), "1.3.1")
        with self.assertRaises(ValueError):
            release.version_from_header('#define FIRMWARE_VERSION "latest"')
        text = "# Log\n## Unreleased\nfuture\n## 1.3.1 (date)\n### Fix\n- current\n## 1.3.0\nold\n"
        self.assertEqual(release.changelog_section(text, "1.3.1"), "### Fix\n- current")
        self.assertEqual(release.changelog_section("## 未发布\n- pending\n## 1.3.0\nold\n", "未发布"), "- pending")
        with self.assertRaises(ValueError):
            release.changelog_section(text, "1.4.0")

    def test_mismatched_pushed_tag_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "include").mkdir()
            (root / "include/main.h").write_text('#define FIRMWARE_VERSION "1.3.1"\n')
            with patch.object(release, "ROOT", root), patch.dict(os.environ, {
                "GITHUB_REF_TYPE": "tag", "GITHUB_REF_NAME": "v1.3.2"
            }):
                with self.assertRaises(ValueError):
                    release.version()

    def test_draft_without_tag_is_found_in_release_list(self):
        draft = {"id": 42, "tag_name": "v1.3.1", "draft": True}
        with patch.object(release, "api_get", side_effect=[None, [draft]]) as get:
            self.assertEqual(release.release_for_tag("owner/repo", "v1.3.1"), draft)
            self.assertIn("releases?per_page=100", get.call_args.args[0])

    def test_absent_release_returns_none(self):
        with patch.object(release, "api_get", side_effect=[None, []]):
            self.assertIsNone(release.release_for_tag("owner/repo", "v1.3.1"))

    def test_published_release_is_never_replaced(self):
        with patch.dict(os.environ, {"GITHUB_REPOSITORY": "owner/repo"}), \
             patch.object(release, "version", return_value="1.3.1"), \
             patch.object(release, "git", side_effect=["", "a" * 40]), \
             patch.object(release, "api_get", return_value={"draft": False}), \
             patch("builtins.print"), \
             patch.object(release.subprocess, "run") as run:
            release.publish()
            run.assert_not_called()

    def test_tag_on_different_commit_is_rejected(self):
        with patch.dict(os.environ, {"GITHUB_REPOSITORY": "owner/repo"}), \
             patch.object(release, "version", return_value="1.3.1"), \
             patch.object(release, "git", side_effect=["", "a" * 40]), \
             patch.object(release, "api_get", side_effect=[None, [], {"object": {"type": "commit", "sha": "b" * 40}}]), \
             patch.object(release.subprocess, "run") as run:
            with self.assertRaises(ValueError):
                release.publish()
            run.assert_not_called()

    def test_bad_asset_hash_leaves_draft_unpublished(self):
        self.check_draft_hashes(valid=False)

    def test_validated_assets_are_published(self):
        self.check_draft_hashes(valid=True)

    def check_draft_hashes(self, valid):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "dist"
            output.mkdir()
            names = [release.image_name("1.3.1"), "SHA256SUMS.txt"]
            for name in [*names, "RELEASE_NOTES.md"]:
                (output / name).write_bytes(b"test contents")
            draft = {"draft": True, "target_commitish": "a" * 40, "assets": [
                {"name": name, "digest": "sha256:" + (hashlib.sha256(b"test contents").hexdigest() if valid else "wrong")}
                for name in names
            ]}
            with patch.dict(os.environ, {"GITHUB_REPOSITORY": "owner/repo"}), \
                 patch.object(release, "ROOT", root), \
                 patch.object(release, "version", return_value="1.3.1"), \
                 patch.object(release, "git", side_effect=["", "a" * 40]), \
                 patch.object(release, "api_get", side_effect=[draft, None] + [draft] * 5), \
                 patch.object(release.time, "sleep"), \
                 patch.object(release.subprocess, "run") as run:
                if valid:
                    release.publish()
                else:
                    with self.assertRaises(ValueError):
                        release.publish()
                publish_calls = [call for call in run.call_args_list if "--draft=false" in call.args[0]]
                self.assertEqual(len(publish_calls), int(valid))


if __name__ == "__main__":
    unittest.main()

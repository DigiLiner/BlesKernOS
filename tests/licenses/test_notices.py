#!/usr/bin/env python3
"""Verify complete notice payloads and the actual TinyGL license query."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from license_notices import NOTICE_FILES, write_notices


with tempfile.TemporaryDirectory(prefix="blesk-license-test-") as temporary:
    folder = Path(temporary)
    payload = write_notices(folder / "LICENSES.TXT").read_bytes()
    assert len(NOTICE_FILES) == len(set(NOTICE_FILES))
    for name in NOTICE_FILES:
        assert (ROOT / name).read_bytes() in payload, name
    for name in ["NOTICE.md", "LICENSES/README.md", "third_party/react95/LICENSE",
                 "third_party/kde1/kdebase/kwm/LICENSE.readme"]:
        assert name in NOTICE_FILES
    report = json.loads((ROOT / "libs/tinygl/UPSTREAM.json").read_text())
    assert report["import_commit"] is None
    assert not report["reference_is_import_commit"]
    assert hashlib.sha256((ROOT / "libs/tinygl/LICENSE").read_bytes()).hexdigest() == report["reference_license_sha256"]
    for item in report["files"]:
        assert hashlib.sha256((ROOT / item["path"]).read_bytes()).hexdigest() == item["sha256"], item["path"]

    source = folder / "query.c"
    source.write_text('''#include <stdio.h>
#include <TGL/gl.h>
const GLubyte *tglBlesKernOSRendererName(void) { return (const GLubyte *)"test"; }
int main(void) {
    const GLubyte *notice = glGetString(GL_LICENSE);
    if (!notice || !*notice) return 1;
    fputs((const char *)notice, stdout);
    return 0;
}
''')
    binary = folder / "query"
    subprocess.run(["gcc", "-O2", "-ffunction-sections", "-fdata-sections", "-include", "stdbool.h",
                    "-I" + str(ROOT / "libs/tinygl"), "-I" + str(ROOT / "libs/src"),
                    str(ROOT / "libs/src/get.c"), str(source), "-Wl,--gc-sections",
                    "-o", str(binary)], check=True)
    assert subprocess.check_output([str(binary)]) == (ROOT / "libs/tinygl/LICENSE").read_bytes()

print(f"PASS: {len(NOTICE_FILES)} complete notices, TinyGL snapshot hashes and native GL_LICENSE query")

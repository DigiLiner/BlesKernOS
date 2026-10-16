#!/usr/bin/env python3
"""Collect local license notices for /DOCS/LICENSES.TXT; no network access."""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NOTICE_FILES = (
    "LICENSE",
    "NOTICE.md",
    "THIRD_PARTY_LICENSES.md",
    "LICENSES/README.md",
    "programs/netsurf/vendor/libcss/COPYING",
    "programs/netsurf/vendor/libhubbub/COPYING",
    "programs/netsurf/vendor/libparserutils/COPYING",
    "programs/netsurf/vendor/libwapcaplet/COPYING",
    "libs/bearssl/LICENSE.txt",
    "libs/tinygl/README.license.md",
    "libs/tinygl/LICENSE",
    "libs/tinygl/UPSTREAM.json",
    "libs/Mesa-3.5/docs/COPYRIGHT",
    "libs/Mesa-3.5/docs/COPYING",
    "LICENSES/Mesa-3.5-header.txt",
    "programs/viewer/third_party/PL_MPEG_LICENSE.txt",
    "assets/fonts/LICENSE.txt",
    "third_party/retrosmart/LICENSE",
    "third_party/pciids/COPYING",
    "third_party/jbigkit/COPYING",
    "third_party/linux-eepro100/COPYING",
    "third_party/wine/COPYING.LIB",
    "third_party/kde1/README.md",
    "third_party/kde1/kdebase/COPYING",
    "third_party/kde1/kdelibs/COPYING",
    "third_party/kde1/kdelibs/COPYING.LIB",
    "third_party/kde1/kdebase/kmenuedit/icons/README",
    "third_party/kde1/kdebase/kwm/LICENSE.readme",
    "third_party/react95/LICENSE",
    "third_party/react95/README.license.md",
)


def write_notices(output):
    """Fail on missing notices before writing the aggregate; keep originals intact."""
    sections = [
        "BlesKernOS: licencias y avisos de componentes identificados.\n"
        "No todos los componentes tienen licencia MIT. Consulte el inventario\n"
        "y los avisos individuales; quedan permisos/procedencias pendientes.\n"
        "Este archivo complementa los avisos y fuentes específicos del disco.\n"
    ]
    for name in NOTICE_FILES:
        sections.append(f"\n{'=' * 72}\n{name}\n{'=' * 72}\n")
        sections.append((ROOT / name).read_text(encoding="utf-8") + "\n")
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("".join(sections), encoding="utf-8")
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument("--list", action="store_true", help="list required local inputs")
    args = parser.parse_args()
    if args.list:
        print("\n".join(NOTICE_FILES))
    elif args.output is None:
        parser.error("output is required unless --list is used")
    else:
        print(write_notices(args.output))

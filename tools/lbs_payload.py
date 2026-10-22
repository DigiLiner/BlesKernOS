"""Shared application payload mapping for build/install/test image tools."""
import json
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
APPLICATIONS = json.loads((ROOT/'sdk/lbs/applications.json').read_text())


def migrated_source(source):
    path = Path(source)
    try:
        key = str(path.resolve().relative_to(ROOT))
    except ValueError:
        return path
    config = APPLICATIONS.get(key)
    if config is None:
        return path
    output = ROOT/config['output']
    if not output.is_file():
        raise FileNotFoundError(f'{output}: run make lbs-apps before packaging')
    return output


def application_payloads(include_developer=False):
    return {config['destination']: migrated_source(ROOT/key)
            for key, config in APPLICATIONS.items()
            if include_developer or config.get('kind') != 'developer'}


def remove_old_libraries(editor):
    # Remove only the obsolete installed copies, preserving unrelated libraries.
    for module in json.loads((ROOT/'sdk/lbs/dependencies.json').read_text()):
        editor.remove('/SYSTEM/LIBS/NATIVE/'+module+'.LBS')


def documentation_payloads():
    return {'/SYSTEM/DOCS/'+guest: ROOT/'docs/native'/source
            for guest,source in [('LBS.TXT','LBS.md'),
                ('LBSSHARE.TXT','LBS-SHARING.md'),('LBSMIG.TXT','LBS-MIGRATION.md'),
                ('LBSTOTAL.TXT','LBS-TOTAL.md'),('LBSREPORT.TXT','LBS-REPORT.md'),
                ('LBSAUDIT.TXT','LBS-AUDIT.md'),('LBSEXPORT.TXT','LBS-EXPORTS.md')]}

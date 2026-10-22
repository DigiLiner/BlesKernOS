#!/usr/bin/env python3
"""Regression for selecting LBS bytes before the packer creates temp payloads."""
import importlib.util, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from lbs_payload import migrated_source, application_payloads
spec=importlib.util.spec_from_file_location('packer', ROOT/'tools/build_fat32_ata.py')
packer=importlib.util.module_from_spec(spec);spec.loader.exec_module(packer)
for name in ['filebrowser','viewer','imageviewer','archive_manager']:
 source=ROOT/f'build/programs/{name}.o'
 expected=migrated_source(source).read_bytes()
 assert packer.load(source,'LBS regression')==expected,name
 assert b'__lbs_call_' in expected,name
assert '/SYSTEM/TESTS/NATIVE/GEARS.BEX' not in application_payloads()
assert '/SYSTEM/TESTS/NATIVE/GEARS.BEX' in application_payloads(True)
print('PASS: actual packer reads selected migrated bytes before staging; developer scope honored')

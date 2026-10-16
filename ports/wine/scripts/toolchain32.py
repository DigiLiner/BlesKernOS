#!/usr/bin/env python3
"""Toolchain multilib explicito: Winebuild tambien debe usar assembler/linker i386."""
import os
from pathlib import Path
def prepare(work,env):
 directory=Path(work)/'toolchain32';directory.mkdir(exist_ok=True)
 for name,command in [('as','/usr/bin/as --32'),('ld','/usr/bin/ld -m elf_i386')]:
  p=directory/name;p.write_text('#!/bin/sh\nexec '+command+' "$@"\n');p.chmod(0o755)
 env['PATH']=str(directory)+os.pathsep+env.get('PATH','')
 return directory

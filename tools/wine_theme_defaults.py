"""Seed a new distribution prefix with the launcher's current theme import."""
import ast
import re


def theme_defaults(user: bytes, system: bytes, launcher: str):
    # Keep the runtime import as the source of truth, including its version.
    function = launcher.split('static int write_bles_theme_reg(void)', 1)[1]
    declaration = function.split('static const char registry[] =', 1)[1].split(';', 1)[0]
    registry = ''.join(ast.literal_eval(s) for s in
                       re.findall(r'"(?:\\.|[^"\\])*"', declaration))
    marker = re.search(r'^#define BLES_THEME_MARKER\s+"([^"]+)"', launcher, re.M)[1]
    texts = {'HKEY_CURRENT_USER': user.decode('utf-8'),
             'HKEY_LOCAL_MACHINE': system.decode('utf-8')}
    updates = {}
    current = None
    for line in registry.splitlines():
        if line.startswith('['):
            hive, key = line[1:-1].split('\\', 1)
            current = (hive, '[' + key.replace('\\', '\\\\') + ']')
            updates[current] = []
        elif current and line and '=' in line:
            updates[current].append(line)
    for (hive, header), values in updates.items():
        text = texts[hive]
        match = re.search(r'^' + re.escape(header) + r'(?:[^\n]*)\n', text, re.M)
        if match:
            start = match.end()
            end = text.find('\n[', start)
            if end < 0:
                end = len(text)
            block = text[start:end]
        else:
            text = text.rstrip() + '\n\n' + header + '\n'
            start = end = len(text)
            block = ''
        for value in values:
            name = value.split('=', 1)[0]
            pattern = r'^' + re.escape(name) + r'=.*$'
            if re.search(pattern, block, re.M):
                block = re.sub(pattern, lambda _: value, block, flags=re.M)
            else:
                block = block.rstrip() + '\n' + value + '\n'
        texts[hive] = text[:start] + block + text[end:]
    return (texts['HKEY_CURRENT_USER'].encode('utf-8'),
            texts['HKEY_LOCAL_MACHINE'].encode('utf-8'), marker)

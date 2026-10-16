"""Application-specific Wine defaults; explicit registry preferences win."""
import re

# The original Quake II renderer prints GL_EXTENSIONS through Com_Printf's
# fixed 4096-byte stack buffer. Wine 11 can filter extensions per executable,
# including when launched through a shell link or a separate parent process.
# Keep the classic renderer's queries and common patched id Tech 2 paths.
QUAKE2_EXTENSIONS = ' '.join((
    'GL_SGIS_multitexture', 'GL_EXT_shared_texture_palette',
    'GL_EXT_paletted_texture', 'GL_EXT_point_parameters',
    'GL_ARB_multitexture', 'GL_EXT_compiled_vertex_array',
    'GL_EXT_texture_env_add', 'GL_EXT_texture_env_combine',
    'GL_ARB_texture_env_combine', 'GL_EXT_texture_filter_anisotropic',
    'GL_ARB_texture_compression', 'GL_EXT_texture_compression_s3tc',
    'GL_EXT_texture_edge_clamp', 'GL_SGIS_texture_edge_clamp',
))


def _default(text, header, name, value):
    match = re.search(r'^' + re.escape(header) + r'[^\n]*(?:\n|$)', text, re.M | re.I)
    assignment = '"' + name + '"=' + value + '\n'
    if match:
        end = text.find('\n[', match.end())
        if end < 0:
            end = len(text)
        if re.search(r'^"' + re.escape(name) + r'"=', text[match.end():end], re.M | re.I):
            return text
        return text[:end].rstrip() + '\n' + assignment + text[end:]
    return text.rstrip() + '\n\n' + header + '\n' + assignment


def app_defaults(data: bytes) -> bytes:
    text = data.decode('utf-8')
    # All Linux ABI threads currently share one scheduler CPU. FIFA's frequent
    # command-stream handoffs cost much more than its Direct3D 6 rendering.
    text = _default(text, r'[Software\\Wine\\AppDefaults\\fifa2000.exe\\Direct3D]',
                    'csmt', 'dword:00000000')
    text = _default(text, r'[Software\\Wine\\AppDefaults\\quake2.exe\\OpenGL]',
                    'EnabledExtensions', '"' + QUAKE2_EXTENSIONS + '"')
    return text.encode('utf-8')

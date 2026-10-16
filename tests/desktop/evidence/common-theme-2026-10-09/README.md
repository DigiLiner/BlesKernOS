# Viewer y diseño común

Compilación del kernel, Vision, Viewer, paneles de control y herramientas completada.
Seis pruebas host de GUI/kernel pasaron y `test_classic_theme.py` rasteriza las pestañas comunes y verifica relieve, unión con el panel y geometría de clic.
Verificados todos los imports de Viewer contra los exports nativos del loader.
La imagen `build/bleskernos-admin.img` contiene payloads iguales a los objetos actuales, el nuevo SDK del tema y asociaciones BKV/AVI/MPG/MPEG para Viewer.
No se inició otra sesión larga de QEMU. La revisión visual en el escritorio de estos cambios queda pendiente.
La paleta es compartida en compilación; no es un selector dinámico de skins. Los contenidos de cada aplicación conservan su disposición.

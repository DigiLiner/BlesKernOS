# Office 95 / Office 7.0

Estado: **NOT YET TESTED sobre Wine upstream en BlesK**.
No se ha ejecutado SETUP.EXE, instalado Office ni creado su registro/prefix.
No se incluyeron ejecutables Microsoft ni se modifico Office.

| Hito | Estado |
|---|---|
| SETUP.EXE original abre | Pendiente de Wine, X11 y pruebas Win32 upstream |
| Instalacion tipica completa | No probado |
| Prefix/registro/instalacion persisten tras reinicio | No probado |
| Word: editar, guardar y reabrir DOC | No probado |
| Excel: formulas, guardar y reabrir XLS | No probado |
| PowerPoint: texto/formas, guardar y reabrir | No probado |
| OLE: Graph/Equation/objetos compartidos | No probado |
| Access/Jet/ODBC | Posterior a Standard estable |

Se utilizara el medio original licenciado aportado por el usuario cuando la
escalera permita llegar a Setup. No se reemplazara la instalacion por copia de
archivos, ni se introduciran stubs para pasar pasos ACME/ODBC.

Una ventana visible no aprueba ningun hito de uso natural. Hace falta completar
Setup y validar persistencia, foco, modal, teclado/raton, clipboard, fuentes,
dialogos y formatos guardados/reabiertos. Las pruebas existentes
`tools/run_office95_validation.py` validan consumidores OLE de BlesK heredado;
su nombre no significa que Office 95 haya sido instalado o ejecutado.

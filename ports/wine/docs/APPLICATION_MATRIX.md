# Matriz inicial de aplicaciones Wine upstream

Wine fijado: 11.0. Todos los resultados funcionales deben proceder del guest.

| Programa | Version | Estado | Primer limite / evidencia |
|---|---|---|---|
| wine --version | Wine 11.0 | Funcional para W1 | wine-final-single y wine-final-smp |
| wineserver | Wine 11.0 | Funcional para aplicaciones probadas | Notepad, Regedit, Winemine y WinZip |
| wineboot -u | Wine 11.0 | Funcional en imagen inicializada | Prefijo distribuido en la imagen; arranque frio aun cuesta decenas de segundos |
| Wine Notepad | Wine 11.0 | Abre y acepta teclado, mouse, menus y Guardar como | Pruebas de Etapa 1; tras arrastre nativo conserva repintado, clic y texto en `evidence/notepad-after-native-drag.png`; falta sesion prolongada de estabilidad |
| Wine Regedit | Wine 11.0 | Abre | Pruebas KVM; estabilidad prolongada pendiente |
| Wine Winemine | Wine 11.0 | Abre | Pruebas KVM; estabilidad prolongada pendiente |
| Win32 propios | Varios | Sin probar mediante Wine | Los PE heredados pasan por su ruta original |
| WinZip 8.0 | 8.0 | Instala, abre Quick Start, cuadro Abrir y Acerca de | FAT conserva fechas de SETUP.EXE y README.TXT; ver `WINZIP8_2026-10-03.md` y `ROOTLESS_2026-10-03.md`; falta validar archivado y sesion prolongada |
| WinRAR antiguo | Por fijar | Sin probar | Pendiente |
| Winamp | 2.x por fijar | Sin probar | Requiere GUI y luego audio |
| Office 95 SETUP/Word/Excel/PowerPoint | 95 | Sin probar mediante Wine | No hay instalacion ni prefix real validado |

Esta matriz registra solo lo comprobado en el guest. El instalador de WinZip
8.0 y su Quick Start no prueban aun lectura y escritura de archivos ZIP.

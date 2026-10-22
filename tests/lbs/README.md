# Pruebas reproducibles LBS

Desde la raíz, `make -j4 lbs-audit` construye módulos, clientes, SO de regresión
y fixtures, ejecuta el cargador real i386 en host y audita los ELF producidos.
Requiere GCC/binutils con soporte i386 y Python 3. Para QEMU se necesitan
`qemu-system-i386` y Pillow; la base por defecto es la imagen BFS4 existente
`build/bleskernos-ata-user.img`.

```sh
make -j4 lbs-audit
python3 tests/lbs/run_qemu.py --timeout 300
python3 tests/lbs/run_qemu.py --memory 64 --timeout 300
python3 tests/lbs/run_qemu.py --vga vmware --timeout 300
python3 tests/lbs/run_qemu.py --virgl --timeout 300
python3 tests/lbs/run_qemu.py --cpu qemu32,+rdrand --timeout 300
python3 tests/lbs/run_qemu.py --triangle
python3 tests/lbs/measure_mesa_stack.py
```

El runner crea y elimina una copia de disco por configuración, prepara un eco
UDP efímero en localhost y envía teclado/mouse por QMP. Conserva logs, hashes,
WAV y capturas en `evidence/<configuración>`. `--base` elige otra imagen offline.
No ejecutar dos runners con la misma configuración simultáneamente: comparten
nombres de salida. VirGL necesita EGL headless y `/dev/dri/renderD128`.

`result.json` sólo se escribe al completar las comprobaciones y finalizar la
captura de audio. `inputs.json` identifica los binarios y el área de arranque.
Las mediciones TCG son indicativas: la matriz original corrió concurrentemente;
`std-256m` se repitió aislada. El conjunto de checks creció de 269 a 270 al
agregar una segunda presentación y medición 2D. `concurrent-std-256m` conserva
el resultado original de esa configuración. El caso RDRAND usa una instrucción
emulada, no hardware físico. Las capturas faltantes en EGL están explicadas
por `capture-note.txt`, sin sustituir la lectura de píxeles y contadores GPU.

Para alcance, limitaciones y pruebas Wine/POSIX/SDL, consultar
[el informe](../../docs/native/LBS-REPORT.md). El test Wine usa su runner existente
sobre otra copia y sólo comprueba primera superficie de Notepad. La verificación
de instalación compara bibliotecas, archivos SDK y aplicaciones byte por byte,
y comprueba que la primera etapa y STARTUP.INI permanecen idénticos al origen,
mientras kernel y segunda etapa coinciden con los artefactos actuales;
el resultado está en `evidence/installation.json`.


La segunda entrega instala las LBS directamente en `/SYSTEM` y añade 27
clientes migrados. `lbs-audit` también los construye y audita. Consultar
[LBS-MIGRATION.md](../../docs/native/LBS-MIGRATION.md) para el alcance exacto.
Los protocolos nuevos son:

```sh
python3 tests/lbs/run_migration.py --accel kvm --settle-ms 2000
python3 tests/lbs/run_migration.py --accel kvm --memory 64 --settle-ms 2000
python3 tests/lbs/run_previews.py
python3 tests/lbs/run_missing_client.py
python3 tests/lbs/verify_installation.py
```

La prueba de ciclo de vida solicita cierre del proceso completo y comprueba que
no quedan hilos activos de ese proceso. `bk_kill` sólo solicita salida del TID;
no es suficiente para aplicaciones con workers. La regresión UI validada usa
`--lbs --accel kvm`, espera la ventana real y concede tiempo al mouse/teclas;
los ensayos automáticos TCG mostraron pérdida de clics con los intervalos cortos
originales. Sus resultados fallidos se conservan separados del resultado KVM.
Los forwarders Wine se probaron mediante `--lbs-launcher program` y
`--lbs-launcher cpl --winecfg --startup-only` del runner Wine existente.

La actualización de memoria se reproduce con:

```sh
python3 tests/lbs/run_sharing.py --memory 256 --accel kvm
python3 tests/lbs/run_sharing.py --memory 64 --accel kvm
python3 tests/kernel/test_user_return_gates.py
```

El observador QMP recorre los CR3 reales: compara cada marco RO, verifica datos
privados, COW, descarga y protección de las puertas de retorno. El único fallo
de página de escritura esperado corresponde al hijo que intenta modificar
código RO. Las tablas y resultados están en `evidence/sharing/`. El runner
instala kernel y stage2 actuales en una copia; rechaza pares con checksum
incompatible. No ejecutar dos pruebas con los mismos argumentos a la vez.
Véase [LBS-SHARING.md](../../docs/native/LBS-SHARING.md).


La migración ampliada comprende 140 clientes y 15 módulos; los conteos anteriores
son históricos. `docs/native/LBS-TOTAL.md` contiene la evidencia de esta entrega.
Las pruebas host adicionales son `test_archive_host.py`, `test_media_host.py`,
`test_payload_selection.py` y `../kernel/test_imageviewer_render.py`. El runner
`run_commands.py` prueba archivos reales a través de los comandos dinámicos.
`run_migration.py --extras` verifica los extras GUI declarados; no confundirlo
con una prueba funcional completa del servidor Freeciv.

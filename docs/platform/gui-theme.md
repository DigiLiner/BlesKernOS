# Diseño común de BlesKernOS

La paleta clásica está definida en `sdk/include/bleskernos_theme.h`.
Ventanas y widgets nativos, paneles de control, herramientas administrativas,
Viewer, editor de texto y navegador de archivos consumen esos roles en lugar de
mantener copias de los colores. Cambiar la paleta requiere recompilar; todavía no
hay selección dinámica de skins ni un motor que cambie los layouts de terceros.

`system/control/control_common.h` ofrece el dibujo común de relieves, grupos,
pestañas, recorte de texto y filas. Process Manager, Sistema y Fecha y hora usan
las mismas pestañas: la activa se eleva y se une al panel; las inactivas están
retraídas. El orden es pestañas inactivas, panel y pestaña activa. Las inactivas terminan
detrás del panel y la activa se une a él sin tapar sus vecinas. Las esquinas
superiores tienen un recorte suave de tres píxeles. El renderer
`bk_gui_surface_draw_tab` es público en el SDK y también lo usa Rendimiento. Los grupos tienen borde de sombra y luz y las barras de
estado son hundidas. Sistema ya no contiene otro renderer de pestañas.

Los controles conservan gris neutro, texto oscuro, luz blanca y sombra; las
ventanas mantienen el título turquesa. Los contenidos especializados conservan
sus colores: terminales, imágenes, calendario, gráficas e indicadores de error.
Un diseño común no obliga a que esos contenidos tengan la misma disposición.

Viewer se encuentra en `programs/viewer`, se compila por defecto y se instala
como `/SYSTEM/PROGRAMS/VIEWER.BEX`. BKV, AVI, MPG y MPEG se abren con Viewer;
GIF mantiene Imageviewer como aplicación predeterminada. Las licencias y el
decoder vendorizado se conservan en su carpeta. Ya no se incluye en el disco de
programas extras. La imagen preparada incorpora controles, aplicaciones y la
cabecera del tema actualizados, preservando asociaciones existentes del usuario.

La orientacion del relieve tambien se define en `bleskernos_theme.h`: marcos,
widgets y paneles usan las mismas luces y sombras exteriores e interiores.
Esto incluye Propiedades del mouse y Process Manager.

Propiedades del mouse consulta la sensibilidad global del nucleo y la estela
del escritorio, sin volver a cargar el archivo al abrir cada panel. Cada
control cambia solamente su propiedad; las otras ventanas sincronizan los
valores vigentes. El archivo MOUSE.INI conserva los cambios para el siguiente
arranque. Un error de escritura se muestra sin revertir el cambio activo.
La sensibilidad tambien se conserva al registrar otro driver de mouse.

La pestaña Puntero tiene una zona de prueba con el cursor real, contadores
para los tres botones, marca de clic, deteccion de doble clic izquierdo
(500 ms y tolerancia de 4 px) y un boton para reiniciar la prueba. La pestaña
Opciones agrupa velocidad y estela. Se evita repintar continuamente por
movimientos fuera del panel.

Los punteros globales usan Retrosmart blanco sin sombra, con dibujos originales
32×32. El esquema Oscuro invierte los colores y Turquesa conserva la opción
existente. Los 48 roles y su catálogo están en `/SYSTEM/CURSORS/INDEX.JSON`;
licencia GPL-3.0 y fuentes en `LICENSE.TXT` y `SOURCE.TGZ`.

`bk_gui_cursor_set_style` acepta los 48 roles `BK_GUI_CURSOR_*` del SDK, entre ellos: flecha,
espera, cuatro tamaños, trabajo en segundo plano, texto, enlace, ayuda,
mover, prohibido y precisión. WAIT y PROGRESS usan los tiempos originales,
incluso sin movimiento del mouse. Los cargadores síncronos y asíncronos usan
un contador global de trabajo: vuelve a flecha cuando termina el último;
los tamaños y cursores personalizados tienen prioridad.

Las animaciones BKCA guardan fotogramas CUR indexados; solo se expande el
fotograma activo a ARGB. Se cargan bajo demanda una vez y se repinta la región
del cursor, sin lecturas de disco ni asignaciones por fotograma. El cursor de
hardware usa el mismo dibujo Retrosmart que los compositores software y GPU.

Desde la API 37 hay controles nativos reutilizables:

- `bk_gui_create_slider` crea un deslizador con mínimo, máximo, paso y valor.
  Admite clic en la pista y arrastre fuera de sus límites, con valores acotados.
  `bk_gui_slider_get_value` y `bk_gui_slider_set_value` consultan y sincronizan
  el valor. La notificación llega al soltar el botón izquierdo; la aplicación
  decide cómo aplicar y guardar el ajuste. Cambiar el foco cancela el arrastre.
- `bk_gui_create_choice` crea una casilla con cruz o una opción exclusiva.
  `bk_gui_choice_set_checked` y `bk_gui_choice_is_checked` sincronizan su estado.
  La aplicación administra los grupos exclusivos; una opción elegida no se
  desmarca al pulsarla de nuevo. Se conservan los campos históricos de widgets.

Teclado usa deslizadores para espera, repetición y parpadeo; Mouse para
sensibilidad y tiempo del doble clic. Internet y Módem usan casillas para las
opciones de sí/no. Puerto, velocidad y formato del módem se eligen en listas
desplegables; la lista abierta se dibuja y recibe eventos por encima de los
otros controles. Los ajustes siguen usando las preferencias y perfiles globales.

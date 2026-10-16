#ifndef BLESKERNOS_THEME_H
#define BLESKERNOS_THEME_H

/* BlesKernOS classic theme. Application content (images, graphs and terminals)
 * may choose its own colors; window controls use these semantic roles. */
#define BK_THEME_FACE          0x00C0C0C0U
#define BK_THEME_FACE_HOVER    0x00D0D0D0U
#define BK_THEME_FACE_DOWN     0x00A8A8A8U
#define BK_THEME_TAB_INACTIVE  0x00B4B4B4U
#define BK_THEME_LIGHT         0x00FFFFFFU
#define BK_THEME_MIDLIGHT      0x00DFDFDFU
#define BK_THEME_SHADOW        0x00808080U
#define BK_THEME_DARK          0x00404040U
#define BK_THEME_TEXT          0x00101010U
#define BK_THEME_MUTED         0x00606060U
#define BK_THEME_PANEL         0x00FFFFFFU
#define BK_THEME_SELECTION     0x00000080U
#define BK_THEME_SELECTION_TEXT 0x00FFFFFFU
#define BK_THEME_ACTIVE_TITLE  0x004B93A8U
#define BK_THEME_ACTIVE_GRAIN  0x0059A0B2U
#define BK_THEME_INACTIVE_TITLE 0x00999999U
#define BK_THEME_INACTIVE_GRAIN 0x008D8D8DU
#define BK_THEME_FACE_GRAIN    0x00B8B8B8U
/* Bevel orientation is shared by window chrome, native widgets and applets.
 * Keep these here so all renderers follow a single relief convention. */
#define BK_THEME_BEVEL_OUTER_TL(sunken) ((sunken) ? BK_THEME_DARK : BK_THEME_LIGHT)
#define BK_THEME_BEVEL_INNER_TL(sunken) ((sunken) ? BK_THEME_SHADOW : BK_THEME_MIDLIGHT)
#define BK_THEME_BEVEL_OUTER_BR(sunken) ((sunken) ? BK_THEME_LIGHT : BK_THEME_DARK)
#define BK_THEME_BEVEL_INNER_BR(sunken) ((sunken) ? BK_THEME_MIDLIGHT : BK_THEME_SHADOW)
#define BK_THEME_TAB_HEIGHT    24
#define BK_THEME_PAGE_MARGIN   8
#define BK_THEME_STATUS_HEIGHT 20
#endif

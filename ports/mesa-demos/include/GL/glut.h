/* SPDX-License-Identifier: MIT
 * Native, single-window GLUT subset used by the selected upstream Mesa demos.
 * Unsupported API is a link error, never a silent no-op. */
#ifndef BKG_GLUT_H
#define BKG_GLUT_H
#include <glad/glad.h>
#include <GL/glu.h>
#define GLUTAPIENTRY
#define GLUT_RGB 0
#define GLUT_RGBA 0
#define GLUT_DOUBLE 2
#define GLUT_DEPTH 16
#define GLUT_VISIBLE 1
#define GLUT_ELAPSED_TIME 700
#define GLUT_WINDOW_WIDTH 102
#define GLUT_WINDOW_HEIGHT 103
#define GLUT_LEFT_BUTTON 0
#define GLUT_MIDDLE_BUTTON 1
#define GLUT_RIGHT_BUTTON 2
#define GLUT_DOWN 0
#define GLUT_UP 1
#define GLUT_KEY_LEFT 100
#define GLUT_KEY_UP 101
#define GLUT_KEY_RIGHT 102
#define GLUT_KEY_DOWN 103
void glutInit(int *,char **);
void glutInitWindowSize(int,int);
void glutInitDisplayMode(unsigned);
int glutCreateWindow(const char *);
void glutDestroyWindow(int);
void glutDisplayFunc(void (*)(void));
void glutReshapeFunc(void (*)(int,int));
void glutKeyboardFunc(void (*)(unsigned char,int,int));
void glutSpecialFunc(void (*)(int,int,int));
void glutVisibilityFunc(void (*)(int));
void glutIdleFunc(void (*)(void));
void glutMouseFunc(void (*)(int,int,int,int));
void glutMotionFunc(void (*)(int,int));
void glutPostRedisplay(void);
void glutSwapBuffers(void);
void glutMainLoop(void);
int glutGet(unsigned);
int glutExtensionSupported(const char *);
void glutFullScreen(void);
int glutCreateMenu(void (*)(int));
void glutAddMenuEntry(const char *,int);
void glutAttachMenu(int);
void glutSolidTeapot(GLdouble);
void glutWireTeapot(GLdouble);
#endif

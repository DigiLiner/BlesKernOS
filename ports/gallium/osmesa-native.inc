/* SPDX-License-Identifier: MIT
 * Native presentation, independent of X11/Wine. Included in the OSMesa
 * frontend of the pinned upstream Mesa after the shared frontend patch. */
extern bool bles_virgl_present_resource(struct pipe_resource *);
extern int bles_virgl_native_event(struct pipe_resource *);
extern bool bles_virgl_native_ready(struct pipe_resource *);
GLAPI GLint GLAPIENTRY OSMesaBleskNativeReady(OSMesaContext);
GLAPI GLint GLAPIENTRY OSMesaBleskNativeReady(OSMesaContext c)
{
   if (!c || !c->current_buffer) return 0;
   struct pipe_resource *front=c->current_buffer->textures[ST_ATTACHMENT_FRONT_LEFT];
   return front && bles_virgl_native_ready(front);
}
GLAPI GLint GLAPIENTRY OSMesaBleskNativeEvent(OSMesaContext);
GLAPI GLint GLAPIENTRY OSMesaBleskNativeEvent(OSMesaContext c)
{
   if (!c || !c->current_buffer) return 0;
   struct pipe_resource *front=c->current_buffer->textures[ST_ATTACHMENT_FRONT_LEFT];
   return front?bles_virgl_native_event(front):0;
}
GLAPI GLboolean GLAPIENTRY OSMesaBleskNativeMode(OSMesaContext, GLboolean);
GLAPI GLboolean GLAPIENTRY
OSMesaBleskNativeMode(OSMesaContext c, GLboolean enabled)
{
   if (!c) return GL_FALSE;
   c->native_present = !!enabled;
   return GL_TRUE;
}
GLAPI GLboolean GLAPIENTRY OSMesaBleskPresentNative(OSMesaContext);
GLAPI GLboolean GLAPIENTRY OSMesaBleskPresentNative(OSMesaContext c)
{
   if (!c || c != OSMesaGetCurrentContext() || !c->native_present || !c->current_buffer)
      return GL_FALSE;
   struct bles_swap swap = {c, false};
   st_context_flush(c->st, ST_FLUSH_END_OF_FRAME, NULL, osmesa_bles_copy_front, &swap);
   if (!swap.ok) return GL_FALSE;
   struct pipe_resource *front = c->current_buffer->textures[ST_ATTACHMENT_FRONT_LEFT];
   return front && bles_virgl_present_resource(front);
}

extern int bles_virgl_native_poll(struct pipe_resource *, void *);
GLAPI GLint GLAPIENTRY OSMesaBleskNativePoll(OSMesaContext, void *);
GLAPI GLint GLAPIENTRY OSMesaBleskNativePoll(OSMesaContext c, void *event)
{
   if (!c || !c->current_buffer || !event) return 0;
   struct pipe_resource *front=c->current_buffer->textures[ST_ATTACHMENT_FRONT_LEFT];
   return front?bles_virgl_native_poll(front,event):0;
}

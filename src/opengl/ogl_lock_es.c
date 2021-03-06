/*         ______   ___    ___
 *        /\  _  \ /\_ \  /\_ \
 *        \ \ \L\ \\//\ \ \//\ \      __     __   _ __   ___
 *         \ \  __ \ \ \ \  \ \ \   /'__`\ /'_ `\/\`'__\/ __`\
 *          \ \ \/\ \ \_\ \_ \_\ \_/\  __//\ \L\ \ \ \//\ \L\ \
 *           \ \_\ \_\/\____\/\____\ \____\ \____ \ \_\\ \____/
 *            \/_/\/_/\/____/\/____/\/____/\/___L\ \/_/ \/___/
 *                                           /\____/
 *                                           \_/__/
 *
 *      OpenGL bitmap locking (GLES).
 *
 *      See LICENSE.txt for copyright information.
 */

#include "allegro5/allegro.h"
#include "allegro5/allegro_opengl.h"
#include "allegro5/internal/aintern.h"
#include "allegro5/internal/aintern_opengl.h"
#include "allegro5/internal/aintern_pixels.h"

#if defined ALLEGRO_ANDROID
   #include "allegro5/internal/aintern_android.h"
#endif

#include "ogl_helpers.h"

/*
 * This is the GLES implementation of ogl_lock_region and ogl_unlock_region.
 * The version for desktop GL is in ogl_lock.c.  They are pretty similar again
 * so probably could consider unifying them again.
 */
#if defined(ALLEGRO_CFG_OPENGLES)

ALLEGRO_DEBUG_CHANNEL("opengl")

#define get_glformat(f, c) _al_ogl_get_glformat((f), (c))


/* Helper to get smallest fitting power of two. */
static int pot(int x)
{
   int y = 1;
   while (y < x) y *= 2;
   return y;
}


/*
 * Helpers - duplicates code in ogl_bitmap.c for now
 */

static int ogl_pixel_alignment(int pixel_size)
{
   /* Valid alignments are: 1, 2, 4, 8 bytes. */
   switch (pixel_size) {
      case 1:
      case 2:
      case 4:
      case 8:
         return pixel_size;
      case 3:
         return 1;
      case 16: /* float32 */
         return 4;
      default:
         ASSERT(false);
         return 4;
   }
}

static int ogl_pitch(int w, int pixel_size)
{
   int pitch = w * pixel_size;
   return pitch;
}



/*
 * Locking
 */

static ALLEGRO_LOCKED_REGION *ogl_lock_region_bb_readonly(
   ALLEGRO_BITMAP *bitmap, int x, int y, int w, int h, int real_format);
static ALLEGRO_LOCKED_REGION *ogl_lock_region_bb_proxy(ALLEGRO_BITMAP *bitmap,
   int x, int y, int w, int h, int real_format, int flags);
static ALLEGRO_LOCKED_REGION *ogl_lock_region_nonbb(ALLEGRO_BITMAP *bitmap,
   int x, int y, int w, int h, int real_format, int flags);
static bool ogl_lock_region_nonbb_writeonly(
   ALLEGRO_BITMAP *bitmap, ALLEGRO_BITMAP_EXTRA_OPENGL *ogl_bitmap,
   int x, int gl_y, int w, int h, int real_format);
static bool ogl_lock_region_nonbb_readwrite(
   ALLEGRO_BITMAP *bitmap, ALLEGRO_BITMAP_EXTRA_OPENGL *ogl_bitmap,
   int x, int gl_y, int w, int h, int real_format);
static bool ogl_lock_region_nonbb_readwrite_fbo(
   ALLEGRO_BITMAP *bitmap, ALLEGRO_BITMAP_EXTRA_OPENGL *ogl_bitmap,
   int x, int gl_y, int w, int h, int real_format);


ALLEGRO_LOCKED_REGION *_al_ogl_lock_region_gles(ALLEGRO_BITMAP *bitmap,
   int x, int y, int w, int h, int format, int flags)
{
   ALLEGRO_BITMAP_EXTRA_OPENGL * const ogl_bitmap = bitmap->extra;
   ALLEGRO_DISPLAY *disp;
   int real_format;

   if (format == ALLEGRO_PIXEL_FORMAT_ANY) {
      /* Never pick compressed formats with ANY, as it interacts weirdly with
       * existing code (e.g. al_get_pixel_size() etc) */
      int bitmap_format = al_get_bitmap_format(bitmap);
      if (_al_pixel_format_is_compressed(bitmap_format)) {
         // XXX Get a good format from the driver?
         format = ALLEGRO_PIXEL_FORMAT_ABGR_8888_LE;
      }
      else {
         format = bitmap_format;
      }
   }

   disp = al_get_current_display();
   real_format = _al_get_real_pixel_format(disp, format);

   if (ogl_bitmap->is_backbuffer) {
      if (flags & ALLEGRO_LOCK_READONLY) {
         return ogl_lock_region_bb_readonly(bitmap, x, y, w, h, real_format);
      }
      else {
         return ogl_lock_region_bb_proxy(bitmap, x, y, w, h, real_format,
            flags);
      }
   }
   else {
      return ogl_lock_region_nonbb(bitmap, x, y, w, h, real_format, flags);
   }
}


static ALLEGRO_LOCKED_REGION *ogl_lock_region_bb_readonly(
   ALLEGRO_BITMAP *bitmap, int x, int y, int w, int h, int real_format)
{
   ALLEGRO_BITMAP_EXTRA_OPENGL * const ogl_bitmap = bitmap->extra;
   const int pixel_size = al_get_pixel_size(real_format);
   const int pitch = ogl_pitch(w, pixel_size);
   const int gl_y = bitmap->h - y - h;
   GLenum e;

   ogl_bitmap->lock_buffer = al_malloc(pitch * h);
   if (ogl_bitmap->lock_buffer == NULL) {
      ALLEGRO_ERROR("Out of memory\n");
      return false;
   }

   glReadPixels(x, gl_y, w, h,
      get_glformat(real_format, 2),
      get_glformat(real_format, 1),
      ogl_bitmap->lock_buffer);
   e = glGetError();
   if (e) {
      ALLEGRO_ERROR("glReadPixels for format %s failed (%s).\n",
         _al_pixel_format_name(real_format), _al_gl_error_string(e));
      al_free(ogl_bitmap->lock_buffer);
      ogl_bitmap->lock_buffer = NULL;
      return false;
   }

   bitmap->locked_region.data = ogl_bitmap->lock_buffer + pitch * (h - 1);
   bitmap->locked_region.format = real_format;
   bitmap->locked_region.pitch = -pitch;
   bitmap->locked_region.pixel_size = pixel_size;
   return &bitmap->locked_region;
}


static ALLEGRO_LOCKED_REGION *ogl_lock_region_bb_proxy(ALLEGRO_BITMAP *bitmap,
   int x, int y, int w, int h, int real_format, int flags)
{
   ALLEGRO_BITMAP_EXTRA_OPENGL * const ogl_bitmap = bitmap->extra;
   ALLEGRO_BITMAP *proxy;
   ALLEGRO_LOCKED_REGION *lr;

   ALLEGRO_DEBUG("Creating backbuffer proxy bitmap\n");
   proxy = _al_create_bitmap_params(al_get_current_display(),
      w, h, real_format, ALLEGRO_VIDEO_BITMAP|ALLEGRO_NO_PRESERVE_TEXTURE);
   if (!proxy) {
      return NULL;
   }

   ALLEGRO_DEBUG("Locking backbuffer proxy bitmap\n");
   proxy->lock_x = 0;
   proxy->lock_y = 0;
   proxy->lock_w = w;
   proxy->lock_h = h;
   proxy->lock_flags = flags;
   lr = ogl_lock_region_nonbb(proxy, 0, 0, w, h, real_format, flags);
   if (!lr) {
      al_destroy_bitmap(proxy);
      return NULL;
   }

   if (!(flags & ALLEGRO_LOCK_WRITEONLY)) {
      ALLEGRO_BITMAP_EXTRA_OPENGL *ogl_proxy = proxy->extra;
      const int gl_y = bitmap->h - y - h;
      GLenum e;

      glReadPixels(x, gl_y, w, h,
         get_glformat(real_format, 2),
         get_glformat(real_format, 1),
         ogl_proxy->lock_buffer);
      e = glGetError();
      if (e) {
         ALLEGRO_ERROR("glReadPixels for format %s failed (%s).\n",
            _al_pixel_format_name(real_format), _al_gl_error_string(e));
         al_destroy_bitmap(proxy);
         return NULL;
      }
   }

   proxy->locked = true;
   bitmap->locked_region = proxy->locked_region;
   ogl_bitmap->lock_proxy = proxy;
   return lr;
}


static ALLEGRO_LOCKED_REGION *ogl_lock_region_nonbb(ALLEGRO_BITMAP *bitmap,
   int x, int y, int w, int h, int real_format, int flags)
{
   ALLEGRO_BITMAP_EXTRA_OPENGL * const ogl_bitmap = bitmap->extra;
   const int gl_y = bitmap->h - y - h;
   ALLEGRO_DISPLAY *disp;
   ALLEGRO_DISPLAY *old_disp = NULL;
   bool ok;

   disp = al_get_current_display();

   /* Change OpenGL context if necessary. */
   if (!disp ||
      (_al_get_bitmap_display(bitmap)->ogl_extras->is_shared == false &&
       _al_get_bitmap_display(bitmap) != disp))
   {
      old_disp = disp;
      _al_set_current_display_only(_al_get_bitmap_display(bitmap));
   }

   ok = true;

   /* Set up the pixel store state.  We will need to match it when unlocking.
    * There may be other pixel store state we should be setting.
    * See also pitfalls 7 & 8 from:
    * http://www.opengl.org/resources/features/KilgardTechniques/oglpitfall/
    */
   {
      const int pixel_size = al_get_pixel_size(real_format);
      const int pixel_alignment = ogl_pixel_alignment(pixel_size);
      GLenum e;
      glPixelStorei(GL_PACK_ALIGNMENT, pixel_alignment);
      e = glGetError();
      if (e) {
         ALLEGRO_ERROR("glPixelStorei(GL_PACK_ALIGNMENT, %d) failed (%s).\n",
            pixel_alignment, _al_gl_error_string(e));
         ok = false;
      }
   }

   if (ok) {
      if (flags & ALLEGRO_LOCK_WRITEONLY) {
         ALLEGRO_DEBUG("Locking non-backbuffer WRITEONLY\n");
         ok = ogl_lock_region_nonbb_writeonly(bitmap, ogl_bitmap,
            x, gl_y, w, h, real_format);
      }
      else {
         ALLEGRO_DEBUG("Locking non-backbuffer %s\n",
            (flags & ALLEGRO_LOCK_READONLY) ? "READONLY" : "READWRITE");
         ok = ogl_lock_region_nonbb_readwrite(bitmap, ogl_bitmap,
            x, gl_y, w, h, real_format);
      }
   }

   if (old_disp != NULL) {
      _al_set_current_display_only(old_disp);
   }

   if (ok) {
      return &bitmap->locked_region;
   }

   ALLEGRO_ERROR("Failed to lock region\n");
   ASSERT(ogl_bitmap->lock_buffer == NULL);
   return NULL;
}


static bool ogl_lock_region_nonbb_writeonly(
   ALLEGRO_BITMAP *bitmap, ALLEGRO_BITMAP_EXTRA_OPENGL *ogl_bitmap,
   int x, int gl_y, int w, int h, int real_format)
{
   const int pixel_size = al_get_pixel_size(real_format);
   const int pitch = ogl_pitch(w, pixel_size);
   (void) x;
   (void) gl_y;

   ogl_bitmap->lock_buffer = al_malloc(pitch * h);
   if (ogl_bitmap->lock_buffer == NULL) {
      return false;
   }

   bitmap->locked_region.data = ogl_bitmap->lock_buffer + pitch * (h - 1);
   bitmap->locked_region.format = real_format;
   bitmap->locked_region.pitch = -pitch;
   bitmap->locked_region.pixel_size = pixel_size;

   // Not sure why this is needed on Pi
   if (IS_RASPBERRYPI) {
      glFlush();
   }

   return true;
}


static bool ogl_lock_region_nonbb_readwrite(
   ALLEGRO_BITMAP *bitmap, ALLEGRO_BITMAP_EXTRA_OPENGL *ogl_bitmap,
   int x, int gl_y, int w, int h, int real_format)
{
   ALLEGRO_BITMAP *old_target;
   bool fbo_was_set;
   bool ok;

   ASSERT(bitmap->parent == NULL);
   ASSERT(bitmap->locked == false);
   ASSERT(_al_get_bitmap_display(bitmap) == al_get_current_display());

   /* Try to create an FBO if there isn't one. */
   old_target = al_get_target_bitmap();
   fbo_was_set =
      _al_ogl_setup_fbo_non_backbuffer(_al_get_bitmap_display(bitmap), bitmap);

   /* Unlike in desktop GL, there seems to be nothing we can do without an FBO. */
   if (fbo_was_set && ogl_bitmap->fbo_info) {
      ok = ogl_lock_region_nonbb_readwrite_fbo(bitmap, ogl_bitmap,
         x, gl_y, w, h, real_format);
   }
   else {
      ALLEGRO_ERROR("no fbo\n");
      ok = false;
   }

   /* Restore state after switching FBO. */
   if (fbo_was_set) {
      if (!old_target) {
         /* Old target was NULL; release the context. */
         _al_set_current_display_only(NULL);
      }
      else if (!_al_get_bitmap_display(old_target)) {
         /* Old target was memory bitmap; leave the current display alone. */
      }
      else if (old_target != bitmap) {
         /* Old target was another OpenGL bitmap. */
         _al_ogl_setup_fbo(_al_get_bitmap_display(old_target), old_target);
      }
   }

   ASSERT(al_get_target_bitmap() == old_target);

   return ok;
}


static bool ogl_lock_region_nonbb_readwrite_fbo(
   ALLEGRO_BITMAP *bitmap, ALLEGRO_BITMAP_EXTRA_OPENGL *ogl_bitmap,
   int x, int gl_y, int w, int h, int real_format)
{
   const int pixel_size = al_get_pixel_size(real_format);
   const int pitch = ogl_pitch(w, pixel_size);
   const int start_h = h;
   GLint old_fbo;
   GLenum e;
   bool ok;

   ASSERT(ogl_bitmap->fbo_info);

   old_fbo = _al_ogl_bind_framebuffer(ogl_bitmap->fbo_info->fbo);
   e = glGetError();
   if (e) {
      ALLEGRO_ERROR("glBindFramebufferEXT failed (%s).\n",
         _al_gl_error_string(e));
      return false;
   }

   ok = true;

   /* Allocate a buffer big enough for both purposes. This requires more
    * memory to be held for the period of the lock, but overall less
    * memory is needed to complete the lock.
    */
   if (ok) {
      size_t size = _ALLEGRO_MAX(pitch * h, ogl_pitch(w, 4) * h);
      ogl_bitmap->lock_buffer = al_malloc(size);
      if (ogl_bitmap->lock_buffer == NULL) {
         ok = false;
      }
   }

   if (ok) {
      /* NOTE: GLES (1.1?) can only read 4 byte pixels, we have to convert */
      glReadPixels(x, gl_y, w, h,
         GL_RGBA, GL_UNSIGNED_BYTE,
         ogl_bitmap->lock_buffer);
      e = glGetError();
      if (e) {
         ALLEGRO_ERROR("glReadPixels for format %s failed (%s).\n",
            _al_pixel_format_name(real_format), _al_gl_error_string(e));
         al_free(ogl_bitmap->lock_buffer);
         ogl_bitmap->lock_buffer = NULL;
         ok = false;
      }
   }

   if (ok) {
      ALLEGRO_DEBUG("Converting from format %d -> %d\n",
         ALLEGRO_PIXEL_FORMAT_ABGR_8888_LE, real_format);

      /* That's right, we convert in-place.
       * (safe as long as dst size <= src size, which it always is)
       */
      _al_convert_bitmap_data(ogl_bitmap->lock_buffer,
         ALLEGRO_PIXEL_FORMAT_ABGR_8888_LE,
         ogl_pitch(w, 4),
         ogl_bitmap->lock_buffer,
         real_format,
         pitch,
         0, 0, 0, 0,
         w, h);

      bitmap->locked_region.data = ogl_bitmap->lock_buffer + pitch * (start_h - 1);
      bitmap->locked_region.format = real_format;
      bitmap->locked_region.pitch = -pitch;
      bitmap->locked_region.pixel_size = pixel_size;
      ok = true;
   }

   _al_ogl_bind_framebuffer(old_fbo);

   return ok;
}



/*
 * Unlocking
 */

static void ogl_unlock_region_bb_proxy(ALLEGRO_BITMAP *bitmap,
   ALLEGRO_BITMAP_EXTRA_OPENGL *ogl_bitmap);
static void ogl_unlock_region_nonbb(ALLEGRO_BITMAP *bitmap,
   ALLEGRO_BITMAP_EXTRA_OPENGL *ogl_bitmap);
static void ogl_unlock_region_nonbb_2(ALLEGRO_BITMAP *bitmap,
   ALLEGRO_BITMAP_EXTRA_OPENGL *ogl_bitmap, int gl_y, int orig_format);
static void ogl_unlock_region_nonbb_nonfbo_conv(ALLEGRO_BITMAP *bitmap,
   ALLEGRO_BITMAP_EXTRA_OPENGL *ogl_bitmap, int gl_y, int orig_format);
static void ogl_unlock_region_nonbb_nonfbo_noconv(ALLEGRO_BITMAP *bitmap,
   ALLEGRO_BITMAP_EXTRA_OPENGL *ogl_bitmap, int gl_y, int orig_format);


void _al_ogl_unlock_region_gles(ALLEGRO_BITMAP *bitmap)
{
   ALLEGRO_BITMAP_EXTRA_OPENGL *ogl_bitmap = bitmap->extra;

   if (bitmap->lock_flags & ALLEGRO_LOCK_READONLY) {
      ALLEGRO_DEBUG("Unlocking READONLY\n");
      ASSERT(ogl_bitmap->lock_proxy == NULL);
   }
   else if (ogl_bitmap->lock_proxy != NULL) {
      ogl_unlock_region_bb_proxy(bitmap, ogl_bitmap);
   }
   else {
      ogl_unlock_region_nonbb(bitmap, ogl_bitmap);
   }

   al_free(ogl_bitmap->lock_buffer);
   ogl_bitmap->lock_buffer = NULL;
}


static void ogl_unlock_region_bb_proxy(ALLEGRO_BITMAP *bitmap,
   ALLEGRO_BITMAP_EXTRA_OPENGL *ogl_bitmap)
{
   ALLEGRO_BITMAP *proxy = ogl_bitmap->lock_proxy;

   ASSERT(proxy);
   ASSERT(ogl_bitmap->lock_buffer == NULL);

   ALLEGRO_DEBUG("Unlocking backbuffer proxy bitmap\n");
   _al_ogl_unlock_region_gles(proxy);
   proxy->locked = false;

   ALLEGRO_DEBUG("Drawing proxy to backbuffer\n");
   {
      ALLEGRO_DISPLAY *disp;
      ALLEGRO_TRANSFORM proj0;
      ALLEGRO_STATE state0;
      ALLEGRO_TRANSFORM t;
      bool held;

      disp = al_get_current_display();
      held = al_is_bitmap_drawing_held();
      if (held) {
         al_hold_bitmap_drawing(false);
      }
      /* Projection transform is currently per-display. */
      al_copy_transform(&proj0, al_get_projection_transform(disp));
      al_store_state(&state0, ALLEGRO_STATE_TARGET_BITMAP |
         ALLEGRO_STATE_TRANSFORM | ALLEGRO_STATE_BLENDER);
      {
         al_set_target_bitmap(bitmap);
         al_identity_transform(&t);
         al_use_transform(&t);
         al_orthographic_transform(&t, 0, 0, -1, disp->w, disp->h, 1);
         al_set_projection_transform(disp, &t);
         al_set_blender(ALLEGRO_ADD, ALLEGRO_ONE, ALLEGRO_ZERO);
         al_draw_bitmap(proxy, bitmap->lock_x, bitmap->lock_y, 0);
      }
      al_restore_state(&state0);
      al_set_projection_transform(disp, &proj0);
      al_hold_bitmap_drawing(held);
   }

   ALLEGRO_DEBUG("Destroying backbuffer proxy bitmap\n");
   al_destroy_bitmap(proxy);
   ogl_bitmap->lock_proxy = NULL;
}


static void ogl_unlock_region_nonbb(ALLEGRO_BITMAP *bitmap,
   ALLEGRO_BITMAP_EXTRA_OPENGL *ogl_bitmap)
{
   const int gl_y = bitmap->h - bitmap->lock_y - bitmap->lock_h;
   ALLEGRO_DISPLAY *old_disp = NULL;
   ALLEGRO_DISPLAY *disp;
   int orig_format;
   GLenum e;

   disp = al_get_current_display();
   orig_format = _al_get_real_pixel_format(disp, al_get_bitmap_format(bitmap));

   /* Change OpenGL context if necessary. */
   if (!disp ||
      (_al_get_bitmap_display(bitmap)->ogl_extras->is_shared == false &&
       _al_get_bitmap_display(bitmap) != disp))
   {
      old_disp = disp;
      _al_set_current_display_only(_al_get_bitmap_display(bitmap));
   }

   /* Desktop code sets GL_UNPACK_ALIGNMENT here instead of later. */

   ogl_unlock_region_nonbb_2(bitmap, ogl_bitmap, gl_y, orig_format);

   /* If using FBOs, we need to regenerate mipmaps explicitly now. */
   /* XXX why don't we check ogl_bitmap->fbo_info? */
   if ((al_get_bitmap_flags(bitmap) & ALLEGRO_MIPMAP) &&
      al_get_opengl_extension_list()->ALLEGRO_GL_EXT_framebuffer_object)
   {
      glGenerateMipmapEXT(GL_TEXTURE_2D);
      e = glGetError();
      if (e) {
         ALLEGRO_ERROR("glGenerateMipmapEXT for texture %d failed (%s).\n",
            ogl_bitmap->texture, _al_gl_error_string(e));
      }
   }

   if (old_disp) {
      _al_set_current_display_only(old_disp);
   }
}


static void ogl_unlock_region_nonbb_2(ALLEGRO_BITMAP *bitmap,
   ALLEGRO_BITMAP_EXTRA_OPENGL *ogl_bitmap, int gl_y, int orig_format)
{
   GLint fbo;
   GLenum e;

#ifdef ALLEGRO_ANDROID
   fbo = _al_android_get_curr_fbo();
#else
   glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &fbo);
#endif
   glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
#ifdef ALLEGRO_ANDROID
   _al_android_set_curr_fbo(0);
#endif

   glBindTexture(GL_TEXTURE_2D, ogl_bitmap->texture);
   e = glGetError();
   if (e) {
      ALLEGRO_ERROR("glBindTexture failed (%s).\n", _al_gl_error_string(e));
   }

   /* Differs from desktop code. */
   ALLEGRO_DEBUG("Unlocking non-backbuffer (non-FBO)\n");
   if (bitmap->locked_region.format != orig_format) {
      ALLEGRO_DEBUG(
         "Unlocking non-backbuffer non-FBO with conversion (%d -> %d)\n",
         bitmap->locked_region.format, orig_format);
      ogl_unlock_region_nonbb_nonfbo_conv(bitmap, ogl_bitmap, gl_y,
         orig_format);
   }
   else {
      ALLEGRO_DEBUG("Unlocking non-backbuffer non-FBO without conversion\n");
      ogl_unlock_region_nonbb_nonfbo_noconv(bitmap, ogl_bitmap, gl_y,
         orig_format);
   }

   glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
#ifdef ALLEGRO_ANDROID
   _al_android_set_curr_fbo(fbo);
#endif
}


static void ogl_unlock_region_nonbb_nonfbo_conv(ALLEGRO_BITMAP *bitmap,
   ALLEGRO_BITMAP_EXTRA_OPENGL *ogl_bitmap, int gl_y, int orig_format)
{
   const int lock_format = bitmap->locked_region.format;
   const int orig_pixel_size = al_get_pixel_size(orig_format);
   const int dst_pitch = bitmap->lock_w * orig_pixel_size;
   unsigned char * const tmpbuf = al_malloc(dst_pitch * bitmap->lock_h);
   GLenum e;

   _al_convert_bitmap_data(
      ogl_bitmap->lock_buffer,
      bitmap->locked_region.format,
      -bitmap->locked_region.pitch,
      tmpbuf,
      orig_format,
      dst_pitch,
      0, 0, 0, 0,
      bitmap->lock_w, bitmap->lock_h);

   glPixelStorei(GL_UNPACK_ALIGNMENT, ogl_pixel_alignment(orig_pixel_size));

   glTexSubImage2D(GL_TEXTURE_2D, 0,
      bitmap->lock_x, gl_y,
      bitmap->lock_w, bitmap->lock_h,
      get_glformat(orig_format, 2),
      get_glformat(orig_format, 1),
      tmpbuf);
   e = glGetError();
   if (e) {
      ALLEGRO_ERROR("glTexSubImage2D for format %d failed (%s).\n",
         lock_format, _al_gl_error_string(e));
   }

   al_free(tmpbuf);
}


static void ogl_unlock_region_nonbb_nonfbo_noconv(ALLEGRO_BITMAP *bitmap,
   ALLEGRO_BITMAP_EXTRA_OPENGL *ogl_bitmap, int gl_y, int orig_format)
{
   const int lock_format = bitmap->locked_region.format;
   const int orig_pixel_size = al_get_pixel_size(orig_format);
   GLenum e;

   glPixelStorei(GL_UNPACK_ALIGNMENT, ogl_pixel_alignment(orig_pixel_size));
   e = glGetError();
   if (e) {
      ALLEGRO_ERROR("glPixelStorei for format %s failed (%s).\n",
         _al_pixel_format_name(lock_format), _al_gl_error_string(e));
   }

   glTexSubImage2D(GL_TEXTURE_2D, 0,
      bitmap->lock_x, gl_y,
      bitmap->lock_w, bitmap->lock_h,
      get_glformat(lock_format, 2),
      get_glformat(lock_format, 1),
      ogl_bitmap->lock_buffer);
   e = glGetError();
   if (e) {
      ALLEGRO_ERROR("glTexSubImage2D for format %s failed (%s).\n",
         _al_pixel_format_name(lock_format), _al_gl_error_string(e));
   }
}


#endif

/* vim: set sts=3 sw=3 et: */

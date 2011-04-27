/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2008  Brian Paul   All Rights Reserved.
 * Copyright (C) 2009-2011  VMware, Inc.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
 * AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */


/**
 * \file pbo.c
 * \brief Functions related to Pixel Buffer Objects.
 */



#include "glheader.h"
#include "bufferobj.h"
#include "image.h"
#include "imports.h"
#include "mtypes.h"
#include "pbo.h"



/**
 * When we're about to read pixel data out of a PBO (via glDrawPixels,
 * glTexImage, etc) or write data into a PBO (via glReadPixels,
 * glGetTexImage, etc) we call this function to check that we're not
 * going to read/write out of bounds.
 *
 * XXX This would also be a convenient time to check that the PBO isn't
 * currently mapped.  Whoever calls this function should check for that.
 * Remember, we can't use a PBO when it's mapped!
 *
 * If we're not using a PBO, this is a no-op.
 *
 * \param width  width of image to read/write
 * \param height  height of image to read/write
 * \param depth  depth of image to read/write
 * \param format  format of image to read/write
 * \param type  datatype of image to read/write
 * \param clientMemSize  the maximum number of bytes to read/write
 * \param ptr  the user-provided pointer/offset
 * \return GL_TRUE if the buffer access is OK, GL_FALSE if the access would
 *         go out of bounds.
 */
GLboolean
_mesa_validate_pbo_access(GLuint dimensions,
                          const struct gl_pixelstore_attrib *pack,
                          GLsizei width, GLsizei height, GLsizei depth,
                          GLenum format, GLenum type, GLsizei clientMemSize,
                          const GLvoid *ptr)
{
   const GLvoid *start, *end, *offset;
   const GLubyte *sizeAddr; /* buffer size, cast to a pointer */

   /* If no PBO is bound, 'ptr' is a pointer to client memory containing
      'clientMemSize' bytes.
      If a PBO is bound, 'ptr' is an offset into the bound PBO.
      In that case 'clientMemSize' is ignored: we just use the PBO's size.
    */
   if (!_mesa_is_bufferobj(pack->BufferObj)) {
      offset = 0;
      sizeAddr = ((const GLubyte *) 0) + clientMemSize;
   } else {
      offset = ptr;
      sizeAddr = ((const GLubyte *) 0) + pack->BufferObj->Size;
   }

   if (sizeAddr == 0)
      /* no buffer! */
      return GL_FALSE;

   /* get the offset to the first pixel we'll read/write */
   start = _mesa_image_address(dimensions, pack, offset, width, height,
                               format, type, 0, 0, 0);

   /* get the offset to just past the last pixel we'll read/write */
   end =  _mesa_image_address(dimensions, pack, offset, width, height,
                              format, type, depth-1, height-1, width);

   if ((const GLubyte *) start > sizeAddr) {
      /* This will catch negative values / wrap-around */
      return GL_FALSE;
   }
   if ((const GLubyte *) end > sizeAddr) {
      /* Image read/write goes beyond end of buffer */
      return GL_FALSE;
   }

   /* OK! */
   return GL_TRUE;
}


/**
 * For commands that read from a PBO (glDrawPixels, glTexImage,
 * glPolygonStipple, etc), if we're reading from a PBO, map it read-only
 * and return the pointer into the PBO.  If we're not reading from a
 * PBO, return \p src as-is.
 * If non-null return, must call _mesa_unmap_pbo_source() when done.
 *
 * \return NULL if error, else pointer to start of data
 */
const GLvoid *
_mesa_map_pbo_source(struct gl_context *ctx,
                     const struct gl_pixelstore_attrib *unpack,
                     const GLvoid *src)
{
   const GLubyte *buf;

   if (_mesa_is_bufferobj(unpack->BufferObj)) {
      /* unpack from PBO */
      buf = (GLubyte *) ctx->Driver.MapBuffer(ctx, GL_PIXEL_UNPACK_BUFFER_EXT,
                                              GL_READ_ONLY_ARB,
                                              unpack->BufferObj);
      if (!buf)
         return NULL;

      buf = ADD_POINTERS(buf, src);
   }
   else {
      /* unpack from normal memory */
      buf = src;
   }

   return buf;
}


/**
 * Combine PBO-read validation and mapping.
 * If any GL errors are detected, they'll be recorded and NULL returned.
 * \sa _mesa_validate_pbo_access
 * \sa _mesa_map_pbo_source
 * A call to this function should have a matching call to
 * _mesa_unmap_pbo_source().
 */
const GLvoid *
_mesa_map_validate_pbo_source(struct gl_context *ctx,
                                 GLuint dimensions,
                                 const struct gl_pixelstore_attrib *unpack,
                                 GLsizei width, GLsizei height, GLsizei depth,
                                 GLenum format, GLenum type, GLsizei clientMemSize,
                                 const GLvoid *ptr, const char *where)
{
   ASSERT(dimensions == 1 || dimensions == 2 || dimensions == 3);

   if (!_mesa_validate_pbo_access(dimensions, unpack, width, height, depth,
                                  format, type, clientMemSize, ptr)) {
      if (_mesa_is_bufferobj(unpack->BufferObj)) {
         _mesa_error(ctx, GL_INVALID_OPERATION,
                     "%s(out of bounds PBO access)", where);
      } else {
         _mesa_error(ctx, GL_INVALID_OPERATION,
                     "%s(out of bounds access: bufSize (%d) is too small)",
                     where, clientMemSize);
      }
      return NULL;
   }

   if (!_mesa_is_bufferobj(unpack->BufferObj)) {
      /* non-PBO access: no further validation to be done */
      return ptr;
   }

   if (_mesa_bufferobj_mapped(unpack->BufferObj)) {
      /* buffer is already mapped - that's an error */
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(PBO is mapped)", where);
      return NULL;
   }

   ptr = _mesa_map_pbo_source(ctx, unpack, ptr);
   return ptr;
}


/**
 * Counterpart to _mesa_map_pbo_source()
 */
void
_mesa_unmap_pbo_source(struct gl_context *ctx,
                       const struct gl_pixelstore_attrib *unpack)
{
   ASSERT(unpack != &ctx->Pack); /* catch pack/unpack mismatch */
   if (_mesa_is_bufferobj(unpack->BufferObj)) {
      ctx->Driver.UnmapBuffer(ctx, GL_PIXEL_UNPACK_BUFFER_EXT,
                              unpack->BufferObj);
   }
}


/**
 * For commands that write to a PBO (glReadPixels, glGetColorTable, etc),
 * if we're writing to a PBO, map it write-only and return the pointer
 * into the PBO.  If we're not writing to a PBO, return \p dst as-is.
 * If non-null return, must call _mesa_unmap_pbo_dest() when done.
 *
 * \return NULL if error, else pointer to start of data
 */
void *
_mesa_map_pbo_dest(struct gl_context *ctx,
                   const struct gl_pixelstore_attrib *pack,
                   GLvoid *dest)
{
   void *buf;

   if (_mesa_is_bufferobj(pack->BufferObj)) {
      /* pack into PBO */
      buf = (GLubyte *) ctx->Driver.MapBuffer(ctx, GL_PIXEL_PACK_BUFFER_EXT,
                                              GL_WRITE_ONLY_ARB,
                                              pack->BufferObj);
      if (!buf)
         return NULL;

      buf = ADD_POINTERS(buf, dest);
   }
   else {
      /* pack to normal memory */
      buf = dest;
   }

   return buf;
}


/**
 * Combine PBO-write validation and mapping.
 * If any GL errors are detected, they'll be recorded and NULL returned.
 * \sa _mesa_validate_pbo_access
 * \sa _mesa_map_pbo_dest
 * A call to this function should have a matching call to
 * _mesa_unmap_pbo_dest().
 */
GLvoid *
_mesa_map_validate_pbo_dest(struct gl_context *ctx,
                               GLuint dimensions,
                               const struct gl_pixelstore_attrib *unpack,
                               GLsizei width, GLsizei height, GLsizei depth,
                               GLenum format, GLenum type, GLsizei clientMemSize,
                               GLvoid *ptr, const char *where)
{
   ASSERT(dimensions == 1 || dimensions == 2 || dimensions == 3);

   if (!_mesa_validate_pbo_access(dimensions, unpack, width, height, depth,
                                  format, type, clientMemSize, ptr)) {
      if (_mesa_is_bufferobj(unpack->BufferObj)) {
         _mesa_error(ctx, GL_INVALID_OPERATION,
                     "%s(out of bounds PBO access)", where);
      } else {
         _mesa_error(ctx, GL_INVALID_OPERATION,
                     "%s(out of bounds access: bufSize (%d) is too small)",
                     where, clientMemSize);
      }
      return NULL;
   }

   if (!_mesa_is_bufferobj(unpack->BufferObj)) {
      /* non-PBO access: no further validation to be done */
      return ptr;
   }

   if (_mesa_bufferobj_mapped(unpack->BufferObj)) {
      /* buffer is already mapped - that's an error */
      _mesa_error(ctx, GL_INVALID_OPERATION, "%s(PBO is mapped)", where);
      return NULL;
   }

   ptr = _mesa_map_pbo_dest(ctx, unpack, ptr);
   return ptr;
}


/**
 * Counterpart to _mesa_map_pbo_dest()
 */
void
_mesa_unmap_pbo_dest(struct gl_context *ctx,
                     const struct gl_pixelstore_attrib *pack)
{
   ASSERT(pack != &ctx->Unpack); /* catch pack/unpack mismatch */
   if (_mesa_is_bufferobj(pack->BufferObj)) {
      ctx->Driver.UnmapBuffer(ctx, GL_PIXEL_PACK_BUFFER_EXT, pack->BufferObj);
   }
}


/**
 * Check if an unpack PBO is active prior to fetching a texture image.
 * If so, do bounds checking and map the buffer into main memory.
 * Any errors detected will be recorded.
 * The caller _must_ call _mesa_unmap_teximage_pbo() too!
 */
const GLvoid *
_mesa_validate_pbo_teximage(struct gl_context *ctx, GLuint dimensions,
			    GLsizei width, GLsizei height, GLsizei depth,
			    GLenum format, GLenum type, const GLvoid *pixels,
			    const struct gl_pixelstore_attrib *unpack,
			    const char *funcName)
{
   GLubyte *buf;

   if (!_mesa_is_bufferobj(unpack->BufferObj)) {
      /* no PBO */
      return pixels;
   }
   if (!_mesa_validate_pbo_access(dimensions, unpack, width, height, depth,
                                     format, type, INT_MAX, pixels)) {
      _mesa_error(ctx, GL_INVALID_OPERATION, funcName, "(invalid PBO access)");
      return NULL;
   }

   buf = (GLubyte *) ctx->Driver.MapBuffer(ctx, GL_PIXEL_UNPACK_BUFFER_EXT,
                                          GL_READ_ONLY_ARB, unpack->BufferObj);
   if (!buf) {
      _mesa_error(ctx, GL_INVALID_OPERATION, funcName, "(PBO is mapped)");
      return NULL;
   }

   return ADD_POINTERS(buf, pixels);
}


/**
 * Check if an unpack PBO is active prior to fetching a compressed texture
 * image.
 * If so, do bounds checking and map the buffer into main memory.
 * Any errors detected will be recorded.
 * The caller _must_ call _mesa_unmap_teximage_pbo() too!
 */
const GLvoid *
_mesa_validate_pbo_compressed_teximage(struct gl_context *ctx,
                                 GLsizei imageSize, const GLvoid *pixels,
                                 const struct gl_pixelstore_attrib *packing,
                                 const char *funcName)
{
   GLubyte *buf;

   if (!_mesa_is_bufferobj(packing->BufferObj)) {
      /* not using a PBO - return pointer unchanged */
      return pixels;
   }
   if ((const GLubyte *) pixels + imageSize >
       ((const GLubyte *) 0) + packing->BufferObj->Size) {
      /* out of bounds read! */
      _mesa_error(ctx, GL_INVALID_OPERATION, funcName, "(invalid PBO access)");
      return NULL;
   }

   buf = (GLubyte*) ctx->Driver.MapBuffer(ctx, GL_PIXEL_UNPACK_BUFFER_EXT,
                                         GL_READ_ONLY_ARB, packing->BufferObj);
   if (!buf) {
      _mesa_error(ctx, GL_INVALID_OPERATION, funcName, "(PBO is mapped");
      return NULL;
   }

   return ADD_POINTERS(buf, pixels);
}


/**
 * This function must be called after either of the validate_pbo_*_teximage()
 * functions.  It unmaps the PBO buffer if it was mapped earlier.
 */
void
_mesa_unmap_teximage_pbo(struct gl_context *ctx,
                         const struct gl_pixelstore_attrib *unpack)
{
   if (_mesa_is_bufferobj(unpack->BufferObj)) {
      ctx->Driver.UnmapBuffer(ctx, GL_PIXEL_UNPACK_BUFFER_EXT,
                              unpack->BufferObj);
   }
}



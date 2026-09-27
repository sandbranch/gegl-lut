/*
 * Color Lookup: applies a 3D or 1D LUT, a GEGL operation
 *
 * color-lookup.c
 * Copyright 2026 by David
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see
 * <https://www.gnu.org/licenses/>.
 *
 * Applies a color lookup table (LUT) read from a file, as film looks and
 * color grades are shared between Photoshop (Color Lookup adjustment),
 * Resolve, Premiere and phone apps:
 *
 *   - Adobe/Iridas .cube (Cube LUT Specification 1.0: LUT_3D_SIZE,
 *     LUT_1D_SIZE, DOMAIN_MIN, DOMAIN_MAX, TITLE) and the Resolve variant
 *     (LUT_1D_INPUT_RANGE, LUT_3D_INPUT_RANGE, and a 1D "shaper" LUT
 *     followed by a 3D LUT in one file, as Blackmagic describes it and
 *     OpenColorIO's FileFormatResolveCube.cpp documents);
 *   - Autodesk .3dl (Flame and Lustre): integers, the first line is the
 *     input grid, blue changes fastest; the output bit depth comes from a
 *     "Mesh <in> <out>" line or, like OpenColorIO's FileFormat3DL.cpp,
 *     from the largest value;
 *   - Hald CLUT images (PNG, TIFF, ...), in the layout of Eskil
 *     Steenberg's Hald CLUT that G'MIC, RawTherapee, darktable, ImageMagick
 *     and FFmpeg use: level L, an image of L^3 x L^3 pixels holding a cube
 *     of L^2 points per axis, red fastest in raster order.
 *
 * 3D LUTs are interpolated tetrahedrally (the method of the Truelight
 * paper by FilmLight that FFmpeg's vf_lut3d.c credits, written here as a
 * sort of the three fractions) or trilinearly; 1D LUTs linearly. The code
 * is written for this operation; FFmpeg's libavfilter/vf_lut3d.c
 * (LGPL-2.1+), OpenColorIO's file readers (BSD-3-Clause) and darktable's
 * src/iop/lut3d.c (GPL-3) were read for how the formats are used.
 *
 * The LUT is applied to the pixels in the encoding it expects (the image's
 * perceptual curve or linear light) and in the image's color space or in
 * sRGB, chosen with babl formats. Parsed files are cached by path,
 * modification time and size. A file that cannot be used leaves the image
 * as it is, with a warning, and the reason in the "error" property (which
 * GIMP shows in the filter dialog).
 */

#include <glib/gi18n-lib.h>

#ifdef GEGL_PROPERTIES

enum_start (lut_color_lookup_interpolation)
  enum_value (LUT_INTERPOLATION_TETRAHEDRAL, "tetrahedral", N_("Tetrahedral"))
  enum_value (LUT_INTERPOLATION_TRILINEAR,   "trilinear",   N_("Trilinear"))
enum_end (LutColorLookupInterpolation)

enum_start (lut_color_lookup_encoding)
  enum_value (LUT_ENCODING_PERCEPTUAL, "perceptual",
              N_("Perceptual (sRGB-like curve)"))
  enum_value (LUT_ENCODING_LINEAR,     "linear", N_("Linear light"))
enum_end (LutColorLookupEncoding)

enum_start (lut_color_lookup_space)
  enum_value (LUT_SPACE_IMAGE, "image", N_("The image's"))
  enum_value (LUT_SPACE_SRGB,  "srgb",  N_("sRGB"))
enum_end (LutColorLookupSpace)

enum_start (lut_color_lookup_out_of_range)
  enum_value (LUT_OUT_OF_RANGE_CLAMP,       "clamp",       N_("Clamp"))
  enum_value (LUT_OUT_OF_RANGE_EXTRAPOLATE, "extrapolate", N_("Extrapolate"))
enum_end (LutColorLookupOutOfRange)

property_file_path (path, _("LUT file"), "")
  description (_("The lookup table: an Adobe or Resolve .cube file (3D or "
                 "1D), an Autodesk .3dl file, or a Hald CLUT image (PNG or "
                 "TIFF)"))

property_double (strength, _("Strength"), 1.0)
  description (_("How much of the LUT is applied: 0 leaves the image as it "
                 "is, 1 applies the LUT fully. The mix is made in the "
                 "encoding the LUT expects."))
  value_range (0.0, 1.0)
  ui_digits (2)

property_enum (interpolation, _("Interpolation"),
               LutColorLookupInterpolation, lut_color_lookup_interpolation,
               LUT_INTERPOLATION_TETRAHEDRAL)
  description (_("How colors between the points of a 3D LUT are computed. "
                 "Tetrahedral is smoother along the gray axis and what most "
                 "color grading software uses. 1D LUTs are always "
                 "interpolated linearly."))

property_enum (encoding, _("LUT input encoding"),
               LutColorLookupEncoding, lut_color_lookup_encoding,
               LUT_ENCODING_PERCEPTUAL)
  description (_("The encoding the LUT was made for. Most creative LUTs are "
                 "made for display referred Rec. 709 or sRGB values, that "
                 "is the perceptual encoding; some are made for linear "
                 "light."))

property_enum (lut_space, _("LUT color space"),
               LutColorLookupSpace, lut_color_lookup_space,
               LUT_SPACE_IMAGE)
  description (_("The color space the LUT was made for. \"The image's\" "
                 "applies it to the image's own values, as Photoshop "
                 "does. \"sRGB\" converts the image to sRGB for the LUT "
                 "and back; for images in another space, such as Adobe RGB "
                 "or Rec. 2020, with a LUT made for sRGB or Rec. 709. Colors "
                 "outside sRGB are then clamped by the LUT unless it "
                 "extrapolates."))

property_enum (out_of_range, _("Outside the LUT's range"),
               LutColorLookupOutOfRange, lut_color_lookup_out_of_range,
               LUT_OUT_OF_RANGE_CLAMP)
  description (_("What happens to values outside the input range of the LUT "
                 "(usually 0 to 1): clamp them to its edge, or extend the "
                 "LUT linearly from its edge"))

property_boolean (lut_problem, _("The LUT file cannot be used"), FALSE)
  description (_("Set by the operation when the LUT file cannot be used."))
  ui_meta ("visible", "0")

/* GIMP shows it as a message box; "visible" hides the box while there is
 * no message (GIMP 3.2 shows an empty box otherwise) */
property_string (error, _("The LUT file cannot be used"), "")
  description (_("Why the LUT file cannot be used; the image is then left "
                 "as it is. Set by the operation."))
  ui_meta ("error", "true")
  ui_meta ("visible", "lut-problem")

#else

#define GEGL_OP_POINT_FILTER
#define GEGL_OP_NAME     color_lookup
#define GEGL_OP_C_SOURCE color-lookup.c

#include "gegl-op.h"

#include <gio/gio.h>
#include <math.h>
#include <string.h>

/* the largest LUTs read: the limits of the Cube LUT Specification 1.0 */
#define LUT_MAX_3D    256
#define LUT_MAX_1D    65536
/* the largest magnitude of a value in a LUT, and of a text file */
#define LUT_MAX_VALUE 1e6
#define LUT_MAX_FILE  ((goffset) 1 << 30)
/* extrapolation reaches this many times the input range beyond each edge;
 * farther values are held there, so that the output stays finite */
#define LUT_EXTRAPOLATE_REACH 1e4f
/* parsed LUTs that are kept */
#define LUT_CACHE_SIZE 8

/* a 1D or 3D table of RGB triplets over an input range per channel; a 3D
 * table has red changing fastest, then green, then blue */
typedef struct
{
  gint    n;          /* points along each axis, 0: not in the file */
  gfloat *data;
  gfloat  min[3];
  gfloat  max[3];
  gfloat  scale[3];   /* (n - 1) / (max - min) */
} LutStage;

typedef struct
{
  gint     ref_count;
  gchar   *path;
  gint64   mtime;     /* microseconds; -1: the file was not found, -2: it
                       * is a folder */
  goffset  size;
  gchar   *error;     /* NULL when the LUT can be used */
  LutStage s1;        /* 1D, applied first */
  LutStage s3;        /* 3D */
  gfloat  *mesh;      /* NULL, or the input values of the points of s3 on
                       * each axis for a .3dl file with an uneven grid */
} Lut;

static void
lut_unref (Lut *lut)
{
  if (lut && g_atomic_int_dec_and_test (&lut->ref_count))
    {
      g_free (lut->path);
      g_free (lut->error);
      g_free (lut->s1.data);
      g_free (lut->s3.data);
      g_free (lut->mesh);
      g_free (lut);
    }
}

static Lut *
lut_ref (Lut *lut)
{
  g_atomic_int_inc (&lut->ref_count);
  return lut;
}

/* sets the input range of a stage; FALSE if it is empty or not finite */
static gboolean
stage_set_range (LutStage      *s,
                 const gdouble *min,
                 const gdouble *max)
{
  gint c;

  for (c = 0; c < 3; c++)
    {
      if (! (isfinite (min[c]) && isfinite (max[c]) && max[c] > min[c]))
        return FALSE;
      s->min[c]   = min[c];
      s->max[c]   = max[c];
      s->scale[c] = (s->n - 1) / (max[c] - min[c]);
      if (! (isfinite (s->scale[c]) && s->scale[c] > 0.0f))
        return FALSE;
    }
  return TRUE;
}

/* reading text ----------------------------------------------------------- */

typedef struct
{
  gchar *p;       /* the rest of the file */
  gchar *end;
  gint   number;  /* of the line last returned, from 1 */
} Lines;

/* the next line, without its end (\n, \r\n or \r); NULL at the end. The
 * text is from g_file_get_contents, which puts a 0 after it: the last line
 * can end there */
static gchar *
next_line (Lines *l)
{
  gchar *line = l->p;
  gchar *e    = l->p;

  if (l->p >= l->end)
    return NULL;

  while (e < l->end && *e != '\n' && *e != '\r')
    e++;
  l->p = e;
  if (l->p < l->end && *l->p == '\r')
    l->p++;
  if (l->p < l->end && *l->p == '\n')
    l->p++;
  *e = '\0';
  l->number++;

  return line;
}

static inline gboolean
is_space (gchar c)
{
  return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}

static const gchar *
skip_space (const gchar *s)
{
  while (is_space (*s))
    s++;
  return s;
}

/* the end of a token: a space, the end of the line or a comment */
static inline gboolean
is_token_end (gchar c)
{
  return c == '\0' || c == '#' || is_space (c);
}

/* reads the numbers of a line up to a comment: at most max are stored,
 * *count is how many there are. FALSE if something else is on the line or
 * a number is not finite */
static gboolean
read_numbers (const gchar *s,
              gdouble     *values,
              gint         max,
              gint        *count)
{
  *count = 0;
  for (;;)
    {
      gchar   *e;
      gdouble  v;

      s = skip_space (s);
      if (*s == '\0' || *s == '#')
        return TRUE;
      /* g_ascii_strtod does not depend on the locale: 0.5, never 0,5 */
      v = g_ascii_strtod (s, &e);
      if (e == s || ! is_token_end (*e) || ! isfinite (v))
        return FALSE;
      if (*count < max)
        values[*count] = v;
      (*count)++;
      s = e;
    }
}

/* reads the unsigned integers of a line, as read_numbers */
static gboolean
read_integers (const gchar *s,
               gint64      *values,
               gint         max,
               gint        *count)
{
  *count = 0;
  for (;;)
    {
      const gchar *start;
      gint64       v = 0;

      s = skip_space (s);
      if (*s == '\0' || *s == '#')
        return TRUE;
      if (*s == '+')
        s++;
      start = s;
      while (*s >= '0' && *s <= '9')
        {
          if (v > G_MAXINT32)
            return FALSE;
          v = v * 10 + (*s - '0');
          s++;
        }
      if (s == start || ! is_token_end (*s) || v > G_MAXINT32)
        return FALSE;
      if (*count < max)
        values[*count] = v;
      (*count)++;
    }
}

/* "nan" and "inf" start a line of numbers (that are refused), not a
 * keyword */
static gboolean
starts_with_number (const gchar *s)
{
  gchar *e;

  g_ascii_strtod (s, &e);
  return e != s && is_token_end (*e);
}

/* keyword lines start with a word that has a letter: "3DMESH" too */
static gboolean
first_token_has_letter (const gchar *s)
{
  for (; ! is_token_end (*s); s++)
    if (g_ascii_isalpha (*s))
      return TRUE;
  return FALSE;
}

/* the keyword at s, if the line starts with a keyword */
static gboolean
is_keyword (const gchar *s,
            const gchar *keyword,
            const gchar **rest)
{
  gsize len = strlen (keyword);

  if (g_ascii_strncasecmp (s, keyword, len) != 0 || ! is_token_end (s[len]))
    return FALSE;
  *rest = s + len;
  return TRUE;
}

static gboolean
read_size (const gchar *s,
           gint         min,
           gint         max,
           gint        *size)
{
  gdouble v;
  gint    count;

  if (! read_numbers (s, &v, 1, &count) || count != 1 ||
      v != floor (v) || v < min || v > max)
    return FALSE;
  *size = (gint) v;
  return TRUE;
}

/* .cube ------------------------------------------------------------------ */

static gchar *
parse_cube (gchar       *text,
            gsize        length,
            const gchar *name,
            Lut         *lut)
{
  Lines    lines   = { text, text + length, 0 };
  gdouble  dmin[3] = { 0.0, 0.0, 0.0 };
  gdouble  dmax[3] = { 1.0, 1.0, 1.0 };
  gdouble  r1[2]   = { 0.0, 1.0 };
  gdouble  r3[2]   = { 0.0, 1.0 };
  gboolean have_r1 = FALSE, have_r3 = FALSE;
  gint     size1   = 0, size3 = 0;
  gint64   expected = 0, count = 0;
  GArray  *data    = NULL;
  gchar   *error   = NULL;
  gchar   *line;

  while ((line = next_line (&lines)))
    {
      const gchar *s = skip_space (line);
      const gchar *rest;
      gdouble      v[3];
      gint         n;

      if (*s == '\0' || *s == '#')
        continue;

      if ((g_ascii_isalpha (*s) || *s == '_') && ! starts_with_number (s))
        {
          if (count > 0)
            {
              error = g_strdup_printf (_("%s: line %d: a keyword after the "
                                         "table"), name, lines.number);
              break;
            }
          if (is_keyword (s, "LUT_3D_SIZE", &rest))
            {
              if (size3 || ! read_size (rest, 2, LUT_MAX_3D, &size3))
                {
                  error = g_strdup_printf (_("%s: line %d: LUT_3D_SIZE must "
                                             "be given once, from 2 to %d"),
                                           name, lines.number, LUT_MAX_3D);
                  break;
                }
            }
          else if (is_keyword (s, "LUT_1D_SIZE", &rest))
            {
              if (size1 || ! read_size (rest, 2, LUT_MAX_1D, &size1))
                {
                  error = g_strdup_printf (_("%s: line %d: LUT_1D_SIZE must "
                                             "be given once, from 2 to %d"),
                                           name, lines.number, LUT_MAX_1D);
                  break;
                }
            }
          else if (is_keyword (s, "DOMAIN_MIN", &rest) ||
                   is_keyword (s, "DOMAIN_MAX", &rest))
            {
              gdouble *d = g_ascii_toupper (s[8]) == 'I' ? dmin : dmax;

              if (! read_numbers (rest, v, 3, &n) || n != 3)
                {
                  error = g_strdup_printf (_("%s: line %d: DOMAIN_MIN and "
                                             "DOMAIN_MAX need three numbers"),
                                           name, lines.number);
                  break;
                }
              memcpy (d, v, sizeof (v));
            }
          else if (is_keyword (s, "LUT_1D_INPUT_RANGE", &rest) ||
                   is_keyword (s, "LUT_3D_INPUT_RANGE", &rest))
            {
              gboolean is1 = s[4] == '1';

              if (! read_numbers (rest, v, 2, &n) || n != 2)
                {
                  error = g_strdup_printf (_("%s: line %d: an input range "
                                             "needs two numbers"),
                                           name, lines.number);
                  break;
                }
              memcpy (is1 ? r1 : r3, v, 2 * sizeof (gdouble));
              if (is1)
                have_r1 = TRUE;
              else
                have_r3 = TRUE;
            }
          /* TITLE, and keywords of other programs, are not needed */
          continue;
        }

      if (! size1 && ! size3)
        {
          error = g_strdup_printf (_("%s: line %d: numbers before "
                                     "LUT_3D_SIZE or LUT_1D_SIZE: not a "
                                     ".cube LUT"), name, lines.number);
          break;
        }
      if (! data)
        {
          expected = size1 + (gint64) size3 * size3 * size3;
          data = g_array_new (FALSE, FALSE, sizeof (gfloat));
        }
      if (! read_numbers (s, v, 3, &n) || n != 3)
        {
          error = g_strdup_printf (_("%s: line %d: expected three finite "
                                     "numbers"), name, lines.number);
          break;
        }
      if (fabs (v[0]) > LUT_MAX_VALUE || fabs (v[1]) > LUT_MAX_VALUE ||
          fabs (v[2]) > LUT_MAX_VALUE)
        {
          error = g_strdup_printf (_("%s: line %d: a value beyond %g"),
                                   name, lines.number, LUT_MAX_VALUE);
          break;
        }
      if (count == expected)
        {
          error = g_strdup_printf (_("%s: more than the %" G_GINT64_FORMAT
                                     " lines of numbers that the sizes "
                                     "give"), name, expected);
          break;
        }
      {
        gfloat f[3] = { v[0], v[1], v[2] };
        g_array_append_vals (data, f, 3);
      }
      count++;
    }

  if (! error && ! size1 && ! size3)
    error = g_strdup_printf (_("%s: no LUT_3D_SIZE or LUT_1D_SIZE: not a "
                               ".cube LUT"), name);
  expected = size1 + (gint64) size3 * size3 * size3;
  if (! error && count != expected)
    error = g_strdup_printf (_("%s: %" G_GINT64_FORMAT " lines of numbers, "
                               "the sizes give %" G_GINT64_FORMAT),
                             name, count, expected);

  if (! error)
    {
      gfloat  *f = (gfloat *) data->data;
      gdouble  in1[3] = { r1[0], r1[0], r1[0] }, ax1[3] = { r1[1], r1[1], r1[1] };
      gdouble  in3[3] = { r3[0], r3[0], r3[0] }, ax3[3] = { r3[1], r3[1], r3[1] };
      gboolean ok = TRUE;

      /* the domain is that of the file's input: of the 1D LUT if there is
       * one, else of the 3D LUT; the Resolve input ranges override it */
      lut->s1.n = size1;
      lut->s3.n = size3;
      if (size1)
        {
          lut->s1.data = g_memdup2 (f, size1 * 3 * sizeof (gfloat));
          ok = have_r1 ? stage_set_range (&lut->s1, in1, ax1)
                       : stage_set_range (&lut->s1, dmin, dmax);
        }
      if (size3 && ok)
        {
          lut->s3.data = g_memdup2 (f + size1 * 3,
                                    (gsize) size3 * size3 * size3 * 3 *
                                    sizeof (gfloat));
          if (have_r3)
            ok = stage_set_range (&lut->s3, in3, ax3);
          else if (size1)
            {
              const gdouble z[3] = { 0.0, 0.0, 0.0 }, o[3] = { 1.0, 1.0, 1.0 };
              ok = stage_set_range (&lut->s3, z, o);
            }
          else
            ok = stage_set_range (&lut->s3, dmin, dmax);
        }
      if (! ok)
        error = g_strdup_printf (_("%s: an input range (DOMAIN_MIN and "
                                   "DOMAIN_MAX, or LUT_*_INPUT_RANGE) is "
                                   "empty"), name);
    }

  if (data)
    g_array_free (data, TRUE);

  return error;
}

/* .3dl ------------------------------------------------------------------- */

/* the bit depth of integer values whose largest is max, as OpenColorIO
 * infers it (FileFormat3DL.cpp, GetLikelyLutBitDepth): the even depth
 * whose largest value is up to twice max, 14 read as 16 */
static gint
likely_bit_depth (gint64 max)
{
  gint bits;

  for (bits = 8; bits <= 16; bits += 2)
    if (max <= ((gint64) 1 << bits) * 2 - 1)
      return bits == 14 ? 16 : bits;
  return 16;
}

static gchar *
parse_3dl (gchar       *text,
           gsize        length,
           const gchar *name,
           Lut         *lut)
{
  Lines   lines    = { text, text + length, 0 };
  GArray *mesh     = g_array_new (FALSE, FALSE, sizeof (gint64));
  GArray *data     = g_array_new (FALSE, FALSE, sizeof (gint32));
  gint    out_bits = 0;
  gint64  max_value = 0;
  gchar  *error    = NULL;
  gchar  *line;
  gint64  n_points, n = 0, i;

  while ((line = next_line (&lines)))
    {
      const gchar *s = skip_space (line);
      const gchar *rest;
      gint64       v[3];
      gint         count;

      if (*s == '\0' || *s == '#')
        continue;
      if (*s == '<')
        {
          error = g_strdup_printf (_("%s: line %d: XML, not a .3dl LUT"),
                                   name, lines.number);
          break;
        }
      if (first_token_has_letter (s))
        {
          /* Lustre: "3DMESH", "Mesh <input bits> <output bits>", "LUT8",
           * "gamma 1.0"; only the output depth is needed */
          if (is_keyword (s, "Mesh", &rest) &&
              read_integers (rest, v, 2, &count) && count == 2 &&
              v[1] >= 8 && v[1] <= 16)
            out_bits = v[1];
          continue;
        }

      if (! read_integers (s, v, 3, &count))
        {
          error = g_strdup_printf (_("%s: line %d: expected whole numbers "
                                     "of 0 or more"), name, lines.number);
          break;
        }
      /* the first line of numbers that is not a triplet is the input
       * grid (the mesh) */
      if (count != 3 && mesh->len == 0 && data->len == 0 && count >= 2)
        {
          gint64 *m;

          if (count > LUT_MAX_3D)
            {
              error = g_strdup_printf (_("%s: line %d: more than %d points "
                                         "in the input grid"),
                                       name, lines.number, LUT_MAX_3D);
              break;
            }
          g_array_set_size (mesh, count);
          m = (gint64 *) mesh->data;
          read_integers (s, m, count, &count);
          continue;
        }
      if (count != 3)
        {
          error = g_strdup_printf (_("%s: line %d: expected three numbers"),
                                   name, lines.number);
          break;
        }
      if (data->len / 3 >= (guint) LUT_MAX_3D * LUT_MAX_3D * LUT_MAX_3D + 1)
        {
          error = g_strdup_printf (_("%s: more than %d^3 points"),
                                   name, LUT_MAX_3D);
          break;
        }
      {
        gint32 t[3] = { v[0], v[1], v[2] };
        g_array_append_vals (data, t, 3);
      }
      max_value = MAX (max_value, MAX (v[0], MAX (v[1], v[2])));
    }

  n_points = data->len / 3;

  /* a grid line of three values reads as a triplet: 3 points, 27 + 1 */
  if (! error && mesh->len == 0 && n_points == 28)
    {
      for (i = 0; i < 3; i++)
        {
          gint64 m = g_array_index (data, gint32, i);
          g_array_append_val (mesh, m);
        }
      g_array_remove_range (data, 0, 3);
      n_points = 27;
      max_value = 0;
      for (i = 0; i < 27 * 3; i++)
        max_value = MAX (max_value, g_array_index (data, gint32, i));
    }

  if (! error)
    {
      for (n = 2; n * n * n < n_points && n <= LUT_MAX_3D; n++);
      if (n_points == 0)
        error = g_strdup_printf (_("%s: no table: not a .3dl LUT"), name);
      else if (n * n * n != n_points || n > LUT_MAX_3D)
        error = g_strdup_printf (_("%s: %" G_GINT64_FORMAT " points, not the "
                                   "cube of a size from 2 to %d"),
                                 name, n_points, LUT_MAX_3D);
      else if (mesh->len && mesh->len != n)
        error = g_strdup_printf (_("%s: the input grid has %u points, the "
                                   "table %" G_GINT64_FORMAT " per axis"),
                                 name, mesh->len, n);
      else if (! out_bits && max_value < 128)
        error = g_strdup_printf (_("%s: the largest value is %" G_GINT64_FORMAT
                                   ", too small for the integers of a .3dl "
                                   "LUT"), name, max_value);
    }

  if (! error)
    {
      const gint32 *d     = (const gint32 *) data->data;
      gdouble       scale = 1.0 / (((gint64) 1 << (out_bits ? out_bits :
                                     likely_bit_depth (max_value))) - 1);
      gdouble       zero[3] = { 0.0, 0.0, 0.0 }, one[3] = { 1.0, 1.0, 1.0 };
      gint          r, g, b;

      lut->s3.n    = n;
      lut->s3.data = g_new (gfloat, n * n * n * 3);
      /* blue changes fastest in the file, red in the table */
      for (r = 0, i = 0; r < n; r++)
        for (g = 0; g < n; g++)
          for (b = 0; b < n; b++, i++)
            {
              gfloat *p = lut->s3.data + ((b * n + g) * n + r) * 3;

              p[0] = d[i * 3 + 0] * scale;
              p[1] = d[i * 3 + 1] * scale;
              p[2] = d[i * 3 + 2] * scale;
            }
      stage_set_range (&lut->s3, zero, one);

      if (mesh->len)
        {
          const gint64 *m       = (const gint64 *) mesh->data;
          gint64        in_max  = m[n - 1];
          gdouble       in_code = ((gint64) 1 << likely_bit_depth (in_max)) - 1;
          gboolean      even    = TRUE;

          /* the grid lists the input values of the points, as codes of
           * some bit depth. Grids that are even to within 2 codes, like
           * 0 64 128 ... 960 1023, are taken as even over 0 to 1, as
           * OpenColorIO does; an uneven grid places the points at those
           * input values */
          for (i = 0; i < n; i++)
            {
              if (i > 0 && m[i] <= m[i - 1])
                {
                  error = g_strdup_printf (_("%s: the input grid does not "
                                             "increase"), name);
                  break;
                }
              if (fabs (m[i] - i * in_code / (n - 1)) >= 2.0)
                even = FALSE;
            }
          if (! error && ! even)
            {
              gdouble lo[3], hi[3];

              lut->mesh = g_new (gfloat, n);
              for (i = 0; i < n; i++)
                lut->mesh[i] = m[i] / in_code;
              lo[0] = lo[1] = lo[2] = lut->mesh[0];
              hi[0] = hi[1] = hi[2] = lut->mesh[n - 1];
              stage_set_range (&lut->s3, lo, hi);
            }
        }
    }

  g_array_free (mesh, TRUE);
  g_array_free (data, TRUE);

  return error;
}

/* Hald CLUT images --------------------------------------------------------- */

/* the GEGL loader for the file's first bytes, NULL if it is not an image */
static const gchar *
image_loader (const guchar *head,
              gsize         length)
{
  static const struct { const gchar *magic; gsize len; const gchar *op; }
  kinds[] =
    {
      { "\x89PNG\r\n\x1a\n", 8, "gegl:png-load"  },
      { "II*\0",             4, "gegl:tiff-load" },
      { "MM\0*",             4, "gegl:tiff-load" },
      { "\xff\xd8\xff",      3, "gegl:jpg-load"  },
      { "v/1\x01",           4, "gegl:exr-load"  },
    };
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (kinds); i++)
    if (length >= kinds[i].len &&
        memcmp (head, kinds[i].magic, kinds[i].len) == 0)
      return gegl_has_operation (kinds[i].op) ? kinds[i].op : NULL;
  if (length >= 12 && memcmp (head, "RIFF", 4) == 0 &&
      memcmp (head + 8, "WEBP", 4) == 0)
    return gegl_has_operation ("gegl:webp-load") ? "gegl:webp-load" : NULL;
  return NULL;
}

/* whether a PNG file is whole: every chunk within the file, with the
 * right CRC, up to IEND. gegl:png-load (GEGL 0.4.72) loops for ever on a
 * PNG that is cut short, as its read function ignores short reads. */
static gboolean
png_is_whole (const guchar *data,
              gsize         length)
{
  static guint32 table[256];
  static gsize   init = 0;
  gsize          at   = 8;

  if (g_once_init_enter (&init))
    {
      guint32 i, k;

      for (i = 0; i < 256; i++)
        {
          guint32 c = i;

          for (k = 0; k < 8; k++)
            c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
          table[i] = c;
        }
      g_once_init_leave (&init, 1);
    }

  while (length - at >= 12)
    {
      guint32 size = (guint32) data[at] << 24 | data[at + 1] << 16 |
                     data[at + 2] << 8 | data[at + 3];
      guint32 crc  = 0xffffffffu, stored;
      gsize   i;

      if (size > length - at - 12)
        return FALSE;
      for (i = at + 4; i < at + 8 + size; i++)
        crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
      stored = (guint32) data[i] << 24 | data[i + 1] << 16 |
               data[i + 2] << 8 | data[i + 3];
      if ((crc ^ 0xffffffffu) != stored)
        return FALSE;
      if (memcmp (data + at + 4, "IEND", 4) == 0)
        return TRUE;
      at += 12 + size;
    }
  return FALSE;
}

/* whether the first image of a classic TIFF file is whole: its tags, and
 * the strips or tiles of its pixels, within the file. gegl:tiff-load fills
 * what it cannot read with zeros, without a warning. BigTIFF is not
 * checked. */
static guint64
tiff_get (const guchar *p,
          gboolean      big_endian,
          gint          size)
{
  guint64 v = 0;
  gint    i;

  for (i = 0; i < size; i++)
    v |= (guint64) p[big_endian ? i : size - 1 - i] << (8 * (size - 1 - i));
  return v;
}

static gboolean
tiff_is_whole (const guchar *data,
               gsize         length)
{
  /* bytes per value of the TIFF field types 1 to 18 */
  static const gint type_size[19] = { 0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8,
                                      0, 0, 0, 8, 8, 8 };
  const gboolean be = data[0] == 'M';
  guint64        ifd, n, e, count[2] = { 0, 0 };
  gsize          at[2] = { 0, 0 }, width[2] = { 0, 0 };
  gint           k;

  if (length < 8 || tiff_get (data + 2, be, 2) != 42)
    return length >= 4 && tiff_get (data + 2, be, 2) == 43;
  ifd = tiff_get (data + 4, be, 4);
  if (ifd > length - 2)
    return FALSE;
  n = tiff_get (data + ifd, be, 2);
  if (n == 0 || ifd + 2 + 12 * n + 4 > length)
    return FALSE;

  for (e = 0; e < n; e++)
    {
      const guchar *entry = data + ifd + 2 + 12 * e;
      guint64       tag   = tiff_get (entry, be, 2);
      guint64       type  = tiff_get (entry + 2, be, 2);
      guint64       cnt   = tiff_get (entry + 4, be, 4);
      gint          size  = type < 19 ? type_size[type] : 0;
      guint64       bytes = cnt * size;
      gsize         where = entry + 8 - data;

      if (size == 0)
        continue;
      if (bytes > 4)
        {
          where = tiff_get (entry + 8, be, 4);
          if (where > length || bytes > length - where)
            return FALSE;
        }
      /* offsets and byte counts of strips (273, 279) or tiles (324, 325) */
      k = tag == 273 || tag == 324 ? 0 : tag == 279 || tag == 325 ? 1 : -1;
      if (k >= 0 && (size == 2 || size == 4))
        {
          count[k] = cnt;
          at[k]    = where;
          width[k] = size;
        }
    }

  if (count[0] == 0 || count[0] != count[1])
    return FALSE;
  for (e = 0; e < count[0]; e++)
    {
      guint64 offset = tiff_get (data + at[0] + e * width[0], be, width[0]);
      guint64 bytes  = tiff_get (data + at[1] + e * width[1], be, width[1]);

      if (offset > length || bytes > length - offset)
        return FALSE;
    }
  return TRUE;
}

static gchar *
load_hald (const gchar *path,
           const gchar *loader,
           const gchar *name,
           Lut         *lut)
{
  GeglNode      *graph  = gegl_node_new ();
  GeglBuffer    *buffer = NULL;
  GeglNode      *load, *sink;
  GeglRectangle  extent;
  const Babl    *format, *space, *model;
  const gchar   *model_name, *target;
  gchar         *error  = NULL;
  gint           level  = 0, n;

  load = gegl_node_new_child (graph, "operation", loader, "path", path, NULL);
  sink = gegl_node_new_child (graph, "operation", "gegl:buffer-sink",
                              "buffer", &buffer, NULL);
  gegl_node_link (load, sink);
  gegl_node_process (sink);
  g_object_unref (graph);

  if (! buffer)
    return g_strdup_printf (_("%s: the image cannot be read"), name);

  extent = *gegl_buffer_get_extent (buffer);
  if (extent.width > 0 && extent.width == extent.height)
    for (level = 2; level * level * level < extent.width; level++);
  if (extent.width <= 0 || extent.width != extent.height ||
      level * level * level != extent.width || level * level > LUT_MAX_3D)
    {
      error = g_strdup_printf (_("%s: an image of %d x %d pixels is not a "
                                 "Hald CLUT (a square of L^3 pixels a side, "
                                 "L from 2 to 16)"),
                               name, extent.width, extent.height);
      g_object_unref (buffer);
      return error;
    }
  n = level * level;

  /* the values as they are in the file: the same encoding (perceptual,
   * linear or the sRGB curve) in the image's own space, as float */
  format     = gegl_buffer_get_format (buffer);
  space      = babl_format_get_space (format);
  model      = babl_format_get_model (format);
  model_name = model ? babl_get_name (model) : "R'G'B'";
  if (strchr (model_name, '~'))
    target = "R~G~B~ float";
  else if (strchr (model_name, '\''))
    target = "R'G'B' float";
  else
    target = "RGB float";

  lut->s3.n    = n;
  lut->s3.data = g_new (gfloat, (gsize) n * n * n * 3);
  /* pixel i in raster order is the point r = i % n, g = i / n % n,
   * b = i / n^2: the order of the table */
  gegl_buffer_get (buffer, &extent, 1.0, babl_format_with_space (target, space),
                   lut->s3.data, GEGL_AUTO_ROWSTRIDE, GEGL_ABYSS_NONE);
  g_object_unref (buffer);

  {
    const gdouble zero[3] = { 0.0, 0.0, 0.0 }, one[3] = { 1.0, 1.0, 1.0 };
    gsize         i;

    for (i = 0; i < (gsize) n * n * n * 3; i++)
      if (! (fabsf (lut->s3.data[i]) <= LUT_MAX_VALUE))
        return g_strdup_printf (_("%s: the image has values that are not "
                                  "finite"), name);
    stage_set_range (&lut->s3, zero, one);
  }

  return NULL;
}

/* loading and the cache ----------------------------------------------------- */

static gchar *
lut_load (Lut *lut)
{
  gchar       *name = g_filename_display_basename (lut->path);
  gchar       *text = NULL;
  gsize        length;
  GError      *gerror = NULL;
  gchar       *error = NULL;
  const gchar *loader;

  if (lut->mtime == -1)
    error = g_strdup_printf (_("%s: the file is not there"), name);
  else if (lut->mtime < 0)
    error = g_strdup_printf (_("%s: a folder, not a LUT file"), name);
  else if (lut->size == 0)
    error = g_strdup_printf (_("%s: the file is empty"), name);
  else if (lut->size > LUT_MAX_FILE)
    error = g_strdup_printf (_("%s: the file is too large for a LUT"), name);
  else if (! g_file_get_contents (lut->path, &text, &length, &gerror))
    {
      error = g_strdup_printf (_("%s: %s"), name, gerror->message);
      g_error_free (gerror);
    }
  else if ((loader = image_loader ((const guchar *) text, length)))
    {
      if (! strcmp (loader, "gegl:png-load") &&
          ! png_is_whole ((const guchar *) text, length))
        error = g_strdup_printf (_("%s: the PNG file is cut short or "
                                   "damaged"), name);
      else if (! strcmp (loader, "gegl:tiff-load") &&
               ! tiff_is_whole ((const guchar *) text, length))
        error = g_strdup_printf (_("%s: the TIFF file is cut short or "
                                   "damaged"), name);
      else
        error = load_hald (lut->path, loader, name, lut);
    }
  else if (memchr (text, '\0', length))
    error = g_strdup_printf (_("%s: not a LUT: neither text nor a known "
                               "image"), name);
  else
    {
      gchar *lower = g_ascii_strdown (lut->path, -1);

      if (g_str_has_suffix (lower, ".3dl"))
        error = parse_3dl (text, length, name, lut);
      else
        error = parse_cube (text, length, name, lut);
      g_free (lower);
    }

  g_free (text);
  g_free (name);

  return error;
}

static GMutex lut_cache_mutex;
static Lut   *lut_cache[LUT_CACHE_SIZE];   /* most recently used first */

/* the LUT for a path, parsed when the file is new or changed; each
 * failure is reported once per version of the file */
static Lut *
lut_get (const gchar *path)
{
  GFile     *file  = g_file_new_for_path (path);
  GFileInfo *info;
  gint64     mtime = -1;
  goffset    size  = 0;
  Lut       *lut   = NULL;
  gint       i;

  info = g_file_query_info (file,
                            G_FILE_ATTRIBUTE_STANDARD_SIZE ","
                            G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                            G_FILE_ATTRIBUTE_TIME_MODIFIED ","
                            G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC,
                            G_FILE_QUERY_INFO_NONE, NULL, NULL);
  g_object_unref (file);
  if (info)
    {
      mtime = g_file_info_get_attribute_uint64 (info,
                G_FILE_ATTRIBUTE_TIME_MODIFIED) * G_USEC_PER_SEC +
              g_file_info_get_attribute_uint32 (info,
                G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
      size  = g_file_info_get_size (info);
      if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
        mtime = -2;
      g_object_unref (info);
    }

  g_mutex_lock (&lut_cache_mutex);
  for (i = 0; i < LUT_CACHE_SIZE && lut_cache[i]; i++)
    if (strcmp (lut_cache[i]->path, path) == 0 &&
        lut_cache[i]->mtime == mtime && lut_cache[i]->size == size)
      {
        lut = lut_cache[i];
        memmove (lut_cache + 1, lut_cache, i * sizeof (Lut *));
        lut_cache[0] = lut_ref (lut);
        break;
      }
  g_mutex_unlock (&lut_cache_mutex);
  if (lut)
    return lut;

  lut = g_new0 (Lut, 1);
  lut->ref_count = 1;
  lut->path      = g_strdup (path);
  lut->mtime     = mtime;
  lut->size      = size;
  lut->error     = lut_load (lut);
  if (lut->error)
    g_warning ("lut:color-lookup: %s; the image is left as it is",
               lut->error);

  g_mutex_lock (&lut_cache_mutex);
  /* the older version of the same file goes, else the least used */
  for (i = 0; i < LUT_CACHE_SIZE - 1 && lut_cache[i]; i++)
    if (strcmp (lut_cache[i]->path, path) == 0)
      break;
  lut_unref (lut_cache[i]);
  memmove (lut_cache + 1, lut_cache, i * sizeof (Lut *));
  lut_cache[0] = lut_ref (lut);
  g_mutex_unlock (&lut_cache_mutex);

  return lut;
}

/* applying the LUT ----------------------------------------------------- */

/* the loop is made for each combination of options below; the helpers must
 * be inlined into it for that */
#if defined (__GNUC__)
#define LUT_INLINE static inline __attribute__ ((always_inline))
#else
#define LUT_INLINE static inline
#endif

typedef struct
{
  const LutStage *s1, *s3;
  const gfloat   *mesh;
  gfloat          lo1, hi1;   /* limits of the coordinates, in points */
  gfloat          lo3, hi3;
  gfloat          mlo, mhi;   /* limits of the input on an uneven grid */
  gfloat          in_min[3];  /* the input range of the first stage, for */
  gfloat          in_max[3];  /* input that is not finite */
  gfloat          strength;
} Run;

/* the position of x on an axis of n points, in points: clamped to the
 * limits, NaN at the lowest point */
LUT_INLINE gfloat
coordinate (gfloat x,
            gfloat min,
            gfloat scale,
            gfloat lo,
            gfloat hi)
{
  gfloat t = (x - min) * scale;

  t = t == t ? t : 0.0f;
  t = t > lo ? t : lo;
  return t < hi ? t : hi;
}

/* the same on an uneven grid of input values */
LUT_INLINE gfloat
mesh_coordinate (const gfloat *mesh,
                 gint          n,
                 gfloat        x,
                 gfloat        lo,
                 gfloat        hi)
{
  gint a = 0, b = n - 2;

  x = x == x ? x : mesh[0];
  x = x > lo ? x : lo;
  x = x < hi ? x : hi;
  while (a < b)
    {
      gint m = (a + b + 1) / 2;

      if (mesh[m] <= x)
        a = m;
      else
        b = m - 1;
    }
  return a + (x - mesh[a]) / (mesh[a + 1] - mesh[a]);
}

/* the cell of coordinate t: 0 to n - 2; outside the table, t - cell is
 * below 0 or above 1 and the edge cell is extended */
LUT_INLINE gint
cell (gfloat t,
      gint   n)
{
  gint i = (gint) t;

  i = i > 0 ? i : 0;
  return i < n - 2 ? i : n - 2;
}

LUT_INLINE void
apply_1d (const LutStage *s,
          gfloat          lo,
          gfloat          hi,
          gfloat         *v)
{
  gint c;

  for (c = 0; c < 3; c++)
    {
      gfloat        t = coordinate (v[c], s->min[c], s->scale[c], lo, hi);
      gint          i = cell (t, s->n);
      gfloat        f = t - i;
      const gfloat *p = s->data + i * 3 + c;

      v[c] = p[0] + f * (p[3] - p[0]);
    }
}

/* tetrahedral interpolation: the cell is cut into six tetrahedra along
 * its gray diagonal, and the one holding the point is found by sorting
 * the three fractions; its corners are the path from the cell's first
 * corner to its last that steps along the axis of the largest fraction,
 * then the middle, then the smallest. The weights are the differences of
 * the sorted fractions. Sorting with selects instead of branching keeps
 * the loop free of hard to predict jumps. */
#define SORT2(fa, oa, fb, ob)                          \
  G_STMT_START {                                       \
    const gint   swap_ = -(gint) (fa < fb);            \
    const gint   x_    = (oa ^ ob) & swap_;            \
    const gfloat hi_   = fa > fb ? fa : fb;            \
    const gfloat lo_   = fa > fb ? fb : fa;            \
    oa ^= x_;                                          \
    ob ^= x_;                                          \
    fa  = hi_;                                         \
    fb  = lo_;                                         \
  } G_STMT_END

LUT_INLINE void
tetrahedral (const LutStage *s,
             const gfloat   *t,
             gfloat         *v)
{
  const gint    n  = s->n;
  const gint    ir = cell (t[0], n), ig = cell (t[1], n), ib = cell (t[2], n);
  gfloat        f0 = t[0] - ir, f1 = t[1] - ig, f2 = t[2] - ib;
  gint          o0 = 3, o1 = 3 * n, o2 = 3 * n * n;
  const gfloat *c0 = s->data + ((ib * n + ig) * n + ir) * 3;
  const gfloat *c3 = c0 + 3 + 3 * n + 3 * n * n;
  const gfloat *c1, *c2;
  gint          c;

  SORT2 (f0, o0, f1, o1);
  SORT2 (f1, o1, f2, o2);
  SORT2 (f0, o0, f1, o1);
  c1 = c0 + o0;
  c2 = c1 + o1;
  /* (1 - f0) c0 + (f0 - f1) c1 + (f1 - f2) c2 + f2 c3, written as steps
   * along the path: exact at the points, and precise far outside the
   * table when extrapolating */
  for (c = 0; c < 3; c++)
    v[c] = c0[c] + f0 * (c1[c] - c0[c]) + f1 * (c2[c] - c1[c]) +
           f2 * (c3[c] - c2[c]);
}

LUT_INLINE void
trilinear (const LutStage *s,
           const gfloat   *t,
           gfloat         *v)
{
  const gint    n  = s->n;
  const gint    ir = cell (t[0], n), ig = cell (t[1], n), ib = cell (t[2], n);
  const gfloat  fr = t[0] - ir, fg = t[1] - ig, fb = t[2] - ib;
  const gint    dg = 3 * n, db = 3 * n * n;
  const gfloat *p  = s->data + ((ib * n + ig) * n + ir) * 3;
  gint          c;

  for (c = 0; c < 3; c++)
    {
      const gfloat *q   = p + c;
      const gfloat  c00 = q[0]       + fr * (q[3]           - q[0]);
      const gfloat  c10 = q[dg]      + fr * (q[dg + 3]      - q[dg]);
      const gfloat  c01 = q[db]      + fr * (q[db + 3]      - q[db]);
      const gfloat  c11 = q[db + dg] + fr * (q[db + dg + 3] - q[db + dg]);
      const gfloat  c0  = c00 + fg * (c10 - c00);
      const gfloat  c1  = c01 + fg * (c11 - c01);

      v[c] = c0 + fb * (c1 - c0);
    }
}

/* the loop, made for each combination of stages and options so that the
 * choices are not made per pixel */
LUT_INLINE void
run_pixels (const Run    *r,
            const gfloat *in,
            gfloat       *out,
            glong         n_pixels,
            const gboolean has1,
            const gboolean has3,
            const gboolean tetra,
            const gboolean mesh,
            const gboolean mix)
{
  const LutStage *s1 = r->s1, *s3 = r->s3;
  glong           i;
  gint            c;

  for (i = 0; i < n_pixels; i++, in += 4, out += 4)
    {
      /* read everything first: in and out may be the same memory */
      gfloat       v[3] = { in[0], in[1], in[2] };
      const gfloat a    = in[3];
      gfloat       x[3] = { in[0], in[1], in[2] };

      if (has1)
        apply_1d (s1, r->lo1, r->hi1, v);
      if (has3)
        {
          gfloat t[3];

          if (mesh)
            for (c = 0; c < 3; c++)
              t[c] = mesh_coordinate (r->mesh, s3->n, v[c], r->mlo, r->mhi);
          else
            for (c = 0; c < 3; c++)
              t[c] = coordinate (v[c], s3->min[c], s3->scale[c],
                                 r->lo3, r->hi3);
          if (tetra)
            tetrahedral (s3, t, v);
          else
            trilinear (s3, t, v);
        }
      if (mix)
        for (c = 0; c < 3; c++)
          {
            /* NaN and infinities mix as the edges of the input range */
            const gfloat xc = isfinite (x[c]) ? x[c] :
                              x[c] > 0.0f ? r->in_max[c] : r->in_min[c];

            v[c] = xc * (1.0f - r->strength) + v[c] * r->strength;
          }
      out[0] = v[0];
      out[1] = v[1];
      out[2] = v[2];
      out[3] = a;
    }
}

typedef void (* RunFunc) (const Run *, const gfloat *, gfloat *, glong);

#define DEFINE_RUN(name, h1, h3, te, me, mx)                             \
  static void                                                            \
  name (const Run *r, const gfloat *in, gfloat *out, glong n)            \
  {                                                                      \
    run_pixels (r, in, out, n, h1, h3, te, me, mx);                      \
  }

/* bits: 1 the 1D stage, 2 the 3D stage, 4 tetrahedral, 8 uneven grid,
 * 16 mix */
#define DEFINE_RUN_4(b, h3, te, me)                                      \
  DEFINE_RUN (run_##b##_0_0, 0, h3, te, me, 0)                           \
  DEFINE_RUN (run_##b##_1_0, 1, h3, te, me, 0)                           \
  DEFINE_RUN (run_##b##_0_1, 0, h3, te, me, 1)                           \
  DEFINE_RUN (run_##b##_1_1, 1, h3, te, me, 1)
DEFINE_RUN_4 (only1,   0, 0, 0)
DEFINE_RUN_4 (tri,     1, 0, 0)
DEFINE_RUN_4 (tet,     1, 1, 0)
DEFINE_RUN_4 (trimesh, 1, 0, 1)
DEFINE_RUN_4 (tetmesh, 1, 1, 1)

static const RunFunc run_funcs[5][2][2] =
{
  { { run_only1_0_0,   run_only1_0_1   }, { run_only1_1_0,   run_only1_1_1   } },
  { { run_tri_0_0,     run_tri_0_1     }, { run_tri_1_0,     run_tri_1_1     } },
  { { run_tet_0_0,     run_tet_0_1     }, { run_tet_1_0,     run_tet_1_1     } },
  { { run_trimesh_0_0, run_trimesh_0_1 }, { run_trimesh_1_0, run_trimesh_1_1 } },
  { { run_tetmesh_0_0, run_tetmesh_0_1 }, { run_tetmesh_1_0, run_tetmesh_1_1 } },
};

/* what the operation keeps in user_data */
typedef struct
{
  Lut   *lut;
  gchar *pending;    /* the error on its way to the property, or NULL */
} State;

static const Lut *
state_lut (GeglProperties *o)
{
  return o->user_data ? ((State *) o->user_data)->lut : NULL;
}

static gboolean
process (GeglOperation       *operation,
         void                *in_buf,
         void                *out_buf,
         glong                n_pixels,
         const GeglRectangle *roi,
         gint                 level)
{
  GeglProperties *o      = GEGL_PROPERTIES (operation);
  const Lut      *lut    = state_lut (o);
  const gboolean  extrap = o->out_of_range == LUT_OUT_OF_RANGE_EXTRAPOLATE;
  const gfloat    reach  = extrap ? LUT_EXTRAPOLATE_REACH : 0.0f;
  Run             r;
  gint            kind, c;

  (void) roi;
  (void) level;

  if (! lut || lut->error)
    {
      if (in_buf != out_buf)
        memcpy (out_buf, in_buf, n_pixels * 4 * sizeof (gfloat));
      return TRUE;
    }

  r.s1       = &lut->s1;
  r.s3       = &lut->s3;
  r.mesh     = lut->mesh;
  r.strength = o->strength;
  r.lo1 = r.hi1 = r.lo3 = r.hi3 = r.mlo = r.mhi = 0.0f;
  if (lut->s1.n)
    {
      r.lo1 = -reach * (lut->s1.n - 1);
      r.hi1 = (lut->s1.n - 1) * (1.0f + reach);
    }
  if (lut->s3.n)
    {
      r.lo3 = -reach * (lut->s3.n - 1);
      r.hi3 = (lut->s3.n - 1) * (1.0f + reach);
    }
  if (lut->mesh)
    {
      gfloat m0 = lut->mesh[0], m1 = lut->mesh[lut->s3.n - 1];

      r.mlo = m0 - reach * (m1 - m0);
      r.mhi = m1 + reach * (m1 - m0);
    }
  for (c = 0; c < 3; c++)
    {
      const LutStage *first = lut->s1.n ? &lut->s1 : &lut->s3;

      r.in_min[c] = first->min[c];
      r.in_max[c] = first->max[c];
    }

  if (! lut->s3.n)
    kind = 0;
  else
    kind = (o->interpolation == LUT_INTERPOLATION_TETRAHEDRAL ? 2 : 1) +
           (lut->mesh ? 2 : 0);

  run_funcs[kind][lut->s1.n > 0][o->strength < 1.0]
    (&r, in_buf, out_buf, n_pixels);

  return TRUE;
}

/* The error property is set from the main loop, not from prepare: GEGL
 * calls prepare with the graph locked, and the notification of a changed
 * property prepares the graph again (a deadlock). GIMP runs the main loop
 * in its main thread, where it then updates the filter dialog. */
typedef struct
{
  GWeakRef  operation;
  gchar    *error;
} ErrorUpdate;

static GMutex error_mutex;

static gboolean
error_update_idle (gpointer data)
{
  ErrorUpdate *u         = data;
  GObject     *operation = g_weak_ref_get (&u->operation);

  if (operation)
    {
      GeglProperties *o    = GEGL_PROPERTIES (operation);
      State          *state;
      gboolean        mine = FALSE;

      /* only the latest update is made */
      g_mutex_lock (&error_mutex);
      state = o->user_data;
      if (state && state->pending && ! strcmp (state->pending, u->error))
        {
          g_clear_pointer (&state->pending, g_free);
          mine = TRUE;
        }
      g_mutex_unlock (&error_mutex);

      if (mine && (strcmp (o->error ? o->error : "", u->error) != 0 ||
                   o->lut_problem != (u->error[0] != '\0')))
        g_object_set (operation,
                      "lut-problem", u->error[0] != '\0',
                      "error",       u->error,
                      NULL);
      g_object_unref (operation);
    }
  return G_SOURCE_REMOVE;
}

static void
error_update_free (gpointer data)
{
  ErrorUpdate *u = data;

  g_weak_ref_clear (&u->operation);
  g_free (u->error);
  g_free (u);
}

/* makes the error property say error (and lut-problem say whether there
 * is one), unless they do or will. They are compared with the properties
 * themselves, so that a message that someone cleared (a plug-in that sets
 * all the settings again) comes back */
static void
report_error (GeglOperation *operation,
              State         *state,
              const gchar   *error)
{
  GeglProperties *o = GEGL_PROPERTIES (operation);
  ErrorUpdate    *u;

  g_mutex_lock (&error_mutex);
  if (state->pending ? strcmp (state->pending, error) == 0 :
      strcmp (o->error ? o->error : "", error) == 0 &&
      o->lut_problem == (error[0] != '\0'))
    {
      g_mutex_unlock (&error_mutex);
      return;
    }
  g_free (state->pending);
  state->pending = g_strdup (error);
  g_mutex_unlock (&error_mutex);

  u = g_new0 (ErrorUpdate, 1);
  g_weak_ref_init (&u->operation, operation);
  u->error = g_strdup (error);
  g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, error_update_idle, u,
                   error_update_free);
}

static void
prepare (GeglOperation *operation)
{
  GeglProperties *o = GEGL_PROPERTIES (operation);
  const Babl     *space;
  const Babl     *format;
  State          *state;
  Lut            *lut = NULL;

  /* the LUT sees the pixels in the encoding and space it was made for;
   * babl converts to them and back */
  if (o->lut_space == LUT_SPACE_SRGB)
    space = babl_space ("sRGB");
  else
    space = gegl_operation_get_source_space (operation, "input");
  format = babl_format_with_space (o->encoding == LUT_ENCODING_LINEAR ?
                                   "RGBA float" : "R'G'B'A float", space);
  gegl_operation_set_format (operation, "input", format);
  gegl_operation_set_format (operation, "output", format);

  if (! o->user_data)
    o->user_data = g_new0 (State, 1);
  state = o->user_data;

  if (o->path && o->path[0])
    lut = lut_get (o->path);
  lut_unref (state->lut);
  state->lut = lut;

  report_error (operation, state, lut && lut->error ? lut->error : "");
}

/* without a usable LUT, or at strength 0, the input goes through as it is */
static gboolean
operation_process (GeglOperation        *operation,
                   GeglOperationContext *context,
                   const gchar          *output_prop,
                   const GeglRectangle  *result,
                   gint                  level)
{
  GeglProperties *o   = GEGL_PROPERTIES (operation);
  const Lut      *lut = state_lut (o);

  (void) level;

  if (! lut || lut->error || o->strength <= 0.0)
    {
      gpointer in = gegl_operation_context_get_object (context, "input");

      if (in)
        gegl_operation_context_take_object (context, "output",
                                            g_object_ref (G_OBJECT (in)));
      return TRUE;
    }

  return GEGL_OPERATION_CLASS (gegl_op_parent_class)->process (
           operation, context, output_prop, result,
           gegl_operation_context_get_level (context));
}

static void
finalize (GObject *object)
{
  GeglProperties *o = GEGL_PROPERTIES (object);

  if (o->user_data)
    {
      State *state = o->user_data;

      lut_unref (state->lut);
      g_free (state->pending);
      g_free (state);
      o->user_data = NULL;
    }

  G_OBJECT_CLASS (gegl_op_parent_class)->finalize (object);
}

static void
gegl_op_class_init (GeglOpClass *klass)
{
  GObjectClass                  *object_class    = G_OBJECT_CLASS (klass);
  GeglOperationClass            *operation_class = GEGL_OPERATION_CLASS (klass);
  GeglOperationPointFilterClass *point_class     =
    GEGL_OPERATION_POINT_FILTER_CLASS (klass);

  object_class->finalize          = finalize;
  operation_class->prepare        = prepare;
  operation_class->process        = operation_process;
  operation_class->opencl_support = FALSE;
  point_class->process            = process;

  gegl_operation_class_set_keys (operation_class,
    "name",            "lut:color-lookup",
    "title",           _("Color Lookup (LUT)"),
    "categories",      "color",
    "description",     _("Applies a 3D or 1D color lookup table (LUT) from "
                         "a .cube, .3dl or Hald CLUT file, the way film "
                         "looks and color grades are shared"),
    "gimp:menu-path",  "<Image>/Colors",
    "gimp:menu-label", _("Color Lookup (LUT)..."),
    NULL);
}

#endif

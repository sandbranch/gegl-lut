/*
 * Automated checks for lut:color-lookup
 *
 * check.c
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
 * Writes LUT files (.cube, .3dl, Hald CLUT images) into a temporary
 * folder, runs the operation on synthetic pixels and checks the results
 * against an interpolation written here in double precision in the
 * textbook form, and against what the interpolation must give
 * analytically: identities, affine transforms (exact everywhere), a
 * gamma curve (linear between the points, within h^2/8 max|f''| of the
 * curve), products of channels (exact for trilinear, a known error for
 * tetrahedral). Also 1D LUTs, input ranges, values outside the range,
 * NaN and infinities, alpha, strength, the encoding and the color space
 * options, bad files, the cache, and the fixture files in tests/fixtures.
 *
 *   check <color-lookup.so> <tests/fixtures>
 *
 * Prints PASS or FAIL per case and exits with 1 if any case failed.
 */

#include <gegl.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define OP     "lut:color-lookup"
/* the working format of the operation with its defaults, on sRGB */
#define WORK   "R'G'B'A float"

static gint      n_failed   = 0;
static gint      n_passed   = 0;
static gint      n_messages = 0;  /* warnings and criticals from GLib and GEGL */
static gboolean  quiet_messages = FALSE;
static gchar    *tmp_dir    = NULL;
static gchar    *fixtures   = NULL;

static void
log_handler (const gchar    *domain,
             GLogLevelFlags  level,
             const gchar    *message,
             gpointer        data)
{
  (void) data;
  if (level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING))
    n_messages++;
  if (quiet_messages && (level & G_LOG_LEVEL_WARNING))
    printf ("      (warning: %s)\n", message);
  else
    g_log_default_handler (domain, level, message, NULL);
}

static void
report (const gchar *name,
        gboolean     ok,
        const gchar *format,
        ...) G_GNUC_PRINTF (3, 4);

static void
report (const gchar *name,
        gboolean     ok,
        const gchar *format,
        ...)
{
  printf ("%s  %s", ok ? "PASS" : "FAIL", name);
  if (format)
    {
      va_list args;
      gchar  *detail;

      va_start (args, format);
      detail = g_strdup_vprintf (format, args);
      va_end (args);
      printf (": %s", detail);
      g_free (detail);
    }
  printf ("\n");
  fflush (stdout);

  if (ok)
    n_passed++;
  else
    n_failed++;
}

/* transforms baked into LUTs -------------------------------------------- */

typedef void (*Transform) (const gdouble *in, gdouble *out, gpointer data);

static void
tf_identity (const gdouble *in, gdouble *out, gpointer data)
{
  (void) data;
  out[0] = in[0]; out[1] = in[1]; out[2] = in[2];
}

/* the channels turned: red takes green, green blue, blue red */
static void
tf_swap (const gdouble *in, gdouble *out, gpointer data)
{
  (void) data;
  out[0] = in[1]; out[1] = in[2]; out[2] = in[0];
}

static void
tf_invert (const gdouble *in, gdouble *out, gpointer data)
{
  (void) data;
  out[0] = 1.0 - in[0]; out[1] = 1.0 - in[1]; out[2] = 1.0 - in[2];
}

/* an affine color matrix with an offset, some of it outside 0 to 1 */
static const gdouble matrix[3][4] =
  {
    {  0.8,  0.3, -0.1,  0.05 },
    { -0.2,  1.1,  0.2, -0.02 },
    {  0.1, -0.3,  1.3,  0.0  },
  };

static void
tf_matrix (const gdouble *in, gdouble *out, gpointer data)
{
  gint c;

  (void) data;
  for (c = 0; c < 3; c++)
    out[c] = matrix[c][0] * in[0] + matrix[c][1] * in[1] +
             matrix[c][2] * in[2] + matrix[c][3];
}

static void
tf_gamma (const gdouble *in, gdouble *out, gpointer data)
{
  gint c;

  (void) data;
  for (c = 0; c < 3; c++)
    out[c] = pow (MAX (in[c], 0.0), 2.2);
}

static void
tf_product (const gdouble *in, gdouble *out, gpointer data)
{
  (void) data;
  out[0] = in[0] * in[1]; out[1] = in[1] * in[2]; out[2] = in[2] * in[0];
}

/* a smooth, colorful grade: a contrast curve, a hue twist, saturation */
static void
tf_grade (const gdouble *in, gdouble *out, gpointer data)
{
  gdouble y = 0.3 * in[0] + 0.55 * in[1] + 0.15 * in[2];
  gint    c;

  (void) data;
  for (c = 0; c < 3; c++)
    {
      gdouble v = y + 1.3 * (in[c] - y) + 0.08 * sin (3.0 * in[(c + 1) % 3]);

      out[c] = 0.5 + 0.5 * tanh (2.2 * (v - 0.5)) / tanh (1.1);
    }
}

/* the same kept within 0 to 1, for images that cannot hold more */
static void
tf_grade01 (const gdouble *in, gdouble *out, gpointer data)
{
  gint c;

  tf_grade (in, out, data);
  for (c = 0; c < 3; c++)
    out[c] = CLAMP (out[c], 0.0, 1.0);
}

/* the reference LUT and its interpolation, in double precision ------ */

typedef struct
{
  gint     n;
  gdouble *v;         /* n^3 x RGB, red fastest */
  gdouble  min[3];
  gdouble  max[3];
} Ref;

static Ref
ref_new (gint n, Transform tf, gpointer data,
         const gdouble *min, const gdouble *max)
{
  static const gdouble zero[3] = { 0, 0, 0 }, one[3] = { 1, 1, 1 };
  Ref  r;
  gint i, j, k, c;

  r.n = n;
  r.v = g_new (gdouble, (gsize) n * n * n * 3);
  for (c = 0; c < 3; c++)
    {
      r.min[c] = min ? min[c] : zero[c];
      r.max[c] = max ? max[c] : one[c];
    }
  for (k = 0; k < n; k++)
    for (j = 0; j < n; j++)
      for (i = 0; i < n; i++)
        {
          gint    idx[3] = { i, j, k };
          gdouble x[3];

          for (c = 0; c < 3; c++)
            x[c] = r.min[c] + (r.max[c] - r.min[c]) * idx[c] / (n - 1);
          tf (x, r.v + (((gsize) k * n + j) * n + i) * 3, data);
        }
  return r;
}

static const gdouble *
ref_at (const Ref *r, gint i, gint j, gint k)
{
  return r->v + (((gsize) k * r->n + j) * r->n + i) * 3;
}

/* the classic forms: trilinear as three stages of lerps, tetrahedral with
 * its six cases written out (Kasson, Nin and Plouffe 1995; the Truelight
 * paper) */
static void
ref_eval (const Ref     *r,
          const gdouble *x,
          gboolean       tetra,
          gboolean       extrap,
          gdouble       *out)
{
  gint    n = r->n, i[3], c;
  gdouble f[3];

  for (c = 0; c < 3; c++)
    {
      gdouble t = (x[c] - r->min[c]) / (r->max[c] - r->min[c]) * (n - 1);

      if (isnan (t))
        t = 0;
      if (extrap)
        t = CLAMP (t, -1e4 * (n - 1), (n - 1) * (1 + 1e4));
      else
        t = CLAMP (t, 0, n - 1);
      i[c] = CLAMP ((gint) floor (t), 0, n - 2);
      f[c] = t - i[c];
    }

  if (! tetra)
    {
      for (c = 0; c < 3; c++)
        {
          gdouble c00, c10, c01, c11, c0, c1;
#define V(a, b, d) ref_at (r, i[0] + a, i[1] + b, i[2] + d)[c]
          c00 = V (0, 0, 0) * (1 - f[0]) + V (1, 0, 0) * f[0];
          c10 = V (0, 1, 0) * (1 - f[0]) + V (1, 1, 0) * f[0];
          c01 = V (0, 0, 1) * (1 - f[0]) + V (1, 0, 1) * f[0];
          c11 = V (0, 1, 1) * (1 - f[0]) + V (1, 1, 1) * f[0];
#undef V
          c0 = c00 * (1 - f[1]) + c10 * f[1];
          c1 = c01 * (1 - f[1]) + c11 * f[1];
          out[c] = c0 * (1 - f[2]) + c1 * f[2];
        }
      return;
    }

  {
    const gdouble fr = f[0], fg = f[1], fb = f[2];
    const gdouble *c000 = ref_at (r, i[0],     i[1],     i[2]);
    const gdouble *c111 = ref_at (r, i[0] + 1, i[1] + 1, i[2] + 1);
    const gdouble *p1, *p2;
    gdouble        w0, w1, w2, w3;

    if (fr > fg)
      {
        if (fg > fb)
          {
            p1 = ref_at (r, i[0] + 1, i[1], i[2]);
            p2 = ref_at (r, i[0] + 1, i[1] + 1, i[2]);
            w0 = 1 - fr; w1 = fr - fg; w2 = fg - fb; w3 = fb;
          }
        else if (fr > fb)
          {
            p1 = ref_at (r, i[0] + 1, i[1], i[2]);
            p2 = ref_at (r, i[0] + 1, i[1], i[2] + 1);
            w0 = 1 - fr; w1 = fr - fb; w2 = fb - fg; w3 = fg;
          }
        else
          {
            p1 = ref_at (r, i[0], i[1], i[2] + 1);
            p2 = ref_at (r, i[0] + 1, i[1], i[2] + 1);
            w0 = 1 - fb; w1 = fb - fr; w2 = fr - fg; w3 = fg;
          }
      }
    else
      {
        if (fb > fg)
          {
            p1 = ref_at (r, i[0], i[1], i[2] + 1);
            p2 = ref_at (r, i[0], i[1] + 1, i[2] + 1);
            w0 = 1 - fb; w1 = fb - fg; w2 = fg - fr; w3 = fr;
          }
        else if (fb > fr)
          {
            p1 = ref_at (r, i[0], i[1] + 1, i[2]);
            p2 = ref_at (r, i[0], i[1] + 1, i[2] + 1);
            w0 = 1 - fg; w1 = fg - fb; w2 = fb - fr; w3 = fr;
          }
        else
          {
            p1 = ref_at (r, i[0], i[1] + 1, i[2]);
            p2 = ref_at (r, i[0] + 1, i[1] + 1, i[2]);
            w0 = 1 - fg; w1 = fg - fr; w2 = fr - fb; w3 = fb;
          }
      }
    for (c = 0; c < 3; c++)
      out[c] = w0 * c000[c] + w1 * p1[c] + w2 * p2[c] + w3 * c111[c];
  }
}

/* writing LUT files ------------------------------------------------------ */

static gchar *
tmp_path (const gchar *name)
{
  return g_build_filename (tmp_dir, name, NULL);
}

static gchar *
write_data (const gchar *name,
            const gchar *data,
            gssize       length)
{
  gchar *path = tmp_path (name);

  g_file_set_contents (path, data, length, NULL);
  return path;
}

typedef struct
{
  const gdouble *min, *max;  /* DOMAIN_MIN, DOMAIN_MAX, or NULL */
  const gchar   *header;     /* more lines before the table */
  const gchar   *eol;        /* "\n" if NULL */
  const gchar   *format;     /* of a line of numbers, "%.9g %.9g %.9g" */
} CubeStyle;

static gchar *
write_cube (const gchar     *name,
            gint             n,
            Transform        tf,
            gpointer         data,
            const CubeStyle *style)
{
  static const CubeStyle plain = { NULL, NULL, NULL, NULL, NULL };
  GString *s;
  gchar   *path;
  Ref      r;
  gsize    i;

  if (! style)
    style = &plain;
  r = ref_new (n, tf, data, style->min, style->max);
  s = g_string_new (NULL);
  g_string_append_printf (s, "TITLE \"%s\"%s", name, style->eol ? style->eol : "\n");
  if (style->header)
    g_string_append (s, style->header);
  g_string_append_printf (s, "LUT_3D_SIZE %d%s", n, style->eol ? style->eol : "\n");
  if (style->min)
    {
      gchar a[3][G_ASCII_DTOSTR_BUF_SIZE];
      gint  c;

      for (c = 0; c < 3; c++)
        g_ascii_dtostr (a[c], sizeof (a[c]), style->min[c]);
      g_string_append_printf (s, "DOMAIN_MIN %s %s %s%s", a[0], a[1], a[2],
                              style->eol ? style->eol : "\n");
      for (c = 0; c < 3; c++)
        g_ascii_dtostr (a[c], sizeof (a[c]), style->max[c]);
      g_string_append_printf (s, "DOMAIN_MAX %s %s %s%s", a[0], a[1], a[2],
                              style->eol ? style->eol : "\n");
    }
  for (i = 0; i < (gsize) n * n * n; i++)
    {
      gchar a[3][G_ASCII_DTOSTR_BUF_SIZE];
      gint  c;

      for (c = 0; c < 3; c++)
        g_ascii_formatd (a[c], sizeof (a[c]), "%.9g", r.v[i * 3 + c]);
      if (style->format)
        {
          gchar *line = g_strdup_printf (style->format, a[0], a[1], a[2]);
          g_string_append (s, line);
          g_free (line);
        }
      else
        g_string_append_printf (s, "%s %s %s", a[0], a[1], a[2]);
      g_string_append (s, style->eol ? style->eol : "\n");
    }
  path = write_data (name, s->str, s->len);
  g_string_free (s, TRUE);
  g_free (r.v);
  return path;
}

/* a 1D LUT: out[c] = f (x) per channel over [min, max] */
typedef gdouble (*Curve) (gdouble x, gint channel);

static gdouble
curve_mixed (gdouble x, gint c)
{
  return c == 0 ? x * x : c == 1 ? sqrt (MAX (x, 0.0)) : 1.0 - x;
}

static gdouble
curve_affine (gdouble x, gint c)
{
  return 0.2 + (0.5 + 0.1 * c) * x;
}

static gchar *
write_cube_1d (const gchar *name,
               gint         n,
               Curve        curve,
               gdouble      min,
               gdouble      max,
               gboolean     resolve_range)
{
  GString *s = g_string_new (NULL);
  gchar   *path;
  gint     i, c;

  g_string_append_printf (s, "# a 1D LUT\nLUT_1D_SIZE %d\n", n);
  if (resolve_range)
    g_string_append_printf (s, "LUT_1D_INPUT_RANGE %.9g %.9g\n", min, max);
  else if (min != 0.0 || max != 1.0)
    g_string_append_printf (s, "DOMAIN_MIN %.9g %.9g %.9g\n"
                               "DOMAIN_MAX %.9g %.9g %.9g\n",
                            min, min, min, max, max, max);
  for (i = 0; i < n; i++)
    {
      gdouble x = min + (max - min) * i / (n - 1);

      for (c = 0; c < 3; c++)
        g_string_append_printf (s, "%.9g%s", curve (x, c), c < 2 ? " " : "\n");
    }
  path = write_data (name, s->str, s->len);
  g_string_free (s, TRUE);
  return path;
}

/* the linear interpolation of a 1D LUT written by write_cube_1d */
static gdouble
curve_lerp (Curve curve, gint n, gdouble min, gdouble max, gdouble x, gint c)
{
  gdouble t = (x - min) / (max - min) * (n - 1);
  gint    i;

  t = isnan (t) ? 0 : CLAMP (t, 0, n - 1);
  i = CLAMP ((gint) floor (t), 0, n - 2);
  return curve (min + (max - min) * i / (n - 1), c) * (1 - (t - i)) +
         curve (min + (max - min) * (i + 1) / (n - 1), c) * (t - i);
}

typedef struct
{
  gint         out_bits;  /* the table's bit depth */
  const gint  *mesh;      /* the input grid in 10 bits, NULL: even */
  gboolean     no_mesh;   /* no grid line at all */
  gboolean     lustre;    /* 3DMESH, Mesh and the trailer */
  gint         mesh_bits; /* the Lustre "Mesh" input bits */
  const gchar *eol;
} Style3dl;

/* the points are at the grid's input values; blue changes fastest */
static gchar *
write_3dl (const gchar    *name,
           gint            n,
           Transform       tf,
           gpointer        data,
           const Style3dl *style)
{
  const gchar *eol  = style->eol ? style->eol : "\n";
  gdouble      top  = (1 << style->out_bits) - 1;
  GString     *s    = g_string_new ("# written by tests/check.c\n");
  gdouble     *grid = g_new (gdouble, n);
  gchar       *path;
  gint         i, j, k;

  if (style->lustre)
    g_string_append_printf (s, "3DMESH%sMesh %d %d%s", eol, style->mesh_bits,
                            style->out_bits, eol);
  for (i = 0; i < n; i++)
    {
      gint code = style->mesh ? style->mesh[i] :
                  (gint) floor (i * 1023.0 / (n - 1) + 0.5);

      grid[i] = style->mesh ? code / 1023.0 : (gdouble) i / (n - 1);
      if (! style->no_mesh)
        g_string_append_printf (s, "%d%s", code, i < n - 1 ? " " : eol);
    }
  g_string_append (s, eol);
  for (i = 0; i < n; i++)          /* red */
    for (j = 0; j < n; j++)        /* green */
      for (k = 0; k < n; k++)      /* blue */
        {
          gdouble x[3] = { grid[i], grid[j], grid[k] }, y[3];
          gint    c;

          tf (x, y, data);
          for (c = 0; c < 3; c++)
            g_string_append_printf (s, "%d%s",
                                    (gint) floor (CLAMP (y[c], 0, 1) * top + 0.5),
                                    c < 2 ? " " : eol);
        }
  if (style->lustre)
    g_string_append_printf (s, "%sLUT8%sgamma 1.0%s", eol, eol, eol);
  path = write_data (name, s->str, s->len);
  g_string_free (s, TRUE);
  g_free (grid);
  return path;
}

/* a Hald CLUT of level L: an image of L^3 x L^3 pixels, n = L^2 points per
 * axis, red fastest in raster order; kind "png8", "png16" or "tiff" */
static gchar *
write_hald (const gchar *name,
            gint         level,
            Transform    tf,
            gpointer     data,
            const gchar *kind)
{
  gint        n = level * level, w = level * level * level;
  gfloat     *px = g_new (gfloat, (gsize) w * w * 3);
  GeglBuffer *buffer;
  GeglNode   *graph, *src, *save;
  gchar      *path = tmp_path (name);
  gint        p;

  for (p = 0; p < w * w; p++)
    {
      gdouble x[3] = { (gdouble) (p % n) / (n - 1),
                       (gdouble) (p / n % n) / (n - 1),
                       (gdouble) (p / n / n) / (n - 1) }, y[3];
      gint    c;

      tf (x, y, data);
      for (c = 0; c < 3; c++)
        px[(gsize) p * 3 + c] = y[c];
    }
  buffer = gegl_buffer_new (GEGL_RECTANGLE (0, 0, w, w),
                            babl_format ("R'G'B' float"));
  gegl_buffer_set (buffer, NULL, 0, babl_format ("R'G'B' float"), px,
                   GEGL_AUTO_ROWSTRIDE);
  graph = gegl_node_new ();
  src   = gegl_node_new_child (graph, "operation", "gegl:buffer-source",
                               "buffer", buffer, NULL);
  if (g_str_has_prefix (kind, "png"))
    save = gegl_node_new_child (graph, "operation", "gegl:png-save",
                                "path", path,
                                "bitdepth", strcmp (kind, "png8") ? 16 : 8,
                                "compression", 1, NULL);
  else
    save = gegl_node_new_child (graph, "operation", "gegl:tiff-save",
                                "path", path, NULL);
  gegl_node_link (src, save);
  gegl_node_process (save);
  g_object_unref (graph);
  g_object_unref (buffer);
  g_free (px);
  return path;
}

/* running the operation -------------------------------------------------- */

typedef struct
{
  gfloat *px;      /* count RGBA pixels, in the format of the input */
  gchar  *error;   /* the error property, after the main loop ran */
} Result;

static void
result_free (Result *r)
{
  g_clear_pointer (&r->px, g_free);
  g_clear_pointer (&r->error, g_free);
}

/* runs the operation on count pixels (a row, or w x h if h > 1) in format
 * and returns the output in the same format */
static Result
run_valist (const gfloat *px,
            gint          w,
            gint          h,
            const Babl   *format,
            const gchar  *path,
            const gchar  *first_property,
            va_list       args)
{
  GeglBuffer *in;
  GeglNode   *graph, *src, *op;
  Result      r;
  gint        nc = babl_format_get_n_components (format);

  in = gegl_buffer_new (GEGL_RECTANGLE (0, 0, w, h), format);
  gegl_buffer_set (in, NULL, 0, format, px, GEGL_AUTO_ROWSTRIDE);
  graph = gegl_node_new ();
  src   = gegl_node_new_child (graph, "operation", "gegl:buffer-source",
                               "buffer", in, NULL);
  op    = gegl_node_new_child (graph, "operation", OP,
                               "path", path ? path : "", NULL);
  if (first_property)
    gegl_node_set_valist (op, first_property, args);
  gegl_node_link (src, op);

  r.px = g_new0 (gfloat, (gsize) w * h * nc);
  gegl_node_blit (op, 1.0, GEGL_RECTANGLE (0, 0, w, h), format, r.px,
                  GEGL_AUTO_ROWSTRIDE, GEGL_BLIT_DEFAULT);
  /* the error property is set from the main loop */
  while (g_main_context_iteration (NULL, FALSE));
  gegl_node_get (op, "error", &r.error, NULL);

  g_object_unref (graph);
  g_object_unref (in);
  return r;
}

static Result
run_fmt (const gfloat *px, gint count, const Babl *format, const gchar *path,
         const gchar *first_property, ...)
{
  va_list args;
  Result  r;

  va_start (args, first_property);
  r = run_valist (px, count, 1, format, path, first_property, args);
  va_end (args);
  return r;
}

/* the same in the working format */
static Result
run (const gfloat *px, gint count, const gchar *path,
     const gchar *first_property, ...)
{
  va_list args;
  Result  r;

  va_start (args, first_property);
  r = run_valist (px, count, 1, babl_format (WORK), path, first_property,
                  args);
  va_end (args);
  return r;
}

/* test pixels: all points of an n grid (or a sample of them when there
 * are many), random colors, and alpha of all kinds */
static gfloat *
test_pixels (gint n, gint n_random, guint32 seed, gint *count)
{
  GRand  *rand = g_rand_new_with_seed (seed);
  gint    n_grid = n > 0 ? MIN (n * n * n, 4000) : 0;
  gfloat *px = g_new (gfloat, (gsize) (n_grid + n_random) * 4);
  gint    p;

  for (p = 0; p < n_grid; p++)
    {
      gint idx = n * n * n <= 4000 ? p : g_rand_int_range (rand, 0, n * n * n);

      px[p * 4 + 0] = (gfloat) (idx % n) / (n - 1);
      px[p * 4 + 1] = (gfloat) (idx / n % n) / (n - 1);
      px[p * 4 + 2] = (gfloat) (idx / n / n) / (n - 1);
      px[p * 4 + 3] = (p % 3) * 0.5f;
    }
  for (; p < n_grid + n_random; p++)
    {
      px[p * 4 + 0] = g_rand_double (rand);
      px[p * 4 + 1] = g_rand_double (rand);
      px[p * 4 + 2] = g_rand_double (rand);
      px[p * 4 + 3] = g_rand_double (rand);
    }
  g_rand_free (rand);
  *count = n_grid + n_random;
  return px;
}

/* the largest difference of the RGB of out from the reference applied to
 * in (or from in itself when ref is NULL); alpha must be unchanged */
static gdouble
max_error (const gfloat *in, const gfloat *out, gint count,
           const Ref *ref, gboolean tetra, gboolean *alpha_ok)
{
  gdouble worst = 0;
  gint    p, c;

  if (alpha_ok)
    *alpha_ok = TRUE;
  for (p = 0; p < count; p++)
    {
      gdouble x[3] = { in[p * 4], in[p * 4 + 1], in[p * 4 + 2] }, y[3];

      if (ref)
        ref_eval (ref, x, tetra, FALSE, y);
      else
        memcpy (y, x, sizeof (x));
      for (c = 0; c < 3; c++)
        {
          gdouble d = fabs (out[p * 4 + c] - y[c]);

          if (! (d <= worst))
            worst = isnan (d) ? INFINITY : d;
        }
      if (alpha_ok && memcmp (&in[p * 4 + 3], &out[p * 4 + 3], sizeof (gfloat)))
        *alpha_ok = FALSE;
    }
  return worst;
}

static gboolean
all_finite (const gfloat *px, gint count)
{
  gint p, c;

  for (p = 0; p < count; p++)
    for (c = 0; c < 3; c++)
      if (! isfinite (px[p * 4 + c]))
        return FALSE;
  return TRUE;
}

static const gchar *interp_name[2] = { "trilinear", "tetrahedral" };

/* property values of the enums, as the operation numbers them */
enum { TETRAHEDRAL = 0, TRILINEAR = 1 };
enum { PERCEPTUAL = 0, LINEAR = 1 };
enum { SPACE_IMAGE = 0, SPACE_SRGB = 1 };
enum { CLAMP = 0, EXTRAPOLATE = 1 };

static gint
interp (gboolean tetra)
{
  return tetra ? TETRAHEDRAL : TRILINEAR;
}

/* the largest difference of out from tf (in) */
static gdouble
max_error_tf (const gfloat *in, const gfloat *out, gint count, Transform tf,
              gpointer data)
{
  gdouble worst = 0;
  gint    p, c;

  for (p = 0; p < count; p++)
    {
      gdouble x[3] = { in[p * 4], in[p * 4 + 1], in[p * 4 + 2] }, y[3];

      tf (x, y, data);
      for (c = 0; c < 3; c++)
        {
          gdouble d = fabs (out[p * 4 + c] - y[c]);

          if (! (d <= worst))
            worst = isnan (d) ? INFINITY : d;
        }
    }
  return worst;
}

static gboolean
same_bits (const gfloat *a, const gfloat *b, gint count)
{
  return memcmp (a, b, (gsize) count * 4 * sizeof (gfloat)) == 0;
}

/* identities --------------------------------------------------------------- */

static void
check_identity (const gchar *what, const gchar *path, gint n, gdouble tol)
{
  gint    count, t;
  gfloat *px = test_pixels (n, 3000, 7 + n, &count);

  for (t = 0; t < 2; t++)
    {
      Result   r = run (px, count, path, "interpolation", interp (t), NULL);
      gboolean alpha_ok;
      gdouble  e = max_error (px, r.px, count, NULL, FALSE, &alpha_ok);
      gchar   *name = g_strdup_printf ("identity_%s_%d_%s", what, n,
                                       interp_name[t]);

      report (name, e <= tol && alpha_ok && ! r.error[0],
              "max error %.2g (tolerance %.2g)%s%s", e, tol,
              alpha_ok ? "" : ", alpha changed", r.error);
      g_free (name);
      result_free (&r);
    }
  g_free (px);
}

static void
test_identity (void)
{
  static const gint sizes[] = { 2, 17, 33, 65 };
  static const gint levels[] = { 2, 4, 8 };
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (sizes); i++)
    {
      gint      n    = sizes[i];
      Style3dl  st   = { 16, NULL, FALSE, FALSE, 0, NULL };
      gchar    *name = g_strdup_printf ("identity-%d.cube", n);
      gchar    *path = write_cube (name, n, tf_identity, NULL, NULL);

      check_identity ("cube", path, n, 1e-6);
      g_free (path);
      g_free (name);

      /* 16 bit integers: within half a step */
      name = g_strdup_printf ("identity-%d.3dl", n);
      path = write_3dl (name, n, tf_identity, NULL, &st);
      check_identity ("3dl16", path, n, 0.5 / 65535 + 1e-6);
      g_free (path);
      g_free (name);
    }

  for (i = 0; i < G_N_ELEMENTS (levels); i++)
    {
      gint   l = levels[i];
      gchar *name, *path, *what;

      name = g_strdup_printf ("hald-%d.png", l);
      path = write_hald (name, l, tf_identity, NULL, "png16");
      check_identity ("hald_png16", path, l * l, 0.5 / 65535 + 1e-6);
      g_free (path);
      g_free (name);

      name = g_strdup_printf ("hald-%d.tif", l);
      path = write_hald (name, l, tf_identity, NULL, "tiff");
      check_identity ("hald_tiff_float", path, l * l, 1e-6);
      g_free (path);
      g_free (name);

      /* levels 2 and 4 are exact in 8 bits (255 / 3, 255 / 15) */
      name = g_strdup_printf ("hald-%d-8.png", l);
      path = write_hald (name, l, tf_identity, NULL, "png8");
      what = g_strdup ("hald_png8");
      check_identity (what, path, l * l, l < 8 ? 1e-6 : 0.5 / 255 + 1e-6);
      g_free (what);
      g_free (path);
      g_free (name);
    }
}

/* known transforms ------------------------------------------------------ */

static void
test_affine (void)
{
  static const struct { const gchar *name; Transform tf; } tfs[] =
    {
      { "swap",   tf_swap   },
      { "invert", tf_invert },
      { "matrix", tf_matrix },
    };
  gint    count;
  gfloat *px = test_pixels (5, 3000, 11, &count);
  gsize   i;
  gint    t, n;

  /* piecewise linear interpolation reproduces an affine function
   * everywhere, at and between the points, for any size */
  for (i = 0; i < G_N_ELEMENTS (tfs); i++)
    for (n = 2; n <= 5; n += 3)
      {
        gchar *file = g_strdup_printf ("%s-%d.cube", tfs[i].name, n);
        gchar *path = write_cube (file, n, tfs[i].tf, NULL, NULL);

        for (t = 0; t < 2; t++)
          {
            Result  r = run (px, count, path, "interpolation", interp (t), NULL);
            gdouble e = max_error_tf (px, r.px, count, tfs[i].tf, NULL);
            gchar  *name = g_strdup_printf ("affine_%s_%d_%s", tfs[i].name, n,
                                            interp_name[t]);

            report (name, e <= 2e-6, "max error %.2g", e);
            g_free (name);
            result_free (&r);
          }
        g_free (path);
        g_free (file);
      }
  g_free (px);
}

static void
test_gamma (void)
{
  const gint    n = 17;
  const gdouble h = 1.0 / (n - 1);
  /* |f''| of x^2.2 is 2.2 * 1.2 * x^0.2, at most 2.64 on 0 to 1: linear
   * interpolation is within h^2 / 8 * 2.64 of the curve */
  const gdouble bound = h * h / 8 * 2.2 * 1.2;
  gchar        *path = write_cube ("gamma-17.cube", n, tf_gamma, NULL, NULL);
  Ref           ref  = ref_new (n, tf_gamma, NULL, NULL, NULL);
  gint          count, n_grid, t;
  gfloat       *grid = test_pixels (n, 0, 3, &n_grid);
  gfloat       *px   = test_pixels (0, 20000, 4, &count);

  for (t = 0; t < 2; t++)
    {
      Result  g = run (grid, n_grid, path, "interpolation", interp (t), NULL);
      Result  r = run (px, count, path, "interpolation", interp (t), NULL);
      gdouble e_grid  = max_error_tf (grid, g.px, n_grid, tf_gamma, NULL);
      gdouble e_lerp  = max_error (px, r.px, count, &ref, t, NULL);
      gdouble e_curve = max_error_tf (px, r.px, count, tf_gamma, NULL);
      gchar  *name;

      name = g_strdup_printf ("gamma_at_points_%s", interp_name[t]);
      report (name, e_grid <= 1e-6, "max error %.2g", e_grid);
      g_free (name);
      /* a curve per channel: both interpolations are the 1D linear
       * interpolation between the neighboring points */
      name = g_strdup_printf ("gamma_between_points_is_linear_%s",
                              interp_name[t]);
      report (name, e_lerp <= 2e-6, "max error %.2g", e_lerp);
      g_free (name);
      name = g_strdup_printf ("gamma_within_interpolation_bound_%s",
                              interp_name[t]);
      report (name, e_curve <= bound + 1e-6 && e_curve > 0.5 * bound,
              "max error %.3g, bound h^2/8 max|f''| = %.3g", e_curve, bound);
      g_free (name);
      result_free (&g);
      result_free (&r);
    }
  g_free (ref.v);
  g_free (grid);
  g_free (px);
  g_free (path);
}

/* the products r g, g b, b r: trilinear interpolation is exact for them,
 * tetrahedral gives, in a cell of size h at (a0, b0) with fractions
 * (fa, fb), a0 b0 + h (a0 fb + b0 fa) + h^2 min (fa, fb) instead of
 * h^2 fa fb */
static void
test_product (void)
{
  const gint    n = 9;
  const gdouble h = 1.0 / (n - 1);
  gchar        *path = write_cube ("product-9.cube", n, tf_product, NULL, NULL);
  gint          count, n_grid, p, c;
  gfloat       *px   = test_pixels (0, 20000, 5, &count);
  gfloat       *grid = test_pixels (n, 0, 6, &n_grid);
  Result        tri  = run (px, count, path, "interpolation", TRILINEAR, NULL);
  Result        tet  = run (px, count, path, "interpolation", TETRAHEDRAL, NULL);
  Result        gtri = run (grid, n_grid, path, "interpolation", TRILINEAR, NULL);
  Result        gtet = run (grid, n_grid, path, "interpolation", TETRAHEDRAL, NULL);
  gdouble       e_tri = max_error_tf (px, tri.px, count, tf_product, NULL);
  gdouble       e_tet = 0, apart = 0, at_points = 0;

  for (p = 0; p < count; p++)
    for (c = 0; c < 3; c++)
      {
        gdouble a = px[p * 4 + c], b = px[p * 4 + (c + 1) % 3];
        gint    ia = MIN ((gint) floor (a / h), n - 2);
        gint    ib = MIN ((gint) floor (b / h), n - 2);
        gdouble fa = a / h - ia, fb = b / h - ib;
        gdouble a0 = ia * h, b0 = ib * h;
        gdouble want = a0 * b0 + h * (a0 * fb + b0 * fa) + h * h * MIN (fa, fb);

        e_tet = MAX (e_tet, fabs (tet.px[p * 4 + c] - want));
        apart = MAX (apart, fabs (tet.px[p * 4 + c] - tri.px[p * 4 + c]));
      }
  for (p = 0; p < n_grid * 4; p++)
    if (p % 4 != 3)
      at_points = MAX (at_points, fabs (gtet.px[p] - gtri.px[p]));

  report ("product_trilinear_exact", e_tri <= 2e-6, "max error %.2g", e_tri);
  report ("product_tetrahedral_as_predicted", e_tet <= 2e-6,
          "max error from h^2 min (fa, fb) %.2g", e_tet);
  /* the difference h^2 (min (fa, fb) - fa fb) is largest, h^2 / 4, at
   * fa = fb = 1/2 */
  report ("tetrahedral_and_trilinear_differ_between_points",
          apart > 0.8 * h * h / 4 && apart <= h * h / 4 + 1e-6,
          "largest difference %.4g, h^2/4 = %.4g", apart, h * h / 4);
  report ("tetrahedral_and_trilinear_agree_at_points", at_points <= 1e-6,
          "largest difference %.2g", at_points);

  result_free (&tri);
  result_free (&tet);
  result_free (&gtri);
  result_free (&gtet);
  g_free (px);
  g_free (grid);
  g_free (path);
}

/* a colorful grade against the reference interpolation, both methods */
static void
test_reference (void)
{
  static const gint sizes[] = { 2, 17, 33, 65 };
  gsize i;
  gint  t;

  for (i = 0; i < G_N_ELEMENTS (sizes); i++)
    {
      gint    n = sizes[i], count;
      gchar  *file = g_strdup_printf ("grade-%d.cube", n);
      gchar  *path = write_cube (file, n, tf_grade, NULL, NULL);
      Ref     ref  = ref_new (n, tf_grade, NULL, NULL, NULL);
      gfloat *px   = test_pixels (n, 5000, 20 + n, &count);

      for (t = 0; t < 2; t++)
        {
          Result   r = run (px, count, path, "interpolation", interp (t), NULL);
          gboolean alpha_ok;
          gdouble  e = max_error (px, r.px, count, &ref, t, &alpha_ok);
          gchar   *name = g_strdup_printf ("reference_grade_%d_%s", n,
                                           interp_name[t]);

          report (name, e <= 2e-6 && alpha_ok, "max error %.2g", e);
          g_free (name);
          result_free (&r);
        }
      g_free (ref.v);
      g_free (px);
      g_free (path);
      g_free (file);
    }
}

/* 1D LUTs ------------------------------------------------------------------- */

static gdouble
curve_tonemap (gdouble x, gint c)
{
  return x / (1.0 + x) * (1.0 + 0.1 * c);
}

static gdouble
curve_identity (gdouble x, gint c)
{
  (void) c;
  return x;
}

static void
check_1d (const gchar *name, Curve curve, gint n, gdouble min, gdouble max,
          gboolean resolve, gdouble lo, gdouble hi)
{
  gchar  *file = g_strdup_printf ("%s.cube", name);
  gchar  *path = write_cube_1d (file, n, curve, min, max, resolve);
  gint    count, p, c;
  gfloat *px   = test_pixels (0, 5000, 30, &count);
  Result  tet, tri;
  gdouble e = 0;

  for (p = 0; p < count; p++)
    for (c = 0; c < 3; c++)
      px[p * 4 + c] = lo + (hi - lo) * px[p * 4 + c];
  tet = run (px, count, path, "interpolation", TETRAHEDRAL, NULL);
  tri = run (px, count, path, "interpolation", TRILINEAR, NULL);
  for (p = 0; p < count; p++)
    for (c = 0; c < 3; c++)
      e = MAX (e, fabs (tet.px[p * 4 + c] -
                        curve_lerp (curve, n, min, max, px[p * 4 + c], c)));
  report (name, e <= 2e-6 * MAX (1, fabs (hi)) && same_bits (tet.px, tri.px, count),
          "max error %.2g%s", e,
          same_bits (tet.px, tri.px, count) ? "" : ", interpolation changed it");
  result_free (&tet);
  result_free (&tri);
  g_free (px);
  g_free (path);
  g_free (file);
}

static void
test_1d (void)
{
  check_1d ("lut1d_mixed_curves_1024", curve_mixed, 1024, 0, 1, FALSE, 0, 1);
  check_1d ("lut1d_mixed_curves_7", curve_mixed, 7, 0, 1, FALSE, 0, 1);
  check_1d ("lut1d_affine_2", curve_affine, 2, 0, 1, FALSE, 0, 1);
  check_1d ("lut1d_domain_-1_to_2", curve_mixed, 64, -1, 2, FALSE, -1, 2);
  check_1d ("lut1d_resolve_input_range_0_to_4", curve_tonemap, 256, 0, 4,
            TRUE, 0, 4);
  check_1d ("lut1d_65536_points", curve_tonemap, 65536, 0, 1, FALSE, 0, 1);
}

/* input ranges ------------------------------------------------------------ */

static void
test_domain (void)
{
  const gdouble min[3] = { -0.5, 0.0, 0.25 }, max[3] = { 1.5, 2.0, 0.75 };
  CubeStyle     st     = { min, max, NULL, NULL, NULL };
  gchar        *path   = write_cube ("domain.cube", 9, tf_matrix, NULL, &st);
  gint          count, p, c, e_mode;
  gfloat       *px     = test_pixels (0, 5000, 40, &count);

  /* inside, and up to half the range beyond each edge */
  for (p = 0; p < count; p++)
    for (c = 0; c < 3; c++)
      px[p * 4 + c] = min[c] + (max[c] - min[c]) * (2 * px[p * 4 + c] - 0.5);

  for (e_mode = 0; e_mode < 2; e_mode++)
    {
      Result  r = run (px, count, path, "out-of-range", e_mode, NULL);
      gdouble e_in = 0, e_out = 0;

      for (p = 0; p < count; p++)
        {
          gdouble  x[3], y[3];
          gboolean inside = TRUE;

          for (c = 0; c < 3; c++)
            {
              x[c] = px[p * 4 + c];
              if (x[c] < min[c] || x[c] > max[c])
                inside = FALSE;
              if (e_mode == CLAMP)
                x[c] = CLAMP (x[c], min[c], max[c]);
            }
          tf_matrix (x, y, NULL);
          for (c = 0; c < 3; c++)
            {
              gdouble d = fabs (r.px[p * 4 + c] - y[c]);

              if (inside)
                e_in = MAX (e_in, d);
              else
                e_out = MAX (e_out, d);
            }
        }
      if (e_mode == CLAMP)
        report ("domain_min_max_inside", e_in <= 3e-6, "max error %.2g", e_in);
      report (e_mode == CLAMP ? "domain_min_max_outside_clamped"
                              : "domain_min_max_outside_extrapolated",
              e_out <= 3e-6, "max error %.2g", e_out);
      result_free (&r);
    }
  g_free (path);

  /* Resolve's LUT_3D_INPUT_RANGE, one range for the three channels */
  {
    const gdouble rmin[3] = { -0.25, -0.25, -0.25 }, rmax[3] = { 1.25, 1.25, 1.25 };
    Ref     ref = ref_new (9, tf_grade, NULL, rmin, rmax);
    GString *s  = g_string_new ("LUT_3D_SIZE 9\nLUT_3D_INPUT_RANGE -0.25 1.25\n");
    Result   r;
    gdouble  e;
    gsize    i;

    for (i = 0; i < 9 * 9 * 9 * 3; i++)
      g_string_append_printf (s, "%.9g%s", ref.v[i], i % 3 == 2 ? "\n" : " ");
    path = write_data ("resolve-range.cube", s->str, s->len);
    GRand   *rand = g_rand_new_with_seed (41);

    for (p = 0; p < count; p++)
      for (c = 0; c < 3; c++)
        px[p * 4 + c] = -0.25 + 1.5 * g_rand_double (rand);
    g_rand_free (rand);
    r = run (px, count, path, NULL);
    e = max_error (px, r.px, count, &ref, TRUE, NULL);
    report ("resolve_lut_3d_input_range", e <= 2e-6, "max error %.2g", e);
    result_free (&r);
    g_string_free (s, TRUE);
    g_free (ref.v);
    g_free (path);
  }

  /* a Resolve 1D shaper followed by a 3D LUT: the shaper halves 0 to 2,
   * the 3D LUT doubles 0 to 1: together the identity on 0 to 2 */
  {
    GString *s = g_string_new ("# shaper and cube\nLUT_1D_SIZE 5\n"
                               "LUT_1D_INPUT_RANGE 0 2\nLUT_3D_SIZE 3\n"
                               "LUT_3D_INPUT_RANGE 0 1\n");
    Result   r;
    gdouble  e;
    gint     i, j, k;

    for (i = 0; i < 5; i++)
      g_string_append_printf (s, "%g %g %g\n", i / 4.0, i / 4.0, i / 4.0);
    for (k = 0; k < 3; k++)
      for (j = 0; j < 3; j++)
        for (i = 0; i < 3; i++)
          g_string_append_printf (s, "%g %g %g\n", i * 1.0, j * 1.0, k * 1.0);
    path = write_data ("shaper.cube", s->str, s->len);
    GRand   *rand = g_rand_new_with_seed (42);

    for (p = 0; p < count; p++)
      for (c = 0; c < 3; c++)
        px[p * 4 + c] = 2.0 * g_rand_double (rand);
    g_rand_free (rand);
    r = run (px, count, path, NULL);
    e = max_error (px, r.px, count, NULL, FALSE, NULL);
    report ("resolve_1d_shaper_then_3d", e <= 2e-6, "max error %.2g", e);
    result_free (&r);
    g_string_free (s, TRUE);
    g_free (path);
  }
  g_free (px);
}

/* values outside the range, NaN, infinities -------------------------------- */

static void
test_out_of_range (void)
{
  static const gfloat special[] =
    { -1.0f, -1e-3f, 0.5f, 1.001f, 5.0f, 1e30f, -1e30f, NAN, INFINITY,
      -INFINITY };
  const gint ns    = G_N_ELEMENTS (special);
  const gint count = ns * ns;
  gfloat    *px    = g_new (gfloat, count * 4);
  gchar     *id3   = write_cube ("oor-identity.cube", 17, tf_identity, NULL, NULL);
  gchar     *inv3  = write_cube ("oor-invert.cube", 17, tf_invert, NULL, NULL);
  gchar     *id1   = write_cube_1d ("oor-1d.cube", 5, curve_identity, 0, 1, FALSE);
  const gchar *paths[3] = { id3, inv3, id1 };
  const gchar *names[3] = { "3d_identity", "3d_invert", "1d_identity" };
  gint       i, j, m, e_mode, t;

  for (i = 0; i < ns; i++)
    for (j = 0; j < ns; j++)
      {
        gfloat *p = px + (i * ns + j) * 4;

        p[0] = special[i];
        p[1] = special[j];
        p[2] = special[(i + j) % ns];
        p[3] = 1.0f;
      }

  for (m = 0; m < 3; m++)
    for (e_mode = 0; e_mode < 2; e_mode++)
      for (t = 0; t < 2; t++)
        {
          Result  r = run (px, count, paths[m], "out-of-range", e_mode,
                           "interpolation", interp (t), NULL);
          gdouble e = 0;
          gchar  *name;
          gint    p, c;

          for (p = 0; p < count; p++)
            for (c = 0; c < 3; c++)
              {
                gdouble x = px[p * 4 + c], want;

                /* NaN is the lowest point; extrapolation reaches 10^4
                 * times the range beyond each edge */
                if (isnan (x))
                  x = 0;
                x = e_mode == CLAMP ? CLAMP (x, 0, 1) : CLAMP (x, -1e4, 1 + 1e4);
                want = m == 1 ? 1 - x : x;
                e = MAX (e, fabs (r.px[p * 4 + c] - want) / MAX (1, fabs (want)));
              }
          name = g_strdup_printf ("out_of_range_%s_%s_%s", names[m],
                                  e_mode == CLAMP ? "clamp" : "extrapolate",
                                  interp_name[t]);
          report (name, all_finite (r.px, count) && e <= 2e-6,
                  "largest relative error %.2g%s", e,
                  all_finite (r.px, count) ? "" : ", not finite");
          g_free (name);
          result_free (&r);
        }

  /* mixed with the input, NaN and infinities mix as the edges */
  for (e_mode = 0; e_mode < 2; e_mode++)
    {
      Result r = run (px, count, inv3, "strength", 0.5,
                      "out-of-range", e_mode, NULL);

      report (e_mode == CLAMP ? "out_of_range_strength_half_finite_clamp"
                              : "out_of_range_strength_half_finite_extrapolate",
              all_finite (r.px, count), NULL);
      result_free (&r);
    }

  g_free (id3);
  g_free (inv3);
  g_free (id1);
  g_free (px);
}

/* alpha --------------------------------------------------------------------- */

static void
test_alpha (void)
{
  static const gfloat alphas[] =
    { 0.0f, 0.25f, 1.0f, 2.0f, -1.0f, -0.0f, NAN, INFINITY, 1e-30f };
  gint    count, p;
  gfloat *px   = test_pixels (0, 900, 50, &count);
  gchar  *p3   = write_cube ("alpha-3d.cube", 9, tf_grade, NULL, NULL);
  gchar  *p1   = write_cube_1d ("alpha-1d.cube", 16, curve_mixed, 0, 1, FALSE);
  gboolean ok  = TRUE;
  Result   r[6];
  gint     i;

  for (p = 0; p < count; p++)
    px[p * 4 + 3] = alphas[p % G_N_ELEMENTS (alphas)];

  r[0] = run (px, count, p3, "interpolation", TETRAHEDRAL, NULL);
  r[1] = run (px, count, p3, "interpolation", TRILINEAR, NULL);
  r[2] = run (px, count, p1, NULL);
  r[3] = run (px, count, p3, "strength", 0.5, NULL);
  r[4] = run (px, count, p3, "out-of-range", EXTRAPOLATE, NULL);
  r[5] = run (px, count, p3, "strength", 0.0, NULL);
  for (i = 0; i < 6; i++)
    {
      for (p = 0; p < count; p++)
        if (memcmp (&px[p * 4 + 3], &r[i].px[p * 4 + 3], sizeof (gfloat)))
          ok = FALSE;
      result_free (&r[i]);
    }
  report ("alpha_unchanged_to_the_bit", ok,
          "0, 1/4, 1, 2, -1, -0, NaN, infinity, 1e-30; 3D, 1D, strength, "
          "extrapolation");

  /* through a conversion (linear encoding of perceptual pixels) the
   * values stay */
  {
    Result rl = run (px, count, p3, "encoding", LINEAR, NULL);

    ok = TRUE;
    for (p = 0; p < count; p++)
      {
        gfloat a = px[p * 4 + 3], b = rl.px[p * 4 + 3];

        if (! (a == b || (isnan (a) && isnan (b))))
          ok = FALSE;
      }
    report ("alpha_unchanged_through_babl_conversion", ok, NULL);
    result_free (&rl);
  }
  g_free (px);
  g_free (p1);
  g_free (p3);
}

/* strength ------------------------------------------------------------------ */

static void
test_strength (void)
{
  gint    count, p, c;
  gfloat *px   = test_pixels (0, 3000, 60, &count);
  gchar  *inv  = write_cube ("strength-invert.cube", 17, tf_invert, NULL, NULL);
  gchar  *gam  = write_cube ("strength-gamma.cube", 17, tf_gamma, NULL, NULL);
  Ref     ref  = ref_new (17, tf_gamma, NULL, NULL, NULL);
  Result  r0, r1, rh, rq;
  gdouble e1, eh = 0, eq = 0;

  /* one pixel that is not a number, which strength 0 leaves alone */
  px[0] = NAN;
  px[5] = INFINITY;

  r0 = run (px, count, inv, "strength", 0.0, NULL);
  r1 = run (px + 8, count - 2, inv, "strength", 1.0, NULL);
  rh = run (px + 8, count - 2, inv, "strength", 0.5, NULL);
  rq = run (px + 8, count - 2, gam, "strength", 0.25, NULL);
  e1 = max_error_tf (px + 8, r1.px, count - 2, tf_invert, NULL);
  for (p = 0; p < count - 2; p++)
    {
      gdouble x[3] = { px[8 + p * 4], px[8 + p * 4 + 1], px[8 + p * 4 + 2] }, y[3];

      ref_eval (&ref, x, TRUE, FALSE, y);
      for (c = 0; c < 3; c++)
        {
          eh = MAX (eh, fabs (rh.px[p * 4 + c] - 0.5));
          eq = MAX (eq, fabs (rq.px[p * 4 + c] - (0.75 * x[c] + 0.25 * y[c])));
        }
    }
  report ("strength_0_is_the_input_to_the_bit", same_bits (px, r0.px, count),
          NULL);
  report ("strength_1_is_the_lut", e1 <= 1e-6, "max error %.2g", e1);
  report ("strength_half_of_invert_is_gray", eh <= 1e-6, "max error %.2g", eh);
  report ("strength_quarter_mixes_linearly", eq <= 2e-6, "max error %.2g", eq);

  result_free (&r0);
  result_free (&r1);
  result_free (&rh);
  result_free (&rq);
  g_free (ref.v);
  g_free (px);
  g_free (inv);
  g_free (gam);
}

/* encoding and color space ------------------------------------------------ */

static gdouble
srgb_encode (gdouble v)
{
  return v <= 0.0031308 ? 12.92 * v : 1.055 * pow (v, 1 / 2.4) - 0.055;
}

static gdouble
srgb_decode (gdouble v)
{
  return v <= 0.04045 ? v / 12.92 : pow ((v + 0.055) / 1.055, 2.4);
}

static void
convert (const gchar *from, const Babl *from_space, const gchar *to,
         const Babl *to_space, const gfloat *in, gfloat *out, gint count)
{
  babl_process (babl_fish (babl_format_with_space (from, from_space),
                           babl_format_with_space (to, to_space)),
                in, out, count);
}

static void
test_encoding (void)
{
  const Babl *lin  = babl_format ("RGBA float");
  gint        count, p, c;
  gfloat     *px   = test_pixels (0, 5000, 70, &count);
  gchar      *path = write_cube ("encoding.cube", 33, tf_grade, NULL, NULL);
  Ref         ref  = ref_new (33, tf_grade, NULL, NULL, NULL);
  gfloat     *enc  = g_new (gfloat, count * 4);
  gfloat     *want = g_new (gfloat, count * 4);
  Result      rp, rl, rm;
  gdouble     e_babl = 0, e_formula = 0, e_lin = 0, apart = 0, e_mix = 0;

  /* the input is linear light; perceptual: the LUT sees the sRGB curve */
  rp = run_fmt (px, count, lin, path, "encoding", PERCEPTUAL, NULL);
  rl = run_fmt (px, count, lin, path, "encoding", LINEAR, NULL);

  convert ("RGBA float", NULL, "R'G'B'A float", NULL, px, enc, count);
  for (p = 0; p < count; p++)
    {
      gdouble x[3] = { enc[p * 4], enc[p * 4 + 1], enc[p * 4 + 2] }, y[3];
      gdouble xf[3], yf[3];

      ref_eval (&ref, x, TRUE, FALSE, y);
      for (c = 0; c < 3; c++)
        want[p * 4 + c] = y[c];
      want[p * 4 + 3] = px[p * 4 + 3];

      /* the same with the sRGB formulas written here */
      for (c = 0; c < 3; c++)
        xf[c] = srgb_encode (px[p * 4 + c]);
      ref_eval (&ref, xf, TRUE, FALSE, yf);
      for (c = 0; c < 3; c++)
        e_formula = MAX (e_formula, fabs (rp.px[p * 4 + c] - srgb_decode (yf[c])));

      {
        gdouble xl[3] = { px[p * 4], px[p * 4 + 1], px[p * 4 + 2] }, yl[3];

        ref_eval (&ref, xl, TRUE, FALSE, yl);
        for (c = 0; c < 3; c++)
          {
            e_lin = MAX (e_lin, fabs (rl.px[p * 4 + c] - yl[c]));
            apart = MAX (apart, fabs (rl.px[p * 4 + c] - rp.px[p * 4 + c]));
          }
      }
    }
  convert ("R'G'B'A float", NULL, "RGBA float", NULL, want, want, count);
  for (p = 0; p < count; p++)
    for (c = 0; c < 3; c++)
      e_babl = MAX (e_babl, fabs (rp.px[p * 4 + c] - want[p * 4 + c]));

  report ("encoding_perceptual_applies_to_srgb_curve_values", e_babl <= 2e-6,
          "max error against babl %.2g, against the sRGB formulas %.2g",
          e_babl, e_formula);
  report ("encoding_perceptual_matches_srgb_formulas", e_formula <= 1e-4,
          "max error %.2g", e_formula);
  report ("encoding_linear_applies_to_linear_values", e_lin <= 2e-6,
          "max error %.2g", e_lin);
  report ("encodings_differ", apart > 0.05, "largest difference %.3g", apart);

  /* strength mixes in the LUT's encoding: perceptual pixels, linear LUT */
  rm = run (px, count, path, "encoding", LINEAR, "strength", 0.5, NULL);
  convert ("R'G'B'A float", NULL, "RGBA float", NULL, px, enc, count);
  for (p = 0; p < count; p++)
    {
      gdouble x[3] = { enc[p * 4], enc[p * 4 + 1], enc[p * 4 + 2] }, y[3];

      ref_eval (&ref, x, TRUE, FALSE, y);
      for (c = 0; c < 3; c++)
        want[p * 4 + c] = 0.5 * x[c] + 0.5 * y[c];
      want[p * 4 + 3] = px[p * 4 + 3];
    }
  convert ("RGBA float", NULL, "R'G'B'A float", NULL, want, want, count);
  for (p = 0; p < count; p++)
    for (c = 0; c < 3; c++)
      e_mix = MAX (e_mix, fabs (rm.px[p * 4 + c] - want[p * 4 + c]));
  report ("strength_mixes_in_the_lut_encoding", e_mix <= 2e-6,
          "max error %.2g", e_mix);

  result_free (&rp);
  result_free (&rl);
  result_free (&rm);
  g_free (ref.v);
  g_free (enc);
  g_free (want);
  g_free (px);
  g_free (path);
}

static void
test_space (void)
{
  const Babl *rec2020 = babl_space ("Rec2020");
  const Babl *fmt;
  gint        count, p, c;
  gfloat     *px, *tmp, *want;
  gchar      *path, *id;
  Ref         ref;
  Result      ri, rs;
  gdouble     e_image = 0, e_srgb = 0, apart = 0;

  if (! rec2020)
    {
      report ("lut_space", FALSE, "babl has no Rec2020 space");
      return;
    }
  fmt  = babl_format_with_space (WORK, rec2020);
  px   = test_pixels (0, 5000, 80, &count);
  tmp  = g_new (gfloat, count * 4);
  want = g_new (gfloat, count * 4);
  path = write_cube ("space.cube", 17, tf_grade, NULL, NULL);
  ref  = ref_new (17, tf_grade, NULL, NULL, NULL);

  /* the image's space: the LUT sees the Rec. 2020 values */
  ri = run_fmt (px, count, fmt, path, "lut-space", SPACE_IMAGE, NULL);
  e_image = max_error (px, ri.px, count, &ref, TRUE, NULL);
  report ("lut_space_image_uses_the_image_values", e_image <= 2e-6,
          "Rec. 2020 image, max error %.2g", e_image);

  /* sRGB: converted to sRGB, the LUT, converted back */
  rs = run_fmt (px, count, fmt, path, "lut-space", SPACE_SRGB, NULL);
  convert (WORK, rec2020, WORK, NULL, px, tmp, count);
  for (p = 0; p < count; p++)
    {
      gdouble x[3] = { tmp[p * 4], tmp[p * 4 + 1], tmp[p * 4 + 2] }, y[3];

      ref_eval (&ref, x, TRUE, FALSE, y);
      for (c = 0; c < 3; c++)
        tmp[p * 4 + c] = y[c];
    }
  convert (WORK, NULL, WORK, rec2020, tmp, want, count);
  for (p = 0; p < count; p++)
    for (c = 0; c < 3; c++)
      {
        e_srgb = MAX (e_srgb, fabs (rs.px[p * 4 + c] - want[p * 4 + c]));
        apart  = MAX (apart, fabs (rs.px[p * 4 + c] - ri.px[p * 4 + c]));
      }
  report ("lut_space_srgb_converts_there_and_back", e_srgb <= 1e-5,
          "max error %.2g", e_srgb);
  report ("lut_spaces_differ", apart > 0.02, "largest difference %.3g", apart);
  result_free (&ri);
  result_free (&rs);

  /* Rec. 2020 green is outside sRGB: an identity LUT in sRGB clamps it,
   * unless it extrapolates */
  id = write_cube ("space-identity.cube", 17, tf_identity, NULL, NULL);
  {
    gfloat green[8] = { 0.0f, 1.0f, 0.0f, 1.0f, 0.3f, 0.6f, 0.2f, 1.0f };
    Result rc = run_fmt (green, 2, fmt, id, "lut-space", SPACE_SRGB, NULL);
    Result re = run_fmt (green, 2, fmt, id, "lut-space", SPACE_SRGB,
                         "out-of-range", EXTRAPOLATE, NULL);
    gdouble dc = fabs (rc.px[0] - green[0]) + fabs (rc.px[2] - green[2]);
    gdouble de = 0;

    for (c = 0; c < 8; c++)
      if (c % 4 != 3)
        de = MAX (de, fabs (re.px[c] - green[c]));
    report ("lut_space_srgb_clamps_colors_outside_srgb", dc > 0.05,
            "Rec. 2020 green moved by %.3g", dc);
    report ("lut_space_srgb_extrapolation_keeps_them", de <= 1e-4,
            "max difference %.2g", de);
    result_free (&rc);
    result_free (&re);
  }

  g_free (ref.v);
  g_free (tmp);
  g_free (want);
  g_free (px);
  g_free (path);
  g_free (id);
}

/* .3dl ---------------------------------------------------------------------- */

static void
tf_scale (const gdouble *in, gdouble *out, gpointer data)
{
  gdouble k = *(const gdouble *) data;

  out[0] = k * in[0]; out[1] = k * in[1]; out[2] = k * in[2];
}

static void
check_3dl (const gchar *name, const gchar *path, gint n_grid_of, Transform tf,
           gpointer data, gdouble tol)
{
  gint    count, t;
  gfloat *px = test_pixels (n_grid_of, 3000, 90, &count);

  for (t = 0; t < 2; t++)
    {
      Result  r = run (px, count, path, "interpolation", interp (t), NULL);
      gdouble e = max_error_tf (px, r.px, count, tf, data);
      gchar  *full = g_strdup_printf ("%s_%s", name, interp_name[t]);

      report (full, e <= tol && ! r.error[0], "max error %.2g (tolerance %.2g)%s%s",
              e, tol, r.error[0] ? ", " : "", r.error);
      g_free (full);
      result_free (&r);
    }
  g_free (px);
}

/* piecewise linear interpolation of x^2.2 on the uneven grid */
static void
tf_gamma_on_mesh (const gdouble *in, gdouble *out, gpointer data)
{
  const gint *mesh = data;   /* 4 points in 10 bits */
  gint        c, i;

  for (c = 0; c < 3; c++)
    {
      gdouble x = CLAMP (in[c], 0, 1);
      gdouble a, b, f, ya, yb;

      /* the cell of x: the last whose start is below it */
      i = 0;
      while (i < 2 && x > mesh[i + 1] / 1023.0)
        i++;
      a  = mesh[i] / 1023.0;
      b  = mesh[i + 1] / 1023.0;
      f  = (x - a) / (b - a);
      ya = floor (pow (a, 2.2) * 65535 + 0.5) / 65535;
      yb = floor (pow (b, 2.2) * 65535 + 0.5) / 65535;
      out[c] = ya * (1 - f) + yb * f;
    }
}

static void
test_3dl (void)
{
  static const gint uneven[4] = { 0, 100, 300, 1023 };
  gdouble           k = 0.4;
  gchar            *path;

  {
    Style3dl st = { 12, NULL, FALSE, FALSE, 0, NULL };
    path = write_3dl ("id-12.3dl", 17, tf_identity, NULL, &st);
    check_3dl ("3dl_12_bit_identity_17", path, 17, tf_identity, NULL,
               0.5 / 4095 + 1e-6);
    g_free (path);
  }
  {
    Style3dl st = { 10, NULL, FALSE, FALSE, 0, NULL };
    path = write_3dl ("id-10.3dl", 17, tf_identity, NULL, &st);
    check_3dl ("3dl_10_bit_identity_17", path, 17, tf_identity, NULL,
               0.5 / 1023 + 1e-6);
    g_free (path);
  }
  {
    /* the largest value, 1638, alone would read as 10 bits: the Lustre
     * Mesh line says 12 */
    Style3dl st = { 12, NULL, FALSE, TRUE, 4, NULL };
    path = write_3dl ("lustre.3dl", 17, tf_scale, &k, &st);
    check_3dl ("3dl_lustre_mesh_line_gives_bit_depth", path, 17, tf_scale, &k,
               0.5 / 4095 + 1e-6);
    g_free (path);
  }
  {
    /* without it the depth comes from the largest value, as OpenColorIO
     * does: 1638 is taken as 10 bits */
    Style3dl st = { 12, NULL, FALSE, FALSE, 0, NULL };
    gdouble  k10 = 0.4 * 4095 / 1023;
    path = write_3dl ("flame-0.4.3dl", 17, tf_scale, &k, &st);
    check_3dl ("3dl_bit_depth_from_largest_value", path, 17, tf_scale, &k10,
               0.5 / 1023 + 1e-6);
    g_free (path);
  }
  {
    Style3dl st = { 16, NULL, TRUE, FALSE, 0, NULL };
    path = write_3dl ("no-mesh.3dl", 17, tf_identity, NULL, &st);
    check_3dl ("3dl_without_grid_line", path, 17, tf_identity, NULL,
               0.5 / 65535 + 1e-6);
    g_free (path);
  }
  {
    Style3dl st = { 12, NULL, FALSE, TRUE, 3, "\r\n" };
    path = write_3dl ("crlf.3dl", 9, tf_swap, NULL, &st);
    check_3dl ("3dl_crlf_lustre_swap", path, 9, tf_swap, NULL, 0.5 / 4095 + 1e-6);
    g_free (path);
  }
  {
    /* an uneven grid places the points at its input values: the identity
     * there is the identity everywhere, and a curve is interpolated
     * between those points */
    Style3dl st = { 16, uneven, FALSE, FALSE, 0, NULL };
    path = write_3dl ("uneven-id.3dl", 4, tf_identity, NULL, &st);
    check_3dl ("3dl_uneven_grid_identity", path, 0, tf_identity, NULL,
               0.5 / 65535 + 1e-6);
    g_free (path);
    path = write_3dl ("uneven-gamma.3dl", 4, tf_gamma, NULL, &st);
    check_3dl ("3dl_uneven_grid_curve", path, 0, tf_gamma_on_mesh,
               (gpointer) uneven, 1e-6);
    g_free (path);
  }
  {
    /* three points: the grid line reads like a triplet */
    Style3dl st = { 16, NULL, FALSE, FALSE, 0, NULL };
    path = write_3dl ("three.3dl", 3, tf_swap, NULL, &st);
    check_3dl ("3dl_grid_of_three_points", path, 3, tf_swap, NULL,
               0.5 / 65535 + 1e-6);
    g_free (path);
  }
  {
    Style3dl st = { 16, NULL, FALSE, FALSE, 0, NULL };
    path = write_3dl ("two.3dl", 2, tf_invert, NULL, &st);
    check_3dl ("3dl_grid_of_two_points", path, 2, tf_invert, NULL, 1e-6);
    g_free (path);
  }
}

/* .cube variants ----------------------------------------------------------- */

static void
check_cube_text (const gchar *name, const gchar *text, Transform tf)
{
  gchar  *file = g_strdup_printf ("%s.cube", name);
  gchar  *path = write_data (file, text, -1);
  gint    count;
  gfloat *px   = test_pixels (2, 500, 100, &count);
  Result  r    = run (px, count, path, NULL);
  gdouble e    = max_error_tf (px, r.px, count, tf, NULL);

  report (name, e <= 1e-6 && ! r.error[0], "max error %.2g%s%s", e,
          r.error[0] ? ", " : "", r.error);
  result_free (&r);
  g_free (px);
  g_free (path);
  g_free (file);
}

static void
test_cube_variants (void)
{
  CubeStyle st = { NULL, NULL, NULL, NULL, NULL };
  gchar    *path;
  gint      count;
  gfloat   *px = test_pixels (5, 500, 101, &count);

  {
    struct { const gchar *name; CubeStyle style; } styles[] =
      {
        { "cube_crlf",            { NULL, NULL, NULL, "\r\n", NULL } },
        { "cube_old_mac_cr",      { NULL, NULL, NULL, "\r",   NULL } },
        { "cube_tabs",            { NULL, NULL, NULL, NULL, "%s\t%s\t%s" } },
        { "cube_spaces_around",   { NULL, NULL, NULL, NULL, "   %s   %s  %s   " } },
        { "cube_trailing_comments", { NULL, NULL, NULL, NULL, "%s %s %s # a comment" } },
        { "cube_blank_and_comment_lines",
          { NULL, NULL, "\n# comment\n\n   # indented comment\n\n", NULL,
            "%s %s %s\n\n# between\n" } },
        { "cube_other_keywords",
          { NULL, NULL, "LUT_IN_VIDEO_RANGE_IGNORED\nKEYWORD_OF_ANOTHER_PROGRAM 1 2\n",
            NULL, NULL } },
      };
    gsize i;

    for (i = 0; i < G_N_ELEMENTS (styles); i++)
      {
        gchar  *file = g_strdup_printf ("%s.cube", styles[i].name);
        Result  r;
        gdouble e;

        path = write_cube (file, 5, tf_swap, NULL, &styles[i].style);
        r = run (px, count, path, NULL);
        e = max_error_tf (px, r.px, count, tf_swap, NULL);
        report (styles[i].name, e <= 1e-6 && ! r.error[0], "max error %.2g%s%s",
                e, r.error[0] ? ", " : "", r.error);
        result_free (&r);
        g_free (path);
        g_free (file);
      }
  }

  check_cube_text ("cube_lowercase_keywords_no_final_newline",
                   "title \"x\"\nlut_3d_size 2\n"
                   "0 0 0\n0 0 1\n1 0 0\n1 0 1\n0 1 0\n0 1 1\n1 1 0\n1 1 1",
                   tf_swap);
  check_cube_text ("cube_number_forms",
                   "LUT_3D_SIZE 2.0\nDOMAIN_MAX 1e0 +1 1.000\n"
                   "0e0 .0 -0\n0 0 1E+0\n1 0 0\n1 0 1\n0 1 0\n0 1 1\n"
                   "1 1 0\n1.0000000000001 1 1\n", tf_swap);
  check_cube_text ("cube_title_with_hash_and_keyword_order",
                   "DOMAIN_MIN 0 0 0\nTITLE \"a # b\"\n# c\nDOMAIN_MAX 1 1 1\n"
                   "LUT_3D_SIZE 2\n"
                   "0 0 0\n0 0 1\n1 0 0\n1 0 1\n0 1 0\n0 1 1\n1 1 0\n1 1 1\n",
                   tf_swap);

  /* the decimal comma of the locale does not matter */
  {
    static const gchar *locales[] = { "sv_SE.UTF-8", "sv_SE.utf8", "de_DE.UTF-8",
                                      "de_DE.utf8", "en_DK.UTF-8", "en_DK.utf8" };
    const gchar *found = NULL;
    gsize        i;

    for (i = 0; i < G_N_ELEMENTS (locales) && ! found; i++)
      if (setlocale (LC_NUMERIC, locales[i]))
        {
          gchar half[8];

          /* a locale that writes one half as 0,5 */
          g_snprintf (half, sizeof (half), "%.1f", 0.5);
          if (strcmp (half, "0,5") == 0)
            found = locales[i];
        }
    if (found)
      {
        Result  r;
        gdouble e;

        path = write_cube ("locale.cube", 9, tf_grade, NULL, &st);
        r = run (px, count, path, NULL);
        {
          Ref ref = ref_new (9, tf_grade, NULL, NULL, NULL);
          e = max_error (px, r.px, count, &ref, TRUE, NULL);
          g_free (ref.v);
        }
        setlocale (LC_NUMERIC, "C");
        report ("cube_with_decimal_comma_locale", e <= 2e-6, "%s, max error %.2g",
                found, e);
        result_free (&r);
        g_free (path);
      }
    else
      printf ("SKIP  cube_with_decimal_comma_locale: no such locale here\n");
  }
  g_free (px);
}

/* Hald CLUTs ---------------------------------------------------------------- */

static void
test_hald (void)
{
  gint    count;
  gfloat *px = test_pixels (0, 4000, 110, &count);
  gchar  *hald, *cube;
  Result  a, b;
  gdouble e;
  Ref     ref;

  /* a Hald CLUT and a .cube of the same transform */
  hald = write_hald ("grade-hald-4.png", 4, tf_grade01, NULL, "png16");
  cube = write_cube ("grade-hald-4.cube", 16, tf_grade01, NULL, NULL);
  a = run (px, count, hald, NULL);
  b = run (px, count, cube, NULL);
  e = max_error (a.px, b.px, count, NULL, FALSE, NULL);
  {
    gdouble d = 0;
    gint    p;

    for (p = 0; p < count * 4; p++)
      if (p % 4 != 3)
        d = MAX (d, fabs (a.px[p] - b.px[p]));
    e = d;
  }
  report ("hald_png16_same_as_cube", e <= 0.5 / 65535 + 1e-6,
          "max difference %.2g", e);
  result_free (&a);
  result_free (&b);
  g_free (hald);
  g_free (cube);

  /* 8 bits, level 8 (512 x 512, 64 points): within half a step */
  hald = write_hald ("grade-hald-8.png", 8, tf_grade01, NULL, "png8");
  ref  = ref_new (64, tf_grade01, NULL, NULL, NULL);
  a = run (px, count, hald, NULL);
  e = max_error (px, a.px, count, &ref, TRUE, NULL);
  report ("hald_png8_level_8", e <= 0.5 / 255 + 1e-6, "max error %.2g", e);
  result_free (&a);
  g_free (ref.v);
  g_free (hald);
  g_free (px);
}

/* bad files ------------------------------------------------------------------ */

static void
check_bad (const gchar *name, const gchar *path)
{
  gint     count, before, after, again;
  gfloat  *px = test_pixels (3, 200, 120, &count);
  Result   r, r2;
  gboolean same;
  gint64   t0 = g_get_monotonic_time ();

  px[4] = NAN;
  quiet_messages = TRUE;
  before = n_messages;
  r      = run (px, count, path, "interpolation", TRILINEAR, NULL);
  after  = n_messages;
  r2     = run (px, count, path, NULL);
  again  = n_messages;
  quiet_messages = FALSE;

  same = same_bits (px, r.px, count) && same_bits (px, r2.px, count);
  report (name, same && after > before && again == after &&
          r.error[0] && ! strcmp (r.error, r2.error) &&
          g_get_monotonic_time () - t0 < 5 * G_USEC_PER_SEC,
          "%s%s%s%s", r.error[0] ? r.error : "no error given",
          same ? "" : "; the image changed",
          after > before ? "" : "; no warning",
          again == after ? "" : "; warned again for the same file");
  result_free (&r);
  result_free (&r2);
  g_free (px);
}

static void
check_bad_text (const gchar *name, const gchar *file, const gchar *text,
                gssize length)
{
  gchar *path = write_data (file, text, length);

  check_bad (name, path);
  g_free (path);
}

static const gchar cube8[] =
  "0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";

static void
test_bad_files (void)
{
  gchar *path, *text;

  path = tmp_path ("missing.cube");
  check_bad ("bad_missing_file", path);
  g_free (path);
  check_bad ("bad_folder", tmp_dir);
  check_bad_text ("bad_empty_file", "empty.cube", "", 0);
  check_bad_text ("bad_only_comments", "comments.cube", "# nothing\n\n# here\n", -1);
  check_bad_text ("bad_size_without_table", "notable.cube", "LUT_3D_SIZE 2\n", -1);
  text = g_strdup_printf ("LUT_3D_SIZE 2\n%.*s", (gint) strlen (cube8) - 6, cube8);
  check_bad_text ("bad_too_few_lines", "few.cube", text, -1);
  g_free (text);
  text = g_strdup_printf ("LUT_3D_SIZE 2\n%s1 1 1\n", cube8);
  check_bad_text ("bad_too_many_lines", "many.cube", text, -1);
  g_free (text);
  check_bad_text ("bad_garbage_number", "garbage.cube",
                  "LUT_3D_SIZE 2\n0 0 0\n1 abc 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n", -1);
  check_bad_text ("bad_number_glued_to_text", "glued.cube",
                  "LUT_3D_SIZE 2\n0 0 0\n1 0x 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n", -1);
  check_bad_text ("bad_infinite_number", "inf.cube",
                  "LUT_3D_SIZE 2\n0 0 0\n1e999 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n", -1);
  check_bad_text ("bad_nan_number", "nan.cube",
                  "LUT_3D_SIZE 2\nnan 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n", -1);
  check_bad_text ("bad_value_too_large", "large.cube",
                  "LUT_3D_SIZE 2\n0 0 0\n1e7 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n", -1);
  check_bad_text ("bad_two_numbers_on_a_line", "two.cube",
                  "LUT_3D_SIZE 2\n0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n", -1);
  check_bad_text ("bad_four_numbers_on_a_line", "four.cube",
                  "LUT_3D_SIZE 2\n0 0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n", -1);
  check_bad_text ("bad_huge_size", "huge.cube", "LUT_3D_SIZE 100000\n0 0 0\n", -1);
  check_bad_text ("bad_size_257", "257.cube", "LUT_3D_SIZE 257\n0 0 0\n", -1);
  /* the table is only allocated as it is read: this fails at once */
  check_bad_text ("bad_size_256_with_3_lines", "256.cube",
                  "LUT_3D_SIZE 256\n0 0 0\n1 1 1\n0.5 0.5 0.5\n", -1);
  check_bad_text ("bad_1d_size_too_large", "1dhuge.cube", "LUT_1D_SIZE 70000\n0 0 0\n", -1);
  check_bad_text ("bad_size_1", "size1.cube", "LUT_3D_SIZE 1\n0 0 0\n", -1);
  check_bad_text ("bad_size_negative", "sizeneg.cube", "LUT_3D_SIZE -5\n0 0 0\n", -1);
  check_bad_text ("bad_size_fraction", "sizefrac.cube", "LUT_3D_SIZE 2.5\n0 0 0\n", -1);
  check_bad_text ("bad_size_missing", "sizenone.cube", "LUT_3D_SIZE\n0 0 0\n", -1);
  text = g_strdup_printf ("LUT_3D_SIZE 2\nLUT_3D_SIZE 2\n%s", cube8);
  check_bad_text ("bad_size_twice", "twice.cube", text, -1);
  g_free (text);
  text = g_strdup_printf ("LUT_3D_SIZE 2\nDOMAIN_MIN 0 0.5 0\nDOMAIN_MAX 1 0.5 1\n%s", cube8);
  check_bad_text ("bad_empty_domain", "domain.cube", text, -1);
  g_free (text);
  text = g_strdup_printf ("LUT_3D_SIZE 2\nDOMAIN_MIN 0 0\n%s", cube8);
  check_bad_text ("bad_domain_two_numbers", "domain2.cube", text, -1);
  g_free (text);
  text = g_strdup_printf ("LUT_3D_SIZE 2\n%.*sDOMAIN_MAX 1 1 1\n%s",
                          6, cube8, cube8 + 6);
  check_bad_text ("bad_keyword_inside_table", "kwdata.cube", text, -1);
  g_free (text);
  check_bad_text ("bad_numbers_before_size", "early.cube", cube8, -1);
  check_bad_text ("bad_text_file", "readme.cube", "This is not a LUT.\nReally.\n", -1);
  {
    guchar  junk[3000];
    GRand  *rand = g_rand_new_with_seed (7);
    gsize   i;

    for (i = 0; i < sizeof (junk); i++)
      junk[i] = g_rand_int_range (rand, 0, 256);
    g_rand_free (rand);
    check_bad_text ("bad_binary_junk", "junk.cube", (const gchar *) junk,
                    sizeof (junk));
  }

  /* .3dl */
  check_bad_text ("bad_3dl_not_a_cube_count", "count.3dl",
                  "0 1023\n0 0 0\n0 0 1023\n0 1023 0\n", -1);
  check_bad_text ("bad_3dl_grid_of_wrong_length", "meshlen.3dl",
                  "0 512 1023\n0 0 0\n0 0 4095\n0 4095 0\n0 4095 4095\n"
                  "4095 0 0\n4095 0 4095\n4095 4095 0\n4095 4095 4095\n", -1);
  check_bad_text ("bad_3dl_decreasing_grid", "meshdec.3dl",
                  "600 500\n"
                  "0 0 0\n0 0 4095\n0 4095 0\n0 4095 4095\n"
                  "4095 0 0\n4095 0 4095\n4095 4095 0\n4095 4095 4095\n", -1);
  check_bad_text ("bad_3dl_floats", "floats.3dl",
                  "0 1023\n0 0 0\n0 0 0.5\n0 1 0\n0 1 1\n1 0 0\n1 0 1\n1 1 0\n1 1 1\n", -1);
  check_bad_text ("bad_3dl_negative", "neg.3dl",
                  "0 1023\n0 0 -1\n0 0 4095\n0 4095 0\n0 4095 4095\n"
                  "4095 0 0\n4095 0 4095\n4095 4095 0\n4095 4095 4095\n", -1);
  check_bad_text ("bad_3dl_xml", "xml.3dl", "<?xml version=\"1.0\"?>\n<LUT/>\n", -1);
  check_bad_text ("bad_3dl_values_too_small", "small.3dl",
                  "0 0 0\n0 0 1\n0 1 0\n0 1 1\n1 0 0\n1 0 1\n1 1 0\n1 1 1\n", -1);
  check_bad_text ("bad_3dl_huge_integer", "hugeint.3dl",
                  "0 0 0\n0 0 99999999999999\n0 1 0\n0 1 1\n1 0 0\n1 0 1\n1 1 0\n1 1 1\n", -1);
  check_bad_text ("bad_3dl_empty", "empty.3dl", "# only a comment\n", -1);

  /* images */
  {
    gchar *png = write_hald ("good.png", 2, tf_identity, NULL, "png16");
    gchar *data;
    gsize  length;

    g_file_get_contents (png, &data, &length, NULL);
    check_bad_text ("bad_truncated_png", "truncated.png", data, length / 2);
    check_bad_text ("bad_png_header_only", "header.png", data, 8);
    data[length - 20] ^= 0x55;
    check_bad_text ("bad_png_damaged_chunk", "damaged.png", data, length);
    g_free (data);
    g_free (png);
    png = write_hald ("good.tif", 2, tf_identity, NULL, "tiff");
    g_file_get_contents (png, &data, &length, NULL);
    check_bad_text ("bad_truncated_tiff", "truncated.tif", data, length / 2);
    g_free (data);
    g_free (png);
  }
  {
    /* an image that is not a Hald CLUT: 10 x 10, and 8 x 4 */
    GeglBuffer *b = gegl_buffer_new (GEGL_RECTANGLE (0, 0, 10, 10),
                                     babl_format ("R'G'B' u8"));
    GeglNode   *g = gegl_node_new ();
    GeglNode   *s = gegl_node_new_child (g, "operation", "gegl:buffer-source",
                                         "buffer", b, NULL);
    gchar      *p = tmp_path ("ten.png");
    GeglNode   *w = gegl_node_new_child (g, "operation", "gegl:png-save",
                                         "path", p, NULL);

    gegl_node_link (s, w);
    gegl_node_process (w);
    check_bad ("bad_hald_wrong_size", p);
    g_free (p);
    g_object_unref (b);
    b = gegl_buffer_new (GEGL_RECTANGLE (0, 0, 8, 4), babl_format ("R'G'B' u8"));
    p = tmp_path ("8x4.png");
    gegl_node_set (s, "buffer", b, NULL);
    gegl_node_set (w, "path", p, NULL);
    gegl_node_process (w);
    check_bad ("bad_hald_not_square", p);
    g_free (p);
    g_object_unref (b);
    g_object_unref (g);
  }
}

/* the error property: set from the main loop, back if someone clears it,
 * empty again once the file is good */
static void
test_error_property (void)
{
  gchar      *path = tmp_path ("error-prop.cube");
  GeglBuffer *in   = gegl_buffer_new (GEGL_RECTANGLE (0, 0, 4, 1), babl_format (WORK));
  GeglNode   *g    = gegl_node_new ();
  GeglNode   *src  = gegl_node_new_child (g, "operation", "gegl:buffer-source",
                                          "buffer", in, NULL);
  GeglNode   *op   = gegl_node_new_child (g, "operation", OP, "path", path, NULL);
  gfloat      out[16];
  gchar      *e1, *e2, *e3, *e4;

  gegl_node_link (src, op);
#define RENDER()                                                          \
  gegl_node_blit (op, 1.0, GEGL_RECTANGLE (0, 0, 4, 1), babl_format (WORK), \
                  out, GEGL_AUTO_ROWSTRIDE, GEGL_BLIT_DEFAULT)
  quiet_messages = TRUE;
  RENDER ();
  quiet_messages = FALSE;
  gegl_node_get (op, "error", &e1, NULL);      /* before the main loop */
  while (g_main_context_iteration (NULL, FALSE));
  gegl_node_get (op, "error", &e2, NULL);
  gegl_node_set (op, "error", "", NULL);
  RENDER ();
  while (g_main_context_iteration (NULL, FALSE));
  gegl_node_get (op, "error", &e3, NULL);
  g_free (write_cube ("error-prop.cube", 2, tf_swap, NULL, NULL));
  gegl_node_set (op, "path", path, NULL);
  RENDER ();
  while (g_main_context_iteration (NULL, FALSE));
  gegl_node_get (op, "error", &e4, NULL);
#undef RENDER
  report ("error_property_from_the_main_loop", ! e1[0] && e2[0], "%s", e2);
  report ("error_property_comes_back_when_cleared", ! strcmp (e2, e3), "%s", e3);
  report ("error_property_empty_when_the_file_is_good", ! e4[0], "%s", e4);
  g_free (e1);
  g_free (e2);
  g_free (e3);
  g_free (e4);
  g_object_unref (g);
  g_object_unref (in);
  g_free (path);
}

/* the cache ---------------------------------------------------------------- */

static void
set_mtime (const gchar *path, time_t t)
{
  struct timespec ts[2] = { { t, 0 }, { t, 0 } };

  utimensat (AT_FDCWD, path, ts, 0);
}

static void
test_cache (void)
{
  CubeStyle fixed = { NULL, NULL, NULL, NULL, NULL };
  gchar    *a, *b, *path, *da, *db;
  gsize     la, lb;
  time_t    t0 = time (NULL) - 1000;
  gint      count;
  gfloat   *px = test_pixels (2, 300, 130, &count);
  Result    r;
  gdouble   e;

  /* two LUTs of the same length: invert and swap */
  a = write_cube ("cache-a.cube", 2, tf_invert, NULL, &fixed);
  b = write_cube ("cache-b.cube", 2, tf_swap, NULL, &fixed);
  g_file_get_contents (a, &da, &la, NULL);
  g_file_get_contents (b, &db, &lb, NULL);
  path = tmp_path ("cache.cube");

  g_file_set_contents (path, da, la, NULL);
  set_mtime (path, t0);
  r = run (px, count, path, NULL);
  e = max_error_tf (px, r.px, count, tf_invert, NULL);
  report ("cache_first_load", la == lb && e <= 1e-6, "max error %.2g", e);
  result_free (&r);

  /* the same size and modification time: the parsed LUT is used again,
   * the file is not read */
  g_file_set_contents (path, db, lb, NULL);
  set_mtime (path, t0);
  r = run (px, count, path, NULL);
  e = max_error_tf (px, r.px, count, tf_invert, NULL);
  report ("cache_used_while_the_file_is_unchanged", e <= 1e-6,
          "max error %.2g", e);
  result_free (&r);

  /* a new modification time: read again */
  set_mtime (path, t0 + 5);
  r = run (px, count, path, NULL);
  e = max_error_tf (px, r.px, count, tf_swap, NULL);
  report ("cache_reads_a_changed_file", e <= 1e-6, "max error %.2g", e);
  result_free (&r);

  /* the same node, prepared again, sees the change */
  {
    GeglBuffer *in  = gegl_buffer_new (GEGL_RECTANGLE (0, 0, count, 1),
                                       babl_format (WORK));
    GeglNode   *g   = gegl_node_new ();
    GeglNode   *src = gegl_node_new_child (g, "operation", "gegl:buffer-source",
                                           "buffer", in, NULL);
    GeglNode   *op  = gegl_node_new_child (g, "operation", OP, "path", path, NULL);
    gfloat     *out = g_new (gfloat, count * 4);
    gdouble     e1, e2;

    gegl_buffer_set (in, NULL, 0, babl_format (WORK), px, GEGL_AUTO_ROWSTRIDE);
    gegl_node_link (src, op);
    gegl_node_blit (op, 1.0, GEGL_RECTANGLE (0, 0, count, 1),
                    babl_format (WORK), out, GEGL_AUTO_ROWSTRIDE,
                    GEGL_BLIT_DEFAULT);
    e1 = max_error_tf (px, out, count, tf_swap, NULL);
    g_file_set_contents (path, da, la, NULL);
    set_mtime (path, t0 + 10);
    /* what GIMP does when the filter is shown again or a setting changes */
    gegl_node_set (op, "path", path, NULL);
    gegl_node_blit (op, 1.0, GEGL_RECTANGLE (0, 0, count, 1),
                    babl_format (WORK), out, GEGL_AUTO_ROWSTRIDE,
                    GEGL_BLIT_DEFAULT);
    e2 = max_error_tf (px, out, count, tf_invert, NULL);
    report ("cache_same_node_sees_the_change", e1 <= 1e-6 && e2 <= 1e-6,
            "max errors %.2g, %.2g", e1, e2);
    g_free (out);
    g_object_unref (g);
    g_object_unref (in);
  }

  /* a file that is not there yet, then is */
  {
    gchar *later = tmp_path ("later.cube");
    gint   before;

    quiet_messages = TRUE;
    before = n_messages;
    r = run (px, count, later, NULL);
    quiet_messages = FALSE;
    report ("cache_missing_file_passes_through",
            same_bits (px, r.px, count) && n_messages == before + 1 && r.error[0],
            "%s", r.error);
    result_free (&r);
    g_file_set_contents (later, db, lb, NULL);
    r = run (px, count, later, NULL);
    e = max_error_tf (px, r.px, count, tf_swap, NULL);
    report ("cache_file_that_appears_is_used", e <= 1e-6 && ! r.error[0],
            "max error %.2g", e);
    result_free (&r);
    g_free (later);
  }

  g_free (da);
  g_free (db);
  g_free (a);
  g_free (b);
  g_free (path);
  g_free (px);
}

/* pieces, formats, no input ------------------------------------------------ */

static void
test_pieces (void)
{
  const gint  w = 70, h = 45;
  gint        count, x, y;
  gfloat     *px   = test_pixels (0, w * h, 140, &count);
  gchar      *path = write_cube ("pieces.cube", 17, tf_grade, NULL, NULL);
  GeglBuffer *in   = gegl_buffer_new (GEGL_RECTANGLE (0, 0, w, h), babl_format (WORK));
  gfloat     *whole = g_new (gfloat, w * h * 4);
  gfloat     *parts = g_new (gfloat, w * h * 4);
  GeglNode   *g, *src, *op;

  gegl_buffer_set (in, NULL, 0, babl_format (WORK), px, GEGL_AUTO_ROWSTRIDE);
  g   = gegl_node_new ();
  src = gegl_node_new_child (g, "operation", "gegl:buffer-source", "buffer", in, NULL);
  op  = gegl_node_new_child (g, "operation", OP, "path", path, NULL);
  gegl_node_link (src, op);
  gegl_node_blit (op, 1.0, GEGL_RECTANGLE (0, 0, w, h), babl_format (WORK),
                  whole, GEGL_AUTO_ROWSTRIDE, GEGL_BLIT_DEFAULT);
  g_object_unref (g);

  for (y = 0; y < h; y += 16)
    for (x = 0; x < w; x += 16)
      {
        GeglRectangle r = { x, y, MIN (16, w - x), MIN (16, h - y) };

        g   = gegl_node_new ();
        src = gegl_node_new_child (g, "operation", "gegl:buffer-source",
                                   "buffer", in, NULL);
        op  = gegl_node_new_child (g, "operation", OP, "path", path, NULL);
        gegl_node_link (src, op);
        gegl_node_blit (op, 1.0, &r, babl_format (WORK),
                        parts + (y * w + x) * 4, w * 4 * sizeof (gfloat),
                        GEGL_BLIT_DEFAULT);
        g_object_unref (g);
      }
  report ("pieces_same_as_whole", same_bits (whole, parts, w * h), NULL);

  g_object_unref (in);
  g_free (whole);
  g_free (parts);
  g_free (px);
  g_free (path);
}

static void
test_formats (void)
{
  gint    count, p, c;
  gfloat *px   = test_pixels (0, 2000, 150, &count);
  gchar  *path = write_cube ("formats.cube", 17, tf_grade, NULL, NULL);
  gchar  *id   = write_cube ("formats-id.cube", 5, tf_identity, NULL, NULL);
  guint8 *u8   = g_new (guint8, count * 4);
  gfloat *f    = g_new (gfloat, count * 4);

  /* an 8 bit image, as in GIMP: the same as the float result, rounded */
  {
    GeglBuffer *in  = gegl_buffer_new (GEGL_RECTANGLE (0, 0, count, 1),
                                       babl_format ("R'G'B'A u8"));
    GeglNode   *g   = gegl_node_new ();
    GeglNode   *src = gegl_node_new_child (g, "operation", "gegl:buffer-source",
                                           "buffer", in, NULL);
    GeglNode   *op  = gegl_node_new_child (g, "operation", OP, "path", path, NULL);
    guint8     *out = g_new (guint8, count * 4);
    Result      r;
    gint        worst = 0;

    for (p = 0; p < count * 4; p++)
      u8[p] = (guint8) floor (px[p] * 255 + 0.5);
    for (p = 0; p < count * 4; p++)
      f[p] = u8[p] / 255.0f;
    gegl_buffer_set (in, NULL, 0, babl_format ("R'G'B'A u8"), u8, GEGL_AUTO_ROWSTRIDE);
    gegl_node_link (src, op);
    gegl_node_blit (op, 1.0, GEGL_RECTANGLE (0, 0, count, 1),
                    babl_format ("R'G'B'A u8"), out, GEGL_AUTO_ROWSTRIDE,
                    GEGL_BLIT_DEFAULT);
    r = run (f, count, path, NULL);
    for (p = 0; p < count; p++)
      {
        for (c = 0; c < 3; c++)
          worst = MAX (worst, abs (out[p * 4 + c] -
                                   (gint) floor (CLAMP (r.px[p * 4 + c], 0, 1) * 255 + 0.5)));
        worst = MAX (worst, abs (out[p * 4 + 3] - u8[p * 4 + 3]) * 10);
      }
    report ("format_8_bit", worst <= 1, "largest difference %d of 255", worst);
    result_free (&r);
    g_free (out);
    g_object_unref (g);
    g_object_unref (in);
  }

  /* gray, and RGB without alpha, through an identity LUT */
  {
    const Babl *fmts[2] = { babl_format ("Y'A float"), babl_format ("R'G'B' float") };
    const gchar *names[2] = { "format_gray_with_alpha", "format_rgb_without_alpha" };
    gint i;

    for (i = 0; i < 2; i++)
      {
        Result  r = run_fmt (px, count / 2, fmts[i], id, NULL);
        gint    nc = babl_format_get_n_components (fmts[i]);
        gdouble e = 0;

        for (p = 0; p < count / 2 * nc; p++)
          e = MAX (e, fabs (r.px[p] - px[p]));
        report (names[i], e <= 1e-6, "max error %.2g", e);
        result_free (&r);
      }
  }

  g_free (u8);
  g_free (f);
  g_free (px);
  g_free (path);
  g_free (id);
}

static void
test_no_input (void)
{
  gchar        *path = write_cube ("noinput.cube", 2, tf_invert, NULL, NULL);
  GeglNode     *g    = gegl_node_new ();
  GeglNode     *op   = gegl_node_new_child (g, "operation", OP, "path", path, NULL);
  GeglRectangle bb   = gegl_node_get_bounding_box (op);
  gfloat        out[16 * 4] = { 1.0f, };

  gegl_node_blit (op, 1.0, GEGL_RECTANGLE (0, 0, 4, 4), babl_format (WORK), out,
                  GEGL_AUTO_ROWSTRIDE, GEGL_BLIT_DEFAULT);
  report ("no_input", bb.width == 0 && bb.height == 0 && out[0] == 0.0f, NULL);
  g_object_unref (g);
  g_free (path);
}

/* the committed fixtures --------------------------------------------------- */

static void
check_fixture (const gchar *file, Transform tf, gpointer data, gint n, gdouble tol)
{
  gchar  *path = g_build_filename (fixtures, file, NULL);
  gchar  *name = g_strdup_printf ("fixture_%s", file);
  gint    count, t;
  gfloat *px   = test_pixels (n, 2000, 160, &count);
  gdouble worst = 0;
  gchar  *error = NULL;

  for (t = 0; t < 2; t++)
    {
      Result r = run (px, count, path, "interpolation", interp (t), NULL);

      worst = MAX (worst, max_error_tf (px, r.px, count, tf, data));
      if (r.error[0] && ! error)
        error = g_strdup (r.error);
      result_free (&r);
    }
  report (name, worst <= tol && ! error, "max error %.2g (tolerance %.2g)%s%s",
          worst, tol, error ? ", " : "", error ? error : "");
  g_free (error);
  g_free (px);
  g_free (name);
  g_free (path);
}

static void
tf_square (const gdouble *in, gdouble *out, gpointer data)
{
  gint c;

  (void) data;
  /* x^2 interpolated linearly between 0, 1/4, 1/2, 3/4 and 1 */
  for (c = 0; c < 3; c++)
    {
      gdouble x = CLAMP (in[c], 0, 1);
      gint    i = MIN ((gint) floor (x * 4), 3);
      gdouble f = x * 4 - i;

      out[c] = (i / 4.0) * (i / 4.0) * (1 - f) + ((i + 1) / 4.0) * ((i + 1) / 4.0) * f;
    }
}

static void
test_fixtures (void)
{
  check_fixture ("resolve-shaper.cube", tf_identity, NULL, 3, 1e-6);
  check_fixture ("swap-crlf.cube", tf_swap, NULL, 2, 1e-6);
  check_fixture ("square-1d.cube", tf_square, NULL, 5, 1e-6);
  check_fixture ("lustre-identity-5.3dl", tf_identity, NULL, 5, 0.5 / 4095 + 1e-6);
  check_fixture ("flame-swap-3.3dl", tf_swap, NULL, 3, 0.5 / 1023 + 1e-6);
  check_fixture ("hald-2-invert.png", tf_invert, NULL, 4, 1e-6);
}

/* ------------------------------------------------------------------------ */

static void
remove_tree (const gchar *dir)
{
  GDir        *d = g_dir_open (dir, 0, NULL);
  const gchar *name;

  if (! d)
    return;
  while ((name = g_dir_read_name (d)))
    {
      gchar *p = g_build_filename (dir, name, NULL);

      if (g_file_test (p, G_FILE_TEST_IS_DIR) && ! g_file_test (p, G_FILE_TEST_IS_SYMLINK))
        remove_tree (p);
      else
        g_unlink (p);
      g_free (p);
    }
  g_dir_close (d);
  g_rmdir (dir);
}

int
main (int    argc,
      char **argv)
{
  gchar *mod_dir, *link, *target;
  gint   expected;

  if (argc != 3)
    {
      fprintf (stderr, "usage: %s <color-lookup.so> <tests/fixtures>\n", argv[0]);
      return 2;
    }
  fixtures = g_canonicalize_filename (argv[2], NULL);

  /* GEGL loads every file in a module folder, and a build folder holds
   * files that it must not load: load the module from a folder of its own.
   * GEGL_PATH replaces GEGL's own list of folders, so that an installed
   * copy of the operation (in the user's folder) is not loaded instead. */
  mod_dir = g_dir_make_tmp ("lut-check-module-XXXXXX", NULL);
  tmp_dir = g_dir_make_tmp ("lut-check-XXXXXX", NULL);
  link    = g_build_filename (mod_dir ? mod_dir : ".", "color-lookup.so", NULL);
  target  = g_canonicalize_filename (argv[1], NULL);
  if (! mod_dir || ! tmp_dir || symlink (target, link) != 0)
    {
      fprintf (stderr, "cannot link %s into a temporary folder\n", argv[1]);
      return 2;
    }
  {
    gchar *path = g_strconcat (mod_dir, G_SEARCHPATH_SEPARATOR_S,
                               GEGL_PLUGINSDIR, NULL);

    g_setenv ("GEGL_PATH", path, TRUE);
    g_free (path);
  }

  gegl_init (&argc, &argv);
  g_log_set_default_handler (log_handler, NULL);

  if (! gegl_has_operation (OP))
    {
      fprintf (stderr, OP " did not load from %s\n", argv[1]);
      return 2;
    }

  test_identity ();
  test_affine ();
  test_gamma ();
  test_product ();
  test_reference ();
  test_1d ();
  test_domain ();
  test_out_of_range ();
  test_alpha ();
  test_strength ();
  test_encoding ();
  test_space ();
  test_3dl ();
  test_cube_variants ();
  test_hald ();
  expected = n_messages;
  test_bad_files ();
  test_error_property ();
  test_cache ();
  expected = n_messages - expected;
  test_pieces ();
  test_formats ();
  test_no_input ();
  test_fixtures ();

  report ("no_other_warnings_or_criticals", n_messages == expected,
          "%d, of which %d expected from the bad files", n_messages, expected);

  gegl_exit ();

  remove_tree (tmp_dir);
  /* GEGL loads the module again when it is used after a pause: remove it
   * only at the end, or not at all for LeakSanitizer, which needs it to
   * name the functions when it reports after main () */
  if (! g_getenv ("LUT_CHECK_KEEP_MODULE"))
    {
      g_unlink (link);
      g_rmdir (mod_dir);
    }
  g_free (target);
  g_free (link);
  g_free (mod_dir);
  g_free (tmp_dir);
  g_free (fixtures);

  printf ("\n%d passed, %d failed\n", n_passed, n_failed);

  return n_failed ? 1 : 0;
}

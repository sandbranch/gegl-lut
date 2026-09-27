/*
 * How long lut:color-lookup takes on a 24 megapixel image
 *
 * bench.c
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
 *   bench <color-lookup.so> [width height]
 *
 * Writes .cube LUTs of 33 and 65 points into a temporary folder, then
 * times reading them (the first time, and from the cache) and applying
 * them to a 6000 x 4000 image, in float and in 8 bits (as GIMP's 8 bit
 * images), with GEGL's threads and with one thread; the best of three
 * runs. Two point filters of GEGL, gegl:invert-gamma and gegl:levels,
 * are timed the same way, for scale.
 */

#include <gegl.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static gint W = 6000, H = 4000;

static gchar *
write_cube (const gchar *dir, gint n)
{
  gchar   *path = g_strdup_printf ("%s/bench-%d.cube", dir, n);
  GString *s    = g_string_new (NULL);
  gint     i, j, k, c;

  g_string_append_printf (s, "LUT_3D_SIZE %d\n", n);
  for (k = 0; k < n; k++)
    for (j = 0; j < n; j++)
      for (i = 0; i < n; i++)
        {
          gdouble x[3] = { (gdouble) i / (n - 1), (gdouble) j / (n - 1),
                           (gdouble) k / (n - 1) };
          gdouble y = 0.3 * x[0] + 0.55 * x[1] + 0.15 * x[2];

          for (c = 0; c < 3; c++)
            {
              gdouble v = y + 1.3 * (x[c] - y) + 0.08 * sin (3.0 * x[(c + 1) % 3]);
              gchar   buf[G_ASCII_DTOSTR_BUF_SIZE];

              v = 0.5 + 0.5 * tanh (2.2 * (v - 0.5)) / tanh (1.1);
              g_ascii_formatd (buf, sizeof (buf), "%.6f", v);
              g_string_append (s, buf);
              g_string_append_c (s, c < 2 ? ' ' : '\n');
            }
        }
  g_file_set_contents (path, s->str, s->len, NULL);
  g_string_free (s, TRUE);
  return path;
}

/* a photo-like image: smooth ramps with fine detail */
static GeglBuffer *
make_image (const gchar *format)
{
  GeglBuffer *b   = gegl_buffer_new (GEGL_RECTANGLE (0, 0, W, H),
                                     babl_format (format));
  gfloat     *row = g_new (gfloat, W * 4);
  gint        x, y;

  for (y = 0; y < H; y++)
    {
      for (x = 0; x < W; x++)
        {
          row[x * 4 + 0] = (gfloat) x / W;
          row[x * 4 + 1] = (gfloat) y / H;
          row[x * 4 + 2] = 0.5f + 0.5f * sinf (x * 0.013f + y * 0.007f);
          row[x * 4 + 3] = 1.0f;
        }
      gegl_buffer_set (b, GEGL_RECTANGLE (0, y, W, 1), 0,
                       babl_format ("R'G'B'A float"), row, GEGL_AUTO_ROWSTRIDE);
    }
  g_free (row);
  return b;
}

/* seconds for the operation over the whole image, the best of three */
static gdouble
time_op (GeglBuffer  *in,
         const gchar *op,
         const gchar *first_property,
         ...)
{
  GeglBuffer *out  = gegl_buffer_new (GEGL_RECTANGLE (0, 0, W, H),
                                      gegl_buffer_get_format (in));
  gdouble     best = G_MAXDOUBLE;
  gint        run;

  for (run = 0; run < 4; run++)
    {
      GeglNode *g    = gegl_node_new ();
      GeglNode *src  = gegl_node_new_child (g, "operation", "gegl:buffer-source",
                                            "buffer", in, NULL);
      GeglNode *node = gegl_node_new_child (g, "operation", op, NULL);
      GeglNode *sink = gegl_node_new_child (g, "operation", "gegl:write-buffer",
                                            "buffer", out, NULL);
      gint64    t0;
      va_list   args;

      if (first_property)
        {
          va_start (args, first_property);
          gegl_node_set_valist (node, first_property, args);
          va_end (args);
        }
      gegl_node_link_many (src, node, sink, NULL);
      t0 = g_get_monotonic_time ();
      gegl_node_process (sink);
      /* the first run allocates the output's tiles */
      if (run > 0)
        best = MIN (best, (g_get_monotonic_time () - t0) / 1e6);
      g_object_unref (g);
    }
  g_object_unref (out);
  return best;
}

/* seconds to prepare the operation on a LUT file */
static gdouble
time_prepare (GeglBuffer *in, const gchar *path)
{
  GeglNode *g    = gegl_node_new ();
  GeglNode *src  = gegl_node_new_child (g, "operation", "gegl:buffer-source",
                                        "buffer", in, NULL);
  GeglNode *node = gegl_node_new_child (g, "operation", "lut:color-lookup",
                                        "path", path, NULL);
  gint64    t0;

  gegl_node_link (src, node);
  t0 = g_get_monotonic_time ();
  gegl_node_get_bounding_box (node);   /* prepares it */
  t0 = g_get_monotonic_time () - t0;
  g_object_unref (g);
  return t0 / 1e6;
}

int
main (int    argc,
      char **argv)
{
  gchar      *dir, *link, *target, *path, *cube2, *cube33, *cube65, *cube1d;
  GeglBuffer *img_f, *img_8;
  gint        threads, t;

  if (argc != 2 && argc != 4)
    {
      fprintf (stderr, "usage: %s <color-lookup.so> [width height]\n", argv[0]);
      return 2;
    }
  if (argc == 4)
    {
      W = atoi (argv[2]);
      H = atoi (argv[3]);
    }

  dir    = g_dir_make_tmp ("lut-bench-XXXXXX", NULL);
  link   = g_build_filename (dir, "color-lookup.so", NULL);
  target = g_canonicalize_filename (argv[1], NULL);
  if (symlink (target, link) != 0)
    return 2;
  path = g_strconcat (dir, G_SEARCHPATH_SEPARATOR_S, GEGL_PLUGINSDIR, NULL);
  g_setenv ("GEGL_PATH", path, TRUE);
  g_free (path);
  gegl_init (NULL, NULL);
  /* room for the images in memory, as GIMP gives GEGL; the default is
   * smaller, and swapping to disk would be what is timed */
  g_object_set (gegl_config (), "tile-cache-size", (guint64) 8 << 30, NULL);
  g_object_get (gegl_config (), "threads", &threads, NULL);

  cube2  = write_cube (dir, 2);
  cube33 = write_cube (dir, 33);
  cube65 = write_cube (dir, 65);
  cube1d = g_strdup_printf ("%s/bench-1d.cube", dir);
  {
    GString *s1 = g_string_new ("LUT_1D_SIZE 1024\n");
    gint     i;

    for (i = 0; i < 1024; i++)
      {
        gchar b[G_ASCII_DTOSTR_BUF_SIZE];

        g_ascii_formatd (b, sizeof (b), "%.6f", pow (i / 1023.0, 1.2));
        g_string_append_printf (s1, "%s %s %s\n", b, b, b);
      }
    g_file_set_contents (cube1d, s1->str, s1->len, NULL);
    g_string_free (s1, TRUE);
  }
  img_f  = make_image ("R'G'B'A float");
  img_8  = make_image ("R'G'B'A u8");

  printf ("%d x %d pixels (%.1f megapixels), GEGL threads: %d\n\n",
          W, H, W * H / 1e6, threads);
  {
    /* one after the other: the order of a call's arguments is not fixed */
    gdouble p33 = time_prepare (img_f, cube33);
    gdouble p65 = time_prepare (img_f, cube65);
    gdouble pc  = time_prepare (img_f, cube65);

    printf ("reading the LUT: 33 points %.3f s, 65 points %.3f s; "
            "again from the cache %.6f s\n\n", p33, p65, pc);
  }

  for (t = 0; t < 2; t++)
    {
      gint n = t == 0 ? threads : 1;

      g_object_set (gegl_config (), "threads", n, NULL);
      printf ("%d thread%s                       float     8 bit\n", n, n > 1 ? "s" : "");
      /* two point filters of GEGL, for scale */
      printf ("  gegl:invert-gamma            %6.3f s  %6.3f s\n",
              time_op (img_f, "gegl:invert-gamma", NULL),
              time_op (img_8, "gegl:invert-gamma", NULL));
      printf ("  gegl:levels (linear light)   %6.3f s  %6.3f s\n",
              time_op (img_f, "gegl:levels", "out-high", 0.9, NULL),
              time_op (img_8, "gegl:levels", "out-high", 0.9, NULL));
#define ROW(label, file, interp)                                          \
      printf ("  %-28s %6.3f s  %6.3f s\n", label,                       \
              time_op (img_f, "lut:color-lookup", "path", file,          \
                       "interpolation", interp, NULL),                   \
              time_op (img_8, "lut:color-lookup", "path", file,          \
                       "interpolation", interp, NULL))
      ROW ("33 points, tetrahedral", cube33, 0);
      ROW ("33 points, trilinear", cube33, 1);
      ROW ("65 points, tetrahedral", cube65, 0);
      ROW ("65 points, trilinear", cube65, 1);
      ROW ("2 points, tetrahedral", cube2, 0);
      ROW ("1D, 1024 points", cube1d, 0);
#undef ROW
      printf ("  %-28s %6.3f s  %6.3f s\n", "strength 0 (passes through)",
              time_op (img_f, "lut:color-lookup", "path", cube65,
                       "strength", 0.0, NULL),
              time_op (img_8, "lut:color-lookup", "path", cube65,
                       "strength", 0.0, NULL));
      printf ("\n");
    }

  g_object_unref (img_f);
  g_object_unref (img_8);
  gegl_exit ();
  g_unlink (cube2);
  g_unlink (cube33);
  g_unlink (cube65);
  g_unlink (cube1d);
  g_free (cube2);
  g_free (cube1d);
  g_unlink (link);
  g_rmdir (dir);
  g_free (cube33);
  g_free (cube65);
  g_free (link);
  g_free (dir);
  g_free (target);
  return 0;
}

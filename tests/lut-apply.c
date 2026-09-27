/*
 * Applies lut:color-lookup to raw float pixels, for tests/crosscheck.sh
 *
 * lut-apply.c
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
 *   lut-apply <color-lookup.so> <in.raw> <out.raw> <width> <height>
 *             [property=value ...]
 *
 * The raw files hold R'G'B' float pixels (sRGB, 12 bytes each, row by
 * row, in the machine's byte order). Properties are given as on the gegl
 * command line, e.g. path=look.cube interpolation=trilinear.
 */

#include <gegl.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FMT "R'G'B' float"

static gboolean
set_property (GeglNode    *node,
              const gchar *assignment)
{
  const gchar *eq    = strchr (assignment, '=');
  gchar       *name;
  GParamSpec  *pspec;
  GValue       value = G_VALUE_INIT;

  if (! eq)
    return FALSE;
  name  = g_strndup (assignment, eq - assignment);
  pspec = gegl_node_find_property (node, name);
  if (! pspec)
    {
      g_free (name);
      return FALSE;
    }
  g_value_init (&value, pspec->value_type);
  if (G_IS_PARAM_SPEC_ENUM (pspec))
    {
      GEnumClass *klass = g_type_class_ref (pspec->value_type);
      GEnumValue *v     = g_enum_get_value_by_nick (klass, eq + 1);

      if (! v)
        {
          g_type_class_unref (klass);
          g_free (name);
          return FALSE;
        }
      g_value_set_enum (&value, v->value);
      g_type_class_unref (klass);
    }
  else if (G_IS_PARAM_SPEC_DOUBLE (pspec))
    g_value_set_double (&value, g_ascii_strtod (eq + 1, NULL));
  else if (G_IS_PARAM_SPEC_STRING (pspec))
    g_value_set_string (&value, eq + 1);
  else
    {
      g_free (name);
      return FALSE;
    }
  gegl_node_set_property (node, name, &value);
  g_value_unset (&value);
  g_free (name);
  return TRUE;
}

int
main (int    argc,
      char **argv)
{
  gchar      *dir, *link, *target, *pixels = NULL, *path;
  gsize       length;
  gint        w, h, i;
  GeglBuffer *in;
  GeglNode   *graph, *src, *op;
  gfloat     *out;

  if (argc < 6)
    {
      fprintf (stderr, "usage: %s <color-lookup.so> <in.raw> <out.raw> "
                       "<width> <height> [property=value ...]\n", argv[0]);
      return 2;
    }
  w = atoi (argv[4]);
  h = atoi (argv[5]);
  if (w <= 0 || h <= 0 ||
      ! g_file_get_contents (argv[2], &pixels, &length, NULL) ||
      length != (gsize) w * h * 3 * sizeof (gfloat))
    {
      fprintf (stderr, "%s: cannot read %d x %d pixels\n", argv[2], w, h);
      return 2;
    }

  /* the module from a folder of its own, as tests/check.c does */
  dir    = g_dir_make_tmp ("lut-apply-XXXXXX", NULL);
  link   = g_build_filename (dir, "color-lookup.so", NULL);
  target = g_canonicalize_filename (argv[1], NULL);
  if (symlink (target, link) != 0)
    return 2;
  path = g_strconcat (dir, G_SEARCHPATH_SEPARATOR_S, GEGL_PLUGINSDIR, NULL);
  g_setenv ("GEGL_PATH", path, TRUE);
  g_free (path);
  gegl_init (NULL, NULL);

  in = gegl_buffer_new (GEGL_RECTANGLE (0, 0, w, h), babl_format (FMT));
  gegl_buffer_set (in, NULL, 0, babl_format (FMT), pixels, GEGL_AUTO_ROWSTRIDE);
  graph = gegl_node_new ();
  src   = gegl_node_new_child (graph, "operation", "gegl:buffer-source",
                               "buffer", in, NULL);
  op    = gegl_node_new_child (graph, "operation", "lut:color-lookup", NULL);
  for (i = 6; i < argc; i++)
    if (! set_property (op, argv[i]))
      {
        fprintf (stderr, "cannot set %s\n", argv[i]);
        return 2;
      }
  gegl_node_link (src, op);
  out = g_new (gfloat, (gsize) w * h * 3);
  gegl_node_blit (op, 1.0, GEGL_RECTANGLE (0, 0, w, h), babl_format (FMT), out,
                  GEGL_AUTO_ROWSTRIDE, GEGL_BLIT_DEFAULT);
  g_file_set_contents (argv[3], (const gchar *) out,
                       (gssize) w * h * 3 * sizeof (gfloat), NULL);

  g_object_unref (graph);
  g_object_unref (in);
  g_free (out);
  g_free (pixels);
  gegl_exit ();
  g_unlink (link);
  g_rmdir (dir);
  g_free (link);
  g_free (dir);
  g_free (target);

  return 0;
}

#include <config.h>

#include <string.h>
#include <unistd.h>
#include <errno.h>
#ifdef HAVE_SELINUX
#include <selinux/selinux.h>
#endif

#include <sys/types.h>
#include <attr/xattr.h>

#include <glib/gi18n-lib.h>

#include "gfileinfosimple.h"
#include "gvfserror.h"

static gchar *
read_link (const gchar *full_name)
{
	gchar *buffer;
	guint size;

	size = 256;
	buffer = g_malloc (size);
          
	while (1) {
		int read_size;

                read_size = readlink (full_name, buffer, size);
		if (read_size < 0) {
			g_free (buffer);
			return NULL;
		}
                if (read_size < size) {
			buffer[read_size] = 0;
			return buffer;
		}
                size *= 2;
		buffer = g_realloc (buffer, size);
	}
}


/* Get the SELinux security context */
static void
get_selinux_context (const char *path,
		     GFileInfo *info,
		     GFileAttributeMatcher *attribute_matcher,
		     gboolean follow_symlinks)
{
#ifdef HAVE_SELINUX
  char *context;

  if (!g_file_attribute_matcher_matches (attribute_matcher,
					 "selinux", "selinux:context"))
    return;
  
  if (is_selinux_enabled ()) {
    if (follow_symlinks) {
      if (lgetfilecon_raw (path, &context) < 0)
	return;
    } else {
      if (getfilecon_raw (path, &context) < 0)
	return;
    }

    if (context)
      {
	g_file_info_set_attribute (info, "selinux:context", context);
	freecon(context);
      }
  }
#endif
}

static gboolean
valid_char (char c)
{
  return c >= 32 && c <= 126 && c != '\\';
}

static void
escape_xattr (GFileInfo *info,
	      const char *attr,
	      const char *value, /* Is zero terminated */
	      size_t len /* not including zero termination */)
{
  char *full_attr;
  int num_invalid, i;
  char *escaped_val, *p;
  unsigned char c;
  static char *hex_digits = "0123456789abcdef";
  
  full_attr = g_strconcat ("xattr:", attr, NULL);

  num_invalid = 0;
  for (i = 0; i < len; i++)
    {
      if (!valid_char (value[i]))
	num_invalid++;
    }
	
  if (num_invalid == 0)
    g_file_info_set_attribute (info, full_attr, value);
  else
    {
      escaped_val = g_malloc (len + num_invalid*3 + 1);

      p = escaped_val;
      for (i = 0; i < len; i++)
	{
	  if (valid_char (value[i]))
	    *p++ = value[i];
	  else
	    {
	      c = value[i];
	      *p++ = '\\';
	      *p++ = 'x';
	      *p++ = hex_digits[(c >> 8) & 0xf];
	      *p++ = hex_digits[c & 0xf];
	    }
	}
      *p++ = 0;
      g_file_info_set_attribute (info, full_attr, escaped_val);
      g_free (escaped_val);
    }
  
  g_free (full_attr);
}

static void
get_one_xattr (const char *path,
	       GFileInfo *info,
	       const char *attr,
	       gboolean follow_symlinks)
{
  char value[64];
  char *value_p;
  ssize_t len;

  if (follow_symlinks)  
    len = getxattr (path, attr, value, sizeof (value)-1);
  else
    len = lgetxattr (path, attr,value, sizeof (value)-1);

  value_p = NULL;
  if (len >= 0)
    value_p = value;
  else if (len == -1 && errno == ERANGE)
    {
      if (follow_symlinks)  
	len = getxattr (path, attr, NULL, 0);
      else
	len = lgetxattr (path, attr, NULL, 0);

      if (len < 0)
	return;

      value_p = g_malloc (len+1);

      if (follow_symlinks)  
	len = getxattr (path, attr, value_p, len);
      else
	len = lgetxattr (path, attr, value_p, len);

      if (len < 0)
	{
	  g_free (value_p);
	  return;
	}
    }
  else
    return;
  
  /* Null terminate */
  value_p[len] = 0;

  escape_xattr (info, attr, value_p, len);
  
  if (value_p != value)
    g_free (value_p);
}

static void
get_xattrs (const char *path,
	    GFileInfo *info,
	    GFileAttributeMatcher *matcher,
	    gboolean follow_symlinks)
{
  gboolean all;
  gsize list_size;
  ssize_t list_res_size;
  size_t len;
  char *list;
  const char *attr;

  all = g_file_attribute_matcher_enumerate (matcher, "xattr");

  if (all)
    {
      if (follow_symlinks)
	list_res_size = listxattr (path, NULL, 0);
      else
	list_res_size = llistxattr (path, NULL, 0);

      if (list_res_size == -1 ||
	  list_res_size == 0)
	return;

      list_size = list_res_size;
      list = g_malloc (list_size);

    retry:
      
      if (follow_symlinks)
	list_res_size = listxattr (path, list, list_size);
      else
	list_res_size = llistxattr (path, list, list_size);
      
      if (list_res_size == -1 && errno == ERANGE)
	{
	  list_size = list_size * 2;
	  list = g_realloc (list, list_size);
	  goto retry;
	}

      if (list_res_size == -1)
	return;

      attr = list;
      while (list_res_size > 0)
	{
	  get_one_xattr (path, info, attr, follow_symlinks);
	  len = strlen (attr) + 1;
	  attr += len;
	  list_res_size -= len;
	}

      g_free (list);
    }
  else
    {
      while ((attr = g_file_attribute_matcher_enumerate_next (matcher)) != NULL)
	get_one_xattr (path, info, attr, follow_symlinks);
    }
}

GFileInfo *
g_file_info_simple_get (const char *basename,
			const char *path,
			GFileInfoRequestFlags requested,
			GFileAttributeMatcher *attribute_matcher,
			gboolean follow_symlinks,
			GError **error)
{
  GFileInfo *info;
  struct stat statbuf;
  int res;

  info = g_file_info_new ();
  
  if (requested & G_FILE_INFO_NAME)
    g_file_info_set_name (info, basename);

  if (requested & G_FILE_INFO_IS_HIDDEN)
    g_file_info_set_is_hidden (info,
			  basename != NULL &&
			  basename[0] == '.');


  /* Avoid stat in trivial case */
  if ((requested & ~(G_FILE_INFO_NAME|G_FILE_INFO_IS_HIDDEN)) == 0 &&
      attribute_matcher == NULL)
    return info;
  
  if (follow_symlinks)
    res = stat (path, &statbuf);
  else
    res = lstat (path, &statbuf);
  
  if (res == -1)
    {
      g_object_unref (info);
      g_set_error (error,
		   G_FILE_ERROR,
		   g_file_error_from_errno (errno),
		   _("Error stating file '%s': %s"),
		   path, g_strerror (errno));
      return NULL;
    }
  
  g_file_info_set_from_stat (info, requested, &statbuf);
  
  if (requested & G_FILE_INFO_SYMLINK_TARGET)
    {
      char *link = read_link (path);
      g_file_info_set_symlink_target (info, link);
      g_free (link);
    }

  if (requested & G_FILE_INFO_ACCESS_RIGHTS)
    {
      /* TODO */
    }
  
  if (requested & G_FILE_INFO_DISPLAY_NAME)
    {
      /* TODO */
    }
  
  if (requested & G_FILE_INFO_EDIT_NAME)
    {
      /* TODO */
    }

  if (requested & G_FILE_INFO_MIME_TYPE)
    {
      /* TODO */
    }
  
  if (requested & G_FILE_INFO_ICON)
    {
      /* TODO */
    }

  get_selinux_context (path, info, attribute_matcher, follow_symlinks);
  get_xattrs (path, info, attribute_matcher, follow_symlinks);
  
  return info;
}


GFileInfo *
g_file_info_simple_get_from_fd (int fd,
				GFileInfoRequestFlags requested,
				char *attributes,
				GError **error)
{
  struct stat stat_buf;
  GFileAttributeMatcher *matcher;
  GFileInfo *info;
  
  if (fstat (fd, &stat_buf) == -1)
    {
      g_vfs_error_from_errno (error, errno);
      return NULL;
    }

  info = g_file_info_new ();
  
  g_file_info_set_from_stat (info, requested, &stat_buf);

  matcher = g_file_attribute_matcher_new (attributes);
  
#ifdef HAVE_SELINUX
  if (g_file_attribute_matcher_matches (matcher, "selinux", "selinux:context") &&
      is_selinux_enabled ()) {
    char *context;
    if (fgetfilecon_raw (fd, &context) >= 0)
      {
	g_file_info_set_attribute (info, "selinux:context", context);
	freecon(context);
      }
  }
#endif
  
  g_file_attribute_matcher_free (matcher);
  
  return info;
}

/* MJ 2001 */

#include "sysdeps.h"
#include "tools.h"
#include "parameters.h"

#ifdef HAVE_NEW_HEADERS
# include <cstdlib>
#else
# include <stdlib.h>
#endif

int get_geometry(char *dev_path, geo_type geo) {
  return -1;
}

/*
 * Get the path to folder with user-specific files (configuration, NVRAM)
 */
char *getConfFolder(char *buffer, unsigned int bufsize)
{
	// local cache
	static char path[512] = "";

	if (strlen(path) == 0) {
		// Unix-like systems define HOME variable as the user home folder
		char *home = getenv("HOME");
		if (home == NULL)
			home = "";	// alternatively use current directory

		int homelen = strlen(home);
		if (homelen > 0) {
			unsigned int len = strlen(ARANYMHOME);
			if ((homelen + 1 + len + 1) < bufsize) {
				strcpy(path, home);
				strcat(path, DIRSEPARATOR);
				strcat(path, ARANYMHOME);
			}
		}
	}

	return safe_strncpy(buffer, path, bufsize);
}

char *getDataFolder(char *buffer, unsigned int bufsize)
{
	// data folder is defined at configure time in DATADIR (using --datadir)
	return safe_strncpy(buffer, DATADIR, bufsize);
}


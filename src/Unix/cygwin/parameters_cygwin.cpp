/* MJ 2001 */

#include "sysdeps.h"
#include "tools.h"
#include "parameters.h"

#ifdef HAVE_NEW_HEADERS
# include <cstdlib>
#else
# include <stdlib.h>
#endif

#define ARADATA		"aranym"

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
		// Cygwin defines either a sensible value or "/" (root) which means
		// that Unix-like filesystem hierarchy is not in place.
		// In such case let's fall back to Windows environment variables..
		char *home = getenv("HOME");
		if (home == NULL || strcmp(home, "/") == 0)
			home = getenv("WINDIR");		// Win9x use WINDIR
		if (home == NULL)
			home = getenv("USERPROFILE");	// WinNT/2K/XP use USERPROFILE
		if (home == NULL)
			home = "";	// last resort - current folder

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
	char program_home[512];	// set by main()

	// remember path to program
	strcpy(program_home, program_name);
	char *ptr = strrchr(program_home, '/');
	if (ptr != NULL)
		ptr[0] = '\0';	// strip out filename and separator from the path
	else if ((ptr = strrchr(program_home, DIRSEPARATOR[0])) != NULL)
		ptr[0] = '\0';	// strip out filename and separator from the path
	else
		program_home[0] = '\0';

	// test if Unix-like filesystem is in place
	char *home = getenv("HOME");
	if (home == NULL || strcmp(home, "/") == 0) {
		// if not, data folder path is extracted from argv[0]
		// (path to aranym executable) + ARADATA subfolder
		safe_strncpy(buffer, program_home, bufsize);
		if ((strlen(buffer) + 1 + strlen(ARADATA) + 1) < bufsize) {
			// appending ARADATA subfolder to the path to aranym executable
			strcat(buffer, DIRSEPARATOR);
			strcat(buffer, ARADATA);
		}
		else {
			panicbug("getDataFolder(): buffer too small!");
		}
		return buffer;
	}
	else
		return safe_strncpy(buffer, DATADIR, bufsize);
}

/* 
 * misc/bximage.c
 * $Id$
 *
 * Create empty hard disk or floppy disk images for ARAnyM.
 *
 */

#include "config.h"
#include "sysdeps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#ifdef WIN32
#  include <conio.h>
#endif

typedef unsigned long long Bit64u;
typedef long long Bit64s;

char *EOF_ERR = "ERROR: End of input";
char *rcsid = "$Id$";
char *divider =
	"========================================================================";

/* menu data for choosing floppy/hard disk */
char *fdhd_menu =
	"\nDo you want to create a floppy disk image or a hard disk image?\nPlease type hd or fd. [hd] ";
char *fdhd_choices[] = { "fd", "hd" };

/* menu data for choosing floppy size */
char *fdsize_menu =
	"\nChoose the size of floppy disk image to create.\nPlease type dd or hd. [hd] ";
char *fdsize_choices[] = { "dd", "hd" };

void myexit(int code)
{
#ifdef WIN32
	printf("\nPress any key to continue\n");
	getch();
#endif
	exit(code);
}

/* stolen from main.cc */
void bx_center_print(FILE * file, char *line, int maxwidth)
{
	int imax;
	int i;
	imax = (maxwidth - strlen(line)) >> 1;
	for (i = 0; i < imax; i++)
		fputc(' ', file);
	fputs(line, file);
}

void print_banner()
{
	printf("%s\n", divider);
	bx_center_print(stdout, "diskimage 1.00\n", 72);
	bx_center_print(stdout, "Disk Image Creation Tool for ARAnyM\n", 72);
	bx_center_print(stdout, "Shamelessly copied from bximage/bochs, see below\n", 72);
	bx_center_print(stdout, rcsid, 72);
	printf("\n%s\n", divider);
}

/* this is how we crash */
void fatal(char *c)
{
	printf("%s\n", c);
	myexit(1);
}

/* remove leading spaces, newline junk at end.  returns pointer to 
 cleaned string, which is between s0 and the null */
char *clean_string(char *s0)
{
	char *s = s0;
	char *ptr;
	/* find first nonblank */
	while (isspace(*s))
		s++;
	/* truncate string at first non-alphanumeric */
	ptr = s;
	while (isprint(*ptr))
		ptr++;
	*ptr = 0;
	return s;
}

/* returns 0 on success, -1 on failure.  The value goes into out. */
int ask_int(char *prompt, int min, int max, int the_default, int *out)
{
	int n = max + 1;
	char buffer[1024];
	char *clean;
	int illegal;
	while (1) {
		printf("%s", prompt);
		if (!fgets(buffer, sizeof(buffer), stdin))
			return -1;
		clean = clean_string(buffer);
		if (strlen(clean) < 1) {
			// empty line, use the default
			*out = the_default;
			return 0;
		}
		illegal = (1 != sscanf(buffer, "%d", &n));
		if (illegal || n < min || n > max) {
			printf
				("Your choice (%s) was not an integer between %d and %d.\n\n",
				 clean, min, max);
		}
		else {
			// choice is okay
			*out = n;
			return 0;
		}
	}
}

int
ask_menu(char *prompt, int n_choices, char *choice[], int the_default,
		 int *out)
{
	char buffer[1024];
	char *clean;
	int i;
	*out = -1;
	while (1) {
		printf("%s", prompt);
		if (!fgets(buffer, sizeof(buffer), stdin))
			return -1;
		clean = clean_string(buffer);
		if (strlen(clean) < 1) {
			// empty line, use the default
			*out = the_default;
			return 0;
		}
		for (i = 0; i < n_choices; i++) {
			if (!strcmp(choice[i], clean)) {
				// matched, return the choice number
				*out = i;
				return 0;
			}
		}
		printf("Your choice (%s) did not match any of the choices:\n",
			   clean);
		for (i = 0; i < n_choices; i++) {
			if (i > 0)
				printf(", ");
			printf("%s", choice[i]);
		}
		printf("\n");
	}
}

int ask_yn(char *prompt, int the_default, int *out)
{
	char buffer[16];
	char *clean;
	*out = -1;
	while (1) {
		printf("%s", prompt);
		if (!fgets(buffer, sizeof(buffer), stdin))
			return -1;
		clean = clean_string(buffer);
		if (strlen(clean) < 1) {
			// empty line, use the default
			*out = the_default;
			return 0;
		}
		switch (tolower(clean[0])) {
		case 'y':
			*out = 1;
			return 0;
		case 'n':
			*out = 0;
			return 0;
		}
		printf("Please type either yes or no.\n");
	}
}

int ask_string(char *prompt, char *the_default, char *out)
{
	char buffer[1024];
	char *clean;
	out[0] = 0;
	printf("%s", prompt);
	if (!fgets(buffer, sizeof(buffer), stdin))
		return -1;
	clean = clean_string(buffer);
	if (strlen(clean) < 1) {
		// empty line, use the default
		strcpy(out, the_default);
		return 0;
	}
	strcpy(out, clean);
	return 0;
}

/* produce the image file */
int make_image(Bit64u sec, char *filename)
{
	FILE *fp;
	char buffer[1024];

	// check if it exists before trashing someone's disk image
	fp = fopen(filename, "r");
	if (fp) {
		int confirm;
		sprintf(buffer,
				"\nThe disk image '%s' already exists.  Are you sure you want to replace it?\nPlease type yes or no. [no] ",
				filename);
		if (ask_yn(buffer, 0, &confirm) < 0)
			fatal(EOF_ERR);
		if (!confirm)
			fatal("ERROR: Aborted");
		fclose(fp);
	}

	// okay, now open it for writing
	fp = fopen(filename, "w");
	if (fp == NULL) {
		// attempt to print an error
#ifdef HAVE_PERROR
		sprintf(buffer, "while opening '%s' for writing", filename);
		perror(buffer);
#endif
		fatal("ERROR: Could not write disk image");
	}

	printf("\nWriting: [");

	/*
	 * seek to sec*512-1 and write a single character.
	 * can't just do: fseek(fp, 512*sec-1, SEEK_SET)
	 * because 512*sec may be too large for signed int.
	 */
	while (sec > 0) {
		/* temp <-- min(sec, 4194303)
		 * 4194303 is (int)(0x7FFFFFFF/512)
		 */
		long temp = ((sec < 4194303) ? sec : 4194303);
		fseek(fp, 512 * temp, SEEK_CUR);
		sec -= temp;
	}

	fseek(fp, -1, SEEK_CUR);
	if (fputc('\0', fp) == EOF) {
		fclose(fp);
		fatal("ERROR: The disk image is not complete!");
	}

	printf("] Done.\n");
	fclose(fp);
	return 0;
}

int main()
{
	int hd;
	Bit64s sectors = 0;
	char filename[256];
	char aranymcfg_line[256];
	print_banner();
	filename[0] = 0;
	if (ask_menu(fdhd_menu, sizeof(fdhd_choices) / sizeof(fdhd_choices[0]), fdhd_choices, 1, &hd) < 0)
		fatal(EOF_ERR);
	if (hd) {
		int hdsize, cyl, heads = 16, spt = 63;
		if (ask_int
			("\nEnter the hard disk size in megabytes, between 1 and 32255\n[10] ",
			 1, 32255, 10, &hdsize) < 0)
			fatal(EOF_ERR);
		cyl = (int) (hdsize * 1024.0 * 1024.0 / 16.0 / 63.0 / 512.0);
		assert(cyl < 65536);
		sectors = cyl * heads * spt;
		printf("\nI will create a hard disk image with\n");
		printf("  cyl=%d\n", cyl);
		printf("  heads=%d\n", heads);
		printf("  sectors per track=%d\n", spt);
		printf("  total sectors=%lld\n", sectors);
		printf("  total size=%.2f megabytes\n",
			   (float) sectors * 512.0 / 1024.0 / 1024.0);
		if (ask_string
			("\nWhat should I name the image?\n[c.img] ", "c.img",
			 filename) < 0)
			fatal(EOF_ERR);
		sprintf(aranymcfg_line, "[IDE0]\n\
 Present = Yes\n\
 IsCDROM = No\n\
 Path = %s\n\
 Cylinders = %d\n\
 Heads = %d\n\
 SectorsPerTrack = %d\n\
 ByteSwap = No",
				filename, cyl, heads, spt);
	}
	else {
		int fdsize, cyl = 0, heads = 0, spt = 0;
		char *name = NULL;
		if (ask_menu
			(fdsize_menu, sizeof(fdsize_choices) / sizeof(fdsize_choices[0]),
			 fdsize_choices, 1, &fdsize) < 0)
			fatal(EOF_ERR);
		switch (fdsize) {
		case 0:
			name = "720k";
			cyl = 80;
			heads = 2;
			spt = 9;
			break;				/* 0.72 meg */
		case 1:
			name = "1_44";
			cyl = 80;
			heads = 2;
			spt = 18;
			break;				/* 1.44 meg */
		default:
			fatal("ERROR: fdsize out of range");
		}
		sectors = cyl * heads * spt;
		printf("I will create a floppy image with\n");
		printf("  cyl=%d\n", cyl);
		printf("  heads=%d\n", heads);
		printf("  sectors per track=%d\n", spt);
		printf("  total sectors=%lld\n", sectors);
		printf("  total bytes=%lld\n", sectors * 512);
		if (ask_string
			("\nWhat should I name the image?\n[a.img] ", "a.img",
			 filename) < 0)
			fatal(EOF_ERR);
		sprintf(aranymcfg_line, "Floppy = %s", filename);
	}
	if (sectors < 1)
		fatal("ERROR: Illegal disk size!");
	if (strlen(filename) < 1)
		fatal("ERROR: Illegal filename");
	make_image(sectors, filename);
	printf("\nI wrote %lld bytes to %s.\n", sectors * 512, filename);
	printf("\nYou should add the following lines to your ARAnyM config:\n");
	printf("%s\n", aranymcfg_line);
	myexit(0);

	// make picky compilers (c++, gcc) happy,
	// even though we leave via 'myexit' just above 
	return 0;
}

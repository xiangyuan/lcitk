#define _FILE_OFFSET_BITS 64

#include "objdump.h"
#include "util.h"
#include <stdio.h>
#include <string.h>

/**
 *  For the specified process pid and object image name, return the address within the process for the start of the image.
 *
 *  @param[in] process
 *  	The process's PID.
 *
 *  @param[in] image_name
 *  	The name of the image to look for. Any image with image_name as a substring is matched.
 *
 *  @param[out] image_path
 *  	The full filesystem path of the image.
 *
 *  @param[out] image_start
 *  	The starting address in virtual memory for the specified image.
 *
 *  @return
 *  	A true value if the image was found, false for failure.
 *
 */
int find_image_address(int process, const char* image_name, char image_path[512], intptr_t* image_start)
{
	char buf[512];

	*image_start = 0;

	// The first step is to find where the image is mapped.

	snprintf(buf, sizeof(buf), "/proc/%d/maps", process);

	FILE* maps = fopen(buf, "r");
	if(!maps)
		return 0;

	while(fgets(buf, sizeof(buf), maps) != NULL)
	{
		unsigned long long start;
		char permissions[512];

		if(sscanf(buf, "%llx-%*llx %s %*llx %*s %*d %s", &start, permissions, image_path) != 3)
			continue;

		// we're looking for an entry that is both readable and executable
		if(permissions[0] != 'r' || permissions[2] != 'x')
			continue;

		// the filename should also start with the image_name
		if(strstr(image_path, image_name) == NULL)
			continue;

		// this is probably it. let's use it.
		*image_start = start;
		break;
	}

	fclose(maps);

	if(*image_start == 0)
		return 0;

	// second step is to discover the offset from the start of the image the first loaded section actually is.
	char* symbolTable = get_command_output("/usr/bin/objdump", "/usr/bin/objdump", "-p", image_path, NULL);
	char* symbolTableLine = strtok(symbolTable, "\n");	// TODO: Not thread-safe
	do
	{
		unsigned long long offset;
		unsigned long long vaddr;

		if(sscanf(symbolTableLine, " LOAD off 0x%llx vaddr 0x%llx paddr 0x%*llx align %*s", &offset, &vaddr) != 2)
			continue;

		// die if this was the last line
		if((symbolTableLine = strtok(NULL, "\n")) == NULL)
			break;

		if(sscanf(symbolTableLine, " filesz 0x%*llx memsz 0x%*llx flags %s", buf) != 1)
			continue;

		// we're looking for a LOAD segment that is both readable and executable like the one in the map
		if(buf[0] != 'r' || buf[2] != 'x')
			continue;

		// we must adjust image_start by the amount the first section has shifted up from the actual start
		// of the image.
		*image_start -= vaddr - offset;		
		break;
	}
	while((symbolTableLine = strtok(NULL, "\n")) != NULL);

	free(symbolTable);

	return 1;
}

/**
 *  For the specified process, find the image path an address belongs to.
 *
 *  @param[in] process
 *  	The process's PID.
 *
 *  @param[in] address
 *  	The address to check for belonging to a file.
 *
 *  @param[out] image_path
 *  	The full filesystem path of the image.
 *
 *  @return
 *  	A true value if an image path was found, false for failure.
 *
 */
int find_image_for_address(int process, void* address, char image_path[512])
{
	char buf[512];

	// Iterate through the memory map of the process, looking for an entry that corresponds to the address.
	snprintf(buf, sizeof(buf), "/proc/%d/maps", process);

	FILE* maps = fopen(buf, "r");
	if(!maps)
		return 0;

	while(fgets(buf, sizeof(buf), maps) != NULL)
	{
		unsigned long long start;
		unsigned long long end;

		if(sscanf(buf, "%llx-%llx %*s %*llx %*s %*d %s", &start, &end, image_path) != 3)
			continue;

		if(start <= ((intptr_t) address) && ((intptr_t) address) <= end)
		{
			fclose(maps);
			return 1;
		}
	}

	image_path[0] = '\0';
	fclose(maps);
	return 0;
}

/**
 *  For the specified process pid and object image name, return the address within the process for the relocation of the named function.
 *
 *  @param[in] process
 *  	The process's PID.
 *
 *  @param[in] image_name
 *  	The name of the image to look for. Any image with image_name as a substring is matched.
 *
 *  @param[in] func
 *  	The name of the function to find.
 *
 *  @return
 *  	The address of the relocation within the process.
 *
 */
void* find_relocation(int process, const char* image_name, const char* func)
{
	char buf[512];
	char image[512];
	intptr_t image_start = 0;
	intptr_t func_start = 0;

	if(!find_image_address(process, image_name, image, &image_start))
		return NULL;

	// dump the relocation tables for that binary we fished out of /proc/pid/maps
	char* symbolTable = get_command_output("/usr/bin/objdump", "/usr/bin/objdump", "-rR", image, NULL);
	char* symbolTableLine = strtok(symbolTable, "\n");	// TODO: Not thread-safe
	do
	{
		unsigned long long start;

		if(sscanf(symbolTableLine, "%llx %*s %s", &start, buf) != 2)
			continue;

		// we're only interested if we have an exact match for the symbol name
		if(strcmp(buf, func) != 0)
			continue;

		func_start = start;
	}
	while((symbolTableLine = strtok(NULL, "\n")) != NULL);

	free(symbolTable);

	if(func_start == 0)
		return NULL;

	return (void*)(image_start + func_start);
}

/**
 *  For the specified process pid, and object image name, return the address within the process for the named function.
 *
 *  @param[in] process
 *  	The process's PID.
 *
 *  @param[in] image_name
 *  	The name of the image to look for. Any image with image_name as a substring is matched.
 *
 *  @param[in] func
 *  	The name of the function to find.
 *
 *  @param[out] image_path
 *  	Full path of the image with the function.
 *
 *  @return
 *  	The address of the function within the process. NULL if nothing was found.
 *
 */
void* find_function(int process, const char* image_name, const char* func, char** image_path)
{
	char buf[512];
	char image[512];
	intptr_t image_start = 0;
	intptr_t func_start = 0;

	if(!find_image_address(process, image_name, image, &image_start))
		return NULL;

	// second step is to find out where the function is in the libc symbol table
	// I think on the balance, it's better to use the binutils shell commands than try
	// to do something fancy. This way we get a free disassembler and everything.

	// dump the symbol tables for that binary we fished out of /proc/pid/maps
	char* symbolTable = get_command_output("/usr/bin/objdump", "/usr/bin/objdump", "-tT", image, NULL);
	char* symbolTableLine = strtok(symbolTable, "\n");	// TODO: Not thread-safe
	do
	{
		unsigned long long start;

		if(sscanf(symbolTableLine, "%llx %*s %*s %*s %*llx %*s %s", &start, buf) != 2)
		{
			if(sscanf(symbolTableLine, "%llx %*s %*s %*s %*llx %s", &start, buf) != 2)
				continue;
		}

		// we're only interested if we have an exact match for the symbol name
		if(strcmp(buf, func) != 0)
			continue;

		func_start = start;
	}
	while((symbolTableLine = strtok(NULL, "\n")) != NULL);

	free(symbolTable);

	if(func_start == 0)
		return NULL;

	if(image_path != NULL)
		*image_path = strdup(image);

	return (void*)(image_start + func_start);
}

/**
 *  For the specified process pid, return the address within the process for the named libc function.
 *
 *  @param[in] process
 *  	The process's PID.
 *
 *  @param[in] func
 *  	The name of the function to find.
 *
 *  @return
 *  	The address of the function within the process. NULL if nothing was found.
 *
 */
void* find_libc_function(int process, const char* func)
{
	return find_function(process, "/libc", func, NULL);
}
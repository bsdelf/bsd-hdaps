/*-
 * Copyright (c) 2006 Maik Ehinger <m.ehinger@ltur.de>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
#include <sys/param.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_param.h>

#include <sys/systm.h>

#include <machine/pc/bios.h>
#include "smbios.h"

/*
 * System Management BIOS Reference Specification, v2.4 Final
 * http://www.dmtf.org/standards/published_documents/DSP0134.pdf
 */

#define	SMBIOS_START	0xf0000
#define	SMBIOS_STEP	0x10
#define	SMBIOS_OFF	0
#define	SMBIOS_LEN	4
#define	SMBIOS_SIG	"_SM_"

static uintptr_t smbios_eps = 0; /* SMBIOS Entry Point */

static const char nullstring = '\0';

smbios_values_t smbios_values;

static int	smbios_cksum	(uintptr_t addr);

static int 
smbios_find_eps(void)
{
	uintptr_t addr;
	u_int8_t major, minor;

	if (smbios_eps == 0xffff)
		return 0;

	/* already set */
	if (smbios_eps > 0)
		return 1;

	addr = bios_sigsearch(SMBIOS_START, SMBIOS_SIG, SMBIOS_LEN,
			      SMBIOS_STEP, SMBIOS_OFF);
	if (addr != 0) {

		smbios_eps = BIOS_PADDRTOVADDR(addr);

		if (smbios_cksum(smbios_eps)) {
			printf("SMBIOS checksum failed.\n");
			smbios_eps = 0xffff;
			return 0;
		} else {
			printf("SMBIOS found\n"); 
			major = *((u_int8_t *)(smbios_eps + 0x6));
			minor = *((u_int8_t *)(smbios_eps + 0x7));
			printf("SMBIOS %i.%i found\n", major, minor); 
			return 1;
		}
	}

	printf("SMBIOS Signature not found\n");
	smbios_eps = 0xffff;
	return 0;
}

static int
smbios_cksum(uintptr_t addr)
{
	u_int8_t *ptr;
	u_int8_t cksum;
	int i;

	ptr = (u_int8_t *)addr;
	cksum = 0;

	for (i = 0; i < ptr[5]; i++) {
		cksum += ptr[i];
	}

	return (cksum);
}


/***
 * Finds the first structure of Type
 * Sets addr to start of structure if found or
 * NULL if no structure of Type is found
 * Return 1 if a structure was found
 */
static int smbios_FindStructure(u_int8_t Type, uintptr_t *addr)
{
	int ret = 0;
	u_short i = 0;
	uintptr_t TableAddress = 0;

	/* Structure Table Address */
	TableAddress = BIOS_PADDRTOVADDR(*((uintptr_t *)(smbios_eps + 0x18)));
	
    printf("SMBIOS: finding struct");
	while (i < *((u_int16_t *)(smbios_eps + 0x1c))) /*Number of Structures*/ 
	{
        printf(".");
		if ( *((u_int8_t *)TableAddress) == Type ) {
			ret = 1;
			break;
		}

		/* add Length */
		TableAddress += *((u_int8_t *)TableAddress + 0x1);

		while ( *((u_int16_t *)TableAddress) != 0x00 ) {
				TableAddress++;
		}
		
		TableAddress += 2; /* set to start of next structure */ 
		i++;
	}
    printf(" done!\n");

	if (ret)
		*addr = TableAddress;
	else
		addr = NULL;

	return ret;
	
}

/***
 * Copies the Number String from structure starting at Addr to string
 * or null string 
 */
static const char *smbios_get_string(uintptr_t Addr, u_int8_t Number)
{
	u_int8_t i = 1;

	Addr += *((u_int8_t *)(Addr + 0x1)); /* add Length */
	
	/* no strings */
	while( (*((u_int16_t *)Addr) != 0x00) && ( i < Number) ) {

		Addr++;
		while( *((u_int8_t *)Addr) != 0x0 ) {
			Addr++; 
		}

		i++;

		if (i == Number)
			Addr++;
	}

	if ( i == Number ) {
		return (const char *)Addr;
	} else
		return &nullstring;	
}

/**
 * Set some variables
 */
static int smbios_init(void)
{
	uintptr_t addr;

    printf("SMBIOS: find eps...\n");
	if (!smbios_find_eps())
		return 0;
    printf("SMBIOS: find eps done!\n");

	smbios_values.system_maker = &nullstring;
	smbios_values.system_version = &nullstring;
	smbios_values.oem_string = &nullstring;

    printf("SMBIOS: find struct...\n");
	if ( smbios_FindStructure(0x1, &addr) ) { /* System Information Type 1 */

		 /* System Manufacturer */
		smbios_values.system_maker = smbios_get_string(addr,
						*((u_int8_t *)(addr + 0x4)));

		/* System Version */
		smbios_values.system_version = smbios_get_string(addr,
						*((u_int8_t *)(addr + 0x6)));
	} else
		printf("SMBIOS: No System Information\n");

	if (smbios_FindStructure(0xb, &addr)) { /* OEM Strings Type 11 */
		/* OEM String */
		
		int strno;
	
		strno = smbios_find_oem_substring("IBM ThinkPad Embedded Controller");
		if (strno)
			smbios_values.oem_string = smbios_get_string(addr,
									strno);
	} else
		printf("SMBIOS: No OEM Strings Information\n");

	return 1;
	

}

int smbios_check_system(smbios_system_id *list)
{
    return 1;
	int i;

	if (smbios_eps == 0xffff)
		return 0;

	if (smbios_eps == 0)
		if (!smbios_init())
			return 0;
	
	for (i=0; list[i].maker; i++) {
		if ( !strcmp(list[i].maker, smbios_values.system_maker))
			if ( !strcmp(list[i].version,
					smbios_values.system_version)) {

				printf("SMBIOS: %s %s found\n",
					smbios_values.system_maker,
					smbios_values.system_version);
				return 1;	
			}
	}	
	
	return 0;
}

/****
 * Search substring within OEM Strings
 * Returns the number of the matching string, 0 otherwise
 */
int smbios_find_oem_substring(const char *substr)
{
    return 1;
	int i;
	uintptr_t addr;
	const char *oem_string;

	if (smbios_eps == 0xffff)
		return 0;

	if (smbios_eps == 0) {
        printf("SMBIOS: init...\n");
		if (!smbios_init())
			return 0;
    }

    printf("SMBIOS: find...\n");
	if (smbios_FindStructure(0xb, &addr)) { /* OEM Strings Type 11 */
		/* Search all OEM strings */
		for (i=1; i <= *((u_int8_t *)(addr + 0x4)); i++) {
            printf("SMBIOS: searching...\n");
			oem_string = smbios_get_string(addr, i);
			if ( !strncmp(oem_string, substr, strlen(substr)))
				return i;
		}
	} else
		printf("SMBIOS: No OEM String Information\n");

	return 0;
}

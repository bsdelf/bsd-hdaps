typedef struct {
	char *maker;
	char *version;
} smbios_system_id;

typedef struct {
	const char *system_maker;
	const char *system_version;
	const char *oem_string;
} smbios_values_t;

extern smbios_values_t smbios_values;

int smbios_check_system(smbios_system_id *list);
int smbios_find_oem_substring(const char *substr);

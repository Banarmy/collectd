/**
 * collectd - src/openvpn.c
 * Copyright (C) 2008       Doug MacEachern
 * Copyright (C) 2009,2010  Florian octo Forster
 * Copyright (C) 2009       Marco Chiappero
 * Copyright (C) 2009       Fabian Schuh
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the License is applicable.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Doug MacEachern <dougm at hyperic.com>
 *   Florian octo Forster <octo at collectd.org>
 *   Marco Chiappero <marco at absence.it>
 *   Fabian Schuh <mail at xeroc.org>
 **/

#include "collectd.h"
#include "common.h"
#include "plugin.h"

#define V1STRING "Common Name,Real Address,Bytes Received,Bytes Sent,Connected Since\n"
#define V2STRING "HEADER,CLIENT_LIST,Common Name,Real Address,Virtual Address,Bytes Received,Bytes Sent,Connected Since,Connected Since (time_t)\n"
#define V3STRING "HEADER CLIENT_LIST Common Name Real Address Virtual Address Bytes Received Bytes Sent Connected Since Connected Since (time_t)\n"
#define V4STRING "HEADER,CLIENT_LIST,Common Name,Real Address,Virtual Address,Bytes Received,Bytes Sent,Connected Since,Connected Since (time_t),Username\n"
#define VSSTRING "OpenVPN STATISTICS\n"


struct vpn_status_s
{
	char *file;
	enum
	{
		MULTI1 = 1, /* status-version 1 */
		MULTI2,     /* status-version 2 */
		MULTI3,     /* status-version 3 */
		MULTI4,     /* status-version 4 */
		SINGLE = 10 /* currently no versions for single mode, maybe in the future */
	} version;
	char *name;
};
typedef struct vpn_status_s vpn_status_t;

static vpn_status_t **vpn_list = NULL;
static int vpn_num = 0;

static _Bool new_naming_schema = 0;
static _Bool collect_compression = 1;
static _Bool collect_user_count  = 0;
static _Bool collect_individual_users  = 1;

static const char *config_keys[] =
{
	"StatusFile",
	"Compression", /* old, deprecated name */
	"ImprovedNamingSchema",
	"CollectCompression",
	"CollectUserCount",
	"CollectIndividualUsers"
};
static int config_keys_num = STATIC_ARRAY_SIZE (config_keys);


/* Helper function
 * copy-n-pasted from common.c - changed delim to ","  */
static int openvpn_strsplit (char *string, char **fields, size_t size)
{
	size_t i;
	char *ptr;
	char *saveptr;

	i = 0;
	ptr = string;
	saveptr = NULL;
	while ((fields[i] = strtok_r (ptr, ",", &saveptr)) != NULL)
	{
		ptr = NULL;
		i++;

		if (i >= size)
			break;
	}

	return (i);
} /* int openvpn_strsplit */

/* dispatches number of users */
static void numusers_submit (const char *pinst, const char *tinst,
		gauge_t value)
{
	value_t values[1];
	value_list_t vl = VALUE_LIST_INIT;

	values[0].gauge = value;

	vl.values = values;
	vl.values_len = STATIC_ARRAY_SIZE (values);
	sstrncpy (vl.host, hostname_g, sizeof (vl.host));
	sstrncpy (vl.plugin, "openvpn", sizeof (vl.plugin));
	sstrncpy (vl.type, "users", sizeof (vl.type));
	if (pinst != NULL)
		sstrncpy (vl.plugin_instance, pinst, sizeof (vl.plugin_instance));
	if (tinst != NULL)
		sstrncpy (vl.type_instance, tinst, sizeof (vl.type_instance));

	plugin_dispatch_values (&vl);
} /* void numusers_submit */

/* dispatches stats about traffic (TCP or UDP) generated by the tunnel
 * per single endpoint */
static void iostats_submit (const char *pinst, const char *tinst,
		derive_t rx, derive_t tx)
{
	value_t values[2];
	value_list_t vl = VALUE_LIST_INIT;

	values[0].derive = rx;
	values[1].derive = tx;

	/* NOTE ON THE NEW NAMING SCHEMA:
	 *       using plugin_instance to identify each vpn config (and
	 *       status) file; using type_instance to identify the endpoint
	 *       host when in multimode, traffic or overhead when in single.
	 */

	vl.values = values;
	vl.values_len = STATIC_ARRAY_SIZE (values);
	sstrncpy (vl.host, hostname_g, sizeof (vl.host));
	sstrncpy (vl.plugin, "openvpn", sizeof (vl.plugin));
	if (pinst != NULL)
		sstrncpy (vl.plugin_instance, pinst,
				sizeof (vl.plugin_instance));
	sstrncpy (vl.type, "if_octets", sizeof (vl.type));
	if (tinst != NULL)
		sstrncpy (vl.type_instance, tinst, sizeof (vl.type_instance));

	plugin_dispatch_values (&vl);
} /* void traffic_submit */

/* dispatches stats about data compression shown when in single mode */
static void compression_submit (const char *pinst, const char *tinst,
		derive_t uncompressed, derive_t compressed)
{
	value_t values[2];
	value_list_t vl = VALUE_LIST_INIT;

	values[0].derive = uncompressed;
	values[1].derive = compressed;

	vl.values = values;
	vl.values_len = STATIC_ARRAY_SIZE (values);
	sstrncpy (vl.host, hostname_g, sizeof (vl.host));
	sstrncpy (vl.plugin, "openvpn", sizeof (vl.plugin));
	if (pinst != NULL)
		sstrncpy (vl.plugin_instance, pinst,
				sizeof (vl.plugin_instance));
	sstrncpy (vl.type, "compression", sizeof (vl.type));
	if (tinst != NULL)
		sstrncpy (vl.type_instance, tinst, sizeof (vl.type_instance));

	plugin_dispatch_values (&vl);
} /* void compression_submit */

static int single_read (const char *name, FILE *fh)
{
	char buffer[1024];
	char *fields[4];
	const int max_fields = STATIC_ARRAY_SIZE (fields);
	int  fields_num, read = 0;

	derive_t link_rx, link_tx;
	derive_t tun_rx, tun_tx;
	derive_t pre_compress, post_compress;
	derive_t pre_decompress, post_decompress;
	derive_t overhead_rx, overhead_tx;

	link_rx = 0;
	link_tx = 0;
	tun_rx = 0;
	tun_tx = 0;
	pre_compress = 0;
	post_compress = 0;
	pre_decompress = 0;
	post_decompress = 0;

	while (fgets (buffer, sizeof (buffer), fh) != NULL)
	{
		fields_num = openvpn_strsplit (buffer, fields, max_fields);

		/* status file is generated by openvpn/sig.c:print_status()
		 * http://svn.openvpn.net/projects/openvpn/trunk/openvpn/sig.c
		 *
		 * The line we're expecting has 2 fields. We ignore all lines
		 *  with more or less fields.
		 */
		if (fields_num != 2)
		{
			continue;
		}

		if (strcmp (fields[0], "TUN/TAP read bytes") == 0)
		{
			/* read from the system and sent over the tunnel */
			tun_tx = atoll (fields[1]);
		}
		else if (strcmp (fields[0], "TUN/TAP write bytes") == 0)
		{
			/* read from the tunnel and written in the system */
			tun_rx = atoll (fields[1]);
		}
		else if (strcmp (fields[0], "TCP/UDP read bytes") == 0)
		{
			link_rx = atoll (fields[1]);
		}
		else if (strcmp (fields[0], "TCP/UDP write bytes") == 0)
		{
			link_tx = atoll (fields[1]);
		}
		else if (strcmp (fields[0], "pre-compress bytes") == 0)
		{
			pre_compress = atoll (fields[1]);
		}
		else if (strcmp (fields[0], "post-compress bytes") == 0)
		{
			post_compress = atoll (fields[1]);
		}
		else if (strcmp (fields[0], "pre-decompress bytes") == 0)
		{
			pre_decompress = atoll (fields[1]);
		}
		else if (strcmp (fields[0], "post-decompress bytes") == 0)
		{
			post_decompress = atoll (fields[1]);
		}
	}

	iostats_submit (name, "traffic", link_rx, link_tx);

	/* we need to force this order to avoid negative values with these unsigned */
	overhead_rx = (((link_rx - pre_decompress) + post_decompress) - tun_rx);
	overhead_tx = (((link_tx - post_compress) + pre_compress) - tun_tx);

	iostats_submit (name, "overhead", overhead_rx, overhead_tx);

	if (collect_compression)
	{
		compression_submit (name, "data_in", post_decompress, pre_decompress);
		compression_submit (name, "data_out", pre_compress, post_compress);
	}

	read = 1;

	return (read);
} /* int single_read */

/* for reading status version 1 */
static int multi1_read (const char *name, FILE *fh)
{
	char buffer[1024];
	char *fields[10];
	int  fields_num, found_header = 0;
	long long sum_users = 0;

	/* read the file until the "ROUTING TABLE" line is found (no more info after) */
	while (fgets (buffer, sizeof (buffer), fh) != NULL)
	{
		if (strcmp (buffer, "ROUTING TABLE\n") == 0)
			break;

		if (strcmp (buffer, V1STRING) == 0)
		{
			found_header = 1;
			continue;
		}

		/* skip the first lines until the client list section is found */
		if (found_header == 0)
			/* we can't start reading data until this string is found */
			continue;

		fields_num = openvpn_strsplit (buffer,
				fields, STATIC_ARRAY_SIZE (fields));
		if (fields_num < 4)
			continue;

		if (collect_user_count)
			/* If so, sum all users, ignore the individuals*/
		{
			sum_users += 1;
		}
		if (collect_individual_users)
		{
			if (new_naming_schema)
			{
				iostats_submit (name,               /* vpn instance */
						fields[0],          /* "Common Name" */
						atoll (fields[2]),  /* "Bytes Received" */
						atoll (fields[3])); /* "Bytes Sent" */
			}
			else
			{
				iostats_submit (fields[0],          /* "Common Name" */
						NULL,               /* unused when in multimode */
						atoll (fields[2]),  /* "Bytes Received" */
						atoll (fields[3])); /* "Bytes Sent" */
			}
		}
	}

	if (ferror (fh))
		return (0);

	if (collect_user_count)
		numusers_submit(name, name, sum_users);

	return (1);
} /* int multi1_read */

/* for reading status version 2 */
static int multi2_read (const char *name, FILE *fh)
{
	char buffer[1024];
	char *fields[10];
	const int max_fields = STATIC_ARRAY_SIZE (fields);
	int  fields_num, read = 0;
	long long sum_users    = 0;

	while (fgets (buffer, sizeof (buffer), fh) != NULL)
	{
		fields_num = openvpn_strsplit (buffer, fields, max_fields);

		/* status file is generated by openvpn/multi.c:multi_print_status()
		 * http://svn.openvpn.net/projects/openvpn/trunk/openvpn/multi.c
		 *
		 * The line we're expecting has 8 fields. We ignore all lines
		 *  with more or less fields.
		 */
		if (fields_num != 8)
			continue;

		if (strcmp (fields[0], "CLIENT_LIST") != 0)
			continue;

		if (collect_user_count)
			/* If so, sum all users, ignore the individuals*/
		{
			sum_users += 1;
		}
		if (collect_individual_users)
		{
			if (new_naming_schema)
			{
				/* plugin inst = file name, type inst = fields[1] */
				iostats_submit (name,               /* vpn instance */
						fields[1],          /* "Common Name" */
						atoll (fields[4]),  /* "Bytes Received" */
						atoll (fields[5])); /* "Bytes Sent" */
			}
			else
			{
				/* plugin inst = fields[1], type inst = "" */
				iostats_submit (fields[1],          /* "Common Name" */
						NULL,               /* unused when in multimode */
						atoll (fields[4]),  /* "Bytes Received" */
						atoll (fields[5])); /* "Bytes Sent" */
			}
		}

		read = 1;
	}

	if (collect_user_count)
	{
		numusers_submit(name, name, sum_users);
		read = 1;
	}

	return (read);
} /* int multi2_read */

/* for reading status version 3 */
static int multi3_read (const char *name, FILE *fh)
{
	char buffer[1024];
	char *fields[15];
	const int max_fields = STATIC_ARRAY_SIZE (fields);
	int  fields_num, read = 0;
	long long sum_users    = 0;

	while (fgets (buffer, sizeof (buffer), fh) != NULL)
	{
		fields_num = strsplit (buffer, fields, max_fields);

		/* status file is generated by openvpn/multi.c:multi_print_status()
		 * http://svn.openvpn.net/projects/openvpn/trunk/openvpn/multi.c
		 *
		 * The line we're expecting has 12 fields. We ignore all lines
		 *  with more or less fields.
		 */
		if (fields_num != 12)
		{
			continue;
		}
		else
		{
			if (strcmp (fields[0], "CLIENT_LIST") != 0)
				continue;

			if (collect_user_count)
				/* If so, sum all users, ignore the individuals*/
			{
				sum_users += 1;
			}

			if (collect_individual_users)
			{
				if (new_naming_schema)
				{
					iostats_submit (name,               /* vpn instance */
							fields[1],          /* "Common Name" */
							atoll (fields[4]),  /* "Bytes Received" */
							atoll (fields[5])); /* "Bytes Sent" */
				}
				else
				{
					iostats_submit (fields[1],          /* "Common Name" */
							NULL,               /* unused when in multimode */
							atoll (fields[4]),  /* "Bytes Received" */
							atoll (fields[5])); /* "Bytes Sent" */
				}
			}

			read = 1;
		}
	}

	if (collect_user_count)
	{
		numusers_submit(name, name, sum_users);
		read = 1;
	}

	return (read);
} /* int multi3_read */

/* for reading status version 4 */
static int multi4_read (const char *name, FILE *fh)
{
	char buffer[1024];
	char *fields[11];
	const int max_fields = STATIC_ARRAY_SIZE (fields);
	int  fields_num, read = 0;
	long long sum_users    = 0;

	while (fgets (buffer, sizeof (buffer), fh) != NULL)
	{
		fields_num = openvpn_strsplit (buffer, fields, max_fields);

		/* status file is generated by openvpn/multi.c:multi_print_status()
		 * http://svn.openvpn.net/projects/openvpn/trunk/openvpn/multi.c
		 *
		 * The line we're expecting has 9 fields. We ignore all lines
		 *  with more or less fields.
		 */
		if (fields_num != 9)
			continue;


		if (strcmp (fields[0], "CLIENT_LIST") != 0)
			continue;


		if (collect_user_count)
			/* If so, sum all users, ignore the individuals*/
		{
			sum_users += 1;
		}
		if (collect_individual_users)
		{
			if (new_naming_schema)
			{
				/* plugin inst = file name, type inst = fields[1] */
				iostats_submit (name,               /* vpn instance */
						fields[1],          /* "Common Name" */
						atoll (fields[4]),  /* "Bytes Received" */
						atoll (fields[5])); /* "Bytes Sent" */
			}
			else
			{
				/* plugin inst = fields[1], type inst = "" */
				iostats_submit (fields[1],          /* "Common Name" */
						NULL,               /* unused when in multimode */
						atoll (fields[4]),  /* "Bytes Received" */
						atoll (fields[5])); /* "Bytes Sent" */
			}
		}

		read = 1;
	}

	if (collect_user_count)
	{
		numusers_submit(name, name, sum_users);
		read = 1;
	}

	return (read);
} /* int multi4_read */

/* read callback */
static int openvpn_read (void)
{
	FILE *fh;
	int  i, read;

	read = 0;

	/* call the right read function for every status entry in the list */
	for (i = 0; i < vpn_num; i++)
	{
		int vpn_read = 0;

		fh = fopen (vpn_list[i]->file, "r");
		if (fh == NULL)
		{
			char errbuf[1024];
			WARNING ("openvpn plugin: fopen(%s) failed: %s", vpn_list[i]->file,
					sstrerror (errno, errbuf, sizeof (errbuf)));

			continue;
		}

		switch (vpn_list[i]->version)
		{
			case SINGLE:
				vpn_read = single_read(vpn_list[i]->name, fh);
				break;

			case MULTI1:
				vpn_read = multi1_read(vpn_list[i]->name, fh);
				break;

			case MULTI2:
				vpn_read = multi2_read(vpn_list[i]->name, fh);
				break;

			case MULTI3:
				vpn_read = multi3_read(vpn_list[i]->name, fh);
				break;

			case MULTI4:
				vpn_read = multi4_read(vpn_list[i]->name, fh);
				break;
		}

		fclose (fh);
		read += vpn_read;
	}

	return (read ? 0 : -1);
} /* int openvpn_read */

static int version_detect (const char *filename)
{
	FILE *fh;
	char buffer[1024];
	int version = 0;

	/* Sanity checking. We're called from the config handling routine, so
	 * better play it save. */
	if ((filename == NULL) || (*filename == 0))
		return (0);

	fh = fopen (filename, "r");
	if (fh == NULL)
	{
		char errbuf[1024];
		WARNING ("openvpn plugin: Unable to read \"%s\": %s", filename,
				sstrerror (errno, errbuf, sizeof (errbuf)));
		return (0);
	}

	/* now search for the specific multimode data format */
	while ((fgets (buffer, sizeof (buffer), fh)) != NULL)
	{
		/* we look at the first line searching for SINGLE mode configuration */
		if (strcmp (buffer, VSSTRING) == 0)
		{
			DEBUG ("openvpn plugin: found status file version SINGLE");
			version = SINGLE;
			break;
		}
		/* searching for multi version 1 */
		else if (strcmp (buffer, V1STRING) == 0)
		{
			DEBUG ("openvpn plugin: found status file version MULTI1");
			version = MULTI1;
			break;
		}
		/* searching for multi version 2 */
		else if (strcmp (buffer, V2STRING) == 0)
		{
			DEBUG ("openvpn plugin: found status file version MULTI2");
			version = MULTI2;
			break;
		}
		/* searching for multi version 3 */
		else if (strcmp (buffer, V3STRING) == 0)
		{
			DEBUG ("openvpn plugin: found status file version MULTI3");
			version = MULTI3;
			break;
		}
		/* searching for multi version 4 */
		else if (strcmp (buffer, V4STRING) == 0)
		{
			DEBUG ("openvpn plugin: found status file version MULTI4");
			version = MULTI4;
			break;
		}
	}

	if (version == 0)
	{
		/* This is only reached during configuration, so complaining to
		 * the user is in order. */
		NOTICE ("openvpn plugin: %s: Unknown file format, please "
				"report this as bug. Make sure to include "
				"your status file, so the plugin can "
				"be adapted.", filename);
	}

	fclose (fh);

	return version;
} /* int version_detect */

static int openvpn_config (const char *key, const char *value)
{
	if (strcasecmp ("StatusFile", key) == 0)
	{
		char    *status_file, *status_name, *filename;
		int     status_version, i;
		vpn_status_t *temp;

		/* try to detect the status file format */
		status_version = version_detect (value);

		if (status_version == 0)
		{
			WARNING ("openvpn plugin: unable to detect status version, \
					discarding status file \"%s\".", value);
			return (1);
		}

		status_file = sstrdup (value);
		if (status_file == NULL)
		{
			char errbuf[1024];
			WARNING ("openvpn plugin: sstrdup failed: %s",
					sstrerror (errno, errbuf, sizeof (errbuf)));
			return (1);
		}

		/* it determines the file name as string starting at location filename + 1 */
		filename = strrchr (status_file, (int) '/');
		if (filename == NULL)
		{
			/* status_file is already the file name only */
			status_name = status_file;
		}
		else
		{
			/* doesn't waste memory, uses status_file starting at filename + 1 */
			status_name = filename + 1;
		}

		/* scan the list looking for a clone */
		for (i = 0; i < vpn_num; i++)
		{
			if (strcasecmp (vpn_list[i]->name, status_name) == 0)
			{
				WARNING ("openvpn plugin: status filename \"%s\" "
						"already used, please choose a "
						"different one.", status_name);
				sfree (status_file);
				return (1);
			}
		}

		/* create a new vpn element since file, version and name are ok */
		temp = malloc (sizeof (*temp));
		if (temp == NULL)
		{
			char errbuf[1024];
			ERROR ("openvpn plugin: malloc failed: %s",
					sstrerror (errno, errbuf, sizeof (errbuf)));
			sfree (status_file);
			return (1);
		}
		temp->file = status_file;
		temp->version = status_version;
		temp->name = status_name;

		vpn_status_t **tmp_list = realloc (vpn_list, (vpn_num + 1) * sizeof (*vpn_list));
		if (tmp_list == NULL)
		{
			char errbuf[1024];
			ERROR ("openvpn plugin: realloc failed: %s",
					sstrerror (errno, errbuf, sizeof (errbuf)));

			sfree (vpn_list);
			sfree (temp->file);
			sfree (temp);
			return (1);
		}
		vpn_list = tmp_list;

		vpn_list[vpn_num] = temp;
		vpn_num++;

		DEBUG ("openvpn plugin: status file \"%s\" added", temp->file);

	} /* if (strcasecmp ("StatusFile", key) == 0) */
	else if ((strcasecmp ("CollectCompression", key) == 0)
		|| (strcasecmp ("Compression", key) == 0)) /* old, deprecated name */
	{
		if (IS_FALSE (value))
			collect_compression = 0;
		else
			collect_compression = 1;
	} /* if (strcasecmp ("CollectCompression", key) == 0) */
	else if (strcasecmp ("ImprovedNamingSchema", key) == 0)
	{
		if (IS_TRUE (value))
		{
			DEBUG ("openvpn plugin: using the new naming schema");
			new_naming_schema = 1;
		}
		else
		{
			new_naming_schema = 0;
		}
	} /* if (strcasecmp ("ImprovedNamingSchema", key) == 0) */
	else if (strcasecmp("CollectUserCount", key) == 0)
	{
		if (IS_TRUE(value))
			collect_user_count = 1;
		else
			collect_user_count = 0;
	} /* if (strcasecmp("CollectUserCount", key) == 0) */
	else if (strcasecmp("CollectIndividualUsers", key) == 0)
	{
		if (IS_FALSE (value))
			collect_individual_users = 0;
		else
			collect_individual_users = 1;
	} /* if (strcasecmp("CollectIndividualUsers", key) == 0) */
	else
	{
		return (-1);
	}

	return (0);
} /* int openvpn_config */

/* shutdown callback */
static int openvpn_shutdown (void)
{
	int i;

	for (i = 0; i < vpn_num; i++)
	{
		sfree (vpn_list[i]->file);
		sfree (vpn_list[i]);
	}

	sfree (vpn_list);

	return (0);
} /* int openvpn_shutdown */

static int openvpn_init (void)
{
	if (!collect_individual_users
			&& !collect_compression
			&& !collect_user_count)
	{
		WARNING ("OpenVPN plugin: Neither `CollectIndividualUsers', "
				"`CollectCompression', nor `CollectUserCount' is true. There's no "
				"data left to collect.");
		return (-1);
	}

	plugin_register_read ("openvpn", openvpn_read);
	plugin_register_shutdown ("openvpn", openvpn_shutdown);

	return (0);
} /* int openvpn_init */

void module_register (void)
{
	plugin_register_config ("openvpn", openvpn_config,
			config_keys, config_keys_num);
	plugin_register_init ("openvpn", openvpn_init);
} /* void module_register */

/* vim: set sw=2 ts=2 : */

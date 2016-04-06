#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#include "ganglia_priv.h"
#include "ganglia.h"
#include "influxdb.h"

#include <confuse.h>
#include <apr_pools.h>
#include <apr_strings.h>
#include <apr_tables.h>
#include <apr_net.h>
//#include <apr_file_io.h>
//#include <apr_network_io.h>
#include <apr_lib.h>
//#include <sys/types.h>
//#include <sys/stat.h>
//#include <fcntl.h>
//#include <dirent.h>
//#include <fnmatch.h>


/* Send create InfluxDB send channels */
Ganglia_influxdb_send_channels
Ganglia_influxdb_send_channels_create( Ganglia_pool p, Ganglia_gmond_config config ) 
{
    apr_array_header_t *send_channels = NULL;
    cfg_t *cfg = (cfg_t *) config;
    unsigned int num_influxdb_send_channels = cfg_size ( cfg, "influxdb_send_channel");
    int i;
    apr_pool_t *context = (apr_pool_t*)p;

    debug_msg("Found %d influxdb_send_channel stanzas", num_influxdb_send_channels);

    //No channels?  We're done here.
    if (num_influxdb_send_channels <= 0) {
        return (Ganglia_influxdb_send_channels)send_channels;
    }

    send_channels = apr_array_make(context, 
                                   num_influxdb_send_channels, 
                                   sizeof(apr_socket_t *));


    for(i=0; i<num_influxdb_send_channels; i++) {
        cfg_t *influxdb_send_channel;
        int port = -1;
        char *host, *database, *default_tags;
        apr_socket_t *socket = NULL;
        apr_pool_t *pool = NULL;

        influxdb_send_channel = cfg_getnsec(cfg, "influxdb_send_channel", i);
        host                  = cfg_getstr(influxdb_send_channel, "host");
        port                  = cfg_getint(influxdb_send_channel, "port");
        database              = cfg_getstr(influxdb_send_channel, "database");
        default_tags          = cfg_getstr(influxdb_send_channel, "default_tags");

        debug_msg("influxdb_send_channel: dest=%s:%d database=%s default_tags=%s",
                  host ? host : "NULL",
                  port,
                  database ? database : "NULL",
                  default_tags ? default_tags : "NULL");

        apr_pool_create(&pool, context);
        socket = create_udp_client(pool, host, port, NULL, NULL, 0); 
        *(apr_socket_t **)apr_array_push(send_channels) = socket;
        debug_msg("end of influx cfg loop");
    }

    return (Ganglia_influxdb_send_channels)send_channels;
}


// Hostname should already be in the keys table
influxdb_metric_t
create_influxdb_metric(
    apr_pool_t *pool,
    const char *name,
    const char *value,
    influxdb_types type,
    unsigned long int timestamp) {

    influxdb_metric_t influxdb_metric;

    //influxdb_metric.tags      = apr_pstrdup(pool, tags);
    //influxdb_metric.value     = apr_pstrdup(pool, values);
    //influxdb_metric.     = apr_pstrdup(pool, values);
    //influxdb_metric.timestamp = (unsigned long int)apr_time_now()*1000;

    return influxdb_metric;
}


/* EOF */


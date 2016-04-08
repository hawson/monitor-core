#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "ganglia_priv.h"
#include "ganglia.h"
#include "influxdb.h"
#include "gm_msg.h"

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
        cfg_t *influxdb_send_channel = NULL;
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
        if (!socket) {
            err_msg("Unable to create UDP client for %s:%d. No route to IP? Exiting.", host, port);
            exit(1);
        }

        *(apr_socket_t **)apr_array_push(send_channels) = socket;
        debug_msg("end of influx cfg loop");
    }

    return (Ganglia_influxdb_send_channels)send_channels;
}


/* Given a string, try to guess the type:  INT, FLOAT, STRING */
influxdb_types guess_type(const char* string) {

    unsigned int base = 10;
    char * endptr;
    long int value;

    errno = 0;

    value = strtol(string, &endptr, base);

    // if there's an error here, it's probably a string...
    if (  (errno == ERANGE && (value == LONG_MAX || value == LONG_MIN))
        ||(errno != 0 && value == 0)) {
        perror("strtol");

        debug_msg("\tguess error: %s, returning STR", string);

        return STR;
    }

    // no digits found, so call it a string
    if (endptr == string) {
        debug_msg("\tno digits: %s, returning STR", string);
        return STR;
    }

    if (*endptr != '\0') {
        debug_msg("\tstuff left over: %s, returning FLOAT", string);
        return FLOAT;
    } else {
        debug_msg("\tLooks good! %s, returning INT", string);
        return INT;
    }

    debug_msg("\tI quit %s, returning UNDEF", string);
    return UNDEF;

}

// Hostname should already be in the keys table
influxdb_metric_t
create_influxdb_metric(
    apr_pool_t *pool,
    const char *name,
    const char *value,
    influxdb_types type,
    unsigned long int timestamp) {

    influxdb_metric_t metric;

    metric.measurement = apr_pstrdup(pool, name);
    metric.value       = apr_pstrdup(pool, value);
    metric.timestamp   = timestamp ? timestamp : (unsigned long int)apr_time_now()*1000;
    metric.type        = type ? type : guess_type(value);
    //metric.tags      = apr_pstrdup(pool, tags);

    return metric;
}


apr_table_t * get_influxdb_default_tags(void) {

    return "";
    return "default=key";
}


char * build_influxdb_line(
    apr_pool_t  *pool,          // pool to use...
    influxdb_metric_t *metric,  // single metric/value pair
    char *hostname,       // NULL to use base hostname, !NULL for spoofing.
    const char *tags           // a string of tags to add
    ) {

    char *local_hostname = NULL;
    char *local_tags = "";
    char *local_value = NULL;

    char *default_tags = get_influxdb_default_tags();

    if (*hostname) {
        local_hostname = hostname;
    } else {
        apr_gethostname( local_hostname, APRMAXHOSTLEN+1, pool);
    }

    if (strnlen(default_tags, MAX_VALUE_LENGTH)) {
        local_tags = apr_pstrcat(pool, ",", default_tags, NULL);
    }

    switch (guess_type(metric->value)) {
        case INT:
            local_value = apr_psprintf(pool, "%si", metric->value);
            break;

        default:
            local_value = metric->value;
    }


    return apr_psprintf(pool, "%s,hostname=%s%s %s %lu",
            metric->measurement,
            local_hostname,
            local_tags,
            local_value,
            metric->timestamp);

}


void dump_metric(const influxdb_metric_t *metric) {
    const influxdb_types type = metric->type;
    debug_msg("      metric=%s", metric->measurement);
    debug_msg("      value=%s", metric->value);
    debug_msg("      type=%s", INT   == type ? "INT" :
                               FLOAT == type ? "FLOAT" :
                               STR   == type ? "STR" : "UNDEF" );
    debug_msg("      ts=%lu", metric->timestamp);
}

int send_influxdb(
    apr_pool_t *pool, 
    const Ganglia_influxdb_send_channels *influxdb_channels, 
    const apr_array_header_t *metrics,
    const Ganglia_gmond_config config
    ) {

    apr_status_t status;
    apr_size_t size;
    int num_errors = 0;
    int i,j;
    int debug_level = get_debug_msg_level();

    cfg_t *cfg = (cfg_t *) config;
    cfg_t *influxdb_send_channel = NULL;

    apr_array_header_t *channel = (apr_array_header_t*)influxdb_channels;


    if (debug_level) {
        influxdb_send_channel = cfg_getnsec(cfg, "influxdb_send_channel",i);
    }

    for (i=0; i < channel->nelts; i++) {

        apr_socket_t *socket = ((apr_socket_t **)(channel->elts))[i];
        apr_array_header_t *metric = (apr_array_header_t*)metrics;

        if (debug_level && influxdb_send_channel) {
            char * host     = cfg_getstr(influxdb_send_channel, "host");
            int port        = cfg_getint(influxdb_send_channel, "port");
            char * def_tags = cfg_getstr(influxdb_send_channel, "default_tags");
            char * database = cfg_getstr(influxdb_send_channel, "database");

            debug_msg("send_influxdb(%d=%s@%s:%d->%s)", i, database,host, port, def_tags);
        }

        for (int m=0; m < metrics->nelts; m++) {

            

        }


    }

    

    return num_errors;
}

/* EOF */


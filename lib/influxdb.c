#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <errno.h>

#include "ganglia_priv.h"
#include "ganglia.h"
#include "influxdb.h"
#include "gm_msg.h"

#include <confuse.h>
#include <apr_pools.h>
#include <apr_strings.h>
#include <apr_tables.h>
#include <apr_errno.h>
#include "apr_net.h"
#include <apr_network_io.h>
#include <apr_lib.h>


//typedef struct influxdb_send_channel* influxdb_send_channel;

/* Send create InfluxDB send channels */
Ganglia_influxdb_send_channels
Ganglia_influxdb_send_channels_create( Ganglia_pool p, Ganglia_gmond_config config )
{
    int i;
    apr_pool_t *context = (apr_pool_t*)p;

    apr_array_header_t *send_channels = NULL;

    cfg_t *cfg = (cfg_t *) config;

    int num_influxdb_send_channels = cfg_size ( cfg, "influxdb_send_channel");

    debug_msg("Found %d influxdb_send_channel stanzas", num_influxdb_send_channels);

    //No channels?  We're done here.
    if (num_influxdb_send_channels <= 0) {
        return (Ganglia_influxdb_send_channels)send_channels;
    }

    send_channels = apr_array_make(context,
                                   num_influxdb_send_channels,
                                   (int)sizeof(influxdb_send_channel*));


    for(i=0; i<num_influxdb_send_channels; i++) {

        influxdb_send_channel *channel = apr_palloc(context, sizeof(influxdb_send_channel));

        cfg_t *influxdb_cfg = NULL;
        long int port = 0;
        char *host = NULL;
        char *default_tags = NULL;
        int def_tag_len =0;

        influxdb_cfg = cfg_getnsec(cfg, "influxdb_send_channel", (unsigned int) i);
        host         = cfg_getstr(influxdb_cfg, "host");
        port         = cfg_getint(influxdb_cfg, "port");
        default_tags = cfg_getstr(influxdb_cfg, "default_tags");

        if (!host)
            err_quit("Invalid or missing influxdb_send_channel->host attribute");
        if (!port)
            err_quit("Invalid or missing influxdb_send_channel->port attribute");

        if (default_tags) {
            def_tag_len = strnlen(default_tags, MAX_DEF_TAG_LENGTH);
            strncpy(channel->default_tags, default_tags, MAX_DEF_TAG_LENGTH);
        }

        channel->default_tags[def_tag_len] = '\0';

        debug_msg("influxdb_send_channel: dest=%s:%lu default_tags=%s",
                  host ? host : "NULL",
                  port,
                  channel->default_tags ? channel->default_tags : "NULL");

        channel->socket = create_udp_client(context, host, port, NULL, NULL, 0);

        if (!channel->socket) {
            err_msg("Unable to create UDP client for %s:%lu. No route to IP? Exiting.", host, port);
            exit(EXIT_FAILURE);
        }

        APR_ARRAY_PUSH(send_channels, influxdb_send_channel*) = channel;
        cfg_free(influxdb_cfg);
        debug_msg("end of influx cfg loop");
    }

    return (Ganglia_influxdb_send_channels)send_channels;
}


/* Given a string, try to guess the type:  INT, FLOAT, STRING */
influxdb_types guess_type(const char* string) {

    unsigned int base = 10;
    char * endptr;
    long int value;

    int debug_level = get_debug_msg_level();

    errno = 0;

    value = strtol(string, &endptr, base);

    // if there's an error here, it's probably a string...
    if (  (errno == ERANGE && (value == LONG_MAX || value == LONG_MIN))
        ||(errno != 0 && value == 0)) {
        perror("strtol");

        if (debug_level > 2)
            debug_msg("\tGT:guess error: %s, returning STR", string);

        return STR;
    }

    // no digits found.  Possibly a boolean or string
    if (endptr == string) {

        if (!strcmp("t", string) || !strcmp("f", string) ||
            !strcmp("T", string) || !strcmp("F", string) ||
            !strcmp("true", string) || !strcmp("false", string) ||
            !strcmp("True", string) || !strcmp("False", string) ||
            !strcmp("TRUE", string) || !strcmp("FALSE", string)) {
            debug_msg("\tGT:no digits, looks like a bool: %s, returning STR", string);
            return BOOL;
        }

        if (debug_level > 2)
            debug_msg("\tGT:no digits, not bool: %s, returning STR", string);
        return STR;
    }

    if (*endptr != '\0') {
        if (debug_level > 2)
            debug_msg("\tGT:stuff left over: %s, returning FLOAT", string);
        return FLOAT;
    } else {
        if (debug_level > 2)
            debug_msg("\tGT:Looks like integer! %s, returning INT", string);
        return INT;
    }

    if (debug_level > 2)
        debug_msg("\tGT:No clue: %s, returning UNDEF", string);
    return UNDEF;

}

// Hostname should already be in the keys table
influxdb_metric_t *
create_influxdb_metric(
    apr_pool_t *pool,
    const char *name,
    const char *value,
    const char *measurement,
    influxdb_types type,
    unsigned long int timestamp) {

    influxdb_metric_t *metric;
    metric = apr_palloc(pool, sizeof(influxdb_metric_t));


    metric->name        = apr_pstrdup(pool, name);
    metric->measurement = apr_pstrdup(pool, measurement);
    metric->value       = apr_pstrdup(pool, value);
    metric->timestamp   = timestamp ? timestamp : (unsigned long int)apr_time_now()*1000;
    metric->type        = type ? type : guess_type(value);
    //metric.tags      = apr_pstrdup(pool, tags);

    return metric;
}



char * build_influxdb_line(
    apr_pool_t  *pool,          // pool to use...
    influxdb_metric_t *metric,  // single metric/value pair
    const char *hostname,       // NULL to use base hostname, !NULL for spoofing.
    const char *tags           // a string of tags to add
    ) {

    char *local_hostname = NULL;
    char *local_tags = NULL;
    char *local_value = NULL;

    char empty_str[]="";

    if (hostname) {
        local_hostname = hostname;
    } else {
        int rc = apr_gethostname( local_hostname, APRMAXHOSTLEN+1, pool);
        if (rc) {
            err_sys("Error getting hostname(%d)", rc);
        }
    }

    if (tags) {
        local_tags = apr_pstrcat(pool, ",", tags, NULL);
    }

    if (!local_tags) {
        local_tags = empty_str;
    }

    switch (guess_type(metric->value)) {
        case INT:
            local_value = apr_psprintf(pool, "%si", metric->value);
            break;

        case STR:
            local_value = apr_psprintf(pool, "\"%s\"", metric->value);
            break;

        default:
            local_value = metric->value;
    }


    return apr_psprintf(pool, "%s,hostname=%s%s %s=%s %lu",
            metric->measurement ? metric->measurement : metric->name,
            local_hostname,
            local_tags,
            metric->name,
            local_value,
            metric->timestamp);

}


void dump_metric(const influxdb_metric_t *metric) {
    const influxdb_types type = metric->type;

    if (!metric)
        return;

    debug_msg("    metric=%-10s type=%s", metric->name, 
                              (INT   == type ? "INT" :
                               FLOAT == type ? "FLOAT" :
                               STR   == type ? "STR" : "UNDEF" )); 
    debug_msg("     value=%-10s   ts=%lu", metric->value, metric->timestamp);
}


apr_status_t influxdb_emit_udp(
    apr_socket_t *socket,
    char* buf,
    apr_size_t *buf_len) {
    apr_status_t status = apr_socket_send(socket, buf, buf_len);

    if(status) {
        char error[128] = "";
        char *rv;
        rv = apr_strerror(status, error, sizeof(error));
        debug_msg("  influxdb send error (%s) %u: %s", rv, status, error);
    }

    return status;

}

/* take string in src, and escape characters listed in switch() block
 * THE CALLING FUNCTION MUST ENSURE THAT dest IS BIG ENOUGH!!!!
 * returns non-zero on error.
 */
int influxdb_escape(char* dest, const char* src, unsigned int maxlen) {
    int rc = 0;
    int i=0;
    //char* pos = NULL;

    if (!src)
        return 1;
    if (!dest)
        return 2;

    while (*src  && (i < maxlen)) {
        switch (*src) {
            case ' ':
            case ',': i++;
                      *dest++ = '\\';
            default:  *dest++ = *src++;
        }
        i++;
        *dest=0;
    }

    return rc;
}


char * influxdb_escape_string(apr_pool_t *pool, const char* str) {
    char *newstr = NULL;
    int strlen = 0;

    if (NULL == str)
        return NULL;

    strlen = strnlen(str, MAX_VALUE_LENGTH);

    if (strlen) {
        int newstrlen = strlen * 2 + 1; //worst case: escape everything + null
        newstr = apr_palloc(pool, newstrlen);  
        influxdb_escape(newstr, str, newstrlen);
    }

    return newstr;
}

void send_influxdb(
    apr_pool_t *pool,
    const apr_array_header_t *influxdb_channels,
    const apr_array_header_t *metrics,
    const char *hostname,
    int max_udp_message_len
    ) {

    int debug_level = get_debug_msg_level();
    int i;

    for (i=0; i < influxdb_channels->nelts; i++) {

        char* buf = NULL;
        apr_size_t buf_len = 0;
        int lines;
        influxdb_send_channel *channel;
        apr_pool_t *channel_pool;
        apr_status_t status;

        debug_msg("  Sending to influxdb channel %d", i);

        status = apr_pool_create(&channel_pool, pool);
        if (status) {
            err_quit("Failed to create influxdb channel pool! (%d) at %s:%d", status, __FILE__,__LINE__);
        }

        channel = APR_ARRAY_IDX(influxdb_channels, i, influxdb_send_channel*);

        if (debug_level ) {
            debug_msg("  default channel tags: %s", channel->default_tags);
        }

        debug_msg("  starting metric loop");
        for (int m=0; m < metrics->nelts; m++) {
            influxdb_metric_t *metric;
            char *line;
            int line_len;

            metric = APR_ARRAY_IDX(metrics,m, influxdb_metric_t*);

            debug_msg("    metric[%d]: name=%s meas=%s value=%s ts=%lu hostname=%s tags=%s",
                m,
                influxdb_escape_string(pool, metric->name),
                influxdb_escape_string(pool, metric->measurement),
                metric->value,
                metric->timestamp,
                influxdb_escape_string(pool, hostname), /* incase "hostname" is really a service or non-host-like object */
                channel->default_tags /* this is not escaped here, as it is assumed the user has alread handled this in the .conf file. Ha. */
            ) ;

            line = build_influxdb_line(channel_pool, metric, hostname, channel->default_tags);
            line_len = strnlen(line, max_udp_message_len);

            if (debug_level > 2)
                debug_msg("    (%d)->%s", line_len, line);

            /* check current buf_len + new_line_len >= maximum length:  send! */
            if (buf && (buf_len + line_len >= max_udp_message_len)) {

                apr_size_t orig_len = (unsigned int)buf_len;
                buf_len = strnlen(buf, max_udp_message_len);

                /* append newline if needed */
                if ('\n' != buf[buf_len-1]) {
                    buf = apr_pstrcat(channel_pool, buf, "\n", NULL);
                }

                status = influxdb_emit_udp(channel->socket, buf, &buf_len);
                debug_msg("    Send frag: actual %lub, orig %lub, status %d)", buf_len, orig_len, status);
                lines = 0;
                buf = NULL;
            }

            /* if !buf, make it */
            if (!buf) {
                buf = apr_pstrcat(channel_pool, line, "\n", NULL);
                buf_len = line_len;
                lines = 1;
            } else {
                buf = apr_pstrcat(channel_pool, buf, line, "\n", NULL);
                buf_len = strnlen(buf, max_udp_message_len);
                lines+=1;
            }

        }

        if (lines) {
            // emit final (perhaps only) UDP packet for this batch of metrics!
            unsigned long int orig_len = (unsigned long int )buf_len;
            buf_len = strnlen(buf, max_udp_message_len);

            /* append newline if needed */
            if ('\n' != buf[buf_len-1]) {
                buf = apr_pstrcat(channel_pool, buf, "\n", NULL);
            }

            status = influxdb_emit_udp(channel->socket, buf, &buf_len);
            lines = 0;
            debug_msg("    Send final: actual %lub, orig %lub, status %d)", buf_len, orig_len, status);
        }
        apr_pool_destroy(channel_pool);


    }


    return num_errors;
}

/* EOF */


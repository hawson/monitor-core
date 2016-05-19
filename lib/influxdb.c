#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <errno.h>

#include "ganglia_priv.h"
#include "ganglia.h"
#include "influxdb.h"
#include "gm_msg.h"
#include "gm_scoreboard.h"

#include <confuse.h>
#include <apr_pools.h>
#include <apr_strings.h>
#include <apr_tables.h>
#include <apr_errno.h>
#include "apr_net.h"
#include <apr_network_io.h>
#include <apr_lib.h>
#include <sys/types.h>
#include <regex.h>


//typedef struct influxdb_send_channel* influxdb_send_channel;

//Need only compile these once.
//BOOL is the last value of the enum in influxdb.h
#define MAX_MATCHES 1
regex_t re_compiled[BOOL];
static const char* regexes[] = {
    ".*",
    "^[-+]?[0-9]+$",                //INT
    "^[-+]?([0-9]*)\\.([0-9]+)$",   //FLOAT
    ".*",                           //STR
    "^([tT](rue)?|[fF](alse)?|TRUE|FALSE))$",  //BOOL
};


/* Send create InfluxDB send channels */
Ganglia_influxdb_send_channels
Ganglia_influxdb_send_channels_create( Ganglia_pool p, Ganglia_gmond_config config )
{
    int i;
    apr_pool_t *context = (apr_pool_t*)p;

    apr_array_header_t *send_channels = NULL;

    cfg_t *cfg = (cfg_t *) config;

    int num_influxdb_send_channels = (int)cfg_size ( cfg, "influxdb_send_channel");

    debug_msg("Found %d influxdb_send_channel stanzas", num_influxdb_send_channels);

    //No channels?  We're done here.
    if (num_influxdb_send_channels <= 0) {
        return NULL;
    }

    send_channels = apr_array_make(context,
                                   num_influxdb_send_channels,
                                   (int)sizeof(influxdb_send_channel*));


    for(i=0; i<num_influxdb_send_channels; i++) {

        influxdb_send_channel *channel = apr_palloc(context, sizeof(influxdb_send_channel));

        cfg_t *influxdb_cfg = NULL;
        long int port = 0;
        char *host = NULL;
        char *influxdb_tags = NULL;
        int def_tag_len =0;

        influxdb_cfg = cfg_getnsec(cfg, "influxdb_send_channel", (unsigned int) i);
        host         = cfg_getstr(influxdb_cfg, "host");
        port         = cfg_getint(influxdb_cfg, "port");
        influxdb_tags = cfg_getstr(influxdb_cfg, "tags");

        if (NULL == host)
            err_quit("Invalid or missing influxdb_send_channel->host attribute");
        if (!port)
            err_quit("Invalid or missing influxdb_send_channel->port attribute");

        if (influxdb_tags) {
            def_tag_len = strnlen(influxdb_tags, MAX_DEF_TAG_LENGTH);
            strncpy(channel->influxdb_tags, influxdb_tags, MAX_DEF_TAG_LENGTH);
        }

        channel->influxdb_tags[def_tag_len] = '\0';

        debug_msg("influxdb_send_channel: dest=%s:%lu influxdb_tags=%s",
                  host ? host : "NULL",
                  port,
                  channel->influxdb_tags ? channel->influxdb_tags : "NULL");

        channel->socket = create_udp_client(context, host, (apr_port_t)port, NULL, NULL, 0);

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


/* Compile the regexes once, since they never change */
void re_init(regex_t* compiled, const char** regexes) {

    char re_err[MAX_RE_LENGTH];
    int i, rv;
    size_t err_size;

    for (i=0; i<=BOOL; i++) {
        rv = regcomp(&compiled[i], regexes[i], REG_EXTENDED | REG_NOSUB );
        err_size = regerror(rv, &compiled[BOOL], re_err, MAX_RE_LENGTH);
        debug_msg("(%d)%s", (int)err_size, re_err);
        if (rv) {
            err_quit("Failed compiling regex: %s", regexes[i]);
        } else {
            debug_msg("Intialized regex: /%s/", regexes[i]);
        }
    }

}



/* Given a string, try to guess the type:  INT, FLOAT, STRING */
influxdb_types guess_type(
    const char* string
    ) {

    int debug_level = get_debug_msg_level();

    static int re_initialized = 0;

    char re_err[MAX_RE_LENGTH];
    int rc;

    errno = 0;

    if (!string)
        return UNDEF;

    if (!re_initialized) {
        memset(re_compiled, 0, sizeof(regex_t) * BOOL);
        re_init(re_compiled, regexes);
        re_initialized = 1;
    }

    if (debug_level > 2)
        debug_msg("    Testing [%s] against /%s/", string, regexes[BOOL]);
    rc = regexec(&re_compiled[BOOL], string, 0, NULL, 0);
    if (0 == rc) {
        debug_msg("     GT: %s -> BOOL", string);
        return BOOL;
    } else if (REG_NOMATCH != rc) {
        regerror(rc, &re_compiled[BOOL], re_err, MAX_RE_LENGTH);
        debug_msg("%s", re_err);
    }

    if (debug_level > 2)
        debug_msg("    Testing [%s] against /%s/", string, regexes[FLOAT]);
    rc = regexec(&re_compiled[FLOAT], string, 0, NULL, 0);
    if (0 == rc) {
        debug_msg("     GT: %s -> FLOAT", string);
        return FLOAT;
    } else if (REG_NOMATCH != rc) {
        regerror(rc, &re_compiled[FLOAT], re_err, MAX_RE_LENGTH);
        debug_msg("%s", re_err);
    }

    if (debug_level > 2)
        debug_msg("    Testing [%s] against /%s/", string, regexes[INT]);
    rc = regexec(&re_compiled[INT], string, 0, NULL, 0);
    if (0 == rc) {
        debug_msg("     GT: %s -> INT", string);
        return INT;
    } else if (REG_NOMATCH != rc) {
        regerror(rc, &re_compiled[INT], re_err, MAX_RE_LENGTH);
        debug_msg("%s", re_err);
    }

    return STR;


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
        local_hostname = apr_pstrdup(pool,hostname);
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


    return apr_psprintf(pool, "%s,hostname=%s%s %s=%s %lu\n",
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
    debug_msg("     group=%-10s", metric->measurement);

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
int influxdb_escape(
    char* dest,
    const char* src,
    unsigned int maxlen
    ) {

    int rc = 0;
    unsigned int i=0;
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


char * influxdb_escape_string(
    apr_pool_t *pool,
    const char* str
    ) {
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

/* update_influxdb_scoreboard() is a local function, not
 * needed outside the scope of this file */
#ifdef GSTATUS
void _update_influxdb_scoreboard(int status) {
    if (!status) {
        ganglia_scoreboard_inc(PKTS_INFLUXDB_SENT);
    } else {
        ganglia_scoreboard_inc(PKTS_INFLUXDB_FAILED);
    }
}
#else
#define _update_influxdb_scoreboard(status)
#endif

/* local helper function */
int count_lines(const char *buf) {
    char * pchar = NULL;
    int count = 0;

    pchar = strchr(buf, '\n');
    while (NULL != pchar) {
        count += 1;
        pchar = strchr(pchar+1, '\n');
    }
    return count;
}

void handle_send_buffer(
    apr_socket_t *socket,
    char *buf,
    apr_size_t *buf_len,
    int max_udp_message_len,
    int msg_num) {

    unsigned long int orig_len = (unsigned long int) *buf_len;
    int status = 0;

    status = influxdb_emit_udp(socket, buf, buf_len);
    _update_influxdb_scoreboard(status);
    debug_msg("    Send[%d]: actual %lub, orig %lub, status %d)", msg_num, (unsigned long int)buf_len, orig_len, status);
    if (get_debug_msg_level() >= 5)
        debug_msg("Full packet: [%s]", buf);
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
    int influxdb_packets_sent = 0;

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
            debug_msg("  tags: %s", channel->influxdb_tags);
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
                influxdb_escape_string(pool, hostname), /* in case "hostname" is really a service or non-host-like object */
                channel->influxdb_tags /* this is not escaped here, as it is assumed the user has alread handled this in the .conf file. Ha. */
            ) ;

            line = build_influxdb_line(channel_pool, metric, hostname, channel->influxdb_tags);
            line_len = strnlen(line, max_udp_message_len);

            if (debug_level > 2)
                debug_msg("    (%d)->%s", line_len, line);

            /* check current buf_len + new_line_len >= maximum length-1:  send! */
            /* The "-1" is in case we need to append a newline at the end */
            if (buf && (buf_len + line_len >= max_udp_message_len)) {

                handle_send_buffer(channel->socket, buf, &buf_len, max_udp_message_len, influxdb_packets_sent);
                influxdb_packets_sent += 1;
                buf = NULL;
            }

            /* if !buf, make it */
            if (!buf) {
                buf = apr_pstrdup(channel_pool, line);
            } else {
                buf = apr_pstrcat(channel_pool, buf, line, NULL);

            }

            if (!buf) {
                err_quit("Failed appending newline to buffer before sending.  Exiting!");
            } else {
                buf_len = strnlen(buf, max_udp_message_len);
                lines = count_lines(buf);
            }

        }

        // emit final (perhaps only) UDP packet for this batch of metrics.
        if (lines) {
            handle_send_buffer(channel->socket, buf, &buf_len, max_udp_message_len, -influxdb_packets_sent);
            influxdb_packets_sent += 1;
        }

        apr_pool_destroy(channel_pool);

    }

}

/* EOF */


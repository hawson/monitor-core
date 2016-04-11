#ifndef GM_INFLUXDB_H
#define GM_INFLUXDB_H 1

#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif

#include <gm_value.h>
#include <gm_msg.h>

//#include <apr.h>
//#include <apr_pools.h>
#include <apr_tables.h>
#include <apr_net.h>

/* Defines */

/* Maximum number of influxdb lines to process.  This is an arbitrary
 * limit, and should be refactored...but C is like that. */
#define INFLUXDB_MAX_MSGS 500

/* values may not be longer than MAX_VALUE_LENGTH-1.  This is used for safeguarding some string searches */
#define MAX_VALUE_LENGTH 1024

/* max size of defualt tags */
#define MAX_DEF_TAG_LENGTH 1024

/* typedefs, etc */

typedef struct {
    char *tag_name;
    char *tag_value;
} tag_t;

typedef struct {
    char *value_name;
    char *value_value;
} value_t;

enum influxdb_types {
    UNDEF = 0,  // if this is set, guess the type
    INT   = 1,
    FLOAT = 2,
    STR   = 3
};
typedef enum influxdb_types influxdb_types;

typedef struct influxdb_metric_t {
    unsigned long int timestamp; //time in NANOseconds <sigh>
    char *measurement;
    char *value;
    enum influxdb_types type;
    apr_table_t *keys;
} influxdb_metric_t;

/* Function prototypes */

// this one in ganglia.h
/*Ganglia_influxdb_send_channels Ganglia_influxdb_send_channels_create( 
    Ganglia_pool p, 
    Ganglia_gmond_config config );
 */

/* this is different from the normal ganglia UDP send channels, 
 * since we have to carry along the default tags.  So far as I can tell, 
 * the normal ganglia UDP channels are "merely" apr_socket_t pointers;
 * we need a bit more than that */
typedef struct influxdb_send_channel {
    apr_socket_t *socket; 
    char default_tags[MAX_VALUE_LENGTH]; //meh, should be char*
} influxdb_send_channel;

influxdb_metric_t create_influxdb_metric(
    apr_pool_t *pool,
    const char *name,
    const char *value,
    enum influxdb_types type,
    unsigned long int timestamp) ;

char * build_influxdb_line( apr_pool_t *pool, influxdb_metric_t *metric, char *hostname, const char *tags);


/* given a string containing a value of some sort, try to determine the
 * type, and return the appropriate code.  This is mostly to indicate
 * to influxdb if a value is an INTEGER, so that it can marked as such.
 * Specifically, ints are written to influx by appending an "i" to the 
 * number.  Thus:  "4" is consider a float, whereas "4i" is an integer. */
influxdb_types guess_type(const char* string);


/* dumps a metric struct */
void dump_metric(const influxdb_metric_t *metric);

/* send metrics to influxdb channels */
int send_influxdb ( apr_pool_t *pool, const apr_array_header_t *influxdb_channels, const apr_array_header_t *metrics );

#endif

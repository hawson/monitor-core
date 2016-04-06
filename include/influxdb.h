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

/* Defines */

/* Maximum number of influxdb lines to process.  This is an arbitrary
 * limit, and should be refactored...but C is like that. */
#define INFLUXDB_MAX_MSGS 500

/* values may not be longer than MAX_VALUE_LENGTH-1.  This is used for safeguarding some string searches */
#define MAX_VALUE_LENGTH 1024

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
    unsigned int type;
    apr_table_t *keys;
} influxdb_metric_t;

/* Function prototypes */
Ganglia_influxdb_send_channels Ganglia_influxdb_send_channels_create( 
    Ganglia_pool p, 
    Ganglia_gmond_config config );

influxdb_metric_t create_influxdb_metric(
    apr_pool_t *pool,
    const char *metric_name,
    const char *value,
    enum influxdb_types type,
    unsigned long int timestamp) ;


#endif

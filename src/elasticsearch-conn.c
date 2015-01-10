/* Copyright (c) 2006-2014 Dovecot authors, see the included COPYING file */
/* Copyright (c) 2014 Joshua Atkins <josh@ascendantcom.com> */

#include "lib.h"
#include "array.h"
#include "hash.h"
#include "str.h"
#include "strescape.h"
#include "ioloop.h"
#include "istream.h"
#include "http-url.h"
#include "http-client.h"
#include "elasticsearch-conn.h"

#include <json-c/json.h>
#include <stdio.h>

struct http_client *elasticsearch_http_client = NULL;

struct elasticsearch_connection_post {
    struct elasticsearch_connection *conn;

    struct http_client_request *http_req;

    unsigned int failed:1;
};

struct elasticsearch_lookup_context {
    pool_t result_pool;
    HASH_TABLE(char *, struct elasticsearch_result *) mailboxes;
    ARRAY(struct elasticsearch_result *) results;
    int uid;
    float score;
    const char *box_guid;
    bool results_found;
};

struct elasticsearch_connection {
    char *http_host;
    in_port_t http_port;
    char *http_base_url;
    char *http_failure;

    int request_status;

    struct istream *payload;
    struct io *io;

    /* TODO: should probably move this out to a lookup context struct */
    struct elasticsearch_lookup_context *ctx;

    unsigned int debug:1;
    unsigned int posting:1;
    unsigned int xml_failed:1;
    unsigned int http_ssl:1;
};

int elasticsearch_connection_init(const char *url, bool debug,
    struct elasticsearch_connection **conn_r, const char **error_r)
{
    struct http_client_settings http_set;
    struct elasticsearch_connection *conn;
    struct http_url *http_url;
    const char *error;

    debug = TRUE; /* TODO: remove! */

    if (http_url_parse(url, NULL, 0, pool_datastack_create(),
               &http_url, &error) < 0) {
        *error_r = t_strdup_printf(
            "fts_elasticsearch: Failed to parse HTTP url: %s", error);

        return -1;
    }

    conn = i_new(struct elasticsearch_connection, 1);
    conn->http_host = i_strdup(http_url->host_name);
    conn->http_port = http_url->port;
    conn->http_base_url = i_strconcat(http_url->path, http_url->enc_query, NULL);
    conn->http_ssl = http_url->have_ssl;
    conn->debug = debug;

    if (elasticsearch_http_client == NULL) {
        memset(&http_set, 0, sizeof(http_set));
        http_set.max_idle_time_msecs = 5 * 1000;
        http_set.max_parallel_connections = 1;
        http_set.max_pipelined_requests = 1;
        http_set.max_redirects = 1;
        http_set.max_attempts = 3;
        http_set.debug = debug;
        elasticsearch_http_client = http_client_init(&http_set);
    }

    *conn_r = conn;

    return 0;
}

void elasticsearch_connection_deinit(struct elasticsearch_connection *conn)
{
    i_free(conn->http_host);
    i_free(conn->http_base_url);
    i_free(conn);
}

static void
elasticsearch_connection_update_response(const struct http_response *response,
                struct elasticsearch_connection *conn)
{
    if (response->status / 100 != 2) {
        i_error("fts_elasticsearch: Indexing failed: %s", response->reason);
        conn->request_status = -1;
    }
}

static struct http_client_request *
elasticsearch_connection_post_request(struct elasticsearch_connection *conn,
    const char * box_guid, const uint32_t message_uid)
{
    struct http_client_request *http_req;
    const char *url;

    url = t_strconcat(conn->http_base_url, "/_bulk/", NULL);

    http_req = http_client_request(elasticsearch_http_client, "POST",
                       conn->http_host, url,
                       elasticsearch_connection_update_response, conn);
    http_client_request_set_port(http_req, conn->http_port);
    http_client_request_set_ssl(http_req, conn->http_ssl);
    http_client_request_add_header(http_req, "Content-Type", "text/json");
    return http_req;
}

int elasticsearch_connection_post(struct elasticsearch_connection *conn,
    const char *cmd, const char * box_guid, const uint32_t message_uid)
{
    struct http_client_request *http_req;
    struct istream *post_payload;

    http_req = elasticsearch_connection_post_request(conn, box_guid, message_uid);

    post_payload = i_stream_create_from_data(cmd, strlen(cmd));

    http_client_request_set_payload(http_req, post_payload, TRUE);

    i_stream_unref(&post_payload);

    http_client_request_submit(http_req);

    conn->request_status = 0;

    http_client_wait(elasticsearch_http_client);

    return conn->request_status;
}

void json_parse_array(json_object *jobj, char *key, struct elasticsearch_connection *conn)
{
    enum json_type type;

    json_object *jarray = jobj; 

    if (key) {
#if JSON_HAS_GET_EX
        json_object_object_get_ex(jobj, key, &jarray);
#else
        jarray = json_object_object_get(jobj, key);
#endif
    }

    int arraylen = json_object_array_length(jarray);
    int i;
    json_object * jvalue;

    for (i = 0; i < arraylen; i++) {
        jvalue = json_object_array_get_idx(jarray, i);
        type = json_object_get_type(jvalue);

        if (type == json_type_array) {
            json_parse_array(jvalue, NULL, conn);
        }
        else if (type != json_type_object) {
            /* TODO :what do we do here? */
        }
        else {
            json_parse(jvalue, conn);
        }
    }
}

static struct elasticsearch_result *
elasticsearch_result_get(struct elasticsearch_connection *conn, const char *box_id)
{
    struct elasticsearch_result * result;
    char *box_id_dup;
    result = hash_table_lookup(conn->ctx->mailboxes, box_id);
    if (result != NULL)
        return result;

    box_id_dup = p_strdup(conn->ctx->result_pool, box_id);
    result = p_new(conn->ctx->result_pool, struct elasticsearch_result, 1);
    result->box_id = box_id_dup;

    p_array_init(&result->uids, conn->ctx->result_pool, 32);
    p_array_init(&result->scores, conn->ctx->result_pool, 32);
    hash_table_insert(conn->ctx->mailboxes, box_id_dup, result);
    array_append(&conn->ctx->results, &result, 1);

    return result;
}

void json_parse(json_object * jobj, struct elasticsearch_connection *conn)
{
    enum json_type type;

    json_object_object_foreach(jobj, key, val) {
        json_object * temp;
        type = json_object_get_type(val);

        if (strcmp(key, "uid") == 0)
            conn->ctx->uid = json_object_get_int(val);

        if (strcmp(key, "_score") == 0)
            conn->ctx->score = json_object_get_double(val);  

        if (strcmp(key, "box") == 0)
            conn->ctx->box_guid = i_strdup(json_object_get_string(val));

        /* this is all we need for an e-mail result */
        if (conn->ctx->uid != -1 && conn->ctx->score != -1 && conn->ctx->box_guid != NULL) {
            struct elasticsearch_result * result = elasticsearch_result_get(conn, conn->ctx->box_guid);
            struct fts_score_map *tmp_score = array_append_space(&result->scores);

            seq_range_array_add(&result->uids, conn->ctx->uid);
            tmp_score->uid = conn->ctx->uid;
            tmp_score->score = conn->ctx->score;

            /* clean-up */
            conn->ctx->uid = -1;
            conn->ctx->score = -1;
            conn->ctx->box_guid = NULL;
            conn->ctx->results_found = TRUE;
        }

        switch (type) {
        case json_type_boolean: /* fall through */
        case json_type_double:  /* fall through */
        case json_type_int:     /* fall through */
        case json_type_string:  /* fall through */
            break; 
        case json_type_object:
#if JSON_HAS_GET_EX
            json_object_object_get_ex(jobj, key, &temp);
#else
            temp = json_object_object_get(jobj, key);
#endif
            
            json_parse(temp, conn);

            break;
        case json_type_array:
            json_parse_array(jobj, key, conn);

            break;
        case json_type_null:
            i_error("fts-elasticsearch: Invalid JSON returned from ElasticSearch");
            break;
        }
    }
} 

static int elasticsearch_json_parse(struct elasticsearch_connection *conn,
              const void *data, size_t size, bool done)
{
    json_object * jobj = json_tokener_parse(data); 
    json_parse(jobj, conn);

    return 0;
}

static void elasticsearch_connection_payload_input(struct elasticsearch_connection *conn)
{
    const unsigned char *data;
    size_t size;
    int ret;

    while ((ret = i_stream_read_data(conn->payload, &data, &size, 0)) > 0) {
        (void)elasticsearch_json_parse(conn, data, size, FALSE);

        i_stream_skip(conn->payload, size);
    }

    if (ret == 0) {
        /* we will be called again for more data */
    } else {
        if (conn->payload->stream_errno != 0) {
            i_error("fts_elasticsearch: failed to read payload from HTTP server: %m");
            conn->request_status = -1;
        }
        io_remove(&conn->io);
        i_stream_unref(&conn->payload);
    }
}

static void
elasticsearch_connection_select_response(const struct http_response *response,
                struct elasticsearch_connection *conn)
{
    if (response->status / 100 != 2) {
        i_error("fts_elasticsearch: lookup failed: %s", response->reason);
        conn->request_status = -1;
        return;
    }

    if (response->payload == NULL) {
        i_error("fts_elasticsearch: lookup failed: empty response payload");
        conn->request_status = -1;
        return;
    }

    /* TODO: read up in the dovecot source to see how we should clean these up
     * as they are cuasing I/O leaks. */
    i_stream_ref(response->payload);
    conn->payload = response->payload;
    conn->io = io_add_istream(response->payload, elasticsearch_connection_payload_input, conn);
    elasticsearch_connection_payload_input(conn);
}

int elasticsearch_connection_select(struct elasticsearch_connection *conn, pool_t pool,
    const char *query, const char *box_guid, struct elasticsearch_result ***box_results_r)
{
    struct http_client_request *http_req;
    struct istream *post_payload;
    struct elasticsearch_lookup_context lookup_context;
    const char *url;
    int parse_ret;

    /* set-up the context */
    memset(&lookup_context, 0, sizeof(lookup_context));
    conn->ctx = &lookup_context;
    conn->ctx->result_pool = pool;
    conn->ctx->uid = -1;
    conn->ctx->score = -1;
    conn->ctx->results_found = FALSE;

    p_array_init(&conn->ctx->results, pool, 32);
    
    hash_table_create(&lookup_context.mailboxes, default_pool, 0, str_hash, strcmp);

    /* build the url */
    /* TODO: this should really be configurable */
    url = t_strconcat(conn->http_base_url, box_guid, NULL);
    url = t_strconcat(url, "/mail/_search/", NULL);

    http_req = http_client_request(elasticsearch_http_client, "POST",
                       conn->http_host, url,
                       elasticsearch_connection_select_response, conn);
    http_client_request_set_port(http_req, conn->http_port);
    http_client_request_set_ssl(http_req, conn->http_ssl);
    http_client_request_add_header(http_req, "Content-Type", "text/json");

    post_payload = i_stream_create_from_data(query, strlen(query));
    http_client_request_set_payload(http_req, post_payload, TRUE);
    i_stream_unref(&post_payload);
    http_client_request_submit(http_req);

    conn->request_status = 0;

    http_client_wait(elasticsearch_http_client);

    if (conn->request_status < 0) 
        return -1;

    /* build our results to push back to the fts api */
    array_append_zero(&conn->ctx->results);
    *box_results_r = array_idx_modifiable(&conn->ctx->results, 0);

    if (conn->ctx->results_found)
        return 1;
    else
        return 0;
    
}